# -*- coding: utf-8 -*-
"""
SAOPlayerGUIEngineTogglesMixin — seventh mixin extracted from
SAOPlayerGUI (round 44 of the sao_gui split refactor). 11 methods,
~134 lines.

Two engine-on/off feature clusters that share the toggle + refresh
pattern:

HideSeek cluster (auto hide-and-seek):
  * _toggle_hide_seek — top-level on/off
  * _start_hide_seek — instantiates HideSeekEngine, locator, alert
  * _stop_hide_seek — engine stop + alert cleanup
  * _on_hide_seek_status — status callback (debug logs)
  * _schedule_hide_seek_alert_refresh — 50 s alert refresh tick
  * _refresh_hide_seek_alert — alert refresh worker

Burst cluster (burst-skill trigger):
  * _pick_burst_trigger_slot — cython state-machine delegate
  * _normalize_watched_skill_slots — cython delegate
  * _reset_burst_tracking_state — clears last_burst_* caches
  * _toggle_burst_enabled — toggles `burst_enabled` setting
  * _toggle_burst_slot — adds/removes a slot from `watched_skill_slots`

Required SAOPlayerGUI attrs:
  * self._hide_seek_engine, self._hide_seek_alert_active,
    self._hide_seek_alert_after_id
  * self._last_burst_slot, self._last_burst_ready,
    self._last_burst_slot_shown
  * self.root, self._destroyed

Required SAOPlayerGUI methods (via MRO):
  * _show_entity_alert (SAOPlayerGUI)
  * _refresh_menu_if_open (Menu mixin)
  * _get_setting, _set_setting (SAOPlayerGUI)
"""

from __future__ import annotations

from typing import Any, Optional

import _sao_cy_uihelpers as _CY_UI  # type: ignore[import-not-found]


class SAOPlayerGUIEngineTogglesMixin:
    """Mixin bundling HideSeek + Burst engine toggles + state."""

    def _pick_burst_trigger_slot(self, gs):
        # v2.4.31: state machine moved to _sao_cy_uihelpers.
        watched = self._get_setting(
            'watched_skill_slots', [1, 2, 3, 4, 5, 6, 7, 8, 9]) or []
        slots = getattr(gs, 'skill_slots', []) or []
        prev_slot = int(getattr(self, '_last_burst_slot', 0) or 0)
        chosen = int(_CY_UI.pick_burst_trigger_slot(slots, watched, prev_slot))
        self._last_burst_slot = chosen
        return chosen

    def _toggle_hide_seek(self):
        """切换自动躲猫猫引擎开关."""
        if self._hide_seek_engine and self._hide_seek_engine.running:
            self._stop_hide_seek()
        else:
            self._start_hide_seek()

    def _start_hide_seek(self):
        """启动自动躲猫猫并用 AlertOverlay 保持状态提示."""
        if self._hide_seek_engine and self._hide_seek_engine.running:
            return
        try:
            from hide_seek_engine import HideSeekEngine
            from utils.window_locator import WindowLocator

            locator = getattr(self, '_locator', None)
            if locator is None:
                locator = WindowLocator()

            engine = HideSeekEngine(locator=locator, on_status=self._on_hide_seek_status)
            engine.start()
            self._hide_seek_engine = engine
            self._hide_seek_alert_active = True
            self._show_entity_alert('AUTO HIDE & SEEK', '自动躲猫猫已启动', display_time=60.0)
            self._schedule_hide_seek_alert_refresh()
            self._refresh_menu_if_open()
        except Exception as exc:
            print(f'[SAO Entity] Hide&Seek start failed: {exc}')
            import traceback
            traceback.print_exc()
            self._hide_seek_engine = None
            self._hide_seek_alert_active = False
            self._show_entity_alert('AUTO HIDE & SEEK', '启动失败，请检查游戏窗口与模板资源', display_time=4.0)
            self._refresh_menu_if_open()

    def _stop_hide_seek(self, show_alert: bool = True):
        """停止自动躲猫猫并取消持久提示刷新."""
        self._hide_seek_alert_active = False
        aid = getattr(self, '_hide_seek_alert_after_id', None)
        if aid is not None:
            try:
                self.root.after_cancel(aid)
            except Exception:
                pass
            self._hide_seek_alert_after_id = None
        engine = self._hide_seek_engine
        self._hide_seek_engine = None
        if engine is not None:
            try:
                engine.stop()
            except Exception:
                pass
        if show_alert:
            self._show_entity_alert('AUTO HIDE & SEEK', '自动躲猫猫已关闭', display_time=3.2)
        self._refresh_menu_if_open()

    def _on_hide_seek_status(self, message: str, step: int):
        """Hide & Seek 状态回调 — 当前仅用于调试日志."""
        if message:
            print(f'[HideSeek] step={step}: {message}')

    def _schedule_hide_seek_alert_refresh(self):
        aid = getattr(self, '_hide_seek_alert_after_id', None)
        if aid is not None:
            try:
                self.root.after_cancel(aid)
            except Exception:
                pass
        self._hide_seek_alert_after_id = self.root.after(
            50000, self._refresh_hide_seek_alert)

    def _refresh_hide_seek_alert(self):
        self._hide_seek_alert_after_id = None
        if self._destroyed or not getattr(self, '_hide_seek_alert_active', False):
            return
        engine = getattr(self, '_hide_seek_engine', None)
        if engine is None:
            self._hide_seek_alert_active = False
            return
        if not engine.running:
            print('[SAO Entity] Hide&Seek engine thread is no longer running')
        self._show_entity_alert('AUTO HIDE & SEEK', '自动躲猫猫运行中', display_time=60.0)
        self._schedule_hide_seek_alert_refresh()

    # ── AutoKey config helpers ──
    def _normalize_watched_skill_slots(self, slots):
        # v2.4.31: cython helper.
        return _CY_UI.normalize_watched_skill_slots(slots)

    def _reset_burst_tracking_state(self):
        self._last_burst_ready = False
        self._last_burst_slot = 0
        self._last_burst_slot_shown = 0

    def _toggle_burst_enabled(self):
        cur = bool(self._get_setting('burst_enabled', True))
        self._set_setting('burst_enabled', not cur)
        self._reset_burst_tracking_state()
        self._refresh_menu_if_open()

    def _toggle_burst_slot(self, slot: int):
        watched = self._normalize_watched_skill_slots(
            self._get_setting('watched_skill_slots', [1, 2, 3, 4, 5, 6, 7, 8, 9])
        )
        if not watched:
            watched = [1]
        watched_set = set(watched)
        if slot in watched_set:
            if len(watched_set) == 1:
                self._show_entity_alert('BURST SKILLS', '至少保留一个技能槽', display_time=3.0)
                return
            watched_set.remove(slot)
        else:
            watched_set.add(slot)
        self._set_setting('watched_skill_slots', sorted(watched_set))
        self._reset_burst_tracking_state()
        self._refresh_menu_if_open()

