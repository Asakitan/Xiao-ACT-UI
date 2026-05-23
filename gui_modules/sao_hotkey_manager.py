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

from typing import Any, Dict

from config import DEFAULT_HOTKEYS

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


class SAOHotkeyManager:
    """全局快捷键管理 (与 gui.py HotkeyPanel 逻辑一致).

    Tracks pressed keys via a pynput listener and fires the matching
    action callback from ``actions`` when the saved hotkey VK is in
    the pressed set. ``settings`` is duck-typed — any object with a
    ``get(key, default)`` method works (the original was a
    SAO-specific SettingsManager, but the class never relied on the
    concrete type).
    """

    # F键虚拟键码表 (Windows VK codes)
    _FKEY_VK = {
        'F1': 112, 'F2': 113, 'F3': 114, 'F4': 115,
        'F5': 116, 'F6': 117, 'F7': 118, 'F8': 119,
        'F9': 120, 'F10': 121, 'F11': 122, 'F12': 123,
    }

    def __init__(self, settings: Any, actions: Dict[str, Any]):
        self.settings = settings
        self.actions = actions
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
        saved = self.settings.get('hotkeys', DEFAULT_HOTKEYS)
        for action, info in saved.items():
            vk = None
            if isinstance(info, dict):
                vk = info.get('vk')
            elif isinstance(info, str) and info:
                vk = self._FKEY_VK.get(info.upper())
            if vk and vk in self._pressed_keys:
                cb = self.actions.get(action)
                if cb:
                    cb()
                    self._pressed_keys.clear()
                    return

    def cleanup(self):
        if self._listener:
            try:
                self._listener.stop()
            except Exception:
                pass
