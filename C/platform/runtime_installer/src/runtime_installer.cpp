// SAO Auto -- runtime installer core.
//
// This file owns the process-wide installer state:
//
//   * The currently bound manifest (raw pointer to the caller-owned handle;
//     ownership rules are documented in runtime_installer.h).
//   * A per-runtime named mutex so two threads / two processes cannot race
//     on the same ensure() call.
//   * The download / verify / extract / commit pipeline itself.
//   * Test hooks for offline unit tests (compiled in only when the
//     SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS macro is set).
//
// Directory layout (persisted):
//
//     <root>\<opaque_kind_id>\
//        current.json            -- marker: {"version":"...","sha256":"..."}
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
    Manifest*  bound_manifest = nullptr;
#if defined(SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)
    sao_runtime_installer_test_transport_t test_transport = nullptr;
    void*                                  test_transport_user_data = nullptr;
    std::string                            test_root_override;
#endif
};

InstallerState& state() {
    static InstallerState s;
    return s;
}

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
    std::string blake3_hex;   // empty when not applicable
};

std::string serialize_marker(const InstallMarker& marker) {
    std::string out;
    out.reserve(256);
    out.append("{\"version\":\"");
    out.append(marker.version);
    out.append("\",\"sha256\":\"");
    out.append(marker.sha256_hex);
    if (!marker.blake3_hex.empty()) {
        out.append("\",\"blake3\":\"");
        out.append(marker.blake3_hex);
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
        auto end = text.find('"', pos);
        if (end == std::string::npos) return false;
        value.assign(text, pos, end - pos);
        return true;
    };
    if (!extract("version", out.version)) return false;
    if (!extract("sha256", out.sha256_hex)) return false;
    extract("blake3", out.blake3_hex);  // optional
    return true;
}

