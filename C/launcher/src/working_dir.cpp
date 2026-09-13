// SAO Auto — launcher/working_dir.cpp

#include "sao/launcher/working_dir.h"
#include "sao/launcher/app.h"

#include <algorithm>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace sao::launcher {

namespace {

enum class RuntimeInstallRootState { absent, valid, invalid };

bool pathSeparator(wchar_t value) noexcept {
    return value == L'\\' || value == L'/';
}

bool absoluteInstallPath(std::wstring_view value) noexcept {
    if (value.size() >= 3u &&
        ((value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'a' && value[0] <= L'z')) &&
        value[1] == L':' && pathSeparator(value[2])) {
        return true;
    }
    if (value.size() < 3u || !pathSeparator(value[0]) || !pathSeparator(value[1]))
        return false;
    if (value[2] == L'.')
        return false;
    if (value[2] != L'?')
        return true;
    if (value.size() >= 7u &&
        ((value[4] >= L'A' && value[4] <= L'Z') || (value[4] >= L'a' && value[4] <= L'z')) &&
        value[5] == L':' && pathSeparator(value[6])) {
        return true;
    }
    return value.size() >= 8u &&
           (value.substr(4u, 4u) == L"UNC\\" || value.substr(4u, 4u) == L"UNC/");
}

std::wstring comparablePath(std::wstring value) {
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

bool fullComparablePath(const std::wstring& input, std::wstring& output) {
    const DWORD required = GetFullPathNameW(input.c_str(), 0u, nullptr, nullptr);
    if (required == 0u || required >= 32768u)
        return false;
    std::wstring buffer(static_cast<std::size_t>(required) + 1u, L'\0');
    const DWORD written =
        GetFullPathNameW(input.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (written == 0u || written >= buffer.size())
        return false;
    buffer.resize(written);
    output = comparablePath(std::move(buffer));
    return !output.empty();
}

bool finalComparablePath(HANDLE handle, std::wstring& output) {
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD required = GetFinalPathNameByHandleW(handle, nullptr, 0u, flags);
    if (required == 0u || required >= 32768u)
        return false;
    std::wstring buffer(static_cast<std::size_t>(required) + 1u, L'\0');
    const DWORD written =
        GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), flags);
    if (written == 0u || written >= buffer.size())
        return false;
    buffer.resize(written);
    output = comparablePath(std::move(buffer));
    return !output.empty();
}

bool exactNonReparseDirectory(const std::wstring& input, std::wstring& normalized) {
    std::wstring expected;
    if (!fullComparablePath(input, expected))
        return false;
    HANDLE directory = CreateFileW(
        input.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE)
        return false;

    FILE_ATTRIBUTE_TAG_INFO attributes{};
    std::wstring actual;
    const bool valid =
        GetFileInformationByHandleEx(directory, FileAttributeTagInfo, &attributes,
                                     sizeof(attributes)) != FALSE &&
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u &&
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0u &&
        GetFileType(directory) == FILE_TYPE_DISK && finalComparablePath(directory, actual) &&
        CompareStringOrdinal(expected.data(), static_cast<int>(expected.size()), actual.data(),
                             static_cast<int>(actual.size()), TRUE) == CSTR_EQUAL;
    const bool closed = CloseHandle(directory) != FALSE;
    if (!valid || !closed)
        return false;
    normalized = std::move(expected);
    return true;
}

bool exactNonReparseFile(const std::wstring& input, std::wstring& normalized) {
    std::wstring expected;
    if (!fullComparablePath(input, expected))
        return false;
    HANDLE file = CreateFileW(input.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    FILE_ATTRIBUTE_TAG_INFO attributes{};
    std::wstring actual;
    const bool valid =
        GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes, sizeof(attributes)) !=
            FALSE &&
        (attributes.FileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) == 0u &&
        GetFileType(file) == FILE_TYPE_DISK && finalComparablePath(file, actual) &&
        CompareStringOrdinal(expected.data(), static_cast<int>(expected.size()), actual.data(),
                             static_cast<int>(actual.size()), TRUE) == CSTR_EQUAL;
    const bool closed = CloseHandle(file) != FALSE;
    if (!valid || !closed)
        return false;
    normalized = std::move(expected);
    return true;
}

bool currentProcessModulePath(std::wstring& output) {
    std::vector<wchar_t> buffer(512u);
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0u)
            return false;
        if (length < buffer.size() - 1u ||
            (length == buffer.size() - 1u && GetLastError() != ERROR_INSUFFICIENT_BUFFER)) {
            output.assign(buffer.data(), length);
            return !output.empty();
        }
        if (buffer.size() >= 32768u)
            return false;
        buffer.resize(buffer.size() * 2u);
    }
}

