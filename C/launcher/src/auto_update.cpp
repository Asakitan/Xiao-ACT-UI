#include "sao/launcher/auto_update.h"
#include "sao/launcher/single_instance.h"

#include "sao/server/freetier/updater/updater.h"
#include "sao/server/freetier/updater/handoff.h"
#include "sao_core/sao_status.h"

#include <process.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <memory>
#include <span>
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
constexpr DWORD kHelperCompletionTimeoutMs = 30u * 1000u;
constexpr std::string_view kNativeUpdateManifestUrl =
    "https://x2.sjcmc.cn:15018/update/stable/windows-x64-native/latest.json";

struct WorkerContext {
    UpdateProviderConfiguration configuration;
    std::wstring base_dir;
    std::wstring exe_path;
    DWORD replacement_owner_pid = 0u;
    bool bootstrap_parent = false;
    DWORD launcher_thread_id = 0;
    HANDLE cancel_event = nullptr;
};

bool inherited_parent_pid(DWORD& parent_pid) noexcept {
    parent_pid = 0u;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0u);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            if (entry.th32ProcessID == GetCurrentProcessId()) {
                parent_pid = entry.th32ParentProcessID;
                found = parent_pid != 0u;
                break;
            }
        } while (Process32NextW(snapshot, &entry) != FALSE);
    }
    const bool closed = CloseHandle(snapshot) != FALSE;
    return found && closed;
}

bool expected_bootstrap_process(HANDLE process, std::wstring_view base_dir) noexcept {
    try {
        std::vector<wchar_t> buffer(512u);
        for (;;) {
            DWORD length = static_cast<DWORD>(buffer.size());
            if (QueryFullProcessImageNameW(process, 0u, buffer.data(), &length) != FALSE) {
                std::wstring actual(buffer.data(), length);
                const fs::path expected = fs::path(base_dir) / L"SaoAuto.exe";
                std::wstring expected_canonical;
                std::wstring actual_canonical;
                return singleInstanceCanonicalInstallPath(expected.c_str(), expected_canonical) &&
                       singleInstanceCanonicalInstallPath(actual.c_str(), actual_canonical) &&
                       CompareStringOrdinal(
                           expected_canonical.data(), static_cast<int>(expected_canonical.size()),
                           actual_canonical.data(), static_cast<int>(actual_canonical.size()),
                           TRUE) == CSTR_EQUAL;
            }
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || buffer.size() >= 32768u)
                return false;
            buffer.resize(std::min<std::size_t>(buffer.size() * 2u, 32768u));
        }
    } catch (...) {
        return false;
    }
}

bool exact_install_entry(std::wstring_view base_dir,
                         std::wstring_view exe_path) noexcept {
    try {
        if (base_dir.empty() || exe_path.empty())
            return false;
        const fs::path expected = fs::path(base_dir) / L"SaoAuto.exe";
        std::wstring expected_canonical;
        std::wstring actual_canonical;
        return singleInstanceCanonicalInstallPath(expected.c_str(), expected_canonical) &&
               singleInstanceCanonicalInstallPath(std::wstring(exe_path).c_str(),
                                                  actual_canonical) &&
               CompareStringOrdinal(
                   expected_canonical.data(), static_cast<int>(expected_canonical.size()),
                   actual_canonical.data(), static_cast<int>(actual_canonical.size()), TRUE) ==
                   CSTR_EQUAL;
    } catch (...) {
        return false;
    }
}

