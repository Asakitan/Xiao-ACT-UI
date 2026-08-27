#include "sao/launcher/auto_update.h"

#include "sao/server/freetier/updater/updater.h"
#include "sao_core/sao_status.h"

#include <process.h>
#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sao::launcher {
namespace {

namespace fs = std::filesystem;

#ifndef SAO_LAUNCHER_VERSION
#define SAO_LAUNCHER_VERSION "0.0.0"
#endif

constexpr std::size_t kManifestVersionCapacity = 64u;
constexpr std::size_t kManifestUrlCapacity = 2048u;
constexpr std::size_t kManifestSha256Capacity = 65u;
constexpr std::uint64_t kMaximumUpdateBytes = 512ull * 1024ull * 1024ull;
constexpr DWORD kHelperReadyTimeoutMs = 5u * 60u * 1000u;
constexpr DWORD kHelperTerminationTimeoutMs = 2u * 1000u;
constexpr std::string_view kNativeUpdateManifestUrl =
    "https://x2.sjcmc.cn:15018/update/stable/windows-x64-native/latest.json";

struct WorkerContext {
    UpdateProviderConfiguration configuration;
    std::wstring base_dir;
    std::wstring exe_path;
    DWORD launcher_thread_id = 0;
    HANDLE cancel_event = nullptr;
};

bool is_cancelled(HANDLE cancel_event) noexcept {
    return cancel_event != nullptr && WaitForSingleObject(cancel_event, 0u) == WAIT_OBJECT_0;
}

bool equal_ascii_ignore_case(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const unsigned char a = static_cast<unsigned char>(left[index]);
        const unsigned char b = static_cast<unsigned char>(right[index]);
        if (std::tolower(a) != std::tolower(b))
            return false;
    }
    return true;
}

bool is_sha256(std::string_view value) noexcept {
    if (value.size() != kManifestSha256Capacity - 1u)
        return false;
    for (const unsigned char character : value) {
        if (!std::isxdigit(character))
            return false;
    }
    return true;
}

std::size_t bounded_length(const char* value, std::size_t capacity) noexcept {
    if (value == nullptr)
        return 0u;
    std::size_t length = 0u;
    while (length < capacity && value[length] != '\0')
        ++length;
    return length;
}

std::string_view url_path_without_query(std::string_view url) noexcept {
    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos)
        return {};
    const std::size_t authority_end = url.find('/', scheme_end + 3u);
    const std::size_t path_start =
        authority_end == std::string_view::npos ? url.size() : authority_end;
    const std::size_t query = url.find('?', path_start);
    const std::size_t path_end = query == std::string_view::npos ? url.size() : query;
    return url.substr(path_start, path_end - path_start);
}

bool is_exact_json_url(std::string_view url) noexcept {
    const std::string_view path = url_path_without_query(url);
    return path.size() >= 5u && equal_ascii_ignore_case(path.substr(path.size() - 5u), ".json");
}

std::string_view url_origin(std::string_view url) noexcept {
    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos)
        return {};
    const std::size_t authority_end = url.find_first_of("/?", scheme_end + 3u);
    return url.substr(0, authority_end);
}

bool same_origin(std::string_view left, std::string_view right) noexcept {
    const std::string_view left_origin = url_origin(left);
    const std::string_view right_origin = url_origin(right);
    return !left_origin.empty() && !right_origin.empty() &&
           equal_ascii_ignore_case(left_origin, right_origin);
}

fs::path user_staging_root() {
    PWSTR known_folder = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known_folder)) &&
        known_folder != nullptr) {
        const fs::path root(known_folder);
        CoTaskMemFree(known_folder);
        return root / L"SaoAuto" / L"update-staging";
    }

    std::vector<wchar_t> temporary(512u);
    for (;;) {
        const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        if (length == 0u)
            return {};
        if (length < temporary.size() - 1u)
            return fs::path(temporary.data()) / L"SaoAuto" / L"update-staging";
        if (temporary.size() >= 32768u)
            return {};
        temporary.resize(temporary.size() * 2u);
    }
}