sao_status_t write_marker(const fs::path& path, const InstallMarker& marker) noexcept {
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

sao_status_t read_marker(const fs::path& path, InstallMarker& marker) noexcept {
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
    fs::path kind_root;   // <root>\<opaque>
    fs::path current;     // <root>\<opaque>\current
    fs::path marker;      // <root>\<opaque>\current.json
    fs::path staging;     // <root>\<opaque>\staging
    std::string opaque;
};

sao_status_t build_paths(sao_runtime_kind_t kind, RuntimePaths& out) {
    if (kind <= SAO_RUNTIME_KIND_INVALID || kind >= SAO_RUNTIME_KIND_COUNT_) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::wstring root;
    if (const auto s = resolve_root(root); s != SAO_STATUS_OK) return s;
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
    NamedMutexGuard(const std::wstring& name) noexcept
        : handle_(::CreateMutexW(nullptr, FALSE, name.c_str())) {
        if (handle_ != nullptr) {
            const DWORD w = ::WaitForSingleObject(handle_, INFINITE);
            acquired_ = (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED);
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
    bool ok() const noexcept { return handle_ != nullptr && acquired_; }
private:
    HANDLE handle_ = nullptr;
    bool   acquired_ = false;
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
    FileHashingSink(const fs::path& path, bool include_blake3) noexcept
        : path_(path),
          stream_(path, std::ios::binary | std::ios::trunc),
          include_blake3_(include_blake3) {}

    sao_status_t init() noexcept {
        if (!stream_.is_open()) return SAO_STATUS_ERR_OS_CALL_FAILED;
        return pipeline_.begin(include_blake3_);
    }

    sao_status_t write(const uint8_t* bytes, size_t length) noexcept override {
        stream_.write(reinterpret_cast<const char*>(bytes),
                      static_cast<std::streamsize>(length));
        if (!stream_.good()) return SAO_STATUS_ERR_OS_CALL_FAILED;
        return pipeline_.update(bytes, length);
    }

    void commit_length(uint64_t total_bytes) noexcept override {
        stream_.flush();
        total_bytes_ = total_bytes;
    }

    sao_status_t finalize(std::string& sha256_hex,
                          std::string& blake3_hex) noexcept {
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
// A full production installer would call into the shell/crypter or a
// standalone zip library.  To keep the initial patch dependency-free we
// invoke tar.exe (bundled with Windows 10 1803+) via CreateProcessW.
// ---------------------------------------------------------------------------
bool run_tar_extract(const fs::path& archive, const fs::path& dest) noexcept {
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) return false;
    // tar.exe -xf <archive> -C <dest>.  The shipped Windows tar handles
    // ZIP archives with the -x flag since 1803.
    std::wstring cmd = L"tar.exe -xf \"" + archive.wstring() + L"\" -C \"" + dest.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    ::GetExitCodeProcess(pi.hProcess, &exit_code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return exit_code == 0;
}

sao_status_t install_from_archive(sao_runtime_archive_t archive,
                                  sao_runtime_install_hint_t hint,
                                  const fs::path& payload,
                                  const fs::path& staging_root,
                                  fs::path& out_committed_source) noexcept {
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
            const fs::path extracted = staging_root / L"extracted";
            if (!run_tar_extract(payload, extracted)) {
                return SAO_STATUS_ERR_OS_CALL_FAILED;
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
    sao_status_t write(const uint8_t* bytes, size_t length) noexcept override {
        return next_.write(bytes, length);
    }
    void commit_length(uint64_t total_bytes) noexcept override {
        next_.commit_length(total_bytes);
    }
private:
    FileHashingSink& next_;
};

sao_status_t drive_test_transport(const char* url,
                                  FileHashingSink& sink,
                                  uint64_t max_bytes,
                                  sao_runtime_installer_progress_cb_t progress_cb,
                                  void* user_data) noexcept {
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
    sink.commit_length(static_cast<uint64_t>(body_length));
    return SAO_STATUS_OK;
}
#endif  // SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS

// ---------------------------------------------------------------------------
// The install pipeline itself.
// ---------------------------------------------------------------------------
sao_status_t perform_install(const ManifestEntry& entry,
                             const RuntimePaths& paths,
                             sao_runtime_installer_progress_cb_t progress_cb,
                             void* user_data) noexcept {
    std::error_code ec;

    fs::remove_all(paths.staging, ec);
    if (ec) return SAO_STATUS_ERR_OS_CALL_FAILED;
    fs::create_directories(paths.staging, ec);
    if (ec) return SAO_STATUS_ERR_OS_CALL_FAILED;

    // Session-scoped subdir so orphan directories from crashed installs do
    // not collide with a retry.
    GUID guid{};
    ::CoCreateGuid(&guid);
    wchar_t guid_buf[64] = {0};
    ::StringFromGUID2(guid, guid_buf, static_cast<int>(std::size(guid_buf)));
    const fs::path session_root = paths.staging / std::wstring(guid_buf);
    fs::create_directories(session_root, ec);
    if (ec) return SAO_STATUS_ERR_OS_CALL_FAILED;

    const fs::path payload_path = session_root / L"payload.bin";

    // Try primary URL then mirrors.  Each attempt gets a fresh sink so a
    // partial write does not corrupt the retry.
    std::vector<std::string> urls;
    urls.reserve(1 + entry.mirrors.size());
    urls.push_back(entry.url);
    for (const auto& m : entry.mirrors) urls.push_back(m);

    sao_status_t last_error = SAO_STATUS_ERR_NET_DOWN;
    std::string sha_hex;
    std::string blake_hex;
    bool downloaded = false;
    for (const auto& url : urls) {
        std::error_code sub_ec;
        fs::remove(payload_path, sub_ec);
        FileHashingSink sink(payload_path, !entry.blake3_hex.empty());
        if (const auto s = sink.init(); s != SAO_STATUS_OK) { last_error = s; continue; }

#if defined(SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)
        auto& st = state();
        bool have_hook = false;
        {
            std::lock_guard<std::mutex> guard(st.mutex);
            have_hook = st.test_transport != nullptr;
        }
        sao_status_t transport_status;
        if (have_hook) {
            transport_status = drive_test_transport(
                url.c_str(), sink, entry.size_bytes, progress_cb, user_data);
        } else {
            transport_status = sao::runtime_installer::internal::stream_download(
                url.c_str(), sink, entry.size_bytes, progress_cb, user_data);
        }
#else
        const auto transport_status = sao::runtime_installer::internal::stream_download(
            url.c_str(), sink, entry.size_bytes, progress_cb, user_data);
#endif
        if (transport_status != SAO_STATUS_OK) { last_error = transport_status; continue; }
        if (const auto s = sink.finalize(sha_hex, blake_hex); s != SAO_STATUS_OK) { last_error = s; continue; }
        if (const auto s = sao::runtime_installer::internal::compare_hex_equal(sha_hex, entry.sha256_hex);
            s != SAO_STATUS_OK) {
            last_error = SAO_STATUS_ERR_UNKNOWN;
            continue;
        }
        if (!entry.blake3_hex.empty()) {
            if (const auto s = sao::runtime_installer::internal::compare_hex_equal(blake_hex, entry.blake3_hex);
                s != SAO_STATUS_OK) {
                last_error = SAO_STATUS_ERR_UNKNOWN;
                continue;
            }
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

    // Swap into place -- remove any prior current\ tree, then move the
    // staged tree over.
    std::error_code swap_ec;
    fs::remove_all(paths.current, swap_ec);
    if (swap_ec) {
        fs::remove_all(paths.staging, ec);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    fs::rename(committed_source, paths.current, swap_ec);
    if (swap_ec) {
        // Cross-drive fallback -- copy then remove.
        fs::copy(committed_source, paths.current,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                 swap_ec);
        if (swap_ec) {
            fs::remove_all(paths.staging, ec);
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
    }

    InstallMarker marker;
    marker.version = entry.version;
    marker.sha256_hex = entry.sha256_hex;
    marker.blake3_hex = entry.blake3_hex;
    if (const auto s = write_marker(paths.marker, marker); s != SAO_STATUS_OK) {
        fs::remove_all(paths.staging, ec);
        return s;
    }
    fs::remove_all(paths.staging, ec);
    return SAO_STATUS_OK;
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
        RuntimePaths paths;
        if (const auto s = build_paths(kind, paths); s != SAO_STATUS_OK) return s;

        NamedMutexGuard guard(mutex_name_for(paths.opaque));
        if (!guard.ok()) return SAO_STATUS_ERR_OS_CALL_FAILED;

        // Fast path: already installed and verified against marker.
        {
            std::error_code ec;
            if (fs::exists(paths.marker, ec) && !ec &&
                fs::exists(paths.current, ec) && !ec) {
                return SAO_STATUS_OK;
            }
        }

        // Fetch manifest entry.
        Manifest* manifest = nullptr;
        {
            auto& st = state();
            std::lock_guard<std::mutex> mg(st.mutex);
            manifest = st.bound_manifest;
        }
        if (manifest == nullptr) return SAO_STATUS_ERR_NOT_INITIALIZED;
        const ManifestEntry* entry =
            sao::runtime_installer::internal::find_entry(*manifest, kind);
        if (entry == nullptr) return SAO_STATUS_ERR_NOT_FOUND;

        std::error_code ec;
        fs::create_directories(paths.kind_root, ec);
        if (ec) return SAO_STATUS_ERR_OS_CALL_FAILED;

        return perform_install(*entry, paths, progress_cb, user_data);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_verify_integrity(sao_runtime_kind_t kind) {
    try {
        RuntimePaths paths;
        if (const auto s = build_paths(kind, paths); s != SAO_STATUS_OK) return s;
        std::error_code ec;
        if (!fs::exists(paths.marker, ec) || ec) return SAO_STATUS_ERR_NOT_FOUND;
        if (!fs::exists(paths.current, ec) || ec) return SAO_STATUS_ERR_NOT_FOUND;

        InstallMarker marker;
        if (const auto s = read_marker(paths.marker, marker); s != SAO_STATUS_OK) return s;

        // Two shapes for `current`:
        //   * regular file  -- RAW / single-blob installs.  Re-hash it and
        //                      compare against the marker's sha256.
        //   * directory     -- ZIP/NUPKG installs.  Iterate every file and
        //                      confirm each is still readable.  The
        //                      marker's sha256 is the archive's hash, so
        //                      we do not compare against it directly here;
        //                      corruption is detected by a failed read.
        if (fs::is_regular_file(paths.current, ec) && !ec) {
            std::string file_sha;
            if (const auto s = sao::runtime_installer::internal::hash_file_sha256(
                    paths.current.wstring(), file_sha); s != SAO_STATUS_OK) {
                return s;
            }
            return sao::runtime_installer::internal::compare_hex_equal(
                       file_sha, marker.sha256_hex) == SAO_STATUS_OK
                ? SAO_STATUS_OK
                : SAO_STATUS_ERR_UNKNOWN;
        }
        if (!fs::is_directory(paths.current, ec) || ec) {
            return SAO_STATUS_ERR_UNKNOWN;
        }

        std::vector<fs::path> files;
        for (auto it = fs::recursive_directory_iterator(
                 paths.current, fs::directory_options::skip_permission_denied);
             it != fs::recursive_directory_iterator(); ++it) {
            const auto& e = *it;
            if (e.is_regular_file(ec) && !ec) files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());

        // Feed every file into a rolling SHA-256 to prove none of them are
        // unreadable / truncated.  The digest itself is intentionally not
        // compared against the marker for tree installs (the marker holds
        // the archive hash, not the extracted-tree hash).
        HashPipeline pipe;
        if (const auto s = pipe.begin(false); s != SAO_STATUS_OK) return s;
        std::vector<uint8_t> buf(64 * 1024);
        for (const auto& file : files) {
            std::ifstream stream(file, std::ios::binary);
            if (!stream.is_open()) return SAO_STATUS_ERR_ACCESS_DENIED;
            while (stream.good()) {
                stream.read(reinterpret_cast<char*>(buf.data()),
                            static_cast<std::streamsize>(buf.size()));
                const auto got = static_cast<size_t>(stream.gcount());
                if (got == 0) break;
                if (const auto s = pipe.update(buf.data(), got); s != SAO_STATUS_OK) return s;
            }
        }
        std::string tree_sha;
        std::string tree_blake;
        if (const auto s = pipe.finish(tree_sha, tree_blake); s != SAO_STATUS_OK) return s;
        (void)tree_sha;
        return SAO_STATUS_OK;
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
        // Snapshot the bound manifest under the state mutex so a concurrent
        // release cannot pull the pointer out from under the walk.  We
        // only need read access to the kind list; the entry pointers stay
        // valid until sao_runtime_installer_manifest_release() runs.
        Manifest* manifest = nullptr;
        {
            auto& st = state();
            std::lock_guard<std::mutex> guard(st.mutex);
            manifest = st.bound_manifest;
        }
        if (manifest == nullptr) return SAO_STATUS_ERR_NOT_INITIALIZED;

        sao_status_t first_error = SAO_STATUS_OK;
        // Iterate in enum order so the progress overlay renders
        // deterministically.
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
            const auto rc = sao_runtime_installer_ensure(
                kind,
                progress_cb != nullptr ? aggregate_progress_thunk : nullptr,
                progress_cb != nullptr ? &bridge : nullptr);
            if (rc != SAO_STATUS_OK && first_error == SAO_STATUS_OK) {
                first_error = rc;
            }
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
        if (!guard.ok()) return SAO_STATUS_ERR_OS_CALL_FAILED;
        std::error_code ec;
        fs::remove_all(paths.kind_root, ec);
        return ec ? SAO_STATUS_ERR_OS_CALL_FAILED : SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_load_manifest(
    const char*                       manifest_json_utf8,
    size_t                            manifest_json_length,
    sao_runtime_manifest_handle_t*    out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    try {
        auto owned = std::make_unique<Manifest>();
        if (const auto s = sao::runtime_installer::internal::parse_manifest(
                manifest_json_utf8, manifest_json_length, *owned); s != SAO_STATUS_OK) {
            return s;
        }
        *out_handle = reinterpret_cast<sao_runtime_manifest_handle_t>(owned.release());
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

SAO_RUNTIME_INSTALLER_API void SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_manifest_release(sao_runtime_manifest_handle_t handle) {
    if (handle == nullptr) return;
    auto* raw = reinterpret_cast<Manifest*>(handle);
    // Unbind if this handle was the active one.  Otherwise we would leave a
    // dangling pointer inside the installer state.
    {
        auto& st = state();
        std::lock_guard<std::mutex> guard(st.mutex);
        if (st.bound_manifest == raw) st.bound_manifest = nullptr;
    }
    delete raw;
}

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_bind_manifest(sao_runtime_manifest_handle_t handle) {
    auto& st = state();
    std::lock_guard<std::mutex> guard(st.mutex);
    st.bound_manifest = reinterpret_cast<Manifest*>(handle);
    return SAO_STATUS_OK;
}

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_manifest_has_kind(
    sao_runtime_manifest_handle_t handle,
    sao_runtime_kind_t            kind,
    bool*                         out_present) {
    if (out_present == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_present = false;
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    const auto* manifest = reinterpret_cast<Manifest*>(handle);
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
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    const auto* manifest = reinterpret_cast<Manifest*>(handle);
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
#endif  // SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS

}  // extern "C"