DWORD replacement_owner_pid(std::wstring_view base_dir, bool& bootstrap_parent) noexcept {
    bootstrap_parent = false;
    const DWORD current_pid = GetCurrentProcessId();
    constexpr wchar_t kVariable[] = L"SAO_BOOTSTRAP_PID";
    wchar_t buffer[16]{};
    SetLastError(ERROR_SUCCESS);
    const DWORD length =
        GetEnvironmentVariableW(kVariable, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0u) {
        if (GetLastError() != ERROR_ENVVAR_NOT_FOUND ||
            !expected_bootstrap_process(GetCurrentProcess(), base_dir)) {
            return 0u;
        }
        return current_pid;
    }
    if (length >= std::size(buffer))
        return 0u;
    for (DWORD index = 0u; index < length; ++index) {
        if (buffer[index] < L'0' || buffer[index] > L'9')
            return 0u;
    }
    wchar_t* end = nullptr;
    const unsigned long long value = std::wcstoull(buffer, &end, 10);
    if (value == 0u || value > MAXDWORD || end != buffer + length || *end != L'\0' ||
        value == current_pid) {
        return 0u;
    }
    DWORD actual_parent_pid = 0u;
    if (!inherited_parent_pid(actual_parent_pid) || actual_parent_pid != value)
        return 0u;
    HANDLE parent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                static_cast<DWORD>(value));
    if (parent == nullptr)
        return 0u;
    const bool active = WaitForSingleObject(parent, 0u) == WAIT_TIMEOUT;
    const bool expected = expected_bootstrap_process(parent, base_dir);
    const bool closed = CloseHandle(parent) != FALSE;
    if (!active || !expected || !closed)
        return 0u;
    bootstrap_parent = true;
    return static_cast<DWORD>(value);
}

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

