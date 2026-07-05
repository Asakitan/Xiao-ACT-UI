# -*- coding: utf-8 -*-
# mp_dialog — 原生「打开文件」对话框（Windows comdlg32），跨 UI 模式可用。
#
# 平台插件面板是声明式(button/table)的，没有文件选择控件。host 自己用 tkinter.filedialog，
# 但那需要存在 Tk root（纯 WebView 模式可能没有）。这里直接调 Windows 通用对话框
# `comdlg32.GetOpenFileNameW`，不依赖 Tk，Entity / WebView 两种模式都能弹出资源管理器选 MIDI。
#
# 可在后台线程调用（对话框自带模态消息循环），返回所选绝对路径或 None。

from __future__ import annotations

import ctypes
import os
from ctypes import wintypes
from typing import Optional

# OPENFILENAMEW Flags
OFN_EXPLORER = 0x00080000
OFN_FILEMUSTEXIST = 0x00001000
OFN_PATHMUSTEXIST = 0x00000800
OFN_NOCHANGEDIR = 0x00000008
OFN_HIDEREADONLY = 0x00000004


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


def open_midi(initial_dir: str = "", title: str = "选择 MIDI 文件",
              hwnd_owner: int = 0) -> Optional[str]:
    # 弹出资源管理器选 MIDI，返回绝对路径或 None（取消/失败）。
    try:
        comdlg32 = ctypes.windll.comdlg32
    except Exception:
        return None

    buf = ctypes.create_unicode_buffer(2048)
    # 过滤器：以 \0 分隔、\0\0 结尾
    flt = "MIDI 文件 (*.mid;*.midi)\0*.mid;*.midi\0所有文件 (*.*)\0*.*\0\0"

    ofn = _OPENFILENAMEW()
    ofn.lStructSize = ctypes.sizeof(_OPENFILENAMEW)
    ofn.hwndOwner = hwnd_owner or 0
    ofn.lpstrFilter = flt
    ofn.nFilterIndex = 1
    ofn.lpstrFile = ctypes.cast(buf, wintypes.LPWSTR)
    ofn.nMaxFile = 2048
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
    path = buf.value
    return path or None


def foreground_hwnd() -> int:
    try:
        return int(ctypes.windll.user32.GetForegroundWindow() or 0)
    except Exception:
        return 0


__all__ = ["open_midi", "foreground_hwnd"]
