// SAO Auto -- runtime installer core.
//
// This file owns the process-wide installer state:
//
//   * The currently bound manifest as a process-owned shared snapshot.
//     Caller handles remain independently releasable.
//   * A per-runtime named mutex so two threads / two processes cannot race
//     on the same ensure() call.
//   * The download / verify / extract / commit pipeline itself.
//   * Test hooks for offline unit tests (compiled in only when the
//     SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS macro is set).
//
// Directory layout (persisted):
//
//     <root>\<opaque_kind_id>\
//        current.json            -- marker with payload/tree integrity hashes
//        current\               -- installed tree
//        staging\<guid>\        -- transient work directory
//
// Fail-closed contract: ensure() never leaves the staging directory behind.
// A crash mid-download leaves an orphan staging tree but never taints the
// committed current\ directory.

#include "sao/runtime_installer/manifest.h"
#include "sao/runtime_installer/runtime_installer.h"

#include "download_engine_internal.h"
#include "integrity_internal.h"
#include "manifest_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>

#include "sao_security/obfuscation/enc_str.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Process-wide state.
// ---------------------------------------------------------------------------
namespace {

using sao::runtime_installer::internal::DownloadSink;
using sao::runtime_installer::internal::HashPipeline;
using sao::runtime_installer::internal::Manifest;
using sao::runtime_installer::internal::ManifestEntry;

struct InstallerState {
    std::mutex mutex;
    std::shared_ptr<const Manifest> bound_manifest;
    std::unordered_map<sao_runtime_manifest_s*, std::shared_ptr<Manifest>> handles;
#if defined(SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)
    sao_runtime_installer_test_transport_t test_transport = nullptr;
    void*                                  test_transport_user_data = nullptr;
    std::string                            test_root_override;
    uint32_t                               failpoints = 0;
    uint32_t                               tar_timeout_ms = 30'000;
    uint32_t                               named_mutex_timeout_ms = 5'000;
#endif
};

InstallerState& state() {
    static InstallerState s;
    return s;
}

#if defined(SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)
bool test_failpoint(uint32_t failpoint) noexcept {
    auto& st = state();
    std::lock_guard<std::mutex> guard(st.mutex);
    return (st.failpoints & failpoint) != 0;
}

DWORD tar_timeout_ms() noexcept {
    auto& st = state();
    std::lock_guard<std::mutex> guard(st.mutex);
    return st.tar_timeout_ms;
}

DWORD named_mutex_timeout_ms() noexcept {
    auto& st = state();
    std::lock_guard<std::mutex> guard(st.mutex);
    return st.named_mutex_timeout_ms;
}
#else
constexpr DWORD tar_timeout_ms() noexcept { return 30'000; }
#endif

}  // namespace

namespace sao::runtime_installer::internal {

ManifestSnapshot snapshot_manifest_handle(
    sao_runtime_manifest_handle_t handle) noexcept {
    if (handle == nullptr) return {};
    try {
        auto& st = state();
        std::lock_guard<std::mutex> guard(st.mutex);
        const auto it = st.handles.find(handle);
        if (it == st.handles.end()) return {};
        return it->second;
    } catch (...) {
        return {};
    }
}

sao_status_t register_manifest_handle(
    sao_runtime_manifest_handle_t handle,
    const std::shared_ptr<Manifest>& manifest) {
    if (handle == nullptr || !manifest) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto& st = state();
        std::lock_guard<std::mutex> guard(st.mutex);
        st.handles.emplace(handle, manifest);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

void release_manifest_handle(sao_runtime_manifest_handle_t handle) noexcept {
    if (handle == nullptr) return;
    bool registered = false;
    {
        auto& st = state();
        std::lock_guard<std::mutex> guard(st.mutex);
        registered = st.handles.erase(handle) != 0;
    }
    if (registered) delete handle;
}

}  // namespace sao::runtime_installer::internal

namespace {

// ---------------------------------------------------------------------------
// UTF-16 <-> UTF-8 helpers (small enough to inline; no dependency on core).
// ---------------------------------------------------------------------------
std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int size = ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<size_t>(size), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
        out.data(), size);
    return out;
}

std::string narrow(const std::wstring& utf16) {
    if (utf16.empty()) return {};
    const int size = ::WideCharToMultiByte(
        CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()),
        out.data(), size, nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------------------
// Deterministic opaque directory id per runtime kind.
//
// We hash a per-kind SAO_ENC_STR literal with FNV-1a-64 and format the low
// 48 bits as 12 lowercase hex chars.  Same input every build, but the
// literal itself never touches .rdata as plaintext.
// ---------------------------------------------------------------------------
uint64_t fnv1a64(const char* data, size_t length) noexcept {
    constexpr uint64_t kOffset = 0xcbf29ce484222325ULL;
    constexpr uint64_t kPrime  = 0x00000100000001b3ULL;
    uint64_t h = kOffset;
    for (size_t i = 0; i < length; ++i) {
        h ^= static_cast<uint64_t>(static_cast<unsigned char>(data[i]));
        h *= kPrime;
    }
    return h;
}

std::string opaque_id_for(sao_runtime_kind_t kind) {
    const char* raw = nullptr;
    // Each literal below is protected by SAO_ENC_STR so nobody grepping the
    // shipped binary can pattern-match against the plaintext kind names.
    const auto py = SAO_ENC_STR("rt.py311");
    const auto dn = SAO_ENC_STR("rt.dn8");
    const auto lu = SAO_ENC_STR("rt.lua54");
    const auto ag = SAO_ENC_STR("rt.angel");
    switch (kind) {
        case SAO_RUNTIME_KIND_PYTHON3_EMBED:   raw = py.decrypt(); break;
        case SAO_RUNTIME_KIND_DOTNET_RUNTIME:  raw = dn.decrypt(); break;
        case SAO_RUNTIME_KIND_LUA54_LIB:       raw = lu.decrypt(); break;
        case SAO_RUNTIME_KIND_ANGELSCRIPT_LIB: raw = ag.decrypt(); break;
        default: return {};
    }
    const uint64_t h = fnv1a64(raw, std::strlen(raw));
    std::array<char, 13> buf{};
    // Emit exactly 12 hex chars (48 bits) — deterministic across runs.
    std::snprintf(buf.data(), buf.size(), "%012llx",
                  static_cast<unsigned long long>(h & 0x0000ffffffffffffULL));
    return std::string(buf.data(), 12);
}

// ---------------------------------------------------------------------------
// Root directory resolution.
// ---------------------------------------------------------------------------
sao_status_t resolve_root(std::wstring& out_root) {
#if defined(SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)
    {
        auto& st = state();
        std::lock_guard<std::mutex> guard(st.mutex);
        if (!st.test_root_override.empty()) {
            out_root = widen(st.test_root_override);
            return SAO_STATUS_OK;
        }
    }
#endif
    PWSTR raw = nullptr;
    HRESULT hr = ::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw);
    if (FAILED(hr) || raw == nullptr) {
        if (raw != nullptr) ::CoTaskMemFree(raw);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    // Directory tail is SAO_ENC_STR'd so the shipped exe does not carry a
    // literal "SaoAuto\\runtimes" in .rdata.
    const auto tail = SAO_ENC_STR("\\SaoAuto\\runtimes");
    const char* tail_plain = tail.decrypt();
    std::wstring tail_wide = widen(tail_plain);
    out_root.assign(raw);
    ::CoTaskMemFree(raw);
    out_root.append(tail_wide);
    return SAO_STATUS_OK;
}

// ---------------------------------------------------------------------------
// Marker JSON I/O.  Tiny bespoke serializer -- we only ever need two
// scalar fields.
// ---------------------------------------------------------------------------
struct InstallMarker {
    std::string version;
    std::string sha256_hex;
    std::string tree_sha256_hex;
    std::string blake3_hex;   // empty when not applicable
};

void append_json_string(std::string& out, const std::string& value) {
    static constexpr char kHex[] = "0123456789abcdef";
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': out.append("\\\""); break;
        case '\\': out.append("\\\\"); break;
        case '\b': out.append("\\b"); break;
        case '\f': out.append("\\f"); break;
        case '\n': out.append("\\n"); break;
        case '\r': out.append("\\r"); break;
        case '\t': out.append("\\t"); break;
        default:
            if (ch < 0x20u) {
                out.append("\\u00");
                out.push_back(kHex[(ch >> 4u) & 0x0fu]);
                out.push_back(kHex[ch & 0x0fu]);
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
}

std::string serialize_marker(const InstallMarker& marker) {
    std::string out;
    out.reserve(320);
    out.append("{\"version\":\"");
    append_json_string(out, marker.version);
    out.append("\",\"sha256\":\"");
    append_json_string(out, marker.sha256_hex);
    if (!marker.tree_sha256_hex.empty()) {
        out.append("\",\"tree_sha256\":\"");
        append_json_string(out, marker.tree_sha256_hex);
    }
    if (!marker.blake3_hex.empty()) {
        out.append("\",\"blake3\":\"");
        append_json_string(out, marker.blake3_hex);
    }
    out.append("\"}");
    return out;
}

// Read a marker without depending on the manifest parser.  Very forgiving
// (only the two/three fields we ever write).
bool parse_marker(const std::string& text, InstallMarker& out) {
    auto extract = [&](const char* key, std::string& value) -> bool {
        const std::string needle = std::string("\"") + key + "\":\"";
        auto pos = text.find(needle);
        if (pos == std::string::npos) return false;
        pos += needle.size();
        value.clear();
        while (pos < text.size()) {
            const unsigned char ch = static_cast<unsigned char>(text[pos++]);
            if (ch == '"') return true;
            if (ch < 0x20u) return false;
            if (ch != '\\') {
                value.push_back(static_cast<char>(ch));
                continue;
            }
            if (pos >= text.size()) return false;
            const char escaped = text[pos++];
            switch (escaped) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case 'u': {
                if (pos + 4u > text.size() || text[pos] != '0' ||
                    text[pos + 1u] != '0') {
                    return false;
                }
                const auto nibble = [](char digit) -> int {
                    if (digit >= '0' && digit <= '9') return digit - '0';
                    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
                    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
                    return -1;
                };
                const int high = nibble(text[pos + 2u]);
                const int low = nibble(text[pos + 3u]);
                if (high < 0 || low < 0) return false;
                value.push_back(static_cast<char>((high << 4) | low));
                pos += 4u;
                break;
            }
            default: return false;
            }
        }
        return false;
    };
    if (!extract("version", out.version)) return false;
    if (!extract("sha256", out.sha256_hex)) return false;
    extract("tree_sha256", out.tree_sha256_hex);  // required for directory trees
    extract("blake3", out.blake3_hex);  // verified against required manifest field
    return true;
}

sao_status_t write_marker(const fs::path& path, const InstallMarker& marker) {
#if defined(SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)
    if (test_failpoint(SAO_RUNTIME_INSTALLER_TEST_FAIL_MARKER_WRITE)) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
#endif
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return SAO_STATUS_ERR_OS_CALL_FAILED;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) return SAO_STATUS_ERR_OS_CALL_FAILED;
    const std::string blob = serialize_marker(marker);
    stream.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    if (!stream.good()) return SAO_STATUS_ERR_OS_CALL_FAILED;
    return SAO_STATUS_OK;
}

sao_status_t read_marker(const fs::path& path, InstallMarker& marker) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) return SAO_STATUS_ERR_NOT_FOUND;
    std::string text((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());
    if (!parse_marker(text, marker)) return SAO_STATUS_ERR_UNKNOWN;
    return SAO_STATUS_OK;
}

