// SAO Auto — launcher/dual_run.cpp
//
// Dual-run cutover mode implementation.
//
// See dual_run.h for contract.  Everything here is Win32 + C runtime.  No
// Python.h, no pybind11, no nlohmann::json — we roll a tiny hand-written
// JSON parser/writer because the schema is closed and small (10 fields).
// Keeps the launcher's link posture minimal.

#include "sao/launcher/dual_run.h"
#include "sao/launcher/working_dir.h"

#include "sao_security/obfuscation/enc_str.h"

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

namespace {

// ---------------------------------------------------------------------------
// Global state — a tiny registry the pipeline queries via
// sao_launcher_dual_run_status().  Protected by a mutex.
// ---------------------------------------------------------------------------

struct ChildRecord {
    DWORD    pid = 0;
    std::wstring role;
    int64_t  start_time_qpc = 0;
    HANDLE   process = nullptr;  // may be null; owned
};

struct DualRunState {
    std::mutex mtx;
    sao_launcher_dual_run_mode_t mode = SAO_DUAL_RUN_MODE_CPP_PREFERRED_PYTHON_FALLBACK;
    std::vector<ChildRecord> children;
    std::wstring fallback_reason;
    sao_dual_run_test_spawn_hook_t spawn_hook = nullptr;
    sao_dual_run_test_probe_hook_t probe_hook = nullptr;
    sao_dual_run_test_force_cpp_fail_hook_t force_cpp_fail_hook = nullptr;
    std::vector<std::wstring> parent_argv_override;
};

DualRunState& S() {
    static DualRunState s;
    return s;
}

// ---------------------------------------------------------------------------
// Wide-string helpers
// ---------------------------------------------------------------------------

// ASCII-only widening for identifiers decrypted from SAO_ENC_STR.  Opaque
// identifiers below are pure 7-bit ASCII, so we do a byte-by-byte widen
// instead of MultiByteToWideChar (which would leak decrypted plaintext into
// an internal Kernel32 buffer for longer than necessary).
void widen_ascii(const char* src, wchar_t* dst, size_t dst_cap) {
    if (!dst || dst_cap == 0) return;
    size_t i = 0;
    if (src) {
        for (; src[i] != '\0' && i + 1 < dst_cap; ++i) {
            dst[i] = static_cast<wchar_t>(static_cast<unsigned char>(src[i]));
        }
    }
    dst[i] = L'\0';
}

void wcs_copy_to_fixed(wchar_t* dst, size_t dst_cap_elems, const wchar_t* src) {
    if (!dst || dst_cap_elems == 0) return;
    if (!src) { dst[0] = L'\0'; return; }
    ::lstrcpynW(dst, src, static_cast<int>(dst_cap_elems));
}

// UTF-8 -> UTF-16 helper for JSON parsing.
std::wstring utf8_to_wide(const std::string& in) {
    if (in.empty()) return {};
    int wide_len = ::MultiByteToWideChar(CP_UTF8, 0, in.data(), (int)in.size(),
                                          nullptr, 0);
    if (wide_len <= 0) return {};
    std::wstring out(static_cast<size_t>(wide_len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, in.data(), (int)in.size(),
                          out.data(), wide_len);
    return out;
}

// UTF-16 -> UTF-8 for JSON emission.
std::string wide_to_utf8(const std::wstring& in) {
    if (in.empty()) return {};
    int utf8_len = ::WideCharToMultiByte(CP_UTF8, 0, in.data(), (int)in.size(),
                                          nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0) return {};
    std::string out(static_cast<size_t>(utf8_len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, in.data(), (int)in.size(),
                          out.data(), utf8_len, nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------------------
// %APPDATA% path resolution — %APPDATA%\SaoAuto\dual_run.json
// ---------------------------------------------------------------------------

bool default_dual_run_config_path(wchar_t* out, size_t out_cap) {
    if (!out || out_cap == 0) return false;
    PWSTR appdata = nullptr;
    HRESULT hr = ::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata);
    if (FAILED(hr) || !appdata) {
        if (appdata) ::CoTaskMemFree(appdata);
        return false;
    }
    // The persistent folder name/leaf stay compatible with existing user
    // installs — we only wrap the literal in SAO_ENC_STR so it does not
    // linger in .rdata plaintext.  If we ever rev the storage layout we
    // will bump the leaf here without touching the API surface.
    const auto suffix = SAO_ENC_STR("\\SaoAuto\\dual_run.json");
    wchar_t wide_suffix[64]{};
    widen_ascii(suffix.decrypt(), wide_suffix, std::size(wide_suffix));
    _snwprintf_s(out, out_cap, _TRUNCATE, L"%s%s", appdata, wide_suffix);
    ::CoTaskMemFree(appdata);
    return true;
}

// ---------------------------------------------------------------------------
// Minimal JSON parser — closed schema, so we hand-roll a tolerant tokenizer.
// Supports: object with string keys, string values, integer values, object
// values (env_overrides map).  Rejects arrays, nulls, booleans.  All strings
// are UTF-8 in the file, UTF-16 in memory.
//
// Grammar (informal):
//   Value  := Object | String | Number
//   Object := '{' Pair (',' Pair)* '}'
//   Pair   := String ':' Value
//   String := '"' UTF8 '"'  (with \\ \" \\n \\t \\\\ \\/ handled)
//   Number := signed int32
// ---------------------------------------------------------------------------

struct JsonParser {
    const char* p = nullptr;
    const char* end = nullptr;
    bool ok = true;

    void skip_ws() {
        while (p < end) {
            char c = *p;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++p; continue; }
            break;
        }
    }

    bool expect(char c) {
        skip_ws();
        if (p >= end || *p != c) { ok = false; return false; }
        ++p;
        return true;
    }

    bool peek(char c) {
        skip_ws();
        return (p < end && *p == c);
    }

    bool parse_string(std::string& out) {
        skip_ws();
        if (p >= end || *p != '"') { ok = false; return false; }
        ++p;
        out.clear();
        while (p < end && *p != '"') {
            char c = *p++;
            if (c == '\\' && p < end) {
                char esc = *p++;
                switch (esc) {
                    case '"':  out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'u': {
                        // Accept \uXXXX by decoding the codepoint into UTF-8.
                        if (p + 4 > end) { ok = false; return false; }
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = *p++;
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                            else { ok = false; return false; }
                        }
                        if (cp < 0x80) out.push_back((char)cp);
                        else if (cp < 0x800) {
                            out.push_back((char)(0xC0 | (cp >> 6)));
                            out.push_back((char)(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back((char)(0xE0 | (cp >> 12)));
                            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back((char)(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: out.push_back(esc); break;
                }
            } else {
                out.push_back(c);
            }
        }
        if (p >= end) { ok = false; return false; }
        ++p;  // consume closing quote
        return true;
    }

    // Read whatever atomic value comes next as a string.  For {} we recurse
    // via ``parse_object_of_strings`` at the call site.
    bool parse_string_or_int(std::string& out) {
        skip_ws();
        if (p >= end) { ok = false; return false; }
        if (*p == '"') return parse_string(out);
        // Try integer.
        const char* start = p;
        if (*p == '-' || *p == '+') ++p;
        while (p < end && *p >= '0' && *p <= '9') ++p;
        if (p == start) { ok = false; return false; }
        out.assign(start, p - start);
        return true;
    }

    // Parse an object whose values are all strings; typical for env_overrides.
    bool parse_object_of_strings(std::vector<std::pair<std::string, std::string>>& out) {
        if (!expect('{')) return false;
        skip_ws();
        if (peek('}')) { ++p; return true; }
        while (ok) {
            std::string key;
            if (!parse_string(key)) return false;
            if (!expect(':')) return false;
            std::string val;
            if (!parse_string(val)) return false;
            out.emplace_back(std::move(key), std::move(val));
            skip_ws();
            if (peek(',')) { ++p; continue; }
            if (peek('}')) { ++p; return true; }
            ok = false;
            return false;
        }
        return false;
    }
};

// Mode string -> enum.
sao_launcher_dual_run_mode_t mode_from_string(const std::string& s) {
    if (s == "cpp_only")                         return SAO_DUAL_RUN_MODE_CPP_ONLY;
    if (s == "python_only")                      return SAO_DUAL_RUN_MODE_PYTHON_ONLY;
    if (s == "cpp_preferred_python_fallback")    return SAO_DUAL_RUN_MODE_CPP_PREFERRED_PYTHON_FALLBACK;
    if (s == "python_preferred_cpp_fallback")    return SAO_DUAL_RUN_MODE_PYTHON_PREFERRED_CPP_FALLBACK;
    if (s == "dual_side_by_side")                return SAO_DUAL_RUN_MODE_DUAL_SIDE_BY_SIDE;
    return SAO_DUAL_RUN_MODE_CPP_PREFERRED_PYTHON_FALLBACK;
}

const char* mode_to_string(sao_launcher_dual_run_mode_t m) {
    switch (m) {
        case SAO_DUAL_RUN_MODE_CPP_ONLY:                      return "cpp_only";
        case SAO_DUAL_RUN_MODE_PYTHON_ONLY:                   return "python_only";
        case SAO_DUAL_RUN_MODE_CPP_PREFERRED_PYTHON_FALLBACK: return "cpp_preferred_python_fallback";
        case SAO_DUAL_RUN_MODE_PYTHON_PREFERRED_CPP_FALLBACK: return "python_preferred_cpp_fallback";
        case SAO_DUAL_RUN_MODE_DUAL_SIDE_BY_SIDE:             return "dual_side_by_side";
        default:                                              return "cpp_preferred_python_fallback";
    }
}

// ---------------------------------------------------------------------------
// File I/O helpers
// ---------------------------------------------------------------------------

bool read_file_bytes(const wchar_t* path, std::string& out) {
    HANDLE h = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart > 4 * 1024 * 1024) {
        ::CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD read = 0;
    if (!out.empty()) {
        if (!::ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr)
            || read != out.size()) {
            ::CloseHandle(h);
            return false;
        }
    }
    ::CloseHandle(h);
    return true;
}

bool write_file_bytes(const wchar_t* path, const std::string& in) {
    // Ensure parent dir exists.
    wchar_t parent[MAX_PATH]{};
    ::lstrcpynW(parent, path, MAX_PATH);
    ::PathRemoveFileSpecW(parent);
    sao::launcher::ensureDirectoryExists(parent);

    HANDLE h = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = TRUE;
    if (!in.empty()) {
        ok = ::WriteFile(h, in.data(), static_cast<DWORD>(in.size()),
                          &written, nullptr);
    }
    ::CloseHandle(h);
    return ok == TRUE && written == in.size();
}

// ---------------------------------------------------------------------------
// JSON serialisation — hand-written, closed schema.
// ---------------------------------------------------------------------------

void json_emit_string(std::string& out, const std::string& in) {
    out.push_back('"');
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

std::string serialize_config(const sao_dual_run_config& cfg) {
    std::string s;
    s.reserve(512);
    s += "{\n";
    s += "  \"mode\": ";
    json_emit_string(s, mode_to_string(cfg.mode));
    s += ",\n";
    s += "  \"python_exe_path\": ";
    json_emit_string(s, wide_to_utf8(cfg.python_exe_path));
    s += ",\n";
    s += "  \"python_main_py_path\": ";
    json_emit_string(s, wide_to_utf8(cfg.python_main_py_path));
    s += ",\n";
    s += "  \"cpp_exe_path\": ";
    json_emit_string(s, wide_to_utf8(cfg.cpp_exe_path));
    s += ",\n";
    s += "  \"env_overrides\": {";
    for (int32_t i = 0; i < cfg.env_overrides_count && i < 16; ++i) {
        if (i > 0) s += ",";
        s += "\n    ";
        json_emit_string(s, wide_to_utf8(cfg.env_overrides[i].key));
        s += ": ";
        json_emit_string(s, wide_to_utf8(cfg.env_overrides[i].value));
    }
    if (cfg.env_overrides_count > 0) s += "\n  ";
    s += "}\n";
    s += "}\n";
    return s;
}

// ---------------------------------------------------------------------------
// CommandLineToArgvW-style quoting for a single argument.
// ---------------------------------------------------------------------------
void append_quoted_arg(std::wstring& out, const wchar_t* arg) {
    if (!arg || !*arg) { out += L"\"\""; return; }
    // Need to quote if it has spaces, tabs, quotes, or backslashes at the
    // end.  Simplest safe policy: always quote.
    out += L'"';
    for (const wchar_t* p = arg; *p; ) {
        int backslashes = 0;
        while (*p == L'\\') { ++backslashes; ++p; }
        if (*p == L'\0') {
            // Escape all backslashes; the closing quote follows.
            for (int i = 0; i < 2 * backslashes; ++i) out += L'\\';
            break;
        } else if (*p == L'"') {
            // Escape all backslashes AND the quote.
            for (int i = 0; i < 2 * backslashes + 1; ++i) out += L'\\';
            out += *p;
            ++p;
        } else {
            for (int i = 0; i < backslashes; ++i) out += L'\\';
            out += *p;
            ++p;
        }
    }
    out += L'"';
}

std::vector<std::wstring> snapshot_parent_argv() {
    {
        std::lock_guard<std::mutex> g(S().mtx);
        if (!S().parent_argv_override.empty()) {
            return S().parent_argv_override;
        }
    }

    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv) return {};
    std::vector<std::wstring> snapshot;
    snapshot.reserve(static_cast<size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        snapshot.emplace_back(argv[index] ? argv[index] : L"");
    }
    ::LocalFree(argv);
    return snapshot;
}

std::wstring build_cpp_command_line(const wchar_t* cpp_exe_path,
                                    int argc,
                                    const wchar_t* const* argv) {
    std::wstring command_line;
    append_quoted_arg(command_line, cpp_exe_path);

    bool has_safe_mode = false;
    for (int index = 1; index < argc; ++index) {
        const wchar_t* argument = argv && argv[index] ? argv[index] : L"";
        if (_wcsicmp(argument, L"--safe-mode") == 0) has_safe_mode = true;
        command_line.push_back(L' ');
        append_quoted_arg(command_line, argument);
    }
    if (!has_safe_mode) {
        command_line += L" --safe-mode";
    }
    return command_line;
}

// ---------------------------------------------------------------------------
// Build a CreateProcessW-friendly env block from the current process env plus
// the overrides.  Format: KEY=VAL\0KEY=VAL\0...\0\0
// ---------------------------------------------------------------------------
std::vector<wchar_t> build_env_block(const sao_dual_run_config& cfg,
                                      const wchar_t* role) {
    // Snapshot the current env.
    wchar_t* current = ::GetEnvironmentStringsW();
    std::vector<wchar_t> block;
    block.reserve(8192);

    // Copy parent env, but strip any lines that match one of our overrides
    // or the role env var (we'll re-add).
    std::vector<std::wstring> skip_prefixes;
    skip_prefixes.emplace_back(std::wstring(SAO_DUAL_RUN_ENV_VAR_NAME) + L"=");
    for (int32_t i = 0; i < cfg.env_overrides_count && i < 16; ++i) {
        skip_prefixes.emplace_back(std::wstring(cfg.env_overrides[i].key) + L"=");
    }

    if (current) {
        for (wchar_t* p = current; *p; ) {
            size_t len = wcslen(p);
            bool skip = false;
            for (const auto& pfx : skip_prefixes) {
                if (_wcsnicmp(p, pfx.c_str(), pfx.size()) == 0) {
                    skip = true;
                    break;
                }
            }
            if (!skip) {
                block.insert(block.end(), p, p + len + 1);
            }
            p += len + 1;
        }
        ::FreeEnvironmentStringsW(current);
    }

    // Append role.
    std::wstring role_line = SAO_DUAL_RUN_ENV_VAR_NAME;
    role_line += L"=";
    if (role && *role) role_line += role;
    else               role_line += L"cpp";
    block.insert(block.end(), role_line.c_str(), role_line.c_str() + role_line.size() + 1);

    // Append overrides.
    for (int32_t i = 0; i < cfg.env_overrides_count && i < 16; ++i) {
        std::wstring line;
        line += cfg.env_overrides[i].key;
        line += L"=";
        line += cfg.env_overrides[i].value;
        block.insert(block.end(), line.c_str(), line.c_str() + line.size() + 1);
    }

    // Double null terminator.
    block.push_back(L'\0');
    return block;
}

// ---------------------------------------------------------------------------
// Spawn helper — actually calls CreateProcessW with the given command line.
// ---------------------------------------------------------------------------
sao_status_t do_spawn(const sao_dual_run_config& cfg,
                       const wchar_t* exe_path,
                       const wchar_t* command_line,
                       const wchar_t* role,
                       sao_dual_run_spawn_result* result_out) {
    if (result_out) {
        result_out->pid = 0;
        result_out->process = nullptr;
        result_out->thread = nullptr;
        result_out->start_time_qpc = 0;
    }

    // Test hook — bypass CreateProcessW.
    sao_dual_run_test_spawn_hook_t hook = nullptr;
    {
        std::lock_guard<std::mutex> g(S().mtx);
        hook = S().spawn_hook;
    }
    if (hook) {
        int rc = hook(exe_path, command_line, role, result_out);
        return rc == 0 ? SAO_STATUS_OK : SAO_LAUNCHER_SPAWN_FAILED;
    }

    // Build a mutable copy of the command line — CreateProcessW wants LPWSTR.
    std::vector<wchar_t> cmdline_buf(command_line, command_line + wcslen(command_line) + 1);

    auto env_block = build_env_block(cfg, role);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    DWORD flags = CREATE_UNICODE_ENVIRONMENT;

    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);

    BOOL ok = ::CreateProcessW(
        exe_path,
        cmdline_buf.data(),
        nullptr, nullptr,
        FALSE,                        // do NOT inherit handles
        flags,
        env_block.data(),
        nullptr,
        &si,
        &pi);
    if (!ok) {
        return SAO_LAUNCHER_SPAWN_FAILED;
    }

    if (result_out) {
        result_out->pid = pi.dwProcessId;
        result_out->process = pi.hProcess;
        result_out->thread = pi.hThread;
        result_out->start_time_qpc = now.QuadPart;
    } else {
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
    }

    return SAO_STATUS_OK;
}

// ---------------------------------------------------------------------------
// Python probe internals
// ---------------------------------------------------------------------------

bool run_python_version_check(const wchar_t* python_exe,
                               std::wstring& version_out,
                               int& major, int& minor, int& patch) {
    // Set up an anonymous pipe for stdout.
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!::CreatePipe(&read_end, &write_end, &sa, 0)) return false;
    // Only write_end must be inheritable.
    ::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};

    std::wstring cmd = L"\"";
    cmd += python_exe;
    cmd += L"\" --version";
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');

    BOOL ok = ::CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
                                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    ::CloseHandle(write_end);
    if (!ok) {
        ::CloseHandle(read_end);
        return false;
    }

    // Read what the child wrote.
    std::string blob;
    char buf[512];
    DWORD read_n = 0;
    while (::ReadFile(read_end, buf, sizeof(buf), &read_n, nullptr) && read_n > 0) {
        blob.append(buf, buf + read_n);
    }
    ::CloseHandle(read_end);

    ::WaitForSingleObject(pi.hProcess, 5000);
    DWORD exit_code = 1;
    ::GetExitCodeProcess(pi.hProcess, &exit_code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);

    if (exit_code != 0) return false;

    // Parse "Python X.Y.Z" — Python 3.4+ writes to stdout.
    // Some old versions wrote to stderr; we merged both above.
    auto pos = blob.find("Python ");
    if (pos == std::string::npos) return false;
    const char* p = blob.c_str() + pos + 7;
    int mj = 0, mi = 0, pt = 0;
    if (sscanf_s(p, "%d.%d.%d", &mj, &mi, &pt) < 2) return false;
    major = mj;
    minor = mi;
    patch = pt;

    wchar_t vbuf[32]{};
    _snwprintf_s(vbuf, 32, _TRUNCATE, L"%d.%d.%d", mj, mi, pt);
    version_out = vbuf;
    return true;
}

// ``where python.exe`` — reads the first line of stdout.
bool where_python(std::wstring& out_path) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!::CreatePipe(&read_end, &write_end, &sa, 0)) return false;
    ::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};

    wchar_t cmd[] = L"cmd.exe /c where python.exe";
    std::vector<wchar_t> cmd_buf(std::begin(cmd), std::end(cmd));

    BOOL ok = ::CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
                                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    ::CloseHandle(write_end);
    if (!ok) {
        ::CloseHandle(read_end);
        return false;
    }

    std::string blob;
    char buf[512];
    DWORD read_n = 0;
    while (::ReadFile(read_end, buf, sizeof(buf), &read_n, nullptr) && read_n > 0) {
        blob.append(buf, buf + read_n);
    }
    ::CloseHandle(read_end);
    ::WaitForSingleObject(pi.hProcess, 5000);
    DWORD exit_code = 1;
    ::GetExitCodeProcess(pi.hProcess, &exit_code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);

    if (exit_code != 0 || blob.empty()) return false;

    // Take the first line.  Prefer a non-WindowsApps stub — the App Execution
    // Alias `%LocalAppData%\Microsoft\WindowsApps\python.exe` returns
    // "not installed" popups instead of running Python.
    std::wstring wide = utf8_to_wide(blob);
    size_t start = 0;
    std::wstring chosen;
    while (start < wide.size()) {
        size_t end = wide.find_first_of(L"\r\n", start);
        std::wstring line = (end == std::wstring::npos)
            ? wide.substr(start)
            : wide.substr(start, end - start);
        // trim
        while (!line.empty() && (line.back() == L' ' || line.back() == L'\t'))
            line.pop_back();
        while (!line.empty() && (line.front() == L' ' || line.front() == L'\t'))
            line.erase(line.begin());
        if (!line.empty()) {
            if (chosen.empty()) chosen = line;
            // If we find a non-WindowsApps entry, prefer it.
            if (::wcsstr(line.c_str(), L"\\WindowsApps\\") == nullptr &&
                ::wcsstr(line.c_str(), L"/WindowsApps/") == nullptr) {
                chosen = line;
                break;
            }
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }

    if (chosen.empty()) return false;
    out_path = std::move(chosen);
    return true;
}

} // namespace

// ===========================================================================
// Public API — configuration
// ===========================================================================

extern "C" void sao_launcher_dual_run_config_default(sao_dual_run_config* cfg) {
    if (!cfg) return;
    std::memset(cfg, 0, sizeof(*cfg));
    cfg->mode = SAO_DUAL_RUN_MODE_CPP_PREFERRED_PYTHON_FALLBACK;
}

extern "C" sao_status_t sao_launcher_dual_run_config_load(sao_dual_run_config* cfg_out) {
    wchar_t path[MAX_PATH]{};
    if (!default_dual_run_config_path(path, MAX_PATH)) {
        // Fall back to defaults; that path is not fatal.
        sao_launcher_dual_run_config_default(cfg_out);
        return SAO_STATUS_OK;
    }
    return sao_launcher_dual_run_config_load_from_path(path, cfg_out);
}

extern "C" sao_status_t sao_launcher_dual_run_config_load_from_path(
    const wchar_t* path, sao_dual_run_config* cfg_out) {
    if (!cfg_out) return SAO_STATUS_INVALID_ARGUMENT;
    sao_launcher_dual_run_config_default(cfg_out);

    if (!path || !*path) return SAO_STATUS_INVALID_ARGUMENT;

    // Missing file — treat as "user hasn't chosen".
    if (::GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        return SAO_STATUS_OK;
    }

    std::string blob;
    if (!read_file_bytes(path, blob)) return SAO_LAUNCHER_CONFIG_PARSE_FAILED;

    JsonParser jp;
    jp.p = blob.data();
    jp.end = blob.data() + blob.size();
    if (!jp.expect('{')) return SAO_LAUNCHER_CONFIG_PARSE_FAILED;

    while (jp.ok) {
        jp.skip_ws();
        if (jp.peek('}')) { ++jp.p; break; }
        std::string key;
        if (!jp.parse_string(key)) return SAO_LAUNCHER_CONFIG_PARSE_FAILED;
        if (!jp.expect(':')) return SAO_LAUNCHER_CONFIG_PARSE_FAILED;

        if (key == "mode") {
            std::string val;
            if (!jp.parse_string(val)) return SAO_LAUNCHER_CONFIG_PARSE_FAILED;
            cfg_out->mode = mode_from_string(val);
        } else if (key == "python_exe_path") {
            std::string val;
            if (!jp.parse_string(val)) return SAO_LAUNCHER_CONFIG_PARSE_FAILED;
            wcs_copy_to_fixed(cfg_out->python_exe_path, 260,
                              utf8_to_wide(val).c_str());
        } else if (key == "python_main_py_path") {
            std::string val;
            if (!jp.parse_string(val)) return SAO_LAUNCHER_CONFIG_PARSE_FAILED;
            wcs_copy_to_fixed(cfg_out->python_main_py_path, 260,
                              utf8_to_wide(val).c_str());
        } else if (key == "cpp_exe_path") {
            std::string val;
            if (!jp.parse_string(val)) return SAO_LAUNCHER_CONFIG_PARSE_FAILED;
            wcs_copy_to_fixed(cfg_out->cpp_exe_path, 260,
                              utf8_to_wide(val).c_str());
        } else if (key == "env_overrides") {
            std::vector<std::pair<std::string, std::string>> kv;
            if (!jp.parse_object_of_strings(kv)) return SAO_LAUNCHER_CONFIG_PARSE_FAILED;
            int32_t n = 0;
            for (auto& e : kv) {
                if (n >= 16) break;
                wcs_copy_to_fixed(cfg_out->env_overrides[n].key, 64,
                                  utf8_to_wide(e.first).c_str());
                wcs_copy_to_fixed(cfg_out->env_overrides[n].value, 512,
                                  utf8_to_wide(e.second).c_str());
                ++n;
            }
            cfg_out->env_overrides_count = n;
        } else {
            // Unknown key — accept a string value and drop it.
            std::string val;
            if (!jp.parse_string_or_int(val)) return SAO_LAUNCHER_CONFIG_PARSE_FAILED;
        }

        jp.skip_ws();
        if (jp.peek(',')) { ++jp.p; continue; }
        if (jp.peek('}')) { ++jp.p; break; }
        return SAO_LAUNCHER_CONFIG_PARSE_FAILED;
    }

    return jp.ok ? SAO_STATUS_OK : SAO_LAUNCHER_CONFIG_PARSE_FAILED;
}

extern "C" sao_status_t sao_launcher_dual_run_config_save(const sao_dual_run_config* cfg) {
    if (!cfg) return SAO_STATUS_INVALID_ARGUMENT;
    wchar_t path[MAX_PATH]{};
    if (!default_dual_run_config_path(path, MAX_PATH)) {
        return SAO_LAUNCHER_CONFIG_WRITE_FAILED;
    }
    return sao_launcher_dual_run_config_save_to_path(path, cfg);
}

extern "C" sao_status_t sao_launcher_dual_run_config_save_to_path(
    const wchar_t* path, const sao_dual_run_config* cfg) {
    if (!path || !cfg) return SAO_STATUS_INVALID_ARGUMENT;
    std::string blob = serialize_config(*cfg);
    if (!write_file_bytes(path, blob)) return SAO_LAUNCHER_CONFIG_WRITE_FAILED;
    return SAO_STATUS_OK;
}

extern "C" sao_launcher_dual_run_mode_t sao_launcher_dual_run_mode(void) {
    sao_dual_run_config cfg{};
    sao_status_t s = sao_launcher_dual_run_config_load(&cfg);
    if (s != SAO_STATUS_OK) return SAO_DUAL_RUN_MODE_CPP_PREFERRED_PYTHON_FALLBACK;
    return cfg.mode;
}

// ===========================================================================
// Public API — Python probe
// ===========================================================================

extern "C" sao_status_t sao_launcher_dual_run_probe_python(
    const sao_dual_run_config* cfg,
    sao_dual_run_python_probe* probe_out) {
    if (!probe_out) return SAO_STATUS_INVALID_ARGUMENT;
    std::memset(probe_out, 0, sizeof(*probe_out));

    // Test hook always wins.
    sao_dual_run_test_probe_hook_t hook = nullptr;
    {
        std::lock_guard<std::mutex> g(S().mtx);
        hook = S().probe_hook;
    }
    if (hook) {
        hook(probe_out);
        return SAO_STATUS_OK;
    }

    std::wstring resolved;
    std::wstring version;
    int major = 0, minor = 0, patch = 0;

    // 1. Explicit config path?
    if (cfg && cfg->python_exe_path[0]) {
        if (::GetFileAttributesW(cfg->python_exe_path) != INVALID_FILE_ATTRIBUTES) {
            if (run_python_version_check(cfg->python_exe_path, version,
                                          major, minor, patch)) {
                resolved = cfg->python_exe_path;
            }
        }
    }

    // 2. `where python`
    if (resolved.empty()) {
        std::wstring w;
        if (where_python(w)) {
            if (run_python_version_check(w.c_str(), version, major, minor, patch)) {
                resolved = std::move(w);
            }
        }
    }

    // 3. Well-known fallbacks.
    if (resolved.empty()) {
        const wchar_t* candidates[] = {
            L"E:\\Py\\python.exe",
            L"C:\\Python311\\python.exe",
            L"C:\\Python312\\python.exe",
            L"C:\\Program Files\\Python311\\python.exe",
        };
        for (auto* c : candidates) {
            if (::GetFileAttributesW(c) == INVALID_FILE_ATTRIBUTES) continue;
            if (run_python_version_check(c, version, major, minor, patch)) {
                resolved = c;
                break;
            }
        }
    }

    if (resolved.empty()) {
        probe_out->available = 0;
        return SAO_STATUS_OK;
    }

    if (!(major == 3 && minor >= 11)) {
        probe_out->available = 0;
        wcs_copy_to_fixed(probe_out->path, 260, resolved.c_str());
        wcs_copy_to_fixed(probe_out->version, 32, version.c_str());
        probe_out->major = major;
        probe_out->minor = minor;
        probe_out->patch = patch;
        return SAO_STATUS_OK;
    }

    probe_out->available = 1;
    wcs_copy_to_fixed(probe_out->path, 260, resolved.c_str());
    wcs_copy_to_fixed(probe_out->version, 32, version.c_str());
    probe_out->major = major;
    probe_out->minor = minor;
    probe_out->patch = patch;
    return SAO_STATUS_OK;
}

// ===========================================================================
// Public API — spawn
// ===========================================================================

extern "C" sao_status_t sao_launcher_dual_run_spawn_python(
    const sao_dual_run_config* cfg,
    const wchar_t* role,
    sao_dual_run_spawn_result* result_out) {
    if (!cfg) return SAO_STATUS_INVALID_ARGUMENT;

    sao_dual_run_python_probe probe{};
    sao_status_t s = sao_launcher_dual_run_probe_python(cfg, &probe);
    if (s != SAO_STATUS_OK || !probe.available) {
        return SAO_LAUNCHER_PYTHON_UNAVAILABLE;
    }

    // Resolve main.py.  If the config didn't say, fall back to the standard
    // sao_auto/python/main.py location beside the base dir.
    std::wstring main_py;
    if (cfg->python_main_py_path[0]) {
        main_py = cfg->python_main_py_path;
    } else {
        // Derive from GetModuleFileNameW → parent (base_dir) → \..\python\main.py
        wchar_t exe[MAX_PATH]{};
        ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        ::PathRemoveFileSpecW(exe);
        // If we're in build/windows-debug/bin/Debug, walk up 4.  Otherwise
        // walk up 2 (the runtime/ layout).  Cheap heuristic — if the path
        // doesn't resolve, the caller sees SAO_LAUNCHER_SPAWN_FAILED.
        main_py = exe;
        main_py += L"\\..\\..\\..\\..\\sao_auto\\python\\main.py";
    }

    std::wstring cmdline;
    append_quoted_arg(cmdline, probe.path);
    cmdline += L" ";
    append_quoted_arg(cmdline, main_py.c_str());

    return do_spawn(*cfg, probe.path, cmdline.c_str(),
                    role && *role ? role : L"python",
                    result_out);
}

extern "C" sao_status_t sao_launcher_dual_run_spawn_cpp(
    const sao_dual_run_config* cfg,
    const wchar_t* role,
    sao_dual_run_spawn_result* result_out) {
    if (!cfg) return SAO_STATUS_INVALID_ARGUMENT;

    wchar_t exe[MAX_PATH]{};
    if (cfg->cpp_exe_path[0]) {
        ::lstrcpynW(exe, cfg->cpp_exe_path, MAX_PATH);
    } else {
        ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    }

    const auto parent_argv = snapshot_parent_argv();
    std::vector<const wchar_t*> parent_argv_view;
    parent_argv_view.reserve(parent_argv.size());
    for (const auto& argument : parent_argv) {
        parent_argv_view.push_back(argument.c_str());
    }
    const auto cmdline = build_cpp_command_line(
        exe, static_cast<int>(parent_argv_view.size()),
        parent_argv_view.empty() ? nullptr : parent_argv_view.data());

    return do_spawn(*cfg, exe, cmdline.c_str(),
                    role && *role ? role : L"cpp",
                    result_out);
}

extern "C" sao_status_t sao_launcher_dual_run_build_cpp_command_line(
    const wchar_t* cpp_exe_path,
    int argc,
    const wchar_t* const* argv,
    wchar_t* out_command_line,
    uint32_t* inout_char_count) {
    if (!cpp_exe_path || !*cpp_exe_path || argc < 0 ||
        (argc > 0 && !argv) || !inout_char_count) {
        return SAO_STATUS_INVALID_ARGUMENT;
    }
    const auto command_line = build_cpp_command_line(
        cpp_exe_path, argc, argv);
    const auto required = static_cast<uint32_t>(command_line.size() + 1);
    if (!out_command_line || *inout_char_count < required) {
        *inout_char_count = required;
        return out_command_line ? SAO_STATUS_INVALID_ARGUMENT : SAO_STATUS_OK;
    }
    std::memcpy(out_command_line, command_line.c_str(),
                required * sizeof(wchar_t));
    *inout_char_count = required;
    return SAO_STATUS_OK;
}

// ===========================================================================
// Public API — status registry
// ===========================================================================

extern "C" void sao_launcher_dual_run_register_child(DWORD pid,
                                                     const wchar_t* role,
                                                     int64_t start_time_qpc,
                                                     HANDLE process_handle_owned) {
    ChildRecord rec;
    rec.pid = pid;
    rec.role = role ? std::wstring(role) : L"";
    rec.start_time_qpc = start_time_qpc;
    rec.process = process_handle_owned;

    std::lock_guard<std::mutex> g(S().mtx);
    if (S().children.size() >= SAO_DUAL_RUN_STATUS_MAX_CHILDREN) {
        // Kick out any already-dead entry to make room.
        for (auto it = S().children.begin(); it != S().children.end(); ++it) {
            if (it->process) {
                DWORD ec = STILL_ACTIVE;
                if (::GetExitCodeProcess(it->process, &ec) && ec != STILL_ACTIVE) {
                    ::CloseHandle(it->process);
                    S().children.erase(it);
                    break;
                }
            }
        }
    }
    S().children.push_back(std::move(rec));
}

extern "C" void sao_launcher_dual_run_record_fallback_reason(const wchar_t* reason) {
    std::lock_guard<std::mutex> g(S().mtx);
    S().fallback_reason = reason ? reason : L"";
}

extern "C" void sao_launcher_dual_run_status(sao_dual_run_status* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));

    std::lock_guard<std::mutex> g(S().mtx);
    out->mode = S().mode;
    wcs_copy_to_fixed(out->fallback_reason, 256, S().fallback_reason.c_str());

    int32_t n = 0;
    for (auto& c : S().children) {
        if (n >= SAO_DUAL_RUN_STATUS_MAX_CHILDREN) break;
        auto& slot = out->running_children[n];
        slot.pid = c.pid;
        wcs_copy_to_fixed(slot.role, 32, c.role.c_str());
        slot.start_time_qpc = c.start_time_qpc;

        DWORD ec = STILL_ACTIVE;
        if (c.process) {
            if (::GetExitCodeProcess(c.process, &ec) && ec != STILL_ACTIVE) {
                slot.is_dead = 1;
                slot.exit_code = static_cast<int32_t>(ec);
            }
        }
        ++n;
    }
    out->running_children_count = n;
}

