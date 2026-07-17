#include "tool_launch_package_internal.h"

#include <algorithm>
#include <array>
#include <cwchar>
#include <iterator>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sao::launcher::tool_launch::detail {

struct PackageLease::Impl {
    ~Impl() {
        for (const HANDLE handle : guards) {
            if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
                CloseHandle(handle);
            }
        }
    }

    std::vector<HANDLE> guards;
};

namespace {

constexpr wchar_t kRuntimeExecutable[] = L"XiaoACTUI.exe";
constexpr wchar_t kAiEditorDocument[] = L"ai_editor_app.html";
constexpr wchar_t kAiEditorArgument[] = L"--ai-editor";

sao_status_t map_os_error(DWORD error) noexcept {
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return SAO_STATUS_ERR_NOT_FOUND;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return SAO_STATUS_ERR_ACCESS_DENIED;
    default:
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

class OwnedHandle final {
public:
    explicit OwnedHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~OwnedHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;

    OwnedHandle(OwnedHandle&& other) noexcept : handle_(other.release()) {}
    OwnedHandle& operator=(OwnedHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    HANDLE get() const noexcept { return handle_; }
    HANDLE release() noexcept {
        const HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }
    void reset(HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_{};
};

void append_quoted_argument(std::wstring& command_line,
                            const std::wstring& argument) {
    if (!command_line.empty()) {
        command_line.push_back(L' ');
    }
    command_line.push_back(L'\"');
    std::size_t slash_count = 0;
    for (const wchar_t value : argument) {
        if (value == L'\\') {
            ++slash_count;
            continue;
        }
        if (value == L'\"') {
            command_line.append(slash_count * 2 + 1, L'\\');
            command_line.push_back(L'\"');
            slash_count = 0;
            continue;
        }
        command_line.append(slash_count, L'\\');
        slash_count = 0;
        command_line.push_back(value);
    }
    command_line.append(slash_count * 2, L'\\');
    command_line.push_back(L'\"');
}

sao_status_t reject_reparse_components(
    const std::filesystem::path& path) noexcept {
    try {
        std::filesystem::path current;
        for (const auto& component : path) {
            current /= component;
            if (current == path.root_name()) {
                continue;
            }
            const DWORD attributes = GetFileAttributesW(current.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                return map_os_error(GetLastError());
            }
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                return SAO_STATUS_ERR_ACCESS_DENIED;
            }
        }
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

sao_status_t validate_path(const std::filesystem::path& path,
                           bool directory) noexcept {
    const sao_status_t reparse_status = reject_reparse_components(path);
    if (reparse_status != SAO_STATUS_OK) {
        return reparse_status;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return map_os_error(GetLastError());
    }
    const bool is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return is_directory == directory ? SAO_STATUS_OK
                                     : SAO_STATUS_ERR_NOT_FOUND;
}

std::wstring comparable_final_path(std::wstring path) {
    constexpr wchar_t kDevicePrefix[] = L"\\\\?\\";
    constexpr wchar_t kUncPrefix[] = L"\\\\?\\UNC\\";
    if (path.rfind(kUncPrefix, 0) == 0) {
        return L"\\\\" + path.substr(std::size(kUncPrefix) - 1U);
    }
    if (path.rfind(kDevicePrefix, 0) == 0) {
        return path.substr(std::size(kDevicePrefix) - 1U);
    }
    return path;
}

sao_status_t lock_verified_path(const std::filesystem::path& path,
                                bool directory,
                                PackageLease::Impl& lease) noexcept {
    try {
        const DWORD flags = directory ? FILE_FLAG_BACKUP_SEMANTICS : 0;
        const DWORD access = FILE_READ_ATTRIBUTES |
            (directory ? 0 : GENERIC_READ);
        OwnedHandle handle(CreateFileW(path.c_str(), access, FILE_SHARE_READ,
                                       nullptr, OPEN_EXISTING, flags, nullptr));
        if (handle.get() == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            handle.release();
            return map_os_error(error);
        }
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(handle.get(), &information)) {
            return map_os_error(GetLastError());
        }
        const bool is_directory =
            (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (is_directory != directory) {
            return SAO_STATUS_ERR_NOT_FOUND;
        }

        const DWORD required = GetFinalPathNameByHandleW(
            handle.get(), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (required == 0) {
            return map_os_error(GetLastError());
        }
        std::wstring final_path(static_cast<std::size_t>(required), L'\0');
        const DWORD copied = GetFinalPathNameByHandleW(
            handle.get(), final_path.data(), required,
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (copied == 0 || copied >= required) {
            return map_os_error(GetLastError());
        }
        final_path.resize(copied);
        const auto expected = comparable_final_path(
            std::filesystem::absolute(path).lexically_normal().wstring());
        const auto actual = comparable_final_path(std::move(final_path));
        if (_wcsicmp(expected.c_str(), actual.c_str()) != 0) {
            return SAO_STATUS_ERR_ACCESS_DENIED;
        }
        lease.guards.emplace_back(handle.get());
        (void)handle.release();
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

sao_status_t acquire_package_lease_impl(const std::filesystem::path& base,
                                        const PackagePaths& package,
                                        PackageLease::Impl& lease) noexcept {
    const std::array<std::pair<std::filesystem::path, bool>, 5> paths{
        std::pair{base, true},
        std::pair{base / L"web", true},
        std::pair{base / L"web" / kAiEditorDocument, false},
        std::pair{package.working_directory, true},
        std::pair{package.executable, false},
    };
    for (const auto& [path, directory] : paths) {
        const sao_status_t status = lock_verified_path(path, directory, lease);
        if (status != SAO_STATUS_OK) {
            return status;
        }
    }
    return SAO_STATUS_OK;
}

bool environment_has(const std::vector<std::wstring>& entries,
                     const wchar_t* name) noexcept {
    const std::size_t name_length = std::wcslen(name);
    return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
        return entry.size() > name_length && entry[name_length] == L'=' &&
               _wcsnicmp(entry.c_str(), name, name_length) == 0;
    });
}

void environment_set_default(std::vector<std::wstring>& entries,
                             const wchar_t* name,
                             const std::wstring& value) {
    if (!environment_has(entries, name)) {
        entries.emplace_back(std::wstring(name) + L"=" + value);
    }
}

void environment_set(std::vector<std::wstring>& entries, const wchar_t* name,
                     const wchar_t* value) {
    const std::size_t name_length = std::wcslen(name);
    const auto found = std::find_if(entries.begin(), entries.end(),
                                    [&](const auto& entry) {
        return entry.size() > name_length && entry[name_length] == L'=' &&
               _wcsnicmp(entry.c_str(), name, name_length) == 0;
    });
    const std::wstring setting = std::wstring(name) + L"=" + value;
    if (found == entries.end()) {
        entries.emplace_back(setting);
    } else {
        *found = setting;
    }
}

std::vector<wchar_t> build_child_environment(
    const std::filesystem::path& python_root) {
    struct EnvironmentStrings final {
        LPWCH value{};
        ~EnvironmentStrings() {
            if (value != nullptr) {
                FreeEnvironmentStringsW(value);
            }
        }
    } source{GetEnvironmentStringsW()};
    if (source.value == nullptr) {
        throw std::runtime_error("GetEnvironmentStringsW failed");
    }
    std::vector<std::wstring> entries;
    for (const wchar_t* cursor = source.value; *cursor != L'\0';) {
        entries.emplace_back(cursor);
        cursor += entries.back().size() + 1;
    }

    environment_set_default(entries, L"PYTHONPATH", python_root.wstring());
    environment_set_default(entries, L"PYTHONUNBUFFERED", L"1");
    const DWORD cwd_length = GetCurrentDirectoryW(0, nullptr);
    if (cwd_length > 1) {
        std::wstring cwd(cwd_length, L'\0');
        const DWORD copied = GetCurrentDirectoryW(cwd_length, cwd.data());
        if (copied > 0 && copied < cwd_length) {
            cwd.resize(copied);
            environment_set_default(entries, L"SAO_AI_EDITOR_WORKSPACE_ROOT",
                                    cwd);
        }
    }
    environment_set(entries, L"PYWEBVIEW_GUI", L"edgechromium");
    std::sort(entries.begin(), entries.end(), [](const auto& left,
                                                 const auto& right) {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    });

    std::vector<wchar_t> block;
    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

} // namespace

PackageLease::PackageLease() noexcept = default;
PackageLease::~PackageLease() = default;
PackageLease::PackageLease(PackageLease&&) noexcept = default;
PackageLease& PackageLease::operator=(PackageLease&&) noexcept = default;

sao_status_t acquire_package_lease(const std::filesystem::path& base,
                                   const PackagePaths& package,
                                   PackageLease& out_lease) noexcept {
    try {
        PackageLease candidate;
        candidate.impl_ = std::make_unique<PackageLease::Impl>();
        const sao_status_t status =
            acquire_package_lease_impl(base, package, *candidate.impl_);
        if (status != SAO_STATUS_OK) {
            return status;
        }
        out_lease = std::move(candidate);
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

sao_status_t resolve_package(const std::filesystem::path& base,
                             PackagePaths& out) noexcept {
    try {
        sao_status_t status = validate_path(base, true);
        if (status != SAO_STATUS_OK) {
            return status;
        }
        const auto web_dir = base / L"web";
        status = validate_path(web_dir, true);
        if (status != SAO_STATUS_OK) {
            return status;
        }
        status = validate_path(web_dir / kAiEditorDocument, false);
        if (status != SAO_STATUS_OK) {
            return status;
        }

        const auto runtime_dir = base / L"runtime";
        const sao_status_t runtime_status = validate_path(runtime_dir, true);
        if (runtime_status == SAO_STATUS_OK) {
            const auto runtime_executable = runtime_dir / kRuntimeExecutable;
            status = validate_path(runtime_executable, false);
            if (status == SAO_STATUS_OK) {
                out = {runtime_executable, runtime_dir};
                return SAO_STATUS_OK;
            }
            if (status != SAO_STATUS_ERR_NOT_FOUND) {
                return status;
            }
        } else if (runtime_status != SAO_STATUS_ERR_NOT_FOUND) {
            return runtime_status;
        }

        const auto root_executable = base / kRuntimeExecutable;
        status = validate_path(root_executable, false);
        if (status != SAO_STATUS_OK) {
            return status;
        }
        out = {root_executable, base};
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

sao_status_t create_ai_editor_process(const std::filesystem::path& base,
                                      PROCESS_INFORMATION& out_process,
                                      PackagePaths& out_package,
                                      PackageLease& out_lease) noexcept {
    out_process = {};
    try {
        PackagePaths package;
        sao_status_t status = resolve_package(base, package);
        if (status != SAO_STATUS_OK) {
            return status;
        }
        PackageLease lease;
        status = acquire_package_lease(base, package, lease);
        if (status != SAO_STATUS_OK) {
            return status;
        }

        std::wstring command_line;
        append_quoted_argument(command_line, package.executable.wstring());
        append_quoted_argument(command_line, kAiEditorArgument);
        std::vector<wchar_t> mutable_command(command_line.begin(),
                                             command_line.end());
        mutable_command.push_back(L'\0');
        auto environment = build_child_environment(package.working_directory);
        out_package = package;

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        constexpr DWORD kFlags = CREATE_NEW_PROCESS_GROUP |
                                 CREATE_UNICODE_ENVIRONMENT |
                                 BELOW_NORMAL_PRIORITY_CLASS;
        if (!CreateProcessW(
                package.executable.c_str(), mutable_command.data(), nullptr,
                nullptr, FALSE, kFlags, environment.data(),
                package.working_directory.c_str(), &startup, &out_process)) {
            return map_os_error(GetLastError());
        }
        out_lease = std::move(lease);
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

} // namespace sao::launcher::tool_launch::detail
