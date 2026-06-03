# -*- coding: utf-8 -*-
"""
SAOPlayerGUIPanelsMixin — ninth mixin extracted from SAOPlayerGUI
(round 47 of the sao_gui split refactor). 11 methods, ~190 lines.

Combines the remaining panel-toggle helpers and small settings
toggles that didn't have a natural home in earlier mixins:

Commander cluster (2):
  * _toggle_commander_panel — open/close the Commander floating panel
  * _push_commander_data — build a packet-driven snapshot + push to
    the panel (sig-cached at the GUI level)

Panel visibility (1):
  * _toggle_hide_all_panels — one-shot withdraw / restore of every
    floating panel (status, updater, autokey-quick/detail,
    bossraid-quick/detail, commander). Snapshots which panels were
    visible so restore can re-show only those.

Recognition (1):
  * _toggle_recognition_menu — menu entry that flips the
    recognition_active flag (or starts the engines on first toggle)

Settings toggles (7):
  * _toggle_sound_enabled — sound on/off (delegates to sao_sound)
  * _adj_sound_volume(delta) — sound volume +/-
  * _toggle_buffmon_enabled — toggles buffmon overlays
  * _cycle_boss_bar_mode — cycles boss_raid / always / off
  * _get_mem_data_source — read mem_data_source setting
  * _cycle_mem_data_source — cycle tcp/hybrid/auto/memory + alert
  * _toggle_topmost — toggles -topmost on the float + status panels

Required SAOPlayerGUI attrs:
  * self._commander_panel, self._last_commander_push_sig
  * self._status_panel, self._update_panel
  * self._autokey_panel, self._bossraid_panel,
    self._autokey_detail_panel, self._bossraid_detail_panel
  * self._panels_hidden, self._hidden_panels_snapshot
  * self._self_buff_overlay, self._boss_buff_overlay
  * self._float, self.root, self._packet_engine
  * self._recog_lock, self._recognition_active,
    self._recognition_engine, self._recognition_engines
  * self._fisheye_ov

Required SAOPlayerGUI methods (via MRO):
  * _dismiss_sao_menu_for_panel (Menu mixin)
  * _raise_panel_window (SAOPlayerGUI)
  * _maybe_stop_fisheye, _any_panel_open, _start_fisheye_overlay
    (Fisheye mixin)
  * _refresh_menu_if_open (Menu mixin)
  * _get_setting, _set_setting (SAOPlayerGUI)
  * _start_recognition, _reset_sta_offline_state,
    _reconfigure_data_engines, _update_status_panel,
    _show_entity_alert (SAOPlayerGUI)
"""

from __future__ import annotations

from typing import Any, Optional

from utils.perf_probe import probe as _probe
from gui_modules.sao_gui_action_log import ActionLogPanel
from gui_modules.sao_gui_commander import CommanderPanel
from gui_modules.sao_gui_combatant_drilldown import CombatantDrilldownPanel
from gui_modules.sao_gui_data_source_health import DataSourceHealthPanel
from gui_modules.sao_gui_graph_timeseries import GraphTimeseriesPanel
from gui_modules.sao_gui_plugin_manager import PluginManagerPanel
from gui_modules.sao_gui_report_export import ReportExportPanel
from gui_modules.sao_gui_skill_drilldown import SkillDrilldownPanel
from gui_modules.sao_gui_timeline_vcr import TimelineVcrPanel
from gui_modules.sao_gui_trigger_timer_manager import TriggerTimerManagerPanel