constexpr ULONGLONG kStagingOrphanAge100ns = 24ull * 60ull * 60ull * 10000000ull;
constexpr wchar_t kStagingPreservationMarker[] = L".sao-preserved";

bool path_is_absent(const fs::path& path) noexcept {
    if (GetFileAttributesW(path.wstring().c_str()) != INVALID_FILE_ATTRIBUTES)
        return false;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool parse_staging_owner_pid(const fs::path& staging, DWORD& pid_out) noexcept {
    const std::wstring name = staging.filename().wstring();
    if (name.rfind(L"run-", 0u) != 0u)
        return false;
    const std::size_t separator = name.find(L'-', 4u);
    if (separator == std::wstring::npos || separator == 4u ||
        separator + 1u >= name.size()) {
        return false;
    }

    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(name.c_str() + 4u, &end, 10);
    if (parsed == 0u || parsed > MAXDWORD ||
        end != name.c_str() + separator) {
        return false;
    }
    for (std::size_t index = separator + 1u; index < name.size(); ++index) {
        if (name[index] < L'0' || name[index] > L'9')
            return false;
    }
    pid_out = static_cast<DWORD>(parsed);
    return true;
}

bool owner_process_is_gone(DWORD pid) noexcept {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (process == nullptr)
        return GetLastError() == ERROR_INVALID_PARAMETER;
    const DWORD wait_result = WaitForSingleObject(process, 0u);
    (void)CloseHandle(process);
    return wait_result == WAIT_OBJECT_0;
}

bool staging_is_old_enough(const fs::path& staging) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(staging.wstring().c_str(), GetFileExInfoStandard,
                              &attributes) ||
        (attributes.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
            FILE_ATTRIBUTE_DIRECTORY) {
        return false;
    }

    FILETIME now_file_time{};
    GetSystemTimeAsFileTime(&now_file_time);
    ULARGE_INTEGER now{};
    now.LowPart = now_file_time.dwLowDateTime;
    now.HighPart = now_file_time.dwHighDateTime;
    ULARGE_INTEGER written{};
    written.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
    written.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
    return now.QuadPart >= written.QuadPart &&
           now.QuadPart - written.QuadPart >= kStagingOrphanAge100ns;
}

bool staging_tree_is_safe(const fs::path& staging) noexcept {
    try {
        std::error_code error;
        fs::recursive_directory_iterator iterator(
            staging, fs::directory_options::none, error);
        if (error)
            return false;
        for (const fs::recursive_directory_iterator end{};
             iterator != end; iterator.increment(error)) {
            if (error)
                return false;
            const DWORD attributes =
                GetFileAttributesW(iterator->path().wstring().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
                return false;
            }
        }
        return !error;
    } catch (...) {
        return false;
    }
}

bool staging_is_safe_orphan(const fs::path& staging) noexcept {
    DWORD owner_pid = 0u;
    if (!parse_staging_owner_pid(staging, owner_pid) ||
        !staging_is_old_enough(staging) || !owner_process_is_gone(owner_pid) ||
        !path_is_absent(staging / kStagingPreservationMarker) ||
        !path_is_absent(staging / L"helper.ready") ||
        !staging_tree_is_safe(staging)) {
        return false;
    }
    return true;
}