extern "C" void sao_launcher_dual_run_reset_for_test(void) {
    std::lock_guard<std::mutex> g(S().mtx);
    for (auto& c : S().children) {
        if (c.process) ::CloseHandle(c.process);
    }
    S().children.clear();
    S().fallback_reason.clear();
    S().spawn_hook = nullptr;
    S().probe_hook = nullptr;
    S().force_cpp_fail_hook = nullptr;
    S().parent_argv_override.clear();
    S().mode = SAO_DUAL_RUN_MODE_CPP_PREFERRED_PYTHON_FALLBACK;
}

// ===========================================================================
// Public API — driver mutex
// ===========================================================================

extern "C" sao_status_t sao_launcher_dual_run_acquire_driver_mutex(HANDLE* mutex_out) {
    if (!mutex_out) return SAO_STATUS_INVALID_ARGUMENT;
    *mutex_out = nullptr;

    // Opaque per-session mutex.  Replaces the plaintext
    // "Local\\SaoAutoLauncherDualRun" identifier so ObjectManager scanners
    // cannot pivot on the product name.
    const auto opaque = SAO_ENC_STR("Local\\4F5A.dr");
    wchar_t name[32]{};
    widen_ascii(opaque.decrypt(), name, std::size(name));
    HANDLE m = ::CreateMutexW(nullptr, FALSE, name);
    if (!m) return SAO_STATUS_INTERNAL;
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(m);
        return SAO_LAUNCHER_ALREADY_RUNNING;
    }
    *mutex_out = m;
    return SAO_STATUS_OK;
}