// ---------------------------------------------------------------------------
// Filesystem helpers.  We use std::filesystem for cross-drive support and
// per-entry error propagation.
// ---------------------------------------------------------------------------
struct RuntimePaths {
    sao_runtime_kind_t kind = SAO_RUNTIME_KIND_INVALID;
    fs::path kind_root;   // <root>\<opaque>
    fs::path current;     // <root>\<opaque>\current
    fs::path marker;      // <root>\<opaque>\current.json
    fs::path staging;     // <root>\<opaque>\staging
    std::string opaque;
};

sao_status_t reject_reparse_if_present_(const fs::path& path) noexcept {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = ::GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
            ? SAO_STATUS_OK
            : SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u
        ? SAO_STATUS_ERR_ACCESS_DENIED
        : SAO_STATUS_OK;
}

sao_status_t build_paths(sao_runtime_kind_t kind, RuntimePaths& out) {
    if (kind <= SAO_RUNTIME_KIND_INVALID || kind >= SAO_RUNTIME_KIND_COUNT_) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::wstring root;
    if (const auto s = resolve_root(root); s != SAO_STATUS_OK) return s;
    out.kind = kind;
    out.opaque = opaque_id_for(kind);
    if (out.opaque.empty()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    out.kind_root = fs::path(root) / widen(out.opaque);
    out.current   = out.kind_root / L"current";
    out.marker    = out.kind_root / L"current.json";
    out.staging   = out.kind_root / L"staging";
    return SAO_STATUS_OK;
}

sao_status_t copy_string_out(const std::string& src,
                             char* out_utf8,
                             size_t capacity,
                             size_t* out_required) noexcept {
    const size_t required = src.size() + 1;
    if (out_required != nullptr) *out_required = required;
    if (out_utf8 == nullptr || capacity < required) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(out_utf8, src.data(), src.size());
    out_utf8[src.size()] = '\0';
    return SAO_STATUS_OK;
}

// ---------------------------------------------------------------------------
// Cross-process serialisation.  Two SaoAuto processes might race on the
// same runtime -- the named mutex funnels them into strict order.  Name is
// derived from the opaque id so distinct runtimes install in parallel.
// ---------------------------------------------------------------------------
class NamedMutexGuard {
public:
    static constexpr DWORD kDeadlineMs = 5'000;

    NamedMutexGuard(const std::wstring& name) noexcept
        : handle_(::CreateMutexW(nullptr, FALSE, name.c_str())) {
        if (handle_ == nullptr) {
            status_ = SAO_STATUS_ERR_OS_CALL_FAILED;
            return;
        }
#if defined(SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)
        const DWORD deadline_ms = named_mutex_timeout_ms();
#else
        const DWORD deadline_ms = kDeadlineMs;
#endif
        const DWORD wait = ::WaitForSingleObject(handle_, deadline_ms);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
            acquired_ = true;
            status_ = SAO_STATUS_OK;
        } else if (wait == WAIT_TIMEOUT) {
            status_ = SAO_STATUS_ERR_TIMEOUT;
        } else {
            status_ = SAO_STATUS_ERR_OS_CALL_FAILED;
        }
    }
    ~NamedMutexGuard() {
        if (handle_ != nullptr) {
            if (acquired_) ::ReleaseMutex(handle_);
            ::CloseHandle(handle_);
        }
    }
    NamedMutexGuard(const NamedMutexGuard&) = delete;
    NamedMutexGuard& operator=(const NamedMutexGuard&) = delete;
    bool ok() const noexcept { return status_ == SAO_STATUS_OK; }
    sao_status_t status() const noexcept { return status_; }
private:
    HANDLE handle_ = nullptr;
    bool acquired_ = false;
    sao_status_t status_ = SAO_STATUS_ERR_OS_CALL_FAILED;
};

std::wstring mutex_name_for(const std::string& opaque) {
    // The scope prefix keeps the object session-local so a compromised
    // renderer sandbox cannot steal the name.
    return L"Local\\SaoAuto.runtime_installer." + widen(opaque);
}

// ---------------------------------------------------------------------------
// Download sinks.
// ---------------------------------------------------------------------------
class FileHashingSink final : public DownloadSink {
public:
    FileHashingSink(const fs::path& path, bool include_blake3)
        : path_(path),
          stream_(path, std::ios::binary | std::ios::trunc),
          include_blake3_(include_blake3) {}

    sao_status_t init() {
        if (!stream_.is_open()) return SAO_STATUS_ERR_OS_CALL_FAILED;
        return pipeline_.begin(include_blake3_);
    }

