# -*- coding: utf-8 -*-
"""
SAOPlayerGUIDpsThemeMixin — eighth mixin extracted from SAOPlayerGUI
(round 46 of the sao_gui split refactor). 19 methods, ~227 lines.

Combines two related concerns that share the menu-refresh + overlay-
update pattern:

DPS cluster (16 methods, ~204 lines):
  Timeouts + idle reset:
    * _combat_damage_timeout_s — user-configurable fade timeout
    * _boss_hp_hold_timeout_s — boss HP hold timeout (DPS-relative)
    * _cancel_dps_idle_reset_after — cancel pending idle reset
    * _schedule_dps_idle_reset_after_fade — schedule idle reset
  Snapshot factories:
    * _empty_dps_snapshot (staticmethod) — zero-state snapshot
    * _request_dps_live_snapshot — current live snapshot from tracker
    * _get_dps_last_report / _request_dps_last_report — last report
    * _request_dps_entity_detail — entity skill breakdown
  Report availability:
    * _get_dps_last_report_available — bool query
    * _sync_dps_report_availability — push availability to overlay +
      refresh menu when transitioning
  Overlay show paths:
    * _show_dps_live_snapshot — show live mode
    * _show_dps_last_report — show last-report mode
    * _show_last_dps_report_menu — menu handler (fallback alert if
      no report)
  Reset + toggle:
    * _reset_dps_tracker — full tracker reset
    * _toggle_dps_enabled — toggle the dps_enabled setting

Theme cluster (3 methods, ~33 lines):
  * _toggle_panel_theme(key) — cycle a single overlay's light/dark
  * _set_all_themes(theme) — apply same theme to every overlay
  * _apply_theme_to_overlay(key, theme) — internal helper used by
    both toggle paths

Required SAOPlayerGUI attrs:
  * self._dps_tracker, self._dps_overlay, self._dps_mode,
    self._dps_visible, self._dps_faded, self._dps_last_report_available
  * self._dps_idle_reset_after_id
  * self._THEME_OVERLAY_MAP (class attribute on SAOPlayerGUI)
  * self.root, self.settings, self._cfg_settings_ref, self._sao_menu

Required SAOPlayerGUI methods (via MRO):
  * _get_setting, _set_setting (SAOPlayerGUI)
  * _show_entity_alert (SAOPlayerGUI)
  * _refresh_menu_if_open, _refresh_menu_immediate (Menu mixin)
"""

from __future__ import annotations

from typing import Any, Optional


