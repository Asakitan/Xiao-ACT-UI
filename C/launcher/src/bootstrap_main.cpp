#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <sao/runtime/runtime_key.h>
#include <sao/shell/artifact/bundle.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace {

namespace fs = std::filesystem;

HANDLE bootstrap_diagnostic_output() noexcept {
    static HANDLE output = []() noexcept {
        HANDLE source = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (source == nullptr || source == INVALID_HANDLE_VALUE)
            return source;
        HANDLE duplicate = nullptr;
        return ::DuplicateHandle(::GetCurrentProcess(), source, ::GetCurrentProcess(), &duplicate,
                                 0u, FALSE, DUPLICATE_SAME_ACCESS) != FALSE
                   ? duplicate
                   : source;
    }();
    return output;
}

void bootstrap_diagnostic(const char* line) noexcept {
    if (line == nullptr)
        return;
    HANDLE output = bootstrap_diagnostic_output();
    if (output == nullptr || output == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0u;
    (void)::WriteFile(output, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
    static constexpr char newline[] = "\r\n";
    (void)::WriteFile(output, newline, sizeof(newline) - 1u, &written, nullptr);
}

constexpr wchar_t kRuntimeLeaf[] = L"runtime";
constexpr wchar_t kHelperLeaf[] = L"helper";
constexpr wchar_t kHelperTransactionLockLeaf[] = L".sao-runtime-helper.lock";
constexpr wchar_t kOwnerMarkerLeaf[] = L".sao-runtime-owner";
constexpr std::size_t kNonceBytes = 16u;
constexpr std::size_t kMarkerBytes = 64u;
constexpr std::size_t kMaximumPruneDirectories = 32u;
constexpr std::size_t kMaximumPruneEntries = 256u;
constexpr std::size_t kMaximumTreeEntries = 100'000u;
constexpr std::size_t kMaximumTreeDepth = 64u;
// Marker-less gen-* orphan reclaim: the directory's ftCreationTime must be
// at least this old (FILETIME 100ns units — 10 minutes) before pruning, so
// a racing create_generation_directory can never be swept between
// CreateDirectoryW and write_owner_marker.
constexpr std::uint64_t kOrphanGenerationAgeMargin = 10ull * 60ull * 10'000'000ull;
constexpr std::size_t kMaximumEnvironmentCharacters = 32'767u;
constexpr std::size_t kMaximumCommandLineCharacters = 32'767u;
constexpr DWORD kCleanupRetryTimeoutMs = 5'000u;
constexpr DWORD kCleanupRetryPollMs = 25u;
constexpr std::array<std::uint8_t, 16> kMarkerMagic = {'S', 'A', 'O', '-', 'R', 'U', 'N', 'T',
                                                       'I', 'M', 'E', '-', 'G', 'E', 'N', '1'};
constexpr std::array<std::wstring_view, 17> kExpectedSessionLeaves = {
    sao::runtime::kPayloadExecutable,   sao::runtime::kCoreDll,
    sao::runtime::kPlatformEngineDll,   sao::runtime::kPlatformNetDll,
    sao::runtime::kRuntimeInstallerDll, sao::runtime::kPlatformSdkDll,
    sao::runtime::kPlatformUiDll,       sao::runtime::kAntiScreencapDll,
    sao::runtime::kShellProtocolDll,    sao::runtime::kShellCrypterDll,
    sao::runtime::kShellPackerDll,      sao::runtime::kShellIntegratorDll,
    sao::runtime::kLicenseProtocolDll,  sao::runtime::kLicenseClientDll,
    sao::runtime::kLicenseSdkDll,       sao::runtime::kServerFreetierDll,
    sao::runtime::kPluginAiEditorDll,
};
constexpr std::array<std::wstring_view, 4> kExpectedHelperLeaves = {
    sao::runtime::kCoreDll,
    sao::runtime::kLicenseProtocolDll,
    sao::runtime::kLicenseClientDll,
    sao::runtime::kLicenseSdkDll,
};

constexpr DWORD kFailureInstall = 10u;
constexpr DWORD kFailureStorage = 11u;
constexpr DWORD kFailureGeneration = 12u;
constexpr DWORD kFailureVerification = 13u;
constexpr DWORD kFailureExtraction = 14u;
constexpr DWORD kFailureEnvironment = 15u;
constexpr DWORD kFailureLaunch = 16u;
constexpr DWORD kFailureWait = 17u;
constexpr DWORD kFailureCleanup = 18u;
constexpr DWORD kFailureUnexpected = 19u;

class UniqueHandle {
  public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            (void)close();
            handle_ = other.release();
        }
        return *this;
    }

    ~UniqueHandle() {
        (void)close();
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE release() noexcept {
        const HANDLE value = handle_;
        handle_ = nullptr;
        return value;
    }

    bool close() noexcept {
        if (!*this) {
            handle_ = nullptr;
            return true;
        }
        const HANDLE value = handle_;
        handle_ = nullptr;
        return ::CloseHandle(value) != FALSE;
    }

  private:
    HANDLE handle_ = nullptr;
};

class UniqueFindHandle {
  public:
    explicit UniqueFindHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept : handle_(handle) {}
    UniqueFindHandle(const UniqueFindHandle&) = delete;
    UniqueFindHandle& operator=(const UniqueFindHandle&) = delete;
    ~UniqueFindHandle() {
        if (handle_ != INVALID_HANDLE_VALUE)
            (void)::FindClose(handle_);
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    bool close() noexcept {
        if (handle_ == INVALID_HANDLE_VALUE)
            return true;
        const HANDLE value = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return ::FindClose(value) != FALSE;
    }

  private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

class LocalAllocation {
  public:
    LocalAllocation() noexcept = default;
    explicit LocalAllocation(void* pointer) noexcept : pointer_(pointer) {}
    LocalAllocation(const LocalAllocation&) = delete;
    LocalAllocation& operator=(const LocalAllocation&) = delete;

    LocalAllocation(LocalAllocation&& other) noexcept : pointer_(other.release()) {}
    LocalAllocation& operator=(LocalAllocation&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    ~LocalAllocation() {
        reset();
    }

    [[nodiscard]] void* get() const noexcept {
        return pointer_;
    }

    [[nodiscard]] void* release() noexcept {
        void* value = pointer_;
        pointer_ = nullptr;
        return value;
    }

    void reset(void* pointer = nullptr) noexcept {
        if (pointer_ != nullptr)
            (void)::LocalFree(pointer_);
        pointer_ = pointer;
    }

  private:
    void* pointer_ = nullptr;
};

class EnvironmentStrings {
  public:
    EnvironmentStrings() noexcept : value_(::GetEnvironmentStringsW()) {}
    EnvironmentStrings(const EnvironmentStrings&) = delete;
    EnvironmentStrings& operator=(const EnvironmentStrings&) = delete;
    ~EnvironmentStrings() {
        if (value_ != nullptr)
            (void)::FreeEnvironmentStringsW(value_);
    }

    [[nodiscard]] LPWCH get() const noexcept {
        return value_;
    }

  private:
    LPWCH value_ = nullptr;
};

template <typename Key> class KeyWiper {
  public:
    explicit KeyWiper(Key& key) noexcept : key_(key) {}
    KeyWiper(const KeyWiper&) = delete;
    KeyWiper& operator=(const KeyWiper&) = delete;
    ~KeyWiper() {
        wipe();
    }

    void wipe() noexcept {
        if (!wiped_) {
            sao::shell::artifact::secure_zero(key_.data(), key_.size());
            wiped_ = true;
        }
    }

  private:
    Key& key_;
    bool wiped_ = false;
};

enum class PathKind { directory, regular_file };

enum class OwnerState { active, dead, unknown };

enum class FileDeleteResult { deleted, absent, locked, failed, unsafe };

enum class TreeScanResult { safe, retained, unsafe };

enum class TreeDeleteResult { deleted, retained, failed };

struct MarkerRecord {
    DWORD owner_pid = 0u;
    std::uint64_t owner_creation_time = 0u;
    std::array<std::uint8_t, kNonceBytes> nonce{};
};

struct BootstrapOutcome {
    bool failed = true;
    DWORD return_code = kFailureUnexpected;
    const wchar_t* message = L"SaoAuto bootstrap failed.";
};

struct ExtractedPaths {
    fs::path payload;
    std::vector<fs::path> helpers;
};

struct EnvironmentEntry {
    std::wstring name;
    std::wstring value;
};

[[nodiscard]] BootstrapOutcome failure(DWORD code, const wchar_t* message) noexcept {
    return BootstrapOutcome{true, code == 0u ? kFailureUnexpected : code, message};
}

[[nodiscard]] BootstrapOutcome child_outcome(DWORD exit_code) noexcept {
    return BootstrapOutcome{false, exit_code, nullptr};
}

[[nodiscard]] bool is_missing_error(DWORD error) noexcept {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

[[nodiscard]] bool is_locked_error(DWORD error) noexcept {
    return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ||
           error == ERROR_USER_MAPPED_FILE || error == ERROR_ACCESS_DENIED;
}

[[nodiscard]] bool path_is_absent(const fs::path& path) noexcept {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
        return false;
    return is_missing_error(::GetLastError());
}

[[nodiscard]] bool equal_case_insensitive(std::wstring_view left,
                                          std::wstring_view right) noexcept {
    if (left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        right.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                  static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] std::wstring comparable_path(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (value.rfind(L"\\\\?\\UNC\\", 0u) == 0u) {
        value = L"\\\\" + value.substr(8u);
    } else if (value.rfind(L"\\\\?\\", 0u) == 0u) {
        value.erase(0u, 4u);
    }
    while (value.size() > 1u && value.back() == L'\\') {
        if (value.size() == 3u && value[1u] == L':')
            break;
        value.pop_back();
    }
    return value;
}

[[nodiscard]] bool full_comparable_path(const fs::path& path, std::wstring& output) noexcept {
    try {
        if (path.empty() || !path.is_absolute())
            return false;
        const std::wstring& native = path.native();
        const DWORD required = ::GetFullPathNameW(native.c_str(), 0u, nullptr, nullptr);
        if (required == 0u || required >= 32'768u)
            return false;
        std::wstring buffer(static_cast<std::size_t>(required) + 1u, L'\0');
        const DWORD written = ::GetFullPathNameW(native.c_str(), static_cast<DWORD>(buffer.size()),
                                                 buffer.data(), nullptr);
        if (written == 0u || written >= buffer.size())
            return false;
        buffer.resize(written);
        output = comparable_path(std::move(buffer));
        return !output.empty();
    } catch (...) {
        output.clear();
        return false;
    }
}

[[nodiscard]] bool final_comparable_path(HANDLE handle, std::wstring& output) noexcept {
    try {
        constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
        const DWORD required = ::GetFinalPathNameByHandleW(handle, nullptr, 0u, flags);
        if (required == 0u || required >= 32'768u)
            return false;
        std::wstring buffer(static_cast<std::size_t>(required) + 1u, L'\0');
        const DWORD written = ::GetFinalPathNameByHandleW(handle, buffer.data(),
                                                          static_cast<DWORD>(buffer.size()), flags);
        if (written == 0u || written >= buffer.size())
            return false;
        buffer.resize(written);
        output = comparable_path(std::move(buffer));
        return !output.empty();
    } catch (...) {
        output.clear();
        return false;
    }
}

[[nodiscard]] bool handle_matches_path(HANDLE handle, const fs::path& path) noexcept {
    std::wstring expected;
    std::wstring actual;
    return full_comparable_path(path, expected) && final_comparable_path(handle, actual) &&
           equal_case_insensitive(expected, actual);
}

[[nodiscard]] bool handle_has_kind(HANDLE handle, PathKind kind) noexcept {
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) == FALSE)
        return false;
    if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
        return false;
    const bool is_directory = (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;
    if (is_directory != (kind == PathKind::directory))
        return false;
    if (!is_directory && (tag.FileAttributes & FILE_ATTRIBUTE_DEVICE) != 0u)
        return false;
    return ::GetFileType(handle) == FILE_TYPE_DISK;
}

[[nodiscard]] UniqueHandle open_path_guard(const fs::path& path, PathKind kind, DWORD access,
                                           DWORD sharing) noexcept {
    const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT |
                        (kind == PathKind::directory ? FILE_FLAG_BACKUP_SEMANTICS : 0u);
    UniqueHandle handle(
        ::CreateFileW(path.c_str(), access, sharing, nullptr, OPEN_EXISTING, flags, nullptr));
    if (!handle)
        return {};
    if (!handle_has_kind(handle.get(), kind) || !handle_matches_path(handle.get(), path)) {
        (void)handle.close();
        ::SetLastError(ERROR_REPARSE_TAG_INVALID);
        return {};
    }
    return handle;
}

[[nodiscard]] UniqueHandle open_directory_guard(const fs::path& path, DWORD access) noexcept {
    return open_path_guard(path, PathKind::directory, access, FILE_SHARE_READ | FILE_SHARE_WRITE);
}

[[nodiscard]] UniqueHandle open_regular_guard(const fs::path& path, DWORD access,
                                              DWORD sharing) noexcept {
    return open_path_guard(path, PathKind::regular_file, access, sharing);
}

[[nodiscard]] bool is_valid_leaf(const fs::path& value) {
    if (value.empty() || value.is_absolute() || value.has_root_name() ||
        value.has_root_directory() || value.has_parent_path() || value.filename() != value ||
        value == L"." || value == L"..") {
        return false;
    }
    const std::wstring text = value.native();
    if (text.empty() || text.back() == L' ' || text.back() == L'.')
        return false;
    constexpr std::wstring_view forbidden = L"<>:\"/\\|?*";
    return std::none_of(text.begin(), text.end(), [forbidden](wchar_t character) {
        return character < 32 || forbidden.find(character) != std::wstring_view::npos;
    });
}

[[nodiscard]] bool get_module_path(fs::path& output) {
    std::vector<wchar_t> buffer(512u);
    for (;;) {
        const DWORD length =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0u)
            return false;
        if (static_cast<std::size_t>(length) < buffer.size()) {
            output = fs::path(std::wstring(buffer.data(), length)).lexically_normal();
            return output.is_absolute() && output.has_filename();
        }
        if (buffer.size() >= 32'768u)
            return false;
        buffer.resize(std::min<std::size_t>(buffer.size() * 2u, 32'768u));
    }
}

[[nodiscard]] bool path_is_strict_descendant(const fs::path& candidate, const fs::path& root) {
    const fs::path normalized_candidate = candidate.lexically_normal();
    const fs::path normalized_root = root.lexically_normal();
    if (!normalized_candidate.is_absolute() || !normalized_root.is_absolute())
        return false;

    auto candidate_iterator = normalized_candidate.begin();
    for (auto root_iterator = normalized_root.begin(); root_iterator != normalized_root.end();
         ++root_iterator, ++candidate_iterator) {
        if (candidate_iterator == normalized_candidate.end() ||
            !equal_case_insensitive(root_iterator->native(), candidate_iterator->native())) {
            return false;
        }
    }
    return candidate_iterator != normalized_candidate.end();
}

[[nodiscard]] bool contains_dot_component(const fs::path& path) {
    return std::any_of(path.begin(), path.end(), [](const fs::path& component) {
        return component == L"." || component == L"..";
    });
}

[[nodiscard]] bool resolve_extracted_path(const fs::path& root, const fs::path& supplied,
                                          fs::path& output) {
    if (supplied.empty() || contains_dot_component(supplied))
        return false;
    if (!supplied.is_absolute() && (supplied.has_root_name() || supplied.has_root_directory()))
        return false;
    fs::path candidate = supplied.is_absolute() ? supplied : root / supplied;
    candidate = candidate.lexically_normal();
    if (!path_is_strict_descendant(candidate, root))
        return false;
    output = std::move(candidate);
    return true;
}

[[nodiscard]] bool get_local_app_data(fs::path& output) noexcept {
    PWSTR value = nullptr;
    const HRESULT status =
        ::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &value);
    if (FAILED(status) || value == nullptr)
        return false;
    try {
        output = fs::path(value).lexically_normal();
    } catch (...) {
        ::CoTaskMemFree(value);
        return false;
    }
    ::CoTaskMemFree(value);
    return output.is_absolute();
}

[[nodiscard]] bool build_restrictive_descriptor(LocalAllocation& descriptor) {
    UniqueHandle token;
    HANDLE raw_token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw_token) == FALSE)
        return false;
    token = UniqueHandle(raw_token);

    DWORD required = 0u;
    (void)::GetTokenInformation(token.get(), TokenUser, nullptr, 0u, &required);
    if (required == 0u)
        return false;
    std::vector<std::uint8_t> token_storage(required);
    if (::GetTokenInformation(token.get(), TokenUser, token_storage.data(), required, &required) ==
        FALSE) {
        return false;
    }
    const auto* token_user = reinterpret_cast<const TOKEN_USER*>(token_storage.data());
    if (token_user->User.Sid == nullptr || ::IsValidSid(token_user->User.Sid) == FALSE)
        return false;

    LPWSTR sid_text = nullptr;
    if (::ConvertSidToStringSidW(token_user->User.Sid, &sid_text) == FALSE || sid_text == nullptr)
        return false;
    LocalAllocation sid_allocation(sid_text);

    const std::wstring sddl =
        L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + std::wstring(sid_text) + L")";
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                               &raw_descriptor, nullptr) == FALSE ||
        raw_descriptor == nullptr) {
        return false;
    }
    descriptor.reset(raw_descriptor);
    return true;
}

[[nodiscard]] bool apply_restrictive_dacl(HANDLE directory,
                                          PSECURITY_DESCRIPTOR descriptor) noexcept {
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    if (::GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) == FALSE ||
        present == FALSE || dacl == nullptr) {
        return false;
    }
    return ::SetSecurityInfo(directory, SE_FILE_OBJECT,
                             DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                             nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
}

[[nodiscard]] bool ensure_secure_directory(const fs::path& path,
                                           PSECURITY_DESCRIPTOR descriptor) noexcept {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    if (::CreateDirectoryW(path.c_str(), &attributes) == FALSE) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
            return false;
    }

    UniqueHandle directory =
        open_directory_guard(path, FILE_READ_ATTRIBUTES | READ_CONTROL | WRITE_DAC | SYNCHRONIZE);
    return directory && apply_restrictive_dacl(directory.get(), descriptor);
}

[[nodiscard]] bool ensure_plain_directory(const fs::path& path) noexcept {
    if (::CreateDirectoryW(path.c_str(), nullptr) == FALSE) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
            return false;
    }
    return static_cast<bool>(open_directory_guard(path, FILE_READ_ATTRIBUTES | SYNCHRONIZE));
}

void store_u32(std::array<std::uint8_t, kMarkerBytes>& bytes, std::size_t offset,
               std::uint32_t value) noexcept {
    for (std::size_t index = 0u; index < 4u; ++index)
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
}

void store_u64(std::array<std::uint8_t, kMarkerBytes>& bytes, std::size_t offset,
               std::uint64_t value) noexcept {
    for (std::size_t index = 0u; index < 8u; ++index)
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
}

[[nodiscard]] std::uint32_t load_u32(const std::array<std::uint8_t, kMarkerBytes>& bytes,
                                     std::size_t offset) noexcept {
    std::uint32_t value = 0u;
    for (std::size_t index = 0u; index < 4u; ++index)
        value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8u);
    return value;
}

[[nodiscard]] std::uint64_t load_u64(const std::array<std::uint8_t, kMarkerBytes>& bytes,
                                     std::size_t offset) noexcept {
    std::uint64_t value = 0u;
    for (std::size_t index = 0u; index < 8u; ++index)
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8u);
    return value;
}

