# -*- coding: utf-8 -*-
# List small visible top-level windows (find the SAO floating ball).
import ctypes
from ctypes import wintypes

user32 = ctypes.windll.user32
wins = []


@ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
def cb(hwnd, lp):
    if not user32.IsWindowVisible(hwnd):
        return True
    buf = ctypes.create_unicode_buffer(128)
    user32.GetClassNameW(hwnd, buf, 128)
    tbuf = ctypes.create_unicode_buffer(128)
    user32.GetWindowTextW(hwnd, tbuf, 128)
    r = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    if 0 < w < 700 and 0 < h < 700:
        wins.append((buf.value, tbuf.value, r.left, r.top, w, h))
    return True


user32.EnumWindows(cb, 0)
for x in wins:
    print(x)