bool installedBootstrapPath(const std::wstring& root, std::wstring& output) {
    if (root.empty())
        return false;
    std::wstring path(root);
    if (!pathSeparator(path.back()))
        path.push_back(L'\\');
    path.append(L"SaoAuto.exe");
    return exactNonReparseFile(path, output);
}

RuntimeInstallRootState runtimeInstallRoot(std::wstring& output) noexcept {
    output.clear();
    constexpr wchar_t kVariable[] = L"SAO_INSTALL_ROOT";
    SetLastError(ERROR_SUCCESS);
    const DWORD required = GetEnvironmentVariableW(kVariable, nullptr, 0u);
    if (required == 0u) {
        return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? RuntimeInstallRootState::absent
                                                        : RuntimeInstallRootState::invalid;
    }
    if (required > 32767u)
        return RuntimeInstallRootState::invalid;

    try {
        std::wstring value(required, L'\0');
        const DWORD written =
            GetEnvironmentVariableW(kVariable, value.data(), static_cast<DWORD>(value.size()));
        if (written == 0u || written >= value.size())
            return RuntimeInstallRootState::invalid;
        value.resize(written);
        std::wstring normalized;
        std::wstring bootstrap;
        if (value.empty() || !absoluteInstallPath(value) ||
            !exactNonReparseDirectory(value, normalized) ||
            !installedBootstrapPath(normalized, bootstrap)) {
            output.clear();
            return RuntimeInstallRootState::invalid;
        }
        output = std::move(normalized);
        return RuntimeInstallRootState::valid;
    } catch (...) {
        output.clear();
        return RuntimeInstallRootState::invalid;
    }
}

} // namespace

bool getCurrentModulePath(std::wstring& path_out) noexcept {
    path_out.clear();
    try {
        std::wstring runtime_root;
        const RuntimeInstallRootState runtime_state = runtimeInstallRoot(runtime_root);
        if (runtime_state == RuntimeInstallRootState::invalid)
            return false;
        if (runtime_state == RuntimeInstallRootState::valid) {
            return installedBootstrapPath(runtime_root, path_out);
        }
        return currentProcessModulePath(path_out);
    } catch (...) {
        path_out.clear();
        return false;
    }
}

bool resolveWorkingDir(AppState& state) noexcept {
    state.exe_path.clear();
    state.base_dir.clear();
    try {
        std::wstring resolved_exe;
        std::wstring resolved_base;
        std::wstring runtime_root;
        const RuntimeInstallRootState runtime_state = runtimeInstallRoot(runtime_root);
        if (runtime_state == RuntimeInstallRootState::valid) {
            if (!installedBootstrapPath(runtime_root, resolved_exe))
                return false;
            state.exe_path = std::move(resolved_exe);
            state.base_dir = std::move(runtime_root);
            return true;
        }
        if (runtime_state == RuntimeInstallRootState::invalid) {
            return false;
        }
        if (!currentProcessModulePath(resolved_exe))
            return false;
        const std::size_t separator = resolved_exe.find_last_of(L"\\/");
        if (separator == std::wstring::npos)
            return false;
        resolved_base = resolved_exe.substr(0u, separator);
        const std::size_t tail_start = resolved_base.find_last_of(L"\\/");
        const std::wstring tail = tail_start == std::wstring::npos
                                      ? resolved_base
                                      : resolved_base.substr(tail_start + 1u);
        if (_wcsicmp(tail.c_str(), L"runtime") == 0) {
            const std::size_t parent_end = resolved_base.find_last_of(L"\\/");
            if (parent_end == std::wstring::npos)
                return false;
            resolved_base.resize(parent_end);
        }
        if (resolved_base.empty())
            return false;
        state.exe_path = std::move(resolved_exe);
        state.base_dir = std::move(resolved_base);
        return true;
    } catch (...) {
        state.exe_path.clear();
        state.base_dir.clear();
        return false;
    }
}

