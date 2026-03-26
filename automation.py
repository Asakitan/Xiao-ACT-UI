# -*- coding: utf-8 -*-
"""
SAO Auto — 自动化核心

职责:
  - 启动采集引擎
  - 管理热键
  - 状态汇总 → HUD
  - 预留自动动作接口
"""

import threading
import time
from typing import Optional

from config import SettingsManager, DEFAULT_HOTKEYS
from game_state import GameState, GameStateManager
from recognition import RecognitionEngine


class AutomationCore:
    """自动化核心调度器"""

    def __init__(self, state_mgr: GameStateManager, settings: SettingsManager):
        self.state_mgr = state_mgr
        self.settings = settings
        self.recognition = RecognitionEngine(state_mgr, settings)

        # 热键
        self._hk_listener = None
        self._hk_pressed = set()
        self._hk_actions = {}

        # 自动功能开关
        self._auto_enabled = False

        # 状态回调
        self.on_status_change = None

    def start(self):
        """启动采集引擎 + 热键监听"""
        self.recognition.start()
        self._setup_hotkeys()
        print('[自动化] 核心已启动')

    def stop(self):
        """停止所有"""
        self.recognition.stop()
        if self._hk_listener:
            try:
                self._hk_listener.stop()
            except Exception:
                pass
        print('[自动化] 核心已停止')

    def toggle_capture(self):
        """暂停/恢复采集"""
        if self.recognition._running:
            self.recognition.stop()
        else:
            self.recognition.start()

    def toggle_auto(self):
        """启用/禁用自动功能"""
        self._auto_enabled = not self._auto_enabled
        label = '开启' if self._auto_enabled else '关闭'
        print(f'[自动化] 自动功能: {label}')

    # ═══════════════════════════════════════
    #  热键
    # ═══════════════════════════════════════
    _FKEY_VK = {
        'F1': 112, 'F2': 113, 'F3': 114, 'F4': 115,
        'F5': 116, 'F6': 117, 'F7': 118, 'F8': 119,
        'F9': 120, 'F10': 121, 'F11': 122, 'F12': 123,
    }

    def _setup_hotkeys(self):
        self._hk_actions = {
            'toggle_hud':   lambda: print('[热键] 切换 HUD (TODO)'),
            'toggle_debug': lambda: print('[热键] 切换调试 (TODO)'),
            'toggle_auto':  self.toggle_auto,
        }
        try:
            from pynput.keyboard import Listener as KbListener, Key, KeyCode
            self._hk_Key = Key
            self._hk_KeyCode = KeyCode
            self._hk_listener = KbListener(
                on_press=self._hk_on_press, on_release=self._hk_on_release)
            self._hk_listener.daemon = True
            self._hk_listener.start()
            print('[自动化] 热键监听已启动')
        except Exception as e:
            print(f'[自动化] 热键不可用: {e}')

    def _hk_on_press(self, key):
        try:
            if isinstance(key, self._hk_KeyCode) and key.vk:
                self._hk_pressed.add(key.vk)
            elif isinstance(key, self._hk_Key):
                vk = key.value.vk if hasattr(key.value, 'vk') else str(key)
                self._hk_pressed.add(vk)
        except Exception:
            pass
        self._hk_check()

    def _hk_on_release(self, key):
        try:
            if isinstance(key, self._hk_KeyCode) and key.vk:
                self._hk_pressed.discard(key.vk)
            elif isinstance(key, self._hk_Key):
                vk = key.value.vk if hasattr(key.value, 'vk') else str(key)
                self._hk_pressed.discard(vk)
        except Exception:
            pass

    def _hk_check(self):
        saved = self.settings.get('hotkeys', DEFAULT_HOTKEYS)
        for action, info in saved.items():
            vk = None
            if isinstance(info, dict):
                vk = info.get('vk')
            elif isinstance(info, str) and info:
                vk = self._FKEY_VK.get(info.upper())
            if vk and vk in self._hk_pressed:
                cb = self._hk_actions.get(action)
                if cb:
                    threading.Thread(target=cb, daemon=True).start()
                    self._hk_pressed.clear()
                    return