void prune_staging_directories() noexcept {
    try {
        const fs::path root = user_staging_root();
        if (root.empty())
            return;
        const DWORD root_attributes = GetFileAttributesW(root.wstring().c_str());
        if (root_attributes == INVALID_FILE_ATTRIBUTES ||
            (root_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
            (root_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
            return;
        }

        std::error_code error;
        for (fs::directory_iterator iterator(root, error); iterator != fs::directory_iterator();
             iterator.increment(error)) {
            if (error)
                return;
            const DWORD attributes = GetFileAttributesW(iterator->path().wstring().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
                (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u ||
                !staging_is_safe_orphan(iterator->path())) {
                continue;
            }
            std::error_code ignored;
            fs::remove_all(iterator->path(), ignored);
        }
    } catch (...) {
    }
}

fs::path create_staging_directory() {
    const fs::path root = user_staging_root();
    if (root.empty())
        return {};
    std::error_code error;
    fs::create_directories(root, error);
    if (error)
        return {};

    const fs::path staging = root / (L"run-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                     std::to_wstring(GetTickCount64()));
    fs::create_directories(staging, error);
    return error ? fs::path{} : staging;
}

bool write_staging_preservation_marker(const fs::path& staging) noexcept {
    const fs::path marker = staging / kStagingPreservationMarker;
    HANDLE handle = CreateFileW(marker.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS;
    }
    static constexpr char marker_text[] = "preserved\n";
    DWORD written = 0u;
    const bool wrote = WriteFile(handle, marker_text,
                                 static_cast<DWORD>(sizeof(marker_text) - 1u),
                                 &written, nullptr) != FALSE &&
                       written == sizeof(marker_text) - 1u &&
                       FlushFileBuffers(handle) != FALSE;
    const bool closed = CloseHandle(handle) != FALSE;
    if (!wrote || !closed)
        (void)DeleteFileW(marker.wstring().c_str());
    return wrote && closed;
}

class StagingCleanup {
  public:
    explicit StagingCleanup(fs::path path) : path_(std::move(path)) {}
    ~StagingCleanup() {
        if (!preserve_) {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
        }
    }
    void preserve() noexcept {
        preserve_ = true;
        (void)write_staging_preservation_marker(path_);
    }

  private:
    fs::path path_;
    bool preserve_ = false;
};

std::wstring quote_argument(std::wstring_view value) {
    std::wstring result(1u, L'"');
    std::size_t backslashes = 0u;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            result.append(backslashes * 2u + 1u, L'\\');
            result.push_back(character);
            backslashes = 0u;
        } else {
            result.append(backslashes, L'\\');
            result.push_back(character);
            backslashes = 0u;
        }
    }
    result.append(backslashes * 2u, L'\\');
    result.push_back(L'"');
    return result;
}

bool regular_ready_marker(const fs::path& marker) noexcept {
    const DWORD attributes = GetFileAttributesW(marker.wstring().c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0u &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u;
}

bool terminate_helper(const PROCESS_INFORMATION& process, HANDLE job) noexcept {
    if (process.hProcess == nullptr)
        return false;

    DWORD wait_result = WaitForSingleObject(process.hProcess, 0u);
    if (wait_result == WAIT_OBJECT_0)
        return true;
    if (wait_result == WAIT_FAILED)
        return false;

    (void)TerminateProcess(process.hProcess, 1u);
    if (job != nullptr)
        (void)TerminateJobObject(job, 1u);

    const ULONGLONG deadline = GetTickCount64() + kHelperTerminationTimeoutMs;
    ULONGLONG now = GetTickCount64();
    while (now < deadline) {
        const ULONGLONG remaining = deadline - now;
        const DWORD wait_ms = static_cast<DWORD>(std::min<ULONGLONG>(remaining, 100u));
        wait_result = WaitForSingleObject(process.hProcess, wait_ms);
        if (wait_result == WAIT_OBJECT_0)
            return true;
        if (wait_result == WAIT_FAILED)
            return false;
        now = GetTickCount64();
    }
    return WaitForSingleObject(process.hProcess, 0u) == WAIT_OBJECT_0;
}

bool close_helper_handles(PROCESS_INFORMATION& process, HANDLE& job) noexcept {
    bool closed = true;
    const auto close = [&closed](HANDLE& handle) noexcept {
        if (handle != nullptr) {
            if (!CloseHandle(handle))
                closed = false;
            handle = nullptr;
        }
    };
    close(process.hThread);
    close(process.hProcess);
    close(job);
    return closed;
}

bool post_launcher_quit(DWORD launcher_thread_id, HANDLE cancel_event) noexcept {
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (is_cancelled(cancel_event))
            return false;
        if (PostThreadMessageW(launcher_thread_id, WM_QUIT, 0, 0))
            return true;
        Sleep(10u);
    }
    return false;
}

bool launch_helper(const fs::path& helper_path, const fs::path& archive_path,
                   const fs::path& target_dir, const fs::path& restart_exe, std::string_view sha256,
                   DWORD launcher_thread_id, HANDLE cancel_event,
                   bool* preserve_staging_out) noexcept {
    if (preserve_staging_out != nullptr)
        *preserve_staging_out = false;
    if (is_cancelled(cancel_event))
        return false;
    const fs::path ready_marker = archive_path.parent_path() / L"helper.ready";
    if (!DeleteFileW(ready_marker.wstring().c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
        return false;

    std::wstring command = quote_argument(helper_path.wstring());
    command += L" --parent-pid ";
    command += quote_argument(std::to_wstring(GetCurrentProcessId()));
    command += L" --archive ";
    command += quote_argument(archive_path.wstring());
    command += L" --target ";
    command += quote_argument(target_dir.wstring());
    command += L" --restart ";
    command += quote_argument(restart_exe.wstring());
    command += L" --sha256 ";
    command += quote_argument(std::wstring(sha256.begin(), sha256.end()));

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr)
        return false;
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helper_path.wstring().c_str(), mutable_command.data(), nullptr, nullptr,
                        FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                        target_dir.wstring().c_str(), &startup, &process)) {
        (void)close_helper_handles(process, job);
        return false;
    }
    const bool process_owned_by_job = AssignProcessToJobObject(job, process.hProcess) != FALSE;
    const bool resumed =
        process_owned_by_job && ResumeThread(process.hThread) != static_cast<DWORD>(-1);
    bool ready = false;
    bool helper_shutdown_confirmed = true;
    if (!process_owned_by_job || !resumed) {
        if (preserve_staging_out != nullptr)
            *preserve_staging_out = true;
        helper_shutdown_confirmed = terminate_helper(process, process_owned_by_job ? job : nullptr);
    } else {
        const ULONGLONG deadline = GetTickCount64() + kHelperReadyTimeoutMs;
        while (GetTickCount64() < deadline) {
            if (is_cancelled(cancel_event))
                break;
            const DWORD process_result = WaitForSingleObject(process.hProcess, 50u);
            if (process_result == WAIT_OBJECT_0 || process_result == WAIT_FAILED)
                break;
            if (process_result == WAIT_TIMEOUT && regular_ready_marker(ready_marker)) {
                ready = post_launcher_quit(launcher_thread_id, cancel_event);
                break;
            }
        }
        if (!ready) {
            if (preserve_staging_out != nullptr)
                *preserve_staging_out = true;
            helper_shutdown_confirmed = terminate_helper(process, job);
        }
    }
    const bool handles_closed = close_helper_handles(process, job);
    if (!handles_closed && preserve_staging_out != nullptr)
        *preserve_staging_out = true;
    return ready && helper_shutdown_confirmed && handles_closed;
}

bool run_auto_update(const WorkerContext& context) noexcept {
    try {
        if (is_cancelled(context.cancel_event) || !context.configuration.enabled ||
            context.base_dir.empty() || context.exe_path.empty() ||
            context.configuration.manifest_url != kNativeUpdateManifestUrl ||
            !is_exact_json_url(context.configuration.manifest_url)) {
            return false;
        }
        prune_staging_directories();
        if (is_cancelled(context.cancel_event))
            return false;

        sao_updater_manifest_t manifest{};
        if (sao_updater_fetch_manifest_pinned(
                context.configuration.manifest_url.c_str(),
                context.configuration.server_tls_spki_sha256.data(),
                context.configuration.server_tls_spki_sha256.size(), &manifest) != SAO_OK ||
            is_cancelled(context.cancel_event)) {
            return false;
        }

        const std::size_t version_length =
            bounded_length(manifest.version, kManifestVersionCapacity);
        const std::size_t url_length = bounded_length(manifest.url, kManifestUrlCapacity);
        const std::size_t sha256_length = bounded_length(manifest.sha256, kManifestSha256Capacity);
        if (version_length == 0u || version_length >= kManifestVersionCapacity) {
            return false;
        }
        if (url_length == 0u && sha256_length == 0u && manifest.size == 0u) {
            return false;
        }
        if (url_length == 0u || url_length >= kManifestUrlCapacity ||
            sha256_length != kManifestSha256Capacity - 1u) {
            return false;
        }

        const std::string_view version(manifest.version, version_length);
        const std::string_view download_url(manifest.url, url_length);
        const std::string_view sha256(manifest.sha256, sha256_length);
        const std::string version_copy(version);
        if (sao_updater_compare_semver(version_copy.c_str(), SAO_LAUNCHER_VERSION) <= 0 ||
            !same_origin(context.configuration.manifest_url, download_url) || !is_sha256(sha256) ||
            manifest.size == 0u || manifest.size > kMaximumUpdateBytes ||
            is_cancelled(context.cancel_event)) {
            return false;
        }

        const fs::path staging = create_staging_directory();
        if (staging.empty())
            return false;
        StagingCleanup cleanup(staging);
        if (is_cancelled(context.cancel_event))
            return false;
        const fs::path archive = staging / L"SaoAutoUpdate.archive";
        const fs::path helper = staging / L"SaoAutoUpdateHelper.exe";
        const fs::path source_helper = fs::path(context.base_dir) / L"SaoAutoUpdateHelper.exe";
        const fs::path target(context.base_dir);
        const fs::path restart(context.exe_path);

        if (!CopyFileW(source_helper.wstring().c_str(), helper.wstring().c_str(), TRUE) ||
            is_cancelled(context.cancel_event)) {
            return false;
        }
        if (sao_updater_download_w_pinned(
                manifest.url, context.configuration.server_tls_spki_sha256.data(),
                context.configuration.server_tls_spki_sha256.size(), manifest.sha256,
                archive.wstring().c_str(), nullptr, nullptr) != SAO_OK ||
            is_cancelled(context.cancel_event)) {
            return false;
        }

        std::error_code error;
        const std::uintmax_t downloaded_size = fs::file_size(archive, error);
        if (error || downloaded_size != manifest.size || is_cancelled(context.cancel_event)) {
            return false;
        }
        bool preserve_staging = false;
        if (launch_helper(helper, archive, target, restart, sha256, context.launcher_thread_id,
                          context.cancel_event, &preserve_staging)) {
            cleanup.preserve();
            return true;
        }
        if (preserve_staging)
            cleanup.preserve();
    } catch (...) {
    }
    return false;
}

unsigned __stdcall auto_update_worker(void* raw_context) noexcept {
    std::unique_ptr<WorkerContext> context(static_cast<WorkerContext*>(raw_context));
    if (context != nullptr)
        run_auto_update(*context);
    return 0u;
}

} // namespace

