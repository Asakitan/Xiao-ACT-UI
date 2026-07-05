# -*- coding: utf-8 -*-
# native_dialog — 原生「打开文件」对话框（Windows comdlg32），跨 UI 模式可用。
#
# 平台插件管理面板在 WebView 模式下没有 Tk root，``tkinter.filedialog`` 不可用；
# 这里直接调 ``comdlg32.GetOpenFileNameW``，Entity / WebView 两种模式都能弹出资源
# 管理器。可在后台线程调用（对话框自带模态消息循环）。从 midi_piano 的 mp_dialog
# 上提为平台通用能力。

from __future__ import annotations

import os
from typing import Optional, Sequence

# OPENFILENAMEW Flags
OFN_EXPLORER = 0x00080000
OFN_FILEMUSTEXIST = 0x00001000
OFN_PATHMUSTEXIST = 0x00000800
OFN_NOCHANGEDIR = 0x00000008
OFN_HIDEREADONLY = 0x00000004


def _build_filter(filters: Sequence[tuple[str, str]]) -> str:
    # [(label, pattern), ...] → comdlg32 以 \0 分隔、\0\0 结尾的过滤器串。
    parts: list[str] = []
    for label, pattern in filters:
        parts.append(str(label))
        parts.append(str(pattern))
    return "\0".join(parts) + "\0\0"


def open_file(filters: Optional[Sequence[tuple[str, str]]] = None,
              title: str = "选择文件",
              initial_dir: str = "",
              hwnd_owner: int = 0) -> Optional[str]:
    # 弹出资源管理器选单个文件，返回绝对路径或 None（取消 / 失败 / 非 Windows）。
    try:
        import ctypes
        from ctypes import wintypes
    except Exception:
        return None

    try:
        comdlg32 = ctypes.windll.comdlg32
    except Exception:
        return None

    class _OPENFILENAMEW(ctypes.Structure):
        _fields_ = [
            ("lStructSize", wintypes.DWORD),
            ("hwndOwner", wintypes.HWND),
            ("hInstance", wintypes.HINSTANCE),
            ("lpstrFilter", wintypes.LPCWSTR),
            ("lpstrCustomFilter", wintypes.LPWSTR),
            ("nMaxCustFilter", wintypes.DWORD),
            ("nFilterIndex", wintypes.DWORD),
            ("lpstrFile", wintypes.LPWSTR),
            ("nMaxFile", wintypes.DWORD),
            ("lpstrFileTitle", wintypes.LPWSTR),
            ("nMaxFileTitle", wintypes.DWORD),
            ("lpstrInitialDir", wintypes.LPCWSTR),
            ("lpstrTitle", wintypes.LPCWSTR),
            ("Flags", wintypes.DWORD),
            ("nFileOffset", wintypes.WORD),
            ("nFileExtension", wintypes.WORD),
            ("lpstrDefExt", wintypes.LPCWSTR),
            ("lCustData", ctypes.c_void_p),
            ("lpfnHook", ctypes.c_void_p),
            ("lpTemplateName", wintypes.LPCWSTR),
            ("pvReserved", ctypes.c_void_p),
            ("dwReserved", wintypes.DWORD),
            ("FlagsEx", wintypes.DWORD),
        ]

    if not filters:
        filters = [("所有文件 (*.*)", "*.*")]

    buf = ctypes.create_unicode_buffer(4096)
    ofn = _OPENFILENAMEW()
    ofn.lStructSize = ctypes.sizeof(_OPENFILENAMEW)
    ofn.hwndOwner = hwnd_owner or 0
    ofn.lpstrFilter = _build_filter(filters)
    ofn.nFilterIndex = 1
    ofn.lpstrFile = ctypes.cast(buf, wintypes.LPWSTR)
    ofn.nMaxFile = 4096
    ofn.lpstrTitle = title
    if initial_dir and os.path.isdir(initial_dir):
        ofn.lpstrInitialDir = initial_dir
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY

    try:
        ok = comdlg32.GetOpenFileNameW(ctypes.byref(ofn))
    except Exception:
        return None
    if not ok:
        return None
    return buf.value or None


def foreground_hwnd() -> int:
    try:
        import ctypes
        return int(ctypes.windll.user32.GetForegroundWindow() or 0)
    except Exception:
        return 0


def open_plugin_archive(initial_dir: str = "", hwnd_owner: int = 0) -> Optional[str]:
    # 选一个插件包(.zip / .saoplugin)，返回绝对路径或 None。
    return open_file(
        filters=[
            ("SAO 插件包 (*.zip;*.saoplugin)", "*.zip;*.saoplugin"),
            ("所有文件 (*.*)", "*.*"),
        ],
        title="导入插件包 Import plugin",
        initial_dir=initial_dir,
        hwnd_owner=hwnd_owner or foreground_hwnd(),
    )


__all__ = ["open_file", "open_plugin_archive", "foreground_hwnd"]