class SAOPlayerGUIDpsThemeMixin:
    """Mixin bundling the DPS + Theme toggle/setup helpers."""

    def _toggle_panel_theme(self, key: str) -> None:
        """切换某个面板的 Light/Dark 主题。"""
        cfg = self._cfg_settings_ref or self.settings
        themes = dict(cfg.get('panel_themes', {}))
        current = themes.get(key, 'dark')
        new_theme = 'dark' if current == 'light' else 'light'
        self._apply_theme_to_overlay(key, new_theme)
        themes[key] = new_theme
        cfg.set('panel_themes', themes)
        cfg.save()
        self._refresh_menu_immediate()

    def _set_all_themes(self, theme: str) -> None:
        """将所有面板设置为同一主题。"""
        cfg = self._cfg_settings_ref or self.settings
        themes = {}
        for key in self._THEME_OVERLAY_MAP:
            themes[key] = theme
            self._apply_theme_to_overlay(key, theme)
        cfg.set('panel_themes', themes)
        cfg.save()
        self._refresh_menu_immediate()

    def _apply_theme_to_overlay(self, key: str, theme: str) -> None:
        """将主题应用到 overlay 实例（如果已创建）。"""
        attr = self._THEME_OVERLAY_MAP.get(key, '')
        ov = getattr(self, attr, None)
        if ov is not None and hasattr(ov, '_apply_theme'):
            try:
                ov._apply_theme(theme)
            except Exception as e:
                print(f'[THEME] apply_to_overlay FAILED: key={key} err={e}')

    def _combat_damage_timeout_s(self) -> float:
        # User-configurable fade timeout. 0 means never fade; otherwise respect
        # the menu value so short-fade users are not pinned to the old 60s floor.
        try:
            raw = self._get_setting('dps_fade_timeout_s', 5)
            v = float(raw if raw is not None else 5)
        except Exception:
            v = 5.0
        if v <= 0:
            return 86400.0
        return float(max(1.0, v))

    def _cancel_dps_idle_reset_after(self):
        after_id = getattr(self, '_dps_idle_reset_after_id', None)
        if after_id:
            try:
                self.root.after_cancel(after_id)
            except Exception:
                pass
        self._dps_idle_reset_after_id = None

    def _schedule_dps_idle_reset_after_fade(self, timeout_s: float,
                                           expected_last_event_time: float = 0.0):
        self._cancel_dps_idle_reset_after()
        delay_ms = int(max(350, min(1500, float(timeout_s or 0.0) * 1000)))

        def _reset_if_still_idle():
            self._dps_idle_reset_after_id = None
            tracker = getattr(self, '_dps_tracker', None)
            if not tracker:
                return
            try:
                if tracker.has_recent_damage(0.2):
                    return
                if tracker.reset_idle_live_data(
                        'idle_timeout', expected_last_event_time):
                    self._sync_dps_report_availability()
            except Exception:
                pass

        try:
            self._dps_idle_reset_after_id = self.root.after(delay_ms, _reset_if_still_idle)
        except Exception:
            pass

    def _boss_hp_hold_timeout_s(self) -> float:
        """BossHP should survive death/revive and long mechanic downtime.

        v2.5.5: drop the 180 s floor — users reported BossHP refusing to fade
        for several minutes after an encounter went idle or after a map change.
        Now the user-set value wins (with a 1 s minimum); the only enforced
        floor is the dps fade timeout, so BB can never disappear before DPS.
        """
        try:
            raw = self._get_setting('boss_hp_hold_timeout_s', 5)
            v = float(raw if raw is not None else 5)
        except Exception:
            v = 5.0
        if v <= 0:
            return 86400.0
        return float(max(1.0, v, self._combat_damage_timeout_s()))

    @staticmethod
    def _empty_dps_snapshot():
        return {
            'encounter_active': False,
            'elapsed_s': 0.0,
            'total_damage': 0,
            'total_heal': 0,
            'total_dps': 0,
            'total_hps': 0,
            'entities': [],
        }

    def _get_dps_last_report_available(self) -> bool:
        tracker = getattr(self, '_dps_tracker', None)
        if not tracker:
            return False
        try:
            return bool(tracker.has_last_report())
        except Exception:
            return False

    def _sync_dps_report_availability(self):
        available = self._get_dps_last_report_available()
        if self._dps_overlay:
            self._dps_overlay.set_report_available(available)
        if available == self._dps_last_report_available:
            return
        self._dps_last_report_available = available
        if getattr(self, '_sao_menu', None) is not None:
            self._refresh_menu_if_open()

    def _request_dps_live_snapshot(self):
        tracker = getattr(self, '_dps_tracker', None)
        self._dps_mode = 'live'
        if tracker:
            try:
                return tracker.get_snapshot() or self._empty_dps_snapshot()
            except Exception:
                pass
        return self._empty_dps_snapshot()

    def _get_dps_last_report(self):
        tracker = getattr(self, '_dps_tracker', None)
        if not tracker:
            return None
        try:
            return tracker.get_last_report()
        except Exception:
            return None

    def _request_dps_last_report(self):
        report = self._get_dps_last_report()
        if report:
            self._dps_mode = 'report'
            self._dps_visible = True
            self._dps_faded = False
        return report

    def _request_dps_entity_detail(self, uid):
        """Fetch detail+skill breakdown for `uid`. Returns dict or None.

        Mirrors sao_webview.py DpsAPI.get_entity_detail. Called by the
        entity DpsOverlay when the user clicks an entity row in live mode.
        """
        tracker = getattr(self, '_dps_tracker', None)
        if not tracker:
            return None
        try:
            return tracker.get_entity_detail(int(uid or 0))
        except Exception:
            return None

    def _show_dps_live_snapshot(self, snapshot=None):
        if not self._dps_overlay:
            return False
        if snapshot is None:
            snapshot = self._request_dps_live_snapshot()
        self._dps_visible = True
        self._dps_faded = False
        self._dps_mode = 'live'
        self._dps_overlay.show_live(snapshot)
        return True

    def _show_dps_last_report(self, report=None) -> bool:
        if not self._dps_overlay:
            return False
        if report is None:
            report = self._request_dps_last_report()
        if not report:
            self._sync_dps_report_availability()
            return False
        self._dps_visible = True
        self._dps_faded = False
        self._dps_mode = 'report'
        self._dps_overlay.set_report_available(True)
        return bool(self._dps_overlay.show_last_report(report))

    def _reset_dps_tracker(self):
        tracker = getattr(self, '_dps_tracker', None)
        if tracker:
            try:
                tracker.reset()
            except Exception:
                pass
        self._sync_dps_report_availability()
        self._dps_mode = 'live'
        return self._request_dps_live_snapshot()

    def _show_last_dps_report_menu(self):
        if self._show_dps_last_report():
            return
        self._show_entity_alert(
            'DPS METER',
            '暂无上一场战斗报告 / No last combat report yet.',
            display_time=3.0,
        )

    # ── Sound / Display 开关 ──

    def _toggle_dps_enabled(self):
        cur = bool(self._get_setting('dps_enabled', True))
        new = not cur
        self._set_setting('dps_enabled', new)
        self._dps_enabled = new
        if not new:
            self._dps_visible = False
            self._dps_faded = False
            self._dps_mode = 'hidden'
            if self._dps_overlay:
                self._dps_overlay.hide()
        self._refresh_menu_if_open()

