# -*- coding: utf-8 -*-
"""RW module — input simulation (kernel-mode only).

Uses Engine E kernel-level HID injection (rt_io.send_m / send_k)
which injects through mouclass/kbdclass service callbacks — appears
as genuine hardware input.  Returns silently when Engine E is
unavailable; no user-mode path is used in active code.
"""

from __future__ import annotations

import random
import time
from typing import Optional

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


# --- Public API (kernel-mode only) --------------------------------------------

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
    """Click with randomised hold duration (reactor)."""
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
