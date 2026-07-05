# -*- coding: utf-8 -*-
# mp_input — ctypes 扫描码键盘注入后端（`keyboard` 库 API 兼容垫片）。
#
# SAO-UI 不依赖 PyPI 的 `keyboard` 库，且扫描码注入对
# 游戏（DirectInput / RawInput 读扫描码）兼容性最好。本模块只暴露 mp_player.py
# 真正用到的两个函数——`press(name)` / `release(name)`——签名与 `keyboard` 库一致，
# 因此 `import mp_input as keyboard` 后，KeyboardSimulator 的全部落键调用零改动直通。
#
# 实现参考主程序 engines/auto_key_engine.py 的 SendInput 用法，但改走
# KEYEVENTF_SCANCODE（wVk=0, wScan=扫描码），即发送物理扫描码而非虚拟键码。

from __future__ import annotations

import ctypes
from ctypes import wintypes

# ── SendInput 结构体 ───────────────────────────────────────────────────────
PUL = ctypes.POINTER(ctypes.c_ulong)


class _KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", wintypes.WORD),
        ("wScan", wintypes.WORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", PUL),
    ]


class _INPUT_UNION(ctypes.Union):
    _fields_ = [("ki", _KEYBDINPUT)]


class _INPUT(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _INPUT_UNION)]


_INPUT_KEYBOARD = 1
_KEYEVENTF_EXTENDEDKEY = 0x0001
_KEYEVENTF_KEYUP = 0x0002
_KEYEVENTF_SCANCODE = 0x0008

try:
    _user32 = ctypes.WinDLL("user32", use_last_error=True)
    _SendInput = _user32.SendInput
    _SendInput.argtypes = (wintypes.UINT, ctypes.POINTER(_INPUT), ctypes.c_int)
    _SendInput.restype = wintypes.UINT
    AVAILABLE = True
except Exception:  # pragma: no cover - 非 Windows
    _user32 = None
    _SendInput = None
    AVAILABLE = False

# ── 名称 → Set 1 扫描码（US 布局 make code）─────────────────────────────────
# 覆盖游戏电子琴用到的全部物理键：z-m / a-j / q-u / i o p [ ] / 1-0 与修饰键。
_SCAN = {
    # 数字行黑键
    "1": 0x02, "2": 0x03, "3": 0x04, "4": 0x05, "5": 0x06,
    "6": 0x07, "7": 0x08, "8": 0x09, "9": 0x0A, "0": 0x0B,
    "-": 0x0C, "=": 0x0D,
    # QWERTY 行（高音区白键 + 黑键 i o p [ ]）
    "q": 0x10, "w": 0x11, "e": 0x12, "r": 0x13, "t": 0x14, "y": 0x15,
    "u": 0x16, "i": 0x17, "o": 0x18, "p": 0x19, "[": 0x1A, "]": 0x1B,
    # ASDF 行（中音区白键）
    "a": 0x1E, "s": 0x1F, "d": 0x20, "f": 0x21, "g": 0x22, "h": 0x23,
    "j": 0x24, "k": 0x25, "l": 0x26, ";": 0x27, "'": 0x28,
    # ZXCV 行（低音区白键）+ 扩展模式方向键 , .
    "z": 0x2C, "x": 0x2D, "c": 0x2E, "v": 0x2F, "b": 0x30, "n": 0x31,
    "m": 0x32, ",": 0x33, ".": 0x34, "/": 0x35,
    # 常用控制键（备用）
    "space": 0x39, "enter": 0x1C, "tab": 0x0F, "esc": 0x01, "backspace": 0x0E,
}

# 修饰键别名 → 扫描码（左右键都映成左键扫描码即可满足游戏切档）
_MODIFIERS = {
    "shift": 0x2A, "left shift": 0x2A, "lshift": 0x2A, "right shift": 0x36,
    "ctrl": 0x1D, "control": 0x1D, "left ctrl": 0x1D, "lctrl": 0x1D, "right ctrl": 0x1D,
    "alt": 0x38, "left alt": 0x38, "lalt": 0x38,
}

# 扩展键集合（需置 KEYEVENTF_EXTENDEDKEY）。本插件用到的键都不是扩展键，
# 但保留 right ctrl 以防将来扩展。
_EXTENDED = {0x36: False, 0x1D: False}


def _resolve(name) -> tuple:
    # 把 keyboard 库式名称解析成 (scan_code, is_extended)。返回 (None, False) 表示未知。
    if name is None:
        return None, False
    key = str(name).strip().lower()
    if key in _MODIFIERS:
        return _MODIFIERS[key], False
    # 单字符（含 '[' ']' ',' '.' 等）直接查
    if key in _SCAN:
        return _SCAN[key], False
    # keyboard 库允许传 'left shift' 之类已在 _MODIFIERS；其余未知键忽略
    return None, False


def _send(scan: int, keyup: bool, extended: bool = False) -> bool:
    if not AVAILABLE or scan is None:
        return False
    flags = _KEYEVENTF_SCANCODE
    if keyup:
        flags |= _KEYEVENTF_KEYUP
    if extended:
        flags |= _KEYEVENTF_EXTENDEDKEY
    inp = _INPUT(type=_INPUT_KEYBOARD,
                 ki=_KEYBDINPUT(wVk=0, wScan=scan, dwFlags=flags, time=0, dwExtraInfo=None))
    sent = _SendInput(1, ctypes.byref(inp), ctypes.sizeof(_INPUT))
    return sent == 1


# ── keyboard 库兼容 API ─────────────────────────────────────────────────────
def press(name) -> bool:
    # 按下（不释放）。与 keyboard.press 同名同义。
    scan, ext = _resolve(name)
    return _send(scan, keyup=False, extended=ext)


def release(name) -> bool:
    # 释放。与 keyboard.release 同名同义。
    scan, ext = _resolve(name)
    return _send(scan, keyup=True, extended=ext)


def tap(name, hold: float = 0.0):
    # 按下→（可选保持）→释放，便于独立测试。
    import time as _t
    press(name)
    if hold > 0:
        _t.sleep(hold)
    release(name)


def supported(name) -> bool:
    # 该名称是否可注入（用于自检 / 单测）。
    scan, _ = _resolve(name)
    return scan is not None


__all__ = ["press", "release", "tap", "supported", "AVAILABLE"]
