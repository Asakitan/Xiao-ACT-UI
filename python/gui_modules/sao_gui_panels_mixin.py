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

import math
import time
from typing import Any, Optional

from utils.perf_probe import probe as _probe
from gui_modules.sao_gui_action_log import ActionLogPanel
from gui_modules.sao_gui_act_aggregate import ActAggregatePanel
from gui_modules.sao_gui_mem_scope import MemScopePanel
from gui_modules.sao_gui_data_source_health import DataSourceHealthPanel
from gui_modules.sao_gui_graph_timeseries import GraphTimeseriesPanel
from gui_modules.sao_gui_offline_import import OfflineImportPanel
from gui_modules.sao_gui_plugin_manager import PluginDetachedPanel, PluginManagerPanel
from gui_modules.sao_gui_report_export import ReportExportPanel
from gui_modules.sao_gui_skill_drilldown import SkillDrilldownPanel
from gui_modules.sao_gui_timeline_vcr import TimelineVcrPanel
# Game panels (Commander/CombatantDrilldown/DeathRecap/TriggerTimer) loaded lazily
from gui_modules.sao_panel_ui import _set_sao_panel_theme


def _finite_int(value: Any, default: int = 0, *, lo: Optional[int] = None, hi: Optional[int] = None) -> int:
    try:
        num = float(default if value is None or value == '' else value)
    except Exception:
        num = float(default or 0)
    if not math.isfinite(num):
        num = float(default or 0)
    if lo is not None:
        num = max(float(lo), num)
    if hi is not None:
        num = min(float(hi), num)
    return int(num)


