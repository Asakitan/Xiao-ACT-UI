// SAO Auto - launcher/rollout_manager.cpp
//
// Wave 10 / Agent a - Phase 12 rollout system core.
//
// See rollout.h for the contract.  Everything here is Win32 + C runtime
// plus (optionally) the telemetry_client static library.  We roll a
// tiny hand-written JSON parser/writer to keep the launcher's link
// posture minimal (matches dual_run.cpp's style).
//
// This file does NOT modify any of the Wave 8a APIs defined in
// dual_run.cpp; it lives entirely alongside them.

#include "sao/launcher/rollout.h"
#include "sao/launcher/working_dir.h"

// Wave 9b telemetry_client is an optional dependency.  When the launcher
// is built with SAO_BUILD_SERVER=OFF we still need this file to compile;
// the emit_telemetry() helper then no-ops.  When the target IS available
// the CMake wiring sets SAO_LAUNCHER_HAS_TELEMETRY_CLIENT=1.
#if defined(SAO_LAUNCHER_HAS_TELEMETRY_CLIENT)
#  include "sao/server/freetier/telemetry_client/telemetry_client.h"
#endif

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <bcrypt.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {

// ---------------------------------------------------------------------------
// Module state (kept small - one mutex, a few overrides).
// ---------------------------------------------------------------------------
struct RolloutState {
    std::mutex mtx;
    std::wstring appdata_override;                        // empty = system default
    int64_t frozen_now_ms = 0;                            // 0 = live clock
    sao_rollout_test_telemetry_hook_t telemetry_hook = nullptr;
};

RolloutState& S() {
    static RolloutState s;
    return s;
}

// ---------------------------------------------------------------------------
// Time helper
// ---------------------------------------------------------------------------
int64_t now_ms_impl() {
    {
        std::lock_guard<std::mutex> g(S().mtx);
        if (S().frozen_now_ms != 0) return S().frozen_now_ms;
    }
    FILETIME ft{};
    ::GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // FILETIME is 100ns intervals since 1601-01-01 UTC.  Convert to Unix ms.
    constexpr uint64_t kFtEpochToUnixSec = 11644473600ULL;
    uint64_t total_100ns = u.QuadPart;
    uint64_t sec = total_100ns / 10000000ULL;
    uint64_t frac_ms = (total_100ns / 10000ULL) % 1000ULL;
    return static_cast<int64_t>((sec - kFtEpochToUnixSec) * 1000ULL + frac_ms);
}

// ---------------------------------------------------------------------------
// UTF-8 <-> UTF-16
// ---------------------------------------------------------------------------
std::wstring utf8_to_wide(const std::string& in) {
    if (in.empty()) return {};
    int wide_len = ::MultiByteToWideChar(CP_UTF8, 0, in.data(),
                                          static_cast<int>(in.size()),
                                          nullptr, 0);
    if (wide_len <= 0) return {};
    std::wstring out(static_cast<size_t>(wide_len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, in.data(),
                          static_cast<int>(in.size()),
                          out.data(), wide_len);
    return out;
}

// ---------------------------------------------------------------------------
// AppData paths
// ---------------------------------------------------------------------------
bool default_appdata_dir(wchar_t* out, size_t out_cap) {
    {
        std::lock_guard<std::mutex> g(S().mtx);
        if (!S().appdata_override.empty()) {
            ::lstrcpynW(out, S().appdata_override.c_str(), static_cast<int>(out_cap));
            return true;
        }
    }
    PWSTR appdata = nullptr;
    HRESULT hr = ::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata);
    if (FAILED(hr) || !appdata) {
        if (appdata) ::CoTaskMemFree(appdata);
        return false;
    }
    _snwprintf_s(out, out_cap, _TRUNCATE, L"%s\\SaoAuto", appdata);
    ::CoTaskMemFree(appdata);
    return true;
}

bool default_rollout_config_path(wchar_t* out, size_t out_cap) {
    wchar_t base[MAX_PATH]{};
    if (!default_appdata_dir(base, MAX_PATH)) return false;
    _snwprintf_s(out, out_cap, _TRUNCATE, L"%s\\rollout.json", base);
    return true;
}

bool default_rollout_stats_path(wchar_t* out, size_t out_cap) {
    wchar_t base[MAX_PATH]{};
    if (!default_appdata_dir(base, MAX_PATH)) return false;
    _snwprintf_s(out, out_cap, _TRUNCATE, L"%s\\rollout_stats.json", base);
    return true;
}

// ---------------------------------------------------------------------------
// File I/O (small files only; 1 MiB cap).
// ---------------------------------------------------------------------------
bool read_file_bytes(const wchar_t* path, std::string& out) {
    HANDLE h = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart > 1 * 1024 * 1024) {
        ::CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD read = 0;
    if (!out.empty()) {
        if (!::ReadFile(h, out.data(), static_cast<DWORD>(out.size()),
                         &read, nullptr) || read != out.size()) {
            ::CloseHandle(h);
            return false;
        }
    }
    ::CloseHandle(h);
    return true;
}

bool write_file_bytes(const wchar_t* path, const std::string& in) {
    wchar_t parent[MAX_PATH]{};
    ::lstrcpynW(parent, path, MAX_PATH);
    ::PathRemoveFileSpecW(parent);
    if (parent[0]) sao::launcher::ensureDirectoryExists(parent);

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
// JSON emit helpers
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

std::string int64_to_string(int64_t v) {
    char buf[32]{};
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%lld", static_cast<long long>(v));
    return buf;
}

std::string int32_to_string(int32_t v) {
    char buf[16]{};
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", v);
    return buf;
}

// ---------------------------------------------------------------------------
// Minimal JSON parser - matches dual_run.cpp's style but adds bool/array
// values because rollout schema has retreat_history arrays.
// ---------------------------------------------------------------------------
struct JsonToken {
    enum Kind { KEnd, KLBrace, KRBrace, KLBracket, KRBracket, KColon, KComma,
                KString, KNumber, KNull, KBool };
    Kind kind = KEnd;
    std::string sval;
    int64_t nval = 0;
    bool bval = false;
};

struct JsonLexer {
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

    JsonToken next() {
        JsonToken t;
        skip_ws();
        if (p >= end) { t.kind = JsonToken::KEnd; return t; }
        char c = *p;
        switch (c) {
            case '{': ++p; t.kind = JsonToken::KLBrace;   return t;
            case '}': ++p; t.kind = JsonToken::KRBrace;   return t;
            case '[': ++p; t.kind = JsonToken::KLBracket; return t;
            case ']': ++p; t.kind = JsonToken::KRBracket; return t;
            case ':': ++p; t.kind = JsonToken::KColon;    return t;
            case ',': ++p; t.kind = JsonToken::KComma;    return t;
            case '"': {
                ++p;
                std::string s;
                while (p < end && *p != '"') {
                    char cc = *p++;
                    if (cc == '\\' && p < end) {
                        char esc = *p++;
                        switch (esc) {
                            case '"':  s.push_back('"');  break;
                            case '\\': s.push_back('\\'); break;
                            case '/':  s.push_back('/');  break;
                            case 'n':  s.push_back('\n'); break;
                            case 't':  s.push_back('\t'); break;
                            case 'r':  s.push_back('\r'); break;
                            case 'b':  s.push_back('\b'); break;
                            case 'f':  s.push_back('\f'); break;
                            default:   s.push_back(esc);  break;
                        }
                    } else {
                        s.push_back(cc);
                    }
                }
                if (p >= end) { ok = false; return t; }
                ++p; // closing quote
                t.kind = JsonToken::KString;
                t.sval = std::move(s);
                return t;
            }
            case 't':
                if (end - p >= 4 && std::memcmp(p, "true", 4) == 0) {
                    p += 4; t.kind = JsonToken::KBool; t.bval = true; return t;
                }
                ok = false; return t;
            case 'f':
                if (end - p >= 5 && std::memcmp(p, "false", 5) == 0) {
                    p += 5; t.kind = JsonToken::KBool; t.bval = false; return t;
                }
                ok = false; return t;
            case 'n':
                if (end - p >= 4 && std::memcmp(p, "null", 4) == 0) {
                    p += 4; t.kind = JsonToken::KNull; return t;
                }
                ok = false; return t;
            default: {
                if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
                    const char* start = p;
                    if (*p == '-' || *p == '+') ++p;
                    while (p < end && *p >= '0' && *p <= '9') ++p;
                    if (p == start) { ok = false; return t; }
                    std::string num(start, p - start);
                    t.kind = JsonToken::KNumber;
                    t.nval = _atoi64(num.c_str());
                    return t;
                }
                ok = false; return t;
            }
        }
    }

    JsonToken peek() {
        const char* saved = p;
        bool saved_ok = ok;
        JsonToken t = next();
        p = saved;
        ok = saved_ok;
        return t;
    }
};

// ---------------------------------------------------------------------------
// user_override serialisation
// ---------------------------------------------------------------------------
const char* user_override_to_string(sao_rollout_user_override_t o) {
    switch (o) {
        case SAO_ROLLOUT_USER_OVERRIDE_CPP:    return "cpp";
        case SAO_ROLLOUT_USER_OVERRIDE_PYTHON: return "python";
        case SAO_ROLLOUT_USER_OVERRIDE_AUTO:   return "auto";
        default:                                return "null";
    }
}

sao_rollout_user_override_t user_override_from_string(const std::string& s) {
    if (s == "cpp")    return SAO_ROLLOUT_USER_OVERRIDE_CPP;
    if (s == "python") return SAO_ROLLOUT_USER_OVERRIDE_PYTHON;
    if (s == "auto")   return SAO_ROLLOUT_USER_OVERRIDE_AUTO;
    return SAO_ROLLOUT_USER_OVERRIDE_UNSET;
}

// ---------------------------------------------------------------------------
// Config serialisation
// ---------------------------------------------------------------------------
std::string serialise_config(const sao_rollout_config& cfg) {
    std::string s;
    s.reserve(1024);
    s += "{\n";
    s += "  \"schema\": ";
    s += int32_to_string(cfg.schema);
    s += ",\n";
    s += "  \"cpp_percent\": ";
    s += int32_to_string(cfg.cpp_percent);
    s += ",\n";
    s += "  \"user_override\": ";
    if (cfg.user_override == SAO_ROLLOUT_USER_OVERRIDE_UNSET) {
        s += "null";
    } else {
        json_emit_string(s, user_override_to_string(cfg.user_override));
    }
    s += ",\n";
    s += "  \"retreat_history\": [";
    for (int32_t i = 0; i < cfg.retreat_history_count
                            && i < SAO_ROLLOUT_MAX_RETREAT_HISTORY; ++i) {
        if (i > 0) s += ",";
        s += "\n    {";
        s += "\"ts_ms\": ";
        s += int64_to_string(cfg.retreat_history[i].ts_ms);
        s += ", \"old_percent\": ";
        s += int32_to_string(cfg.retreat_history[i].old_percent);
        s += ", \"new_percent\": ";
        s += int32_to_string(cfg.retreat_history[i].new_percent);
        s += ", \"reason\": ";
        json_emit_string(s, std::string(cfg.retreat_history[i].reason));
        s += "}";
    }
    if (cfg.retreat_history_count > 0) s += "\n  ";
    s += "]\n";
    s += "}\n";
    return s;
}

// Parse a single retreat_history array entry.
bool parse_retreat_entry(JsonLexer& lx, sao_rollout_retreat_entry& out) {
    std::memset(&out, 0, sizeof(out));
    auto tok = lx.next();
    if (!lx.ok || tok.kind != JsonToken::KLBrace) return false;
    // Loop pairs until '}'.
    while (lx.ok) {
        auto k = lx.next();
        if (!lx.ok) return false;
        if (k.kind == JsonToken::KRBrace) return true;
        if (k.kind != JsonToken::KString) return false;
        auto colon = lx.next();
        if (!lx.ok || colon.kind != JsonToken::KColon) return false;
        auto v = lx.next();
        if (!lx.ok) return false;
        if (k.sval == "ts_ms" && v.kind == JsonToken::KNumber) {
            out.ts_ms = v.nval;
        } else if (k.sval == "old_percent" && v.kind == JsonToken::KNumber) {
            out.old_percent = static_cast<int32_t>(v.nval);
        } else if (k.sval == "new_percent" && v.kind == JsonToken::KNumber) {
            out.new_percent = static_cast<int32_t>(v.nval);
        } else if (k.sval == "reason" && v.kind == JsonToken::KString) {
            ::lstrcpynA(out.reason, v.sval.c_str(), sizeof(out.reason));
        }
        auto next_or_end = lx.peek();
        if (!lx.ok) return false;
        if (next_or_end.kind == JsonToken::KComma) { lx.next(); continue; }
        if (next_or_end.kind == JsonToken::KRBrace) {
            lx.next();
            return true;
        }
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Stats serialisation (schema is closed and only 5 slots, so this is small).
// ---------------------------------------------------------------------------
std::string serialise_stats(const sao_rollout_stats& st) {
    std::string s;
    s.reserve(1024);
    s += "{\n";
    s += "  \"schema\": 1,\n";
    s += "  \"total_successes\": ";
    s += int64_to_string(st.total_successes);
    s += ",\n";
    s += "  \"total_failures\": ";
    s += int64_to_string(st.total_failures);
    s += ",\n";
    s += "  \"recent\": [";
    for (int32_t i = 0; i < st.recent_count
                            && i < SAO_ROLLOUT_MAX_RECENT_RESULTS; ++i) {
        if (i > 0) s += ",";
        s += "\n    {";
        s += "\"ts_ms\": ";
        s += int64_to_string(st.recent[i].ts_ms);
        s += ", \"mode\": ";
        s += int32_to_string(st.recent[i].mode);
        s += ", \"success\": ";
        s += int32_to_string(st.recent[i].success);
        s += ", \"duration_ms\": ";
        s += int32_to_string(st.recent[i].duration_ms);
        s += ", \"reason_code\": ";
        s += int32_to_string(st.recent[i].reason_code);
        s += "}";
    }
    if (st.recent_count > 0) s += "\n  ";
    s += "]\n";
    s += "}\n";
    return s;
}

bool parse_stats_entry(JsonLexer& lx, sao_rollout_stats_entry& out) {
    std::memset(&out, 0, sizeof(out));
    auto tok = lx.next();
    if (!lx.ok || tok.kind != JsonToken::KLBrace) return false;
    while (lx.ok) {
        auto k = lx.next();
        if (!lx.ok) return false;
        if (k.kind == JsonToken::KRBrace) return true;
        if (k.kind != JsonToken::KString) return false;
        auto colon = lx.next();
        if (!lx.ok || colon.kind != JsonToken::KColon) return false;
        auto v = lx.next();
        if (!lx.ok || v.kind != JsonToken::KNumber) return false;
        if      (k.sval == "ts_ms")       out.ts_ms       = v.nval;
        else if (k.sval == "mode")        out.mode        = static_cast<int32_t>(v.nval);
        else if (k.sval == "success")     out.success     = static_cast<int32_t>(v.nval);
        else if (k.sval == "duration_ms") out.duration_ms = static_cast<int32_t>(v.nval);
        else if (k.sval == "reason_code") out.reason_code = static_cast<int32_t>(v.nval);
        auto next_or_end = lx.peek();
        if (!lx.ok) return false;
        if (next_or_end.kind == JsonToken::KComma) { lx.next(); continue; }
        if (next_or_end.kind == JsonToken::KRBrace) {
            lx.next();
            return true;
        }
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// HMAC-SHA-256 via BCrypt
// ---------------------------------------------------------------------------
bool hmac_sha256(const uint8_t* key, size_t key_len,
                  const uint8_t* data, size_t data_len,
                  uint8_t out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(
            &alg, BCRYPT_SHA256_ALGORITHM, nullptr,
            BCRYPT_ALG_HANDLE_HMAC_FLAG))) {
        return false;
    }
    BCRYPT_HASH_HANDLE h = nullptr;
    NTSTATUS ok = ::BCryptCreateHash(alg, &h, nullptr, 0,
                                       const_cast<PUCHAR>(key),
                                       static_cast<ULONG>(key_len), 0);
    if (!BCRYPT_SUCCESS(ok)) {
        ::BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    ok = ::BCryptHashData(h, const_cast<PUCHAR>(data),
                            static_cast<ULONG>(data_len), 0);
    if (!BCRYPT_SUCCESS(ok)) {
        ::BCryptDestroyHash(h);
        ::BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    ok = ::BCryptFinishHash(h, out, 32, 0);
    ::BCryptDestroyHash(h);
    ::BCryptCloseAlgorithmProvider(alg, 0);
    return BCRYPT_SUCCESS(ok);
}

// ---------------------------------------------------------------------------
// Telemetry emit (with test hook + safe fallback when telemetry_init has not
// been called on this process).
// ---------------------------------------------------------------------------
void emit_telemetry(const char* event_name, const std::string& props_json) {
    sao_rollout_test_telemetry_hook_t hook = nullptr;
    {
        std::lock_guard<std::mutex> g(S().mtx);
        hook = S().telemetry_hook;
    }
    if (hook) {
        hook(event_name, props_json.c_str());
        return;
    }
#if defined(SAO_LAUNCHER_HAS_TELEMETRY_CLIENT)
    // Best-effort - the telemetry client is initialised at process
    // startup by the launcher's init pipeline.  When absent, track_event
    // returns SAO_ERR_NOT_INITIALIZED and we swallow it (rollout events
    // are advisory).
    (void)::sao_telemetry_track_event(event_name, props_json.c_str());
#else
    // Telemetry client not linked in - drop the event silently.  Tests
    // that observe telemetry set the hook above; production launches
    // that need telemetry compile with the target on.
    (void)event_name;
    (void)props_json;
#endif
}

// ---------------------------------------------------------------------------
// Retreat history pruning: keep the most-recent N slots.
// ---------------------------------------------------------------------------
void push_retreat_history(sao_rollout_config& cfg,
                           const sao_rollout_retreat_entry& e) {
    if (cfg.retreat_history_count < SAO_ROLLOUT_MAX_RETREAT_HISTORY) {
        cfg.retreat_history[cfg.retreat_history_count++] = e;
        return;
    }
    // Shift left by one, drop the oldest, append new.
    for (int32_t i = 1; i < SAO_ROLLOUT_MAX_RETREAT_HISTORY; ++i) {
        cfg.retreat_history[i - 1] = cfg.retreat_history[i];
    }
    cfg.retreat_history[SAO_ROLLOUT_MAX_RETREAT_HISTORY - 1] = e;
}

// Circular buffer for recent stats.
void push_recent_stat(sao_rollout_stats& st,
                       const sao_rollout_stats_entry& e) {
    if (st.recent_count < SAO_ROLLOUT_MAX_RECENT_RESULTS) {
        st.recent[st.recent_count++] = e;
        return;
    }
    for (int32_t i = 1; i < SAO_ROLLOUT_MAX_RECENT_RESULTS; ++i) {
        st.recent[i - 1] = st.recent[i];
    }
    st.recent[SAO_ROLLOUT_MAX_RECENT_RESULTS - 1] = e;
}

} // namespace

// ===========================================================================
// Public API - config
// ===========================================================================
extern "C" void sao_rollout_config_default(sao_rollout_config* cfg) {
    if (!cfg) return;
    std::memset(cfg, 0, sizeof(*cfg));
    cfg->schema = 1;
    cfg->cpp_percent = 100;
    cfg->user_override = SAO_ROLLOUT_USER_OVERRIDE_UNSET;
    cfg->retreat_history_count = 0;
}

extern "C" sao_status_t sao_rollout_config_load(sao_rollout_config* cfg_out) {
    wchar_t path[MAX_PATH]{};
    if (!default_rollout_config_path(path, MAX_PATH)) {
        sao_rollout_config_default(cfg_out);
        return SAO_STATUS_OK;
    }
    return sao_rollout_config_load_from_path(path, cfg_out);
}

extern "C" sao_status_t sao_rollout_config_load_from_path(
    const wchar_t* path, sao_rollout_config* cfg_out) {
    if (!cfg_out) return SAO_STATUS_INVALID_ARGUMENT;
    sao_rollout_config_default(cfg_out);
    if (!path || !*path) return SAO_STATUS_INVALID_ARGUMENT;

    // Missing file -> defaults are already loaded, treat as OK.
    if (::GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        return SAO_STATUS_OK;
    }

    std::string blob;
    if (!read_file_bytes(path, blob)) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;

    JsonLexer lx;
    lx.p = blob.data();
    lx.end = blob.data() + blob.size();

    auto open = lx.next();
    if (!lx.ok || open.kind != JsonToken::KLBrace) {
        return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
    }

    while (lx.ok) {
        auto k = lx.next();
        if (!lx.ok) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
        if (k.kind == JsonToken::KRBrace) break;
        if (k.kind != JsonToken::KString) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
        auto colon = lx.next();
        if (!lx.ok || colon.kind != JsonToken::KColon) {
            return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
        }
        if (k.sval == "schema") {
            auto v = lx.next();
            if (!lx.ok || v.kind != JsonToken::KNumber) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
            cfg_out->schema = static_cast<int32_t>(v.nval);
        } else if (k.sval == "cpp_percent") {
            auto v = lx.next();
            if (!lx.ok || v.kind != JsonToken::KNumber) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
            int32_t p = static_cast<int32_t>(v.nval);
            if (p < 0)   p = 0;
            if (p > 100) p = 100;
            cfg_out->cpp_percent = p;
        } else if (k.sval == "user_override") {
            auto v = lx.next();
            if (!lx.ok) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
            if (v.kind == JsonToken::KNull) {
                cfg_out->user_override = SAO_ROLLOUT_USER_OVERRIDE_UNSET;
            } else if (v.kind == JsonToken::KString) {
                cfg_out->user_override = user_override_from_string(v.sval);
            } else {
                return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
            }
        } else if (k.sval == "retreat_history") {
            auto lb = lx.next();
            if (!lx.ok || lb.kind != JsonToken::KLBracket) {
                return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
            }
            cfg_out->retreat_history_count = 0;
            // Handle empty array.
            auto peek0 = lx.peek();
            if (peek0.kind == JsonToken::KRBracket) {
                lx.next();
            } else {
                while (lx.ok) {
                    sao_rollout_retreat_entry e{};
                    if (!parse_retreat_entry(lx, e)) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
                    if (cfg_out->retreat_history_count < SAO_ROLLOUT_MAX_RETREAT_HISTORY) {
                        cfg_out->retreat_history[cfg_out->retreat_history_count++] = e;
                    }
                    auto sep = lx.next();
                    if (!lx.ok) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
                    if (sep.kind == JsonToken::KComma) continue;
                    if (sep.kind == JsonToken::KRBracket) break;
                    return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
                }
            }
        } else {
            // Unknown key - accept + drop a scalar or object.
            auto v = lx.next();
            if (!lx.ok) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
            if (v.kind == JsonToken::KLBrace) {
                int depth = 1;
                while (lx.ok && depth > 0) {
                    auto t = lx.next();
                    if (!lx.ok) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
                    if (t.kind == JsonToken::KLBrace)  ++depth;
                    if (t.kind == JsonToken::KRBrace)  --depth;
                }
            } else if (v.kind == JsonToken::KLBracket) {
                int depth = 1;
                while (lx.ok && depth > 0) {
                    auto t = lx.next();
                    if (!lx.ok) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
                    if (t.kind == JsonToken::KLBracket)  ++depth;
                    if (t.kind == JsonToken::KRBracket)  --depth;
                }
            }
        }
        auto sep = lx.peek();
        if (!lx.ok) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
        if (sep.kind == JsonToken::KComma) { lx.next(); continue; }
        if (sep.kind == JsonToken::KRBrace) { lx.next(); break; }
        return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t sao_rollout_config_save(const sao_rollout_config* cfg) {
    if (!cfg) return SAO_STATUS_INVALID_ARGUMENT;
    wchar_t path[MAX_PATH]{};
    if (!default_rollout_config_path(path, MAX_PATH)) {
        return SAO_ROLLOUT_CONFIG_WRITE_FAILED;
    }
    return sao_rollout_config_save_to_path(path, cfg);
}

extern "C" sao_status_t sao_rollout_config_save_to_path(
    const wchar_t* path, const sao_rollout_config* cfg) {
    if (!path || !cfg) return SAO_STATUS_INVALID_ARGUMENT;
    std::string blob = serialise_config(*cfg);
    if (!write_file_bytes(path, blob)) return SAO_ROLLOUT_CONFIG_WRITE_FAILED;
    return SAO_STATUS_OK;
}

// ===========================================================================
// Public API - stats
// ===========================================================================
extern "C" sao_status_t sao_rollout_stats_load(sao_rollout_stats* stats_out) {
    wchar_t path[MAX_PATH]{};
    if (!default_rollout_stats_path(path, MAX_PATH)) {
        if (stats_out) std::memset(stats_out, 0, sizeof(*stats_out));
        return SAO_STATUS_OK;
    }
    return sao_rollout_stats_load_from_path(path, stats_out);
}

extern "C" sao_status_t sao_rollout_stats_load_from_path(
    const wchar_t* path, sao_rollout_stats* stats_out) {
    if (!stats_out) return SAO_STATUS_INVALID_ARGUMENT;
    std::memset(stats_out, 0, sizeof(*stats_out));
    if (!path || !*path) return SAO_STATUS_INVALID_ARGUMENT;
    if (::GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        return SAO_STATUS_OK;
    }

    std::string blob;
    if (!read_file_bytes(path, blob)) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;

    JsonLexer lx;
    lx.p = blob.data();
    lx.end = blob.data() + blob.size();
    auto open = lx.next();
    if (!lx.ok || open.kind != JsonToken::KLBrace) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
    while (lx.ok) {
        auto k = lx.next();
        if (!lx.ok) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
        if (k.kind == JsonToken::KRBrace) break;
        if (k.kind != JsonToken::KString) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
        auto colon = lx.next();
        if (!lx.ok || colon.kind != JsonToken::KColon) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
        if (k.sval == "total_successes" || k.sval == "total_failures"
            || k.sval == "schema") {
            auto v = lx.next();
            if (!lx.ok || v.kind != JsonToken::KNumber) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
            if (k.sval == "total_successes") stats_out->total_successes = v.nval;
            else if (k.sval == "total_failures") stats_out->total_failures = v.nval;
        } else if (k.sval == "recent") {
            auto lb = lx.next();
            if (!lx.ok || lb.kind != JsonToken::KLBracket) {
                return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
            }
            stats_out->recent_count = 0;
            auto peek0 = lx.peek();
            if (peek0.kind == JsonToken::KRBracket) {
                lx.next();
            } else {
                while (lx.ok) {
                    sao_rollout_stats_entry e{};
                    if (!parse_stats_entry(lx, e)) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
                    if (stats_out->recent_count < SAO_ROLLOUT_MAX_RECENT_RESULTS) {
                        stats_out->recent[stats_out->recent_count++] = e;
                    }
                    auto sep = lx.next();
                    if (!lx.ok) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
                    if (sep.kind == JsonToken::KComma) continue;
                    if (sep.kind == JsonToken::KRBracket) break;
                    return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
                }
            }
        } else {
            auto v = lx.next();
            (void)v;
            if (!lx.ok) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
        }
        auto sep = lx.peek();
        if (!lx.ok) return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
        if (sep.kind == JsonToken::KComma) { lx.next(); continue; }
        if (sep.kind == JsonToken::KRBrace) { lx.next(); break; }
        return SAO_ROLLOUT_CONFIG_PARSE_FAILED;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t sao_rollout_stats_save(const sao_rollout_stats* stats) {
    if (!stats) return SAO_STATUS_INVALID_ARGUMENT;
    wchar_t path[MAX_PATH]{};
    if (!default_rollout_stats_path(path, MAX_PATH)) {
        return SAO_ROLLOUT_STATS_WRITE_FAILED;
    }
    return sao_rollout_stats_save_to_path(path, stats);
}

extern "C" sao_status_t sao_rollout_stats_save_to_path(
    const wchar_t* path, const sao_rollout_stats* stats) {
    if (!path || !stats) return SAO_STATUS_INVALID_ARGUMENT;
    std::string blob = serialise_stats(*stats);
    if (!write_file_bytes(path, blob)) return SAO_ROLLOUT_STATS_WRITE_FAILED;
    return SAO_STATUS_OK;
}

// ===========================================================================
// Public API - bucket
// ===========================================================================
extern "C" int32_t sao_rollout_compute_bucket(const char* anon_id,
                                                const char* salt) {
    if (!anon_id || !anon_id[0]) return -1;
    const char* effective_salt = (salt && salt[0]) ? salt : SAO_ROLLOUT_DEFAULT_SALT;

    uint8_t mac[32]{};
    if (!hmac_sha256(reinterpret_cast<const uint8_t*>(effective_salt),
                       std::strlen(effective_salt),
                       reinterpret_cast<const uint8_t*>(anon_id),
                       std::strlen(anon_id),
                       mac)) {
        return -1;
    }
    // Take the high 64 bits and reduce mod 100.  100 is not a divisor of
    // 2^64, but 2^64 / 100 * 100 is very close (1.844e19) so the bias is
    // ~5.4e-18 - vastly below any statistical detection.
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | mac[i];
    }
    return static_cast<int32_t>(v % 100ULL);
}

extern "C" int32_t sao_rollout_should_use_cpp(int32_t bucket,
                                                const sao_rollout_config* cfg) {
    if (!cfg) return 0;
    if (cfg->user_override == SAO_ROLLOUT_USER_OVERRIDE_CPP)    return 1;
    if (cfg->user_override == SAO_ROLLOUT_USER_OVERRIDE_PYTHON) return 0;
    // UNSET or AUTO fall through to bucket logic.
    if (cfg->cpp_percent >= 100) return 1;
    if (cfg->cpp_percent <= 0)   return 0;
    if (bucket < 0) {
        // Unknown bucket - safer default: use CPP only when cpp_percent
        // reached 100.  We already returned above in that case, so 0 here.
        return 0;
    }
    return bucket < cfg->cpp_percent ? 1 : 0;
}

// ===========================================================================
// Public API - record_success / record_failure
// ===========================================================================
namespace {

std::string mode_name(sao_launcher_dual_run_mode_t mode) {
    switch (mode) {
        case SAO_DUAL_RUN_MODE_CPP_ONLY:                        return "cpp_only";
        case SAO_DUAL_RUN_MODE_PYTHON_ONLY:                     return "python_only";
        case SAO_DUAL_RUN_MODE_CPP_PREFERRED_PYTHON_FALLBACK:   return "cpp_preferred";
        case SAO_DUAL_RUN_MODE_PYTHON_PREFERRED_CPP_FALLBACK:   return "python_preferred";
        case SAO_DUAL_RUN_MODE_DUAL_SIDE_BY_SIDE:               return "dual_side_by_side";
        default:                                                 return "unknown";
    }
}

int32_t is_cpp_mode(sao_launcher_dual_run_mode_t mode) {
    return (mode == SAO_DUAL_RUN_MODE_CPP_ONLY
            || mode == SAO_DUAL_RUN_MODE_CPP_PREFERRED_PYTHON_FALLBACK) ? 1 : 0;
}

std::string build_props(const std::pair<const char*, std::string> kvs[], size_t n) {
    std::string s = "{";
    for (size_t i = 0; i < n; ++i) {
        if (i > 0) s += ", ";
        s += "\"";
        s += kvs[i].first;
        s += "\": ";
        s += kvs[i].second;
    }
    s += "}";
    return s;
}

} // namespace

extern "C" sao_status_t sao_rollout_record_success(
    sao_launcher_dual_run_mode_t mode, int32_t duration_ms) {
    sao_rollout_stats st{};
    (void)sao_rollout_stats_load(&st);
    st.total_successes += 1;
    sao_rollout_stats_entry e{};
    e.ts_ms = now_ms_impl();
    e.mode = mode;
    e.success = 1;
    e.duration_ms = duration_ms;
    e.reason_code = 0;
    push_recent_stat(st, e);
    (void)sao_rollout_stats_save(&st);

    std::string mode_quoted = "\"" + mode_name(mode) + "\"";
    std::pair<const char*, std::string> kv[] = {
        {"mode",         mode_quoted},
        {"duration_ms",  int32_to_string(duration_ms)},
    };
    emit_telemetry("sao.rollout.launch_success", build_props(kv, 2));
    return SAO_STATUS_OK;
}

extern "C" sao_status_t sao_rollout_record_failure(
    sao_launcher_dual_run_mode_t mode, int32_t reason_code,
    const char* stack_hint) {
    sao_rollout_stats st{};
    (void)sao_rollout_stats_load(&st);
    st.total_failures += 1;
    sao_rollout_stats_entry e{};
    e.ts_ms = now_ms_impl();
    e.mode = mode;
    e.success = 0;
    e.duration_ms = 0;
    e.reason_code = reason_code;
    push_recent_stat(st, e);
    (void)sao_rollout_stats_save(&st);

    std::string mode_quoted = "\"" + mode_name(mode) + "\"";
    std::string reason_quoted = int32_to_string(reason_code);
    std::string hint_str;
    hint_str.reserve(80);
    json_emit_string(hint_str, stack_hint ? stack_hint : "");
    std::pair<const char*, std::string> kv[] = {
        {"mode",         mode_quoted},
        {"reason_code",  reason_quoted},
        {"stack_hint",   hint_str},
    };
    emit_telemetry("sao.rollout.launch_failure", build_props(kv, 3));
    return SAO_STATUS_OK;
}

// ===========================================================================
// Public API - check_auto_retreat
// ===========================================================================
extern "C" int32_t sao_rollout_check_auto_retreat(int32_t* new_percent_out) {
    if (new_percent_out) *new_percent_out = -1;

    sao_rollout_stats st{};
    (void)sao_rollout_stats_load(&st);
    if (st.recent_count < 3) return 0;

    // Only count CPP-flavoured runs.  A pure PYTHON_ONLY run failing has no
    // bearing on the CPP ramp.
    int32_t cpp_failures = 0;
    int32_t cpp_total    = 0;
    for (int32_t i = 0; i < st.recent_count; ++i) {
        if (!is_cpp_mode(st.recent[i].mode)) continue;
        ++cpp_total;
        if (!st.recent[i].success) ++cpp_failures;
    }
    if (cpp_total < 3) return 0;
    if (cpp_failures < 3) return 0;

    // Load, halve, save.
    sao_rollout_config cfg{};
    (void)sao_rollout_config_load(&cfg);
    int32_t old_percent = cfg.cpp_percent;
    int32_t new_percent = old_percent / 2;
    if (new_percent < 0) new_percent = 0;
    if (old_percent == new_percent) {
        // Already at 0; nothing to retreat.
        return 0;
    }
    cfg.cpp_percent = new_percent;

    sao_rollout_retreat_entry re{};
    re.ts_ms = now_ms_impl();
    re.old_percent = old_percent;
    re.new_percent = new_percent;
    ::lstrcpynA(re.reason, "3_of_5_failed", sizeof(re.reason));
    push_retreat_history(cfg, re);

    (void)sao_rollout_config_save(&cfg);

    std::string old_pct = int32_to_string(old_percent);
    std::string new_pct = int32_to_string(new_percent);
    std::pair<const char*, std::string> kv[] = {
        {"old_percent", old_pct},
        {"new_percent", new_pct},
        {"reason",      std::string("\"3_of_5_failed\"")},
    };
    emit_telemetry("sao.rollout.auto_retreat", build_props(kv, 3));

    if (new_percent_out) *new_percent_out = new_percent;
    return 1;
}

// ===========================================================================
// Public API - test hooks
// ===========================================================================
extern "C" void sao_rollout_test_set_appdata_dir(const wchar_t* dir) {
    std::lock_guard<std::mutex> g(S().mtx);
    if (!dir || !dir[0]) {
        S().appdata_override.clear();
    } else {
        S().appdata_override = dir;
    }
}

extern "C" void sao_rollout_test_set_now_ms(int64_t frozen_ms) {
    std::lock_guard<std::mutex> g(S().mtx);
    S().frozen_now_ms = frozen_ms;
}

extern "C" void sao_rollout_test_set_telemetry_hook(
    sao_rollout_test_telemetry_hook_t hook) {
    std::lock_guard<std::mutex> g(S().mtx);
    S().telemetry_hook = hook;
}

extern "C" void sao_rollout_reset_for_test(void) {
    std::lock_guard<std::mutex> g(S().mtx);
    S().appdata_override.clear();
    S().frozen_now_ms = 0;
    S().telemetry_hook = nullptr;
}
