// SAO Auto — launcher/working_dir.cpp

#include "sao/launcher/working_dir.h"
#include "sao/launcher/app.h"

#include <windows.h>
#include <cwchar>
#include <string>
#include <vector>


namespace sao::launcher {

bool getCurrentModulePath(std::wstring& path_out) noexcept {
    path_out.clear();
    try {
        std::vector<wchar_t> buffer(512u);
        for (;;) {
            SetLastError(ERROR_SUCCESS);
            const DWORD length = GetModuleFileNameW(
                nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0u)
                return false;
            if (length < buffer.size() - 1u ||
                (length == buffer.size() - 1u &&
                 GetLastError() != ERROR_INSUFFICIENT_BUFFER)) {
                path_out.assign(buffer.data(), length);
                return !path_out.empty();
            }
            if (buffer.size() >= 32768u)
                return false;
            buffer.resize(buffer.size() * 2u);
        }
    } catch (...) {
        path_out.clear();
        return false;
    }
}

bool resolveWorkingDir(AppState& state) noexcept {
    if (!getCurrentModulePath(state.exe_path))
        return false;
    try {
        const std::size_t separator = state.exe_path.find_last_of(L"\\/");
        if (separator == std::wstring::npos)
            return false;
        state.base_dir = state.exe_path.substr(0u, separator);
        const std::size_t tail_start = state.base_dir.find_last_of(L"\\/");
        const std::wstring tail = tail_start == std::wstring::npos
            ? state.base_dir
            : state.base_dir.substr(tail_start + 1u);
        if (_wcsicmp(tail.c_str(), L"runtime") == 0) {
            const std::size_t parent_end = state.base_dir.find_last_of(L"\\/");
            if (parent_end == std::wstring::npos)
                return false;
            state.base_dir.resize(parent_end);
        }
        return !state.base_dir.empty();
    } catch (...) {
        state.base_dir.clear();
        return false;
    }
}

bool computeBaseDir(const wchar_t* exe_path,
                    wchar_t* base_dir_out,
                    std::size_t base_dir_cap) noexcept {
    if (!exe_path || !base_dir_out || base_dir_cap == 0) return false;
    base_dir_out[0] = L'\0';
    try {
        std::wstring path(exe_path);
        const std::size_t separator = path.find_last_of(L"\\/");
        if (separator == std::wstring::npos)
            return false;
        std::wstring base_dir = path.substr(0u, separator);
        const std::size_t tail_start = base_dir.find_last_of(L"\\/");
        const std::wstring tail = tail_start == std::wstring::npos
            ? base_dir
            : base_dir.substr(tail_start + 1u);
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


bool computeBaseDir(const wchar_t* exe_path,
                    std::wstring& base_dir_out) noexcept {
    base_dir_out.clear();
    if (!exe_path) return false;
    try {
        std::wstring path(exe_path);
        const std::size_t separator = path.find_last_of(L"\\/");
        if (separator == std::wstring::npos) return false;
        base_dir_out = path.substr(0u, separator);
        const std::size_t tail_start = base_dir_out.find_last_of(L"\\/");
        const std::wstring tail = tail_start == std::wstring::npos
            ? base_dir_out
            : base_dir_out.substr(tail_start + 1u);
        if (_wcsicmp(tail.c_str(), L"runtime") == 0) {
            const std::size_t parent_end = base_dir_out.find_last_of(L"\\/");
            if (parent_end == std::wstring::npos) return false;
            base_dir_out.resize(parent_end);
        }
        return !base_dir_out.empty();
    } catch (...) {
        base_dir_out.clear();
        return false;
    }
}
bool ensureDirectoryExists(const wchar_t* path) noexcept {
    if (!path || !*path) return false;

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
