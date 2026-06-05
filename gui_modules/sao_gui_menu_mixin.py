# -*- coding: utf-8 -*-
"""
SAOPlayerGUIMenuMixin — fourth mixin extracted from SAOPlayerGUI
(round 40 of the sao_gui split refactor). 17 methods, ~526 lines.

Holds the SAO PopUpMenu lifecycle + refresh + signature-caching logic:
  * ``_setup_sao_menu`` — constructs the SAOPopUpMenu with 7 categories.
  * ``_toggle_sao_menu`` — open/close handler (with motion blur + sounds).
  * ``_close_sao_menu_from_background`` / ``_clear_sao_menu_close_pending``
    — outside-click close path with fisheye z-order release.
  * ``_on_sao_menu_open`` / ``_on_sao_menu_close`` — open/close hooks
    that drive fisheye overlay + breath animation + entity-menu state
    persistence.
  * ``_refresh_menu_if_open`` / ``_refresh_menu_immediate`` —
    debounced + immediate refresh paths.
  * ``_apply_menu_refresh_if_open`` — main-thread continuation invoked
    by the debounce after().
  * ``_compute_menu_refresh_signature`` — 200 ms-cached signature for
    detecting whether the menu actually needs to rebuild.
  * ``_get_menu_children_cached`` — sig-cached menu children dict.
  * ``_cancel_pending_menu_refresh`` — cancel a pending debounce.
  * ``_build_menu_children`` — the 7-category dict builder (the
    biggest method in the cluster at ~139 lines).
  * ``_persist_entity_menu_state`` / ``_restore_entity_menu_state`` —
    remember which child menu was last active across menu close/open.
  * ``_dismiss_sao_menu_for_panel`` — close the SAO menu before
    showing a floating panel (avoid topmost overlay covering it).
  * ``_build_update_menu_label`` — formats the updater status string
    for the menu's "关于" → "检查更新" entry.

All 17 methods are SAOPlayerGUI instance methods today; this mixin
holds the definitions. SAOPlayerGUI's __init__ still owns the
relevant state (``self._sao_menu``, ``self._menu_icons``,
``self._menu_refresh_after_id``, signature caches, etc.) — the mixin
only contains method bodies that access ``self.X``.
"""

from __future__ import annotations

import time
from typing import Any, Callable, Dict, List, Optional

from act_platform.runtime import (
    act_action_log_status,
    act_combatant_drilldown_status,
    act_data_source_health,
    act_death_recap_status,
    act_graph_timeseries_status,
    act_plugin_disable,
    act_plugin_enable,
    act_plugin_reload,
    act_plugin_status,
    act_report_status,
    act_skill_drilldown_status,
    act_timeline_status,
    act_trigger_status,
    ensure_act_plugin_manager,
)
from config import DEFAULT_HOTKEYS
from utils.sao_sound import play_sound
from sao_theme import SAOPopUpMenu