extern "C" void sao_launcher_dual_run_release_driver_mutex(HANDLE mutex) {
    if (mutex) ::CloseHandle(mutex);
}

// ===========================================================================
// Public API — step zero
// ===========================================================================

extern "C" sao_status_t sao_launcher_dual_run_step_zero(
    const sao_dual_run_config* cfg,
    int32_t* continue_out,
    int32_t* exit_code_out) {
    if (continue_out) *continue_out = 1;
    if (exit_code_out) *exit_code_out = 0;
    if (!cfg) return SAO_STATUS_INVALID_ARGUMENT;

    // If the parent already spawned us as a peer child (SAO_DUAL_RUN_ROLE is
    // set to any non-empty value), we're not the dual-run driver; we act as
    // the CPP peer and skip the whole mode-dispatch dance.
    wchar_t role_env[64]{};
    if (::GetEnvironmentVariableW(SAO_DUAL_RUN_ENV_VAR_NAME, role_env, 64) > 0) {
        if (role_env[0]) {
            if (continue_out) *continue_out = 1;
            return SAO_STATUS_OK;
        }
    }

    // Remember the effective mode for status queries.
    {
        std::lock_guard<std::mutex> g(S().mtx);
        S().mode = cfg->mode;
    }

    switch (cfg->mode) {
        case SAO_DUAL_RUN_MODE_CPP_ONLY:
            if (continue_out) *continue_out = 1;
            return SAO_STATUS_OK;

        case SAO_DUAL_RUN_MODE_PYTHON_ONLY: {
            sao_dual_run_spawn_result r{};
            sao_status_t s = sao_launcher_dual_run_spawn_python(cfg, L"python", &r);
            if (s != SAO_STATUS_OK) return s;
            sao_launcher_dual_run_register_child(r.pid, L"python",
                                                  r.start_time_qpc, r.process);
            if (r.thread) ::CloseHandle(r.thread);
            if (continue_out) *continue_out = 0;
            if (exit_code_out) *exit_code_out = SAO_EXIT_HANDOFF_TO_PYTHON;
            return SAO_STATUS_OK;
        }

        case SAO_DUAL_RUN_MODE_CPP_PREFERRED_PYTHON_FALLBACK: {
            // Test hook can force us into the fallback branch.
            sao_dual_run_test_force_cpp_fail_hook_t force = nullptr;
            {
                std::lock_guard<std::mutex> g(S().mtx);
                force = S().force_cpp_fail_hook;
            }
            if (force && force() != 0) {
                sao_launcher_dual_run_record_fallback_reason(
                    L"forced_cpp_fail_test_hook");
                sao_dual_run_spawn_result r{};
                sao_status_t s = sao_launcher_dual_run_spawn_python(cfg,
                                                                     L"python_fallback",
                                                                     &r);
                if (s != SAO_STATUS_OK) return s;
                sao_launcher_dual_run_register_child(r.pid, L"python_fallback",
                                                      r.start_time_qpc, r.process);
                if (r.thread) ::CloseHandle(r.thread);
                if (continue_out) *continue_out = 0;
                if (exit_code_out) *exit_code_out = SAO_EXIT_HANDOFF_TO_PYTHON;
                return SAO_STATUS_OK;
            }
            // Normal path: keep going with CPP.  The 10-step pipeline calls
            // maybe_fallback_to_python() on any fatal error.
            if (continue_out) *continue_out = 1;
            return SAO_STATUS_OK;
        }

        case SAO_DUAL_RUN_MODE_PYTHON_PREFERRED_CPP_FALLBACK: {
            // Spawn Python; if that fails, continue with CPP.
            sao_dual_run_spawn_result r{};
            sao_status_t s = sao_launcher_dual_run_spawn_python(cfg,
                                                                 L"python",
                                                                 &r);
            if (s == SAO_STATUS_OK) {
                sao_launcher_dual_run_register_child(r.pid, L"python",
                                                      r.start_time_qpc, r.process);
                if (r.thread) ::CloseHandle(r.thread);
                if (continue_out) *continue_out = 0;
                if (exit_code_out) *exit_code_out = SAO_EXIT_HANDOFF_TO_PYTHON;
                return SAO_STATUS_OK;
            }
            sao_launcher_dual_run_record_fallback_reason(
                L"python_spawn_failed_falling_back_to_cpp");
            if (continue_out) *continue_out = 1;
            return SAO_STATUS_OK;
        }

        case SAO_DUAL_RUN_MODE_DUAL_SIDE_BY_SIDE: {
            // Spawn Python as a peer + continue with CPP.
            sao_dual_run_spawn_result rp{};
            sao_status_t sp = sao_launcher_dual_run_spawn_python(cfg,
                                                                  L"python",
                                                                  &rp);
            if (sp == SAO_STATUS_OK) {
                sao_launcher_dual_run_register_child(rp.pid, L"python",
                                                      rp.start_time_qpc,
                                                      rp.process);
                if (rp.thread) ::CloseHandle(rp.thread);
            } else {
                sao_launcher_dual_run_record_fallback_reason(
                    L"side_by_side_python_spawn_failed_cpp_only");
            }
            if (continue_out) *continue_out = 1;
            return SAO_STATUS_OK;
        }

        default:
            if (continue_out) *continue_out = 1;
            return SAO_STATUS_OK;
    }
}

