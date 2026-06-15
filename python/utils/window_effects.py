from __future__ import annotations

import ctypes


def apply_native_chrome(win, *, dark_caption: bool = True,
                        rounded: bool = True,
                        shadow: bool = True) -> None:
    """Apply native Windows chrome tweaks to a Tk toplevel.

    Safe no-op on unsupported systems.
    """
    try:
        win.update_idletasks()
        user32 = ctypes.windll.user32
        dwmapi = ctypes.windll.dwmapi
        hwnd = int(user32.GetParent(ctypes.c_void_p(win.winfo_id())) or win.winfo_id())

        if rounded:
            corner_pref = ctypes.c_int(2)  # DWMWCP_ROUND
            try:
                dwmapi.DwmSetWindowAttribute(
                    hwnd, 33, ctypes.byref(corner_pref), ctypes.sizeof(corner_pref)
                )
            except Exception:
                pass

        if dark_caption:
            enabled = ctypes.c_int(1)
            for attr in (20, 19):
                try:
                    result = dwmapi.DwmSetWindowAttribute(
                        hwnd, attr, ctypes.byref(enabled), ctypes.sizeof(enabled)
                    )
                    if result == 0:
                        break
                except Exception:
                    pass

        if shadow:
            gcl_style = -26
            cs_dropshadow = 0x00020000
            try:
                class_style = user32.GetClassLongW(hwnd, gcl_style)
                user32.SetClassLongW(hwnd, gcl_style, class_style | cs_dropshadow)
            except Exception:
                pass
    except Exception:
        pass