    sao_status_t write(const uint8_t* bytes, size_t length) override {
        stream_.write(reinterpret_cast<const char*>(bytes),
                      static_cast<std::streamsize>(length));
        if (!stream_.good()) return SAO_STATUS_ERR_OS_CALL_FAILED;
        return pipeline_.update(bytes, length);
    }

    sao_status_t commit_length(uint64_t total_bytes) override {
        stream_.flush();
        if (!stream_.good()) return SAO_STATUS_ERR_OS_CALL_FAILED;
        total_bytes_ = total_bytes;
        return SAO_STATUS_OK;
    }

    sao_status_t finalize(std::string& sha256_hex,
                          std::string& blake3_hex) {
        stream_.flush();
        if (!stream_.good()) return SAO_STATUS_ERR_OS_CALL_FAILED;
        stream_.close();
        if (stream_.fail()) return SAO_STATUS_ERR_OS_CALL_FAILED;
        return pipeline_.finish(sha256_hex, blake3_hex);
    }

    uint64_t bytes_written() const noexcept { return total_bytes_; }

private:
    fs::path       path_;
    std::ofstream  stream_;
    HashPipeline   pipeline_;
    bool           include_blake3_ = false;
    uint64_t       total_bytes_ = 0;
};

// ---------------------------------------------------------------------------
// Post-download install strategies.
//
// The manifest lists an archive_type + install_hint; here we route each
// combination through the right extraction / rename step.  Extraction
// itself relies on the Shell API (unzip via IShellDispatch) when possible,
// with a plain-copy fallback for RAW payloads.  For NUPKG we treat the file
// as a normal zip and select the runtimes/win-x64 subtree afterwards.
//
// ZIP/NUPKG member names are validated before invoking the system tar.
// ---------------------------------------------------------------------------
uint16_t read_le16_(const uint8_t* bytes) noexcept {
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(bytes[1]) << 8u;
}

uint32_t read_le32_(const uint8_t* bytes) noexcept {
    return static_cast<uint32_t>(bytes[0]) |
           static_cast<uint32_t>(bytes[1]) << 8u |
           static_cast<uint32_t>(bytes[2]) << 16u |
           static_cast<uint32_t>(bytes[3]) << 24u;
}

bool valid_archive_component_(std::wstring_view component) {
    if (component.empty() || component == L"." || component == L".." ||
        component.back() == L'.' || component.back() == L' ') {
        return false;
    }
    std::wstring stem(component.substr(0, component.find(L'.')));
    for (wchar_t& ch : stem) {
        if (ch >= L'a' && ch <= L'z') ch -= (L'a' - L'A');
    }
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" ||
        stem == L"NUL") {
        return false;
    }
    if (stem.size() == 4u &&
        ((stem.rfind(L"COM", 0u) == 0u) ||
         (stem.rfind(L"LPT", 0u) == 0u)) &&
        stem[3] >= L'1' && stem[3] <= L'9') {
        return false;
    }
    return true;
}

bool normalize_archive_name_(const std::string& raw, bool utf8,
                             std::wstring& normalized) {
    if (raw.empty() || raw.size() > 32u * 1024u ||
        raw.front() == '/' || raw.front() == '\\' ||
        raw.find('\0') != std::string::npos ||
        raw.find(':') != std::string::npos) {
        return false;
    }
    std::string portable = raw;
    for (char& ch : portable) {
        if (ch == '\\') ch = '/';
    }
    while (!portable.empty() && portable.back() == '/') portable.pop_back();
    if (portable == ".") {
        normalized = L".";
        return true;
    }
    while (portable.rfind("./", 0u) == 0u) portable.erase(0u, 2u);
    if (portable.empty()) return false;

    const bool ascii = std::all_of(
        portable.begin(), portable.end(),
        [](unsigned char ch) { return ch < 0x80u; });
    if (!utf8 && !ascii) return false;
    const int required = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, portable.data(),
        static_cast<int>(portable.size()), nullptr, 0);
    if (required <= 0) return false;
    normalized.assign(static_cast<size_t>(required), L'\0');
    if (::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, portable.data(),
            static_cast<int>(portable.size()), normalized.data(),
            required) != required) {
        return false;
    }
    size_t begin = 0u;
    while (begin < normalized.size()) {
        const size_t slash = normalized.find(L'/', begin);
        const size_t end = slash == std::wstring::npos
            ? normalized.size()
            : slash;
        if (!valid_archive_component_(
                std::wstring_view(normalized).substr(begin, end - begin))) {
            return false;
        }
        if (slash == std::wstring::npos) break;
        begin = slash + 1u;
    }
    ::CharLowerBuffW(normalized.data(), static_cast<DWORD>(normalized.size()));
    return true;
}

