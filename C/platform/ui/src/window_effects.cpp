// window_effects.cpp — DWM chrome tweaks (Phase 14 Python parity closure).
//
// Port of python/utils/window_effects.py. Applies dark caption / rounded /
// shadow via DwmSetWindowAttribute + SetClassLongW. Safe no-op on non-Win.

#include "sao/ui/theme.h"

#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

extern "C" void sao_ui_apply_native_chrome(void* hwnd_opaque, int dark_mode,
                                            int rounded_corners, int shadow) {
#if defined(_WIN32)
    HWND hwnd = static_cast<HWND>(hwnd_opaque);
    if (hwnd == nullptr) return;
    BOOL dark = dark_mode ? TRUE : FALSE;
    // Windows 20H1+: attribute 20 = DWMWA_USE_IMMERSIVE_DARK_MODE.
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    // Older 1809-1909 build fallback: attribute 19.
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));

    // 33 = DWMWA_WINDOW_CORNER_PREFERENCE; 2 = DWMWCP_ROUND.
    DWORD corner_pref = rounded_corners ? 2 : 1;
    DwmSetWindowAttribute(hwnd, 33, &corner_pref, sizeof(corner_pref));

    // Drop shadow via class-style CS_DROPSHADOW.
    LONG_PTR cls = GetClassLongPtrW(hwnd, GCL_STYLE);
    LONG_PTR new_cls = shadow ? (cls | CS_DROPSHADOW) : (cls & ~CS_DROPSHADOW);
    if (new_cls != cls) SetClassLongPtrW(hwnd, GCL_STYLE, new_cls);
#else
    (void)hwnd_opaque;
    (void)dark_mode;
    (void)rounded_corners;
    (void)shadow;
#endif
}