[[nodiscard]] std::uint64_t file_time_value(const FILETIME& value) noexcept {
    ULARGE_INTEGER converted{};
    converted.LowPart = value.dwLowDateTime;
    converted.HighPart = value.dwHighDateTime;
    return converted.QuadPart;
}

[[nodiscard]] bool current_process_creation_time(std::uint64_t& output) noexcept {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (::GetProcessTimes(::GetCurrentProcess(), &creation, &exit, &kernel, &user) == FALSE)
        return false;
    output = file_time_value(creation);
    return output != 0u;
}

[[nodiscard]] std::array<std::uint8_t, kMarkerBytes>
encode_marker(const MarkerRecord& marker) noexcept {
    std::array<std::uint8_t, kMarkerBytes> bytes{};
    std::copy(kMarkerMagic.begin(), kMarkerMagic.end(), bytes.begin());
    store_u32(bytes, 16u, 1u);
    store_u32(bytes, 20u, static_cast<std::uint32_t>(bytes.size()));
    store_u32(bytes, 24u, marker.owner_pid);
    store_u32(bytes, 28u, 0u);
    store_u64(bytes, 32u, marker.owner_creation_time);
    std::copy(marker.nonce.begin(), marker.nonce.end(), bytes.begin() + 40u);
    store_u64(bytes, 56u, 0u);
    return bytes;
}

