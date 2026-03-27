# -*- coding: utf-8 -*-
"""Headless automation entrypoint that reuses the shared auto-key engine."""

import threading

from auto_key_engine import AutoKeyEngine
from config import DEFAULT_HOTKEYS, SettingsManager
from game_state import GameStateManager
from packet_bridge import PacketBridge
from recognition import RecognitionEngine


class AutomationCore:
    def __init__(self, state_mgr: GameStateManager, settings: SettingsManager):
        self.state_mgr = state_mgr
        self.settings = settings
        self.packet = PacketBridge(state_mgr, settings)
        self.recognition = RecognitionEngine(state_mgr, settings)
        self.auto_key = AutoKeyEngine(state_mgr, settings)

        self._hk_listener = None
        self._hk_pressed = set()
        self._hk_actions = {}

    _FKEY_VK = {
        "F1": 112, "F2": 113, "F3": 114, "F4": 115,
        "F5": 116, "F6": 117, "F7": 118, "F8": 119,
        "F9": 120, "F10": 121, "F11": 122, "F12": 123,
    }

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
        saved = self.settings.get("hotkeys", DEFAULT_HOTKEYS)
        for action, info in saved.items():
            vk = None
            if isinstance(info, dict):
                vk = info.get("vk")
            elif isinstance(info, str) and info:
                vk = self._FKEY_VK.get(info.upper())
            if vk and vk in self._hk_pressed:
                cb = self._hk_actions.get(action)
                if cb:
                    threading.Thread(target=cb, daemon=True).start()
                    self._hk_pressed.clear()
                    return