bool fetch_validated_manifest(const UpdateProviderConfiguration& configuration,
                              HANDLE cancel_event,
                              sao_updater_manifest_t& manifest) noexcept {
    manifest = {};
    if (!configuration.enabled ||
        configuration.manifest_url != kNativeUpdateManifestUrl ||
        !is_exact_json_url(configuration.manifest_url) || is_cancelled(cancel_event) ||
        sao_updater_fetch_manifest_pinned(
            configuration.manifest_url.c_str(),
            configuration.server_tls_spki_sha256.data(),
            configuration.server_tls_spki_sha256.size(), &manifest) != SAO_OK ||
        is_cancelled(cancel_event)) {
        return false;
    }

    const std::size_t version_length =
        bounded_length(manifest.version, kManifestVersionCapacity);
    const std::size_t url_length = bounded_length(manifest.url, kManifestUrlCapacity);
    const std::size_t sha256_length =
        bounded_length(manifest.sha256, kManifestSha256Capacity);
    if (version_length == 0u || version_length >= kManifestVersionCapacity ||
        url_length == 0u || url_length >= kManifestUrlCapacity ||
        sha256_length != kManifestSha256Capacity - 1u || manifest.size == 0u ||
        manifest.size > kMaximumUpdateBytes) {
        return false;
    }

    const std::string_view download_url(manifest.url, url_length);
    const std::string_view sha256(manifest.sha256, sha256_length);
    return same_origin(configuration.manifest_url, download_url) && is_sha256(sha256);
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
constexpr wchar_t kStagingOwnerMarker[] = L".sao-update-owner";
constexpr std::array<char, 10> kStagingPreservationMarkerBytes = {'p', 'r', 'e', 's', 'e',
                                                                  'r', 'v', 'e', 'd', '\n'};
constexpr wchar_t kHelperReadyMarker[] = L"helper.ready";
constexpr wchar_t kHelperCompletionMarker[] = L"helper.complete";
constexpr std::string_view kStagingOwnerMarkerPrefix = "SAO-UPDATE-STAGING-OWNER-1\n";
constexpr std::size_t kMaximumStagingDirectories = 64u;
constexpr std::size_t kMaximumStagingEntries = 16u;

bool path_is_absent(const fs::path& path) noexcept {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        return false;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool parse_staging_owner_pid(const fs::path& staging, DWORD& pid_out) noexcept {
    pid_out = 0u;
    try {
        const std::wstring name = staging.filename().wstring();
        if (name.rfind(L"run-", 0u) != 0u)
            return false;
        const std::size_t separator = name.find(L'-', 4u);
        if (separator == std::wstring::npos || separator == 4u || separator + 1u >= name.size() ||
            name.size() - separator - 1u > 20u) {
            return false;
        }
        for (std::size_t index = 4u; index < separator; ++index) {
            if (name[index] < L'0' || name[index] > L'9')
                return false;
        }

        wchar_t* end = nullptr;
        const unsigned long parsed = std::wcstoul(name.c_str() + 4u, &end, 10);
        if (parsed == 0u || parsed > MAXDWORD || end != name.c_str() + separator)
            return false;
        for (std::size_t index = separator + 1u; index < name.size(); ++index) {
            if (name[index] < L'0' || name[index] > L'9')
                return false;
        }
        pid_out = static_cast<DWORD>(parsed);
        return true;
    } catch (...) {
        pid_out = 0u;
        return false;
    }
}

bool staging_owner_marker_bytes(const fs::path& staging, std::vector<char>& bytes) noexcept {
    try {
        DWORD owner_pid = 0u;
        const std::wstring leaf = staging.filename().wstring();
        if (!parse_staging_owner_pid(staging, owner_pid) || owner_pid == 0u)
            return false;
        bytes.assign(kStagingOwnerMarkerPrefix.begin(), kStagingOwnerMarkerPrefix.end());
        for (const wchar_t value : leaf) {
            if (value > 0x7fu)
                return false;
            bytes.push_back(static_cast<char>(value));
        }
        bytes.push_back('\n');
        return !bytes.empty();
    } catch (...) {
        bytes.clear();
        return false;
    }
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
    if (!GetFileAttributesExW(staging.c_str(), GetFileExInfoStandard, &attributes) ||
        (attributes.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
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

bool handoff_temporary_leaf(std::wstring_view leaf) noexcept {
    constexpr std::wstring_view ready_prefix = L"helper.ready.tmp.";
    constexpr std::wstring_view complete_prefix = L"helper.complete.tmp.";
    const std::wstring_view prefix = leaf.starts_with(ready_prefix)
        ? ready_prefix
        : (leaf.starts_with(complete_prefix) ? complete_prefix : std::wstring_view{});
    if (prefix.empty() || leaf.size() == prefix.size() || leaf.size() - prefix.size() > 10u)
        return false;
    const std::wstring_view suffix = leaf.substr(prefix.size());
    return std::all_of(suffix.begin(), suffix.end(),
                       [](wchar_t value) { return value >= L'0' && value <= L'9'; });
}

bool known_staging_leaf(std::wstring_view leaf) noexcept {
    return leaf == L"SaoAutoUpdate.archive" || leaf == L"SaoAutoUpdate.archive.part" ||
           leaf == L"SaoAutoUpdateHelper.exe" || leaf == kHelperReadyMarker ||
           leaf == kHelperCompletionMarker ||
           leaf == kStagingPreservationMarker || leaf == kStagingOwnerMarker ||
           handoff_temporary_leaf(leaf);
}

bool exact_marker_file(const fs::path& marker, std::span<const char> expected) noexcept {
    std::vector<char> bytes;
    try {
        bytes.resize(expected.size());
    } catch (...) {
        return false;
    }
    HANDLE file = CreateFileW(marker.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    FILE_STANDARD_INFO standard{};
    DWORD read = 0u;
    const bool valid =
        GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes, sizeof(attributes)) !=
            FALSE &&
        (attributes.FileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) == 0u &&
        GetFileType(file) == FILE_TYPE_DISK &&
        GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) !=
            FALSE &&
        !bytes.empty() && standard.EndOfFile.QuadPart == static_cast<LONGLONG>(bytes.size()) &&
        ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != FALSE &&
        read == static_cast<DWORD>(bytes.size()) &&
        std::equal(bytes.begin(), bytes.end(), expected.begin(), expected.end());
    const bool closed = CloseHandle(file) != FALSE;
    return valid && closed;
}

bool read_handoff_record(const fs::path& marker,
                         sao_update_handoff_v1_t& record) noexcept {
    record = {};
    HANDLE file = CreateFileW(marker.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    FILE_STANDARD_INFO standard{};
    DWORD read = 0u;
    const bool loaded =
        GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes,
                                     sizeof(attributes)) != FALSE &&
        (attributes.FileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) == 0u &&
        GetFileType(file) == FILE_TYPE_DISK &&
        GetFileInformationByHandleEx(file, FileStandardInfo, &standard,
                                     sizeof(standard)) != FALSE &&
        standard.EndOfFile.QuadPart == static_cast<LONGLONG>(sizeof(record)) &&
        ReadFile(file, &record, sizeof(record), &read, nullptr) != FALSE &&
        read == sizeof(record);
    const bool closed = CloseHandle(file) != FALSE;
    if (!loaded || !closed)
        return false;

    const std::size_t version_length = bounded_length(
        record.version, SAO_UPDATE_HANDOFF_VERSION_CAPACITY);
    const std::size_t sha_length = bounded_length(
        record.sha256, SAO_UPDATE_HANDOFF_SHA256_CAPACITY);
    const bool reserved_zero = std::all_of(
        std::begin(record.reserved), std::end(record.reserved),
        [](std::uint8_t value) { return value == 0u; });
    return record.struct_size == sizeof(record) &&
        record.abi_version == SAO_UPDATE_HANDOFF_ABI_VERSION &&
        record.phase >= SAO_UPDATE_HANDOFF_PHASE_READY &&
        record.phase <= SAO_UPDATE_HANDOFF_PHASE_COMPLETE &&
        record.parent_pid != 0u && record.helper_pid != 0u && record.flags == 0u &&
        version_length > 0u && version_length < SAO_UPDATE_HANDOFF_VERSION_CAPACITY &&
        sha_length == 64u && is_sha256(std::string_view(record.sha256, sha_length)) &&
        reserved_zero;
}

bool handoff_matches(const sao_update_handoff_v1_t& record,
                     std::string_view version, std::string_view sha256,
                     DWORD parent_pid, DWORD helper_pid) noexcept {
    const std::size_t version_length = bounded_length(
        record.version, SAO_UPDATE_HANDOFF_VERSION_CAPACITY);
    const std::size_t sha_length = bounded_length(
        record.sha256, SAO_UPDATE_HANDOFF_SHA256_CAPACITY);
    return record.parent_pid == parent_pid && record.helper_pid == helper_pid &&
        version_length == version.size() && sha_length == sha256.size() &&
        std::equal(version.begin(), version.end(), record.version) &&
        equal_ascii_ignore_case(
            sha256, std::string_view(record.sha256, sha_length));
}

bool exact_preservation_marker(const fs::path& marker) noexcept {
    return exact_marker_file(marker, kStagingPreservationMarkerBytes);
}

bool exact_staging_owner_marker(const fs::path& staging) noexcept {
    try {
        std::vector<char> expected;
        return staging_owner_marker_bytes(staging, expected) &&
               exact_marker_file(staging / kStagingOwnerMarker, expected);
    } catch (...) {
        return false;
    }
}

bool write_exact_marker(const fs::path& marker, std::span<const char> bytes,
                        bool accept_existing) noexcept {
    HANDLE handle = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return accept_existing && (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) &&
               exact_marker_file(marker, bytes);
    }
    DWORD written = 0u;
    const bool wrote = WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                                 nullptr) != FALSE &&
                       written == static_cast<DWORD>(bytes.size()) &&
                       FlushFileBuffers(handle) != FALSE;
    const bool closed = CloseHandle(handle) != FALSE;
    if (!wrote || !closed)
        (void)DeleteFileW(marker.c_str());
    return wrote && closed;
}

bool write_staging_owner_marker(const fs::path& staging) noexcept {
    try {
        std::vector<char> bytes;
        return staging_owner_marker_bytes(staging, bytes) &&
               write_exact_marker(staging / kStagingOwnerMarker, bytes, false);
    } catch (...) {
        return false;
    }
}

bool collect_known_staging_files(const fs::path& staging, std::vector<fs::path>& files) noexcept {
    try {
        std::error_code error;
        fs::directory_iterator iterator(staging, fs::directory_options::none, error);
        if (error)
            return false;
        for (const fs::directory_iterator end{}; iterator != end; iterator.increment(error)) {
            if (error)
                return false;
            if (files.size() >= kMaximumStagingEntries)
                return false;
            const DWORD attributes = GetFileAttributesW(iterator->path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY |
                               FILE_ATTRIBUTE_DEVICE)) != 0u ||
                !known_staging_leaf(iterator->path().filename().wstring())) {
                return false;
            }
            if (iterator->path().filename().wstring() == kStagingPreservationMarker &&
                !exact_preservation_marker(iterator->path())) {
                return false;
            }
            if (iterator->path().filename().wstring() == kStagingOwnerMarker &&
                !exact_staging_owner_marker(staging)) {
                return false;
            }
            const std::wstring leaf = iterator->path().filename().wstring();
            if (leaf == kHelperReadyMarker || leaf == kHelperCompletionMarker) {
                sao_update_handoff_v1_t record{};
                if (!read_handoff_record(iterator->path(), record) ||
                    (leaf == kHelperReadyMarker &&
                     record.phase != SAO_UPDATE_HANDOFF_PHASE_READY) ||
                    (leaf == kHelperCompletionMarker &&
                     record.phase == SAO_UPDATE_HANDOFF_PHASE_READY)) {
                    return false;
                }
            }
            files.push_back(iterator->path());
        }
        return !error;
    } catch (...) {
        files.clear();
        return false;
    }
}