[[nodiscard]] bool decode_marker(const std::array<std::uint8_t, kMarkerBytes>& bytes,
                                 MarkerRecord& marker) noexcept {
    if (!std::equal(kMarkerMagic.begin(), kMarkerMagic.end(), bytes.begin()) ||
        load_u32(bytes, 16u) != 1u ||
        load_u32(bytes, 20u) != static_cast<std::uint32_t>(bytes.size()) ||
        load_u32(bytes, 28u) != 0u || load_u64(bytes, 56u) != 0u) {
        return false;
    }
    MarkerRecord candidate{};
    candidate.owner_pid = load_u32(bytes, 24u);
    candidate.owner_creation_time = load_u64(bytes, 32u);
    std::copy_n(bytes.begin() + 40u, candidate.nonce.size(), candidate.nonce.begin());
    if (candidate.owner_pid == 0u || candidate.owner_creation_time == 0u)
        return false;
    marker = candidate;
    return true;
}

[[nodiscard]] wchar_t hex_digit(std::uint8_t value) noexcept {
    return static_cast<wchar_t>(value < 10u ? L'0' + value : L'a' + (value - 10u));
}

[[nodiscard]] std::wstring generation_name(const MarkerRecord& marker) {
    std::wstring result = L"gen-";
    result.reserve(45u);
    for (int shift = 28; shift >= 0; shift -= 4)
        result.push_back(hex_digit(static_cast<std::uint8_t>((marker.owner_pid >> shift) & 0x0fu)));
    result.push_back(L'-');
    for (const std::uint8_t value : marker.nonce) {
        result.push_back(hex_digit(static_cast<std::uint8_t>(value >> 4u)));
        result.push_back(hex_digit(static_cast<std::uint8_t>(value & 0x0fu)));
    }
    return result;
}

[[nodiscard]] bool parse_hex_digit(wchar_t value, std::uint8_t& output) noexcept {
    if (value >= L'0' && value <= L'9') {
        output = static_cast<std::uint8_t>(value - L'0');
        return true;
    }
    if (value >= L'a' && value <= L'f') {
        output = static_cast<std::uint8_t>(value - L'a' + 10);
        return true;
    }
    return false;
}

[[nodiscard]] bool parse_generation_name(std::wstring_view value, DWORD& owner_pid,
                                         std::array<std::uint8_t, kNonceBytes>& nonce) noexcept {
    if (value.size() != 45u || value.substr(0u, 4u) != L"gen-" || value[12u] != L'-')
        return false;
    std::uint32_t pid = 0u;
    for (std::size_t index = 4u; index < 12u; ++index) {
        std::uint8_t digit = 0u;
        if (!parse_hex_digit(value[index], digit))
            return false;
        pid = (pid << 4u) | digit;
    }
    if (pid == 0u)
        return false;
    for (std::size_t index = 0u; index < nonce.size(); ++index) {
        std::uint8_t high = 0u;
        std::uint8_t low = 0u;
        if (!parse_hex_digit(value[13u + index * 2u], high) ||
            !parse_hex_digit(value[14u + index * 2u], low)) {
            return false;
        }
        nonce[index] = static_cast<std::uint8_t>((high << 4u) | low);
    }
    owner_pid = pid;
    return true;
}

[[nodiscard]] bool marker_matches_directory(const fs::path& generation,
                                            const MarkerRecord& marker) {
    DWORD parsed_pid = 0u;
    std::array<std::uint8_t, kNonceBytes> parsed_nonce{};
    const std::wstring leaf = generation.filename().native();
    return parse_generation_name(leaf, parsed_pid, parsed_nonce) &&
           parsed_pid == marker.owner_pid && parsed_nonce == marker.nonce &&
           leaf == generation_name(marker);
}