class SAOPlayerGUIPanelsMixin:
    """Mixin bundling commander + panel-visibility + settings toggles."""

    def _toggle_commander_panel(self):
        """打开/关闭 Commander 面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._commander_panel:
            self._commander_panel = CommanderPanel(self.root)
        if self._commander_panel.is_visible():
            self._commander_panel.hide()
        else:
            self._commander_panel.show()
            self._push_commander_data()
            self.root.after(120, lambda: self._raise_panel_window(self._commander_panel))

    def _toggle_act_plugin_manager_panel(self):
        """打开/关闭 ACT 插件管理面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_plugin_manager_panel:
            self._act_plugin_manager_panel = PluginManagerPanel(self.root, self)
        if self._act_plugin_manager_panel.is_visible():
            self._act_plugin_manager_panel.hide()
        else:
            self._act_plugin_manager_panel.show()
            self.root.after(120, lambda: self._raise_panel_window(self._act_plugin_manager_panel))

    def _toggle_act_trigger_timer_panel(self):
        """打开/关闭 ACT 触发/计时管理面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_trigger_timer_panel:
            self._act_trigger_timer_panel = TriggerTimerManagerPanel(self.root, self)
        if self._act_trigger_timer_panel.is_visible():
            self._act_trigger_timer_panel.hide()
        else:
            self._act_trigger_timer_panel.show()
            self.root.after(120, lambda: self._raise_panel_window(self._act_trigger_timer_panel))

    def _toggle_act_data_source_health_panel(self):
        """打开/关闭 ACT 数据源健康面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_data_source_health_panel:
            self._act_data_source_health_panel = DataSourceHealthPanel(self.root, self)
        if self._act_data_source_health_panel.is_visible():
            self._act_data_source_health_panel.hide()
        else:
            self._act_data_source_health_panel.show()
            self.root.after(120, lambda: self._raise_panel_window(self._act_data_source_health_panel))

    def _toggle_act_report_export_panel(self):
        """打开/关闭 ACT 报告/导出面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_report_export_panel:
            self._act_report_export_panel = ReportExportPanel(self.root, self)
        if self._act_report_export_panel.is_visible():
            self._act_report_export_panel.hide()
        else:
            self._act_report_export_panel.show()
            self.root.after(120, lambda: self._raise_panel_window(self._act_report_export_panel))

    def _toggle_act_timeline_vcr_panel(self):
        """打开/关闭 ACT 时间线/VCR 面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_timeline_vcr_panel:
            self._act_timeline_vcr_panel = TimelineVcrPanel(self.root, self)
        if self._act_timeline_vcr_panel.is_visible():
            self._act_timeline_vcr_panel.hide()
        else:
            self._act_timeline_vcr_panel.show()
            self.root.after(120, lambda: self._raise_panel_window(self._act_timeline_vcr_panel))

    def _toggle_act_action_log_panel(self):
        """打开/关闭 ACT 行为日志面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_action_log_panel:
            self._act_action_log_panel = ActionLogPanel(self.root, self)
        if self._act_action_log_panel.is_visible():
            self._act_action_log_panel.hide()
        else:
            self._act_action_log_panel.show()
            self.root.after(120, lambda: self._raise_panel_window(self._act_action_log_panel))

    def _toggle_act_graph_timeseries_panel(self):
        """打开/关闭 ACT 图表/曲线面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_graph_timeseries_panel:
            self._act_graph_timeseries_panel = GraphTimeseriesPanel(self.root, self)
        if self._act_graph_timeseries_panel.is_visible():
            self._act_graph_timeseries_panel.hide()
        else:
            self._act_graph_timeseries_panel.show()
            self.root.after(120, lambda: self._raise_panel_window(self._act_graph_timeseries_panel))

    def _toggle_act_combatant_drilldown_panel(self):
        """打开/关闭 ACT 战斗成员钻取面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_combatant_drilldown_panel:
            self._act_combatant_drilldown_panel = CombatantDrilldownPanel(self.root, self)
        if self._act_combatant_drilldown_panel.is_visible():
            self._act_combatant_drilldown_panel.hide()
        else:
            self._act_combatant_drilldown_panel.show()
            self.root.after(120, lambda: self._raise_panel_window(self._act_combatant_drilldown_panel))

    def _toggle_act_skill_drilldown_panel(self):
        """打开/关闭 ACT 技能钻取面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_skill_drilldown_panel:
            self._act_skill_drilldown_panel = SkillDrilldownPanel(self.root, self)
        if self._act_skill_drilldown_panel.is_visible():
            self._act_skill_drilldown_panel.hide()
        else:
            self._act_skill_drilldown_panel.show()
            self.root.after(120, lambda: self._raise_panel_window(self._act_skill_drilldown_panel))

    @_probe.decorate('ui.commander_push')
    def _push_commander_data(self):
        """Build + push a snapshot to the Commander panel."""
        if not self._commander_panel or not self._commander_panel.is_visible():
            return
        try:
            bridge = getattr(self, '_packet_engine', None)
            if bridge and hasattr(bridge, 'get_commander_data'):
                data = bridge.get_commander_data()
            else:
                data = {'members': [], 'team_id': 0,
                        'leader_uid': 0, 'dungeon_id': 0}
            _members = []
            for _m in list(data.get('members') or []):
                _slots = tuple(
                    (
                        int(_s.get('index') or 0),
                        str(_s.get('state') or ''),
                        round(float(_s.get('cooldown_pct') or 0.0), 3),
                        int(_s.get('remaining_ms') or 0),
                    )
                    for _s in list(_m.get('skill_slots') or [])
                )
                _members.append((
                    int(_m.get('uid') or 0),
                    str(_m.get('name') or ''),
                    str(_m.get('profession') or ''),
                    int(_m.get('fight_point') or 0),
                    int(_m.get('level') or 0),
                    bool(_m.get('is_self')),
                    bool(_m.get('is_leader')),
                    int(_m.get('hp') or 0),
                    int(_m.get('max_hp') or 0),
                    _slots,
                ))
            _sig = (
                int(data.get('team_id') or 0),
                int(data.get('leader_uid') or 0),
                int(data.get('dungeon_id') or 0),
                int(data.get('self_uid') or 0),
                tuple(_members),
            )
            if _sig != self._last_commander_push_sig:
                self._last_commander_push_sig = _sig
                self._commander_panel.update(data)
        except Exception:
            pass

    def _toggle_hide_all_panels(self):
        """一键隐藏/显示所有浮动面板 (不销毁, 只是 withdraw/deiconify)"""
        panels = [
            ('status',  self._status_panel),
            ('updater', self._update_panel),
            ('autokey_quick', getattr(self._autokey_panel, '_win', None)),
            ('bossraid_quick', getattr(self._bossraid_panel, '_win', None)),
            ('autokey_detail', getattr(self._autokey_detail_panel, '_win', None)),
            ('bossraid_detail', getattr(self._bossraid_detail_panel, '_win', None)),
            ('commander', getattr(self._commander_panel, '_win', None)),
            ('act_plugin_manager', getattr(self._act_plugin_manager_panel, '_win', None)),
            ('act_trigger_timer', getattr(self._act_trigger_timer_panel, '_win', None)),
            ('act_data_source_health', getattr(self._act_data_source_health_panel, '_win', None)),
            ('act_report_export', getattr(self._act_report_export_panel, '_win', None)),
            ('act_timeline_vcr', getattr(self._act_timeline_vcr_panel, '_win', None)),
            ('act_action_log', getattr(self._act_action_log_panel, '_win', None)),
            ('act_graph_timeseries', getattr(self._act_graph_timeseries_panel, '_win', None)),
            ('act_combatant_drilldown', getattr(self._act_combatant_drilldown_panel, '_win', None)),
            ('act_skill_drilldown', getattr(self._act_skill_drilldown_panel, '_win', None)),
        ]

        if not self._panels_hidden:
            # ── 隐藏 ──
            self._hidden_panels_snapshot = []
            for name, p in panels:
                try:
                    if p and p.winfo_exists():
                        self._hidden_panels_snapshot.append(name)
                        p.withdraw()
                except Exception:
                    pass
            self._panels_hidden = True
            self._maybe_stop_fisheye()
        else:
            # ── 恢复 ──
            for name, p in panels:
                try:
                    if name in self._hidden_panels_snapshot and p and p.winfo_exists():
                        p.deiconify()
                        p.lift()
                except Exception:
                    pass
            self._hidden_panels_snapshot = []
            self._panels_hidden = False
            # 面板恢复后确保鱼眼也恢复
            if self._fisheye_ov is None and self._any_panel_open():
                self.root.after(100, self._start_fisheye_overlay)

        self._refresh_menu_if_open()

    # ── Panel Theme ──

    _THEME_OVERLAY_MAP = {
        'dps':     '_dps_overlay',
        'hp':      '_hp_overlay',
        'bosshp':  '_boss_hp_overlay',
        'skillfx': '_skillfx_overlay',
        'alert':   '_alert_overlay',
    }

    def _toggle_recognition_menu(self):
        """切换识别开关 — SAO Entity UI."""
        with self._recog_lock:
            if not self._recognition_active:
                if not self._recognition_engine and not self._recognition_engines:
                    self._start_recognition()
                else:
                    self._recognition_active = True
            else:
                self._recognition_active = False
                self._reset_sta_offline_state()
        self._refresh_menu_if_open()

    def _toggle_sound_enabled(self):
        from utils.sao_sound import set_sound_enabled, get_sound_enabled
        new = not get_sound_enabled()
        set_sound_enabled(new)
        self._set_setting('sound_enabled', new)
        self._refresh_menu_if_open()

    def _adj_sound_volume(self, delta: int):
        from utils.sao_sound import set_sound_volume, get_sound_volume
        cur = get_sound_volume()
        nv = max(0, min(100, cur + delta))
        set_sound_volume(nv)
        self._set_setting('sound_volume', nv)

    def _toggle_buffmon_enabled(self):
        cur = bool(self._get_setting('buffmon_enabled', True))
        new = not cur
        self._set_setting('buffmon_enabled', new)
        for ov in (self._self_buff_overlay, self._boss_buff_overlay):
            try:
                if ov is not None:
                    ov.set_enabled(new)
            except Exception:
                pass
        self._refresh_menu_if_open()

    def _cycle_boss_bar_mode(self):
        modes = ['boss_raid', 'always', 'off']
        cur = self._get_setting('boss_bar_mode', 'boss_raid') or 'boss_raid'
        idx = modes.index(cur) if cur in modes else 0
        nxt = modes[(idx + 1) % len(modes)]
        self._set_setting('boss_bar_mode', nxt)
        self._refresh_menu_if_open()

    def _get_mem_data_source(self) -> str:
        mode = str(self._get_setting('mem_data_source', 'tcp') or 'tcp').strip().lower()
        return mode if mode in ('tcp', 'memory', 'hybrid', 'auto') else 'tcp'

    def _cycle_mem_data_source(self):
        modes = ['tcp', 'hybrid', 'auto', 'memory']
        cur = self._get_mem_data_source()
        nxt = modes[(modes.index(cur) + 1) % len(modes)]
        self._set_setting('mem_data_source', nxt)
        self._reconfigure_data_engines()
        self._update_status_panel()
        self._refresh_menu_if_open()
        labels = {'tcp': 'TCP', 'memory': 'MEM', 'hybrid': 'HYBRID', 'auto': 'AUTO'}
        self._show_entity_alert('DATA SOURCE', f'已切换到 {labels.get(nxt, nxt.upper())}', display_time=2.4)

    # ══════════════════════════════════════════════
    #  其他功能
    # ══════════════════════════════════════════════
    def _toggle_topmost(self):
        current = self._float.attributes('-topmost')
        new_val = not current
        self._float.attributes('-topmost', new_val)
        for panel in [self._status_panel, self._update_panel]:
            try:
                if panel and panel.winfo_exists():
                    panel.attributes('-topmost', new_val)
            except Exception:
                pass
        self._refresh_menu_if_open()