sao_status_t validate_zip_archive_(const fs::path& archive) noexcept {
    try {
        std::ifstream stream(archive, std::ios::binary);
        if (!stream.is_open()) return SAO_STATUS_ERR_NOT_FOUND;
        stream.seekg(0, std::ios::end);
        const auto end_position = stream.tellg();
        if (end_position < 22) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const uint64_t file_size = static_cast<uint64_t>(end_position);
        const size_t tail_size = static_cast<size_t>(
            std::min<uint64_t>(file_size, 65'557u));
        std::vector<uint8_t> tail(tail_size);
        stream.seekg(static_cast<std::streamoff>(file_size - tail_size));
        stream.read(reinterpret_cast<char*>(tail.data()),
                    static_cast<std::streamsize>(tail.size()));
        if (stream.gcount() != static_cast<std::streamsize>(tail.size())) {
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }

        size_t eocd = std::string::npos;
        for (size_t cursor = tail.size() - 22u + 1u; cursor-- > 0u;) {
            if (read_le32_(tail.data() + cursor) != 0x06054b50u) continue;
            const uint16_t comment_length = read_le16_(tail.data() + cursor + 20u);
            if (cursor + 22u + comment_length == tail.size()) {
                eocd = cursor;
                break;
            }
        }
        if (eocd == std::string::npos ||
            read_le16_(tail.data() + eocd + 4u) != 0u ||
            read_le16_(tail.data() + eocd + 6u) != 0u) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        const uint16_t entries_on_disk = read_le16_(tail.data() + eocd + 8u);
        const uint16_t entry_count = read_le16_(tail.data() + eocd + 10u);
        const uint32_t directory_size = read_le32_(tail.data() + eocd + 12u);
        const uint32_t directory_offset = read_le32_(tail.data() + eocd + 16u);
        if (entries_on_disk != entry_count || entry_count == 0xffffu ||
            directory_size == 0xffffffffu ||
            directory_offset == 0xffffffffu || entry_count > 50'000u) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        const uint64_t eocd_offset = file_size - tail_size + eocd;
        const uint64_t directory_end =
            static_cast<uint64_t>(directory_offset) + directory_size;
        if (directory_end > eocd_offset || directory_end < directory_offset) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }

        std::unordered_set<std::wstring> names;
        uint64_t central_cursor = directory_offset;
        for (uint32_t index = 0u; index < entry_count; ++index) {
            if (central_cursor + 46u > directory_end) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            std::array<uint8_t, 46> header{};
            stream.clear();
            stream.seekg(static_cast<std::streamoff>(central_cursor));
            stream.read(reinterpret_cast<char*>(header.data()), header.size());
            if (stream.gcount() != static_cast<std::streamsize>(header.size()) ||
                read_le32_(header.data()) != 0x02014b50u) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            const uint16_t flags = read_le16_(header.data() + 8u);
            const uint32_t compressed_size = read_le32_(header.data() + 20u);
            const uint32_t uncompressed_size = read_le32_(header.data() + 24u);
            const uint16_t name_length = read_le16_(header.data() + 28u);
            const uint16_t extra_length = read_le16_(header.data() + 30u);
            const uint16_t comment_length = read_le16_(header.data() + 32u);
            const uint16_t disk_start = read_le16_(header.data() + 34u);
            const uint32_t external_attributes = read_le32_(header.data() + 38u);
            const uint32_t local_offset = read_le32_(header.data() + 42u);
            const uint64_t next_central = central_cursor + 46u + name_length +
                extra_length + comment_length;
            if ((flags & 0x0001u) != 0u || name_length == 0u ||
                compressed_size == 0xffffffffu ||
                uncompressed_size == 0xffffffffu ||
                local_offset == 0xffffffffu || disk_start != 0u ||
                next_central > directory_end) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            const uint32_t unix_mode = external_attributes >> 16u;
            if ((unix_mode & 0170000u) == 0120000u ||
                (external_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
                ((external_attributes >> 16u) & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            std::string name(name_length, '\0');
            stream.read(name.data(), static_cast<std::streamsize>(name.size()));
            if (stream.gcount() != static_cast<std::streamsize>(name.size())) {
                return SAO_STATUS_ERR_OS_CALL_FAILED;
            }
            std::wstring normalized;
            if (!normalize_archive_name_(name, (flags & 0x0800u) != 0u,
                                         normalized) ||
                !names.insert(normalized).second) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }

            std::array<uint8_t, 30> local{};
            stream.clear();
            stream.seekg(static_cast<std::streamoff>(local_offset));
            stream.read(reinterpret_cast<char*>(local.data()), local.size());
            if (stream.gcount() != static_cast<std::streamsize>(local.size()) ||
                read_le32_(local.data()) != 0x04034b50u) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            const uint16_t local_flags = read_le16_(local.data() + 6u);
            const uint16_t local_name_length = read_le16_(local.data() + 26u);
            const uint16_t local_extra_length = read_le16_(local.data() + 28u);
            if ((local_flags & 0x0001u) != 0u ||
                local_name_length != name_length) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            std::string local_name(local_name_length, '\0');
            stream.read(local_name.data(),
                        static_cast<std::streamsize>(local_name.size()));
            if (stream.gcount() != static_cast<std::streamsize>(local_name.size()) ||
                local_name != name) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            const uint64_t data_offset = static_cast<uint64_t>(local_offset) +
                30u + local_name_length + local_extra_length;
            if (data_offset > file_size ||
                compressed_size > file_size - data_offset) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            central_cursor = next_central;
        }
        return central_cursor == directory_end
            ? SAO_STATUS_OK
            : SAO_STATUS_ERR_INVALID_ARGUMENT;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t run_tar_extract(const fs::path& archive, const fs::path& dest) {
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) return SAO_STATUS_ERR_OS_CALL_FAILED;
    wchar_t system_directory[MAX_PATH]{};
    const UINT system_length = ::GetSystemDirectoryW(
        system_directory, static_cast<UINT>(std::size(system_directory)));
    if (system_length == 0u || system_length >= std::size(system_directory)) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    const fs::path tar_path = fs::path(system_directory) / L"tar.exe";
    const DWORD tar_attributes = ::GetFileAttributesW(tar_path.c_str());
    if (tar_attributes == INVALID_FILE_ATTRIBUTES ||
        (tar_attributes & (FILE_ATTRIBUTE_DIRECTORY |
                           FILE_ATTRIBUTE_REPARSE_POINT)) != 0u) {
        return SAO_STATUS_ERR_CAPABILITY_MISSING;
    }
    std::wstring cmd = L"\"" + tar_path.wstring() + L"\" -xf \"" +
        archive.wstring() + L"\" -C \"" + dest.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) return SAO_STATUS_ERR_OS_CALL_FAILED;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                   &limits, sizeof(limits)) ||
        !::CreateProcessW(tar_path.c_str(), cmd.data(), nullptr, nullptr,
                          FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                          nullptr, &si, &pi)) {
        ::CloseHandle(job);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    if (!::AssignProcessToJobObject(job, pi.hProcess) ||
        ::ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        ::TerminateProcess(pi.hProcess, 1u);
        ::CloseHandle(job);
        ::WaitForSingleObject(pi.hProcess, 5'000u);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
        fs::remove_all(dest, ec);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    const DWORD timeout_ms = tar_timeout_ms();
    const DWORD wait = ::WaitForSingleObject(pi.hProcess, timeout_ms);
    if (wait != WAIT_OBJECT_0) {
        ::TerminateProcess(pi.hProcess, 1u);
        ::CloseHandle(job);
        ::WaitForSingleObject(pi.hProcess, 5'000u);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
        std::error_code cleanup_ec;
        fs::remove_all(dest, cleanup_ec);
        return wait == WAIT_TIMEOUT ? SAO_STATUS_ERR_TIMEOUT
                                    : SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    DWORD exit_code = 1;
    const BOOL exit_status = ::GetExitCodeProcess(pi.hProcess, &exit_code);
    ::CloseHandle(job);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return exit_status != FALSE && exit_code == 0
        ? SAO_STATUS_OK
        : SAO_STATUS_ERR_OS_CALL_FAILED;
}

sao_status_t install_from_archive(sao_runtime_archive_t archive,
                                  sao_runtime_install_hint_t hint,
                                  const fs::path& payload,
                                  const fs::path& staging_root,
                                  fs::path& out_committed_source) {
    std::error_code ec;
    switch (archive) {
        case SAO_RUNTIME_ARCHIVE_RAW: {
            // No extraction -- the payload file itself is the artifact.
            out_committed_source = staging_root / L"payload";
            fs::create_directories(staging_root, ec);
            if (ec) return SAO_STATUS_ERR_OS_CALL_FAILED;
            fs::rename(payload, out_committed_source, ec);
            if (ec) return SAO_STATUS_ERR_OS_CALL_FAILED;
            return SAO_STATUS_OK;
        }
        case SAO_RUNTIME_ARCHIVE_ZIP:
        case SAO_RUNTIME_ARCHIVE_NUPKG: {
            if (const auto s = validate_zip_archive_(payload);
                s != SAO_STATUS_OK) {
                return s;
            }
            const fs::path extracted = staging_root / L"extracted";
            if (const auto s = run_tar_extract(payload, extracted); s != SAO_STATUS_OK) {
                return s;
            }
            if (archive == SAO_RUNTIME_ARCHIVE_NUPKG &&
                hint == SAO_RUNTIME_INSTALL_HINT_NUPKG_RUNTIME_TREE) {
                // Pick runtimes/win-x64/lib subtree if it exists; otherwise
                // fall through to the entire extracted root.
                const fs::path runtime_tree = extracted / L"runtimes" / L"win-x64";
                if (fs::exists(runtime_tree, ec) && !ec) {
                    out_committed_source = runtime_tree;
                    return SAO_STATUS_OK;
                }
            }
            out_committed_source = extracted;
            return SAO_STATUS_OK;
        }
        default:
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
}

// ---------------------------------------------------------------------------
// Test transport handling.
// ---------------------------------------------------------------------------
#if defined(SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)
class MemorySink final : public DownloadSink {
public:
    MemorySink(FileHashingSink& next) noexcept : next_(next) {}
    sao_status_t write(const uint8_t* bytes, size_t length) override {
        return next_.write(bytes, length);
    }
    sao_status_t commit_length(uint64_t total_bytes) override {
        return next_.commit_length(total_bytes);
    }
private:
    FileHashingSink& next_;
};

sao_status_t drive_test_transport(const char* url,
                                  FileHashingSink& sink,
                                  uint64_t max_bytes,
                                  sao_runtime_installer_progress_cb_t progress_cb,
                                  void* user_data) {
    sao_runtime_installer_test_transport_t transport = nullptr;
    void* user = nullptr;
    {
        auto& st = state();
        std::lock_guard<std::mutex> guard(st.mutex);
        transport = st.test_transport;
        user = st.test_transport_user_data;
    }
    if (transport == nullptr) return SAO_STATUS_ERR_CAPABILITY_MISSING;
    const uint8_t* body = nullptr;
    size_t body_length = 0;
    const auto rc = transport(url, &body, &body_length, user);
    if (rc != SAO_STATUS_OK) return rc;
    if (body == nullptr || body_length == 0) return SAO_STATUS_ERR_NET_DOWN;
    if (body_length > max_bytes) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    // Feed the sink in 4 KiB chunks so tests can observe progress ticks.
    const size_t chunk = 4096;
    size_t off = 0;
    while (off < body_length) {
        const size_t take = std::min(chunk, body_length - off);
        if (const auto s = sink.write(body + off, take); s != SAO_STATUS_OK) return s;
        off += take;
        if (progress_cb != nullptr) {
            progress_cb(static_cast<uint64_t>(off),
                        static_cast<uint64_t>(body_length),
                        user_data);
        }
    }
    return sink.commit_length(static_cast<uint64_t>(body_length));
}
#endif  // SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS

// ---------------------------------------------------------------------------
// The install pipeline itself.
// ---------------------------------------------------------------------------
sao_status_t move_path(const fs::path& source,
                       const fs::path& destination,
                       uint32_t rename_failpoint,
                       uint32_t copy_failpoint) {
#if !defined(SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)
    (void)rename_failpoint;
    (void)copy_failpoint;
#endif
    std::error_code ec;
    if (fs::exists(destination, ec)) {
        return SAO_STATUS_ERR_ALREADY_EXISTS;
    }
    if (ec) return SAO_STATUS_ERR_OS_CALL_FAILED;
#if defined(SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)
    const bool force_rename_failure = rename_failpoint != 0 &&
        test_failpoint(rename_failpoint);
#else
    const bool force_rename_failure = false;
#endif
    if (!force_rename_failure) {
        fs::rename(source, destination, ec);
        if (!ec) return SAO_STATUS_OK;
    } else {
        ec = std::make_error_code(std::errc::cross_device_link);
    }

#if defined(SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)
    if (copy_failpoint != 0 && test_failpoint(copy_failpoint)) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
#endif
    ec.clear();
    fs::copy(source, destination,
             fs::copy_options::recursive,
             ec);
    if (ec) {
        std::error_code cleanup_ec;
        fs::remove_all(destination, cleanup_ec);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    ec.clear();
    fs::remove_all(source, ec);
    return ec ? SAO_STATUS_ERR_OS_CALL_FAILED : SAO_STATUS_OK;
}

sao_status_t perform_install(const ManifestEntry& entry,
                             const RuntimePaths& paths,
                             sao_runtime_installer_progress_cb_t progress_cb,
                             void* user_data) {
    std::error_code ec;
    if (const auto status = reject_reparse_if_present_(paths.kind_root);
        status != SAO_STATUS_OK) return status;
    if (const auto status = reject_reparse_if_present_(paths.staging);
        status != SAO_STATUS_OK) return status;
    const fs::path backup_parent = paths.kind_root / L"backup";
    if (const auto status = reject_reparse_if_present_(backup_parent);
        status != SAO_STATUS_OK) return status;
    fs::remove_all(paths.staging, ec);
    if (ec) return SAO_STATUS_ERR_OS_CALL_FAILED;
    fs::create_directories(paths.staging, ec);
    if (ec) return SAO_STATUS_ERR_OS_CALL_FAILED;

    GUID guid{};
    if (FAILED(::CoCreateGuid(&guid))) {
        fs::remove_all(paths.staging, ec);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    wchar_t guid_buf[64] = {0};
    if (::StringFromGUID2(guid, guid_buf, static_cast<int>(std::size(guid_buf))) <= 0) {
        fs::remove_all(paths.staging, ec);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    const std::wstring transaction_id(guid_buf);
    const fs::path session_root = paths.staging / transaction_id;
    fs::create_directories(session_root, ec);
    if (ec) return SAO_STATUS_ERR_OS_CALL_FAILED;

    const fs::path payload_path = session_root / L"payload.bin";
    std::vector<std::string> urls;
    urls.reserve(1 + entry.mirrors.size());
    urls.push_back(entry.url);
    for (const auto& mirror : entry.mirrors) urls.push_back(mirror);

    sao_status_t last_error = SAO_STATUS_ERR_NET_DOWN;
    std::string sha_hex;
    std::string blake_hex;
    bool downloaded = false;
    for (const auto& url : urls) {
        std::error_code sub_ec;
        fs::remove(payload_path, sub_ec);
        FileHashingSink sink(payload_path, !entry.blake3_hex.empty());
        if (const auto s = sink.init(); s != SAO_STATUS_OK) {
            last_error = s;
            continue;
        }

#if defined(SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)
        auto& st = state();
        bool have_hook = false;
        {
            std::lock_guard<std::mutex> guard(st.mutex);
            have_hook = st.test_transport != nullptr;
        }
        const auto transport_status = have_hook
            ? drive_test_transport(url.c_str(), sink, entry.size_bytes, progress_cb, user_data)
            : sao::runtime_installer::internal::stream_download(
                url.c_str(), sink, entry.size_bytes, progress_cb, user_data);
#else
        const auto transport_status = sao::runtime_installer::internal::stream_download(
            url.c_str(), sink, entry.size_bytes, progress_cb, user_data);
#endif
        if (transport_status != SAO_STATUS_OK) {
            last_error = transport_status;
            continue;
        }
        if (sink.bytes_written() != entry.size_bytes) {
            last_error = SAO_STATUS_ERR_INVALID_ARGUMENT;
            continue;
        }
        if (const auto s = sink.finalize(sha_hex, blake_hex); s != SAO_STATUS_OK) {
            last_error = s;
            continue;
        }
        if (sao::runtime_installer::internal::compare_hex_equal(
                sha_hex, entry.sha256_hex) != SAO_STATUS_OK) {
            last_error = SAO_STATUS_ERR_UNKNOWN;
            continue;
        }
        if (!entry.blake3_hex.empty() &&
            sao::runtime_installer::internal::compare_hex_equal(
                blake_hex, entry.blake3_hex) != SAO_STATUS_OK) {
            last_error = SAO_STATUS_ERR_UNKNOWN;
            continue;
        }
        downloaded = true;
        break;
    }
    if (!downloaded) {
        fs::remove_all(paths.staging, ec);
        return last_error;
    }

    fs::path committed_source;
    if (const auto s = install_from_archive(
            entry.archive, entry.install_hint, payload_path, session_root, committed_source);
        s != SAO_STATUS_OK) {
        fs::remove_all(paths.staging, ec);
        return s;
    }

    const fs::path candidate = session_root / L"candidate";
    if (const auto s = move_path(committed_source, candidate, 0, 0); s != SAO_STATUS_OK) {
        fs::remove_all(paths.staging, ec);
        return s;
    }

    InstallMarker candidate_marker;
    candidate_marker.version = entry.version;
    candidate_marker.sha256_hex = entry.sha256_hex;
    candidate_marker.blake3_hex = entry.blake3_hex;
    const bool candidate_is_directory = fs::is_directory(candidate, ec) && !ec;
    if (candidate_is_directory) {
        if (const auto s = sao::runtime_installer::internal::hash_directory_tree_sha256(
                candidate.wstring(), candidate_marker.tree_sha256_hex);
            s != SAO_STATUS_OK) {
            fs::remove_all(paths.staging, ec);
            return s;
        }
    } else if (!fs::is_regular_file(candidate, ec) || ec) {
        fs::remove_all(paths.staging, ec);
        return SAO_STATUS_ERR_UNKNOWN;
    }

    const fs::path candidate_marker_path = session_root / L"candidate.json";
    if (const auto s = write_marker(candidate_marker_path, candidate_marker); s != SAO_STATUS_OK) {
        fs::remove_all(paths.staging, ec);
        return s;
    }

    fs::create_directories(paths.kind_root, ec);
    if (ec) {
        fs::remove_all(paths.staging, ec);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    if (const auto status = reject_reparse_if_present_(paths.kind_root);
        status != SAO_STATUS_OK) {
        return SAO_RUNTIME_INSTALLER_STATUS_ERR_PARTIAL_RECOVERY;
    }
    if (const auto status = reject_reparse_if_present_(backup_parent);
        status != SAO_STATUS_OK) {
        return SAO_RUNTIME_INSTALLER_STATUS_ERR_PARTIAL_RECOVERY;
    }
    const fs::path backup_root = paths.kind_root / L"backup" / transaction_id;
    const fs::path backup_current = backup_root / L"current";
    const fs::path backup_marker = backup_root / L"current.json";
    fs::create_directories(backup_root, ec);
    if (ec) {
        fs::remove_all(paths.staging, ec);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    if (const auto status = reject_reparse_if_present_(backup_parent);
        status != SAO_STATUS_OK) {
        return SAO_RUNTIME_INSTALLER_STATUS_ERR_PARTIAL_RECOVERY;
    }

    const bool had_current = fs::exists(paths.current, ec) && !ec;
    if (ec) {
        fs::remove_all(paths.staging, ec);
        fs::remove_all(backup_root, ec);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    const bool had_marker = fs::exists(paths.marker, ec) && !ec;
    if (ec) {
        fs::remove_all(paths.staging, ec);
        fs::remove_all(backup_root, ec);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    bool old_current_moved = false;
    bool old_marker_moved = false;
    bool current_commit_started = false;
    bool marker_commit_started = false;
    auto rollback = [&](sao_status_t failure) -> sao_status_t {
        bool restored = true;
        std::error_code rollback_ec;
        if (current_commit_started) {
            fs::remove_all(paths.current, rollback_ec);
            if (rollback_ec) restored = false;
        }
        if (marker_commit_started) {
            fs::remove(paths.marker, rollback_ec);
            if (rollback_ec) restored = false;
        }
        if (old_marker_moved) {
            const auto restore_status = move_path(
                backup_marker, paths.marker,
#if defined(SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)
                SAO_RUNTIME_INSTALLER_TEST_FAIL_RESTORE_MARKER,
                SAO_RUNTIME_INSTALLER_TEST_FAIL_RESTORE_MARKER
#else
                0, 0
#endif
            );
            if (restore_status != SAO_STATUS_OK) restored = false;
        }
        if (old_current_moved) {
            const auto restore_status = move_path(
                backup_current, paths.current,
#if defined(SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)
                SAO_RUNTIME_INSTALLER_TEST_FAIL_RESTORE_CURRENT,
                SAO_RUNTIME_INSTALLER_TEST_FAIL_RESTORE_CURRENT
#else
                0, 0
#endif
            );
            if (restore_status != SAO_STATUS_OK) restored = false;
        }
        rollback_ec.clear();
        fs::remove_all(paths.staging, rollback_ec);
        if (rollback_ec) restored = false;
        if (!restored) {
            return SAO_RUNTIME_INSTALLER_STATUS_ERR_PARTIAL_RECOVERY;
        }
        rollback_ec.clear();
        fs::remove_all(backup_root, rollback_ec);
        return rollback_ec
            ? SAO_RUNTIME_INSTALLER_STATUS_ERR_PARTIAL_RECOVERY
            : failure;
    };

    if (had_current) {
        if (const auto s = move_path(
                paths.current, backup_current,
#if defined(SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)
                SAO_RUNTIME_INSTALLER_TEST_FAIL_BACKUP_CURRENT_RENAME, 0
#else
                0, 0
#endif
            ); s != SAO_STATUS_OK) {
            fs::remove_all(paths.staging, ec);
            fs::remove_all(backup_root, ec);
            return s;
        }
        old_current_moved = true;
    }
    if (had_marker) {
        if (const auto s = move_path(
                paths.marker, backup_marker,
#if defined(SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)
                SAO_RUNTIME_INSTALLER_TEST_FAIL_BACKUP_MARKER_RENAME, 0
#else
                0, 0
#endif
            ); s != SAO_STATUS_OK) {
            return rollback(s);
        }
        old_marker_moved = true;
    }

    current_commit_started = true;
    if (const auto s = move_path(
            candidate, paths.current,
#if defined(SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)
            SAO_RUNTIME_INSTALLER_TEST_FAIL_COMMIT_CURRENT_RENAME,
            SAO_RUNTIME_INSTALLER_TEST_FAIL_COMMIT_CURRENT_COPY
#else
            0, 0
#endif
        ); s != SAO_STATUS_OK) {
        return rollback(s);
    }

    marker_commit_started = true;
    if (const auto s = move_path(
            candidate_marker_path, paths.marker,
#if defined(SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)
            SAO_RUNTIME_INSTALLER_TEST_FAIL_COMMIT_MARKER_RENAME,
            SAO_RUNTIME_INSTALLER_TEST_FAIL_COMMIT_MARKER_COPY
#else
            0, 0
#endif
        ); s != SAO_STATUS_OK) {
        return rollback(s);
    }

    ec.clear();
    fs::remove_all(paths.staging, ec);
    if (ec) return SAO_RUNTIME_INSTALLER_STATUS_ERR_PARTIAL_RECOVERY;
    ec.clear();
    fs::remove_all(backup_root, ec);
    return ec ? SAO_RUNTIME_INSTALLER_STATUS_ERR_PARTIAL_RECOVERY
              : SAO_STATUS_OK;
}

sao_status_t verify_integrity_paths(
    const RuntimePaths& paths,
    const std::shared_ptr<const Manifest>& manifest) {
    std::error_code ec;
    if (!fs::exists(paths.marker, ec) || ec ||
        !fs::exists(paths.current, ec) || ec) {
        return ec ? SAO_STATUS_ERR_OS_CALL_FAILED : SAO_STATUS_ERR_NOT_FOUND;
    }

    InstallMarker marker;
    if (const auto s = read_marker(paths.marker, marker); s != SAO_STATUS_OK) return s;
    if (manifest) {
        const auto* entry = sao::runtime_installer::internal::find_entry(*manifest, paths.kind);
        if (entry == nullptr || marker.version != entry->version ||
            sao::runtime_installer::internal::compare_hex_equal(
                marker.sha256_hex, entry->sha256_hex) != SAO_STATUS_OK) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
        if (sao::runtime_installer::internal::compare_hex_equal(
            marker.blake3_hex, entry->blake3_hex) != SAO_STATUS_OK) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }

    const DWORD current_attributes = ::GetFileAttributesW(paths.current.c_str());
    if (current_attributes == INVALID_FILE_ATTRIBUTES ||
        (current_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    if (fs::is_regular_file(paths.current, ec) && !ec) {
        if (!marker.tree_sha256_hex.empty()) return SAO_STATUS_ERR_UNKNOWN;
        std::string file_sha;
        if (const auto s = sao::runtime_installer::internal::hash_file_sha256(
                paths.current.wstring(), file_sha); s != SAO_STATUS_OK) return s;
        return sao::runtime_installer::internal::compare_hex_equal(
                   file_sha, marker.sha256_hex) == SAO_STATUS_OK
            ? SAO_STATUS_OK
            : SAO_STATUS_ERR_UNKNOWN;
    }
    if (!fs::is_directory(paths.current, ec) || ec || marker.tree_sha256_hex.empty()) {
        return SAO_STATUS_ERR_UNKNOWN;
    }

    std::string tree_sha;
    if (const auto s = sao::runtime_installer::internal::hash_directory_tree_sha256(
            paths.current.wstring(), tree_sha); s != SAO_STATUS_OK) return s;
    return sao::runtime_installer::internal::compare_hex_equal(
               tree_sha, marker.tree_sha256_hex) == SAO_STATUS_OK
        ? SAO_STATUS_OK
        : SAO_STATUS_ERR_UNKNOWN;
}

sao_status_t remove_transaction_residue_(const RuntimePaths& paths) {
    const fs::path backup_parent = paths.kind_root / L"backup";
    if (const auto status = reject_reparse_if_present_(paths.staging);
        status != SAO_STATUS_OK) return status;
    if (const auto status = reject_reparse_if_present_(backup_parent);
        status != SAO_STATUS_OK) return status;
    std::error_code ec;
    fs::remove_all(paths.staging, ec);
    if (ec) return SAO_RUNTIME_INSTALLER_STATUS_ERR_PARTIAL_RECOVERY;
    ec.clear();
    fs::remove_all(backup_parent, ec);
    return ec ? SAO_RUNTIME_INSTALLER_STATUS_ERR_PARTIAL_RECOVERY
              : SAO_STATUS_OK;
}

sao_status_t recover_interrupted_install_(const RuntimePaths& paths) {
    if (verify_integrity_paths(paths, nullptr) == SAO_STATUS_OK) {
        return remove_transaction_residue_(paths);
    }

    const fs::path backup_parent = paths.kind_root / L"backup";
    if (const auto status = reject_reparse_if_present_(backup_parent);
        status != SAO_STATUS_OK) return status;
    std::error_code ec;
    if (!fs::exists(backup_parent, ec)) {
        if (ec) return SAO_STATUS_ERR_OS_CALL_FAILED;
        if (const auto status = reject_reparse_if_present_(paths.current);
            status != SAO_STATUS_OK) return status;
        if (const auto status = reject_reparse_if_present_(paths.marker);
            status != SAO_STATUS_OK) return status;
        fs::remove_all(paths.current, ec);
        if (ec) return SAO_RUNTIME_INSTALLER_STATUS_ERR_PARTIAL_RECOVERY;
        ec.clear();
        fs::remove(paths.marker, ec);
        if (ec) return SAO_RUNTIME_INSTALLER_STATUS_ERR_PARTIAL_RECOVERY;
        return remove_transaction_residue_(paths);
    }

    struct BackupCandidate {
        fs::path root;
        fs::file_time_type modified;
    };
    std::vector<BackupCandidate> backups;
    for (fs::directory_iterator it(backup_parent, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec) || ec ||
            reject_reparse_if_present_(it->path()) != SAO_STATUS_OK) {
            return SAO_RUNTIME_INSTALLER_STATUS_ERR_PARTIAL_RECOVERY;
        }
        backups.push_back({it->path(), it->last_write_time(ec)});
        if (ec) return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    if (ec) return SAO_STATUS_ERR_OS_CALL_FAILED;
    std::sort(backups.begin(), backups.end(),
              [](const BackupCandidate& left,
                 const BackupCandidate& right) {
                  return left.modified > right.modified;
              });

    enum class RestoreShape { none, both_backup, backup_current, backup_marker };
    bool meaningful_backup = false;
    for (const auto& backup : backups) {
        const fs::path backup_current = backup.root / L"current";
        const fs::path backup_marker = backup.root / L"current.json";
        const bool has_backup_current = fs::exists(backup_current, ec) && !ec;
        if (ec) return SAO_STATUS_ERR_OS_CALL_FAILED;
        const bool has_backup_marker = fs::exists(backup_marker, ec) && !ec;
        if (ec) return SAO_STATUS_ERR_OS_CALL_FAILED;
        meaningful_backup |= has_backup_current || has_backup_marker;
        if (!has_backup_current && !has_backup_marker) continue;
        RestoreShape shape = RestoreShape::none;
        RuntimePaths candidate = paths;
        candidate.current = backup_current;
        candidate.marker = backup_marker;
        if (verify_integrity_paths(candidate, nullptr) == SAO_STATUS_OK) {
            shape = RestoreShape::both_backup;
        } else {
            candidate.current = backup_current;
            candidate.marker = paths.marker;
            if (verify_integrity_paths(candidate, nullptr) == SAO_STATUS_OK) {
                shape = RestoreShape::backup_current;
            } else {
                candidate.current = paths.current;
                candidate.marker = backup_marker;
                if (verify_integrity_paths(candidate, nullptr) == SAO_STATUS_OK) {
                    shape = RestoreShape::backup_marker;
                }
            }
        }
        if (shape == RestoreShape::none) continue;

        if (const auto status = reject_reparse_if_present_(paths.current);
            status != SAO_STATUS_OK) return status;
        if (const auto status = reject_reparse_if_present_(paths.marker);
            status != SAO_STATUS_OK) return status;
        if (shape != RestoreShape::backup_marker) {
            ec.clear();
            fs::remove_all(paths.current, ec);
            if (ec) return SAO_RUNTIME_INSTALLER_STATUS_ERR_PARTIAL_RECOVERY;
            if (move_path(backup_current, paths.current, 0u, 0u) !=
                SAO_STATUS_OK) {
                return SAO_RUNTIME_INSTALLER_STATUS_ERR_PARTIAL_RECOVERY;
            }
        }
        if (shape != RestoreShape::backup_current) {
            ec.clear();
            fs::remove(paths.marker, ec);
            if (ec) return SAO_RUNTIME_INSTALLER_STATUS_ERR_PARTIAL_RECOVERY;
            if (move_path(backup_marker, paths.marker, 0u, 0u) !=
                SAO_STATUS_OK) {
                return SAO_RUNTIME_INSTALLER_STATUS_ERR_PARTIAL_RECOVERY;
            }
        }
        if (verify_integrity_paths(paths, nullptr) != SAO_STATUS_OK) {
            return SAO_RUNTIME_INSTALLER_STATUS_ERR_PARTIAL_RECOVERY;
        }
        return remove_transaction_residue_(paths);
    }
    if (!meaningful_backup) {
        if (const auto status = reject_reparse_if_present_(paths.current);
            status != SAO_STATUS_OK) return status;
        if (const auto status = reject_reparse_if_present_(paths.marker);
            status != SAO_STATUS_OK) return status;
        ec.clear();
        fs::remove_all(paths.current, ec);
        if (ec) return SAO_RUNTIME_INSTALLER_STATUS_ERR_PARTIAL_RECOVERY;
        ec.clear();
        fs::remove(paths.marker, ec);
        if (ec) return SAO_RUNTIME_INSTALLER_STATUS_ERR_PARTIAL_RECOVERY;
        return remove_transaction_residue_(paths);
    }
    return SAO_RUNTIME_INSTALLER_STATUS_ERR_PARTIAL_RECOVERY;
}

std::shared_ptr<const Manifest> snapshot_bound_manifest() noexcept {
    auto& st = state();
    std::lock_guard<std::mutex> guard(st.mutex);
    return st.bound_manifest;
}

sao_status_t ensure_with_manifest(
    sao_runtime_kind_t kind,
    sao_runtime_installer_progress_cb_t progress_cb,
    void* user_data,
    const std::shared_ptr<const Manifest>& manifest) {
    RuntimePaths paths;
    if (const auto s = build_paths(kind, paths); s != SAO_STATUS_OK) return s;
    NamedMutexGuard guard(mutex_name_for(paths.opaque));
    if (!guard.ok()) return guard.status();

    if (const auto recovery = recover_interrupted_install_(paths);
        recovery != SAO_STATUS_OK) {
        return recovery;
    }

    const auto verification = verify_integrity_paths(paths, manifest);
    if (verification == SAO_STATUS_OK) return SAO_STATUS_OK;
    if (!manifest) return SAO_STATUS_ERR_NOT_INITIALIZED;
    if (verification != SAO_STATUS_ERR_NOT_FOUND &&
        verification != SAO_STATUS_ERR_UNKNOWN) return verification;

    const auto* entry = sao::runtime_installer::internal::find_entry(*manifest, kind);
    if (entry == nullptr) return SAO_STATUS_ERR_NOT_FOUND;
    std::error_code ec;
    fs::create_directories(paths.kind_root, ec);
    if (ec) return SAO_STATUS_ERR_OS_CALL_FAILED;
    return perform_install(*entry, paths, progress_cb, user_data);
}

}  // namespace

// ---------------------------------------------------------------------------
// ABI thunks.
// ---------------------------------------------------------------------------
extern "C" {

SAO_RUNTIME_INSTALLER_API uint32_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_abi_version(void) {
    return SAO_RUNTIME_INSTALLER_ABI_VERSION;
}

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_probe(
    sao_runtime_kind_t kind,
    bool*   out_present,
    char*   out_path_utf8,
    size_t  path_capacity,
    size_t* out_required) {
    if (out_present == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_present = false;
    try {
        RuntimePaths paths;
        if (const auto s = build_paths(kind, paths); s != SAO_STATUS_OK) return s;
        std::error_code ec;
        const bool marker_exists = fs::exists(paths.marker, ec) && !ec;
        const bool tree_exists   = fs::exists(paths.current, ec) && !ec;
        *out_present = marker_exists && tree_exists;
        const std::string path_utf8 = paths.current.string();
        if (out_required != nullptr) *out_required = path_utf8.size() + 1;
        if (out_path_utf8 == nullptr) {
            return path_capacity == 0
                ? SAO_STATUS_OK
                : SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        if (path_capacity < path_utf8.size() + 1) {
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        }
        std::memcpy(out_path_utf8, path_utf8.data(), path_utf8.size());
        out_path_utf8[path_utf8.size()] = '\0';
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_ensure(
    sao_runtime_kind_t                  kind,
    sao_runtime_installer_progress_cb_t progress_cb,
    void*                               user_data) {
    try {
        return ensure_with_manifest(
            kind, progress_cb, user_data, snapshot_bound_manifest());
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_verify_integrity(sao_runtime_kind_t kind) {
    try {
        RuntimePaths paths;
        if (const auto s = build_paths(kind, paths); s != SAO_STATUS_OK) return s;
        NamedMutexGuard guard(mutex_name_for(paths.opaque));
        if (!guard.ok()) return guard.status();
        return verify_integrity_paths(paths, snapshot_bound_manifest());
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// Aggregate ensure -- walks the bound manifest and installs every listed
// runtime kind in turn.  Order is deterministic (Python then .NET then Lua
// then AngelScript) so the compositor progress overlay renders in the same
// sequence every launch.  Returns the first non-OK status observed; still
// runs remaining kinds so the caller can uninstall a partially-installed
// tree at teardown if desired.
namespace {

struct AggregateProgressBridge {
    sao_runtime_installer_progress_cb_ex_t cb = nullptr;
    void* user_data = nullptr;
    const char* opaque_id = nullptr;
};

void SAO_RUNTIME_INSTALLER_CALL aggregate_progress_thunk(
    uint64_t bytes_transferred, uint64_t bytes_total, void* user) {
    auto* bridge = static_cast<AggregateProgressBridge*>(user);
    if (bridge == nullptr || bridge->cb == nullptr) return;
    bridge->cb(bridge->opaque_id, bytes_transferred, bytes_total, bridge->user_data);
}

}  // namespace

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_ensure_all(
    const wchar_t* /*base_dir*/,
    sao_runtime_installer_progress_cb_ex_t progress_cb,
    void* user_data) {
    try {
        const auto manifest = snapshot_bound_manifest();
        if (!manifest) return SAO_STATUS_ERR_NOT_INITIALIZED;
        sao_status_t first_error = SAO_STATUS_OK;
        static const sao_runtime_kind_t kOrder[] = {
            SAO_RUNTIME_KIND_PYTHON3_EMBED,
            SAO_RUNTIME_KIND_DOTNET_RUNTIME,
            SAO_RUNTIME_KIND_LUA54_LIB,
            SAO_RUNTIME_KIND_ANGELSCRIPT_LIB,
        };
        for (const auto kind : kOrder) {
            if (sao::runtime_installer::internal::find_entry(*manifest, kind) == nullptr) {
                continue;
            }
            const std::string opaque = opaque_id_for(kind);
            AggregateProgressBridge bridge{progress_cb, user_data, opaque.c_str()};
            const auto rc = ensure_with_manifest(
                kind,
                progress_cb != nullptr ? aggregate_progress_thunk : nullptr,
                progress_cb != nullptr ? &bridge : nullptr,
                manifest);
            if (rc != SAO_STATUS_OK && first_error == SAO_STATUS_OK) first_error = rc;
        }
        return first_error;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_uninstall(sao_runtime_kind_t kind) {
    try {
        RuntimePaths paths;
        if (const auto s = build_paths(kind, paths); s != SAO_STATUS_OK) return s;
        NamedMutexGuard guard(mutex_name_for(paths.opaque));
        if (!guard.ok()) return guard.status();
        if (const auto status = reject_reparse_if_present_(paths.kind_root);
            status != SAO_STATUS_OK) return status;
        std::error_code ec;
        fs::remove_all(paths.kind_root, ec);
        return ec ? SAO_STATUS_ERR_OS_CALL_FAILED : SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_load_manifest(
    const char* manifest_json_utf8,
    size_t manifest_json_length,
    sao_runtime_manifest_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    try {
        auto manifest = std::make_shared<Manifest>();
        if (const auto s = sao::runtime_installer::internal::parse_manifest(
                manifest_json_utf8, manifest_json_length, *manifest); s != SAO_STATUS_OK) {
            return s;
        }
        auto wrapper = std::make_unique<sao_runtime_manifest_s>();
        wrapper->manifest = manifest;
        const auto handle = wrapper.get();
        if (const auto s = sao::runtime_installer::internal::register_manifest_handle(
                handle, manifest); s != SAO_STATUS_OK) {
            return s;
        }
        wrapper.release();
        *out_handle = handle;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

SAO_RUNTIME_INSTALLER_API void SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_manifest_release(sao_runtime_manifest_handle_t handle) {
    sao::runtime_installer::internal::release_manifest_handle(handle);
}

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_bind_manifest(sao_runtime_manifest_handle_t handle) {
    try {
        auto& st = state();
        std::lock_guard<std::mutex> guard(st.mutex);
        if (handle == nullptr) {
            st.bound_manifest.reset();
            return SAO_STATUS_OK;
        }
        const auto it = st.handles.find(handle);
        if (it == st.handles.end()) return SAO_STATUS_ERR_HANDLE_INVALID;
        st.bound_manifest = it->second;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_manifest_has_kind(
    sao_runtime_manifest_handle_t handle,
    sao_runtime_kind_t            kind,
    bool*                         out_present) {
    if (out_present == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_present = false;
    const auto manifest = sao::runtime_installer::internal::snapshot_manifest_handle(handle);
    if (!manifest) return SAO_STATUS_ERR_HANDLE_INVALID;
    *out_present = sao::runtime_installer::internal::find_entry(*manifest, kind) != nullptr;
    return SAO_STATUS_OK;
}

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_kind_opaque_id(
    sao_runtime_kind_t kind,
    char*   out_utf8,
    size_t  capacity,
    size_t* out_required) {
    const std::string id = opaque_id_for(kind);
    if (id.empty()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    return copy_string_out(id, out_utf8, capacity, out_required);
}

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_manifest_version(
    sao_runtime_manifest_handle_t handle,
    char*   out_utf8,
    size_t  capacity,
    size_t* out_required) {
    const auto manifest = sao::runtime_installer::internal::snapshot_manifest_handle(handle);
    if (!manifest) return SAO_STATUS_ERR_HANDLE_INVALID;
    return copy_string_out(manifest->version, out_utf8, capacity, out_required);
}

#if defined(SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)
SAO_RUNTIME_INSTALLER_API void SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_test_set_transport(
    sao_runtime_installer_test_transport_t transport,
    void*                                  user_data) {
    auto& st = state();
    std::lock_guard<std::mutex> guard(st.mutex);
    st.test_transport = transport;
    st.test_transport_user_data = user_data;
}

SAO_RUNTIME_INSTALLER_API void SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_test_set_root(const char* utf8_path_or_null) {
    auto& st = state();
    std::lock_guard<std::mutex> guard(st.mutex);
    if (utf8_path_or_null == nullptr) {
        st.test_root_override.clear();
    } else {
        st.test_root_override.assign(utf8_path_or_null);
    }
}
SAO_RUNTIME_INSTALLER_API void SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_test_set_failpoints(uint32_t failpoints) {
    auto& st = state();
    std::lock_guard<std::mutex> guard(st.mutex);
    st.failpoints = failpoints;
}

SAO_RUNTIME_INSTALLER_API void SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_test_set_tar_timeout_ms(uint32_t timeout_ms) {
    auto& st = state();
    std::lock_guard<std::mutex> guard(st.mutex);
    st.tar_timeout_ms = timeout_ms;
}

SAO_RUNTIME_INSTALLER_API void SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_test_set_named_mutex_timeout_ms(uint32_t timeout_ms) {
    auto& st = state();
    std::lock_guard<std::mutex> guard(st.mutex);
    st.named_mutex_timeout_ms = timeout_ms;
}

#endif  // SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS

}  // extern "C"