[[nodiscard]] bool write_owner_marker(const fs::path& generation, PSECURITY_DESCRIPTOR descriptor,
                                      const MarkerRecord& marker) {
    const fs::path marker_path = generation / kOwnerMarkerLeaf;
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    UniqueHandle file(::CreateFileW(
        marker_path.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0, &attributes, CREATE_NEW,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr));
    if (!file)
        return false;
    const auto bytes = encode_marker(marker);
    DWORD written = 0u;
    const bool write_ok = ::WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()),
                                      &written, nullptr) != FALSE &&
                          written == static_cast<DWORD>(bytes.size()) &&
                          ::FlushFileBuffers(file.get()) != FALSE;
    const bool close_ok = file.close();
    if (!write_ok || !close_ok) {
        (void)::DeleteFileW(marker_path.c_str());
        return false;
    }
    return true;
}

[[nodiscard]] bool read_owner_marker(const fs::path& generation, MarkerRecord& marker) {
    const fs::path marker_path = generation / kOwnerMarkerLeaf;
    WIN32_FIND_DATAW marker_data{};
    UniqueFindHandle marker_find(::FindFirstFileW(marker_path.c_str(), &marker_data));
    if (!marker_find)
        return false;
    if (std::wstring_view(marker_data.cFileName) != kOwnerMarkerLeaf ||
        (marker_data.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) != 0u) {
        (void)marker_find.close();
        ::SetLastError(ERROR_INVALID_DATA);
        return false;
    }
    if (!marker_find.close())
        return false;
    UniqueHandle file =
        open_regular_guard(marker_path, GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ);
    if (!file)
        return false;
    FILE_STANDARD_INFO standard{};
    if (::GetFileInformationByHandleEx(file.get(), FileStandardInfo, &standard, sizeof(standard)) ==
        FALSE) {
        const DWORD error = ::GetLastError();
        (void)file.close();
        ::SetLastError(error);
        return false;
    }
    if (standard.EndOfFile.QuadPart != static_cast<LONGLONG>(kMarkerBytes)) {
        (void)file.close();
        ::SetLastError(ERROR_INVALID_DATA);
        return false;
    }
    std::array<std::uint8_t, kMarkerBytes> bytes{};
    DWORD read = 0u;
    if (::ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ==
        FALSE) {
        const DWORD error = ::GetLastError();
        (void)file.close();
        ::SetLastError(error);
        return false;
    }
    if (read != static_cast<DWORD>(bytes.size()) || !decode_marker(bytes, marker) ||
        !marker_matches_directory(generation, marker)) {
        (void)file.close();
        ::SetLastError(ERROR_INVALID_DATA);
        return false;
    }
    return true;
}

[[nodiscard]] OwnerState owner_process_state(const MarkerRecord& marker) noexcept {
    UniqueHandle process(
        ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, marker.owner_pid));
    if (!process) {
        return ::GetLastError() == ERROR_INVALID_PARAMETER ? OwnerState::dead : OwnerState::unknown;
    }
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (::GetProcessTimes(process.get(), &creation, &exit, &kernel, &user) == FALSE)
        return OwnerState::unknown;
    if (file_time_value(creation) != marker.owner_creation_time)
        return OwnerState::dead;
    const DWORD wait = ::WaitForSingleObject(process.get(), 0u);
    if (wait == WAIT_OBJECT_0)
        return OwnerState::dead;
    if (wait == WAIT_TIMEOUT)
        return OwnerState::active;
    return OwnerState::unknown;
}

[[nodiscard]] bool create_generation_directory(const fs::path& runtime_root,
                                               PSECURITY_DESCRIPTOR descriptor,
                                               fs::path& generation, MarkerRecord& marker) {
    std::uint64_t process_creation_time = 0u;
    if (!current_process_creation_time(process_creation_time))
        return false;

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;

    for (std::size_t attempt = 0u; attempt < 16u; ++attempt) {
        MarkerRecord candidate{};
        candidate.owner_pid = ::GetCurrentProcessId();
        candidate.owner_creation_time = process_creation_time;
        if (::BCryptGenRandom(nullptr, candidate.nonce.data(),
                              static_cast<ULONG>(candidate.nonce.size()),
                              BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
            return false;
        }
        const fs::path candidate_path = runtime_root / generation_name(candidate);
        if (::CreateDirectoryW(candidate_path.c_str(), &attributes) == FALSE) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
                continue;
            return false;
        }
        UniqueHandle directory = open_directory_guard(
            candidate_path, FILE_READ_ATTRIBUTES | READ_CONTROL | WRITE_DAC | SYNCHRONIZE);
        if (!directory || !apply_restrictive_dacl(directory.get(), descriptor)) {
            (void)directory.close();
            (void)::RemoveDirectoryW(candidate_path.c_str());
            return false;
        }
        if (!write_owner_marker(candidate_path, descriptor, candidate)) {
            (void)directory.close();
            (void)::RemoveDirectoryW(candidate_path.c_str());
            return false;
        }
        generation = candidate_path;
        marker = candidate;
        return true;
    }
    return false;
}

[[nodiscard]] TreeScanResult scan_generation_tree(const fs::path& directory, std::size_t depth,
                                                  std::size_t& entries) {
    if (depth > kMaximumTreeDepth)
        return TreeScanResult::unsafe;
    UniqueHandle directory_guard =
        open_directory_guard(directory, FILE_READ_ATTRIBUTES | SYNCHRONIZE);
    if (!directory_guard) {
        return is_locked_error(::GetLastError()) ? TreeScanResult::retained
                                                 : TreeScanResult::unsafe;
    }

    const fs::path pattern = directory / L"*";
    WIN32_FIND_DATAW data{};
    UniqueFindHandle find(::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                             FindExSearchNameMatch, nullptr,
                                             FIND_FIRST_EX_LARGE_FETCH));
    if (!find) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_FILE_NOT_FOUND)
            return TreeScanResult::safe;
        return is_locked_error(error) ? TreeScanResult::retained : TreeScanResult::unsafe;
    }

    for (;;) {
        const std::wstring_view name(data.cFileName);
        if (name != L"." && name != L"..") {
            if (++entries > kMaximumTreeEntries ||
                (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
                return TreeScanResult::unsafe;
            }
            const fs::path child = directory / data.cFileName;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
                const TreeScanResult child_result =
                    scan_generation_tree(child, depth + 1u, entries);
                if (child_result != TreeScanResult::safe)
                    return child_result;
            } else {
                UniqueHandle file =
                    open_regular_guard(child, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
                if (!file) {
                    return is_locked_error(::GetLastError()) ? TreeScanResult::retained
                                                             : TreeScanResult::unsafe;
                }
            }
        }
        if (::FindNextFileW(find.get(), &data) == FALSE) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_NO_MORE_FILES)
                return TreeScanResult::safe;
            return is_locked_error(error) ? TreeScanResult::retained : TreeScanResult::unsafe;
        }
    }
}

[[nodiscard]] FileDeleteResult delete_regular_file(const fs::path& path,
                                                   bool allow_locked) noexcept {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = ::GetLastError();
        return is_missing_error(error) ? FileDeleteResult::absent : FileDeleteResult::failed;
    }
    if ((attributes &
         (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)) != 0u) {
        return FileDeleteResult::unsafe;
    }

    UniqueHandle file = open_regular_guard(
        path, DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE, 0u);
    if (!file) {
        const DWORD error = ::GetLastError();
        const DWORD current_attributes = ::GetFileAttributesW(path.c_str());
        if (current_attributes == INVALID_FILE_ATTRIBUTES && is_missing_error(::GetLastError())) {
            return FileDeleteResult::absent;
        }
        if (current_attributes != INVALID_FILE_ATTRIBUTES &&
            (current_attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY |
                                   FILE_ATTRIBUTE_DEVICE)) != 0u) {
            return FileDeleteResult::unsafe;
        }
        if (allow_locked && is_locked_error(error))
            return FileDeleteResult::locked;
        return FileDeleteResult::failed;
    }

    FILE_BASIC_INFO basic{};
    if (::GetFileInformationByHandleEx(file.get(), FileBasicInfo, &basic, sizeof(basic)) == FALSE) {
        const DWORD error = ::GetLastError();
        return allow_locked && is_locked_error(error) ? FileDeleteResult::locked
                                                      : FileDeleteResult::failed;
    }
    if ((basic.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0u) {
        basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
        if (basic.FileAttributes == 0u)
            basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        if (::SetFileInformationByHandle(file.get(), FileBasicInfo, &basic, sizeof(basic)) ==
            FALSE) {
            const DWORD error = ::GetLastError();
            return allow_locked && is_locked_error(error) ? FileDeleteResult::locked
                                                          : FileDeleteResult::failed;
        }
    }

    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    if (::SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition,
                                     sizeof(disposition)) == FALSE) {
        const DWORD error = ::GetLastError();
        return allow_locked && is_locked_error(error) ? FileDeleteResult::locked
                                                      : FileDeleteResult::failed;
    }
    return file.close() ? FileDeleteResult::deleted : FileDeleteResult::failed;
}

