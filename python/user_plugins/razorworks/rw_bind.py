# -*- coding: utf-8 -*-
"""RW module — key bind system for all analysis features.

Each feature has a bind key (VK code):
  0 = always active (no key required)
  other = only active while that key is held down

Common VK codes:
  0x01=LBUTTON  0x02=RBUTTON  0x04=MBUTTON  0x05=XBUTTON1  0x06=XBUTTON2
  0x10=SHIFT    0x11=CTRL     0x12=ALT      0x14=CAPSLOCK
"""

from __future__ import annotations

import ctypes
from typing import Dict

_user32 = None


def _ensure_user32():
    global _user32
    if _user32 is None:
        try:
            _user32 = ctypes.WinDLL("user32", use_last_error=True)
        except Exception:
            pass


def is_key_held(vk: int) -> bool:
    """Check if a key is currently held down. VK=0 means always True."""
    if vk == 0:
        return True
    _ensure_user32()
    if _user32 is None:
        return False
    try:
        return bool(_user32.GetAsyncKeyState(vk) & 0x8000)
    except Exception:
        return False


_toggle_state: dict = {}  # vk → bool (current toggled state)
_toggle_prev: dict = {}   # vk → bool (was held last tick)


def is_bind_active(bind_vk: int, mode: str = "hold") -> bool:
    """Check if a feature's bind key is active.

    mode="hold": active while key is held down (default, original behavior)
    mode="toggle": press once to activate, press again to deactivate
    VK=0 means always active regardless of mode.
    """
    if bind_vk == 0:
        return True
    if mode == "toggle":
        held_now = is_key_held(bind_vk)
        was_held = _toggle_prev.get(bind_vk, False)
        _toggle_prev[bind_vk] = held_now
        if held_now and not was_held:
            _toggle_state[bind_vk] = not _toggle_state.get(bind_vk, False)
        return _toggle_state.get(bind_vk, False)
    return is_key_held(bind_vk)


# VK code to human-readable name (for UI display)
VK_NAMES: Dict[int, str] = {
    0: "始终",
    0x01: "左键", 0x02: "右键", 0x04: "中键",
    0x05: "侧键1", 0x06: "侧键2",
    0x08: "Backspace", 0x09: "Tab", 0x0D: "Enter", 0x1B: "Esc",
    0x10: "Shift", 0x11: "Ctrl", 0x12: "Alt",
    0x14: "CapsLock", 0x20: "Space",
    0x21: "PageUp", 0x22: "PageDown", 0x23: "End", 0x24: "Home",
    0x25: "←", 0x26: "↑", 0x27: "→", 0x28: "↓",
    0x2D: "Insert", 0x2E: "Delete",
    0x30: "0", 0x31: "1", 0x32: "2", 0x33: "3", 0x34: "4",
    0x35: "5", 0x36: "6", 0x37: "7", 0x38: "8", 0x39: "9",
    0x41: "A", 0x42: "B", 0x43: "C", 0x44: "D", 0x45: "E",
    0x46: "F", 0x47: "G", 0x48: "H", 0x49: "I", 0x4A: "J",
    0x4B: "K", 0x4C: "L", 0x4D: "M", 0x4E: "N", 0x4F: "O",
    0x50: "P", 0x51: "Q", 0x52: "R", 0x53: "S", 0x54: "T",
    0x55: "U", 0x56: "V", 0x57: "W", 0x58: "X", 0x59: "Y", 0x5A: "Z",
    0x60: "Num0", 0x61: "Num1", 0x62: "Num2", 0x63: "Num3", 0x64: "Num4",
    0x65: "Num5", 0x66: "Num6", 0x67: "Num7", 0x68: "Num8", 0x69: "Num9",
    0x6A: "Num*", 0x6B: "Num+", 0x6D: "Num-", 0x6E: "Num.", 0x6F: "Num/",
    0x70: "F1", 0x71: "F2", 0x72: "F3", 0x73: "F4",
    0x74: "F5", 0x75: "F6", 0x76: "F7", 0x77: "F8",
    0x78: "F9", 0x79: "F10", 0x7A: "F11", 0x7B: "F12",
    0xA0: "L.Shift", 0xA1: "R.Shift", 0xA2: "L.Ctrl", 0xA3: "R.Ctrl",
    0xA4: "L.Alt", 0xA5: "R.Alt",
    0xBA: ";", 0xBB: "=", 0xBC: ",", 0xBD: "-", 0xBE: ".", 0xBF: "/",
    0xC0: "`", 0xDB: "[", 0xDC: "\\", 0xDD: "]", 0xDE: "'",
}


def vk_name(vk: int) -> str:
    """Human-readable name for a VK code."""
    return VK_NAMES.get(vk, f"0x{vk:02X}" if vk else "始终")
