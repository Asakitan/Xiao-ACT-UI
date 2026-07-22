#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace sao::ai_editor {

inline bool apply_stealth_window(HWND hwnd) {
    if (hwnd == nullptr || !::IsWindow(hwnd)) {
        return false;
    }
    ::SetWindowTextW(hwnd, L"");
    using SetWindowDisplayAffinityFn = BOOL(WINAPI*)(HWND, DWORD);
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    const auto set_affinity =
        user32 == nullptr
            ? nullptr
            : reinterpret_cast<SetWindowDisplayAffinityFn>(
                  ::GetProcAddress(user32, "SetWindowDisplayAffinity"));
    if (set_affinity == nullptr) {
        return false;
    }
    constexpr DWORD kWdaExcludeFromCapture = 0x00000011;
    constexpr DWORD kWdaMonitor = 0x00000001;
    if (set_affinity(hwnd, kWdaExcludeFromCapture) != FALSE) {
        return true;
    }
    return set_affinity(hwnd, kWdaMonitor) != FALSE;
}

}  // namespace sao::ai_editor

#endif