class SAOPlayerGUIPanelsMixin:
    """Mixin bundling commander + panel-visibility + settings toggles."""

    def _toggle_commander_panel(self):
        """Commander 面板 — 游戏插件覆盖。"""
        pass

    def _toggle_ai_editor_panel(self):
        """启动独立 AI Editor GUI 窗口 (pywebview)."""
        self._dismiss_sao_menu_for_panel()
        try:
            from ai_editor.app import launch
            launch(gui_ref=self)
        except Exception as exc:
            print(f"[AIEditor] launch failed: {exc}")
            # Fallback to Tk panel
            try:
                if not self._ai_editor_panel:
                    from gui_modules.sao_gui_ai_editor import AIEditorPanel
                    self._ai_editor_panel = AIEditorPanel(self.root, self)
                if self._ai_editor_panel.is_visible():
                    self._ai_editor_panel.hide()
                else:
                    self._ai_editor_panel.show()
                    self.root.after(120, lambda: self._raise_panel_window(self._ai_editor_panel))
            except Exception:
                pass

    def _toggle_act_plugin_manager_panel(self):
        """打开/关闭 ACT 插件管理面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_plugin_manager_panel:
            self._act_plugin_manager_panel = PluginManagerPanel(self.root, self)
            self._apply_act_panel_theme()
        if self._act_plugin_manager_panel.is_visible():
            self._act_plugin_manager_panel.hide()
        else:
            self._act_plugin_manager_panel.show()
            self._apply_act_panel_theme()
            self.root.after(120, lambda: self._raise_panel_window(self._act_plugin_manager_panel))

    def _open_act_plugin_manager(self, tab='manage'):
        """打开 ACT 插件管理面板并切到指定页签 (manage / panels)."""
        self._dismiss_sao_menu_for_panel()
        self._ensure_plugin_window_bridge()
        if not self._act_plugin_manager_panel:
            self._act_plugin_manager_panel = PluginManagerPanel(self.root, self)
            self._apply_act_panel_theme()
        panel = self._act_plugin_manager_panel
        try:
            panel._active_tab = tab if tab in ('manage', 'panels') else 'manage'
        except Exception:
            pass
        panel.show()
        self._apply_act_panel_theme()
        self.root.after(120, lambda: self._raise_panel_window(panel))

    def _ensure_plugin_window_bridge(self):
        """订阅 ``plugin_open_window``：让插件用 ``ctx.open_window`` 弹出独立窗口。

        幂等。插件按钮动作在 Tk 主线程触发，发布的事件在此同线程回调；为稳妥仍
        marshal 到 ``root.after``。任何面板可见前都会先经过 manager / 分离面板 /
        popup 三个入口之一，故此处必被激活。
        """
        if getattr(self, '_plugin_window_bridge_token', None):
            return
        try:
            from act_platform.runtime import ensure_act_event_bus
            bus = ensure_act_event_bus(self)

            def _on_open(event):
                payload = (event or {}).get('payload') or {}
                pid = str(payload.get('plugin_id') or '')
                if not pid:
                    return
                panel_id = str(payload.get('panel_id') or '')
                w = _finite_int(payload.get('width'), 0, lo=0)
                h = _finite_int(payload.get('height'), 0, lo=0)
                try:
                    self.root.after(0, lambda: self._open_plugin_detached_panel(pid, panel_id, w, h))
                except Exception:
                    pass

            self._plugin_window_bridge_token = bus.subscribe(
                'plugin_open_window', _on_open, owner_id='entity_plugin_window_bridge')
        except Exception:
            self._plugin_window_bridge_token = ''

    def _open_plugin_detached_panel(self, plugin_id, panel_id='', width=0, height=0):
        """打开某插件某面板的独立窗口 (一个面板一个窗口；尺寸由插件声明，缺省则默认)."""
        self._dismiss_sao_menu_for_panel()
        self._ensure_plugin_window_bridge()
        panels = getattr(self, '_plugin_detached_panels', None)
        if panels is None:
            panels = {}
            self._plugin_detached_panels = panels
        key = (str(plugin_id or ''), str(panel_id or ''))
        panel = panels.get(key)
        if panel is None or not panel._exists():
            panel = PluginDetachedPanel(self.root, self, plugin_id,
                                        panel_id=str(panel_id or ''),
                                        width=_finite_int(width, 0, lo=0),
                                        height=_finite_int(height, 0, lo=0))
            panels[key] = panel
        panel.show()
        try:
            self.root.after(120, lambda: self._raise_panel_window(panel))
        except Exception:
            pass

    def _toggle_plugin_enabled(self, plugin_id, enabled):
        """启用/禁用某个插件 (供 popup 菜单调用)."""
        from act_platform.runtime import act_plugin_disable, act_plugin_enable
        try:
            if enabled:
                act_plugin_disable(self, plugin_id)
            else:
                act_plugin_enable(self, plugin_id)
        except Exception:
            pass
        panel = getattr(self, '_act_plugin_manager_panel', None)
        if panel is not None:
            try:
                panel.refresh()
            except Exception:
                pass
        try:
            self._refresh_menu_if_open()
        except Exception:
            pass

    def _show_plugin_popup_menu(self):
        """SAO 插件 popup 菜单 — 插件管理 + 各插件(打开面板/启停/置顶)."""
        import tkinter as tk

        from act_platform.runtime import act_plugin_menu
        self._dismiss_sao_menu_for_panel()
        try:
            data = act_plugin_menu(self)
        except Exception:
            data = {'ok': False, 'plugins': []}
        menu = tk.Menu(self.root, tearoff=0)
        menu.add_command(label='⚙ 插件管理面板 Manage',
                         command=lambda: self._open_act_plugin_manager('manage'))
        menu.add_command(label='⬢ 插件面板 Panels',
                         command=lambda: self._open_act_plugin_manager('panels'))
        menu.add_separator()
        items = list(data.get('plugins') or [])
        if not items:
            menu.add_command(label='(未发现插件 No plugins)', state='disabled')
        for it in items:
            pid = str(it.get('id') or '')
            name = str(it.get('label') or pid)
            enabled = bool(it.get('enabled'))
            pinned = bool(it.get('pinned'))
            sub = tk.Menu(menu, tearoff=0)
            sub.add_command(label=('禁用 Disable' if enabled else '启用 Enable'),
                            command=lambda p=pid, en=enabled: self._toggle_plugin_enabled(p, en))
            sub.add_command(label=('取消置顶 Unpin' if pinned else '置顶 Pin'),
                            command=lambda p=pid, pn=pinned: self._pin_plugin_from_menu(p, not pn))
            # Only plugins that DECLARE a panel get the open-panel entry —
            # opens that plugin's own detached panel (not the manager).
            if it.get('declares_panel'):
                sub.add_command(label='打开面板 Open panel',
                                command=lambda p=pid: self._open_plugin_detached_panel(p))
            prefix = ('★ ' if pinned else '') + ('● ' if it.get('active') else ('◐ ' if enabled else '○ '))
            menu.add_cascade(label=prefix + name, menu=sub)
        try:
            x = self.root.winfo_pointerx()
            y = self.root.winfo_pointery()
            menu.tk_popup(x, y)
        finally:
            try:
                menu.grab_release()
            except Exception:
                pass

    def _toggle_act_trigger_timer_panel(self):
        """打开/关闭 ACT 触发/计时管理面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_trigger_timer_panel:
            self._act_trigger_timer_panel = TriggerTimerManagerPanel(self.root, self)
            self._apply_act_panel_theme()
        if self._act_trigger_timer_panel.is_visible():
            self._act_trigger_timer_panel.hide()
        else:
            self._act_trigger_timer_panel.show()
            self._apply_act_panel_theme()
            self.root.after(120, lambda: self._raise_panel_window(self._act_trigger_timer_panel))

    def _toggle_act_data_source_health_panel(self):
        """打开/关闭 ACT 数据源健康面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_data_source_health_panel:
            self._act_data_source_health_panel = DataSourceHealthPanel(self.root, self)
            self._apply_act_panel_theme()
        if self._act_data_source_health_panel.is_visible():
            self._act_data_source_health_panel.hide()
        else:
            self._act_data_source_health_panel.show()
            self._apply_act_panel_theme()
            self.root.after(120, lambda: self._raise_panel_window(self._act_data_source_health_panel))

    def _toggle_act_report_export_panel(self):
        """打开/关闭 ACT 报告/导出面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_report_export_panel:
            self._act_report_export_panel = ReportExportPanel(self.root, self)
            self._apply_act_panel_theme()
        if self._act_report_export_panel.is_visible():
            self._act_report_export_panel.hide()
        else:
            self._act_report_export_panel.show()
            self._apply_act_panel_theme()
            self.root.after(120, lambda: self._raise_panel_window(self._act_report_export_panel))

    def _toggle_act_offline_import_panel(self):
        """打开/关闭 ACT 离线导入面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_offline_import_panel:
            self._act_offline_import_panel = OfflineImportPanel(self.root, self)
            self._apply_act_panel_theme()
        if self._act_offline_import_panel.is_visible():
            self._act_offline_import_panel.hide()
        else:
            self._act_offline_import_panel.show()
            self._apply_act_panel_theme()
            self.root.after(120, lambda: self._raise_panel_window(self._act_offline_import_panel))

    def _toggle_act_timeline_vcr_panel(self):
        """打开/关闭 ACT 时间线/VCR 面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_timeline_vcr_panel:
            self._act_timeline_vcr_panel = TimelineVcrPanel(self.root, self)
            self._apply_act_panel_theme()
        if self._act_timeline_vcr_panel.is_visible():
            self._act_timeline_vcr_panel.hide()
        else:
            self._act_timeline_vcr_panel.show()
            self._apply_act_panel_theme()
            self.root.after(120, lambda: self._raise_panel_window(self._act_timeline_vcr_panel))

    def _toggle_act_action_log_panel(self):
        """打开/关闭 ACT 行为日志面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_action_log_panel:
            self._act_action_log_panel = ActionLogPanel(self.root, self)
            self._apply_act_panel_theme()
        if self._act_action_log_panel.is_visible():
            self._act_action_log_panel.hide()
        else:
            self._act_action_log_panel.show()
            self._apply_act_panel_theme()
            self.root.after(120, lambda: self._raise_panel_window(self._act_action_log_panel))

    def _toggle_act_aggregate_panel(self):
        """打开/关闭 ACT 聚合总览面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_aggregate_panel:
            self._act_aggregate_panel = ActAggregatePanel(self.root, self)
            self._apply_act_panel_theme()
        if self._act_aggregate_panel.is_visible():
            self._act_aggregate_panel.hide()
        else:
            self._act_aggregate_panel.show()
            self._apply_act_panel_theme()
            self.root.after(120, lambda: self._raise_panel_window(self._act_aggregate_panel))

    def _toggle_mem_scope_panel(self):
        """打开/关闭 内存浏览器 Mem Scope 面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._mem_scope_panel:
            self._mem_scope_panel = MemScopePanel(self.root, self)
            self._apply_act_panel_theme()
        if self._mem_scope_panel.is_visible():
            self._mem_scope_panel.hide()
        else:
            self._mem_scope_panel.show()
            self._apply_act_panel_theme()
            self.root.after(120, lambda: self._raise_panel_window(self._mem_scope_panel))

    def _toggle_act_death_recap_panel(self):
        """打开/关闭 ACT 死亡回放面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_death_recap_panel:
            self._act_death_recap_panel = DeathRecapPanel(self.root, self)
            self._apply_act_panel_theme()
        if self._act_death_recap_panel.is_visible():
            self._act_death_recap_panel.hide()
        else:
            self._act_death_recap_panel.show()
            self._apply_act_panel_theme()
            self.root.after(120, lambda: self._raise_panel_window(self._act_death_recap_panel))

    def _toggle_act_graph_timeseries_panel(self):
        """打开/关闭 ACT 图表/曲线面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_graph_timeseries_panel:
            self._act_graph_timeseries_panel = GraphTimeseriesPanel(self.root, self)
            self._apply_act_panel_theme()
        if self._act_graph_timeseries_panel.is_visible():
            self._act_graph_timeseries_panel.hide()
        else:
            self._act_graph_timeseries_panel.show()
            self._apply_act_panel_theme()
            self.root.after(120, lambda: self._raise_panel_window(self._act_graph_timeseries_panel))

    def _toggle_act_combatant_drilldown_panel(self):
        """打开/关闭 ACT 战斗成员钻取面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_combatant_drilldown_panel:
            self._act_combatant_drilldown_panel = CombatantDrilldownPanel(self.root, self)
            self._apply_act_panel_theme()
        if self._act_combatant_drilldown_panel.is_visible():
            self._act_combatant_drilldown_panel.hide()
        else:
            self._act_combatant_drilldown_panel.show()
            self._apply_act_panel_theme()
            self.root.after(120, lambda: self._raise_panel_window(self._act_combatant_drilldown_panel))

    def _toggle_act_skill_drilldown_panel(self):
        """打开/关闭 ACT 技能钻取面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_skill_drilldown_panel:
            self._act_skill_drilldown_panel = SkillDrilldownPanel(self.root, self)
            self._apply_act_panel_theme()
        if self._act_skill_drilldown_panel.is_visible():
            self._act_skill_drilldown_panel.hide()
        else:
            self._act_skill_drilldown_panel.show()
            self._apply_act_panel_theme()
            self.root.after(120, lambda: self._raise_panel_window(self._act_skill_drilldown_panel))

    def _push_commander_data(self):
        """Commander data push — 游戏插件覆盖。"""
        pass

    def _toggle_hide_all_panels(self):
        """一键隐藏/显示所有浮动面板 (不销毁, 只是 withdraw/deiconify)"""
        panels = []
        for attr in vars(self):
            if attr.endswith('_panel') and attr.startswith('_'):
                obj = getattr(self, attr, None)
                if obj is not None:
                    win = getattr(obj, '_win', obj)
                    panels.append((attr.strip('_'), win))

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
        'act':     '',
        'buffmon': '',   # self/boss 一对 overlay, theme mixin 内特例分发
    }

    _ACT_PANEL_ATTRS = (
        '_act_plugin_manager_panel',
        '_act_trigger_timer_panel',
        '_act_data_source_health_panel',
        '_act_report_export_panel',
        '_act_offline_import_panel',
        '_act_timeline_vcr_panel',
        '_act_aggregate_panel',
        '_act_action_log_panel',
        '_act_death_recap_panel',
        '_act_graph_timeseries_panel',
        '_act_combatant_drilldown_panel',
        '_act_skill_drilldown_panel',
        '_mem_scope_panel',
    )

    def _act_panel_theme(self) -> str:
        try:
            cfg = self._cfg_settings_ref or self.settings
            themes = dict(cfg.get('panel_themes', {}) or {})
            return 'light' if themes.get('act') == 'light' else 'dark'
        except Exception:
            return 'dark'

    def _apply_act_panel_theme(self, theme: Optional[str] = None) -> None:
        """Apply the grouped ACT panel theme to all registered Tk ACT panels."""
        theme = 'light' if str(theme or self._act_panel_theme()).lower() == 'light' else 'dark'
        try:
            _set_sao_panel_theme(theme)
        except Exception:
            pass
        for attr in self._ACT_PANEL_ATTRS:
            panel = getattr(self, attr, None)
            win = getattr(panel, '_win', None)
            try:
                if win and win.winfo_exists():
                    _set_sao_panel_theme(theme, win)
            except Exception:
                pass

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
        """Buff 监视器切换 — 游戏插件覆盖。"""
        pass

    def _cycle_boss_bar_mode(self):
        """Boss 血条模式切换 — 游戏插件覆盖。"""
        pass

    def _get_mem_data_source(self) -> str:
        return str(self._get_setting('mem_data_source', 'tcp') or 'tcp').strip().lower()

    def _cycle_mem_data_source(self):
        """数据源切换 — 游戏插件覆盖。"""
        pass

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