[[nodiscard]] bool remove_helper_transaction_lock(const fs::path& helper_root) noexcept {
    const ULONGLONG deadline = ::GetTickCount64() + 1'000u;
    for (;;) {
        const FileDeleteResult result =
            delete_regular_file(helper_root / kHelperTransactionLockLeaf, true);
        if (result == FileDeleteResult::deleted || result == FileDeleteResult::absent)
            return true;
        if (result == FileDeleteResult::failed || result == FileDeleteResult::unsafe)
            return false;
        if (::GetTickCount64() >= deadline)
            return false;
        ::Sleep(kCleanupRetryPollMs);
    }
}

[[nodiscard]] TreeDeleteResult delete_generation_tree(const fs::path& directory, bool is_root,
                                                      std::size_t depth, std::size_t& entries,
                                                      const MarkerRecord* root_marker,
                                                      PSECURITY_DESCRIPTOR descriptor) {
    if (depth > kMaximumTreeDepth)
        return TreeDeleteResult::failed;
    UniqueHandle directory_guard =
        open_path_guard(directory, PathKind::directory, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE);
    if (!directory_guard) {
        const DWORD error = ::GetLastError();
        if (path_is_absent(directory))
            return TreeDeleteResult::deleted;
        return is_locked_error(error) ? TreeDeleteResult::retained : TreeDeleteResult::failed;
    }

    const fs::path pattern = directory / L"*";
    WIN32_FIND_DATAW data{};
    UniqueFindHandle find(::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                             FindExSearchNameMatch, nullptr,
                                             FIND_FIRST_EX_LARGE_FETCH));
    bool retained = false;
    if (find) {
        for (;;) {
            const std::wstring_view name(data.cFileName);
            if (name != L"." && name != L".." && !(is_root && name == kOwnerMarkerLeaf)) {
                if (++entries > kMaximumTreeEntries ||
                    (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
                    return TreeDeleteResult::failed;
                const fs::path child = directory / data.cFileName;
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
                    const TreeDeleteResult child_result =
                        delete_generation_tree(child, false, depth + 1u, entries, nullptr, nullptr);
                    if (child_result == TreeDeleteResult::failed)
                        return TreeDeleteResult::failed;
                    retained = retained || child_result == TreeDeleteResult::retained;
                } else {
                    const FileDeleteResult child_result = delete_regular_file(child, true);
                    if (child_result == FileDeleteResult::locked) {
                        retained = true;
                    } else if (child_result != FileDeleteResult::deleted &&
                               child_result != FileDeleteResult::absent) {
                        return TreeDeleteResult::failed;
                    }
                }
            }
            if (::FindNextFileW(find.get(), &data) == FALSE) {
                const DWORD error = ::GetLastError();
                if (error != ERROR_NO_MORE_FILES) {
                    return is_locked_error(error) ? TreeDeleteResult::retained
                                                  : TreeDeleteResult::failed;
                }
                break;
            }
        }
    } else {
        const DWORD error = ::GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) {
            return is_locked_error(error) ? TreeDeleteResult::retained : TreeDeleteResult::failed;
        }
    }

    if (!find.close()) {
        return is_locked_error(::GetLastError()) ? TreeDeleteResult::retained
                                                 : TreeDeleteResult::failed;
    }

    if (retained)
        return TreeDeleteResult::retained;

    if (is_root) {
        // The marker may legitimately be absent (orphaned generation whose
        // creator crashed between CreateDirectoryW and write_owner_marker,
        // reclaimed by prune_stale_generations); accept absent as clean.
        const FileDeleteResult marker_result =
            delete_regular_file(directory / kOwnerMarkerLeaf, true);
        if (marker_result == FileDeleteResult::locked)
            return TreeDeleteResult::retained;
        if (marker_result != FileDeleteResult::deleted &&
            marker_result != FileDeleteResult::absent)
            return TreeDeleteResult::failed;
    }

    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    if (::SetFileInformationByHandle(directory_guard.get(), FileDispositionInfo, &disposition,
                                     sizeof(disposition)) == FALSE) {
        const DWORD error = ::GetLastError();
        if (is_root && root_marker != nullptr &&
            !write_owner_marker(directory, descriptor, *root_marker)) {
            return TreeDeleteResult::failed;
        }
        return is_locked_error(error) || error == ERROR_DIR_NOT_EMPTY ? TreeDeleteResult::retained
                                                                      : TreeDeleteResult::failed;
    }
    return directory_guard.close() ? TreeDeleteResult::deleted : TreeDeleteResult::failed;
}

[[nodiscard]] TreeDeleteResult safe_delete_generation(const fs::path& generation,
                                                      const MarkerRecord* expected,
                                                      bool require_dead_owner,
                                                      PSECURITY_DESCRIPTOR descriptor) {
    if (path_is_absent(generation))
        return TreeDeleteResult::deleted;
    UniqueHandle directory = open_directory_guard(generation, FILE_READ_ATTRIBUTES | SYNCHRONIZE);
    if (!directory) {
        const DWORD error = ::GetLastError();
        return is_locked_error(error) ? TreeDeleteResult::retained : TreeDeleteResult::failed;
    }
    MarkerRecord actual{};
    if (!read_owner_marker(generation, actual)) {
        return is_locked_error(::GetLastError()) ? TreeDeleteResult::retained
                                                 : TreeDeleteResult::failed;
    }
    if (expected != nullptr && (actual.owner_pid != expected->owner_pid ||
                                actual.owner_creation_time != expected->owner_creation_time ||
                                actual.nonce != expected->nonce)) {
        return TreeDeleteResult::failed;
    }
    if (require_dead_owner && owner_process_state(actual) != OwnerState::dead)
        return TreeDeleteResult::retained;
    std::size_t entries = 0u;
    const TreeScanResult scan_result = scan_generation_tree(generation, 0u, entries);
    if (scan_result == TreeScanResult::retained)
        return TreeDeleteResult::retained;
    if (scan_result == TreeScanResult::unsafe)
        return TreeDeleteResult::failed;
    if (!directory.close())
        return TreeDeleteResult::failed;
    std::size_t deleted_entries = 0u;
    return delete_generation_tree(generation, true, 0u, deleted_entries, &actual, descriptor);
}

void prune_stale_generations(const fs::path& runtime_root,
                             PSECURITY_DESCRIPTOR descriptor) noexcept {
    try {
        UniqueHandle root = open_directory_guard(runtime_root, FILE_READ_ATTRIBUTES | SYNCHRONIZE);
        if (!root)
            return;
        const fs::path pattern = runtime_root / L"*";
        WIN32_FIND_DATAW data{};
        UniqueFindHandle find(::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                                 FindExSearchNameMatch, nullptr,
                                                 FIND_FIRST_EX_LARGE_FETCH));
        if (!find)
            return;
        std::size_t directories = 0u;
        std::size_t entries = 0u;
        for (;;) {
            const std::wstring_view name(data.cFileName);
            if (name != L"." && name != L".." && ++entries > kMaximumPruneEntries)
                return;
            if (name != L"." && name != L".." &&
                (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
                if (++directories > kMaximumPruneDirectories)
                    return;
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0u) {
                    DWORD parsed_pid = 0u;
                    std::array<std::uint8_t, kNonceBytes> parsed_nonce{};
                    if (parse_generation_name(name, parsed_pid, parsed_nonce)) {
                        const fs::path candidate = runtime_root / data.cFileName;
                        MarkerRecord marker{};
                        if (read_owner_marker(candidate, marker)) {
                            if (marker.owner_pid == parsed_pid && marker.nonce == parsed_nonce &&
                                owner_process_state(marker) == OwnerState::dead) {
                                (void)safe_delete_generation(candidate, &marker, true, descriptor);
                            }
                        } else if (path_is_absent(candidate / kOwnerMarkerLeaf) &&
                                   parsed_pid != ::GetCurrentProcessId()) {
                            // Marker-less orphan: the creator died between
                            // CreateDirectoryW and write_owner_marker.  The
                            // dir name itself proves pid+nonce.  Reclaim only
                            // when the encoded pid is provably dead
                            // (ERROR_INVALID_PARAMETER — an ACCESS_DENIED or
                            // live open means a live/reused pid, keep it)
                            // and the directory is older than the in-flight
                            // create margin.
                            UniqueHandle probe(
                                ::OpenProcess(SYNCHRONIZE, FALSE, parsed_pid));
                            const bool pid_dead =
                                !probe && ::GetLastError() == ERROR_INVALID_PARAMETER;
                            FILETIME now{};
                            ::GetSystemTimeAsFileTime(&now);
                            const bool old_enough =
                                file_time_value(now) >=
                                    file_time_value(data.ftCreationTime) +
                                        kOrphanGenerationAgeMargin;
                            if (pid_dead && old_enough) {
                                std::size_t tree_entries = 0u;
                                if (scan_generation_tree(candidate, 0u, tree_entries) ==
                                    TreeScanResult::safe) {
                                    std::size_t deleted = 0u;
                                    (void)delete_generation_tree(candidate, true, 0u, deleted,
                                                                 nullptr, descriptor);
                                }
                            }
                        }
                    }
                }
            }
            if (::FindNextFileW(find.get(), &data) == FALSE)
                return;
        }
    } catch (...) {
    }
}