class SAOPlayerGUIMenuMixin:
    """Mixin providing the SAO PopUpMenu lifecycle + refresh logic.

    SAOPlayerGUI must initialise the following attributes in its
    ``__init__`` (this is the existing contract — the mixin does not
    seed them):

      * ``self._sao_menu`` (SAOPopUpMenu or None until first toggle)
      * ``self._menu_icons`` (list[dict] — set by _setup_sao_menu)
      * ``self._menu_refresh_after_id`` (int or None)
      * ``self._menu_refresh_force`` (bool)
      * ``self._menu_children_cache`` (dict or None)
      * ``self._menu_children_cache_sig`` (tuple or None)
      * ``self._last_menu_refresh_sig`` (tuple or None)
      * ``self._last_menu_refresh_sig_time`` (float)
      * ``self._sao_menu_close_pending`` (bool)
      * ``self._fisheye_close_suppress_until`` (float)

    Plus all the other ``self.X`` fields the methods read (which are
    initialised by SAOPlayerGUI proper).
    """

    def _persist_entity_menu_state(self, save_now: bool = False):
        settings = getattr(self, '_cfg_settings_ref', None)
        if not settings:
            return
        active_name = ''
        try:
            menu_bar = getattr(getattr(self, '_sao_menu', None), '_menu_bar', None)
            active_item = getattr(menu_bar, '_active_item', None)
            if isinstance(active_item, dict):
                active_name = str(active_item.get('name') or '').strip()
        except Exception:
            active_name = ''
        settings.set('entity_last_menu', active_name)
        if save_now:
            try:
                settings.save()
            except Exception:
                pass

    def _restore_entity_menu_state(self):
        saved_name = str(self._get_setting('entity_last_menu', '') or '').strip()
        if not saved_name:
            return
        menu = getattr(self, '_sao_menu', None)
        menu_bar = getattr(menu, '_menu_bar', None)
        if not menu or not menu.visible or menu_bar is None:
            return
        active_item = getattr(menu_bar, '_active_item', None)
        if isinstance(active_item, dict) and str(active_item.get('name') or '').strip() == saved_name:
            return
        item = next((it for it in (self._menu_icons or []) if str(it.get('name') or '').strip() == saved_name), None)
        if item:
            menu_bar._on_item_click(item)

    def _compute_menu_refresh_signature(self):
        # v2.3.15: cache the signature for 200ms to avoid recomputing
        # on every call from the 16+ _refresh_menu_if_open sites.
        _now = time.time()
        if (self._last_menu_refresh_sig is not None
                and (_now - self._last_menu_refresh_sig_time) < 0.2):
            return self._last_menu_refresh_sig

        hk = self.settings.get('hotkeys', DEFAULT_HOTKEYS) or {}
        hotkey_sig = tuple(sorted((str(k), str(v)) for k, v in hk.items()))
        try:
            topmost = bool(self._float.attributes('-topmost'))
        except Exception:
            topmost = False

        ak_on = False
        br_on = False
        if self._cfg_settings_ref:
            try:
                ak_on = bool(self._load_auto_key_config().get('enabled', False))
            except Exception:
                ak_on = False
            try:
                br_on = bool(self._load_boss_raid_config().get('enabled', False))
            except Exception:
                br_on = False

        hs_on = bool(self._hide_seek_engine and self._hide_seek_engine.running)
        snd_on = bool(self._get_setting('sound_enabled', True))
        dps_on = bool(self._get_setting('dps_enabled', True))
        dps_report_available = bool(self._get_dps_last_report_available())
        buffmon_on = bool(self._get_setting('buffmon_enabled', True))
        burst_on = bool(self._get_setting('burst_enabled', True))
        burst_slots = self._normalize_watched_skill_slots(
            self._get_setting('watched_skill_slots', [1, 2, 3, 4, 5, 6, 7, 8, 9])
        )
        if not burst_slots:
            burst_slots = [1]
        boss_bar_mode = str(self._get_setting('boss_bar_mode', 'boss_raid') or 'boss_raid')
        mem_data_source = self._get_mem_data_source()
        update_label = self._build_update_menu_label()
        try:
            plugin_status = self._get_act_plugin_menu_status()
            plugin_sig = (
                int(plugin_status.get('plugin_count', 0) or 0),
                int(plugin_status.get('active_count', 0) or 0),
            )
        except Exception:
            plugin_sig = (0, 0)
        try:
            trigger_status = self._get_act_trigger_menu_status()
            trigger_sig = (
                int(trigger_status.get('rule_count', 0) or 0),
                int(trigger_status.get('timer_count', 0) or 0),
            )
        except Exception:
            trigger_sig = (0, 0)
        try:
            source_status = self._get_act_data_source_menu_status()
            source_summary = source_status.get('sources', {}).get('summary', {})
            source_sig = (
                str(source_status.get('status') or ''),
                str(source_summary.get('data_source') or ''),
                bool(source_summary.get('packet_active')),
                bool(source_summary.get('memory_active')),
            )
        except Exception:
            source_sig = ('missing', '', False, False)
        try:
            report_status = self._get_act_report_menu_status()
            report_preview = report_status.get('preview') or {}
            report_sig = (
                bool(report_status.get('ok')),
                str(report_status.get('encounter_id') or ''),
                int(report_preview.get('total_damage') or 0),
                int((report_status.get('storage_status') or {}).get('count') or 0),
            )
        except Exception:
            report_sig = (False, '', 0, 0)
        try:
            timeline_status = self._get_act_timeline_menu_status()
            timeline_sig = (
                bool(timeline_status.get('ok')),
                int(len(timeline_status.get('events') or [])),
                int(timeline_status.get('cursor_ms') or 0),
                bool(timeline_status.get('playing')),
            )
        except Exception:
            timeline_sig = (False, 0, 0, False)
        try:
            action_log_status = self._get_act_action_log_menu_status()
            action_log_sig = (
                bool(action_log_status.get('ok')),
                int(len(action_log_status.get('rows') or [])),
                int(len(action_log_status.get('grouped_rows') or [])),
                int((action_log_status.get('cursor') or {}).get('time_ms') or 0),
                str((action_log_status.get('filters') or {}).get('topic') or ''),
            )
        except Exception:
            action_log_sig = (False, 0, 0, 0, '')
        try:
            death_status = self._get_act_death_recap_menu_status()
            death_summary = death_status.get('summary') or {}
            death = death_status.get('death') or {}
            death_sig = (
                bool(death_status.get('ok')),
                str((death or {}).get('entity_id') or ''),
                int(death_summary.get('incoming_damage') or 0),
                int(death_summary.get('death_events') or 0),
            )
        except Exception:
            death_sig = (False, '', 0, 0)
        try:
            graph_status = self._get_act_graph_timeseries_menu_status()
            graph_sig = (
                bool(graph_status.get('ok')),
                str(graph_status.get('selected_metric') or ''),
                int(graph_status.get('row_count') or 0),
                int(graph_status.get('time_range_ms') or 0),
            )
        except Exception:
            graph_sig = (False, '', 0, 0)
        try:
            combatant_status = self._get_act_combatant_menu_status()
            combatant_sig = (
                bool(combatant_status.get('ok')),
                str(combatant_status.get('combatant_id') or ''),
                int(len(combatant_status.get('skills') or [])),
            )
        except Exception:
            combatant_sig = (False, '', 0)
        try:
            skill_status = self._get_act_skill_menu_status()
            skill_sig = (
                bool(skill_status.get('ok')),
                str(skill_status.get('combatant_id') or ''),
                str(skill_status.get('skill_id') or ''),
                int(len(skill_status.get('timeline_refs') or [])),
            )
        except Exception:
            skill_sig = (False, '', '', 0)
        session_visible = False
        session_count = 0
        session_version = int(getattr(self, '_session_players_version', 0) or 0)
        try:
            stack = getattr(self, '_menu_left_stack', None)
            session_visible = bool(stack and stack.is_session_players_visible())
            session_count = len(getattr(self, '_session_players', {}) or {})
        except Exception:
            session_visible = False
            session_count = 0

        sig = (
            hotkey_sig,
            bool(getattr(self, '_recognition_active', False)),
            topmost,
            ak_on,
            hs_on,
            br_on,
            snd_on,
            dps_on,
            dps_report_available,
            buffmon_on,
            burst_on,
            tuple(burst_slots),
            boss_bar_mode,
            mem_data_source,
            bool(self._panels_hidden),
            update_label,
            plugin_sig,
            trigger_sig,
            source_sig,
            report_sig,
            timeline_sig,
            action_log_sig,
            death_sig,
            graph_sig,
            combatant_sig,
            skill_sig,
            session_visible,
            session_count,
            session_version,
        )
        self._last_menu_refresh_sig = sig
        self._last_menu_refresh_sig_time = _now
        return sig

    def _get_menu_children_cached(self, force: bool = False):
        sig = self._compute_menu_refresh_signature()
        if not force and self._menu_children_cache is not None and self._menu_children_cache_sig == sig:
            return self._menu_children_cache
        children = self._build_menu_children()
        self._menu_children_cache_sig = sig
        self._menu_children_cache = children
        return children

    def _cancel_pending_menu_refresh(self):
        after_id = getattr(self, '_menu_refresh_after_id', None)
        if after_id:
            try:
                self.root.after_cancel(after_id)
            except Exception:
                pass
        self._menu_refresh_after_id = None
        self._menu_refresh_force = False

    def _apply_menu_refresh_if_open(self):
        self._menu_refresh_after_id = None
        force = bool(self._menu_refresh_force)
        self._menu_refresh_force = False
        if self._destroyed:
            return
        menu = getattr(self, '_sao_menu', None)
        if not (menu and menu.visible):
            self._update_float_status()
            return
        children = self._get_menu_children_cached(force=force)
        refresh_all = getattr(menu, 'refresh_child_menus', None)
        if callable(refresh_all):
            refresh_all(children, force=force)
        else:
            for name, items in children.items():
                menu.refresh_child_menu(name, items)
        self._update_float_status()

    def _build_menu_children(self):
        """动态构建子菜单 (支持状态反映) — SAO Auto (7 categories)"""
        hk = self.settings.get('hotkeys', DEFAULT_HOTKEYS)

        def _k(key_id):
            v = hk.get(key_id, DEFAULT_HOTKEYS.get(key_id, ''))
            return f'  [{v}]' if v else ''

        recog_label = '识别: ON' if getattr(self, '_recognition_active', False) else '识别: OFF'
        topmost_label = '置顶: ON' if self._float.attributes('-topmost') else '置顶: OFF'

        ak_config = self._load_auto_key_config() if self._cfg_settings_ref else {}
        ak_on = bool(ak_config.get('enabled', False))
        ak_label = f'AutoKey: {"ON" if ak_on else "OFF"}' + _k('toggle_auto_script')
        hs_on = bool(self._hide_seek_engine and self._hide_seek_engine.running)
        hs_label = f'自动躲猫猫: {"ON" if hs_on else "OFF"}' + _k('toggle_hide_seek')

        br_config = self._load_boss_raid_config() if self._cfg_settings_ref else {}
        br_on = bool(br_config.get('enabled', False))
        br_label = f'BossRaid: {"ON" if br_on else "OFF"}' + _k('boss_raid_start')

        snd_on = bool(self._get_setting('sound_enabled', True))
        dps_on = bool(self._get_setting('dps_enabled', True))
        dps_report_available = self._get_dps_last_report_available()
        burst_on = bool(self._get_setting('burst_enabled', True))
        burst_slots = self._normalize_watched_skill_slots(
            self._get_setting('watched_skill_slots', [1, 2, 3, 4, 5, 6, 7, 8, 9])
        )
        if not burst_slots:
            burst_slots = [1]
        burst_slot_set = set(burst_slots)
        burst_slots_disp = ','.join(str(s) for s in burst_slots)
        boss_bar_mode = self._get_setting('boss_bar_mode', 'boss_raid') or 'boss_raid'
        boss_bar_labels = {'always': '常显', 'boss_raid': 'Boss战', 'off': '关闭'}
        boss_bar_disp = boss_bar_labels.get(boss_bar_mode, boss_bar_mode)
        mem_mode = self._get_mem_data_source()
        mem_mode_labels = {'tcp': 'TCP', 'memory': 'MEM', 'hybrid': 'HYBRID', 'auto': 'AUTO'}
        mem_mode_disp = mem_mode_labels.get(mem_mode, mem_mode.upper())
        plugin_status = self._get_act_plugin_menu_status()
        plugin_total = int(plugin_status.get('plugin_count', 0) or 0)
        plugin_active = int(plugin_status.get('active_count', 0) or 0)
        trigger_status = self._get_act_trigger_menu_status()
        trigger_total = int(trigger_status.get('rule_count', 0) or 0)
        trigger_timers = int(trigger_status.get('timer_count', 0) or 0)
        source_status = self._get_act_data_source_menu_status()
        source_summary = source_status.get('sources', {}).get('summary', {})
        source_label = str(source_summary.get('data_source') or source_status.get('requested_mode') or mem_mode_disp).upper()
        source_health_state = str(source_status.get('status') or 'missing').upper()
        report_status = self._get_act_report_menu_status()
        report_preview = report_status.get('preview') or {}
        report_total_damage = int(report_preview.get('total_damage') or 0)
        report_state = 'READY' if report_status.get('ok') else 'EMPTY'
        timeline_status = self._get_act_timeline_menu_status()
        timeline_count = len(timeline_status.get('events') or [])
        timeline_state = 'PLAY' if timeline_status.get('playing') else 'READY'
        action_log_status = self._get_act_action_log_menu_status()
        action_log_count = len(action_log_status.get('rows') or [])
        action_group_count = len(action_log_status.get('grouped_rows') or [])
        action_log_state = 'READY' if action_log_status.get('ok') else 'EMPTY'
        death_status = self._get_act_death_recap_menu_status()
        death_summary = death_status.get('summary') or {}
        death_damage = int(death_summary.get('incoming_damage') or 0)
        death_events = int(death_summary.get('death_events') or 0)
        death_state = 'READY' if death_events else 'EMPTY'
        graph_status = self._get_act_graph_timeseries_menu_status()
        graph_metric = str(graph_status.get('selected_metric') or 'damage')
        graph_count = int(graph_status.get('row_count') or 0)
        graph_state = 'READY' if graph_status.get('ok') else 'EMPTY'
        combatant_status = self._get_act_combatant_menu_status()
        combatant_id = str(combatant_status.get('combatant_id') or 'NONE')
        combatant_count = len(combatant_status.get('skills') or [])
        combatant_state = 'READY' if combatant_status.get('ok') else 'EMPTY'
        skill_status = self._get_act_skill_menu_status()
        skill_id = str(skill_status.get('skill_id') or 'NONE')
        skill_ref_count = len(skill_status.get('timeline_refs') or [])
        skill_state = 'READY' if skill_status.get('ok') else 'EMPTY'
        session_visible = False
        try:
            stack = getattr(self, '_menu_left_stack', None)
            session_visible = bool(stack and stack.is_session_players_visible())
        except Exception:
            session_visible = False
        session_count = len(getattr(self, '_session_players', {}) or {})
        auto_items = [
            {'icon': '⚡', 'label': ak_label, 'command': self._toggle_auto_script},
            {'icon': '◈', 'label': hs_label, 'command': self._toggle_hide_seek},
            {'icon': '◆', 'label': 'AutoKey Quick Panel', 'command': self._toggle_autokey_panel},
            {'icon': '◇', 'label': 'AutoKey Detail Editor', 'command': self._toggle_autokey_detail_panel},
        ]

        boss_items = [
            {'icon': '⚔', 'label': br_label, 'command': self._toggle_boss_raid},
            {'icon': '▸', 'label': '下一阶段' + _k('boss_raid_next_phase'), 'command': self._boss_raid_next_phase},
            {'icon': '◆', 'label': 'BossRaid Quick Panel', 'command': self._toggle_bossraid_panel},
            {'icon': '◇', 'label': 'BossRaid Detail Editor', 'command': self._toggle_bossraid_detail_panel},
        ]

        burst_items = [
            {'icon': '◆', 'label': f'爆发提示: {"ON" if burst_on else "OFF"}', 'command': self._toggle_burst_enabled},
            {'icon': '◇', 'label': f'Burst技能槽: [{burst_slots_disp}]',
             'command': lambda: self._show_entity_alert('BURST SKILLS', f'当前槽位: {burst_slots_disp}', display_time=3.0)},
            {'icon': '─', 'label': '──────────'},
        ]
        for slot in range(1, 10):
            burst_items.append({
                'icon': '◆' if slot in burst_slot_set else '◇',
                'label': f'Burst槽 {slot}' + (' ✓' if slot in burst_slot_set else ''),
                'command': lambda s=slot: self._toggle_burst_slot(s),
            })

        panel_items = [
            {'icon': '◈', 'label': 'Commander', 'command': self._toggle_commander_panel},
            {'icon': '◉', 'label': '状态面板', 'command': self._toggle_status_panel},
            {'icon': '◆' if session_visible else '◇',
             'label': f'Session Players: {session_count}人' + (' ✓' if session_visible else ''),
             'command': self._toggle_session_players_panel},
            {'icon': '◈', 'label': '一键隐藏面板' + (' ✓' if self._panels_hidden else ''), 'command': self._toggle_hide_all_panels},
            {'icon': '─', 'label': '──────────'},
        ]

        # ── 面板皮肤子菜单 ──
        _cfg = self._cfg_settings_ref or self.settings
        _theme_settings = _cfg.get('panel_themes', {})
        _overlay_map = {
            'dps': ('◆', 'DPS'),
            'hp': ('♥', 'HP'),
            'bosshp': ('⚔', 'BossHP'),
            'skillfx': ('✦', 'SkillFX'),
            'alert': ('!', 'Alert'),
            'act': ('▣', 'ACT'),
        }
        skin_items = []
        skin_items.append({'icon': '🎨', 'label': '全部 Light', 'command': lambda: self._set_all_themes('light')})
        skin_items.append({'icon': '🌙', 'label': '全部 Dark', 'command': lambda: self._set_all_themes('dark')})
        skin_items.append({'icon': '─', 'label': '──────────'})
        for _key, (_ico, _lbl) in _overlay_map.items():
            _cur = _theme_settings.get(_key, 'dark')
            skin_items.append({
                'icon': _ico,
                'label': f'{_lbl}: {_cur.upper()}',
                'command': lambda k=_key: self._toggle_panel_theme(k),
            })

        buffmon_on = bool(self._get_setting('buffmon_enabled', True))
        panel_items.extend([
            {'icon': '♪', 'label': f'音效: {"ON" if snd_on else "OFF"}', 'command': self._toggle_sound_enabled},
            {'icon': '♪', 'label': '音量+', 'command': lambda: self._adj_sound_volume(10)},
            {'icon': '♪', 'label': '音量-', 'command': lambda: self._adj_sound_volume(-10)},
            {'icon': '◆', 'label': f'DPS面板: {"ON" if dps_on else "OFF"}', 'command': self._toggle_dps_enabled},
            {'icon': '◆' if dps_report_available else '◇',
             'label': '查看上次战斗DPS' + (' ✓' if dps_report_available else ' (暂无)'),
             'command': self._show_last_dps_report_menu},
            {'icon': '◆' if dps_report_available else '◇',
             'label': 'DPS历史报告' + (' ✓' if dps_report_available else ' (暂无)'),
             'command': self._show_dps_history_menu},
            {'icon': '⬇' if dps_report_available else '◇',
             'label': '导出DPS报告' + (' ✓' if dps_report_available else ' (暂无)'),
             'command': self._export_last_dps_report_menu},
            {'icon': '◇', 'label': f'Boss血条: {boss_bar_disp}', 'command': self._cycle_boss_bar_mode},
            {'icon': '✦', 'label': f'Buff监视器: {"ON" if buffmon_on else "OFF"}', 'command': self._toggle_buffmon_enabled},
            {'icon': '◆', 'label': f'Hybrid数据源: {mem_mode_disp}', 'command': self._cycle_mem_data_source},
        ])

        act_items = [
            {'icon': '☌', 'label': f'ACT插件: {plugin_active}/{plugin_total}', 'command': self._show_act_plugin_status_menu},
            {'icon': '☌', 'label': 'ACT插件管理面板', 'command': self._toggle_act_plugin_manager_panel},
            {'icon': '⏱', 'label': f'ACT触发/计时: {trigger_total}/{trigger_timers}', 'command': self._toggle_act_trigger_timer_panel},
            {'icon': '◉', 'label': f'ACT数据源健康: {source_label}/{source_health_state}', 'command': self._toggle_act_data_source_health_panel},
            {'icon': '⬇', 'label': f'ACT报告/导出: {report_state}/{report_total_damage}', 'command': self._toggle_act_report_export_panel},
            {'icon': '⬇', 'label': 'ACT离线导入向导', 'command': self._toggle_act_offline_import_panel},
            {'icon': '▶', 'label': f'ACT时间线/VCR: {timeline_state}/{timeline_count}', 'command': self._toggle_act_timeline_vcr_panel},
            {'icon': '▣', 'label': f'ACT聚合总览: {action_log_state}/{action_group_count}', 'command': self._toggle_act_aggregate_panel},
            {'icon': '▤', 'label': f'ACT行为日志: {action_log_state}/{action_log_count}', 'command': self._toggle_act_action_log_panel},
            {'icon': '✚', 'label': f'ACT死亡回放: {death_state}/{death_damage}', 'command': self._toggle_act_death_recap_panel},
            {'icon': '⌁', 'label': f'ACT图表/曲线: {graph_state}/{graph_metric}/{graph_count}', 'command': self._toggle_act_graph_timeseries_panel},
            {'icon': '◎', 'label': f'ACT成员钻取: {combatant_state}/{combatant_id}/{combatant_count}', 'command': self._toggle_act_combatant_drilldown_panel},
            {'icon': '✦', 'label': f'ACT技能钻取: {skill_state}/{skill_id}/{skill_ref_count}', 'command': self._toggle_act_skill_drilldown_panel},
            {'icon': '↻', 'label': '重载ACT插件', 'command': self._reload_act_plugins_menu},
            {'icon': '◆', 'label': '切换首个ACT插件', 'command': self._toggle_first_act_plugin_menu},
        ]

        return {
            '控制': [
                {'icon': '⚙', 'label': recog_label + _k('toggle_recognition'), 'command': self._toggle_recognition_menu},
                {'icon': '⬆', 'label': topmost_label + _k('toggle_topmost'), 'command': self._toggle_topmost},
                {'icon': '✓', 'label': '保存设置', 'command': lambda: self.settings.save()},
            ],
            '自动': auto_items,
            'Boss': boss_items,
            'Burst': burst_items,
            '面板': panel_items,
            'ACT': act_items,
            '皮肤': skin_items,
            '关于': [
                {'icon': '◇', 'label': '关于本程序', 'command': self._show_about},
                {'icon': '⬇', 'label': self._build_update_menu_label(), 'command': self._check_for_updates_interactive},
                {'icon': '✎', 'label': '修改角色资料', 'command': self._edit_profile},
                {'icon': '◇', 'label': '切换到 WebView UI', 'command': self._switch_to_webview_ui},
                {'icon': '✕', 'label': '退出', 'command': self._on_close},
            ],
        }

    def _get_act_plugin_menu_status(self):
        try:
            ensure_act_plugin_manager(self, load=False)
            return act_plugin_status(self)
        except Exception as exc:
            return {'ok': False, 'message': str(exc), 'plugin_count': 0, 'active_count': 0, 'plugins': []}

    def _get_act_trigger_menu_status(self):
        try:
            return act_trigger_status(self)
        except Exception as exc:
            return {'ok': False, 'message': str(exc), 'rule_count': 0, 'timer_count': 0, 'triggers': [], 'timers': []}

    def _get_act_data_source_menu_status(self):
        try:
            return act_data_source_health(self)
        except Exception as exc:
            return {'ok': False, 'status': 'error', 'message': str(exc), 'sources': {'summary': {}}, 'latency_ms': 0, 'last_event_ms': 0, 'errors': [str(exc)]}

    def _get_act_report_menu_status(self):
        try:
            return act_report_status(self, limit=12)
        except Exception as exc:
            return {'ok': False, 'message': str(exc), 'preview': {}, 'history': [], 'errors': [str(exc)], 'storage_status': {'count': 0}}

    def _get_act_timeline_menu_status(self):
        try:
            return act_timeline_status(self, limit=24)
        except Exception as exc:
            return {'ok': False, 'message': str(exc), 'events': [], 'cursor_ms': 0, 'speed': 1.0, 'playing': False, 'filters': {'query': ''}, 'errors': [str(exc)]}

    def _get_act_action_log_menu_status(self):
        try:
            return act_action_log_status(self, limit=24)
        except Exception as exc:
            return {'ok': False, 'message': str(exc), 'rows': [], 'columns': [], 'filters': {'query': '', 'topic': ''}, 'cursor': {'time_ms': 0, 'row_count': 0}, 'errors': [str(exc)]}

    def _get_act_death_recap_menu_status(self):
        try:
            return act_death_recap_status(self, limit=24)
        except Exception as exc:
            return {'ok': False, 'message': str(exc), 'death': None, 'rows': [], 'summary': {'incoming_damage': 0, 'death_events': 0}, 'errors': [str(exc)]}

    def _get_act_graph_timeseries_menu_status(self):
        try:
            return act_graph_timeseries_status(self, limit=24)
        except Exception as exc:
            return {'ok': False, 'message': str(exc), 'selected_metric': 'damage', 'series': {}, 'metrics': [], 'time_range_ms': 0, 'row_count': 0, 'filters': {'query': '', 'topic': ''}, 'errors': [str(exc)]}

    def _get_act_combatant_menu_status(self):
        try:
            return act_combatant_drilldown_status(self)
        except Exception as exc:
            return {'ok': False, 'message': str(exc), 'combatant_id': '', 'summary': {}, 'skills': [], 'incoming': [], 'outgoing': [], 'filters': {'query': '', 'focus_target': ''}, 'errors': [str(exc)]}

    def _get_act_skill_menu_status(self):
        try:
            return act_skill_drilldown_status(self, limit=24)
        except Exception as exc:
            return {'ok': False, 'message': str(exc), 'combatant_id': '', 'skill_id': '', 'summary': {}, 'casts': 0, 'hits': 0, 'crit_rate': 0.0, 'timeline_refs': [], 'filters': {'query': ''}, 'errors': [str(exc)]}

    def _show_act_plugin_status_menu(self):
        status = self._get_act_plugin_menu_status()
        plugins = status.get('plugins') or []
        if not plugins:
            self._show_entity_alert('ACT PLUGINS', '未发现插件；可放入 plugins/<id>/plugin.json', display_time=4.0)
            return status
        lines = []
        for plug in plugins[:8]:
            state = 'ON' if plug.get('active') else ('OFF' if plug.get('enabled') else 'DISABLED')
            lines.append(f"{plug.get('id')}: {state} v{plug.get('version')}")
        self._show_entity_alert('ACT PLUGINS', '\n'.join(lines), display_time=5.0)
        return status

    def _reload_act_plugins_menu(self):
        result = act_plugin_reload(self)
        status = result if isinstance(result, dict) else self._get_act_plugin_menu_status()
        self._show_entity_alert(
            'ACT PLUGINS',
            f"重载完成: {status.get('active_count', 0)}/{status.get('plugin_count', 0)} active",
            display_time=3.2,
        )
        self._refresh_menu_if_open(force=True)
        return status

    def _toggle_first_act_plugin_menu(self):
        status = self._get_act_plugin_menu_status()
        plugins = status.get('plugins') or []
        if not plugins:
            self._show_entity_alert('ACT PLUGINS', '未发现可切换插件', display_time=3.0)
            return status
        plugin_id = str(plugins[0].get('id') or '')
        if not plugin_id:
            return status
        if plugins[0].get('enabled'):
            result = act_plugin_disable(self, plugin_id)
            action = 'DISABLED'
        else:
            result = act_plugin_enable(self, plugin_id)
            action = 'ENABLED'
        self._show_entity_alert('ACT PLUGINS', f'{plugin_id}: {action}', display_time=3.0)
        self._refresh_menu_if_open(force=True)
        return result

    def _dismiss_sao_menu_for_panel(self):
        """SAO 菜单关掉再弹面板, 避免 topmost overlay 压在面板上看不见."""
        try:
            menu = getattr(self, '_sao_menu', None)
            if menu is not None and getattr(menu, 'visible', False):
                menu.close()
                try:
                    from render.overlay_scheduler import get_scheduler as _get_sched
                    _get_sched(self.root).set_menu_open(False)
                except Exception:
                    pass
        except Exception:
            pass

    def _setup_sao_menu(self):
        """构建 SAO PopUpMenu 菜单 = 主界面 (7 categories)"""
        self._menu_icons = [
            {'name': '控制', 'icon': '⚙', 'can_active': True},
            {'name': '自动', 'icon': '⚡', 'can_active': True},
            {'name': 'Boss', 'icon': '⚔', 'can_active': True},
            {'name': 'Burst', 'icon': 'B', 'can_active': True},
            {'name': '面板', 'icon': '◆', 'can_active': True},
            {'name': 'ACT', 'icon': 'A', 'can_active': True},
            {'name': '皮肤', 'icon': 'P', 'can_active': True},
            {'name': '关于', 'icon': 'ℹ', 'can_active': True},
        ]

        self._sao_menu = SAOPopUpMenu(
            self.root, self._menu_icons, self._get_menu_children_cached(force=True),
            username=self._username or 'Player',
            description=self._profession or 'SAO Auto — 游戏辅助 UI',
            on_close=self._on_sao_menu_close,
            on_open=self._on_sao_menu_open,
            key_code='a',
            slide_down=False,
            left_widget_factory=self._make_player_panel,
            anchor_widget=self._float,
            external_close=False,
            alt_toggle_close=False,
            on_background_click=self._close_sao_menu_from_background,
        )
        self._sao_menu.bind_events()

    def _toggle_sao_menu(self, allow_close: bool = False):
        # Lazy-init: build menu on first toggle (deferred from __init__)
        if self._sao_menu is None:
            self._setup_sao_menu()
        if self._sao_menu.visible:
            if not allow_close:
                return
            try:
                play_sound('menu_close')
            except Exception:
                pass
            self._play_motion_blur(closing=True)
            self._sao_menu.close()
            try:
                from render.overlay_scheduler import get_scheduler as _get_sched
                _get_sched(self.root).set_menu_open(False)
            except Exception:
                pass
        else:
            self._stop_float_breath()
            try:
                if self._float and self._float.winfo_exists():
                    self._float.update_idletasks()
                    self._breath_base_x = self._float.winfo_x()
                    self._breath_base_y = self._float.winfo_y()
            except Exception:
                pass
            try:
                play_sound('menu_open')
            except Exception:
                pass
            self._play_motion_blur(closing=False)
            self._sao_menu.child_menus = self._get_menu_children_cached(force=True)
            self._sao_menu.open()
            try:
                from render.overlay_scheduler import get_scheduler as _get_sched
                _get_sched(self.root).set_menu_open(True)
            except Exception:
                pass
            # 立即将悬浮按钮浮到 overlay 之上 (避免撕裂)
            self._float.lift()

    def _close_sao_menu_from_background(self):
        if self._sao_menu_close_pending or self._exit_animating or self._close_finalized:
            return
        menu = getattr(self, '_sao_menu', None)
        if menu is None or not getattr(menu, 'visible', False):
            return
        self._sao_menu_close_pending = True
        self._destroy_fisheye_hit_layer()
        self._fisheye_close_suppress_until = time.time() + 1.4
        ov = getattr(self, '_fisheye_ov', None)
        self._release_fisheye_input_zorder(ov)
        request_fadeout = getattr(ov, '_request_fadeout', None)
        if callable(request_fadeout):
            try:
                request_fadeout()
            except Exception:
                pass
        try:
            self._toggle_sao_menu(allow_close=True)
        finally:
            try:
                self.root.after(650, self._clear_sao_menu_close_pending)
            except Exception:
                self._sao_menu_close_pending = False

    def _clear_sao_menu_close_pending(self):
        menu = getattr(self, '_sao_menu', None)
        if menu is not None and getattr(menu, 'visible', False):
            try:
                self.root.after(180, self._clear_sao_menu_close_pending)
                return
            except Exception:
                pass
        self._sao_menu_close_pending = False

    def _on_sao_menu_open(self):
        """SAO 菜单打开时 — 停止呼吸, 启动持久鱼眼 (Win32 z-order 接管)"""
        self._stop_float_breath()
        # 不启动 _lift_float_loop (tkinter .lift() 会引起闪烁);
        # z-order 完全由 _start_fisheye_overlay 内的 Win32 SetWindowPos 管理
        self._lift_loop_active = False
        # 延迟启动鱼眼叠加 (等菜单渲染完再截图), 带重试确保首次也能生效
        self._start_fisheye_with_retry(retries=5, delay=80)
        self._refresh_session_players_panel(force=True)
        try:
            self.root.after(90, self._restore_entity_menu_state)
        except Exception:
            pass

    def _on_sao_menu_close(self):
        """SAO 菜单关闭时 — 重启呼吸动画; 面板仍开时保持鱼眼, 否则渐隐销毁"""
        self._lift_loop_active = False
        self._cancel_pending_menu_refresh()
        self._persist_entity_menu_state(save_now=False)
        try:
            stack = getattr(self, '_menu_left_stack', None)
            panel = getattr(stack, 'session_panel', None) if stack is not None else None
            if panel is not None and hasattr(panel, 'unbind_global_wheel_fallback'):
                panel.unbind_global_wheel_fallback()
        except Exception:
            pass
        self._player_panel = None
        self._menu_left_stack = None
        self._session_players_panel = None
        self._maybe_stop_fisheye()
        if not self._destroyed:
            pass  # 呼吸动画已禁用 (固定位置)

    def _refresh_menu_if_open(self, force: bool = False):
        """如果菜单打开, 刷新子菜单和面板

        v2.3.15: changed from after_idle to after(100) for debounced
        batch execution. Multiple calls within 100ms are merged into
        a single refresh, reducing main-thread callback churn from
        16+ call sites.
        """
        menu = getattr(self, '_sao_menu', None)
        if self._destroyed:
            return
        if not (menu and menu.visible):
            self._update_float_status()
            return
        if force:
            self._menu_refresh_force = True
        if self._menu_refresh_after_id:
            return  # already scheduled; this call is merged
        try:
            self._menu_refresh_after_id = self.root.after(100, self._apply_menu_refresh_if_open)
        except Exception:
            self._apply_menu_refresh_if_open()

    def _refresh_menu_immediate(self):
        """Refresh menu child-bar without debounce (used for theme switch)."""
        menu = getattr(self, '_sao_menu', None)
        if self._destroyed or not (menu and menu.visible):
            return
        self._menu_refresh_force = True
        # Cancel any pending debounced refresh
        aid = self._menu_refresh_after_id
        if aid:
            try:
                self.root.after_cancel(aid)
            except Exception:
                pass
            self._menu_refresh_after_id = None
        self._apply_menu_refresh_if_open()

    def _build_update_menu_label(self) -> str:
        try:
            from updater.sao_updater import get_manager, STATE_AVAILABLE, STATE_READY, STATE_DOWNLOADING
            st = get_manager().snapshot()
            if st.state == STATE_READY:
                return f'更新就绪 v{st.latest_version} (重启应用)'
            if st.state == STATE_AVAILABLE:
                if (not st.force_required) and getattr(st, 'skipped_version', '') == st.latest_version:
                    return '检查更新'
                tag = '强制' if st.force_required else '可更新'
                return f'[{tag}] 新版本 v{st.latest_version}'
            if st.state == STATE_DOWNLOADING:
                # v2.1.2-l: 不再每次 progress 变化都刷新菜单签名 (会触发整菜单
                # rebuild → entity HUD 撕裂). 这里只显示静态文本, 精确百分比
                # 由专用状态面板 (_update_status_panel) 实时显示。
                return '下载中...'
        except Exception:
            pass
        return '检查更新'