bool computeBaseDir(const wchar_t* exe_path, wchar_t* base_dir_out,
                    std::size_t base_dir_cap) noexcept {
    if (!exe_path || !base_dir_out || base_dir_cap == 0)
        return false;
    base_dir_out[0] = L'\0';
    try {
        std::wstring runtime_root;
        const RuntimeInstallRootState runtime_state = runtimeInstallRoot(runtime_root);
        if (runtime_state == RuntimeInstallRootState::valid) {
            if (runtime_root.size() + 1u > base_dir_cap) {
                return false;
            }
            std::wmemcpy(base_dir_out, runtime_root.c_str(), runtime_root.size() + 1u);
            return true;
        }
        if (runtime_state == RuntimeInstallRootState::invalid) {
            return false;
        }
        std::wstring path(exe_path);
        const std::size_t separator = path.find_last_of(L"\\/");
        if (separator == std::wstring::npos)
            return false;
        std::wstring base_dir = path.substr(0u, separator);
        const std::size_t tail_start = base_dir.find_last_of(L"\\/");
        const std::wstring tail =
            tail_start == std::wstring::npos ? base_dir : base_dir.substr(tail_start + 1u);
        if (_wcsicmp(tail.c_str(), L"runtime") == 0) {
            const std::size_t parent_end = base_dir.find_last_of(L"\\/");
            if (parent_end == std::wstring::npos)
                return false;
            base_dir.resize(parent_end);
        }
        if (base_dir.empty() || base_dir.size() + 1u > base_dir_cap)
            return false;
        std::wmemcpy(base_dir_out, base_dir.c_str(), base_dir.size() + 1u);
        return true;
    } catch (...) {
        base_dir_out[0] = L'\0';
        return false;
    }
}

bool computeBaseDir(const wchar_t* exe_path, std::wstring& base_dir_out) noexcept {
    base_dir_out.clear();
    if (!exe_path)
        return false;
    try {
        std::wstring runtime_root;
        const RuntimeInstallRootState runtime_state = runtimeInstallRoot(runtime_root);
        if (runtime_state == RuntimeInstallRootState::valid) {
            base_dir_out = std::move(runtime_root);
            return true;
        }
        if (runtime_state == RuntimeInstallRootState::invalid) {
            return false;
        }
        std::wstring path(exe_path);
        const std::size_t separator = path.find_last_of(L"\\/");
        if (separator == std::wstring::npos)
            return false;
        base_dir_out = path.substr(0u, separator);
        const std::size_t tail_start = base_dir_out.find_last_of(L"\\/");
        const std::wstring tail =
            tail_start == std::wstring::npos ? base_dir_out : base_dir_out.substr(tail_start + 1u);
        if (_wcsicmp(tail.c_str(), L"runtime") == 0) {
            const std::size_t parent_end = base_dir_out.find_last_of(L"\\/");
            if (parent_end == std::wstring::npos)
                return false;
            base_dir_out.resize(parent_end);
        }
        return !base_dir_out.empty();
    } catch (...) {
        base_dir_out.clear();
        return false;
    }
}
bool ensureDirectoryExists(const wchar_t* path) noexcept {
    if (!path || !*path)
        return false;

    // CreateDirectoryW returns FALSE + ERROR_ALREADY_EXISTS if the directory
    // is already there, which we treat as success.
    if (CreateDirectoryW(path, nullptr)) {
        return true;
    }
    DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS) {
        const DWORD attributes = GetFileAttributesW(path);
        return attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    if (err == ERROR_PATH_NOT_FOUND) {
        try {
            std::wstring parent(path);
            const std::size_t separator = parent.find_last_of(L"\\/");
            if (separator == std::wstring::npos)
                return false;
            parent.resize(separator);
            if (!ensureDirectoryExists(parent.c_str()))
                return false;
            if (CreateDirectoryW(path, nullptr))
                return true;
            if (GetLastError() != ERROR_ALREADY_EXISTS)
                return false;
            const DWORD attributes = GetFileAttributesW(path);
            return attributes != INVALID_FILE_ATTRIBUTES &&
                   (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        } catch (...) {
            return false;
        }
    }
    return false;
}

} // namespace sao::launcher
