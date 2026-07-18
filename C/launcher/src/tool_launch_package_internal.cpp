#include "tool_launch_package_internal.h"

#include <windows.h>

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

constexpr wchar_t kRuntimeExecutable[] = L"SaoAiEditor.exe";

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
    const std::array<std::pair<std::filesystem::path, bool>, 2> paths{
        std::pair{base, true},
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
        const auto runtime_dir = base / L"runtime";
        const sao_status_t runtime_status = validate_path(runtime_dir, true);
        if (runtime_status == SAO_STATUS_OK) {
            const auto runtime_executable = runtime_dir / kRuntimeExecutable;
            status = validate_path(runtime_executable, false);
            if (status == SAO_STATUS_OK) {
                out = {runtime_executable, base};
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

} // namespace sao::launcher::tool_launch::detail