bool delete_known_staging_directory(const fs::path& staging) noexcept {
    try {
        const DWORD attributes = GetFileAttributesW(staging.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
            return path_is_absent(staging);
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
            return false;
        }
        if (!exact_staging_owner_marker(staging))
            return false;

        std::vector<fs::path> files;
        if (!collect_known_staging_files(staging, files))
            return false;
        std::sort(files.begin(), files.end(), [](const fs::path& left, const fs::path& right) {
            const auto rank = [](const fs::path& value) {
                if (value.filename() == L"SaoAutoUpdateHelper.exe")
                    return 0;
                if (value.filename() == kStagingPreservationMarker)
                    return 2;
                if (value.filename() == kStagingOwnerMarker)
                    return 3;
                return 1;
            };
            const int left_rank = rank(left);
            const int right_rank = rank(right);
            return left_rank != right_rank ? left_rank < right_rank
                                           : left.native() < right.native();
        });
        for (const fs::path& file : files) {
            if (!DeleteFileW(file.c_str()) && !path_is_absent(file))
                return false;
        }
        return RemoveDirectoryW(staging.c_str()) != FALSE || path_is_absent(staging);
    } catch (...) {
        return false;
    }
}

bool staging_is_safe_orphan(const fs::path& staging) {
    DWORD owner_pid = 0u;
    sao_update_handoff_v1_t ready{};
    const fs::path ready_path = staging / kHelperReadyMarker;
    const bool ready_absent = path_is_absent(ready_path);
    if (!parse_staging_owner_pid(staging, owner_pid) || !staging_is_old_enough(staging) ||
        !owner_process_is_gone(owner_pid) || !exact_staging_owner_marker(staging) ||
        !path_is_absent(staging / kStagingPreservationMarker) ||
        (!ready_absent &&
         (!read_handoff_record(ready_path, ready) ||
          ready.phase != SAO_UPDATE_HANDOFF_PHASE_READY))) {
        return false;
    }
    return true;
}

