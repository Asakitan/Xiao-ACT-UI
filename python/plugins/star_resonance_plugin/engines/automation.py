# -*- coding: utf-8 -*-
# Headless automation entrypoint that reuses the shared auto-key engine.

import threading

from plugins.star_resonance_plugin.engines.auto_key_engine import AutoKeyEngine
from config import (
    DEFAULT_HOTKEYS,
    HOTKEY_FKEY_VK,
    HOTKEY_MOD_ALL_VKS,
    SettingsManager,
    parse_hotkey,
    select_hotkey_match,
)
class AutomationCore:
    def __init__(self, state_mgr, settings: SettingsManager):
        self.state_mgr = state_mgr
        self.settings = settings
        self.packet = None
        self.recognition = RecognitionEngine(state_mgr, settings)
        self.auto_key = AutoKeyEngine(state_mgr, settings)

        self._hk_listener = None
        self._hk_pressed = set()
        self._hk_actions = {}

    # F键虚拟键码表 — 共享表在 config.HOTKEY_FKEY_VK
    _FKEY_VK = HOTKEY_FKEY_VK

    def start(self):
        self.packet.start()
        self.recognition.start()
        self.auto_key.start()
        self._setup_hotkeys()
        print("[Automation] packet + vision + auto-key started")

    def stop(self):
        try:
            self.auto_key.stop()
        except Exception:
            pass
        try:
            self.packet.stop()
        except Exception:
            pass
        try:
            self.recognition.stop()
        except Exception:
            pass
        if self._hk_listener:
            try:
                self._hk_listener.stop()
            except Exception:
                pass
        print("[Automation] stopped")

    def toggle_capture(self):
        if self.recognition._running:
            self.recognition.stop()
            print("[Automation] recognition OFF")
        else:
            self.recognition.start()
            print("[Automation] recognition ON")

    def toggle_auto(self):
        config = self.settings.get("auto_key", {}) or {}
        config["enabled"] = not bool(config.get("enabled", False))
        self.settings.set("auto_key", config)
        self.settings.save()
        self.auto_key.invalidate()
        print(f"[Automation] auto-key {'ON' if config['enabled'] else 'OFF'}")

    def _setup_hotkeys(self):
        self._hk_actions = {
            "toggle_recognition": self.toggle_capture,
            "toggle_auto_script": self.toggle_auto,
        }
        try:
            from pynput.keyboard import Listener as KbListener, Key, KeyCode

            self._hk_Key = Key
            self._hk_KeyCode = KeyCode
            self._hk_listener = KbListener(
                on_press=self._hk_on_press,
                on_release=self._hk_on_release,
            )
            self._hk_listener.daemon = True
            self._hk_listener.start()
            print("[Automation] hotkeys ready: F5=toggle_recognition, F6=toggle_auto_script")
        except Exception as exc:
            print(f"[Automation] hotkeys unavailable: {exc}")

    def _hk_on_press(self, key):
        try:
            if isinstance(key, self._hk_KeyCode) and key.vk:
                self._hk_pressed.add(key.vk)
            elif isinstance(key, self._hk_Key):
                vk = key.value.vk if hasattr(key.value, "vk") else str(key)
                self._hk_pressed.add(vk)
        except Exception:
            pass
        self._hk_check()

    def _hk_on_release(self, key):
        try:
            if isinstance(key, self._hk_KeyCode) and key.vk:
                self._hk_pressed.discard(key.vk)
            elif isinstance(key, self._hk_Key):
                vk = key.value.vk if hasattr(key.value, "vk") else str(key)
                self._hk_pressed.discard(vk)
        except Exception:
            pass

    def _hk_check(self):
        # 组合键支持 (与 SAOHotkeyManager / sao_webview 同一套 config 共享
        # 逻辑): 候选收齐后 select_hotkey_match 取最特异命中。内置键与保存
        # 值 merge — 部分 dict 不能把内置默认杀掉。
        saved = self.settings.get("hotkeys", {}) or {}
        merged = {**DEFAULT_HOTKEYS, **saved} if isinstance(saved, dict) \
            else dict(DEFAULT_HOTKEYS)
        candidates = []
        for action, info in merged.items():
            cb = self._hk_actions.get(action)
            if cb is None:
                continue
            parsed = parse_hotkey(info)
            if parsed:
                candidates.append((parsed, cb))
        cb = select_hotkey_match(candidates, self._hk_pressed)
        if cb:
            threading.Thread(target=cb, daemon=True).start()
            # 只清主键、保留修饰键 (pynput 不会为按住的 Ctrl 重发事件)
            self._hk_pressed = {k for k in self._hk_pressed
                                if k in HOTKEY_MOD_ALL_VKS}