extern "C" int32_t sao_launcher_dual_run_maybe_fallback_to_python(
    const sao_dual_run_config* cfg,
    int cpp_step_exit_code,
    const wchar_t* failing_step_name,
    int32_t* exit_code_out) {
    if (!cfg) return 0;
    if (cfg->mode != SAO_DUAL_RUN_MODE_CPP_PREFERRED_PYTHON_FALLBACK) return 0;
    if (cpp_step_exit_code == 0) return 0;

    wchar_t reason[256]{};
    _snwprintf_s(reason, 256, _TRUNCATE,
                 L"cpp_step_failed:%ls:exit=%d",
                 failing_step_name ? failing_step_name : L"unknown",
                 cpp_step_exit_code);
    sao_launcher_dual_run_record_fallback_reason(reason);

    sao_dual_run_spawn_result r{};
    sao_status_t s = sao_launcher_dual_run_spawn_python(cfg, L"python_fallback", &r);
    if (s != SAO_STATUS_OK) {
        // Python unavailable — propagate the original CPP error.
        return 0;
    }
    sao_launcher_dual_run_register_child(r.pid, L"python_fallback",
                                          r.start_time_qpc, r.process);
    if (r.thread) ::CloseHandle(r.thread);
    if (exit_code_out) *exit_code_out = SAO_EXIT_HANDOFF_TO_PYTHON;
    return 1;
}

