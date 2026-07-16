// SAO Auto — launcher/working_dir.cpp

#include "sao/launcher/working_dir.h"
#include "sao/launcher/app.h"

#include <windows.h>
#include <shlwapi.h>
#include <cwchar>

#pragma comment(lib, "shlwapi.lib")

namespace sao::launcher {

bool resolveWorkingDir(AppState& state) noexcept {
    // 1. Get the exe path.
    if (!GetModuleFileNameW(nullptr, state.exe_path, MAX_PATH)) {
        return false;
    }

    // 2. Compute base_dir.
    return computeBaseDir(state.exe_path, state.base_dir, MAX_PATH);
}

bool computeBaseDir(const wchar_t* exe_path,
                    wchar_t* base_dir_out,
                    std::size_t base_dir_cap) noexcept {
    if (!exe_path || !base_dir_out || base_dir_cap == 0) return false;

    // Copy the exe path, strip the filename.
    lstrcpynW(base_dir_out, exe_path, static_cast<int>(base_dir_cap));
    PathRemoveFileSpecW(base_dir_out);

    // If the parent directory is named "runtime", walk up one more level —
    // this is the Nuitka-style onedir layout where the C launcher lives
    // outside runtime/.  See project_nuitka_runtime_layout_code_pitfalls.
    wchar_t tail[MAX_PATH]{};
    lstrcpynW(tail, base_dir_out, MAX_PATH);
    PathStripPathW(tail);
    if (_wcsicmp(tail, L"runtime") == 0) {
        PathRemoveFileSpecW(base_dir_out);
    }

    return base_dir_out[0] != L'\0';
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
        return true;
    }
    if (err == ERROR_PATH_NOT_FOUND) {
        // Recurse to create parent.
        wchar_t parent[MAX_PATH]{};
        lstrcpynW(parent, path, MAX_PATH);
        if (!PathRemoveFileSpecW(parent)) return false;
        if (!ensureDirectoryExists(parent)) return false;
        return CreateDirectoryW(path, nullptr) != FALSE;
    }
    return false;
}

} // namespace sao::launcher
