# -*- coding: utf-8 -*-
"""
SAOHotkeyManager — global F-key hotkey listener for SAO Entity GUI.

Extracted from sao_gui.py (round 25) to start the long-running refactor
that breaks the 9682-line monolith into focused submodules.

The class is unchanged from the original (same public surface, same
behaviour). Dependencies that live at sao_gui module level have been
re-imported here so this module is self-contained.
"""

from __future__ import annotations

import ctypes
import sys
import threading
from typing import Any, Dict

from config import (
    DEFAULT_HOTKEYS,
    HOTKEY_FKEY_VK,
    HOTKEY_MOD_ALL_VKS,
    parse_hotkey,
    select_hotkey_match,
)

# ── pynput is an optional runtime dependency. When absent, the hotkey
# listener silently no-ops so the rest of the GUI still runs. Mirrors
# the same fallback the original sao_gui.py used. ──
PYNPUT_HOTKEY_AVAILABLE = False
try:
    from pynput import keyboard as pynput_kb
    from pynput.keyboard import Key, KeyCode
    PYNPUT_HOTKEY_AVAILABLE = True
except ImportError:
    pynput_kb = None  # type: ignore[assignment]
    Key = None  # type: ignore[assignment]
    KeyCode = None  # type: ignore[assignment]


# pynput 1.8.1 on Python 3.11+ has a ctypes argument-validation bug in
# its internal _PeekMessage call (expects LP__PUMP_MSG instance but gets
# pointer to MSG). The Listener thread dies on first message-loop tick,
# spewing a noisy traceback to stderr. The error is harmless to our app
# — hotkeys silently fail to register but no other subsystem depends on
# them. Install a threading.excepthook that suppresses ONLY this exact
# traceback so the stderr stays clean.
_PYNPUT_EXCEPTHOOK_INSTALLED = False


def _install_pynput_excepthook() -> None:
    global _PYNPUT_EXCEPTHOOK_INSTALLED
    if _PYNPUT_EXCEPTHOOK_INSTALLED:
        return
    _PYNPUT_EXCEPTHOOK_INSTALLED = True
    prev_hook = threading.excepthook

    def _hook(args):
        exc_type = args.exc_type
        exc_value = args.exc_value
        thread_name = getattr(args.thread, 'name', '') if args.thread else ''
        # Match the exact pynput PeekMessage ctypes failure.
        if (exc_type is ctypes.ArgumentError
                and 'LP__PUMP_MSG' in (str(exc_value) or '')):
            # Quietly swallow — pynput listener thread is done, but our
            # _start() already has try/except so this is non-fatal.
            return
        if prev_hook:
            prev_hook(args)

    threading.excepthook = _hook


_install_pynput_excepthook()


class SAOHotkeyManager:
    """全局快捷键管理 (与 gui.py HotkeyPanel 逻辑一致).

    Tracks pressed keys via a pynput listener and fires the matching
    action callback from ``actions`` when the saved hotkey VK is in
    the pressed set. ``settings`` is duck-typed — any object with a
    ``get(key, default)`` method works (the original was a
    SAO-specific SettingsManager, but the class never relied on the
    concrete type).
    """

    # F键虚拟键码表 (Windows VK codes) — 共享表在 config.HOTKEY_FKEY_VK
    _FKEY_VK = HOTKEY_FKEY_VK

    def __init__(self, settings: Any, actions: Dict[str, Any],
                 hotkey_provider: Any = None):
        self.settings = settings
        self.actions = actions
        # Optional callable returning {action: {'vk': int} | {'key': 'F6'},
        # 'callback': fn} for dynamically-registered (plugin) hotkeys.
        self.hotkey_provider = hotkey_provider
        self._listener = None
        self._pressed_keys: set = set()
        self._start()

    def _start(self):
        if not PYNPUT_HOTKEY_AVAILABLE:
            return
        try:
            self._listener = pynput_kb.Listener(
                on_press=self._on_press, on_release=self._on_release)
            self._listener.daemon = True
            self._listener.start()
        except Exception:
            pass

    def _on_press(self, key):
        try:
            if isinstance(key, KeyCode) and key.vk:
                self._pressed_keys.add(key.vk)
            elif isinstance(key, Key):
                self._pressed_keys.add(key.value.vk if hasattr(key.value, 'vk') else str(key))
        except Exception:
            pass
        self._check_combos()

    def _on_release(self, key):
        try:
            if isinstance(key, KeyCode) and key.vk:
                self._pressed_keys.discard(key.vk)
            elif isinstance(key, Key):
                self._pressed_keys.discard(key.value.vk if hasattr(key.value, 'vk') else str(key))
        except Exception:
            pass

    def _check_combos(self):
        # 'F5' 与 'CTRL+F5' 等组合共存: 候选统一收集后由 select_hotkey_match
        # 取最特异命中 (修饰键最多者优先, 并列时先收集的内置绑定赢)。
        # 内置键与保存值做 merge — set_hotkey 可能写出只含插件项的部分
        # dict, 不 merge 会把内置 F5-F12 全杀掉。
        saved = self.settings.get('hotkeys', {}) or {}
        merged = {**DEFAULT_HOTKEYS, **saved} if isinstance(saved, dict) \
            else dict(DEFAULT_HOTKEYS)
        candidates = []
        for action, info in merged.items():
            cb = self.actions.get(action)
            if cb is None:
                continue
            parsed = parse_hotkey(info)
            if parsed:
                candidates.append((parsed, cb))
        # Dynamically-registered (plugin) hotkeys, re-resolved each press so
        # newly loaded plugins work without restarting the listener.
        if self.hotkey_provider is not None:
            try:
                dynamic = self.hotkey_provider() or {}
            except Exception:
                dynamic = {}
            for action, info in dynamic.items():
                if not isinstance(info, dict):
                    continue
                cb = info.get('callback')
                parsed = parse_hotkey(info)
                if parsed and cb:
                    candidates.append((parsed, cb))
        cb = select_hotkey_match(candidates, self._pressed_keys)
        if cb:
            cb()
            self._clear_pressed_main_keys()

    def _clear_pressed_main_keys(self):
        # 触发后只清主键、保留修饰键 — pynput 不会为仍按住的 Ctrl 重发
        # press 事件, 全清会让紧接着的下一个 Ctrl+F 组合丢失 Ctrl 状态。
        self._pressed_keys = {k for k in self._pressed_keys
                              if k in HOTKEY_MOD_ALL_VKS}

    def cleanup(self):
        # The listener thread is daemon=True so it auto-exits when the
        # main thread terminates. We can't call listener.stop() directly
        # because pynput 1.8.1's stop() can block forever when the
        # message-loop thread already crashed (see ctypes.ArgumentError
        # workaround above). Setting the reference to None and trusting
        # daemon-thread teardown is enough.
        self._listener = None