class RuntimeCleanup {
  public:
    RuntimeCleanup(fs::path generation, MarkerRecord marker, PSECURITY_DESCRIPTOR descriptor)
        : generation_(std::move(generation)), marker_(marker), descriptor_(descriptor) {}
    RuntimeCleanup(const RuntimeCleanup&) = delete;
    RuntimeCleanup& operator=(const RuntimeCleanup&) = delete;

    void set_helpers(std::vector<fs::path> helpers) {
        helpers_ = std::move(helpers);
    }

    ~RuntimeCleanup() {
        if (!attempted_) {
            try {
                (void)cleanup();
            } catch (...) {
            }
        }
    }

    [[nodiscard]] TreeDeleteResult cleanup() {
        attempted_ = true;
        const ULONGLONG deadline = ::GetTickCount64() + kCleanupRetryTimeoutMs;
        for (;;) {
            bool helper_retained = false;
            for (const fs::path& helper_file : helpers_) {
                const FileDeleteResult result = delete_regular_file(helper_file, true);
                if (result == FileDeleteResult::failed || result == FileDeleteResult::unsafe)
                    return TreeDeleteResult::failed;
                helper_retained = helper_retained || result == FileDeleteResult::locked;
            }
            const TreeDeleteResult generation_result =
                safe_delete_generation(generation_, &marker_, false, descriptor_);
            if (generation_result == TreeDeleteResult::failed)
                return TreeDeleteResult::failed;
            if (!helper_retained && generation_result == TreeDeleteResult::deleted)
                return TreeDeleteResult::deleted;
            if (::GetTickCount64() >= deadline)
                return TreeDeleteResult::retained;
            ::Sleep(kCleanupRetryPollMs);
        }
    }

  private:
    fs::path generation_;
    MarkerRecord marker_{};
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    std::vector<fs::path> helpers_;
    bool attempted_ = false;
};

[[nodiscard]] bool helper_path_already_present(const std::vector<fs::path>& paths,
                                               const fs::path& candidate) {
    return std::any_of(paths.begin(), paths.end(), [&candidate](const fs::path& current) {
        const fs::path normalized_current = current.lexically_normal();
        const fs::path normalized_candidate = candidate.lexically_normal();
        auto left = normalized_current.begin();
        auto right = normalized_candidate.begin();
        for (; left != normalized_current.end() && right != normalized_candidate.end();
             ++left, ++right) {
            if (!equal_case_insensitive(left->native(), right->native()))
                return false;
        }
        return left == normalized_current.end() && right == normalized_candidate.end();
    });
}

template <std::size_t Size>
[[nodiscard]] bool exact_leaf_set(const std::vector<fs::path>& paths,
                                  const std::array<std::wstring_view, Size>& expected) {
    if (paths.size() != Size)
        return false;
    std::array<bool, Size> seen{};
    for (const fs::path& path : paths) {
        const std::wstring leaf = path.filename().native();
        bool matched = false;
        for (std::size_t index = 0u; index < expected.size(); ++index) {
            if (!seen[index] && leaf == expected[index]) {
                seen[index] = true;
                matched = true;
                break;
            }
        }
        if (!matched)
            return false;
    }
    return std::all_of(seen.begin(), seen.end(), [](bool value) { return value; });
}

[[nodiscard]] bool exact_extraction_layout(const sao::shell::artifact::ExtractResult& extraction) {
    return extraction.payload_path.filename().native() == sao::runtime::kPayloadExecutable &&
           exact_leaf_set(extraction.session_files, kExpectedSessionLeaves) &&
           exact_leaf_set(extraction.helper_files, kExpectedHelperLeaves);
}

[[nodiscard]] bool validate_helper_paths(const fs::path& helper_root,
                                         const std::vector<fs::path>& raw_helpers,
                                         std::vector<fs::path>& output,
                                         std::vector<UniqueHandle>& guards) {
    output.clear();
    guards.clear();
    output.reserve(raw_helpers.size());
    guards.reserve(raw_helpers.size());
    bool valid = true;
    for (const fs::path& raw_helper : raw_helpers) {
        fs::path helper;
        if (!resolve_extracted_path(helper_root, raw_helper, helper) ||
            helper_path_already_present(output, helper)) {
            valid = false;
            continue;
        }
        UniqueHandle helper_file =
            open_regular_guard(helper, GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ);
        if (!helper_file) {
            valid = false;
            continue;
        }
        output.push_back(std::move(helper));
        guards.push_back(std::move(helper_file));
    }
    return valid && output.size() == raw_helpers.size() && guards.size() == raw_helpers.size();
}

[[nodiscard]] bool validate_session_paths(const fs::path& generation,
                                          const std::vector<fs::path>& raw_paths,
                                          std::vector<fs::path>& output,
                                          std::vector<UniqueHandle>& guards) {
    output.clear();
    guards.clear();
    output.reserve(raw_paths.size());
    guards.reserve(raw_paths.size());
    for (const fs::path& raw_path : raw_paths) {
        fs::path path;
        if (!resolve_extracted_path(generation, raw_path, path) ||
            helper_path_already_present(output, path)) {
            return false;
        }
        UniqueHandle file =
            open_regular_guard(path, GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ);
        if (!file)
            return false;
        output.push_back(std::move(path));
        guards.push_back(std::move(file));
    }
    return !output.empty();
}

[[nodiscard]] bool validate_payload_path(const fs::path& generation, const fs::path& raw_payload,
                                         const std::vector<fs::path>& session_paths,
                                         fs::path& output) {
    output.clear();
    if (!resolve_extracted_path(generation, raw_payload, output))
        return false;
    return output.is_absolute() && helper_path_already_present(session_paths, output);
}

[[nodiscard]] bool parse_environment(std::vector<EnvironmentEntry>& entries) {
    EnvironmentStrings environment;
    if (environment.get() == nullptr)
        return false;
    for (const wchar_t* cursor = environment.get(); *cursor != L'\0';) {
        const std::size_t length = std::wcslen(cursor);
        std::wstring entry(cursor, length);
        cursor += length + 1u;
        const std::size_t separator = entry.find(L'=', entry.starts_with(L'=') ? 1u : 0u);
        if (separator == std::wstring::npos || separator == 0u)
            return false;
        entries.push_back(
            EnvironmentEntry{entry.substr(0u, separator), entry.substr(separator + 1u)});
    }
    return true;
}

[[nodiscard]] std::wstring environment_value(const std::vector<EnvironmentEntry>& entries,
                                             std::wstring_view name) {
    for (const EnvironmentEntry& entry : entries) {
        if (equal_case_insensitive(entry.name, name))
            return entry.value;
    }
    return {};
}

void set_environment_value(std::vector<EnvironmentEntry>& entries, std::wstring name,
                           std::wstring value) {
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&name](const auto& entry) {
                                     return equal_case_insensitive(entry.name, name);
                                 }),
                  entries.end());
    entries.push_back(EnvironmentEntry{std::move(name), std::move(value)});
}

[[nodiscard]] bool environment_entry_less(const EnvironmentEntry& left,
                                          const EnvironmentEntry& right) noexcept {
    if (left.name.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        right.name.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return left.name < right.name;
    }
    const int left_length = static_cast<int>(left.name.size());
    const int right_length = static_cast<int>(right.name.size());
    const int comparison = ::CompareStringOrdinal(left.name.data(), left_length, right.name.data(),
                                                  right_length, TRUE);
    if (comparison == CSTR_LESS_THAN)
        return true;
    if (comparison == CSTR_GREATER_THAN)
        return false;
    return left.name < right.name;
}