void prune_staging_directories() noexcept {
    try {
        const fs::path root = user_staging_root();
        if (root.empty())
            return;
        const DWORD root_attributes = GetFileAttributesW(root.c_str());
        if (root_attributes == INVALID_FILE_ATTRIBUTES ||
            (root_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
            (root_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
            return;
        }

        std::error_code error;
        std::size_t directories = 0u;
        for (fs::directory_iterator iterator(root, error); iterator != fs::directory_iterator();
             iterator.increment(error)) {
            if (error)
                return;
            if (++directories > kMaximumStagingDirectories)
                return;
            const DWORD attributes = GetFileAttributesW(iterator->path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
                (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u ||
                !staging_is_safe_orphan(iterator->path())) {
                continue;
            }
            (void)delete_known_staging_directory(iterator->path());
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

    const DWORD root_attributes = GetFileAttributesW(root.c_str());
    if (root_attributes == INVALID_FILE_ATTRIBUTES ||
        (root_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u ||
        (root_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
        return {};
    }

    const ULONGLONG seed = GetTickCount64();
    for (std::size_t attempt = 0u; attempt < 16u; ++attempt) {
        const fs::path staging = root / (L"run-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                         std::to_wstring(seed + attempt));
        if (CreateDirectoryW(staging.c_str(), nullptr) != FALSE) {
            if (write_staging_owner_marker(staging))
                return staging;
            (void)RemoveDirectoryW(staging.c_str());
            return {};
        }
        const DWORD create_error = GetLastError();
        if (create_error != ERROR_ALREADY_EXISTS && create_error != ERROR_FILE_EXISTS)
            return {};
    }
    return {};
}

bool write_staging_preservation_marker(const fs::path& staging) noexcept {
    try {
        const fs::path marker = staging / kStagingPreservationMarker;
        return write_exact_marker(marker, kStagingPreservationMarkerBytes, true);
    } catch (...) {
        return false;
    }
}

bool acknowledge_completed_staging(const fs::path& staging, DWORD restart_owner_pid,
                                   std::string_view expected_version,
                                   std::string_view expected_sha256) noexcept {
    try {
        if (!exact_staging_owner_marker(staging))
            return false;
        const fs::path completion = staging / kHelperCompletionMarker;
        if (path_is_absent(completion)) {
            DWORD legacy_owner_pid = 0u;
            const fs::path archive = staging / L"SaoAutoUpdate.archive";
            if (expected_version != SAO_LAUNCHER_VERSION ||
                !path_is_absent(staging / kHelperReadyMarker) ||
                !path_is_absent(staging / kStagingPreservationMarker) ||
                !parse_staging_owner_pid(staging, legacy_owner_pid) ||
                !owner_process_is_gone(legacy_owner_pid) ||
                sao_updater_verify_file_sha256_w(
                    archive.c_str(), std::string(expected_sha256).c_str()) != SAO_OK) {
                return false;
            }
            const ULONGLONG legacy_deadline = GetTickCount64() + kHelperCompletionTimeoutMs;
            do {
                if (delete_known_staging_directory(staging))
                    return true;
                Sleep(25u);
            } while (GetTickCount64() < legacy_deadline);
            return false;
        }

        const ULONGLONG deadline = GetTickCount64() + kHelperCompletionTimeoutMs;
        sao_update_handoff_v1_t record{};
        for (;;) {
            if (!read_handoff_record(completion, record) ||
                record.restart_pid != restart_owner_pid) {
                return false;
            }
            const std::size_t version_length = bounded_length(
                record.version, SAO_UPDATE_HANDOFF_VERSION_CAPACITY);
            const std::size_t sha_length = bounded_length(
                record.sha256, SAO_UPDATE_HANDOFF_SHA256_CAPACITY);
            if (expected_version != SAO_LAUNCHER_VERSION ||
                std::string_view(record.version, version_length) != expected_version ||
                !equal_ascii_ignore_case(
                    std::string_view(record.sha256, sha_length), expected_sha256)) {
                return false;
            }
            if (record.phase == SAO_UPDATE_HANDOFF_PHASE_COMPLETE)
                break;
            if (record.phase != SAO_UPDATE_HANDOFF_PHASE_RESTARTING ||
                record.status != SAO_UPDATE_HANDOFF_STATUS_PENDING ||
                GetTickCount64() >= deadline) {
                return false;
            }
            Sleep(25u);
        }

        if (record.status != 0u) {
            (void)write_staging_preservation_marker(staging);
            return false;
        }
        while (!owner_process_is_gone(record.helper_pid)) {
            if (GetTickCount64() >= deadline)
                return false;
            Sleep(25u);
        }
        return delete_known_staging_directory(staging);
    } catch (...) {
        return false;
    }
}

bool has_completed_update_candidate() noexcept {
    try {
        const fs::path root = user_staging_root();
        const DWORD attributes = GetFileAttributesW(root.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
            return false;
        }
        std::error_code error;
        std::size_t count = 0u;
        for (fs::directory_iterator iterator(root, error);
             iterator != fs::directory_iterator(); iterator.increment(error)) {
            if (error || ++count > kMaximumStagingDirectories)
                return false;
            const fs::path& staging = iterator->path();
            const DWORD staging_attributes = GetFileAttributesW(staging.c_str());
            if (staging_attributes == INVALID_FILE_ATTRIBUTES ||
                (staging_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
                (staging_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u ||
                !exact_staging_owner_marker(staging)) {
                continue;
            }
            if (!path_is_absent(staging / kHelperCompletionMarker) ||
                (!path_is_absent(staging / L"SaoAutoUpdate.archive") &&
                 path_is_absent(staging / kHelperReadyMarker) &&
                 path_is_absent(staging / kStagingPreservationMarker))) {
                return true;
            }
        }
    } catch (...) {
    }
    return false;
}

bool acknowledge_completed_updates(DWORD restart_owner_pid,
                                    std::string_view expected_version,
                                    std::string_view expected_sha256) noexcept {
    try {
        if (restart_owner_pid == 0u)
            return false;
        const fs::path root = user_staging_root();
        const DWORD attributes = GetFileAttributesW(root.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
            return false;
        }
        std::error_code error;
        std::size_t count = 0u;
        bool acknowledged = false;
        for (fs::directory_iterator iterator(root, error);
             iterator != fs::directory_iterator(); iterator.increment(error)) {
            if (error || ++count > kMaximumStagingDirectories)
                return acknowledged;
            acknowledged = acknowledge_completed_staging(
                iterator->path(), restart_owner_pid, expected_version, expected_sha256) ||
                acknowledged;
        }
        return acknowledged;
    } catch (...) {
    }
    return false;
}

class StagingCleanup {
  public:
    explicit StagingCleanup(fs::path path) : path_(std::move(path)) {}
    ~StagingCleanup() {
        if (!preserve_)
            (void)delete_known_staging_directory(path_);
    }
    void preserve() noexcept {
        preserve_ = true;
        (void)write_staging_preservation_marker(path_);
    }
    void defer_cleanup() noexcept {
        preserve_ = true;
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

bool exact_ready_marker(const fs::path& marker, std::string_view version,
                        std::string_view sha256, DWORD parent_pid,
                        DWORD helper_pid) noexcept {
    sao_update_handoff_v1_t record{};
    return read_handoff_record(marker, record) &&
        record.phase == SAO_UPDATE_HANDOFF_PHASE_READY &&
        record.status == SAO_UPDATE_HANDOFF_STATUS_PENDING &&
        record.restart_pid == 0u &&
        handoff_matches(record, version, sha256, parent_pid, helper_pid);
}

bool set_job_kill_on_close(HANDLE job, bool enabled) noexcept {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = enabled ? JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE : 0u;
    return SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                   sizeof(limits)) != FALSE;
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
                   std::string_view version, DWORD replacement_owner_pid,
                   bool bootstrap_parent, DWORD launcher_thread_id, HANDLE cancel_event,
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
    command += quote_argument(std::to_wstring(replacement_owner_pid));
    command += L" --archive ";
    command += quote_argument(archive_path.wstring());
    command += L" --target ";
    command += quote_argument(target_dir.wstring());
    command += L" --restart ";
    command += quote_argument(restart_exe.wstring());
    command += L" --sha256 ";
    command += quote_argument(std::wstring(sha256.begin(), sha256.end()));
    command += L" --version ";
    command += quote_argument(std::wstring(version.begin(), version.end()));

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr)
        return false;
    if (!set_job_kill_on_close(job, true)) {
        PROCESS_INFORMATION empty_process{};
        (void)close_helper_handles(empty_process, job);
        return false;
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    DWORD creation_flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
    if (bootstrap_parent)
        creation_flags |= CREATE_BREAKAWAY_FROM_JOB;
    if (!CreateProcessW(helper_path.wstring().c_str(), mutable_command.data(), nullptr, nullptr,
                        FALSE, creation_flags, nullptr, target_dir.wstring().c_str(), &startup,
                        &process)) {
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
            if (process_result == WAIT_TIMEOUT &&
                exact_ready_marker(ready_marker, version, sha256,
                                   replacement_owner_pid, process.dwProcessId)) {
                ready = post_launcher_quit(launcher_thread_id, cancel_event) &&
                        set_job_kill_on_close(job, false);
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
            context.base_dir.empty() || context.exe_path.empty()) {
            return false;
        }
        prune_staging_directories();
        if (is_cancelled(context.cancel_event))
            return false;

        sao_updater_manifest_t manifest{};
        if (!fetch_validated_manifest(context.configuration, context.cancel_event, manifest)) {
            return false;
        }

        const std::size_t version_length =
            bounded_length(manifest.version, kManifestVersionCapacity);
        const std::size_t sha256_length = bounded_length(manifest.sha256, kManifestSha256Capacity);

        const std::string_view version(manifest.version, version_length);
        const std::string_view sha256(manifest.sha256, sha256_length);
        const std::string version_copy(version);
        (void)acknowledge_completed_updates(context.replacement_owner_pid, version, sha256);
        if (sao_updater_compare_semver(version_copy.c_str(), SAO_LAUNCHER_VERSION) <= 0)
            return false;

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
        const fs::path restart = fs::path(context.base_dir) / L"SaoAuto.exe";

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
        if (launch_helper(helper, archive, target, restart, sha256, version,
                  context.replacement_owner_pid, context.bootstrap_parent,
                  context.launcher_thread_id,
                          context.cancel_event, &preserve_staging)) {
            cleanup.defer_cleanup();
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

bool acknowledgeCompletedAutoUpdate(const UpdateProviderConfiguration& configuration,
                                    const std::wstring& base_dir,
                                    const std::wstring& exe_path) noexcept {
    try {
        if (!configuration.enabled || base_dir.empty() || exe_path.empty() ||
            !exact_install_entry(base_dir, exe_path) || !has_completed_update_candidate()) {
            return false;
        }
        bool bootstrap_parent = false;
        const DWORD restart_owner_pid = replacement_owner_pid(base_dir, bootstrap_parent);
        if (restart_owner_pid == 0u)
            return false;

        sao_updater_manifest_t manifest{};
        if (!fetch_validated_manifest(configuration, nullptr, manifest))
            return false;
        const std::size_t version_length =
            bounded_length(manifest.version, kManifestVersionCapacity);
        const std::size_t sha256_length =
            bounded_length(manifest.sha256, kManifestSha256Capacity);
        const std::string_view version(manifest.version, version_length);
        const std::string_view sha256(manifest.sha256, sha256_length);
        if (version != SAO_LAUNCHER_VERSION)
            return false;
        return acknowledge_completed_updates(restart_owner_pid, version, sha256);
    } catch (...) {
        return false;
    }
}

bool startAutoUpdate(UpdateProviderConfiguration configuration, std::wstring base_dir,
                     std::wstring exe_path, DWORD launcher_thread_id, HANDLE* cancel_event_out,
                     HANDLE* worker_handle_out) noexcept {
    if (cancel_event_out == nullptr || worker_handle_out == nullptr)
        return false;
    *cancel_event_out = nullptr;
    *worker_handle_out = nullptr;
    HANDLE cancel_event = nullptr;
    try {
        if (!configuration.enabled || base_dir.empty() || exe_path.empty() ||
            !exact_install_entry(base_dir, exe_path)) {
            return false;
        }
        cancel_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (cancel_event == nullptr)
            return false;

        auto context = std::make_unique<WorkerContext>();
        context->configuration = std::move(configuration);
        context->base_dir = std::move(base_dir);
        context->exe_path = std::move(exe_path);
        context->replacement_owner_pid =
            replacement_owner_pid(context->base_dir, context->bootstrap_parent);
        if (context->replacement_owner_pid == 0u) {
            (void)CloseHandle(cancel_event);
            return false;
        }
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