// ===========================================================================
// Public API — test hooks
// ===========================================================================

extern "C" void sao_launcher_dual_run_set_test_spawn_hook(
    sao_dual_run_test_spawn_hook_t hook) {
    std::lock_guard<std::mutex> g(S().mtx);
    S().spawn_hook = hook;
}

extern "C" void sao_launcher_dual_run_set_test_probe_hook(
    sao_dual_run_test_probe_hook_t hook) {
    std::lock_guard<std::mutex> g(S().mtx);
    S().probe_hook = hook;
}

extern "C" void sao_launcher_dual_run_set_test_force_cpp_fail_hook(
    sao_dual_run_test_force_cpp_fail_hook_t hook) {
    std::lock_guard<std::mutex> g(S().mtx);
    S().force_cpp_fail_hook = hook;
}

extern "C" sao_status_t sao_launcher_dual_run_set_test_parent_argv(
    int argc, const wchar_t* const* argv) {
    if (argc < 0 || (argc > 0 && !argv)) {
        return SAO_STATUS_INVALID_ARGUMENT;
    }
    std::vector<std::wstring> snapshot;
    snapshot.reserve(static_cast<size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        snapshot.emplace_back(argv[index] ? argv[index] : L"");
    }
    std::lock_guard<std::mutex> g(S().mtx);
    S().parent_argv_override = std::move(snapshot);
    return SAO_STATUS_OK;
}
