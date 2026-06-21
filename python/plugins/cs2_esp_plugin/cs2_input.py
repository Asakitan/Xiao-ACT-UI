# -*- coding: utf-8 -*-
"""CS2 plugin — input simulation (kernel-mode only).

Uses Engine E kernel-level HID injection (rt_io.send_m / send_k)
which injects through mouclass/kbdclass service callbacks — appears
as genuine hardware input.  Returns silently when Engine E is
unavailable; no user-mode path is used in active code.
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes
import random
import time
from typing import Optional, Tuple

# --- Engine E (kernel HID injection) ----------------------------------------

_rt = None
_km_available: Optional[bool] = None


def _ensure_rt():
    global _rt
    if _rt is not None:
        return
    try:
        from mem_probe import rt_io
        _rt = rt_io
    except Exception:
        _rt = False


def _km_ok() -> bool:
    global _km_available
    if _km_available is not None:
        return _km_available
    _ensure_rt()
    if not _rt:
        _km_available = False
        return False
    try:
        _km_available = bool(_rt.input_available())
    except Exception:
        _km_available = False
    return _km_available


# --- User-mode SendInput fallback -------------------------------------------

_user32 = ctypes.WinDLL("user32", use_last_error=True)

INPUT_MOUSE = 0
MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010


class _MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", ctypes.c_long), ("dy", ctypes.c_long),
        ("mouseData", ctypes.wintypes.DWORD),
        ("dwFlags", ctypes.wintypes.DWORD),
        ("time", ctypes.wintypes.DWORD),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


class _KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", ctypes.wintypes.WORD), ("wScan", ctypes.wintypes.WORD),
        ("dwFlags", ctypes.wintypes.DWORD),
        ("time", ctypes.wintypes.DWORD),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


class _INPUT_U(ctypes.Union):
    _fields_ = [("mi", _MOUSEINPUT), ("ki", _KEYBDINPUT)]


class _INPUT(ctypes.Structure):
    _fields_ = [("type", ctypes.wintypes.DWORD), ("u", _INPUT_U)]


def _um_send(*inputs: _INPUT) -> int:
    n = len(inputs)
    arr = (_INPUT * n)(*inputs)
    return _user32.SendInput(n, arr, ctypes.sizeof(_INPUT))


def _um_move(dx: int, dy: int):
    inp = _INPUT()
    inp.type = INPUT_MOUSE
    inp.u.mi.dx = dx
    inp.u.mi.dy = dy
    inp.u.mi.dwFlags = MOUSEEVENTF_MOVE
    _um_send(inp)


def _um_click(down_flag: int, up_flag: int, hold_ms: float = 0):
    d = _INPUT(); d.type = INPUT_MOUSE; d.u.mi.dwFlags = down_flag
    u = _INPUT(); u.type = INPUT_MOUSE; u.u.mi.dwFlags = up_flag
    _um_send(d)
    if hold_ms > 0:
        time.sleep(hold_ms / 1000.0)
    _um_send(u)


# --- Public API (auto-selects kernel or usermode) ----------------------------

# btn flags for rt_io.send_m
_BTN_LDOWN = 0x0001
_BTN_LUP = 0x0002
_BTN_RDOWN = 0x0004
_BTN_RUP = 0x0008


def move_mouse(dx: int, dy: int) -> None:
    """Relative mouse movement (pixels). Kernel-mode required."""
    dx, dy = int(dx), int(dy)
    if dx == 0 and dy == 0:
        return
    if _km_ok():
        _rt.send_m(dx, dy)


def click(button: str = "left", hold_ms: float = 0) -> None:
    """Mouse click. Kernel-mode required."""
    if not _km_ok():
        return
    db, ub = {"left": (_BTN_LDOWN, _BTN_LUP),
              "right": (_BTN_RDOWN, _BTN_RUP)}.get(button, (_BTN_LDOWN, _BTN_LUP))
    _rt.send_m(0, 0, db)
    if hold_ms > 0:
        time.sleep(hold_ms / 1000.0)
    _rt.send_m(0, 0, ub)


def mouse_down(button: str = "left") -> None:
    if not _km_ok():
        return
    b = {"left": _BTN_LDOWN, "right": _BTN_RDOWN}.get(button, _BTN_LDOWN)
    _rt.send_m(0, 0, b)


def mouse_up(button: str = "left") -> None:
    if not _km_ok():
        return
    b = {"left": _BTN_LUP, "right": _BTN_RUP}.get(button, _BTN_LUP)
    _rt.send_m(0, 0, b)


def click_with_delay(min_ms: int = 30, max_ms: int = 80,
                     button: str = "left") -> None:
    """Click with randomised hold duration (triggerbot)."""
    hold = random.randint(min_ms, max_ms)
    click(button, hold_ms=hold)


# --- Keyboard (for auto-stop counter-strafe) ---------------------------------

VK_W = 0x57
VK_A = 0x41
VK_S = 0x53
VK_D = 0x44
VK_SHIFT = 0x10


def key_press(vk: int) -> None:
    """Press a key (hold down). Kernel-mode required."""
    if _km_ok():
        _rt.send_k(vk, True)


def key_release(vk: int) -> None:
    """Release a key. Kernel-mode required."""
    if _km_ok():
        _rt.send_k(vk, False)


def key_tap(vk: int, hold_ms: int = 30) -> None:
    """Tap a key (press + delay + release). Kernel-mode required."""
    if _km_ok():
        _rt.send_k_tap(vk, hold_ms)


def input_mode() -> str:
    """Return 'kernel' or 'unavailable' describing current input path."""
    return "kernel" if _km_ok() else "unavailable"