bool startAutoUpdate(UpdateProviderConfiguration configuration, std::wstring base_dir,
                     std::wstring exe_path, DWORD launcher_thread_id, HANDLE* cancel_event_out,
                     HANDLE* worker_handle_out) noexcept {
    if (cancel_event_out == nullptr || worker_handle_out == nullptr)
        return false;
    *cancel_event_out = nullptr;
    *worker_handle_out = nullptr;
    HANDLE cancel_event = nullptr;
    try {
        if (!configuration.enabled || base_dir.empty() || exe_path.empty()) {
            return false;
        }
        cancel_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (cancel_event == nullptr)
            return false;

        auto context = std::make_unique<WorkerContext>();
        context->configuration = std::move(configuration);
        context->base_dir = std::move(base_dir);
        context->exe_path = std::move(exe_path);
        context->launcher_thread_id = launcher_thread_id;
        context->cancel_event = cancel_event;
        const uintptr_t thread =
            _beginthreadex(nullptr, 0u, &auto_update_worker, context.get(), 0u, nullptr);
        if (thread == 0u) {
            CloseHandle(cancel_event);
            return false;
        }
        context.release();
        *cancel_event_out = cancel_event;
        *worker_handle_out = reinterpret_cast<HANDLE>(thread);
        return true;
    } catch (...) {
        if (cancel_event != nullptr)
            (void)CloseHandle(cancel_event);
        return false;
    }
}

} // namespace sao::launcher