[[nodiscard]] bool build_environment_block(const fs::path& install_root, const fs::path& generation,
                                           const fs::path& helper_root,
                                           std::vector<wchar_t>& block) {
    const std::wstring install_value = install_root.native();
    const std::wstring generation_value = generation.native();
    const std::wstring helper_value = helper_root.native();
    if (install_value.empty() || generation_value.empty() || helper_value.empty() ||
        generation_value.find(L';') != std::wstring::npos ||
        helper_value.find(L';') != std::wstring::npos) {
        return false;
    }

    std::vector<EnvironmentEntry> entries;
    if (!parse_environment(entries))
        return false;
    const std::wstring inherited_path = environment_value(entries, L"PATH");
    std::wstring child_path = helper_value;
    child_path.push_back(L';');
    child_path.append(generation_value);
    if (!inherited_path.empty()) {
        child_path.push_back(L';');
        child_path.append(inherited_path);
    }
    set_environment_value(entries, L"SAO_INSTALL_ROOT", install_value);
    set_environment_value(entries, L"SAO_RUNTIME_STAGE", generation_value);
    set_environment_value(entries, L"SAO_BOOTSTRAP_PID", std::to_wstring(GetCurrentProcessId()));
    set_environment_value(entries, L"PATH", std::move(child_path));
    std::sort(entries.begin(), entries.end(), environment_entry_less);

    std::size_t characters = 1u;
    for (const EnvironmentEntry& entry : entries) {
        if (entry.name.empty() || entry.name.find(L'\0') != std::wstring::npos ||
            entry.value.find(L'\0') != std::wstring::npos ||
            entry.name.size() > kMaximumEnvironmentCharacters - 2u ||
            entry.value.size() > kMaximumEnvironmentCharacters - entry.name.size() - 2u) {
            return false;
        }
        const std::size_t entry_characters = entry.name.size() + entry.value.size() + 2u;
        if (characters > kMaximumEnvironmentCharacters - entry_characters)
            return false;
        characters += entry_characters;
    }
    if (characters + (entries.empty() ? 1u : 0u) > kMaximumEnvironmentCharacters)
        return false;

    block.clear();
    block.reserve(characters + 1u);
    for (const EnvironmentEntry& entry : entries) {
        block.insert(block.end(), entry.name.begin(), entry.name.end());
        block.push_back(L'=');
        block.insert(block.end(), entry.value.begin(), entry.value.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (entries.empty())
        block.push_back(L'\0');
    return true;
}

[[nodiscard]] std::wstring quote_argument(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size() + 2u);
    result.push_back(L'"');
    std::size_t backslashes = 0u;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            result.append(backslashes * 2u + 1u, L'\\');
            result.push_back(L'"');
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

[[nodiscard]] bool build_child_command_line(const fs::path& payload,
                                            std::vector<wchar_t>& command_line) {
    int argument_count = 0;
    LPWSTR* raw_arguments = ::CommandLineToArgvW(::GetCommandLineW(), &argument_count);
    if (raw_arguments == nullptr || argument_count < 1)
        return false;
    LocalAllocation arguments(raw_arguments);

    std::wstring command = quote_argument(payload.native());
    for (int index = 1; index < argument_count; ++index) {
        command.push_back(L' ');
        command.append(quote_argument(raw_arguments[index]));
        if (command.size() + 1u > kMaximumCommandLineCharacters)
            return false;
    }
    command_line.assign(command.begin(), command.end());
    command_line.push_back(L'\0');
    return command_line.size() <= kMaximumCommandLineCharacters;
}

[[nodiscard]] bool terminate_failed_child(HANDLE process, HANDLE job) noexcept {
    if (job != nullptr)
        (void)::TerminateJobObject(job, kFailureLaunch);
    else
        (void)::TerminateProcess(process, kFailureLaunch);
    return ::WaitForSingleObject(process, 5'000u) == WAIT_OBJECT_0;
}

[[nodiscard]] bool launch_payload(const fs::path& payload, const fs::path& install_root,
                                  std::vector<wchar_t>& command_line,
                                  std::vector<wchar_t>& environment, DWORD& exit_code,
                                  bool& wait_failed) noexcept {
    wait_failed = false;
    UniqueHandle job(::CreateJobObjectW(nullptr, nullptr));
    if (!job)
        return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    if (::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits,
                                  sizeof(limits)) == FALSE) {
        return false;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    DWORD creation_flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
    // Temporary bring-up diagnostic: SAO_BOOTSTRAP_INHERIT_STDIO lets the
    // payload inherit this console's std handles so its trace output is
    // visible when launched from a shell.
    if (::GetEnvironmentVariableW(L"SAO_BOOTSTRAP_INHERIT_STDIO", nullptr, 0u) != 0u) {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    }
    PROCESS_INFORMATION process_information{};
    if (::CreateProcessW(payload.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                         creation_flags, environment.data(),
                         install_root.c_str(), &startup, &process_information) == FALSE) {
        return false;
    }

    UniqueHandle process(process_information.hProcess);
    UniqueHandle thread(process_information.hThread);
    if (::AssignProcessToJobObject(job.get(), process.get()) == FALSE) {
        (void)terminate_failed_child(process.get(), nullptr);
        return false;
    }
    if (::ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        (void)terminate_failed_child(process.get(), job.get());
        return false;
    }
    if (!thread.close()) {
        (void)terminate_failed_child(process.get(), job.get());
        return false;
    }

    const DWORD wait = ::WaitForSingleObject(process.get(), INFINITE);
    if (wait != WAIT_OBJECT_0) {
        wait_failed = true;
        (void)terminate_failed_child(process.get(), job.get());
        return false;
    }
    if (::GetExitCodeProcess(process.get(), &exit_code) == FALSE) {
        wait_failed = true;
        return false;
    }
    if (!process.close()) {
        wait_failed = true;
        return false;
    }
    if (!job.close()) {
        wait_failed = true;
        return false;
    }
    return true;
}

[[nodiscard]] BootstrapOutcome run_bootstrap() {
    fs::path module_path;
    if (!get_module_path(module_path))
        return failure(kFailureInstall, L"SaoAuto could not locate its installation.");
    const fs::path install_root = module_path.parent_path().lexically_normal();
    const fs::path bundle_leaf(sao::runtime::kBundleLeaf);
    const fs::path product_leaf(sao::runtime::kProductId);
    if (!install_root.is_absolute() || !is_valid_leaf(bundle_leaf) ||
        !is_valid_leaf(product_leaf)) {
        return failure(kFailureInstall, L"SaoAuto installation paths are invalid.");
    }

    const fs::path install_runtime = install_root / kRuntimeLeaf;
    const fs::path bundle_path = install_runtime / bundle_leaf;
    const fs::path helper_root = install_runtime / kHelperLeaf;

    UniqueHandle module_guard = open_regular_guard(module_path, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                                   FILE_SHARE_READ | FILE_SHARE_DELETE);
    UniqueHandle install_guard =
        open_directory_guard(install_root, FILE_READ_ATTRIBUTES | SYNCHRONIZE);
    UniqueHandle runtime_guard =
        open_directory_guard(install_runtime, FILE_READ_ATTRIBUTES | SYNCHRONIZE);
    UniqueHandle bundle_guard =
        open_regular_guard(bundle_path, GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ);
    if (!module_guard || !install_guard || !runtime_guard || !bundle_guard ||
        !ensure_plain_directory(helper_root)) {
        return failure(kFailureInstall, L"SaoAuto runtime paths are unavailable.");
    }
    UniqueHandle helper_guard =
        open_directory_guard(helper_root, FILE_READ_ATTRIBUTES | SYNCHRONIZE);
    if (!helper_guard)
        return failure(kFailureInstall, L"SaoAuto runtime paths are unavailable.");

    fs::path local_app_data;
    LocalAllocation security_descriptor;
    if (!get_local_app_data(local_app_data) || !build_restrictive_descriptor(security_descriptor)) {
        return failure(kFailureStorage, L"SaoAuto runtime storage could not be secured.");
    }
    UniqueHandle local_app_data_guard =
        open_directory_guard(local_app_data, FILE_READ_ATTRIBUTES | SYNCHRONIZE);
    if (!local_app_data_guard)
        return failure(kFailureStorage, L"SaoAuto runtime storage could not be secured.");
    const fs::path product_root = local_app_data / product_leaf;
    const fs::path user_runtime = product_root / kRuntimeLeaf;
    PSECURITY_DESCRIPTOR descriptor = static_cast<PSECURITY_DESCRIPTOR>(security_descriptor.get());
    if (!ensure_secure_directory(product_root, descriptor)) {
        return failure(kFailureStorage, L"SaoAuto runtime storage could not be secured.");
    }
    UniqueHandle product_root_guard =
        open_directory_guard(product_root, FILE_READ_ATTRIBUTES | SYNCHRONIZE);
    if (!product_root_guard || !ensure_secure_directory(user_runtime, descriptor))
        return failure(kFailureStorage, L"SaoAuto runtime storage could not be secured.");
    UniqueHandle user_runtime_guard =
        open_directory_guard(user_runtime, FILE_READ_ATTRIBUTES | SYNCHRONIZE);
    if (!user_runtime_guard)
        return failure(kFailureStorage, L"SaoAuto runtime storage could not be secured.");

    prune_stale_generations(user_runtime, descriptor);

    fs::path generation;
    MarkerRecord marker{};
    if (!create_generation_directory(user_runtime, descriptor, generation, marker))
        return failure(kFailureGeneration, L"SaoAuto runtime stage could not be created.");
    RuntimeCleanup cleanup(generation, marker, descriptor);
    UniqueHandle generation_guard =
        open_directory_guard(generation, FILE_READ_ATTRIBUTES | SYNCHRONIZE);
    if (!generation_guard)
        return failure(kFailureGeneration, L"SaoAuto runtime stage could not be created.");

    auto key = sao::runtime::bundle_key();
    static_assert(std::tuple_size_v<decltype(key)> == 32u);
    KeyWiper key_wiper(key);
    std::wstring artifact_error;
    if (!sao::shell::artifact::verify_bundle(bundle_path, key, artifact_error))
        return failure(kFailureVerification, L"SaoAuto runtime verification failed.");

    ExtractedPaths extracted_paths;
    std::vector<UniqueHandle> helper_file_guards;
    std::vector<fs::path> session_paths;
    std::vector<UniqueHandle> session_guards;
    {
        sao::shell::artifact::ExtractResult extraction;
        const bool extracted = sao::shell::artifact::extract_bundle(
            bundle_path, generation, helper_root, key, extraction, artifact_error);
        const bool layout_valid = extracted && exact_extraction_layout(extraction);
        const bool helper_paths_valid =
            extracted && validate_helper_paths(helper_root, extraction.helper_files,
                                               extracted_paths.helpers, helper_file_guards);
        cleanup.set_helpers(std::move(extracted_paths.helpers));
        const bool session_paths_valid =
            extracted && validate_session_paths(generation, extraction.session_files, session_paths,
                                                session_guards);
        const bool payload_path_valid =
            session_paths_valid && validate_payload_path(generation, extraction.payload_path,
                                                         session_paths, extracted_paths.payload);
        const bool extracted_files_valid =
            layout_valid && helper_paths_valid && session_paths_valid && payload_path_valid &&
            sao::shell::artifact::verify_extracted_bundle(bundle_path, extraction, key,
                                                          artifact_error);
        key_wiper.wipe();
        const bool helper_lock_released = remove_helper_transaction_lock(helper_root);
        if (!extracted || !helper_lock_released)
            return failure(kFailureExtraction, L"SaoAuto runtime extraction failed.");
        if (!extracted_files_valid) {
            return failure(kFailureExtraction, L"SaoAuto runtime extraction was invalid.");
        }
    }

    if (!open_directory_guard(install_root, FILE_READ_ATTRIBUTES | SYNCHRONIZE) ||
        !open_directory_guard(install_runtime, FILE_READ_ATTRIBUTES | SYNCHRONIZE) ||
        !open_directory_guard(helper_root, FILE_READ_ATTRIBUTES | SYNCHRONIZE) ||
        !open_directory_guard(generation, FILE_READ_ATTRIBUTES | SYNCHRONIZE) ||
        !open_regular_guard(bundle_path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ)) {
        return failure(kFailureExtraction, L"SaoAuto runtime paths changed during extraction.");
    }
    if (!bundle_guard.close())
        return failure(kFailureExtraction, L"SaoAuto runtime bundle could not be released.");

    std::vector<wchar_t> environment;
    std::vector<wchar_t> command_line;
    if (!build_environment_block(install_root, generation, helper_root, environment) ||
        !build_child_command_line(extracted_paths.payload, command_line)) {
        return failure(kFailureEnvironment, L"SaoAuto launch parameters are invalid.");
    }

    DWORD child_exit_code = 0u;
    bool wait_failed = false;
    if (!launch_payload(extracted_paths.payload, install_root, command_line, environment,
                        child_exit_code, wait_failed)) {
        return failure(wait_failed ? kFailureWait : kFailureLaunch,
                       wait_failed ? L"SaoAuto payload wait failed."
                                   : L"SaoAuto payload could not be started.");
    }
    char child_diagnostic[96]{};
    (void)std::snprintf(child_diagnostic, sizeof(child_diagnostic),
                        "SAO_BOOTSTRAP_CHILD_EXIT code=%lu",
                        static_cast<unsigned long>(child_exit_code));
    bootstrap_diagnostic(child_diagnostic);

    bool guards_closed = true;
    for (UniqueHandle& session_guard : session_guards)
        guards_closed = session_guard.close() && guards_closed;
    session_guards.clear();
    for (UniqueHandle& helper_file_guard : helper_file_guards)
        guards_closed = helper_file_guard.close() && guards_closed;
    helper_file_guards.clear();
    guards_closed = generation_guard.close() && guards_closed;
    const TreeDeleteResult cleanup_result = cleanup.cleanup();
    guards_closed = remove_helper_transaction_lock(helper_root) && guards_closed;
    guards_closed = helper_guard.close() && guards_closed;
    guards_closed = runtime_guard.close() && guards_closed;
    guards_closed = install_guard.close() && guards_closed;
    guards_closed = module_guard.close() && guards_closed;
    guards_closed = user_runtime_guard.close() && guards_closed;
    guards_closed = product_root_guard.close() && guards_closed;
    guards_closed = local_app_data_guard.close() && guards_closed;
    char cleanup_diagnostic[128]{};
    (void)std::snprintf(cleanup_diagnostic, sizeof(cleanup_diagnostic),
                        "SAO_BOOTSTRAP_CLEANUP guards=%d tree=%u",
                        guards_closed ? 1 : 0, static_cast<unsigned>(cleanup_result));
    bootstrap_diagnostic(cleanup_diagnostic);
    if (!guards_closed || cleanup_result != TreeDeleteResult::deleted) {
        return failure(child_exit_code == 0u ? kFailureCleanup : child_exit_code,
                       L"SaoAuto runtime cleanup was incomplete.");
    }
    return child_outcome(child_exit_code);
}

void show_failure(const wchar_t* message) noexcept {
    (void)::MessageBoxW(nullptr, message != nullptr ? message : L"SaoAuto bootstrap failed.",
                        L"SaoAuto", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    (void)bootstrap_diagnostic_output();
    try {
        const BootstrapOutcome outcome = run_bootstrap();
        char outcome_diagnostic[128]{};
        (void)std::snprintf(outcome_diagnostic, sizeof(outcome_diagnostic),
                            "SAO_BOOTSTRAP_OUTCOME failed=%d code=%lu",
                            outcome.failed ? 1 : 0,
                            static_cast<unsigned long>(outcome.return_code));
        bootstrap_diagnostic(outcome_diagnostic);
        if (outcome.failed) {
            show_failure(outcome.message);
            bootstrap_diagnostic("SAO_BOOTSTRAP_FAILURE_UI_DONE");
        }
        return static_cast<int>(outcome.return_code);
    } catch (const std::bad_alloc&) {
        show_failure(L"SaoAuto bootstrap ran out of memory.");
        return static_cast<int>(kFailureUnexpected);
    } catch (const std::exception&) {
        show_failure(L"SaoAuto bootstrap encountered an unexpected error.");
        return static_cast<int>(kFailureUnexpected);
    } catch (...) {
        show_failure(L"SaoAuto bootstrap encountered an unexpected error.");
        return static_cast<int>(kFailureUnexpected);
    }
}
