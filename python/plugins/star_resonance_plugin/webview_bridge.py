# -*- coding: utf-8 -*-
# Star Resonance WebView bridge.
#
# Game-specific WebView API and engine lifecycle hooks live here so the
# platform-level ``sao_webview.py`` can stay game-agnostic.

from __future__ import annotations

import json
import ctypes
import logging
import math
import os
import sys
import threading
import time
from pathlib import Path
from typing import Any, Optional

logger = logging.getLogger(__name__)

_PLUGIN_ROOT = os.path.dirname(os.path.abspath(__file__))
if _PLUGIN_ROOT not in sys.path:
    sys.path.insert(0, _PLUGIN_ROOT)

from act_platform.runtime import (
    act_action_log_copy,
    act_action_log_filter,
    act_action_log_jump_to_time,
    act_action_log_search,
    act_action_log_status,
    act_aggregate_status,
    act_combatant_drilldown_back,
    act_combatant_drilldown_filter,
    act_combatant_drilldown_focus_target,
    act_combatant_drilldown_status,
    act_data_source_diagnose,
    act_data_source_health,
    act_death_recap_copy,
    act_death_recap_status,
    act_graph_timeseries_export,
    act_graph_timeseries_filter,
    act_graph_timeseries_select_metric,
    act_graph_timeseries_status,
    act_graph_timeseries_zoom,
    act_history_delete,
    act_history_load,
    act_history_status,
    act_mem_attr_map,
    act_mem_narrow,
    act_mem_scope_status,
    act_mem_search,
    act_mem_search_cancel,
    act_mem_search_status,
    act_mini_parse_copy,
    act_mini_parse_preview,
    act_mini_parse_status,
    act_offline_import_file,
    act_offline_import_status,
    act_report_copy,
    act_report_export,
    act_report_status,
    render_apply_hooks as platform_render_apply_hooks,
    act_selective_parsing_clear,
    act_selective_parsing_status,
    act_selective_parsing_update,
    act_skill_drilldown_back,
    act_skill_drilldown_copy,
    act_skill_drilldown_filter,
    act_skill_drilldown_status,
    act_timeline_filter,
    act_timeline_pause,
    act_timeline_play,
    act_timeline_seek,
    act_timeline_set_speed,
    act_timeline_status,
    act_timeline_step,
    act_trigger_disable,
    act_trigger_enable,
    act_trigger_reload,
    act_trigger_status,
    act_trigger_test,
    enrich_action_log_event,
    publish_owner_event,
    register_webview_extension,
    should_record_owner_combat_event,
)
from plugins.star_resonance_plugin.sr_config import get_skill_slot_rects

from plugins.star_resonance_plugin.engines.auto_key_engine import (
    DEFAULT_AUTO_KEY_SERVER_URL,
    AutoKeyCloudClient,
    build_auto_key_state,
    build_identity_state,
    clone_profile,
    default_upload_auth_state,
    delete_profile as delete_auto_key_profile,
    export_profile_to_default_path,
    find_profile as find_auto_key_profile,
    import_profile_from_path,
    load_auto_key_config,
    make_default_profile,
    normalize_profile,
    save_auto_key_config,
    snapshot_author_from_state,
    upsert_profile,
)
from plugins.star_resonance_plugin.engines.boss_autokey_linkage import (
    build_boss_reactions_state,
    build_linkage_state,
    delete_mapping,
    load_linkage_config,
    save_linkage_config,
    set_dodge_enabled as set_linkage_dodge_enabled,
    upsert_mapping,
)
try:
    from plugins.star_resonance_plugin.engines.boss_raid_engine import (
        DEFAULT_BOSS_RAID_SERVER_URL,
        BossRaidCloudClient,
        active_profile as br_active_profile,
        build_boss_raid_state,
        delete_profile as delete_br_profile,
        export_profile_to_default_path as export_br_profile_path,
        find_profile as find_br_profile,
        import_profile_from_path as import_br_profile_path,
        load_boss_raid_config,
        make_default_profile as make_default_br_profile,
        normalize_profile as normalize_br_profile,
        save_boss_raid_config,
        upsert_profile as upsert_br_profile,
    )
except Exception:
    DEFAULT_BOSS_RAID_SERVER_URL = "http://doi.sakisense.top:15538"

    def _boss_raid_module():
        from plugins.star_resonance_plugin.engines import boss_raid_engine
        return boss_raid_engine

    class BossRaidCloudClient:
        def __new__(cls, *args, **kwargs):
            return _boss_raid_module().BossRaidCloudClient(*args, **kwargs)

    def br_active_profile(*args, **kwargs):
        return _boss_raid_module().active_profile(*args, **kwargs)

    def build_boss_raid_state(*args, **kwargs):
        return _boss_raid_module().build_boss_raid_state(*args, **kwargs)

    def delete_br_profile(*args, **kwargs):
        return _boss_raid_module().delete_profile(*args, **kwargs)

    def export_br_profile_path(*args, **kwargs):
        return _boss_raid_module().export_profile_to_default_path(*args, **kwargs)

    def find_br_profile(*args, **kwargs):
        return _boss_raid_module().find_profile(*args, **kwargs)

    def import_br_profile_path(*args, **kwargs):
        return _boss_raid_module().import_profile_from_path(*args, **kwargs)

    def load_boss_raid_config(*args, **kwargs):
        return _boss_raid_module().load_boss_raid_config(*args, **kwargs)

    def make_default_br_profile(*args, **kwargs):
        return _boss_raid_module().make_default_profile(*args, **kwargs)

    def normalize_br_profile(*args, **kwargs):
        return _boss_raid_module().normalize_profile(*args, **kwargs)

    def save_boss_raid_config(*args, **kwargs):
        return _boss_raid_module().save_boss_raid_config(*args, **kwargs)

    def upsert_br_profile(*args, **kwargs):
        return _boss_raid_module().upsert_profile(*args, **kwargs)
from plugins.star_resonance_plugin.engines.combat_analytics import (
    boss_state_from_monster_update,
    build_act_snapshot,
    mem_boss_break_override,
)

try:
    import _sao_cy_sr_uihelpers as _CY_UI  # type: ignore[import-not-found]
except ImportError:
    _CY_UI = None  # type: ignore[assignment]


_OWNER_METHODS = (
    '_send_linked_key',
    '_arm_pending_combat_reset', '_maybe_apply_pending_combat_reset',
    '_current_player_uid_int', '_normalize_damage_event_for_self',
    '_normalize_damage_event_target_for_webview',
    '_on_packet_damage', '_on_monster_update', '_on_boss_event',
    '_on_skill_event', '_on_dungeon_event', '_on_dps_report_finalized',
    '_on_scene_change', '_clear_skillfx_state_for_death',
    '_bump_boss_hp_target_hold', '_boss_monster_usable',
    '_pick_burst_trigger_slot', '_save_game_cache', '_reset_sta_offline_state',
    '_should_show_sta_offline',
    '_eval_mapbanner', '_eval_mech_banner', '_push_mech_banner',
    '_setup_mech_banner_click_through', '_position_alert_window',
    '_show_identity_alert_window', '_hide_identity_alert_window',
    '_position_mapbanner_window', '_schedule_map_banner',
    '_show_map_banner_window', '_hide_map_banner_window',
    '_setup_alert_click_through', '_remove_alert_click_through',
    '_setup_mapbanner_click_through', '_ensure_mapbanner_on_top',
    '_eval_skillfx', '_eval_boss_hp', '_eval_buff_coverage', '_push_buff_coverage',
    '_calc_boss_hp_geometry', '_refresh_boss_hp_geometry',
    '_eval_dps', '_build_dps_act_snapshot', '_push_dps_act_snapshot',
    '_resize_dps_window', '_set_dps_detail_mode',
    '_combat_damage_timeout_s', '_boss_hp_hold_timeout_s', '_get_dps_last_report_available',
    '_sync_dps_report_availability', '_show_dps_window', '_hide_dps_window',
    '_finish_hide_dps_window', '_show_dps_live_snapshot', '_show_dps_last_report',
    '_eval_raid_editor', '_ensure_raid_editor_clickable', '_show_raid_editor',
    '_hide_raid_editor', '_push_raid_editor_entities', '_push_raid_editor_status',
    '_push_raid_editor_full', '_on_raid_entity_update', '_on_boss_action_with_gate',
    '_eval_commander', '_ensure_commander_clickable', '_show_commander',
    '_hide_commander', '_push_commander_data',
    '_eval_autokey_editor', '_ensure_autokey_editor_clickable',
    '_show_autokey_editor', '_hide_autokey_editor',
    '_push_autokey_editor_slots', '_push_autokey_editor_state',
    '_ensure_skillfx_on_top', '_setup_skillfx_click_through',
    '_setup_boss_hp_click_through', '_ensure_boss_hp_on_top',
    '_get_skillfx_layout', '_build_skillfx_payload', '_update_skillfx_layout',
    '_auto_key_settings_ref', '_auto_key_author_snapshot', '_auto_key_identity_state',
    '_set_auto_key_upload_auth', '_auto_key_upload_auth_matches', '_auto_key_upload_auth_valid',
    '_auto_key_upload_auth_state', '_refresh_auto_key_upload_auth', '_load_auto_key_config',
    '_save_auto_key_config', '_get_auto_key_menu_state', '_sync_auto_key_menu',
    '_boss_raid_settings_ref', '_boss_raid_author_snapshot', '_boss_raid_identity_state',
    '_set_boss_raid_upload_auth', '_boss_raid_upload_auth_matches', '_boss_raid_upload_auth_valid',
    '_boss_raid_upload_auth_state', '_load_boss_raid_config', '_save_boss_raid_config',
    '_get_boss_raid_menu_state', '_sync_boss_raid_menu', '_toggle_boss_raid',
    '_boss_raid_next_phase', '_refresh_boss_raid_upload_auth', '_toggle_auto_script',
    '_reset_burst_tracking', '_start_packet_engine_early', '_reconfigure_data_engines',
    '_start_recognition', '_presynthesize_active_profile_web', '_toggle_auto_dodge',
)

_ACT_API_METHODS = (
    'getDataSourceHealth', 'get_data_source_health', 'diagnose_data_source',
    'copy_data_source_health', 'get_report_export_status', 'export_last_report',
    'copy_report_export', 'get_mini_parse_status', 'preview_mini_parse',
    'copy_mini_parse', 'get_selective_parsing_status', 'update_selective_parsing',
    'clear_selective_parsing', 'get_history_status', 'load_history_report',
    'delete_history_report', 'clear_history_reports', 'choose_offline_import_file',
    'import_offline_report', 'get_offline_import_status', 'get_timeline_status',
    'get_aggregate_status', 'get_mem_scope_status', 'mem_search',
    'mem_search_status', 'mem_narrow', 'mem_search_cancel', 'mem_attr_map',
    'play_timeline', 'pause_timeline', 'step_timeline', 'seek_timeline',
    'set_timeline_speed', 'filter_timeline', 'get_action_log_status',
    'search_action_log', 'filter_action_log', 'jump_action_log_time',
    'show_action_log_at', 'copy_action_log', 'get_death_recap_status',
    'copy_death_recap', 'get_graph_timeseries_status', 'select_graph_metric',
    'zoom_graph_timeseries', 'filter_graph_timeseries', 'export_graph_timeseries',
    'get_combatant_drilldown_status', 'filter_combatant_drilldown',
    'focus_combatant_target', 'back_combatant_drilldown',
    'get_skill_drilldown_status', 'filter_skill_drilldown', 'copy_skill_drilldown',
    'back_skill_drilldown', 'open_skill_drilldown', 'get_trigger_status',
    'enable_trigger', 'disable_trigger', 'reload_triggers', 'test_trigger',
)


_API_METHODS = (
    'get_state', 'set_watched_slots', 'set_burst_enabled',
    'set_auto_key_enabled', 'get_auto_key_state', 'create_auto_key_profile',
    'copy_auto_key_profile', 'save_auto_key_profile', 'delete_auto_key_profile',
    'activate_auto_key_profile', 'export_auto_key_profile', 'start_auto_key_import_picker',
    'select_file', 'select_folder', 'set_auto_key_upload_token', 'set_auto_key_server_url',
    'refresh_auto_key_upload_auth', 'search_remote_profiles', 'download_remote_profile',
    'upload_auto_key_profile', 'get_boss_raid_state', 'set_boss_raid_enabled',
    'set_boss_raid_server_url', 'activate_boss_raid_profile', 'create_boss_raid_profile',
    'save_boss_raid_profile', 'delete_boss_raid_profile', 'export_boss_raid_profile',
    'start_boss_raid_import_picker', 'boss_raid_start', 'boss_raid_stop',
    'boss_raid_next_phase', 'boss_raid_reset', 'search_boss_raid_remote',
    'download_boss_raid_remote', 'upload_boss_raid_profile', 'refresh_boss_raid_upload_auth',
    'get_linkage_state', 'set_linkage_enabled', 'set_linkage_debug',
    'set_linkage_global_cooldown', 'save_linkage_mappings', 'reset_linkage',
    'get_boss_reactions', 'save_boss_reaction', 'delete_boss_reaction',
    'import_observed_skills', 'get_mechanics', 'save_mechanic', 'delete_mechanic',
    'create_mechanic_from_skill', 'bind_mechanic_skill', 'unbind_mechanic_skill',
    'set_mechanics_master', 'search_skill_catalog', 'test_mechanic',
    'set_boss_bar_mode', 'get_boss_bar_mode', 'set_dps_enabled', 'set_dps_fade_timeout',
    'get_dps_enabled', 'set_buffmon_enabled', 'get_buffmon_enabled', 'show_last_dps_report',
    'boss_hp_hit_regions', 'toggle_raid_editor', 'get_raid_editor_visible',
    'toggle_autokey_editor', 'get_autokey_editor_visible', 'set_data_source',
    'set_component_source',
    *_ACT_API_METHODS,
)


_PANEL_SURFACE_ACTIONS = {
    'toggle_trigger_timer_manager': 'trigger_timer',
    'toggle_data_source_health': 'data_source_health',
    'toggle_report_export': 'report_export',
    'toggle_offline_import': 'offline_import',
    'toggle_timeline_vcr': 'timeline_vcr',
    'toggle_act_aggregate': 'act_aggregate',
    'toggle_mem_scope': 'mem_scope',
    'toggle_action_log': 'action_log',
    'toggle_death_recap': 'death_recap',
    'toggle_graph_timeseries': 'graph_timeseries',
    'toggle_combatant_drilldown': 'combatant_drilldown',
    'toggle_skill_drilldown': 'skill_drilldown',
}


_PANEL_SURFACE_JS = {
    'trigger_timer': 'TriggerTimerManager',
    'data_source_health': 'DataSourceHealth',
    'report_export': 'ReportExport',
    'offline_import': 'OfflineImport',
    'timeline_vcr': 'TimelineVcr',
    'act_aggregate': 'ActAggregate',
    'mem_scope': 'MemScope',
    'action_log': 'ActionLog',
    'death_recap': 'DeathRecap',
    'graph_timeseries': 'GraphTimeseries',
    'combatant_drilldown': 'CombatantDrilldown',
    'skill_drilldown': 'SkillDrilldown',
}


def _safe_timeline_speed(value: Any, fallback: float = 1.0) -> float:
    try:
        speed = float(value)
    except (TypeError, ValueError):
        speed = float(fallback)
    if not math.isfinite(speed):
        speed = float(fallback)
    return max(0.1, min(speed, 8.0))


def _bind(target: Any, name: str, callback):
    def _wrapped(*args, **kwargs):
        return callback(*args, **kwargs)
    _wrapped.__name__ = name
    setattr(target, name, _wrapped)


def install_webview_bridge(ctx) -> None:
    owner = ctx.engine.owner
    if owner is None:
        return
    bridge = StarResonanceWebViewBridge(ctx)
    register_webview_extension(owner, bridge)
    bridge.initialize_owner_state()
    for name in _OWNER_METHODS:
        if hasattr(bridge, name):
            _bind(owner, name, getattr(bridge, name))
    api = getattr(owner, '_api', None)
    if api is not None:
        for name in _API_METHODS:
            if hasattr(bridge, name):
                _bind(api, name, getattr(bridge, name))
    ctx.log('[SR] WebView bridge installed')


class StarResonanceWebViewBridge:
    def __init__(self, ctx) -> None:
        self.ctx = ctx
        self.owner = ctx.engine.owner
        self._session_players = {}
        self._session_players_self_uid = 0
        self._session_players_version = 0
        self._session_players_last_sig = None
        self._session_players_last_push_ts = 0.0

    def __getattr__(self, name: str):
        if name in _ACT_API_METHODS:
            def _api(*args, **kwargs):
                return self._dispatch_act_api(name, *args, **kwargs)
            _api.__name__ = name
            return _api
        if name in _PANEL_SURFACE_ACTIONS:
            def _toggle(*args, **kwargs):
                return self.menu_action(name)
            _toggle.__name__ = name
            return _toggle
        raise AttributeError(name)

    def _json(self, payload: Any) -> str:
        return json.dumps(payload, ensure_ascii=False)

    def _arg(self, args, kwargs, index: int, key: str, default=None):
        if len(args) > index:
            return args[index]
        return kwargs.get(key, default)

    def _as_int(self, value, default: int = 0) -> int:
        try:
            return int(value if value is not None and value != '' else default)
        except Exception:
            return int(default)

    def _as_float(self, value, default: float = 0.0) -> float:
        try:
            return float(value if value is not None and value != '' else default)
        except Exception:
            return float(default)

    def _as_str(self, value, default: str = '') -> str:
        return str(value if value is not None else default)

    def _surface_meta(self, surface: str) -> dict:
        return dict(getattr(self.owner, '_plugin_surface_meta', {}).get(str(surface or ''), {}) or {})

    def _surface_win(self, surface: str):
        getter = getattr(self.owner, '_plugin_surface_win', None)
        return getter(surface) if callable(getter) else None

    def _surface_visible(self, surface: str) -> bool:
        meta = self._surface_meta(surface)
        visible_attr = str(meta.get('visible_attr') or '')
        return bool(visible_attr and getattr(self.owner, visible_attr, False))

    def _set_surface_visible(self, surface: str, visible: bool):
        visible_attr = str(self._surface_meta(surface).get('visible_attr') or '')
        if visible_attr:
            setattr(self.owner, visible_attr, bool(visible))

    def _eval_surface(self, surface: str, js: str):
        evaluator = getattr(self.owner, '_eval_plugin_surface', None)
        if callable(evaluator):
            evaluator(surface, js)

    def _show_panel_surface(self, surface: str):
        o = self.owner
        if self._surface_visible(surface):
            return
        alpha = getattr(o, '_set_plugin_surface_alpha', None)
        show = getattr(o, '_show_plugin_surface', None)
        title_of = getattr(o, '_plugin_surface_title', None)
        animate = getattr(o, '_animate_window_alpha', None)
        click = getattr(o, '_set_plugin_surface_click_through', None)
        if callable(alpha):
            alpha(surface, 0.0)
        if callable(show):
            show(surface)
        js_obj = _PANEL_SURFACE_JS.get(surface)
        if js_obj:
            self._eval_surface(surface, f'if(window.{js_obj}&&{js_obj}.fadeIn){js_obj}.fadeIn()')
            self._eval_surface(surface, f'if(window.{js_obj}&&{js_obj}.refresh){js_obj}.refresh()')
        title = title_of(surface) if callable(title_of) else ''
        if title and callable(animate):
            threading.Timer(
                0.03,
                lambda: animate(title, 0.0, 1.0, duration_ms=220, steps=8),
            ).start()
        self._set_surface_visible(surface, True)
        if callable(click):
            click(surface, enabled=False, wait_retries=5, ensure_on_top=True)
            threading.Timer(0.5, lambda: click(surface, enabled=False, ensure_on_top=True)).start()

    def _hide_panel_surface(self, surface: str):
        o = self.owner
        if self._surface_visible(surface):
            js_obj = _PANEL_SURFACE_JS.get(surface)
            if js_obj:
                self._eval_surface(surface, f'if(window.{js_obj}&&{js_obj}.fadeOut){js_obj}.fadeOut()')
            hide = getattr(o, '_hide_plugin_surface', None)
            ensure = getattr(o, '_ensure_hidden_panels_passthrough', None)
            def _finish():
                try:
                    if callable(hide):
                        hide(surface)
                    if callable(ensure):
                        ensure()
                except Exception:
                    pass
            threading.Timer(0.25, _finish).start()
        self._set_surface_visible(surface, False)

    def _toggle_panel_surface(self, surface: str):
        if self._surface_visible(surface):
            return self._hide_panel_surface(surface)
        return self._show_panel_surface(surface)

    def _dispatch_act_api(self, name: str, *args, **kwargs):
        o = self.owner
        if name == 'getDataSourceHealth':
            try:
                return act_data_source_health(o)
            except Exception as exc:
                return {'available': False, 'error': str(exc)}
        if name == 'get_data_source_health':
            return self._json(act_data_source_health(o))
        if name == 'diagnose_data_source':
            return self._json(act_data_source_diagnose(o))
        if name == 'copy_data_source_health':
            payload = act_data_source_diagnose(o)
            return self._json({'ok': True, 'text': json.dumps(payload, ensure_ascii=False, indent=2), 'status': payload})
        if name == 'get_report_export_status':
            return self._json(act_report_status(o, limit=self._as_int(self._arg(args, kwargs, 0, 'limit', 20), 20), fmt=self._as_str(self._arg(args, kwargs, 1, 'fmt', 'json'), 'json')))
        if name == 'export_last_report':
            return self._json(act_report_export(o, fmt=self._as_str(self._arg(args, kwargs, 0, 'fmt', 'json'), 'json')))
        if name == 'copy_report_export':
            return self._json(act_report_copy(o, fmt=self._as_str(self._arg(args, kwargs, 0, 'fmt', 'json'), 'json')))
        if name == 'get_mini_parse_status':
            return self._json(act_mini_parse_status(o, formatter_id=self._as_str(self._arg(args, kwargs, 0, 'formatter_id', 'summary_table'), 'summary_table')))
        if name == 'preview_mini_parse':
            return self._json(act_mini_parse_preview(o, formatter_id=self._as_str(self._arg(args, kwargs, 0, 'formatter_id', 'summary_table'), 'summary_table')))
        if name == 'copy_mini_parse':
            return self._json(act_mini_parse_copy(o, formatter_id=self._as_str(self._arg(args, kwargs, 0, 'formatter_id', 'summary_table'), 'summary_table')))
        if name == 'get_selective_parsing_status':
            return self._json(act_selective_parsing_status(o))
        if name == 'update_selective_parsing':
            policy = self._arg(args, kwargs, 0, 'policy', None)
            if isinstance(policy, str):
                try:
                    policy = json.loads(policy or '{}')
                except Exception:
                    policy = {}
            return self._json(act_selective_parsing_update(o, policy if isinstance(policy, dict) else {}))
        if name == 'clear_selective_parsing':
            return self._json(act_selective_parsing_clear(o))
        if name == 'get_history_status':
            return self._json(act_history_status(o, limit=self._as_int(self._arg(args, kwargs, 0, 'limit', 20), 20), query=self._as_str(self._arg(args, kwargs, 1, 'query', ''), '')))
        if name == 'load_history_report':
            return self._json(act_history_load(o, index=self._as_int(self._arg(args, kwargs, 0, 'index', 0), 0), show=bool(self._arg(args, kwargs, 1, 'show', True))))
        if name == 'delete_history_report':
            return self._json(act_history_delete(o, index=self._as_int(self._arg(args, kwargs, 0, 'index', 0), 0)))
        if name == 'clear_history_reports':
            return self._json(act_history_delete(o, clear=True))
        if name == 'choose_offline_import_file':
            try:
                import webview as webview_module
                win = self._surface_win('offline_import') if self._surface_visible('offline_import') else None
                win = win or self._surface_win('report_export')
                if win is None:
                    windows = getattr(webview_module, 'windows', [])
                    win = windows[0] if windows else None
                dialog = getattr(win, 'create_file_dialog', None)
                if not callable(dialog):
                    raise RuntimeError('File dialog is unavailable')
                file_types = (
                    'ACT replay/report (*.json;*.jsonl;*.ndjson;*.xml;*.xml.gz;*.xml.zip;*.zip)',
                    'SAO ACT XML report (*.xml;*.xml.gz;*.xml.zip;*.zip)',
                    'JSON (*.json)',
                    'JSONL/NDJSON (*.jsonl;*.ndjson)',
                    'All files (*.*)',
                )
                try:
                    paths = dialog(getattr(webview_module, 'OPEN_DIALOG', 10), allow_multiple=False, file_types=file_types)
                except TypeError:
                    paths = dialog(getattr(webview_module, 'OPEN_DIALOG', 10), '', False, '', file_types)
                if not paths:
                    return self._json({'ok': False, 'cancelled': True, 'path': '', 'message': 'No file selected'})
                path = paths if isinstance(paths, str) else list(paths)[0]
                return self._json({'ok': True, 'path': str(path)})
            except Exception as exc:
                return self._json({'ok': False, 'path': '', 'message': str(exc), 'errors': [str(exc)]})
        if name == 'import_offline_report':
            try:
                return self._json(act_offline_import_file(o, self._as_str(self._arg(args, kwargs, 0, 'path', ''), ''), persist=bool(self._arg(args, kwargs, 1, 'persist', True)), show=bool(self._arg(args, kwargs, 2, 'show', True))))
            except Exception as exc:
                return self._json({'ok': False, 'message': str(exc), 'errors': [str(exc)]})
        if name == 'get_offline_import_status':
            try:
                return self._json(act_offline_import_status(o, history_limit=self._as_int(self._arg(args, kwargs, 0, 'history_limit', 20), 20)))
            except Exception as exc:
                return self._json({'ok': False, 'message': str(exc), 'accepted_formats': [], 'selected_file': '', 'progress': 0.0, 'status': 'error', 'last_result': {}, 'history': {}, 'errors': [str(exc)]})
        if name == 'get_timeline_status':
            return self._json(act_timeline_status(o, limit=self._as_int(self._arg(args, kwargs, 0, 'limit', 80), 80), query=self._as_str(self._arg(args, kwargs, 1, 'query', ''), '')))
        if name == 'get_aggregate_status':
            return self._json(act_aggregate_status(o, limit=self._as_int(self._arg(args, kwargs, 0, 'limit', 1000), 1000), query=self._as_str(self._arg(args, kwargs, 1, 'query', ''), ''), source=self._as_str(self._arg(args, kwargs, 2, 'source', 'live'), 'live'), window_ms=self._as_int(self._arg(args, kwargs, 3, 'window_ms', 1000), 1000), top_n=self._as_int(self._arg(args, kwargs, 4, 'top_n', 20), 20), encounter_id=self._as_str(self._arg(args, kwargs, 5, 'encounter_id', ''), ''), group_by=self._as_str(self._arg(args, kwargs, 6, 'group_by', 'skill'), 'skill'), group_field=self._as_str(self._arg(args, kwargs, 7, 'group_field', ''), '')))
        if name == 'get_mem_scope_status':
            return self._json(act_mem_scope_status(o, query=self._as_str(self._arg(args, kwargs, 0, 'query', ''), ''), dtype=self._as_str(self._arg(args, kwargs, 1, 'dtype', 'i32'), 'i32'), job_id=self._as_str(self._arg(args, kwargs, 2, 'job_id', ''), '')))
        if name == 'mem_search':
            return self._json(act_mem_search(o, value=self._arg(args, kwargs, 0, 'value', ''), dtype=self._as_str(self._arg(args, kwargs, 1, 'dtype', 'i32'), 'i32'), align=self._as_int(self._arg(args, kwargs, 2, 'align', 0), 0)))
        if name == 'mem_search_status':
            return self._json(act_mem_search_status(o, job_id=self._as_str(self._arg(args, kwargs, 0, 'job_id', ''), '')))
        if name == 'mem_narrow':
            return self._json(act_mem_narrow(o, job_id=self._as_str(self._arg(args, kwargs, 0, 'job_id', ''), ''), value=self._arg(args, kwargs, 1, 'value', '')))
        if name == 'mem_search_cancel':
            return self._json(act_mem_search_cancel(o, job_id=self._as_str(self._arg(args, kwargs, 0, 'job_id', ''), '')))
        if name == 'mem_attr_map':
            return self._json(act_mem_attr_map(o, ent_addr=self._arg(args, kwargs, 0, 'ent_addr', '')))
        if name == 'play_timeline':
            return self._json(act_timeline_play(o, speed=_safe_timeline_speed(self._arg(args, kwargs, 0, 'speed', 1.0))))
        if name == 'pause_timeline':
            return self._json(act_timeline_pause(o))
        if name == 'step_timeline':
            return self._json(act_timeline_step(o, delta_ms=self._as_int(self._arg(args, kwargs, 0, 'delta_ms', 1000), 1000)))
        if name == 'seek_timeline':
            return self._json(act_timeline_seek(o, cursor_ms=self._as_int(self._arg(args, kwargs, 0, 'cursor_ms', 0), 0)))
        if name == 'set_timeline_speed':
            return self._json(act_timeline_set_speed(o, speed=_safe_timeline_speed(self._arg(args, kwargs, 0, 'speed', 1.0))))
        if name == 'filter_timeline':
            return self._json(act_timeline_filter(o, query=self._as_str(self._arg(args, kwargs, 0, 'query', ''), '')))
        if name == 'get_action_log_status':
            params = {'limit': self._as_int(self._arg(args, kwargs, 0, 'limit', 80), 80), 'query': self._as_str(self._arg(args, kwargs, 1, 'query', ''), ''), 'topic': self._as_str(self._arg(args, kwargs, 2, 'topic', ''), ''), 'source': self._as_str(self._arg(args, kwargs, 4, 'source', 'live'), 'live'), 'encounter_id': self._as_str(self._arg(args, kwargs, 5, 'encounter_id', ''), ''), 'offset': self._as_int(self._arg(args, kwargs, 6, 'offset', 0), 0)}
            cursor_ms = self._arg(args, kwargs, 3, 'cursor_ms', None)
            if cursor_ms is not None:
                params['cursor_ms'] = self._as_int(cursor_ms, 0)
            return self._json(act_action_log_status(o, **params))
        if name == 'search_action_log':
            return self._json(act_action_log_search(o, query=self._as_str(self._arg(args, kwargs, 0, 'query', ''), ''), limit=self._as_int(self._arg(args, kwargs, 1, 'limit', 80), 80), source=self._as_str(self._arg(args, kwargs, 2, 'source', 'live'), 'live'), encounter_id=self._as_str(self._arg(args, kwargs, 3, 'encounter_id', ''), ''), offset=self._as_int(self._arg(args, kwargs, 4, 'offset', 0), 0)))
        if name == 'filter_action_log':
            query = self._arg(args, kwargs, 1, 'query', None)
            return self._json(act_action_log_filter(o, topic=self._as_str(self._arg(args, kwargs, 0, 'topic', ''), ''), query=None if query is None else self._as_str(query, ''), limit=self._as_int(self._arg(args, kwargs, 2, 'limit', 80), 80), source=self._as_str(self._arg(args, kwargs, 3, 'source', 'live'), 'live'), encounter_id=self._as_str(self._arg(args, kwargs, 4, 'encounter_id', ''), ''), offset=self._as_int(self._arg(args, kwargs, 5, 'offset', 0), 0)))
        if name == 'jump_action_log_time':
            topic = self._arg(args, kwargs, 5, 'topic', None)
            return self._json(act_action_log_jump_to_time(o, cursor_ms=self._as_int(self._arg(args, kwargs, 0, 'cursor_ms', 0), 0), limit=self._as_int(self._arg(args, kwargs, 1, 'limit', 80), 80), source=self._as_str(self._arg(args, kwargs, 2, 'source', 'live'), 'live'), encounter_id=self._as_str(self._arg(args, kwargs, 3, 'encounter_id', ''), ''), offset=self._as_int(self._arg(args, kwargs, 4, 'offset', 0), 0), topic=None if topic is None else self._as_str(topic, '')))
        if name == 'show_action_log_at':
            self._show_panel_surface('action_log')
            topic = self._arg(args, kwargs, 3, 'topic', None)
            result = act_action_log_jump_to_time(o, cursor_ms=self._as_int(self._arg(args, kwargs, 0, 'cursor_ms', 0), 0), limit=80, source=self._as_str(self._arg(args, kwargs, 1, 'source', 'live'), 'live'), encounter_id=self._as_str(self._arg(args, kwargs, 2, 'encounter_id', ''), ''), offset=0, topic=None if topic is None else self._as_str(topic, ''))
            self._eval_surface('action_log', 'if(window.ActionLog&&ActionLog.refresh)ActionLog.refresh()')
            return self._json(result)
        if name == 'copy_action_log':
            return self._json(act_action_log_copy(o, limit=self._as_int(self._arg(args, kwargs, 0, 'limit', 80), 80), query=self._as_str(self._arg(args, kwargs, 1, 'query', ''), ''), topic=self._as_str(self._arg(args, kwargs, 2, 'topic', ''), ''), source=self._as_str(self._arg(args, kwargs, 3, 'source', 'live'), 'live'), encounter_id=self._as_str(self._arg(args, kwargs, 4, 'encounter_id', ''), ''), offset=self._as_int(self._arg(args, kwargs, 5, 'offset', 0), 0)))
        if name == 'get_death_recap_status':
            return self._json(act_death_recap_status(o, limit=self._as_int(self._arg(args, kwargs, 0, 'limit', 80), 80), window_s=self._as_float(self._arg(args, kwargs, 1, 'window_s', 8.0), 8.0), entity_id=self._arg(args, kwargs, 2, 'entity_id', None)))
        if name == 'copy_death_recap':
            return self._json(act_death_recap_copy(o, limit=self._as_int(self._arg(args, kwargs, 0, 'limit', 80), 80), window_s=self._as_float(self._arg(args, kwargs, 1, 'window_s', 8.0), 8.0), entity_id=self._arg(args, kwargs, 2, 'entity_id', None)))
        if name == 'get_graph_timeseries_status':
            params = {'limit': self._as_int(self._arg(args, kwargs, 1, 'limit', 120), 120)}
            for idx, key in ((0, 'metric'), (2, 'query'), (3, 'topic')):
                value = self._arg(args, kwargs, idx, key, None)
                if value is not None:
                    params[key] = self._as_str(value, '')
            time_range_ms = self._arg(args, kwargs, 4, 'time_range_ms', None)
            if time_range_ms is not None:
                params['time_range_ms'] = self._as_int(time_range_ms, 0)
            return self._json(act_graph_timeseries_status(o, **params))
        if name == 'select_graph_metric':
            return self._json(act_graph_timeseries_select_metric(o, metric=self._as_str(self._arg(args, kwargs, 0, 'metric', 'damage'), 'damage'), limit=self._as_int(self._arg(args, kwargs, 1, 'limit', 120), 120)))
        if name == 'zoom_graph_timeseries':
            return self._json(act_graph_timeseries_zoom(o, time_range_ms=self._as_int(self._arg(args, kwargs, 0, 'time_range_ms', 0), 0), limit=self._as_int(self._arg(args, kwargs, 1, 'limit', 120), 120)))
        if name == 'filter_graph_timeseries':
            query = self._arg(args, kwargs, 0, 'query', None)
            topic = self._arg(args, kwargs, 1, 'topic', None)
            return self._json(act_graph_timeseries_filter(o, query=None if query is None else self._as_str(query, ''), topic=None if topic is None else self._as_str(topic, ''), limit=self._as_int(self._arg(args, kwargs, 2, 'limit', 120), 120)))
        if name == 'export_graph_timeseries':
            metric = self._arg(args, kwargs, 0, 'metric', None)
            query = self._arg(args, kwargs, 2, 'query', None)
            topic = self._arg(args, kwargs, 3, 'topic', None)
            return self._json(act_graph_timeseries_export(o, metric=None if metric is None else self._as_str(metric, ''), limit=self._as_int(self._arg(args, kwargs, 1, 'limit', 120), 120), query=None if query is None else self._as_str(query, ''), topic=None if topic is None else self._as_str(topic, '')))
        if name == 'get_combatant_drilldown_status':
            return self._json(act_combatant_drilldown_status(o, combatant_id=self._arg(args, kwargs, 0, 'combatant_id', None), query=self._arg(args, kwargs, 1, 'query', None), focus_target=self._arg(args, kwargs, 2, 'focus_target', None)))
        if name == 'filter_combatant_drilldown':
            return self._json(act_combatant_drilldown_filter(o, combatant_id=self._arg(args, kwargs, 0, 'combatant_id', None), query=self._as_str(self._arg(args, kwargs, 1, 'query', ''), '')))
        if name == 'focus_combatant_target':
            return self._json(act_combatant_drilldown_focus_target(o, combatant_id=self._arg(args, kwargs, 0, 'combatant_id', None), target_id=self._as_str(self._arg(args, kwargs, 1, 'target_id', ''), '')))
        if name == 'back_combatant_drilldown':
            return self._json(act_combatant_drilldown_back(o))
        if name == 'get_skill_drilldown_status':
            return self._json(act_skill_drilldown_status(o, combatant_id=self._arg(args, kwargs, 0, 'combatant_id', None), skill_id=self._arg(args, kwargs, 1, 'skill_id', None), query=self._arg(args, kwargs, 2, 'query', None), limit=self._as_int(self._arg(args, kwargs, 3, 'limit', 80), 80)))
        if name == 'filter_skill_drilldown':
            return self._json(act_skill_drilldown_filter(o, combatant_id=self._arg(args, kwargs, 0, 'combatant_id', None), skill_id=self._arg(args, kwargs, 1, 'skill_id', None), query=self._as_str(self._arg(args, kwargs, 2, 'query', ''), ''), limit=self._as_int(self._arg(args, kwargs, 3, 'limit', 80), 80)))
        if name == 'copy_skill_drilldown':
            return self._json(act_skill_drilldown_copy(o, combatant_id=self._arg(args, kwargs, 0, 'combatant_id', None), skill_id=self._arg(args, kwargs, 1, 'skill_id', None), query=self._arg(args, kwargs, 2, 'query', None), limit=self._as_int(self._arg(args, kwargs, 3, 'limit', 80), 80)))
        if name == 'back_skill_drilldown':
            return self._json(act_skill_drilldown_back(o))
        if name == 'open_skill_drilldown':
            cid = self._as_str(self._arg(args, kwargs, 0, 'combatant_id', ''), '')
            sid = self._as_str(self._arg(args, kwargs, 1, 'skill_id', ''), '')
            self._show_panel_surface('skill_drilldown')
            status = act_skill_drilldown_status(o, combatant_id=cid or None, skill_id=sid or None, query=None, limit=80)
            js = "(function(){var c=document.getElementById('combatantId'),s=document.getElementById('skillId');" + "if(c)c.value=" + json.dumps(cid) + ";if(s)s.value=" + json.dumps(sid) + ";" + "if(window.SkillDrilldown&&SkillDrilldown.refresh)SkillDrilldown.refresh();})()"
            self._eval_surface('skill_drilldown', js)
            return self._json(status)
        if name == 'get_trigger_status':
            return self._json(act_trigger_status(o))
        if name == 'enable_trigger':
            return self._json(act_trigger_enable(o, self._as_str(self._arg(args, kwargs, 0, 'rule_id', ''), '')))
        if name == 'disable_trigger':
            return self._json(act_trigger_disable(o, self._as_str(self._arg(args, kwargs, 0, 'rule_id', ''), '')))
        if name == 'reload_triggers':
            return self._json(act_trigger_reload(o))
        if name == 'test_trigger':
            return self._json(act_trigger_test(o, self._as_str(self._arg(args, kwargs, 0, 'rule_id', ''), '')))
        raise AttributeError(name)

    # ── common helpers ──
    def _json_error(self, exc: Exception, **extra):
        payload = {'ok': False, 'message': str(exc)}
        payload.update(extra)
        return json.dumps(payload, ensure_ascii=False)

    def resolve_web_uri(self, filename: str) -> Optional[str]:
        name = str(filename or '')
        target = None
        try:
            resolve = getattr(self.ctx, 'resolve_web', None)
            if callable(resolve):
                target = resolve(name)
        except Exception:
            target = None
        if not target:
            base = os.path.abspath(os.path.join(_PLUGIN_ROOT, 'web'))
            candidate = os.path.abspath(os.path.join(base, name))
            try:
                if os.path.commonpath([base, candidate]) == base and os.path.isfile(candidate):
                    target = candidate
            except Exception:
                target = None
        if not target:
            return None
        return Path(str(target)).resolve().as_uri()

    def _browse(self, root: str, mode: str = 'file') -> dict:
        data = json.loads(self.owner._api.browse_dir(root))
        data['mode'] = mode
        return data

    def _menu_asset_text(self, name: str) -> str:
        path = os.path.join(_PLUGIN_ROOT, 'web', name)
        with open(path, 'r', encoding='utf-8') as fh:
            return fh.read()

    def _inject_menu_assets(self) -> None:
        css = self._menu_asset_text('sr_menu.css')
        slots = self._menu_asset_text('sr_menu_slots.html')
        script = self._menu_asset_text('sr_menu.js')
        js = r"""
(function() {
    if (window.__starResonanceMenuInjected) return true;
    window.__starResonanceMenuInjected = true;
    var cssText = __CSS__;
    var slotsHtml = __SLOTS__;
    var scriptText = __SCRIPT__;
    var style = document.getElementById('star-resonance-menu-css');
    if (!style) {
        style = document.createElement('style');
        style.id = 'star-resonance-menu-css';
        document.head.appendChild(style);
    }
    style.textContent = cssText;

    var menuBar = document.getElementById('menu-bar');
    var menuPosition = document.getElementById('menu-position');
    function addMenuItem(name, iconHtml, beforeName, options) {
        if (!menuBar || menuBar.querySelector('[data-name="' + name + '"]')) return;
        options = options || {};
        var li = document.createElement('li');
        li.className = 'item';
        li.setAttribute('data-name', name);
        li.setAttribute('data-sr-menu', '1');
        if (options.persistent) li.setAttribute('data-menu-persistent', '1');
        li.innerHTML = '<div class="in-circle">' + iconHtml + '</div>';
        var before = beforeName ? menuBar.querySelector('[data-name="' + beforeName + '"]') : null;
        menuBar.insertBefore(li, before || null);
    }
    addMenuItem('userInfo', '<span class="sao-icon sao-icon-user"></span>', 'theme', { persistent: true });
    addMenuItem('panels', '<span class="sao-icon sao-icon-settings"></span>', 'theme');
    addMenuItem('skillEffects', '<span style="font-size:15px;line-height:1;color:var(--menu-icon);">9</span>', 'theme');
    addMenuItem('autoKeys', '<span class="sao-icon sao-icon-settings"></span>', 'theme');
    addMenuItem('bossRaid', '<span class="sao-icon sao-icon-panel"></span>', 'theme');

    var childBar = document.getElementById('child-bar');
    if (childBar) {
        var tmp = document.createElement('div');
        tmp.innerHTML = slotsHtml;
        Array.prototype.slice.call(tmp.children).forEach(function(node) {
            if (node.id && document.getElementById(node.id)) return;
            var host = node.getAttribute ? node.getAttribute('data-sr-menu-host') : '';
            if (host === 'position') {
                if (menuPosition) menuPosition.insertBefore(node, menuBar || null);
                return;
            }
            childBar.appendChild(node);
        });
    }
    if (!document.getElementById('star-resonance-menu-js')) {
        var scriptEl = document.createElement('script');
        scriptEl.id = 'star-resonance-menu-js';
        scriptEl.textContent = scriptText;
        document.body.appendChild(scriptEl);
    }
    return true;
})();
""".replace('__CSS__', json.dumps(css, ensure_ascii=False)) \
            .replace('__SLOTS__', json.dumps(slots, ensure_ascii=False)) \
            .replace('__SCRIPT__', json.dumps(script, ensure_ascii=False))
        getattr(self.owner, '_eval_menu', lambda _: None)(js)

    def make_window_api(self, surface: str):
        key = str(surface or '').strip().lower().replace('-', '_')
        mapping = {
            'dps': StarResonanceDpsWindowAPI,
            'raid_editor': StarResonanceRaidEditorAPI,
            'autokey_editor': StarResonanceAutoKeyEditorAPI,
            'auto_key_editor': StarResonanceAutoKeyEditorAPI,
            'commander': StarResonanceCommanderAPI,
        }
        api_cls = mapping.get(key)
        if api_cls is None:
            return getattr(self.owner, '_api', None)
        return api_cls(self.owner)

    def create_overlay_windows(self, webview_module, web_file_uri, monitor_left, monitor_top, screen_w, screen_h):
        o = self.owner
        _sw = max(1, int(screen_w or 1))
        _sh = max(1, int(screen_h or 1))

        def _plugin_web_uri(filename: str) -> str:
            return self.resolve_web_uri(filename) or web_file_uri(filename)

        def _register(surface: str, win, title: str, **meta):
            reg = getattr(o, '_register_plugin_surface', None)
            if callable(reg):
                meta.setdefault('plugin_id', str(getattr(self.ctx, 'plugin_id', '') or ''))
                reg(surface, win, title=title, **meta)
            return win

        def _toggle_meta(visible_attr: str, theme_key: str, **extra):
            meta = {
                'click_through': False,
                'on_top': True,
                'surface_group': 'plugin',
                'visible_attr': visible_attr,
                'theme_key': theme_key,
            }
            meta.update(extra)
            return meta

        hp_url = _plugin_web_uri('hp.html')
        hud_w = int(_sw * 0.75)
        if getattr(o, '_hp_fullscreen', False):
            hp_x, hp_y = int(monitor_left), int(monitor_top)
        else:
            hp_x = int(monitor_left) + max(0, int((_sw - hud_w) / 2))
            hp_y = int(monitor_top) + max(0, int((_sh - 500) / 2))
        hp_w = _sw if getattr(o, '_hp_fullscreen', False) else hud_w
        hp_h = _sh if getattr(o, '_hp_fullscreen', False) else 500
        o.hp_win = _register('hp', webview_module.create_window(
            'SAO-HP', hp_url,
            width=hp_w, height=hp_h,
            x=hp_x, y=hp_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._api,
        ), 'SAO-HP', click_through=False, dotnet_transparency=True, on_top=True)

        alert_url = _plugin_web_uri('alert.html')
        alert_w, alert_h = 416, 226
        o.alert_win = _register('alert', webview_module.create_window(
            'SAO Alert', alert_url,
            width=alert_w, height=alert_h,
            x=max(0, int((_sw - alert_w) / 2)),
            y=max(32, int(_sh * 0.16)),
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._api,
        ), 'SAO Alert', click_through=False, dotnet_transparency=True, on_top=True)

        mapbanner_url = _plugin_web_uri('mapbanner.html')
        mapbanner_w = max(640, int(_sw * 0.6))
        mapbanner_h = 280
        o.mapbanner_win = _register('mapbanner', webview_module.create_window(
            'SAO MapBanner', mapbanner_url,
            width=o._to_webview_px(mapbanner_w),
            height=o._to_webview_px(mapbanner_h),
            x=o._to_webview_px(int(monitor_left) + max(0, int((_sw - mapbanner_w) / 2))),
            y=o._to_webview_px(int(monitor_top) + max(0, int((_sh - mapbanner_h) / 2))),
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._api,
        ), 'SAO MapBanner', click_through=True, dotnet_transparency=True, on_top=True)

        mech_banner_url = _plugin_web_uri('mech_banner.html')
        mech_banner_w = 600
        mech_banner_h = 3 * 64 + 2 * 8 + 12
        mb_mon_w = o._to_webview_px(_sw)
        o.mech_banner_win = _register('mech_banner', webview_module.create_window(
            'SAO MechBanner', mech_banner_url,
            width=mech_banner_w,
            height=mech_banner_h,
            x=o._to_webview_px(int(monitor_left)) + max(0, (mb_mon_w - mech_banner_w) // 2),
            y=o._to_webview_px(int(monitor_top)) + max(0, int(o._to_webview_px(_sh) * 0.08)),
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._api,
        ), 'SAO MechBanner', click_through=True, dotnet_transparency=True, on_top=True)

        skillfx_url = _plugin_web_uri('skillfx.html')
        o.skillfx_win = _register('skillfx', webview_module.create_window(
            'SAO SkillFX', skillfx_url,
            width=o._to_webview_px(max(320, int(_sw * 0.42))),
            height=o._to_webview_px(max(140, int(_sh * 0.20))),
            x=o._to_webview_px(int(monitor_left) + max(0, int(_sw * 0.29))),
            y=o._to_webview_px(int(monitor_top) + max(0, int(_sh * 0.74))),
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._api,
        ), 'SAO SkillFX', click_through=True, dotnet_transparency=True, on_top=True)

        boss_hp_url = _plugin_web_uri('boss_hp.html')
        boss_hp_geom = o._calc_boss_hp_geometry()
        o._boss_hp_geometry = dict(boss_hp_geom)
        o.boss_hp_win = _register('boss_hp', webview_module.create_window(
            'SAO-BossHP', boss_hp_url,
            width=int(boss_hp_geom['width']), height=int(boss_hp_geom['height']),
            x=int(boss_hp_geom['x']), y=int(boss_hp_geom['y']),
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._api,
        ), 'SAO-BossHP', click_through=True, dotnet_transparency=True, on_top=True)
        _register('bosshp', o.boss_hp_win, 'SAO-BossHP',
                  click_through=True, dotnet_transparency=True, on_top=True)

        dps_url = _plugin_web_uri('dps.html')
        dps_w = max(320, int(min(_sw, 1920) * 0.19))
        dps_h = max(420, int(min(_sh, 1080) * 0.48))
        o._dps_base_w = int(dps_w)
        o._dps_base_h = int(dps_h)
        detail_w_default = max(760, int(min(_sw, 1920) * 0.40))
        detail_h_default = max(560, int(min(_sh, 1080) * 0.56))
        try:
            o._dps_detail_w = int(o._get_setting('dps_detail_w', detail_w_default))
            o._dps_detail_h = int(o._get_setting('dps_detail_h', detail_h_default))
        except Exception:
            o._dps_detail_w = detail_w_default
            o._dps_detail_h = detail_h_default
        o._dps_detail_w = max(520, min(1180, int(o._dps_detail_w)))
        o._dps_detail_h = max(420, min(900, int(o._dps_detail_h)))
        dps_x = max(0, _sw - dps_w - max(16, int(_sw * 0.012)))
        dps_y = max(0, int(_sh * 0.18))
        o._dps_api = self.make_window_api('dps')
        o.dps_win = _register('dps', webview_module.create_window(
            'SAO-DPS', dps_url,
            width=dps_w, height=dps_h,
            x=dps_x, y=dps_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._dps_api,
        ), 'SAO-DPS', click_through=False, dotnet_transparency=False, on_top=True)

        buff_cov_url = _plugin_web_uri('buff_coverage.html')
        buff_w = int(getattr(o, '_dps_base_w', 0)) or max(300, int(min(_sw, 1920) * 0.18))
        buff_h = max(220, int(min(_sh, 1080) * 0.24))
        buff_x = dps_x
        buff_y = min(dps_y + dps_h + 12, max(0, _sh - buff_h - 24))
        o.buff_coverage_win = _register('buffmon', webview_module.create_window(
            'SAO-BuffCoverage', buff_cov_url,
            width=buff_w, height=buff_h,
            x=buff_x, y=buff_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._api,
        ), 'SAO-BuffCoverage', click_through=True, dotnet_transparency=False, on_top=True)

        raid_editor_url = _plugin_web_uri('raid_editor.html')
        raid_w = max(360, int(min(_sw, 1920) * 0.22))
        raid_h = max(460, int(min(_sh, 1080) * 0.52))
        raid_x = max(16, int(_sw * 0.012))
        raid_y = max(0, int(_sh * 0.18))
        o._raid_editor_api = self.make_window_api('raid_editor')
        o.raid_editor_win = _register('raid_editor', webview_module.create_window(
            'SAO-RaidEditor', raid_editor_url,
            width=raid_w, height=raid_h,
            x=raid_x, y=raid_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._raid_editor_api,
        ), 'SAO-RaidEditor', click_through=False, dotnet_transparency=False, on_top=True)

        autokey_editor_url = _plugin_web_uri('autokey_editor.html')
        autokey_w = max(340, int(min(_sw, 1920) * 0.20))
        autokey_h = max(400, int(min(_sh, 1080) * 0.44))
        autokey_x = max(16, int(_sw * 0.012))
        autokey_y = raid_y + raid_h + 12
        o._autokey_editor_api = self.make_window_api('autokey_editor')
        o.autokey_editor_win = _register('autokey_editor', webview_module.create_window(
            'SAO-AutoKeyEditor', autokey_editor_url,
            width=autokey_w, height=autokey_h,
            x=autokey_x, y=autokey_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._autokey_editor_api,
        ), 'SAO-AutoKeyEditor', click_through=False, dotnet_transparency=False, on_top=True)

        commander_url = _plugin_web_uri('commander.html')
        commander_w = max(300, int(min(_sw, 1920) * 0.18))
        commander_h = max(380, int(min(_sh, 1080) * 0.42))
        commander_x = max(16, int(_sw * 0.25))
        commander_y = max(0, int(_sh * 0.15))
        o._commander_api = self.make_window_api('commander')
        o.commander_win = _register('commander', webview_module.create_window(
            'SAO-Commander', commander_url,
            width=commander_w, height=commander_h,
            x=commander_x, y=commander_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._commander_api,
        ), 'SAO-Commander', click_through=False, dotnet_transparency=False, on_top=True)

        trigger_timer_url = _plugin_web_uri('trigger_timer_manager.html')
        trigger_timer_w = max(640, int(min(_sw, 1920) * 0.40))
        trigger_timer_h = max(520, int(min(_sh, 1080) * 0.52))
        trigger_timer_x = max(16, int(monitor_left + (_sw - trigger_timer_w) * 0.58))
        trigger_timer_y = max(24, int(monitor_top + (_sh - trigger_timer_h) * 0.23))
        o.trigger_timer_win = _register('trigger_timer', webview_module.create_window(
            'SAO-TriggerTimerManager', trigger_timer_url,
            width=trigger_timer_w, height=trigger_timer_h,
            x=trigger_timer_x, y=trigger_timer_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._api,
        ), 'SAO-TriggerTimerManager', **_toggle_meta('_trigger_timer_manager_visible', 'trigger_timer'))

        data_source_health_url = _plugin_web_uri('data_source_health.html')
        data_source_health_w = max(640, int(min(_sw, 1920) * 0.40))
        data_source_health_h = max(500, int(min(_sh, 1080) * 0.50))
        data_source_health_x = max(16, int(monitor_left + (_sw - data_source_health_w) * 0.45))
        data_source_health_y = max(24, int(monitor_top + (_sh - data_source_health_h) * 0.28))
        o.data_source_health_win = _register('data_source_health', webview_module.create_window(
            'SAO-DataSourceHealth', data_source_health_url,
            width=data_source_health_w, height=data_source_health_h,
            x=data_source_health_x, y=data_source_health_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._api,
        ), 'SAO-DataSourceHealth', **_toggle_meta('_data_source_health_visible', 'data_source_health'))

        report_export_url = _plugin_web_uri('act_report_export.html')
        report_export_w = max(680, int(min(_sw, 1920) * 0.42))
        report_export_h = max(520, int(min(_sh, 1080) * 0.52))
        report_export_x = max(16, int(monitor_left + (_sw - report_export_w) * 0.40))
        report_export_y = max(24, int(monitor_top + (_sh - report_export_h) * 0.24))
        o.report_export_win = _register('report_export', webview_module.create_window(
            'SAO-ReportExport', report_export_url,
            width=report_export_w, height=report_export_h,
            x=report_export_x, y=report_export_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._api,
        ), 'SAO-ReportExport', **_toggle_meta('_report_export_visible', 'report_export'))

        offline_import_url = _plugin_web_uri('act_offline_import.html')
        offline_import_w = max(700, int(min(_sw, 1920) * 0.44))
        offline_import_h = max(520, int(min(_sh, 1080) * 0.52))
        offline_import_x = max(16, int(monitor_left + (_sw - offline_import_w) * 0.42))
        offline_import_y = max(24, int(monitor_top + (_sh - offline_import_h) * 0.26))
        o.offline_import_win = _register('offline_import', webview_module.create_window(
            'SAO-OfflineImport', offline_import_url,
            width=offline_import_w, height=offline_import_h,
            x=offline_import_x, y=offline_import_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._api,
        ), 'SAO-OfflineImport', **_toggle_meta('_offline_import_visible', 'offline_import'))

        timeline_vcr_url = _plugin_web_uri('act_timeline_vcr.html')
        timeline_vcr_w = max(700, int(min(_sw, 1920) * 0.43))
        timeline_vcr_h = max(520, int(min(_sh, 1080) * 0.52))
        timeline_vcr_x = max(16, int(monitor_left + (_sw - timeline_vcr_w) * 0.34))
        timeline_vcr_y = max(24, int(monitor_top + (_sh - timeline_vcr_h) * 0.18))
        o.timeline_vcr_win = _register('timeline_vcr', webview_module.create_window(
            'SAO-TimelineVCR', timeline_vcr_url,
            width=timeline_vcr_w, height=timeline_vcr_h,
            x=timeline_vcr_x, y=timeline_vcr_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._api,
        ), 'SAO-TimelineVCR', **_toggle_meta('_timeline_vcr_visible', 'timeline_vcr'))

        act_aggregate_url = _plugin_web_uri('act_aggregate.html')
        act_aggregate_w = max(820, int(min(_sw, 1920) * 0.54))
        act_aggregate_h = max(620, int(min(_sh, 1080) * 0.62))
        act_aggregate_x = max(16, int(monitor_left + (_sw - act_aggregate_w) * 0.25))
        act_aggregate_y = max(22, int(monitor_top + (_sh - act_aggregate_h) * 0.14))
        o.act_aggregate_win = _register('act_aggregate', webview_module.create_window(
            'SAO-ActAggregate', act_aggregate_url,
            width=act_aggregate_w, height=act_aggregate_h,
            x=act_aggregate_x, y=act_aggregate_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._api,
        ), 'SAO-ActAggregate', **_toggle_meta('_act_aggregate_visible', 'act_aggregate'))

        mem_scope_url = _plugin_web_uri('mem_scope.html')
        mem_scope_w = max(780, int(min(_sw, 1920) * 0.52))
        mem_scope_h = max(620, int(min(_sh, 1080) * 0.62))
        mem_scope_x = max(16, int(monitor_left + (_sw - mem_scope_w) * 0.30))
        mem_scope_y = max(22, int(monitor_top + (_sh - mem_scope_h) * 0.12))
        o.mem_scope_win = _register('mem_scope', webview_module.create_window(
            'SAO-MemScope', mem_scope_url,
            width=mem_scope_w, height=mem_scope_h,
            x=mem_scope_x, y=mem_scope_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._api,
        ), 'SAO-MemScope', **_toggle_meta('_mem_scope_visible', 'mem_scope'))

        action_log_url = _plugin_web_uri('act_action_log.html')
        action_log_w = max(760, int(min(_sw, 1920) * 0.48))
        action_log_h = max(520, int(min(_sh, 1080) * 0.54))
        action_log_x = max(16, int(monitor_left + (_sw - action_log_w) * 0.30))
        action_log_y = max(24, int(monitor_top + (_sh - action_log_h) * 0.20))
        o.action_log_win = _register('action_log', webview_module.create_window(
            'SAO-ActionLog', action_log_url,
            width=action_log_w, height=action_log_h,
            x=action_log_x, y=action_log_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._api,
        ), 'SAO-ActionLog', **_toggle_meta('_action_log_visible', 'action_log'))

        death_recap_url = _plugin_web_uri('act_death_recap.html')
        death_recap_w = max(720, int(min(_sw, 1920) * 0.44))
        death_recap_h = max(500, int(min(_sh, 1080) * 0.50))
        death_recap_x = max(18, int(monitor_left + (_sw - death_recap_w) * 0.32))
        death_recap_y = max(26, int(monitor_top + (_sh - death_recap_h) * 0.23))
        o.death_recap_win = _register('death_recap', webview_module.create_window(
            'SAO-DeathRecap', death_recap_url,
            width=death_recap_w, height=death_recap_h,
            x=death_recap_x, y=death_recap_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._api,
        ), 'SAO-DeathRecap', **_toggle_meta('_death_recap_visible', 'death_recap'))

        graph_timeseries_url = _plugin_web_uri('act_graph_timeseries.html')
        graph_timeseries_w = max(780, int(min(_sw, 1920) * 0.50))
        graph_timeseries_h = max(540, int(min(_sh, 1080) * 0.55))
        graph_timeseries_x = max(16, int(monitor_left + (_sw - graph_timeseries_w) * 0.27))
        graph_timeseries_y = max(24, int(monitor_top + (_sh - graph_timeseries_h) * 0.22))
        o.graph_timeseries_win = _register('graph_timeseries', webview_module.create_window(
            'SAO-GraphTimeseries', graph_timeseries_url,
            width=graph_timeseries_w, height=graph_timeseries_h,
            x=graph_timeseries_x, y=graph_timeseries_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._api,
        ), 'SAO-GraphTimeseries', **_toggle_meta('_graph_timeseries_visible', 'graph_timeseries'))

        combatant_drilldown_url = _plugin_web_uri('act_combatant_drilldown.html')
        combatant_drilldown_w = max(720, int(min(_sw, 1920) * 0.46))
        combatant_drilldown_h = max(520, int(min(_sh, 1080) * 0.54))
        combatant_drilldown_x = max(20, int(monitor_left + (_sw - combatant_drilldown_w) * 0.31))
        combatant_drilldown_y = max(28, int(monitor_top + (_sh - combatant_drilldown_h) * 0.25))
        o.combatant_drilldown_win = _register('combatant_drilldown', webview_module.create_window(
            'SAO-CombatantDrilldown', combatant_drilldown_url,
            width=combatant_drilldown_w, height=combatant_drilldown_h,
            x=combatant_drilldown_x, y=combatant_drilldown_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._api,
        ), 'SAO-CombatantDrilldown', **_toggle_meta('_combatant_drilldown_visible', 'combatant_drilldown'))

        skill_drilldown_url = _plugin_web_uri('act_skill_drilldown.html')
        skill_drilldown_w = max(720, int(min(_sw, 1920) * 0.45))
        skill_drilldown_h = max(500, int(min(_sh, 1080) * 0.52))
        skill_drilldown_x = max(24, int(monitor_left + (_sw - skill_drilldown_w) * 0.34))
        skill_drilldown_y = max(32, int(monitor_top + (_sh - skill_drilldown_h) * 0.28))
        o.skill_drilldown_win = _register('skill_drilldown', webview_module.create_window(
            'SAO-SkillDrilldown', skill_drilldown_url,
            width=skill_drilldown_w, height=skill_drilldown_h,
            x=skill_drilldown_x, y=skill_drilldown_y,
            frameless=True,
            easy_drag=False,
            transparent=True,
            hidden=True,
            on_top=True,
            js_api=o._api,
        ), 'SAO-SkillDrilldown', **_toggle_meta('_skill_drilldown_visible', 'skill_drilldown'))
        return True

    def initialize_owner_state(self) -> None:
        o = self.owner
        o.skillfx_win = None
        o.mapbanner_win = None
        o.mech_banner_win = None
        o.boss_hp_win = None
        o.buff_coverage_win = None
        o.dps_win = None
        o.raid_editor_win = None
        o.autokey_editor_win = None
        o.commander_win = None
        o._boss_hp_hwnd = 0
        o._boss_hp_visible = False
        o._boss_hp_bar_shown = False
        o._boss_hp_geometry = None
        o._bb_last_target_uuid = 0
        o._bb_last_damage_ts = 0.0
        o._bb_recent_targets = {}
        o._bb_damage_timeout = 60.0
        o._pending_combat_reset_after = 0.0
        o._pending_combat_reset_reason = ''
        o._damage_self_fallback_log_ts = 0.0
        o._dps_hwnd = 0
        o._dps_visible = False
        o._dps_faded = False
        o._dps_fade_seq = 0
        o._dps_mode = 'hidden'
        o._dps_tracker = None
        o._dps_api = None
        o._dps_history_store = None
        o._encounter_mgr = None
        o._act_trigger_engine = None
        o._dps_last_report_available = False
        o._dps_base_w = 0
        o._dps_base_h = 0
        o._dps_detail_w = 760
        o._dps_detail_h = 560
        o._dps_detail_mode = False
        o._raid_editor_visible = False
        o._raid_editor_api = None
        o._autokey_editor_visible = False
        o._autokey_editor_api = None
        o._commander_visible = False
        o._commander_api = None
        o._skillfx_hwnd = 0
        o._mapbanner_hwnd = 0
        o._mapbanner_nonce = 0
        o._mapbanner_last_name = ''
        o._mapbanner_last_ts = 0.0
        o._mapbanner_timer = None
        o._mech_banner_hwnd = 0
        o._mech_banner_shown = False
        o._skillfx_visible = False
        o._skillfx_slot_count = 9
        o._skillfx_layout = None
        o._last_ready_slots = {}
        o._burst_seen_cooling = set()
        o._last_watched_signature = ()
        o._last_burst_ready = False
        o._last_burst_slot = 0
        o._last_boss_timer_text = ''
        o._last_boss_timer_urgency = ''
        o._last_boss_event = {}
        # Game identity / progression / vitals — owned by the plugin so the
        # platform stays free of game-specific defaults.
        if not hasattr(o, '_username'):
            o._username = ''
        if not hasattr(o, '_profession'):
            o._profession = ''
        if not hasattr(o, '_level'):
            o._level = 0
        if not hasattr(o, '_last_displayed_level_base'):
            o._last_displayed_level_base = 0
        if not hasattr(o, '_sta_offline_armed'):
            o._sta_offline_armed = False
        if not getattr(o, '_auto_key_upload_auth', None):
            o._auto_key_upload_auth = default_upload_auth_state()
        if not getattr(o, '_boss_raid_upload_auth', None):
            o._boss_raid_upload_auth = default_upload_auth_state()

    # ── owner lifecycle + settings hooks ──
    def _send_linked_key(self, key: str, press_mode: str = 'tap', hold_ms: int = 80, press_count: int = 1):
        try:
            from plugins.star_resonance_plugin.engines.auto_key_engine import (
                INPUT, INPUT_KEYBOARD, KEYBDINPUT, KEYEVENTF_KEYUP, VK_NAME_MAP,
            )
            import ctypes
            keyeventf_scancode = 0x0008
            key = (key or '').strip().upper()
            vk = VK_NAME_MAP.get(key)
            if vk is None and len(key) == 1 and key.isalpha():
                vk = ord(key)
            if vk is None:
                return
            scan = ctypes.windll.user32.MapVirtualKeyW(int(vk), 0)
            if not scan:
                return
            hold_s = max(0.015, hold_ms / 1000.0) if press_mode == 'hold' else 0.015
            extra = ctypes.c_ulong(0)
            for _ in range(max(1, press_count)):
                ki = KEYBDINPUT(wVk=0, wScan=int(scan), dwFlags=keyeventf_scancode,
                                time=0, dwExtraInfo=ctypes.pointer(extra))
                ev = INPUT(type=INPUT_KEYBOARD, ki=ki)
                ctypes.windll.user32.SendInput(1, ctypes.byref(ev), ctypes.sizeof(INPUT))
                time.sleep(hold_s)
                ki2 = KEYBDINPUT(wVk=0, wScan=int(scan),
                                 dwFlags=keyeventf_scancode | KEYEVENTF_KEYUP,
                                 time=0, dwExtraInfo=ctypes.pointer(extra))
                ev2 = INPUT(type=INPUT_KEYBOARD, ki=ki2)
                ctypes.windll.user32.SendInput(1, ctypes.byref(ev2), ctypes.sizeof(INPUT))
                if press_count > 1:
                    time.sleep(0.04)
        except Exception as exc:
            print(f'[Linkage] send_key error: {exc}')

    def _arm_pending_combat_reset(self, scene_event=None):
        o = self.owner
        reason = 'restart'
        delay_s = 3.0
        if isinstance(scene_event, dict):
            reason = str(scene_event.get('reason') or scene_event.get('kind') or reason)
            try:
                delay_s = float(scene_event.get('reset_delay_s', delay_s) or delay_s)
            except Exception:
                delay_s = 3.0
        o._pending_combat_reset_after = time.time() + max(0.0, delay_s)
        o._pending_combat_reset_reason = reason
        o._last_boss_bar_sig = None
        try:
            mgr = getattr(o, '_encounter_mgr', None)
            if mgr is not None:
                mgr.arm_pending_reset(reason, delay_s=delay_s)
        except Exception:
            pass
        try:
            if getattr(o, '_dps_tracker', None):
                o._dps_tracker.invalidate_snapshot_cache()
        except Exception:
            pass
        print(f'[SAO] 同副本重开候选({reason}) - 等下一次伤害再重置 boss bar/DPS', flush=True)

    def _maybe_apply_pending_combat_reset(self, event, is_self_combat_target: bool) -> bool:
        o = self.owner
        try:
            reset_after = float(getattr(o, '_pending_combat_reset_after', 0.0) or 0.0)
        except Exception:
            reset_after = 0.0
        if reset_after <= 0.0 or time.time() < reset_after or not is_self_combat_target:
            return False
        try:
            damage = int(event.get('damage') or 0)
        except Exception:
            damage = 0
        if bool(event.get('is_heal', False)):
            return False
        if damage <= 0 and not (event.get('is_immune') or event.get('is_absorbed')):
            return False
        reason = str(getattr(o, '_pending_combat_reset_reason', '') or 'restart')
        o._bb_last_target_uuid = 0
        o._bb_last_damage_ts = 0.0
        o._bb_recent_targets = {}
        o._pending_combat_reset_after = 0.0
        o._pending_combat_reset_reason = ''
        o._last_boss_bar_sig = None
        try:
            gs = getattr(o, '_game_state', None)
            if gs:
                gs.boss_breaking_stage = -1
                gs.boss_extinction_pct = 0.0
                gs.boss_current_hp = 0
                gs.boss_total_hp = 0
                gs.boss_hp_source = 'none'
                gs.boss_hp_est_pct = 1.0
                gs.boss_shield_active = False
                gs.boss_shield_pct = 0.0
                gs.boss_in_overdrive = False
                gs.boss_invincible = False
        except Exception:
            pass
        tracker = getattr(o, '_dps_tracker', None)
        if tracker:
            try:
                if tracker.has_active_encounter():
                    tracker.reset()
                else:
                    tracker.invalidate_snapshot_cache()
            except Exception:
                pass
        o._dps_visible = False
        o._dps_faded = False
        o._dps_mode = 'hidden'
        boss_engine = getattr(o, '_boss_raid_engine', None)
        if boss_engine:
            try:
                if getattr(boss_engine, '_state', '') != 'running':
                    boss_engine.reset()
            except Exception:
                pass
        print(f'[SAO] 下一次伤害到达，执行延迟重置: {reason}', flush=True)
        return True

    def _current_player_uid_int(self) -> int:
        o = self.owner
        for source in (getattr(o, '_game_state', None), getattr(getattr(o, '_state_mgr', None), 'state', None)):
            try:
                uid = getattr(source, 'player_id', '') if source is not None else ''
                if str(uid).isdigit():
                    return int(uid)
            except Exception:
                pass
        return 0

    def _normalize_damage_event_for_self(self, event):
        o = self.owner
        if not isinstance(event, dict):
            return event
        if event.get('attacker_is_self'):
            try:
                attacker_uid = int(event.get('attacker_uid') or 0)
            except Exception:
                attacker_uid = 0
            self_uid = self._current_player_uid_int()
            if attacker_uid or self_uid <= 0:
                return event
            fixed = dict(event)
            fixed['attacker_uid'] = self_uid
            fixed.setdefault('self_uid', self_uid)
            return fixed
        self_uid = self._current_player_uid_int()
        if self_uid <= 0:
            return event

        def _event_int(name: str) -> int:
            try:
                return int(event.get(name) or 0)
            except Exception:
                return 0

        candidate_uids = [_event_int('attacker_uid')]
        for key in ('attacker_uuid', 'attacker_uuid_raw', 'top_summoner_id'):
            raw = _event_int(key)
            if raw and (raw & 0xFFFF) == 640:
                candidate_uids.append(raw >> 16)
            elif key == 'top_summoner_id' and raw:
                candidate_uids.append(raw)
        if self_uid not in candidate_uids:
            return event
        fixed = dict(event)
        fixed['attacker_is_self'] = True
        fixed['attacker_uid'] = self_uid
        fixed.setdefault('self_uid', self_uid)
        now = time.time()
        if now - float(getattr(o, '_damage_self_fallback_log_ts', 0.0) or 0.0) > 10.0:
            o._damage_self_fallback_log_ts = now
            print(f'[SAO] 修正伤害归属: attacker_uid -> self_uid={self_uid}', flush=True)
        return fixed

    def _normalize_damage_event_target_for_webview(self, event):
        o = self.owner
        if not isinstance(event, dict):
            return event
        try:
            target_uuid = int(event.get('target_uuid') or 0)
        except Exception:
            target_uuid = 0
        if target_uuid <= 0:
            return event
        if event.get('target_is_combat_target') or event.get('target_is_monster'):
            return event
        target_suffix_player = (target_uuid & 0xFFFF) == 640
        if not target_suffix_player:
            fixed = dict(event)
            fixed['target_is_player'] = False
            fixed['target_is_monster'] = True
            fixed['target_is_combat_target'] = True
            fixed['webview_target_fallback'] = 'uuid_suffix_combat_target'
            return fixed
        if not event.get('attacker_is_self'):
            return event
        try:
            target_uid = target_uuid >> 16
        except Exception:
            target_uid = 0
        self_uid = self._current_player_uid_int()
        if target_uid and self_uid and target_uid == self_uid:
            return event
        known_player = False
        bridge = getattr(o, '_packet_engine', None)
        if bridge and target_uid:
            try:
                player = (bridge.get_players() or {}).get(target_uid)
                known_player = bool(
                    player and (
                        str(getattr(player, 'name', '') or '').strip()
                        or int(getattr(player, 'level', 0) or 0) > 0
                        or int(getattr(player, 'profession_id', 0) or 0) > 0
                        or int(getattr(player, 'fight_point', 0) or 0) > 0
                    )
                )
            except Exception:
                known_player = False
        if known_player:
            return event
        try:
            damage = int(event.get('damage') or 0)
        except Exception:
            damage = 0
        if damage <= 0 and not (event.get('is_immune') or event.get('is_absorbed')):
            return event
        fixed = dict(event)
        fixed['target_is_player'] = False
        fixed['target_is_monster'] = True
        fixed['target_is_combat_target'] = True
        fixed['webview_target_fallback'] = 'unknown_target_after_scene_change'
        return fixed

    def _on_packet_damage(self, event):
        o = self.owner
        event = self._normalize_damage_event_for_self(event)
        event = self._normalize_damage_event_target_for_webview(event)
        event = enrich_action_log_event(event, owner=o, topic='damage')
        publish_owner_event(o, 'damage', event, source_name='webview', source_kind='tcp')
        selective_decision = should_record_owner_combat_event(o, event)
        is_self_target = bool(
            event.get('attacker_is_self')
            and event.get('target_uuid', 0)
            and (
                event.get('target_is_combat_target', False)
                or event.get('target_is_monster', False)
                or ('target_is_player' in event and not event.get('target_is_player', False))
            )
        )
        self._maybe_apply_pending_combat_reset(event, is_self_target)
        if is_self_target:
            target_uuid = event.get('target_uuid', 0)
            if target_uuid:
                try:
                    bridge = getattr(o, '_packet_engine', None)
                    monster = bridge.get_monster(target_uuid) if bridge else None
                    if monster is not None and getattr(monster, 'is_dead', False):
                        monster.is_dead = False
                        monster.last_update = time.time()
                except Exception:
                    pass
                o._bb_recent_targets[target_uuid] = time.time()
                o._bb_last_target_uuid = target_uuid
                o._bb_last_damage_ts = time.time()
                if bool(selective_decision.get('record', True)) and getattr(o, '_dps_tracker', None):
                    try:
                        o._dps_tracker.set_boss_uuid(target_uuid)
                    except Exception:
                        pass
                try:
                    bridge = getattr(o, '_packet_engine', None)
                    monster = bridge.get_monster(target_uuid) if bridge else None
                    if monster and monster.max_hp == 0 and monster.hp > 0:
                        monster.max_hp = monster.hp
                        logger.info('[SR-WV] Estimated max_hp from HP on damage: %s uuid=%s', monster.hp, target_uuid)
                    if monster is not None and getattr(o, '_state_mgr', None):
                        updates = boss_state_from_monster_update(
                            monster.to_dict() if hasattr(monster, 'to_dict') else monster)
                        if updates:
                            o._state_mgr.update(**updates)
                except Exception:
                    pass
        boss_engine = getattr(o, '_boss_raid_engine', None)
        if boss_engine:
            try:
                boss_engine.on_damage_event(event)
            except Exception:
                pass
        if bool(selective_decision.get('record', True)):
            tracker = getattr(o, '_dps_tracker', None)
            if tracker:
                try:
                    tracker.on_damage_event(event)
                except Exception:
                    pass
            try:
                mgr = getattr(o, '_encounter_mgr', None)
                if mgr is not None:
                    mgr.on_damage_event(event)
            except Exception:
                pass
            self._push_dps_act_snapshot(throttle=True)

    def _on_monster_update(self, monster_data):
        o = self.owner
        try:
            tracker = getattr(o, '_dps_tracker', None)
            if tracker is not None:
                uuid = int(monster_data.get('uuid', 0) or 0)
                name = str(monster_data.get('monster_name') or monster_data.get('name') or '')
                if uuid and name:
                    tracker.update_monster_info(uuid, name)
        except Exception:
            pass
        publish_owner_event(o, 'monster', monster_data, source_name='webview', source_kind='tcp')
        boss_engine = getattr(o, '_boss_raid_engine', None)
        if boss_engine:
            try:
                boss_engine.on_monster_update(monster_data)
            except Exception:
                pass
        try:
            uuid = monster_data.get('uuid', 0)
            max_ext = int(monster_data.get('max_extinction', 0) or 0)
            max_hp = int(monster_data.get('max_hp', 0) or 0)
            hp = int(monster_data.get('hp', 0) or 0)
            is_dead = monster_data.get('is_dead', False)
            if uuid and (max_hp > 0 or hp > 0) and (not is_dead or hp > 0):
                should_adopt = False
                if not o._bb_last_target_uuid:
                    should_adopt = True
                else:
                    try:
                        bridge = getattr(o, '_packet_engine', None)
                        cur = bridge.get_monster(o._bb_last_target_uuid) if bridge else None
                        if not self._boss_monster_usable(cur):
                            should_adopt = True
                    except Exception:
                        pass
                if should_adopt:
                    o._bb_last_target_uuid = uuid
                    logger.debug('[SR-WV] Pre-tracked monster target uuid=%s max_hp=%s hp=%s max_ext=%s',
                                 uuid, max_hp, hp, max_ext)
                if uuid == o._bb_last_target_uuid and getattr(o, '_state_mgr', None):
                    updates = boss_state_from_monster_update(monster_data)
                    if updates:
                        o._state_mgr.update(**updates)
                        self._push_dps_act_snapshot(throttle=True)
        except Exception:
            pass

    def _on_boss_event(self, event):
        o = self.owner
        try:
            o._last_boss_event = dict(event or {})
            if getattr(o, '_state_mgr', None):
                o._state_mgr.update(last_boss_event=o._last_boss_event)
            self._push_dps_act_snapshot()
            publish_owner_event(o, 'boss', o._last_boss_event, source_name='webview', source_kind='tcp')
        except Exception:
            pass
        boss_engine = getattr(o, '_boss_raid_engine', None)
        if boss_engine:
            try:
                boss_engine.on_boss_event(event)
            except Exception:
                pass
        try:
            evt_type = event.get('event_type', 0)
            host_uuid = event.get('host_uuid', 0)
            target = o._bb_last_target_uuid or (getattr(boss_engine, '_boss_uuid', 0) if boss_engine else 0)
            if not target or host_uuid != target:
                return
            js_type = {58: 'enter_breaking', 47: 'shield_broken',
                       51: 'super_armor_broken', 88: 'into_fracture_state'}.get(evt_type)
            if js_type:
                self._eval_boss_hp(f'triggerBreakEffect("{js_type}")')
        except Exception:
            pass

    def _on_skill_event(self, event):
        o = self.owner
        try:
            o._last_skill_event = enrich_action_log_event(dict(event or {}), owner=o, topic='skill')
            if getattr(o, '_state_mgr', None):
                o._state_mgr.update(last_skill_event=o._last_skill_event)
            mgr = getattr(o, '_encounter_mgr', None)
            if mgr is not None:
                mgr.on_skill_event(o._last_skill_event)
            publish_owner_event(o, 'skill', o._last_skill_event, source_name='webview', source_kind='tcp')
        except Exception:
            pass

    def _on_dungeon_event(self, event):
        o = self.owner
        try:
            ev = dict(event or {})
            o._last_dungeon_event = ev
            updates = {'last_dungeon_event': ev}
            dungeon_id = int(ev.get('dungeon_id') or ev.get('scene_uuid') or 0)
            scene_id = int(ev.get('scene_id') or 0)
            difficulty = int(ev.get('dungeon_difficulty') or 0)
            if dungeon_id > 0:
                updates['dungeon_id'] = dungeon_id
                resolved = self.resolve_dungeon_name(dungeon_id)
                if resolved:
                    updates['dungeon_name'] = resolved
            if scene_id > 0:
                updates['dungeon_scene_id'] = scene_id
            if difficulty > 0:
                updates['dungeon_difficulty'] = difficulty
            banner_name = (updates.get('dungeon_name') or ev.get('dungeon_name') or '').strip()
            if not banner_name:
                lookup = dungeon_id or scene_id
                if lookup > 0:
                    banner_name = (self.resolve_dungeon_name(lookup) or '').strip()
            if banner_name:
                o._schedule_map_banner(banner_name)
            elif dungeon_id or scene_id:
                print(f'[MapBanner][webview] no-name event kind={ev.get("kind")!r} '
                      f'dungeon_id={dungeon_id} scene_id={scene_id}', flush=True)
            if getattr(o, '_state_mgr', None):
                o._state_mgr.update(**updates)
            mgr = getattr(o, '_encounter_mgr', None)
            if mgr is not None:
                ev_for_mgr = dict(ev)
                if 'dungeon_name' not in ev_for_mgr and updates.get('dungeon_name'):
                    ev_for_mgr['dungeon_name'] = updates.get('dungeon_name')
                mgr.on_dungeon_event(ev_for_mgr)
            ev = enrich_action_log_event(ev, owner=o, topic='dungeon')
            publish_owner_event(o, 'dungeon', ev, source_name='webview', source_kind='tcp')
            if scene_id > 0:
                publish_owner_event(o, 'scene', ev, source_name='webview', source_kind='tcp')
        except Exception:
            pass

    def _on_dps_report_finalized(self, report):
        o = self.owner
        try:
            store = getattr(o, '_dps_history_store', None)
            if store is None:
                return
            threading.Thread(target=lambda: store.add_report(report), daemon=True).start()
        except Exception:
            pass

    def _on_scene_change(self, scene_event=None):
        o = self.owner
        scene_kind = ''
        scene_reason = ''
        preserve_combat = False
        reset_on_next_damage = False
        if isinstance(scene_event, dict):
            scene_kind = str(scene_event.get('kind') or '')
            scene_reason = str(scene_event.get('reason') or '')
            preserve_combat = bool(scene_event.get('preserve_combat', False))
            reset_on_next_damage = bool(scene_event.get('reset_on_next_damage', False))
        if reset_on_next_damage:
            combat_live = False
            try:
                if getattr(o, '_dps_tracker', None) is not None:
                    combat_live = bool(o._dps_tracker.has_recent_damage(o._combat_damage_timeout_s()))
            except Exception:
                combat_live = False
            if combat_live:
                self._arm_pending_combat_reset(scene_event)
                return
            preserve_combat = False
        if preserve_combat:
            print(f'[SAO] 同副本软切换({scene_kind or "transition"}/{scene_reason}) - 保留 DPS, 重置 boss bar 候选',
                  flush=True)
            o._bb_last_target_uuid = 0
            o._bb_last_damage_ts = 0.0
            o._bb_recent_targets = {}
            o._last_boss_bar_sig = None
            self._eval_boss_hp('updateBossBar({active:false})')
            try:
                if o._dps_tracker:
                    o._dps_tracker.invalidate_snapshot_cache()
            except Exception:
                pass
            return
        print('[SAO] 场景切换 - 重置 boss bar 和 DPS 追踪', flush=True)
        o._bb_last_target_uuid = 0
        o._bb_last_damage_ts = 0.0
        o._bb_recent_targets = {}
        o._last_boss_bar_sig = None
        o._pending_combat_reset_after = 0.0
        o._pending_combat_reset_reason = ''
        self._eval_boss_hp('updateBossBar({active:false})')
        try:
            gs = getattr(o, '_game_state', None)
            if gs:
                gs.boss_breaking_stage = -1
                gs.boss_extinction_pct = 0.0
                gs.boss_current_hp = 0
                gs.boss_total_hp = 0
                gs.boss_hp_source = 'none'
                gs.boss_hp_est_pct = 1.0
                gs.boss_shield_active = False
                gs.boss_shield_pct = 0.0
                gs.boss_in_overdrive = False
                gs.boss_invincible = False
        except Exception:
            pass

    def _clear_skillfx_state_for_death(self):
        o = self.owner
        o._last_skillfx_sig = None
        self._reset_burst_tracking()
        try:
            o._state_mgr.update(burst_ready=False, skill_slots=[])
        except Exception:
            pass
        payload = {
            'slots': [],
            'watched_slots': o._get_setting('watched_skill_slots', [1, 2, 3, 4, 5, 6, 7, 8, 9]),
            'burst_enabled': bool(o._get_setting('burst_enabled', True)),
            'burst_slot': 0,
            'burst_ready': False,
            'enabled': bool(o._get_setting('burst_enabled', True)),
        }
        layout = self._get_skillfx_layout(getattr(o, '_game_state', None))
        if layout:
            o._skillfx_layout = layout
            payload['viewport'] = o._viewport_to_css(layout['viewport'])
        self._eval_skillfx(f'SkillFX.update({json.dumps(payload, ensure_ascii=False)})')
        self._eval_skillfx('SkillFX.hideBurstReady()')
        o._last_burst_ready = False

    def _is_dead_state(self, gs) -> bool:
        if gs is None:
            return False
        try:
            hp_max = int(getattr(gs, 'hp_max', 0) or 0)
            hp_current = int(getattr(gs, 'hp_current', 0) or 0)
            hp_pct = float(getattr(gs, 'hp_pct', 1.0) or 0.0)
        except Exception:
            return False
        return hp_max > 0 and hp_current <= 0 and hp_pct <= 0.001

    def _pause_vision_for_death(self):
        o = self.owner
        engine = getattr(o, '_vision_engine', None)
        if engine is None or getattr(o, '_vision_paused_for_death', False):
            return
        try:
            engine.stop()
        except Exception:
            pass
        o._vision_engine = None
        o._recognition_engines = [
            item for item in (getattr(o, '_recognition_engines', []) or [])
            if item is not engine
        ]
        o._vision_paused_for_death = True
        self._clear_skillfx_state_for_death()
        print('[SAO] Vision engine paused (death)')

    def _resume_vision_after_revive(self):
        o = self.owner
        if not getattr(o, '_vision_paused_for_death', False):
            return
        if getattr(o, '_vision_engine', None) is not None:
            o._vision_paused_for_death = False
            self._bump_boss_hp_target_hold('revive')
            return
        if not getattr(o, '_cfg_settings_ref', None) or not getattr(o, '_state_mgr', None):
            return
        try:
            self._reconfigure_data_engines(False)
            o._vision_paused_for_death = False
            self._bump_boss_hp_target_hold('revive')
            print('[SAO] Vision engine resumed (revive)')
        except Exception as exc:
            print(f'[SAO] Vision engine resume failed: {exc}')

    def sync_vision_lifecycle(self, gs):
        o = self.owner
        dead_now = self._is_dead_state(gs)
        dead_prev = bool(getattr(o, '_last_dead_state', False))
        if dead_now and not dead_prev:
            self._pause_vision_for_death()
        elif (not dead_now) and dead_prev:
            self._resume_vision_after_revive()
            self._bump_boss_hp_target_hold('revive')
        o._last_dead_state = dead_now

    def _bump_boss_hp_target_hold(self, reason: str = ''):
        o = self.owner
        target_uuid = int(getattr(o, '_bb_last_target_uuid', 0) or 0)
        if not target_uuid:
            return
        bridge = getattr(o, '_packet_engine', None)
        if not bridge:
            return
        try:
            monster = bridge.get_monster(target_uuid)
            if not monster or not self._boss_monster_usable(monster):
                return
            now = time.time()
            o._bb_recent_targets[target_uuid] = now
            o._bb_last_damage_ts = now
            o._last_boss_bar_sig = None
        except Exception:
            pass

    def _boss_monster_usable(self, monster) -> bool:
        if not monster:
            return False
        try:
            hp = int(getattr(monster, 'hp', 0) or 0)
            max_hp = int(getattr(monster, 'max_hp', 0) or 0)
            is_dead = bool(getattr(monster, 'is_dead', False))
            if is_dead and hp > 0:
                monster.is_dead = False
                monster.last_update = time.time()
                is_dead = False
            return (not is_dead) and (max_hp > 0 or hp > 0)
        except Exception:
            return False

    def _pick_burst_trigger_slot(self, gs):
        o = self.owner
        watched = o._get_setting('watched_skill_slots', [1, 2, 3, 4, 5, 6, 7, 8, 9]) or []
        slots = getattr(gs, 'skill_slots', []) or []
        prev_slot = int(getattr(o, '_last_burst_slot', 0) or 0)
        if _CY_UI is not None:
            chosen = int(_CY_UI.pick_burst_trigger_slot(slots, watched, prev_slot))
        else:
            chosen = int(watched[0]) if watched else 1
        o._last_burst_slot = chosen
        return chosen

    def _save_game_cache(self, quiet: bool = False):
        o = self.owner
        try:
            if getattr(o, '_state_mgr', None) and getattr(o, '_cfg_settings_ref', None):
                self.persist_cached_identity_state(save_now=False)
                o._state_mgr.save_cache(o._cfg_settings_ref)
                if not quiet:
                    gs = o._state_mgr.state
                    print(
                        f"[SAO] 退出前已保存游戏状态缓存: "
                        f"HP={int(getattr(gs, 'hp_current', 0) or 0)}/"
                        f"{int(getattr(gs, 'hp_max', 0) or 0)}, "
                        f"LV={int(getattr(gs, 'level_base', 0) or 0)}"
                    )
        except Exception:
            pass

    def _reset_sta_offline_state(self):
        o = self.owner
        o._sta_offline_armed = False
        try:
            o._eval_hp('setSTAOffline(false)')
        except Exception:
            pass

    def on_recognition_changed(self, active: bool = False):
        if not bool(active):
            self._reset_sta_offline_state()

    def build_default_display_regions(self, win_w: int, win_h: int, viewport_h: int):
        stage_width = int(win_w * 0.75)

        def _rect(left, top, width, height):
            return {
                'left': int(left),
                'top': int(top),
                'width': int(max(0, width)),
                'height': int(max(0, height)),
            }

        id_w = int(stage_width * 0.42)
        id_h = 136
        id_left = int(stage_width * 0.01)
        id_top = int(viewport_h) - 146

        hp_w = int(stage_width * 0.34)
        hp_h = 118
        hp_left = int(stage_width * 0.48)
        hp_top = int(viewport_h) - 112

        stamina_w = int(stage_width * 0.30)
        stamina_h = 38
        stamina_left = int(stage_width * 0.53)
        stamina_top = int(viewport_h) - 42

        return [
            _rect(id_left, id_top, id_w, id_h),
            _rect(hp_left - 44, hp_top - 18, hp_w + 88, hp_h + 36),
            _rect(hp_left, hp_top, hp_w, hp_h),
            _rect(stamina_left, stamina_top, stamina_w, stamina_h),
        ]

    def _should_show_sta_offline(self, gs) -> bool:
        if gs is None:
            return False
        try:
            err = str(getattr(gs, 'error_msg', '') or '')
            if 'vision capture failed' in err:
                return False
        except Exception:
            pass
        return bool(getattr(gs, 'stamina_offline', False))
        if getattr(o, '_dps_tracker', None):
            try:
                o._dps_tracker.reset()
                print('[SAO] DPS tracker reset on scene change', flush=True)
            except Exception:
                pass
        try:
            mgr = getattr(o, '_encounter_mgr', None)
            if mgr is not None:
                mgr.reset('scene_change')
        except Exception:
            pass
        boss_engine = getattr(o, '_boss_raid_engine', None)
        if boss_engine:
            try:
                if getattr(boss_engine, '_state', '') != 'running':
                    boss_engine.reset()
            except Exception:
                pass
        try:
            gs = getattr(o, '_game_state', None)
            if gs:
                level = getattr(gs, 'level_base', 0) or getattr(o, '_level', 0)
                extra = int(getattr(gs, 'level_extra', 0) or 0)
                level_text = f'{level}(+{extra})' if extra > 0 else str(level)
                hp = int(getattr(gs, 'hp_current', 0) or 0)
                hp_max = int(getattr(gs, 'hp_max', 0) or 0)
                if hp_max > 0:
                    o._eval_hp(f'updateHP({hp}, {hp_max}, "{level_text}")')
        except Exception:
            pass

    def _auto_key_settings_ref(self):
        return self.owner._cfg_settings_ref or self.owner.settings

    def _auto_key_author_snapshot(self):
        gs = getattr(self.owner, '_game_state', None)
        if gs is not None:
            return snapshot_author_from_state(gs)
        return {
            'player_uid': '',
            'player_name': getattr(self.owner, '_username', 'Player'),
            'profession_id': 0,
            'profession_name': getattr(self.owner, '_profession', ''),
        }

    def _auto_key_identity_state(self):
        source = 'packet' if getattr(self.owner, '_game_state', None) is not None else 'profile'
        return build_identity_state(self._auto_key_author_snapshot(), source=source)

    def _set_auto_key_upload_auth(self, token: str = '', expires_at: str = '', error: str = '',
                                  mode: str = '', identity: Optional[dict] = None,
                                  server_url: str = ''):
        identity_state = identity or self._auto_key_identity_state()
        self.owner._auto_key_upload_auth = {
            'token': str(token or '').strip(),
            'ready': bool(str(token or '').strip()),
            'token_masked': '',
            'expires_at': str(expires_at or '').strip(),
            'error': str(error or '').strip(),
            'mode': str(mode or '').strip(),
            'identity': identity_state,
            'server_url': str(server_url or '').strip(),
        }
        return self.owner._auto_key_upload_auth

    def _auto_key_upload_auth_matches(self, identity_state, config):
        auth = getattr(self.owner, '_auto_key_upload_auth', None) or {}
        cached_identity = auth.get('identity') or {}
        if str(auth.get('server_url') or '') != str((config or {}).get('server_url') or ''):
            return False
        return (
            str(cached_identity.get('player_uid') or '') == str(identity_state.get('player_uid') or '') and
            str(cached_identity.get('player_name') or '') == str(identity_state.get('player_name') or '') and
            int(cached_identity.get('profession_id') or 0) == int(identity_state.get('profession_id') or 0)
        )

    def _auto_key_upload_auth_valid(self, identity_state=None, config=None):
        auth = getattr(self.owner, '_auto_key_upload_auth', None) or {}
        if not auth.get('ready') or not str(auth.get('token') or '').strip():
            return False
        identity_state = identity_state or self._auto_key_identity_state()
        config = config or self._load_auto_key_config()
        if not self._auto_key_upload_auth_matches(identity_state, config):
            return False
        expires_at = str(auth.get('expires_at') or '').strip()
        if expires_at and expires_at <= time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(time.time() + 5)):
            return False
        return True

    def _auto_key_upload_auth_state(self, config=None, identity_state=None):
        config = config or self._load_auto_key_config()
        identity_state = identity_state or self._auto_key_identity_state()
        auth = getattr(self.owner, '_auto_key_upload_auth', None) or default_upload_auth_state()
        if self._auto_key_upload_auth_valid(identity_state, config):
            return auth
        return self._set_auto_key_upload_auth(
            token='', expires_at='', error=str(auth.get('error') or ''), mode=str(auth.get('mode') or ''),
            identity=identity_state, server_url=str(config.get('server_url') or DEFAULT_AUTO_KEY_SERVER_URL))

    def _refresh_auto_key_upload_auth(self, force: bool = False):
        config = self._load_auto_key_config()
        identity_state = self._auto_key_identity_state()
        if not force and self._auto_key_upload_auth_valid(identity_state, config):
            return getattr(self.owner, '_auto_key_upload_auth', None) or default_upload_auth_state()
        server_url = str(config.get('server_url') or DEFAULT_AUTO_KEY_SERVER_URL)
        if (identity_state.get('missing') or []):
            return self._set_auto_key_upload_auth(
                token='', error='Upload identity is incomplete: ' + ', '.join(identity_state.get('missing') or []),
                mode='auto', identity=identity_state, server_url=server_url)
        try:
            client = AutoKeyCloudClient(server_url)
            result = client.issue_upload_token({'identity': identity_state})
            if result.get('error'):
                raise RuntimeError(str(result.get('error')))
            return self._set_auto_key_upload_auth(
                token=str(result.get('token') or ''),
                expires_at=str(result.get('expires_at') or ''),
                error='', mode='auto',
                identity=build_identity_state(result.get('identity') or identity_state,
                                              source=identity_state.get('source') or 'packet'),
                server_url=server_url)
        except Exception as exc:
            return self._set_auto_key_upload_auth(
                token='', error=str(exc), mode='auto', identity=identity_state, server_url=server_url)

    def _load_auto_key_config(self):
        return load_auto_key_config(self._auto_key_settings_ref(), state_snapshot=self._auto_key_author_snapshot())

    def _save_auto_key_config(self, config):
        saved = save_auto_key_config(self._auto_key_settings_ref(), config)
        engine = getattr(self.owner, '_auto_key_engine', None)
        if engine is not None and hasattr(engine, 'reload_config'):
            engine.reload_config(saved)
        return saved

    def _get_auto_key_menu_state(self):
        config = self._load_auto_key_config()
        engine = getattr(self.owner, '_auto_key_engine', None)
        status = engine.get_status() if engine is not None and hasattr(engine, 'get_status') else {}
        burst = engine.get_burst_actions() if engine is not None and hasattr(engine, 'get_burst_actions') else []
        return build_auto_key_state(config, status, self._auto_key_upload_auth_state(config), burst)

    def _sync_auto_key_menu(self):
        state = self._get_auto_key_menu_state()
        if state == getattr(self.owner, '_auto_key_last_menu_state', None):
            return
        self.owner._auto_key_last_menu_state = state
        getattr(self.owner, '_eval_menu', lambda _: None)('if(window.SRMenu&&SRMenu.setAutoKeyState)SRMenu.setAutoKeyState(%s)' % json.dumps(state, ensure_ascii=False))

    def _boss_raid_settings_ref(self):
        return self.owner._cfg_settings_ref or self.owner.settings

    def _boss_raid_author_snapshot(self):
        gs = getattr(self.owner, '_game_state', None)
        if gs is not None:
            return snapshot_author_from_state(gs)
        return {
            'player_uid': '',
            'player_name': getattr(self.owner, '_username', 'Player'),
            'profession_id': 0,
            'profession_name': getattr(self.owner, '_profession', ''),
        }

    def _boss_raid_identity_state(self):
        source = 'packet' if getattr(self.owner, '_game_state', None) is not None else 'profile'
        return build_identity_state(self._boss_raid_author_snapshot(), source=source)

    def _set_boss_raid_upload_auth(self, token: str = '', expires_at: str = '', error: str = '',
                                   mode: str = '', identity: Optional[dict] = None,
                                   server_url: str = ''):
        identity_state = identity or self._boss_raid_identity_state()
        self.owner._boss_raid_upload_auth = {
            'token': str(token or '').strip(),
            'ready': bool(str(token or '').strip()),
            'token_masked': '',
            'expires_at': str(expires_at or '').strip(),
            'error': str(error or '').strip(),
            'mode': str(mode or '').strip(),
            'identity': identity_state,
            'server_url': str(server_url or '').strip(),
        }
        return self.owner._boss_raid_upload_auth

    def _boss_raid_upload_auth_matches(self, identity_state, config):
        auth = getattr(self.owner, '_boss_raid_upload_auth', None) or {}
        cached_identity = auth.get('identity') or {}
        if str(auth.get('server_url') or '') != str((config or {}).get('server_url') or ''):
            return False
        return (
            str(cached_identity.get('player_uid') or '') == str(identity_state.get('player_uid') or '') and
            str(cached_identity.get('player_name') or '') == str(identity_state.get('player_name') or '')
        )

    def _boss_raid_upload_auth_valid(self, identity_state=None, config=None):
        auth = getattr(self.owner, '_boss_raid_upload_auth', None) or {}
        if not auth.get('ready') or not str(auth.get('token') or '').strip():
            return False
        identity_state = identity_state or self._boss_raid_identity_state()
        config = config or self._load_boss_raid_config()
        if not self._boss_raid_upload_auth_matches(identity_state, config):
            return False
        expires_at = str(auth.get('expires_at') or '').strip()
        if expires_at and expires_at <= time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(time.time() + 5)):
            return False
        return True

    def _boss_raid_upload_auth_state(self, config=None, identity_state=None):
        config = config or self._load_boss_raid_config()
        identity_state = identity_state or self._boss_raid_identity_state()
        auth = getattr(self.owner, '_boss_raid_upload_auth', None) or default_upload_auth_state()
        if self._boss_raid_upload_auth_valid(identity_state, config):
            return auth
        return self._set_boss_raid_upload_auth(
            token='', expires_at='', error=str(auth.get('error') or ''), mode=str(auth.get('mode') or ''),
            identity=identity_state, server_url=str(config.get('server_url') or DEFAULT_BOSS_RAID_SERVER_URL))

    def _load_boss_raid_config(self):
        return load_boss_raid_config(self._boss_raid_settings_ref(), state_snapshot=self._boss_raid_author_snapshot())

    def _save_boss_raid_config(self, config):
        return save_boss_raid_config(self._boss_raid_settings_ref(), config)

    def _get_boss_raid_menu_state(self):
        config = self._load_boss_raid_config()
        engine = getattr(self.owner, '_boss_raid_engine', None)
        status = engine.get_status() if engine is not None and hasattr(engine, 'get_status') else {}
        return build_boss_raid_state(config, status, self._boss_raid_upload_auth_state(config))

    def _sync_boss_raid_menu(self):
        state = self._get_boss_raid_menu_state()
        if state == getattr(self.owner, '_boss_raid_last_menu_state', None):
            return
        self.owner._boss_raid_last_menu_state = state
        getattr(self.owner, '_eval_menu', lambda _: None)('if(window.SRMenu&&SRMenu.setBossRaidState)SRMenu.setBossRaidState(%s)' % json.dumps(state, ensure_ascii=False))

    def _refresh_boss_raid_upload_auth(self, force: bool = False):
        config = self._load_boss_raid_config()
        identity_state = self._boss_raid_identity_state()
        if not force and self._boss_raid_upload_auth_valid(identity_state, config):
            return getattr(self.owner, '_boss_raid_upload_auth', None) or default_upload_auth_state()
        server_url = str(config.get('server_url') or DEFAULT_BOSS_RAID_SERVER_URL)
        if identity_state.get('missing'):
            return self._set_boss_raid_upload_auth(
                token='', error='Upload identity is incomplete: ' + ', '.join(identity_state.get('missing') or []),
                mode='auto', identity=identity_state, server_url=server_url)
        try:
            client = BossRaidCloudClient(server_url)
            result = client.issue_upload_token({'identity': identity_state})
            if result.get('error'):
                raise RuntimeError(str(result.get('error')))
            return self._set_boss_raid_upload_auth(
                token=str(result.get('token') or ''), expires_at=str(result.get('expires_at') or ''),
                error='', mode='auto', identity=identity_state, server_url=server_url)
        except Exception as exc:
            return self._set_boss_raid_upload_auth(
                token='', error=str(exc), mode='auto', identity=identity_state, server_url=server_url)

    def _toggle_boss_raid(self):
        engine = getattr(self.owner, '_boss_raid_engine', None)
        if engine is None:
            return
        if getattr(engine, 'running', False):
            engine.stop()
            getattr(self.owner, '_eval_menu', lambda _: None)('SAO.showToast("BOSS RAID: STOP")')
        else:
            profile = br_active_profile(self._load_boss_raid_config())
            if profile:
                engine.start(profile)
                getattr(self.owner, '_eval_menu', lambda _: None)('SAO.showToast("BOSS RAID: START")')
        self._sync_boss_raid_menu()

    def _boss_raid_next_phase(self):
        engine = getattr(self.owner, '_boss_raid_engine', None)
        if engine is not None and hasattr(engine, 'next_phase'):
            engine.next_phase()
        self._sync_boss_raid_menu()

    def _toggle_auto_script(self):
        engine = getattr(self.owner, '_auto_key_engine', None)
        if engine is not None and hasattr(engine, 'toggle'):
            engine.toggle()
        self._sync_auto_key_menu()

    def _reset_burst_tracking(self):
        self.owner._last_ready_slots = {}
        self.owner._burst_seen_cooling = set()
        self.owner._last_burst_ready = False
        self.owner._last_burst_slot = 0

    def _start_packet_engine_early(self):
        from act_platform.runtime import ensure_act_plugin_manager
        ensure_act_plugin_manager(self.owner, load=True)

    def _reconfigure_data_engines(self, restart_packet: bool = True):
        from act_platform.runtime import ensure_act_event_bus, ensure_act_plugin_manager
        if not getattr(self.owner, '_cfg_settings_ref', None) and not getattr(self.owner, 'settings', None):
            return
        if not getattr(self.owner, '_state_mgr', None):
            return
        if hasattr(self.owner, '_stop_recognition_engines'):
            try:
                self.owner._stop_recognition_engines(preserve_packet=not restart_packet)
            except TypeError:
                self.owner._stop_recognition_engines()
        else:
            for eng in list(getattr(self.owner, '_recognition_engines', [])):
                try:
                    eng.stop()
                except Exception:
                    pass
            self.owner._recognition_active = False
        try:
            self.owner._state_mgr.update(burst_ready=False)
        except Exception:
            pass
        self._reset_burst_tracking()
        engines = list(getattr(self.owner, '_recognition_engines', []) or [])
        if restart_packet or getattr(self.owner, '_packet_engine', None) is None:
            try:
                from plugins.star_resonance_plugin.net.packet_bridge import PacketBridge
                mode = str(self.owner._cfg_settings_ref.get('mem_data_source', 'tcp') or 'tcp').lower()
                packet_engine = PacketBridge(
                    self.owner._state_mgr,
                    self.owner._cfg_settings_ref,
                    on_damage=getattr(self.owner, '_on_packet_damage', None),
                    on_monster_update=getattr(self.owner, '_on_monster_update', None),
                    on_boss_event=getattr(self.owner, '_on_boss_event', None),
                    on_skill_event=getattr(self.owner, '_on_skill_event', None),
                    on_dungeon_event=getattr(self.owner, '_on_dungeon_event', None),
                    on_scene_change=getattr(self.owner, '_on_scene_change', None),
                    data_source=mode,
                    plugin_manager=ensure_act_plugin_manager(self.owner, load=True),
                    event_bus=ensure_act_event_bus(self.owner),
                )
                packet_engine.start()
                self.owner._packet_engine = packet_engine
                engines = [engine for engine in engines if engine is not packet_engine]
                engines.insert(0, packet_engine)
                print(f'[SAO] Packet bridge started (data_source={mode!r})')
            except Exception as exc:
                import traceback
                print(f'[SAO] Packet bridge FAILED to start: {exc}', flush=True)
                traceback.print_exc()
                self.owner._packet_engine = None
        elif self.owner._packet_engine not in engines:
            engines.insert(0, self.owner._packet_engine)
        ensure_act_event_bus(self.owner)
        ensure_act_plugin_manager(self.owner, load=True)
        try:
            from plugins.star_resonance_plugin.vision.recognition import RecognitionEngine
            vision_engine = RecognitionEngine(self.owner._state_mgr, self.owner._cfg_settings_ref)
            vision_engine.start()
            engines.append(vision_engine)
            self.owner._vision_engine = vision_engine
            self.owner._vision_paused_for_death = False
            self.owner._last_dead_state = False
            print('[SAO] Recognition engine started (window vision / printwindow)')
        except Exception as exc:
            import traceback
            print(f'[SAO] Recognition engine FAILED to start: {exc}', flush=True)
            traceback.print_exc()
            self.owner._vision_engine = None
        self.owner._recognition_engines = engines
        self.owner._recognition_engine = getattr(self.owner, '_packet_engine', None) or (engines[0] if engines else None)
        self.owner._recognition_active = bool(getattr(self.owner, '_packet_engine', None) or getattr(self.owner, '_vision_engine', None))

    def _start_recognition(self):
        try:
            _call = lambda o, m, *a, **kw: getattr(o, m)(*a, **kw) if hasattr(o, m) else None
            _call(self.owner, '_bootstrap_runtime_state')
            _call(self.owner, '_update_skillfx_layout')
            try:
                from utils.sao_sound import set_sound_enabled, set_sound_volume
                cfg = getattr(self.owner, '_cfg_settings_ref', None) or getattr(self.owner, 'settings', None)
                if cfg:
                    set_sound_enabled(bool(cfg.get('sound_enabled', True)))
                    set_sound_volume(int(cfg.get('sound_volume', 70) or 70))
            except Exception:
                pass
            self._reconfigure_data_engines(restart_packet=not bool(getattr(self.owner, '_packet_engine', None)))
            for fn in (self._sync_auto_key_menu, self._sync_boss_raid_menu):
                try:
                    fn()
                except Exception:
                    pass
            stop_evt = getattr(self.owner, '_cache_loop_stop', None)
            if stop_evt is None:
                import threading as _thr
                stop_evt = _thr.Event()
                self.owner._cache_loop_stop = stop_evt
            def _cache_loop():
                while not stop_evt.is_set():
                    stop_evt.wait(30)
                    if stop_evt.is_set():
                        break
                    try:
                        _call(self.owner, '_persist_cached_identity_state', save_now=False)
                        sm = getattr(self.owner, '_state_mgr', None)
                        cfg = getattr(self.owner, '_cfg_settings_ref', None) or getattr(self.owner, 'settings', None)
                        if sm and cfg:
                            sm.save_cache(cfg)
                    except Exception:
                        pass
            threading.Thread(target=_cache_loop, daemon=True, name='cache_saver').start()
        except Exception as exc:
            print(f'[SAO] Data engine failed: {exc}')
            import traceback
            traceback.print_exc()
            self.owner._recognition_active = False

    def _presynthesize_active_profile_web(self):
        controller = getattr(self.owner, '_mech_alert_controller', None)
        if controller is None:
            return
        try:
            profile = br_active_profile(load_boss_raid_config(self.owner._cfg_settings_ref))
            if profile:
                controller.presynthesize_profile(profile)
        except Exception:
            pass

    def _toggle_auto_dodge(self):
        try:
            cfg = load_linkage_config(self.owner._cfg_settings_ref)
            new_state = not bool(cfg.get('dodge_enabled', False))
            set_linkage_dodge_enabled(self.owner._cfg_settings_ref, new_state)
            director = getattr(self.owner, '_auto_dodge_director', None)
            if director is not None:
                try:
                    director.release_all()
                except Exception:
                    pass
            linkage = getattr(self.owner, '_boss_autokey_linkage', None)
            if linkage is not None:
                try:
                    linkage.panic_stop()
                except Exception:
                    pass
            self.owner._show_identity_alert_window('自动躲避', '已启用 (F12 紧急停用)' if new_state else '已禁用 (F12 重新启用)')
            try:
                self.owner._eval_raid_editor('if(window.RaidEditor&&RaidEditor.loadMechanics)RaidEditor.loadMechanics()')
            except Exception:
                pass
        except Exception as exc:
            print(f'[SAO] toggle_auto_dodge failed: {exc}')

    # ── small plugin services used by platform render paths ──
    def mem_boss_break_override(self, bridge):
        return mem_boss_break_override(bridge)

    def get_break_recovery_time(self, template_id: int) -> float:
        try:
            from plugins.star_resonance_plugin.engines.break_time_lookup import get_break_recovery_time
            return get_break_recovery_time(int(template_id or 0))
        except Exception:
            return 0.0

    def save_character_profile(self, **kwargs):
        from plugins.star_resonance_plugin.engines.character_profile import save_profile
        return save_profile(**kwargs)

    def persist_cached_identity_state(self, save_now: bool = False):
        settings = getattr(self.owner, '_cfg_settings_ref', None)
        if not settings:
            return
        cache = dict(settings.get('game_cache', {}) or {})
        gs = getattr(self.owner, '_game_state', None)
        gs_name = str(getattr(gs, 'player_name', '') or '').strip() if gs is not None else ''
        gs_prof = str(getattr(gs, 'profession_name', '') or '').strip() if gs is not None else ''
        gs_level = int(getattr(gs, 'level_base', 0) or 0) if gs is not None else 0
        name = gs_name or str(getattr(self.owner, '_username', '') or '').strip()
        profession = gs_prof or str(getattr(self.owner, '_profession', '') or '').strip()
        level_base = gs_level if gs_level > 0 else int(getattr(self.owner, '_level', 0) or 0)
        if name and name.lower() != 'player':
            cache['player_name'] = name
        if profession:
            cache['profession_name'] = profession
        if level_base > 0:
            cache['level_base'] = level_base
        level_extra = int(getattr(gs, 'level_extra', 0) or 0) if gs is not None else 0
        season_exp = int(getattr(gs, 'season_exp', 0) or 0) if gs is not None else 0
        uid = str(getattr(gs, 'player_id', '') or '').strip() if gs is not None else ''
        if level_extra > 0:
            cache['level_extra'] = level_extra
        if season_exp > 0:
            cache['season_exp'] = season_exp
        if uid:
            cache['player_id'] = uid
        fight_point = int(getattr(gs, 'fight_point', 0) or 0) if gs is not None else 0
        if fight_point > 0:
            cache['fight_point'] = fight_point
        if gs is not None:
            for field in ('hp_current', 'hp_max', 'hp_pct',
                          'stamina_current', 'stamina_max', 'stamina_pct',
                          'profession_id'):
                value = getattr(gs, field, None)
                if value is not None and (isinstance(value, (int, float)) and value > 0 or
                                          (isinstance(value, str) and value)):
                    cache[field] = value
        settings.set('game_cache', cache)
        if save_now:
            try:
                settings.save()
            except Exception:
                pass

    def sync_identity_alert(self, gs):
        if gs is None:
            return
        try:
            alert_serial = int(getattr(gs, 'identity_alert_serial', 0) or 0)
        except Exception:
            alert_serial = 0
        alert_title = str(getattr(gs, 'identity_alert_title', '') or '')
        alert_message = str(getattr(gs, 'identity_alert_message', '') or '')
        if alert_serial > 0 and alert_serial != getattr(self.owner, '_last_identity_alert_serial', 0):
            self.owner._last_identity_alert_serial = alert_serial
            if not self.owner._is_persistent_alert_active():
                self.owner._show_identity_alert_window(alert_title, alert_message, 9000, alert_kind='identity')
            return
        if self.owner._is_persistent_alert_active():
            return
        has_identity = bool(
            str(getattr(gs, 'player_name', '') or '').strip()
            and int(getattr(gs, 'level_base', 0) or 0) > 0
        )
        if (has_identity
            and getattr(self.owner, '_identity_alert_visible', False)
            and str(getattr(self.owner, '_identity_alert_kind', '') or '') == 'identity'):
            self.owner._hide_identity_alert_window()

    def sync_identity_panel(self, gs):
        if gs is None:
            gs = getattr(getattr(self.owner, '_state_mgr', None), 'state', None)
        if gs is None:
            return
        if getattr(gs, 'player_name', '') and gs.player_name != getattr(self.owner, '_last_gs_name', ''):
            self.owner._last_gs_name = gs.player_name
            self.owner._username = gs.player_name
            self.owner._eval_hp(f'setUsername("{self.owner._safe_js(gs.player_name)}")')
        profession = getattr(gs, 'profession_name', '') or ''
        uid = getattr(gs, 'player_id', '') or ''
        if (profession and profession != getattr(self.owner, '_last_gs_prof', '')) or \
           (uid and uid != getattr(self.owner, '_last_gs_uid', '')):
            self.owner._last_gs_prof = profession
            self.owner._last_gs_uid = uid
            info = {}
            if profession:
                info['profession'] = profession
            if uid:
                info['uid'] = uid
            self.owner._eval_hp(f'setPlayerInfo({json.dumps(info, ensure_ascii=False)})')
        if not getattr(self.owner, '_profile_auto_saved', False) and getattr(gs, 'player_name', ''):
            self.owner._profile_auto_saved = True
            try:
                level = getattr(gs, 'level_base', 0) if getattr(gs, 'level_base', 0) > 0 else 1
                self.save_character_profile(
                    username=gs.player_name,
                    profession=getattr(gs, 'profession_name', '') or '',
                    level=level,
                    uid=getattr(gs, 'player_id', '') or '',
                )
                print(f'[SR-WV] 自动保存角色: {gs.player_name}, '
                      f'职业={getattr(gs, "profession_name", "")}, LV={level}, UID={getattr(gs, "player_id", "")}')
            except Exception:
                pass

    def build_menu_info(self):
        gs = getattr(self.owner, '_game_state', None)
        gs_desc = 'VISION ACTIVE' if getattr(self.owner, '_recognition_active', False) else 'SAO Auto Idle'
        hp_str = '--'
        sta_str = '--'
        if gs and hasattr(gs, 'hp_current'):
            if getattr(gs, 'hp_max', 0) > 0:
                hp_str = f'{gs.hp_current}/{gs.hp_max}'
            if getattr(gs, 'stamina_offline', False):
                sta_str = 'OFFLINE'
            else:
                sta_pct = int(round(max(0.0, min(1.0, float(getattr(gs, 'stamina_pct', 0.0) or 0.0))) * 100.0))
                sta_str = f'{sta_pct}%'
            gs_desc = f'HP: {hp_str}  STA: {sta_str}'
        menu_level = int(getattr(self.owner, '_level', 0) or 0)
        menu_level_str = str(menu_level) if menu_level > 0 else ''
        if gs and hasattr(gs, 'level_base') and getattr(gs, 'level_base', 0) > 0:
            menu_level = int(gs.level_base)
            self.owner._level = menu_level
            level_extra = int(getattr(gs, 'level_extra', 0) or 0)
            menu_level_str = f'{menu_level}(+{level_extra})' if level_extra > 0 else str(menu_level)
        menu_prof = str(getattr(self.owner, '_profession', '') or '')
        if gs and hasattr(gs, 'profession_name') and getattr(gs, 'profession_name', ''):
            menu_prof = gs.profession_name
            self.owner._profession = menu_prof
        return {
            'username': str(getattr(self.owner, '_username', '') or ''),
            'level': menu_level_str,
            'profession': menu_prof,
            'hp': hp_str,
            'sta': sta_str,
            'des': gs_desc,
            'file': '',
        }

    @staticmethod
    def _session_int(value, default: int = 0) -> int:
        try:
            if value is None:
                return default
            return int(value)
        except Exception:
            try:
                text = str(value).strip()
                return int(text) if text.isdigit() else default
            except Exception:
                return default

    def _session_self_uid(self) -> int:
        for source in (
                getattr(self.owner, '_game_state', None),
                getattr(getattr(self.owner, '_state_mgr', None), 'state', None)):
            uid = self._session_int(getattr(source, 'player_id', 0), 0)
            if uid > 0:
                return uid
        return 0

    def _merge_session_player(self, uid, name='', fight_point=0, is_self=False):
        uid = self._session_int(uid, 0)
        if uid <= 0:
            return
        now = time.time()
        changed = False
        entry = self._session_players.get(uid)
        if not entry:
            entry = {
                'uid': uid,
                'name': '',
                'fight_point': 0,
                'first_seen': now,
                'updated_at': now,
                'is_self': False,
            }
            self._session_players[uid] = entry
            changed = True
        name = str(name or '').strip()
        fight_point = self._session_int(fight_point, 0)
        if name and entry.get('name') != name:
            entry['name'] = name
            changed = True
        if fight_point > 0 and entry.get('fight_point') != fight_point:
            entry['fight_point'] = fight_point
            changed = True
        next_is_self = bool(entry.get('is_self') or is_self)
        if bool(entry.get('is_self')) != next_is_self:
            entry['is_self'] = next_is_self
            changed = True
        entry['updated_at'] = now
        if changed:
            self._session_players_version += 1

    def _sync_session_players_cache(self, gs=None):
        self_uid = self._session_self_uid()
        if self_uid > 0 and self_uid != self._session_players_self_uid:
            if self._session_players_self_uid:
                self._session_players.clear()
                self._session_players_version += 1
            self._session_players_self_uid = self_uid
        if gs is None:
            gs = getattr(self.owner, '_game_state', None)
        if gs is not None:
            gs_uid = self._session_int(getattr(gs, 'player_id', 0), 0)
            self._merge_session_player(
                gs_uid,
                getattr(gs, 'player_name', '') or getattr(self.owner, '_username', '') or '',
                getattr(gs, 'fight_point', 0) or 0,
                is_self=bool(gs_uid and gs_uid == self_uid),
            )
        bridge = getattr(self.owner, '_packet_engine', None)
        if not bridge:
            return
        try:
            players = bridge.get_players() or {}
        except Exception:
            players = {}
        for raw_uid, pdata in players.items():
            uid = self._session_int(raw_uid, 0) or self._session_int(getattr(pdata, 'uid', 0), 0)
            self._merge_session_player(
                uid,
                getattr(pdata, 'name', '') or '',
                getattr(pdata, 'fight_point', 0) or 0,
                is_self=bool(uid and uid == self_uid),
            )

    def _format_session_power(self, value) -> str:
        value = self._session_int(value, 0)
        return f'{value:,}' if value > 0 else '--'

    def _get_session_player_rows(self, sync: bool = True):
        if sync:
            self._sync_session_players_cache(getattr(self.owner, '_game_state', None))
        self_uid = self._session_self_uid()
        rows = []
        for uid, entry in self._session_players.items():
            fp = self._session_int(entry.get('fight_point'), 0)
            is_self = bool(uid and uid == self_uid) or bool(entry.get('is_self'))
            rows.append({
                'uid': str(uid) if uid else '--',
                'name': str(entry.get('name') or ''),
                'fight_power': self._format_session_power(fp),
                'fight_power_value': fp,
                'is_self': is_self,
                'first_seen': float(entry.get('first_seen') or 0.0),
            })
        rows.sort(key=lambda r: (
            0 if r.get('is_self') else 1,
            -int(r.get('fight_power_value') or 0),
            str(r.get('name') or ''),
            str(r.get('uid') or ''),
        ))
        return rows

    def build_session_players_payload(self, sync: bool = True):
        rows = self._get_session_player_rows(sync=sync)
        return {
            'ok': True,
            'count': len(rows),
            'self_uid': str(self._session_self_uid() or ''),
            'players': rows,
            'generated_at': int(time.time()),
        }

    def sync_session_players_menu(self, force: bool = False):
        if not getattr(self.owner, 'menu_win', None):
            return
        self._sync_session_players_cache(getattr(self.owner, '_game_state', None))
        sig = (
            len(self._session_players),
            self._session_players_version,
            str(self._session_self_uid() or ''),
        )
        now = time.time()
        if not force and sig == self._session_players_last_sig:
            return
        if not force and now - self._session_players_last_push_ts < 0.5:
            return
        payload = self.build_session_players_payload(sync=False)
        self._session_players_last_sig = sig
        self._session_players_last_push_ts = now
        method = 'showSessionPlayers' if force else 'setSessionPlayersPayload'
        getattr(self.owner, '_eval_menu', lambda _: None)(
            f'if(window.SAO&&SAO.{method})SAO.{method}({json.dumps(payload, ensure_ascii=False)})'
        )

    def toggle_session_players_menu(self):
        if not getattr(self.owner, 'menu_win', None):
            return
        self._sync_session_players_cache(getattr(self.owner, '_game_state', None))
        payload = self.build_session_players_payload(sync=False)
        self._session_players_last_sig = (
            len(self._session_players),
            self._session_players_version,
            str(self._session_self_uid() or ''),
        )
        self._session_players_last_push_ts = time.time()
        getattr(self.owner, '_eval_menu', lambda _: None)(
            f'if(window.SAO&&SAO.showSessionPlayers)SAO.showSessionPlayers({json.dumps(payload, ensure_ascii=False)})'
        )

    def panel_theme_defaults(self):
        return {
            'hp': 'dark',
            'alert': 'dark',
            'dps': 'dark',
            'boss_hp': 'dark',
            'skillfx': 'dark',
            'buffmon': 'dark',
            'trigger_timer': 'dark',
            'data_source_health': 'dark',
            'report_export': 'dark',
            'offline_import': 'dark',
            'timeline_vcr': 'dark',
            'act_aggregate': 'dark',
            'mem_scope': 'dark',
            'action_log': 'dark',
            'death_recap': 'dark',
            'graph_timeseries': 'dark',
            'combatant_drilldown': 'dark',
            'skill_drilldown': 'dark',
            'raid_editor': 'dark',
            'autokey_editor': 'dark',
            'commander': 'dark',
        }

    def panel_theme_keys(self):
        return (
            'hp', 'alert', 'dps', 'boss_hp', 'bosshp', 'skillfx', 'buffmon',
            'trigger_timer', 'data_source_health', 'report_export',
            'offline_import', 'timeline_vcr', 'act_aggregate', 'mem_scope',
            'action_log', 'death_recap', 'graph_timeseries',
            'combatant_drilldown', 'skill_drilldown', 'raid_editor',
            'autokey_editor', 'commander',
        )

    def on_panel_theme_changed(self, panel: str, theme: str):
        if str(panel or '').lower() != 'buffmon':
            return
        for attr in ('_self_buff_overlay', '_boss_buff_overlay'):
            overlay = getattr(self.owner, attr, None)
            if overlay is not None and hasattr(overlay, '_apply_theme'):
                try:
                    overlay._apply_theme(theme)
                except Exception:
                    pass

    def build_menu_settings(self):
        o = self.owner
        try:
            from utils.sao_sound import get_sound_enabled, get_sound_volume
        except Exception:
            get_sound_enabled = lambda: True
            get_sound_volume = lambda: 70
        data_source_map = None
        try:
            ref = getattr(o, '_cfg_settings_ref', None)
            if ref is not None and hasattr(ref, 'get_data_source_map'):
                data_source_map = ref.get_data_source_map()
        except Exception:
            data_source_map = None
        return {
            'watched_slots': o._get_setting('watched_skill_slots', [1, 2, 3, 4, 5, 6, 7, 8, 9]),
            'burst_enabled': bool(o._get_setting('burst_enabled', True)),
            'sound_enabled': get_sound_enabled(),
            'sound_volume': get_sound_volume(),
            'mem_data_source': str(o._get_setting('mem_data_source', 'hybrid') or 'hybrid').lower(),
            'data_source_map': data_source_map,
            'auto_key': self._get_auto_key_menu_state(),
            'boss_bar_mode': o._get_setting('boss_bar_mode', 'boss_raid') or 'boss_raid',
            'dps_enabled': bool(o._get_setting('dps_enabled', True)),
            'dps_fade_timeout_s': int(o._get_setting('dps_fade_timeout_s', 5)),
            'dps_last_report_available': self._get_dps_last_report_available(),
            'buffmon_enabled': bool(o._get_setting('buffmon_enabled', True)),
            'raid_editor_visible': bool(getattr(o, '_raid_editor_visible', False)),
            'autokey_editor_visible': bool(getattr(o, '_autokey_editor_visible', False)),
            'commander_visible': bool(getattr(o, '_commander_visible', False)),
            'hotkey_labels': {
                'toggle_recognition': o._resolved_hotkey('toggle_recognition', 'F5'),
                'toggle_auto_script': o._resolved_hotkey('toggle_auto_script', 'F6'),
            },
        }

    def menu_action(self, action: str):
        o = self.owner
        mapping = {
            'toggle_auto_script': self._toggle_auto_script,
            'toggle_trigger_timer_manager': lambda: self._toggle_panel_surface('trigger_timer'),
            'toggle_data_source_health': lambda: self._toggle_panel_surface('data_source_health'),
            'toggle_report_export': lambda: self._toggle_panel_surface('report_export'),
            'toggle_offline_import': lambda: self._toggle_panel_surface('offline_import'),
            'toggle_timeline_vcr': lambda: self._toggle_panel_surface('timeline_vcr'),
            'toggle_act_aggregate': lambda: self._toggle_panel_surface('act_aggregate'),
            'toggle_mem_scope': lambda: self._toggle_panel_surface('mem_scope'),
            'toggle_action_log': lambda: self._toggle_panel_surface('action_log'),
            'toggle_death_recap': lambda: self._toggle_panel_surface('death_recap'),
            'toggle_graph_timeseries': lambda: self._toggle_panel_surface('graph_timeseries'),
            'toggle_combatant_drilldown': lambda: self._toggle_panel_surface('combatant_drilldown'),
            'toggle_skill_drilldown': lambda: self._toggle_panel_surface('skill_drilldown'),
            'toggle_raid_editor': lambda: self._hide_raid_editor()
            if getattr(self.owner, '_raid_editor_visible', False) else self._show_raid_editor(),
            'toggle_autokey_editor': lambda: self._hide_autokey_editor()
            if getattr(self.owner, '_autokey_editor_visible', False) else self._show_autokey_editor(),
            'toggle_commander': lambda: self._hide_commander()
            if getattr(self.owner, '_commander_visible', False) else self._show_commander(),
            'toggle_session_players': self.toggle_session_players_menu,
        }
        fn = mapping.get(str(action or ''))
        if callable(fn):
            return fn()
        return None

    def on_webview_started(self):
        o = self.owner
        try:
            self._inject_menu_assets()
        except Exception as exc:
            logger.warning("Star Resonance menu asset injection failed: %s", exc)
        o._apply_plugin_surfaces_transparency()
        o._set_plugin_surface_alpha('alert', 1.0)
        self._setup_alert_click_through()
        o._set_plugin_surface_alpha('skillfx', 0.0)
        try:
            if getattr(o, 'skillfx_win', None) and not getattr(o, '_skillfx_visible', False):
                o.skillfx_win.show()
                o._skillfx_visible = True
        except Exception:
            pass
        self._setup_skillfx_click_through()
        threading.Timer(0.5, self._setup_skillfx_click_through).start()
        self._update_skillfx_layout()

        def _reveal_skillfx():
            o._set_plugin_surface_alpha('skillfx', 1.0)
            self._setup_skillfx_click_through()

        threading.Timer(0.8, _reveal_skillfx).start()

        def _late_panel_recovery():
            try:
                if getattr(o, 'boss_hp_win', None) and not getattr(o, '_boss_hp_visible', False):
                    self._setup_boss_hp_click_through()
                    o._set_plugin_surface_alpha('boss_hp', 1.0)
                    try:
                        o.boss_hp_win.show()
                        o._boss_hp_visible = True
                        self._setup_boss_hp_click_through()
                    except Exception:
                        pass
                if getattr(o, 'dps_win', None):
                    o._set_plugin_surface_click_through('dps', enabled=True)
                    self._make_dps_unclickable()
                if getattr(o, 'buff_coverage_win', None) and not getattr(o, '_buff_cov_visible', False):
                    o._set_plugin_surface_click_through('buffmon', enabled=True)
                    o._set_plugin_surface_alpha('buffmon', 1.0)
                    o.buff_coverage_win.show()
                    o._buff_cov_visible = True
            except Exception:
                pass

        for delay in (4.0, 10.0, 16.0):
            threading.Timer(delay, _late_panel_recovery).start()

        for title in (
            'SAO Alert', 'SAO MapBanner', 'SAO MechBanner', 'SAO SkillFX',
            'SAO-BossHP', 'SAO-DPS', 'SAO-BuffCoverage', 'SAO-RaidEditor',
            'SAO-AutoKeyEditor', 'SAO-Commander',
        ):
            o._set_window_icon(title)
        self._setup_boss_hp_click_through()
        o._set_plugin_surface_click_through('boss_hp', enabled=True, wait_retries=20, ensure_on_top=True)
        o._set_plugin_surface_alpha('boss_hp', 1.0)
        try:
            if getattr(o, 'boss_hp_win', None):
                o.boss_hp_win.show()
                o._boss_hp_visible = True
                self._setup_boss_hp_click_through()
                geom = getattr(o, '_boss_hp_geometry', None) or {}
                fx_lr = geom.get('fx_lr', 200)
                fx_top = geom.get('fx_top', 120)
                fx_bot = geom.get('fx_bot', 160)
                multi_h = geom.get('multi_extra_h', 240)
                self._eval_boss_hp(
                    f'if(window.BossHP)BossHP.setFxMargins({fx_top},{fx_lr},{fx_bot},{fx_lr},{multi_h})')
        except Exception:
            pass
        try:
            o._dps_visible = False
            o._dps_mode = 'hidden'
            if getattr(o, 'dps_win', None):
                o._apply_webview2_transparency()
                o._set_plugin_surface_alpha('dps', 1.0)
            o._set_plugin_surface_click_through('dps', enabled=True)
            self._make_dps_unclickable()
            self._sync_dps_report_availability()
        except Exception:
            pass
        try:
            for surface in ('raid_editor', 'autokey_editor', 'commander'):
                o._set_plugin_surface_click_through(surface, enabled=True)
        except Exception:
            pass
        try:
            o._commander_visible = False
            if getattr(o, 'commander_win', None):
                o._set_plugin_surface_alpha('commander', 0.0)
                o.commander_win.hide()
                o._ensure_hidden_panels_passthrough()
        except Exception:
            pass
        return True

    def before_platform_shutdown(self):
        o = self.owner
        for title, duration, steps in (
            ('SAO Alert', 180, 10),
            ('SAO SkillFX', 180, 10),
            ('SAO-BossHP', 180, 10),
            ('SAO-RaidEditor', 140, 8),
            ('SAO-AutoKeyEditor', 140, 8),
            ('SAO-Commander', 140, 8),
        ):
            try:
                o._native_fade_window(title, duration_ms=duration, steps=steps)
            except Exception:
                pass

    def stop_engines(self, preserve_packet: bool = False):
        o = self.owner
        for attr in ('_auto_key_engine', '_boss_raid_engine'):
            engine = getattr(o, attr, None)
            if engine:
                try:
                    engine.stop()
                except Exception:
                    pass
                setattr(o, attr, None)
        tracker = getattr(o, '_dps_tracker', None)
        if tracker:
            try:
                tracker.save_player_cache()
            except Exception:
                pass
        return True

    def on_recognition_start(self):
        try:
            self._update_skillfx_layout()
        except Exception:
            pass
        try:
            self._sync_auto_key_menu()
        except Exception:
            pass
        try:
            self._sync_boss_raid_menu()
        except Exception:
            pass

    def render_tick(self, gs, recognition_active: bool = False):
        o = self.owner
        if gs is None:
            o._eval_hp('setSTAOffline(false)')
            o._eval_hp('setPlayState("idle")')
            return True
        o._game_state = gs
        if not recognition_active:
            self.sync_identity_alert(gs)
            self._render_idle_hp(gs)
            return True

        try:
            self.sync_identity_alert(gs)
            self._render_hp_sta(gs)
            self._render_boss_timer(gs)
            self._render_boss_bar(gs)
            self._render_dps(gs)
            self._render_skillfx(gs)
            self.sync_identity_panel(gs)
        except Exception as exc:
            print(f'[SR-WV] render tick failed: {exc}')
        return True

    def render_slow_tick(self, gs=None, recognition_active: bool = False):
        o = self.owner
        try:
            o._refresh_boss_hp_geometry()
        except Exception:
            pass
        try:
            self._sync_auto_key_menu()
        except Exception:
            pass
        try:
            self._sync_boss_raid_menu()
        except Exception:
            pass
        if getattr(o, '_commander_visible', False):
            try:
                o._push_commander_data()
            except Exception:
                pass
        return True

    def _level_text(self, gs) -> tuple[int, str]:
        level_base = int(getattr(gs, 'level_base', 0) or getattr(self.owner, '_level', 0) or 0)
        level_extra = int(getattr(gs, 'level_extra', 0) or 0)
        if level_extra > 0 and level_base > 0:
            return level_base, f'{level_base}(+{level_extra})'
        if level_base > 0:
            return level_base, str(level_base)
        return 0, ''

    def _render_idle_hp(self, gs):
        o = self.owner
        try:
            if getattr(gs, 'hp_max', 0) > 0:
                hp, hp_max = int(getattr(gs, 'hp_current', 0) or 0), int(gs.hp_max)
            elif getattr(gs, 'hp_pct', 0) > 0:
                hp, hp_max = int(gs.hp_pct * 100), 100
            else:
                hp, hp_max = 0, 1
            _, level_str = self._level_text(gs)
            sig = (hp, hp_max, level_str)
            if getattr(o, '_idle_hp_sig', None) != sig:
                o._idle_hp_sig = sig
                o._eval_hp(f'updateHP({hp}, {hp_max}, "{level_str}")')
            o._eval_hp('setSTAOffline(false)')
            o._eval_hp('setPlayState("idle")')
        except Exception:
            pass

    def _render_hp_sta(self, gs):
        o = self.owner
        if getattr(gs, 'hp_max', 0) > 0:
            hp, hp_max = getattr(gs, 'hp_current', 0), getattr(gs, 'hp_max', 1)
        elif getattr(gs, 'hp_pct', 0) > 0:
            hp, hp_max = int(gs.hp_pct * 100), 100
        else:
            hp, hp_max = 0, 1
        level_base, level_str = self._level_text(gs)
        if level_base > 0 and getattr(o, '_last_displayed_level_base', 0) > 0 \
                and level_base > o._last_displayed_level_base:
            o._eval_hp(f'showLevelUp({o._last_displayed_level_base}, {level_base})')
        if level_base > 0:
            o._last_displayed_level_base = level_base
        o._eval_hp(f'updateHP({hp}, {hp_max}, "{level_str}")')
        sta_offline = o._should_show_sta_offline(gs)
        o._eval_hp(f'setSTAOffline({str(bool(sta_offline)).lower()})')
        if not sta_offline:
            sta = int(round(max(0.0, min(1.0, float(getattr(gs, 'stamina_pct', 0.0) or 0.0))) * 100.0))
            o._eval_hp(f'updateSTA({sta}, 100)')
        if getattr(gs, 'recognition_ok', False) or getattr(gs, 'packet_active', False):
            o._eval_hp('setPlayState("playing")')
        else:
            o._eval_hp('setPlayState("idle")')

    def _render_boss_timer(self, gs):
        o = self.owner
        text = getattr(gs, 'boss_timer_text', '') or ''
        active = getattr(gs, 'boss_raid_active', False)
        enrage = float(getattr(gs, 'boss_enrage_remaining', 0) or 0)
        if active and text:
            urgency = getattr(gs, 'boss_enrage_urgency', '') or ('urgent' if 0 < enrage < 60 else 'normal')
        else:
            text = ''
            urgency = ''
        if text != getattr(o, '_last_boss_timer_text', '') or urgency != getattr(o, '_last_boss_timer_urgency', ''):
            o._last_boss_timer_text = text
            o._last_boss_timer_urgency = urgency
            if text:
                o._eval_hp(f'setBossTimer("{o._safe_js(text)}", "{urgency}")')
            else:
                o._eval_hp('setBossTimer("", "")')

    def _render_boss_bar(self, gs):
        o = self.owner
        mode = o._get_setting('boss_bar_mode', 'boss_raid') or 'boss_raid'
        raid_active = getattr(gs, 'boss_raid_active', False)
        source = getattr(gs, 'boss_hp_source', 'none') or 'none'
        direct_hp = 0
        direct_max = 0
        direct_data = None
        additional = []
        now = time.time()
        timeout = o._boss_hp_hold_timeout_s()
        has_recent_self_damage = (now - getattr(o, '_bb_last_damage_ts', 0.0)) < timeout
        for uuid in list(getattr(o, '_bb_recent_targets', {}).keys()):
            if now - o._bb_recent_targets.get(uuid, 0) > timeout:
                o._bb_recent_targets.pop(uuid, None)
        if not raid_active:
            bridge = getattr(o, '_packet_engine', None)
            if bridge and has_recent_self_damage and getattr(o, '_bb_recent_targets', None):
                recent_monsters = []
                for uuid, dmg_ts in list(o._bb_recent_targets.items()):
                    if now - dmg_ts < timeout:
                        monster = bridge.get_monster(uuid)
                        if o._boss_monster_usable(monster):
                            recent_monsters.append(monster)
                if recent_monsters:
                    from plugins.star_resonance_plugin.panels.sao_gui_state_mixin import _boss_bar_main_key
                    recent_monsters.sort(key=lambda m: _boss_bar_main_key(m, o._bb_recent_targets))
                    main = recent_monsters[0]
                    o._bb_last_target_uuid = getattr(main, 'uuid', 0)
                    direct_max = int(getattr(main, 'max_hp', 0)) or int(getattr(main, 'hp', 0))
                    direct_hp = max(0, int(getattr(main, 'hp', 0)))
                    direct_data = main.to_dict() if hasattr(main, 'to_dict') else {}
                    source = 'packet'
                    from plugins.star_resonance_plugin.panels.sao_gui_state_mixin import _bb_resolve_unit_name as bb_name
                    for monster in recent_monsters[1:]:
                        if len(additional) >= 4:
                            break
                        data = monster.to_dict() if hasattr(monster, 'to_dict') else {}
                        name = (bb_name(data, getattr(monster, 'uuid', 0), getattr(o, '_dps_tracker', None))
                                or str(data.get('name', 'Unit'))[:20])
                        additional.append({
                            'uuid': int(getattr(monster, 'uuid', 0) or 0),
                            'name': name,
                            'hp_pct': round(float(data.get('hp_pct', 0.0)), 3),
                            'extinction_pct': round(float(data.get('extinction_pct', 0.0)), 3),
                            'has_break_data': bool(data.get('has_break_data', False)),
                            'breaking_stage': int(data.get('breaking_stage', -1)),
                            'shield_active': bool(data.get('shield_active', False)),
                            'shield_pct': round(float(data.get('shield_pct', 0.0)), 3),
                        })
            elif getattr(o, '_bb_last_target_uuid', 0) and not has_recent_self_damage:
                try:
                    monster = bridge.get_monster(o._bb_last_target_uuid) if bridge else None
                    if o._boss_monster_usable(monster):
                        direct_max = int(getattr(monster, 'max_hp', 0)) or int(getattr(monster, 'hp', 0))
                        direct_hp = max(0, int(getattr(monster, 'hp', 0)))
                        direct_data = monster.to_dict() if hasattr(monster, 'to_dict') else {}
                        source = 'packet'
                except Exception:
                    pass
            if len(additional) < 4 and direct_data is not None:
                from plugins.star_resonance_plugin.panels.sao_gui_state_mixin import _mem_supplement_additional
                additional = _mem_supplement_additional(
                    bridge, additional, getattr(o, '_bb_last_target_uuid', 0), getattr(o, '_dps_tracker', None))

        if mode == 'off':
            show = False
        elif raid_active:
            show = True
        else:
            show = has_recent_self_damage and (source != 'none' or direct_data is not None)
        if direct_data and not raid_active:
            hp_pct = direct_hp / direct_max if direct_max > 0 else 1.0
            cur_hp = direct_hp
            total_hp = direct_max
            shield_active = bool(direct_data.get('shield_active'))
            shield_pct = float(direct_data.get('shield_pct') or 0.0)
            breaking = int(direct_data.get('breaking_stage') or 0)
            has_break = bool(direct_data.get('has_break_data'))
            extinction = float(direct_data.get('extinction_pct') or 0.0)
            extinction_raw = int(direct_data.get('extinction') or 0)
            max_extinction = int(direct_data.get('max_extinction') or 0)
            stop_ticking = bool(direct_data.get('stop_breaking_ticking'))
            overdrive = bool(direct_data.get('in_overdrive'))
            invincible = False
        else:
            hp_pct = round(getattr(gs, 'boss_hp_est_pct', 1.0), 3)
            cur_hp = getattr(gs, 'boss_current_hp', 0)
            total_hp = getattr(gs, 'boss_total_hp', 0)
            shield_active = getattr(gs, 'boss_shield_active', False)
            shield_pct = round(getattr(gs, 'boss_shield_pct', 0.0), 3)
            breaking = getattr(gs, 'boss_breaking_stage', -1)
            has_break = getattr(gs, 'boss_breaking_stage', -1) != -1
            extinction = round(getattr(gs, 'boss_extinction_pct', 0.0), 3)
            extinction_raw = 0
            max_extinction = 0
            stop_ticking = False
            overdrive = getattr(gs, 'boss_in_overdrive', False)
            invincible = getattr(gs, 'boss_invincible', False)
        mem_break = self.mem_boss_break_override(getattr(o, '_packet_engine', None))
        if mem_break is not None:
            breaking, has_break, extinction, stop_ticking = mem_break
            extinction_raw = 0
            max_extinction = 0
        from plugins.star_resonance_plugin.panels.sao_gui_state_mixin import _bb_resolve_unit_name as bb_name
        boss_name = bb_name(
            direct_data,
            (direct_data or {}).get('uuid', 0) or getattr(o, '_bb_last_target_uuid', 0),
            getattr(o, '_dps_tracker', None)) or ''
        sig = (
            show, round(float(hp_pct), 3), source, int(cur_hp), int(total_hp),
            bool(shield_active), round(float(shield_pct), 3), int(breaking),
            bool(has_break), round(float(extinction), 3), int(extinction_raw),
            int(max_extinction), bool(stop_ticking), bool(overdrive), bool(invincible),
            boss_name,
        )
        if sig == getattr(o, '_last_boss_bar_sig', None):
            return
        o._last_boss_bar_sig = sig
        break_time = 0.0
        try:
            template_id = int((direct_data or {}).get('template_id') or 0)
            if template_id > 0:
                break_time = self.get_break_recovery_time(template_id)
        except Exception:
            pass
        payload = {
            'active': show,
            'hp_pct': sig[1],
            'hp_source': source,
            'current_hp': sig[3],
            'total_hp': sig[4],
            'shield_active': sig[5],
            'shield_pct': sig[6],
            'breaking_stage': sig[7],
            'has_break_data': sig[8],
            'extinction_pct': sig[9],
            'extinction': sig[10],
            'max_extinction': sig[11],
            'stop_breaking_ticking': sig[12],
            'break_recovery_time': break_time,
            'in_overdrive': sig[13],
            'invincible': sig[14],
            'boss_name': boss_name,
            'additional': additional,
        }
        o._eval_boss_hp(f'updateBossBar({json.dumps(payload)})')

    def _render_dps(self, gs):
        o = self.owner
        tracker = getattr(o, '_dps_tracker', None)
        if not tracker:
            return
        try:
            buffs_ref = getattr(gs, 'self_buffs', None)
            if buffs_ref is not getattr(o, '_last_self_buffs_ref', None):
                o._last_self_buffs_ref = buffs_ref
                buff_list = list(buffs_ref or [])
                tracker.update_self_buffs(buff_list)
                o._push_buff_coverage(gs, buff_list)
            if getattr(gs, 'player_id', None):
                player_uid = int(gs.player_id) if str(gs.player_id).isdigit() else 0
                if player_uid:
                    tracker.set_self_uid(player_uid)
                    fight_point = 0
                    bridge = getattr(o, '_packet_engine', None)
                    if bridge:
                        player = bridge.get_players().get(player_uid)
                        if player:
                            fight_point = getattr(player, 'fight_point', 0) or 0
                    tracker.update_player_info(
                        player_uid, getattr(gs, 'player_name', '') or '',
                        getattr(gs, 'profession_name', '') or '', fight_point,
                        int(getattr(gs, 'level_base', 0) or 0))
            bridge = getattr(o, '_packet_engine', None)
            if bridge:
                try:
                    for uid, player in bridge.get_players().items():
                        if uid and getattr(player, 'name', ''):
                            tracker.update_player_info(
                                uid, player.name or '', getattr(player, 'profession', '') or '',
                                getattr(player, 'fight_point', 0) or 0,
                                getattr(player, 'level', 0) or 0)
                except Exception:
                    pass
            enabled = bool(o._get_setting('dps_enabled', True))
            idle_timeout = o._combat_damage_timeout_s()
            if tracker.finalize_if_idle(idle_timeout, 'idle_timeout'):
                o._sync_dps_report_availability()
                if getattr(o, '_dps_mode', '') == 'live':
                    o._hide_dps_window()
            if tracker.is_dirty():
                snapshot = tracker.get_snapshot()
                has_live = bool(int(snapshot.get('total_damage') or 0) > 0 and tracker.has_recent_damage(idle_timeout))
                if enabled and has_live:
                    o._show_dps_live_snapshot(snapshot)
                elif getattr(o, '_dps_visible', False) and getattr(o, '_dps_mode', '') == 'live':
                    o._eval_dps(f'DpsMeter.updateDps({json.dumps(snapshot, ensure_ascii=False)})')
                    o._push_dps_act_snapshot()
            if getattr(o, '_dps_visible', False) and getattr(o, '_dps_mode', '') == 'live':
                try:
                    if not tracker.has_recent_damage(idle_timeout):
                        o._hide_dps_window()
                except Exception:
                    pass
            o._sync_dps_report_availability()
        except Exception:
            pass

    def _render_skillfx(self, gs):
        o = self.owner
        burst_enabled = o._get_setting('burst_enabled', True)
        burst_now = getattr(gs, 'burst_ready', False)
        burst_prev = getattr(o, '_last_burst_ready', False)
        burst_slot = o._pick_burst_trigger_slot(gs) if burst_enabled else 0
        prev_layout = getattr(o, '_skillfx_layout', None)
        payload = o._build_skillfx_payload(gs) or {}
        if prev_layout != getattr(o, '_skillfx_layout', None):
            o._update_skillfx_layout()
        payload['burst_slot'] = int(burst_slot or 0)
        payload['burst_ready'] = bool(burst_now)
        payload['enabled'] = bool(burst_enabled)
        custom_slots = getattr(gs, 'custom_skill_slots', []) or []
        if custom_slots:
            payload['custom_slots'] = [slot for slot in custom_slots if slot.get('visual_enabled')]
        o._eval_skillfx(f'SkillFX.update({json.dumps(payload, ensure_ascii=False)})')
        if (not burst_now) and burst_prev:
            o._eval_skillfx('SkillFX.hideBurstReady()')
        o._last_burst_ready = burst_now
        if getattr(o, '_autokey_editor_visible', False):
            try:
                o._push_autokey_editor_slots()
                if burst_now != burst_prev:
                    o._push_autokey_editor_state()
            except Exception:
                pass
        if getattr(o, '_raid_editor_visible', False):
            try:
                o._push_raid_editor_status()
            except Exception:
                pass

    def _eval_skillfx(self, js):
        self.owner._eval_plugin_surface('skillfx', js)

    def _eval_boss_hp(self, js):
        self.owner._eval_plugin_surface('boss_hp', js)

    def _eval_alert(self, js):
        self.owner._eval_plugin_surface('alert', js)

    def _eval_mapbanner(self, js):
        self.owner._eval_plugin_surface('mapbanner', js)

    def _eval_mech_banner(self, js):
        self.owner._eval_plugin_surface('mech_banner', js)

    def _eval_buff_coverage(self, js):
        self.owner._eval_plugin_surface('buffmon', js)

    def _eval_dps(self, js):
        self.owner._eval_plugin_surface('dps', js)

    def _eval_raid_editor(self, js):
        self.owner._eval_plugin_surface('raid_editor', js)

    def _eval_autokey_editor(self, js):
        self.owner._eval_plugin_surface('autokey_editor', js)

    def _eval_commander(self, js):
        self.owner._eval_plugin_surface('commander', js)

    def _sync_surface_hwnd_attr(self, surface: str, attr: str):
        try:
            meta = getattr(self.owner, '_plugin_surface_meta', {}).get(surface, {}) or {}
            hwnd = int(meta.get('hwnd') or 0)
            if hwnd:
                setattr(self.owner, attr, hwnd)
        except Exception:
            pass

    def _push_mech_banner(self, entry):
        # 机制横幅推一行 (懒显示窗口, 常驻鼠标穿透覆盖层)。
        o = self.owner
        try:
            if not getattr(o, 'mech_banner_win', None):
                return
            if not getattr(o, '_mech_banner_shown', False):
                o._mech_banner_shown = True
                try:
                    o.mech_banner_win.show()
                except Exception:
                    pass
                self._setup_mech_banner_click_through()
            payload = json.dumps(entry or {}, ensure_ascii=False)
            self._eval_mech_banner(
                f'if (window.MechBanner) MechBanner.push({payload})')
        except Exception:
            pass

    def _setup_mech_banner_click_through(self, _wait_retries: int = 20):
        # Make mechanic banner overlay fully click-through (纯覆盖层)。
        self.owner._set_plugin_surface_click_through(
            'mech_banner', enabled=True, wait_retries=_wait_retries)
        self._sync_surface_hwnd_attr('mech_banner', '_mech_banner_hwnd')

    def _ensure_alert_on_top(self):
        self.owner._ensure_plugin_surface_on_top('alert')
        self._sync_surface_hwnd_attr('alert', '_alert_hwnd')

    def _calc_alert_window_rect(self, width: int = 416, height: int = 226):
        left, top, right, bottom = self.owner._get_window_monitor_work_area('SAO-HP')
        work_w = max(width, right - left)
        work_h = max(height, bottom - top)
        x = left + max(0, int((work_w - width) / 2))
        y = top + max(28, int(work_h * 0.16))
        max_y = bottom - height - 28
        if max_y >= top:
            y = min(y, max_y)
        return int(x), int(y), int(width), int(height)

    def _position_alert_window(self):
        o = self.owner
        if not getattr(o, 'alert_win', None):
            return
        x, y, width, height = self._calc_alert_window_rect()
        try:
            o.alert_win.resize(width, height)
        except Exception:
            pass
        try:
            o.alert_win.move(x, y)
        except Exception:
            pass

    def _show_identity_alert_window(self, title: str, message: str,
                                    duration_ms: int = 9000,
                                    play_sound: bool = True,
                                    alert_kind: str = 'generic'):
        o = self.owner
        if not getattr(o, 'alert_win', None):
            return
        # v2.1.2-m: 防"alert 一直弹很多次"问题. 调用方很多,
        # 同一条 (title,message) 在 alert 仍可见 + 4s 窗口内重复触发时,
        # 直接续展当前 alert, 不再 hide+show 闪一下。
        try:
            now_ts = time.time()
        except Exception:
            now_ts = 0.0
        sig = (str(alert_kind or 'generic'), str(title or ''), str(message or ''))
        last_sig = getattr(o, '_last_alert_sig', None)
        last_ts = float(getattr(o, '_last_alert_sig_ts', 0.0) or 0.0)
        if (sig == last_sig
                and getattr(o, '_identity_alert_visible', False)
                and (now_ts - last_ts) < 4.0):
            # 同一条 alert 重复触发: 仅刷新 timestamp / 续 stay 计时, 不重弹
            o._last_alert_sig_ts = now_ts
            return
        o._last_alert_sig = sig
        o._last_alert_sig_ts = now_ts

        o._identity_alert_visible = True
        o._identity_alert_kind = str(alert_kind or 'generic')
        o._identity_alert_nonce = int(getattr(o, '_identity_alert_nonce', 0) or 0) + 1
        nonce = o._identity_alert_nonce
        stay_ms = int(duration_ms or 9000)
        if stay_ms <= 0:
            stay_ms = 9000

        self._position_alert_window()
        try:
            o.alert_win.show()
        except Exception:
            pass
        try:
            o._apply_webview2_transparency()
        except Exception:
            pass
        # Remove click-through so alert buttons can be clicked while visible
        self._remove_alert_click_through()
        o._set_plugin_surface_alpha('alert', 1.0)
        self._ensure_alert_on_top()
        if play_sound:
            o._play_sound('alert')

        safe_title = o._safe_js(title or '提示')
        safe_message = o._safe_js(message or '')

        def _push():
            if nonce != int(getattr(o, '_identity_alert_nonce', 0) or 0):
                return
            self._eval_alert(
                f'if (window.AlertPanel && AlertPanel.showAlert) '
                f'AlertPanel.showAlert("{safe_title}", "{safe_message}")'
            )
            self._ensure_alert_on_top()

        _push()
        threading.Timer(0.35, _push).start()
        threading.Timer(stay_ms / 1000.0, lambda: self._hide_identity_alert_window(expected_nonce=nonce)).start()

    def _hide_identity_alert_window(self, expected_nonce: int = None, force: bool = False):
        o = self.owner
        if not getattr(o, 'alert_win', None):
            return
        current_nonce = int(getattr(o, '_identity_alert_nonce', 0) or 0)
        if expected_nonce is not None and expected_nonce != current_nonce:
            return

        # While a plugin-registered persistent alert is shown, refuse unforced
        # external dismiss requests so the plugin's refresh loop keeps it alive.
        # The plugin's own stop passes force=True to close it. (Stale nonce-
        # bearing auto-hide timers are already rejected above.)
        if (not force and expected_nonce is None
                and o._is_persistent_alert_kind(getattr(o, '_identity_alert_kind', ''))):
            return

        was_visible = bool(getattr(o, '_identity_alert_visible', False))
        o._identity_alert_visible = False
        o._identity_alert_nonce = current_nonce + 1
        closing_nonce = o._identity_alert_nonce

        if not was_visible:
            try:
                o.alert_win.hide()
            except Exception:
                pass
            o._identity_alert_kind = ''
            self._setup_alert_click_through()
            return

        o._play_sound('alert_close')
        self._eval_alert('if (window.AlertPanel && AlertPanel.beginClose) AlertPanel.beginClose()')

        def _finish_hide():
            if closing_nonce != int(getattr(o, '_identity_alert_nonce', 0) or 0):
                return
            try:
                o.alert_win.hide()
            except Exception:
                pass
            o._identity_alert_kind = ''
            # Restore click-through so hidden alert window never captures mouse
            self._setup_alert_click_through()

        threading.Timer(0.52, _finish_hide).start()

    # ── Map-name banner (切换地图中央横幅) ──

    def _position_mapbanner_window(self):
        # 把横幅窗口居中到游戏所在显示器的正中央。
        o = self.owner
        if not getattr(o, 'mapbanner_win', None):
            return
        try:
            rect = getattr(o, '_hud_monitor_rect', None)
            if rect:
                left, top, right, bottom = rect
            else:
                left, top = 0, 0
                right = ctypes.windll.user32.GetSystemMetrics(0)
                bottom = ctypes.windll.user32.GetSystemMetrics(1)
            sw = max(1, right - left)
            sh = max(1, bottom - top)
            width = max(640, int(sw * 0.6))
            height = 280
            x = left + max(0, int((sw - width) / 2))
            y = top + max(0, int((sh - height) / 2))
            try:
                o.mapbanner_win.resize(o._to_webview_px(width), o._to_webview_px(height))
            except Exception:
                pass
            try:
                o.mapbanner_win.move(o._to_webview_px(x), o._to_webview_px(y))
            except Exception:
                pass
        except Exception:
            pass

    def _schedule_map_banner(self, name: str):
        # 检测到切换地图 → 延迟 3 秒后在屏幕中央淡入地图名 (WebView)。
        #
        # 去重改为时间窗 (5s): 同名且距上次调度 < 5s 视为同一次进图的重复抓包,
        # 吞掉防止狂闪; 超过 5s 再次进入同一场景会重新弹 (满足"第二次切入同场景
        # 也要刷出名字")。快速连切到别的图时取消上一个未触发的延迟, 只显示最新。
        o = self.owner
        name = (name or '').strip()
        if not name or not getattr(o, 'mapbanner_win', None):
            return
        now = time.time()
        last = getattr(o, '_mapbanner_last_name', '')
        last_ts = float(getattr(o, '_mapbanner_last_ts', 0.0) or 0.0)
        if name == last and (now - last_ts) < 5.0:
            return
        o._mapbanner_last_name = name
        o._mapbanner_last_ts = now
        print(f'[MapBanner][webview] schedule name={name!r} (+3s)', flush=True)
        prev = getattr(o, '_mapbanner_timer', None)
        if prev is not None:
            try:
                prev.cancel()
            except Exception:
                pass
        t = threading.Timer(3.0, lambda n=name: self._show_map_banner_window(n))
        t.daemon = True
        o._mapbanner_timer = t
        t.start()

    def _show_map_banner_window(self, name: str):
        o = self.owner
        name = (name or '').strip()
        if not name or not getattr(o, 'mapbanner_win', None):
            return
        o._mapbanner_nonce = int(getattr(o, '_mapbanner_nonce', 0) or 0) + 1
        nonce = o._mapbanner_nonce

        self._position_mapbanner_window()
        try:
            o.mapbanner_win.show()
        except Exception:
            pass
        try:
            o._apply_webview2_transparency()
        except Exception:
            pass
        o._set_plugin_surface_alpha('mapbanner', 1.0)
        # 纯覆盖层: 立即设鼠标穿透, 避免拦截游戏点击
        self._setup_mapbanner_click_through()
        threading.Timer(0.5, lambda: self._setup_mapbanner_click_through(_wait_retries=0)).start()

        safe_name = o._safe_js(name)

        def _push():
            if nonce != int(getattr(o, '_mapbanner_nonce', 0) or 0):
                return
            self._eval_mapbanner(
                f'if (window.MapBanner && MapBanner.showBanner) '
                f'MapBanner.showBanner("{safe_name}")'
            )
            self._ensure_mapbanner_on_top()

        _push()
        threading.Timer(0.35, _push).start()
        # 显示约 2.6s 后淡出 (与 mapbanner.html 动画时长配合)
        threading.Timer(3.0, lambda: self._hide_map_banner_window(expected_nonce=nonce)).start()

    def _hide_map_banner_window(self, expected_nonce: int = None):
        o = self.owner
        if not getattr(o, 'mapbanner_win', None):
            return
        if expected_nonce is not None and expected_nonce != int(getattr(o, '_mapbanner_nonce', 0) or 0):
            return
        self._eval_mapbanner('if (window.MapBanner && MapBanner.beginClose) MapBanner.beginClose()')
        closing_nonce = int(getattr(o, '_mapbanner_nonce', 0) or 0)

        def _finish_hide():
            if closing_nonce != int(getattr(o, '_mapbanner_nonce', 0) or 0):
                return
            try:
                o.mapbanner_win.hide()
            except Exception:
                pass

        threading.Timer(0.6, _finish_hide).start()

    def _setup_alert_click_through(self):
        # Make alert window fully click-through (WS_EX_TRANSPARENT).
        #
        # Default state: always click-through. Temporarily removed
        # when an alert is actively showing so buttons can be clicked.
        self.owner._set_plugin_surface_click_through('alert', enabled=True)
        self._sync_surface_hwnd_attr('alert', '_alert_hwnd')

    def _remove_alert_click_through(self):
        # Temporarily remove WS_EX_TRANSPARENT so alert buttons are clickable.
        self.owner._set_plugin_surface_click_through('alert', enabled=False)
        self._sync_surface_hwnd_attr('alert', '_alert_hwnd')

    def _setup_mapbanner_click_through(self, _wait_retries: int = 20):
        # Make Map-name banner overlay fully click-through (纯覆盖层, 永不挡点击).
        self.owner._set_plugin_surface_click_through(
            'mapbanner', enabled=True, wait_retries=_wait_retries, ensure_on_top=True)
        self._sync_surface_hwnd_attr('mapbanner', '_mapbanner_hwnd')

    def _ensure_mapbanner_on_top(self):
        self.owner._ensure_plugin_surface_on_top('mapbanner')
        self._sync_surface_hwnd_attr('mapbanner', '_mapbanner_hwnd')

    def maintain_transient_surfaces(self):
        self._setup_mapbanner_click_through(_wait_retries=0)
        if not getattr(self.owner, '_identity_alert_visible', False):
            self._setup_alert_click_through()

    def show_platform_notice(self, title: str, message: str, duration_ms: int = 5000):
        self._show_identity_alert_window(title, message, duration_ms)

    def _push_buff_coverage(self, gs, buf_list):
        o = self.owner
        try:
            tracker = getattr(o, '_dps_tracker', None)
            if tracker is None:
                return
            if not o._get_setting('buffmon_enabled', True):
                if getattr(o, '_last_buff_cov_sig', None) != 'off':
                    o._last_buff_cov_sig = 'off'
                    self._eval_buff_coverage('updateBuffCoverage({"buffs":[]})')
                return
            try:
                from plugins.star_resonance_plugin.panels.sao_gui_buffmon import is_ultimate_buff
            except Exception:
                is_ultimate_buff = lambda *a, **k: False
            uptime = tracker.get_buff_uptime()
            server_offset = float(getattr(gs, 'server_time_offset_ms', 0.0) or 0.0)
            now_ms = time.time() * 1000.0 + server_offset
            rem_by_id = {}
            for buff in (buf_list or []):
                if not isinstance(buff, dict):
                    continue
                bid = int(buff.get('id', 0) or buff.get('buff_id', 0) or 0)
                begin = int(buff.get('begin_ms', 0) or buff.get('begin_time', 0) or 0)
                duration = int(buff.get('duration_ms', 0) or buff.get('duration', 0) or 0)
                if bid and duration > 0 and begin > 0:
                    rem_by_id[bid] = round(max(0.0, (begin + duration - now_ms) / 1000.0), 1)
            rows = []
            for item in uptime.get('buffs', []):
                bid = int(item.get('buff_id', 0) or 0)
                name = item.get('name', '')
                if not is_ultimate_buff(name, bid):
                    continue
                rows.append({
                    'id': bid,
                    'name': name,
                    'uptime_pct': item.get('uptime_pct', 0.0),
                    'apply_count': item.get('apply_count', 0),
                    'layer': item.get('layer', 0),
                    'max_layer': item.get('max_layer', 0),
                    'active': bool(item.get('active', False)),
                    'rem_s': rem_by_id.get(bid, -1.0),
                })
            payload = {'elapsed_ms': uptime.get('elapsed_ms', 0), 'buffs': rows}
            js_payload = json.dumps(payload, ensure_ascii=False)
            if js_payload != getattr(o, '_last_buff_cov_sig', None):
                o._last_buff_cov_sig = js_payload
                self._eval_buff_coverage('updateBuffCoverage(%s)' % js_payload)
        except Exception:
            pass

    def _calc_boss_hp_geometry(self):
        try:
            sw = ctypes.windll.user32.GetSystemMetrics(0)
            sh = ctypes.windll.user32.GetSystemMetrics(1)
        except Exception:
            sw, sh = 1920, 1080
        pad = max(8, int(min(sw, sh) * 0.008))
        bar_w = max(556, int(sw * 0.28960))
        bar_h = max(88, int(sh * 0.08150))
        right = int(sw * 0.62813)
        fx_lr = max(160, int(min(sw, 1920) * 0.088))
        fx_top = max(100, int(min(sh, 1080) * 0.096))
        fx_bot = max(160, int(min(sh, 1080) * 0.150))
        bar_screen_x = right - bar_w - pad
        bar_screen_y = max(0, int(sh * 0.0046) - 24) + 24
        multi_extra_h = 240
        return {
            'width': bar_w + pad * 2 + 20 + fx_lr * 2,
            'height': bar_h + pad * 2 + fx_top + fx_bot + multi_extra_h,
            'x': bar_screen_x - fx_lr,
            'y': bar_screen_y - fx_top,
            'fx_lr': fx_lr,
            'fx_top': fx_top,
            'fx_bot': fx_bot,
            'multi_extra_h': multi_extra_h,
        }

    def _refresh_boss_hp_geometry(self, force: bool = False):
        o = self.owner
        geom = self._calc_boss_hp_geometry()
        if (not force) and getattr(o, '_boss_hp_geometry', None) == geom:
            return
        o._boss_hp_geometry = geom
        try:
            win = getattr(o, 'boss_hp_win', None)
            if win:
                win.resize(int(geom['width']), int(geom['height']))
                win.move(int(geom['x']), int(geom['y']))
                fx_lr = geom.get('fx_lr', 200)
                fx_top = geom.get('fx_top', 120)
                fx_bot = geom.get('fx_bot', 160)
                multi_h = geom.get('multi_extra_h', 240)
                self._eval_boss_hp(
                    f'if(window.BossHP)BossHP.setFxMargins({fx_top},{fx_lr},{fx_bot},{fx_lr},{multi_h})')
                self._setup_boss_hp_click_through()
        except Exception:
            pass

    def _build_dps_act_snapshot(self, history_limit: int = 0):
        o = self.owner
        try:
            return build_act_snapshot(
                dps_tracker=getattr(o, '_dps_tracker', None),
                history_store=getattr(o, '_dps_history_store', None),
                state_mgr=getattr(o, '_state_mgr', None),
                encounter_mgr=getattr(o, '_encounter_mgr', None),
                trigger_engine=getattr(o, '_act_trigger_engine', None),
                packet_probe=getattr(o, '_packet_engine', None),
                memory_probe=getattr(o, '_mem_bridge', None),
                history_limit=history_limit,
                lite=True,
            )
        except Exception:
            return {}

    def _push_dps_act_snapshot(self, throttle: bool = False):
        o = self.owner
        try:
            if not getattr(o, 'dps_win', None):
                return
            if throttle and (time.time() - getattr(o, '_last_act_snap_push_t', 0.0)) < 0.07:
                return
            o._last_act_snap_push_t = time.time()
            snapshot = self._build_dps_act_snapshot()
            if not snapshot:
                return
            publish_owner_event(o, 'act_snapshot', snapshot, source_name='webview', source_kind='ui')
            try:
                hooked = platform_render_apply_hooks(o, 'dps', snapshot)
                if hooked.get('ok') and hooked.get('payload') is not None:
                    snapshot = hooked['payload']
            except Exception:
                pass
            self._eval_dps(
                f'if(window.DpsMeter&&DpsMeter.showActSnapshot)DpsMeter.showActSnapshot({json.dumps(snapshot, ensure_ascii=False)})')
        except Exception:
            pass

    def _resize_dps_window(self, width: int, height: int, persist: bool = False):
        o = self.owner
        try:
            width = max(520, min(1180, int(width)))
            height = max(420, min(900, int(height)))
        except Exception:
            return
        o._dps_detail_w = width
        o._dps_detail_h = height
        try:
            if getattr(o, 'dps_win', None):
                o.dps_win.resize(width, height)
        except Exception:
            pass
        if persist:
            try:
                o._set_setting('dps_detail_w', width)
                o._set_setting('dps_detail_h', height)
            except Exception:
                pass

    def _set_dps_detail_mode(self, active: bool):
        o = self.owner
        o._dps_detail_mode = bool(active)
        try:
            if o._dps_detail_mode:
                self._resize_dps_window(int(getattr(o, '_dps_detail_w', 760) or 760),
                                        int(getattr(o, '_dps_detail_h', 560) or 560))
            else:
                if getattr(o, 'dps_win', None):
                    o.dps_win.resize(int(getattr(o, '_dps_base_w', 360) or 360),
                                     int(getattr(o, '_dps_base_h', 520) or 520))
        except Exception:
            pass
        if getattr(o, '_dps_visible', False):
            self._ensure_dps_clickable()

    def _combat_damage_timeout_s(self) -> float:
        o = self.owner
        try:
            raw = o._get_setting('dps_fade_timeout_s', 5)
            value = float(raw if raw is not None else 5)
        except Exception:
            value = 5.0
        if value <= 0:
            return 86400.0
        return float(max(1.0, value))

    def _boss_hp_hold_timeout_s(self) -> float:
        o = self.owner
        try:
            raw = o._get_setting('boss_hp_hold_timeout_s', 5)
            value = float(raw if raw is not None else 5)
        except Exception:
            value = 5.0
        if value <= 0:
            return 86400.0
        return float(max(1.0, value, o._combat_damage_timeout_s()))

    def _get_dps_last_report_available(self) -> bool:
        tracker = getattr(self.owner, '_dps_tracker', None)
        if not tracker:
            return False
        try:
            return bool(tracker.has_last_report())
        except Exception:
            return False

    def _sync_dps_report_availability(self):
        o = self.owner
        available = self._get_dps_last_report_available()
        if available == getattr(o, '_dps_last_report_available', False):
            return
        o._dps_last_report_available = available
        o._sync_menu_settings()

    def _ensure_dps_clickable(self):
        self.owner._set_plugin_surface_click_through('dps', enabled=False)

    def _make_dps_unclickable(self):
        self.owner._set_plugin_surface_click_through('dps', enabled=True)

    def _show_dps_window(self):
        o = self.owner
        try:
            if getattr(o, 'dps_win', None) and not getattr(o, '_dps_visible', False):
                o._dps_fade_seq += 1
                o._apply_webview2_transparency()
                o._set_plugin_surface_alpha('dps', 0.0)
                o.dps_win.show()
                self._eval_dps('if (window.DpsMeter && DpsMeter.fadeIn) DpsMeter.fadeIn()')
                threading.Timer(
                    0.03,
                    lambda: o._animate_window_alpha('SAO-DPS', 0.0, 1.0, duration_ms=220, steps=8),
                ).start()
                o._dps_visible = True
                self._ensure_dps_clickable()
                threading.Timer(0.5, self._ensure_dps_clickable).start()
                threading.Timer(1.5, self._ensure_dps_clickable).start()
        except Exception:
            pass

    def _hide_dps_window(self):
        o = self.owner
        o._dps_fade_seq += 1
        fade_seq = o._dps_fade_seq
        self._make_dps_unclickable()
        o._dps_detail_mode = False
        try:
            if getattr(o, 'dps_win', None) and getattr(o, '_dps_visible', False):
                self._eval_dps('if (window.DpsMeter && DpsMeter.setDetailMode) DpsMeter.setDetailMode(false)')
                try:
                    o.dps_win.resize(int(getattr(o, '_dps_base_w', 360) or 360),
                                     int(getattr(o, '_dps_base_h', 520) or 520))
                except Exception:
                    pass
                self._eval_dps('if (window.DpsMeter && DpsMeter.fadeOut) DpsMeter.fadeOut()')
                o._set_plugin_surface_alpha('dps', 1.0)
                threading.Timer(0.26, lambda: self._finish_hide_dps_window(fade_seq)).start()
        except Exception:
            pass
        o._dps_visible = False
        o._dps_faded = False
        o._dps_mode = 'hidden'

    def _finish_hide_dps_window(self, fade_seq: int):
        o = self.owner
        try:
            if fade_seq != o._dps_fade_seq or o._dps_visible:
                return
            o._set_plugin_surface_alpha('dps', 0.0)
        except Exception:
            pass

    def _show_dps_live_snapshot(self, snapshot=None):
        o = self.owner
        tracker = getattr(o, '_dps_tracker', None)
        if snapshot is None and tracker:
            try:
                snapshot = tracker.get_snapshot()
            except Exception:
                snapshot = None
        if snapshot is None:
            snapshot = {
                'encounter_active': False,
                'elapsed_s': 0.0,
                'total_damage': 0,
                'total_heal': 0,
                'total_dps': 0,
                'total_hps': 0,
                'entities': [],
            }
        self._show_dps_window()
        o._dps_mode = 'live'
        self._eval_dps(f'DpsMeter.showLive({json.dumps(snapshot, ensure_ascii=False)})')
        self._push_dps_act_snapshot()

    def _show_dps_last_report(self, report=None) -> bool:
        o = self.owner
        tracker = getattr(o, '_dps_tracker', None)
        if report is None and tracker:
            try:
                report = tracker.get_last_report()
            except Exception:
                report = None
        if not report:
            return False
        self._show_dps_window()
        o._dps_mode = 'report'
        self._eval_dps(f'DpsMeter.showLastReport({json.dumps(report, ensure_ascii=False)})')
        self._push_dps_act_snapshot()
        return True

    def _ensure_raid_editor_clickable(self):
        self.owner._set_plugin_surface_click_through('raid_editor', enabled=False)

    def _show_raid_editor(self):
        o = self.owner
        try:
            if getattr(o, 'raid_editor_win', None) and not getattr(o, '_raid_editor_visible', False):
                o._apply_webview2_transparency()
                o._set_plugin_surface_alpha('raid_editor', 0.0)
                o.raid_editor_win.show()
                self._eval_raid_editor('if(window.RaidEditor&&RaidEditor.fadeIn)RaidEditor.fadeIn()')
                threading.Timer(
                    0.03,
                    lambda: o._animate_window_alpha('SAO-RaidEditor', 0.0, 1.0, duration_ms=220, steps=8),
                ).start()
                o._raid_editor_visible = True
                self._ensure_raid_editor_clickable()
                threading.Timer(0.5, self._ensure_raid_editor_clickable).start()
                self._push_raid_editor_full()
        except Exception:
            pass

    def _hide_raid_editor(self):
        o = self.owner
        try:
            if getattr(o, 'raid_editor_win', None) and getattr(o, '_raid_editor_visible', False):
                self._eval_raid_editor('if(window.RaidEditor&&RaidEditor.fadeOut)RaidEditor.fadeOut()')

                def _finish():
                    try:
                        if getattr(o, 'raid_editor_win', None) and not o._raid_editor_visible:
                            o.raid_editor_win.hide()
                    except Exception:
                        pass

                threading.Timer(0.3, _finish).start()
        except Exception:
            pass
        o._raid_editor_visible = False

    def _push_raid_editor_entities(self, entities=None):
        o = self.owner
        if not getattr(o, '_raid_editor_visible', False):
            return
        try:
            if entities is None:
                engine = getattr(o, '_boss_raid_engine', None)
                entities = engine.get_entities() if engine else []
            self._eval_raid_editor(f'RaidEditor.updateEntities({json.dumps(entities, ensure_ascii=False)})')
        except Exception:
            pass

    def _push_raid_editor_status(self):
        o = self.owner
        if not getattr(o, '_raid_editor_visible', False):
            return
        try:
            engine = getattr(o, '_boss_raid_engine', None)
            if engine:
                try:
                    status = engine.get_status(include_entities=False)
                except TypeError:
                    status = engine.get_status()
                status.pop('entities', None)
                self._eval_raid_editor(f'RaidEditor.updateStatus({json.dumps(status, ensure_ascii=False)})')
        except Exception:
            pass

    def _push_raid_editor_full(self):
        o = self.owner
        if not getattr(o, '_raid_editor_visible', False):
            return
        try:
            engine = getattr(o, '_boss_raid_engine', None)
            if engine:
                payload = {
                    'status': engine.get_status(),
                    'phases': engine.get_profile_phases(),
                    'hotkeys': {'next_phase': o._resolved_hotkey('boss_raid_next_phase', 'F8')},
                }
                self._eval_raid_editor(f'RaidEditor.updateFull({json.dumps(payload, ensure_ascii=False)})')
        except Exception:
            pass

    def _on_raid_entity_update(self, entities):
        if getattr(self.owner, '_raid_editor_visible', False):
            self._push_raid_editor_entities(entities)

    def _on_boss_action_with_gate(self, action):
        o = self.owner
        if not bool(getattr(o, '_recognition_active', False)):
            return
        gs = getattr(o._state_mgr, 'state', None) if getattr(o, '_state_mgr', None) else None
        if gs is not None and bool(getattr(gs, 'self_dead', False)):
            return
        linkage = getattr(o, '_boss_autokey_linkage', None)
        if linkage is not None:
            try:
                linkage.on_boss_action(action)
            except Exception:
                pass

    def _ensure_commander_clickable(self):
        self.owner._set_plugin_surface_click_through('commander', enabled=False)

    def _show_commander(self):
        o = self.owner
        try:
            if getattr(o, 'commander_win', None) and not getattr(o, '_commander_visible', False):
                o._apply_webview2_transparency()
                o._set_plugin_surface_alpha('commander', 0.0)
                o.commander_win.show()
                self._eval_commander('if(window.Commander&&Commander.fadeIn)Commander.fadeIn()')
                threading.Timer(
                    0.03,
                    lambda: o._animate_window_alpha('SAO-Commander', 0.0, 1.0, duration_ms=220, steps=8),
                ).start()
                o._commander_visible = True
                self._ensure_commander_clickable()
                threading.Timer(0.5, self._ensure_commander_clickable).start()
                self._push_commander_data()
        except Exception:
            pass

    def _hide_commander(self):
        o = self.owner
        try:
            if getattr(o, 'commander_win', None) and getattr(o, '_commander_visible', False):
                self._eval_commander('if(window.Commander&&Commander.fadeOut)Commander.fadeOut()')

                def _finish():
                    try:
                        if getattr(o, 'commander_win', None):
                            o.commander_win.hide()
                            o._ensure_hidden_panels_passthrough()
                    except Exception:
                        pass

                threading.Timer(0.3, _finish).start()
        except Exception:
            pass
        o._commander_visible = False

    def _push_commander_data(self):
        o = self.owner
        if not getattr(o, '_commander_visible', False):
            return
        try:
            bridge = getattr(o, '_bridge', None)
            data = bridge.get_commander_data() if bridge else {
                'members': [], 'team_id': 0, 'leader_uid': 0,
                'dungeon_id': 0, 'status': 'backend_not_ready',
            }
            self._eval_commander(f'Commander.update({json.dumps(data, ensure_ascii=False)})')
        except Exception:
            pass

    def _ensure_autokey_editor_clickable(self):
        self.owner._set_plugin_surface_click_through('autokey_editor', enabled=False)

    def _show_autokey_editor(self):
        o = self.owner
        try:
            if getattr(o, 'autokey_editor_win', None) and not getattr(o, '_autokey_editor_visible', False):
                o._apply_webview2_transparency()
                o._set_plugin_surface_alpha('autokey_editor', 0.0)
                o.autokey_editor_win.show()
                self._eval_autokey_editor('if(window.AutoKeyEditor&&AutoKeyEditor.fadeIn)AutoKeyEditor.fadeIn()')
                threading.Timer(
                    0.03,
                    lambda: o._animate_window_alpha('SAO-AutoKeyEditor', 0.0, 1.0, duration_ms=220, steps=8),
                ).start()
                o._autokey_editor_visible = True
                self._ensure_autokey_editor_clickable()
                threading.Timer(0.5, self._ensure_autokey_editor_clickable).start()
                self._push_autokey_editor_state()
                actions = o._get_setting('autokey_burst_actions', [])
                self._eval_autokey_editor(f'AutoKeyEditor.loadActions({json.dumps(actions, ensure_ascii=False)})')
        except Exception:
            pass

    def _hide_autokey_editor(self):
        o = self.owner
        try:
            if getattr(o, 'autokey_editor_win', None) and getattr(o, '_autokey_editor_visible', False):
                self._eval_autokey_editor('if(window.AutoKeyEditor&&AutoKeyEditor.fadeOut)AutoKeyEditor.fadeOut()')

                def _finish():
                    try:
                        if getattr(o, 'autokey_editor_win', None) and not o._autokey_editor_visible:
                            o.autokey_editor_win.hide()
                    except Exception:
                        pass

                threading.Timer(0.3, _finish).start()
        except Exception:
            pass
        o._autokey_editor_visible = False

    def _push_autokey_editor_slots(self, skill_slots=None):
        o = self.owner
        if not getattr(o, '_autokey_editor_visible', False):
            return
        try:
            if skill_slots is None:
                gs = getattr(o, '_game_state', None)
                skill_slots = getattr(gs, 'skill_slots', []) if gs else []
            slots_data = []
            for slot in skill_slots:
                if isinstance(slot, dict):
                    slots_data.append(slot)
                else:
                    slots_data.append({
                        'slot_index': getattr(slot, 'slot_index', 0),
                        'skill_id': getattr(slot, 'skill_id', 0),
                        'skill_name': getattr(slot, 'skill_name', ''),
                        'state': getattr(slot, 'state', 'unknown'),
                        'cooldown_pct': getattr(slot, 'cooldown_pct', 0),
                        'remaining_ms': getattr(slot, 'remaining_ms', 0),
                        'total_cd_ms': getattr(slot, 'total_cd_ms', 0),
                        'charge_count': getattr(slot, 'charge_count', 0),
                        'max_charges': getattr(slot, 'max_charges', 1),
                    })
            self._eval_autokey_editor(f'AutoKeyEditor.updateSlots({json.dumps(slots_data, ensure_ascii=False)})')
        except Exception:
            pass

    def _push_autokey_editor_state(self):
        o = self.owner
        if not getattr(o, '_autokey_editor_visible', False):
            return
        try:
            gs = getattr(o, '_game_state', None)
            state = {
                'burst_ready': bool(getattr(gs, 'burst_ready', False)) if gs else False,
                'profession': getattr(o, '_profession', '') or '',
            }
            self._eval_autokey_editor(f'AutoKeyEditor.updateState({json.dumps(state, ensure_ascii=False)})')
        except Exception:
            pass

    def _ensure_skillfx_on_top(self):
        self.owner._ensure_plugin_surface_on_top('skillfx')

    def _setup_skillfx_click_through(self, _wait_retries: int = 20):
        self.owner._set_plugin_surface_click_through(
            'skillfx', enabled=True, wait_retries=_wait_retries, ensure_on_top=True)

    def _setup_boss_hp_click_through(self, _wait_retries: int = 20):
        self.owner._set_plugin_surface_click_through(
            'boss_hp', enabled=True, wait_retries=_wait_retries, ensure_on_top=True)

    def _ensure_boss_hp_on_top(self):
        self.owner._ensure_plugin_surface_on_top('boss_hp')

    def _get_skillfx_layout(self, gs=None):
        o = self.owner
        if gs is None and hasattr(o, '_state_mgr'):
            gs = o._state_mgr.state
        client_rect = getattr(gs, 'window_rect', None) if gs else None
        if not client_rect:
            try:
                from plugins.star_resonance_plugin.windowing import get_window_rect
                client_rect = get_window_rect()
            except Exception:
                client_rect = None
        if not client_rect:
            return None
        client_left, client_top, client_right, client_bottom = client_rect
        client_w = max(1, int(client_right - client_left))
        client_h = max(1, int(client_bottom - client_top))
        slots = []
        for slot in list(getattr(gs, 'skill_slots', []) or []) if gs else []:
            if not isinstance(slot, dict):
                continue
            rect = slot.get('rect') or {}
            try:
                sx = int(rect.get('x', 0))
                sy = int(rect.get('y', 0))
                sw = int(rect.get('w', 0))
                sh = int(rect.get('h', 0))
                idx = int(slot.get('index', 0) or 0)
            except Exception:
                continue
            if idx <= 0 or sw <= 0 or sh <= 0:
                continue
            slots.append({
                'index': idx,
                'screen_rect': {'x': client_left + sx, 'y': client_top + sy, 'w': sw, 'h': sh},
                'client_rect': {'x': sx, 'y': sy, 'w': sw, 'h': sh},
            })
        if not slots:
            for item in get_skill_slot_rects(client_rect):
                left, top, right, bottom = item['bbox']
                slots.append({
                    'index': int(item['index']),
                    'screen_rect': {'x': left, 'y': top, 'w': right - left, 'h': bottom - top},
                    'client_rect': {'x': left - client_left, 'y': top - client_top,
                                    'w': right - left, 'h': bottom - top},
                })
        if not slots:
            return None
        min_x = min(item['screen_rect']['x'] for item in slots)
        max_y = max(item['screen_rect']['y'] + item['screen_rect']['h'] for item in slots)
        pad_x = max(18, int(round(client_w * 0.012)))
        pad_y = max(18, int(round(client_h * 0.016)))
        pad_left = max(96, int(round(client_w * 0.055)))
        pad_right = max(84, int(round(client_w * 0.044)))
        win_x = max(0, min_x - pad_left)
        win_y = max(0, client_top)
        width = max(420, int((client_right - win_x) + pad_right))
        height = max(220, int((max_y - win_y) + pad_y))
        callout_w = max(440, int(round(client_w * 0.29)))
        callout_h = max(128, int(round(client_h * 0.115)))
        callout_margin_x = max(28, int(round(client_w * 0.022)))
        callout_margin_y = max(24, int(round(client_h * 0.040)))
        callout_x = max(callout_margin_x, width - callout_w - callout_margin_x)
        callout_y = callout_margin_y
        payload_slots = []
        for item in slots:
            rect = item['screen_rect']
            payload_slots.append({
                'index': item['index'],
                'rect': {'x': rect['x'] - win_x, 'y': rect['y'] - win_y,
                         'w': rect['w'], 'h': rect['h']},
                'client_rect': dict(item['client_rect']),
            })
        payload_slots.sort(key=lambda item: item['index'])
        return {
            'window': {'x': int(win_x), 'y': int(win_y), 'w': int(width), 'h': int(height)},
            'viewport': {
                'width': int(width),
                'height': int(height),
                'padding_x': int(max(pad_x, pad_left, pad_right)),
                'padding_y': int(pad_y),
                'callout': {'x': int(callout_x), 'y': int(callout_y),
                            'w': int(callout_w), 'h': int(callout_h)},
            },
            'slots': payload_slots,
            'client_rect': tuple(client_rect),
        }

    def _build_skillfx_payload(self, gs):
        o = self.owner
        layout = self._get_skillfx_layout(gs)
        if not layout:
            return None
        o._skillfx_layout = layout
        slot_map = {}
        for slot in getattr(gs, 'skill_slots', []) or []:
            if isinstance(slot, dict):
                try:
                    slot_map[int(slot.get('index', 0) or 0)] = slot
                except Exception:
                    pass
        payload_slots = []
        for slot_layout in layout['slots']:
            slot = slot_map.get(slot_layout['index'], {})
            payload_slots.append({
                'index': slot_layout['index'],
                'rect': o._rect_to_css(slot_layout['rect']),
                'state': str(slot.get('state', 'unknown') or 'unknown'),
                'cooldown_ratio': float(slot.get('cooldown_ratio', slot.get('cooldown_pct', 0.0)) or 0.0),
                'insufficient_energy': bool(slot.get('insufficient_energy')),
                'ready_edge': bool(slot.get('ready_edge')),
                'active': bool(slot.get('active')),
            })
        return {
            'viewport': o._viewport_to_css(layout['viewport']),
            'slots': payload_slots,
            'watched_slots': o._get_setting('watched_skill_slots', [1, 2, 3, 4, 5, 6, 7, 8, 9]),
            'burst_enabled': bool(o._get_setting('burst_enabled', True)),
        }

    def _update_skillfx_layout(self):
        o = self.owner
        if not getattr(o, 'skillfx_win', None):
            return
        layout = self._get_skillfx_layout(getattr(o, '_game_state', None))
        if not layout:
            return
        o._skillfx_layout = layout
        window = layout['window']
        try:
            o.skillfx_win.resize(int(max(1, o._to_webview_px(window['w']))),
                                 int(max(1, o._to_webview_px(window['h']))))
        except Exception:
            pass
        try:
            o.skillfx_win.move(int(o._to_webview_px(window['x'])),
                               int(o._to_webview_px(window['y'])))
        except Exception:
            pass
        css_vp = o._viewport_to_css(layout['viewport'])
        self._eval_skillfx(f'SkillFX.setViewport({json.dumps(css_vp, ensure_ascii=False)})')
        self._setup_skillfx_click_through()

    def resolve_dungeon_name(self, dungeon_id: int) -> str:
        try:
            from plugins.star_resonance_plugin.tools.tablekit.name_tables import names
            return names.dungeon(int(dungeon_id or 0), default='') or ''
        except Exception:
            return ''

    # ── WebView API: state/settings ──
    def get_state(self):
        gs = getattr(self.owner, '_game_state', None)
        try:
            sta_pct = int(round(max(0.0, min(1.0, float(getattr(gs, 'stamina_pct', 0.0) or 0.0))) * 100.0))
        except Exception:
            sta_pct = 0
        return json.dumps({
            'recognition_active': getattr(self.owner, '_recognition_active', False),
            'hp': gs.hp_current if gs and hasattr(gs, 'hp_current') else 0,
            'hp_max': gs.hp_max if gs and hasattr(gs, 'hp_max') else 0,
            'stamina': sta_pct,
            'stamina_max': 100,
            'level': gs.level_base if gs and hasattr(gs, 'level_base') else 0,
        }, ensure_ascii=False)

    def set_watched_slots(self, slots):
        if isinstance(slots, str):
            try:
                slots = json.loads(slots)
            except Exception:
                slots = []
        normalized = []
        seen = set()
        for raw in slots if isinstance(slots, (list, tuple, set)) else []:
            try:
                slot = int(float(raw))
            except Exception:
                continue
            if 1 <= slot <= 9 and slot not in seen:
                seen.add(slot)
                normalized.append(slot)
        try:
            self.owner._set_setting('watched_skill_slots', normalized)
            self._reset_burst_tracking()
            return json.dumps({'ok': True, 'slots': normalized}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc, slots=normalized)

    def set_burst_enabled(self, enabled):
        state = bool(enabled)
        if isinstance(enabled, str):
            state = enabled.strip().lower() not in {'0', 'false', 'off', 'no', 'disabled'}
        try:
            self.owner._set_setting('burst_enabled', state)
            return json.dumps({'ok': True, 'enabled': state}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc, enabled=state)

    # ── WebView API: AutoKey ──
    def set_auto_key_enabled(self, enabled):
        try:
            config = self._load_auto_key_config()
            config['enabled'] = bool(enabled)
            self._save_auto_key_config(config)
            self._sync_auto_key_menu()
            return json.dumps({'ok': True, 'state': self._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def get_auto_key_state(self):
        try:
            return json.dumps({'ok': True, 'state': self._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def create_auto_key_profile(self):
        try:
            config = self._load_auto_key_config()
            upsert_profile(config, make_default_profile(self._auto_key_author_snapshot()), activate=True)
            self._save_auto_key_config(config)
            self._sync_auto_key_menu()
            return json.dumps({'ok': True, 'state': self._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def copy_auto_key_profile(self, profile_id):
        try:
            config = self._load_auto_key_config()
            created = clone_profile(config, profile_id, self._auto_key_author_snapshot())
            if not created:
                raise RuntimeError('Profile not found')
            self._save_auto_key_config(config)
            self._sync_auto_key_menu()
            return json.dumps({'ok': True, 'state': self._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def save_auto_key_profile(self, profile_payload):
        try:
            if isinstance(profile_payload, str):
                profile_payload = json.loads(profile_payload)
            config = self._load_auto_key_config()
            profile = normalize_profile(profile_payload, author_snapshot=self._auto_key_author_snapshot())
            existing = find_auto_key_profile(config, profile.get('id'))
            if existing and existing.get('created_at'):
                profile['created_at'] = existing.get('created_at')
            upsert_profile(config, profile, activate=str(config.get('active_profile_id') or '') == str(profile.get('id') or ''))
            self._save_auto_key_config(config)
            self._sync_auto_key_menu()
            return json.dumps({'ok': True, 'state': self._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def delete_auto_key_profile(self, profile_id):
        try:
            config = self._load_auto_key_config()
            delete_auto_key_profile(config, profile_id)
            self._save_auto_key_config(config)
            self._sync_auto_key_menu()
            return json.dumps({'ok': True, 'state': self._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def activate_auto_key_profile(self, profile_id):
        try:
            config = self._load_auto_key_config()
            if not find_auto_key_profile(config, profile_id):
                raise RuntimeError('Profile not found')
            config['active_profile_id'] = str(profile_id or '')
            self._save_auto_key_config(config)
            self._sync_auto_key_menu()
            return json.dumps({'ok': True, 'state': self._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def export_auto_key_profile(self, profile_id=None):
        try:
            config = self._load_auto_key_config()
            profile = find_auto_key_profile(config, profile_id) if profile_id else None
            profile = profile or find_auto_key_profile(config, config.get('active_profile_id'))
            if profile is None:
                raise RuntimeError('No active profile')
            return json.dumps({'ok': True, 'path': export_profile_to_default_path(profile)}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def start_auto_key_import_picker(self, path=None):
        try:
            root = str(path or (os.path.dirname(sys.executable) if getattr(sys, 'frozen', False) else os.path.dirname(os.path.abspath(__file__))))
            self.owner._auto_key_picker_purpose = 'auto_key_import'
            return json.dumps({'ok': True, 'browser': self._browse(root)}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def select_file(self, path):
        try:
            if getattr(self.owner, '_auto_key_picker_purpose', '') == 'auto_key_import':
                config = self._load_auto_key_config()
                upsert_profile(config, import_profile_from_path(str(path), self._auto_key_author_snapshot()), activate=False)
                self._save_auto_key_config(config)
                self.owner._auto_key_picker_purpose = ''
                self._sync_auto_key_menu()
                return json.dumps({'ok': True, 'state': self._get_auto_key_menu_state()}, ensure_ascii=False)
            if getattr(self.owner, '_boss_raid_picker_purpose', '') == 'boss_raid_import':
                config = self._load_boss_raid_config()
                upsert_br_profile(config, import_br_profile_path(str(path), self._boss_raid_author_snapshot()), activate=False)
                self._save_boss_raid_config(config)
                self.owner._boss_raid_picker_purpose = ''
                self._sync_boss_raid_menu()
                return json.dumps({'ok': True, 'state': self._get_boss_raid_menu_state()}, ensure_ascii=False)
            raise RuntimeError('No file picker action pending')
        except Exception as exc:
            return self._json_error(exc)

    def select_folder(self, path):
        return json.dumps({'ok': False, 'message': 'Folder selection not used here'}, ensure_ascii=False)

    def set_auto_key_upload_token(self, token):
        try:
            config = self._load_auto_key_config()
            self._set_auto_key_upload_auth(token=str(token or '').strip(), mode='manual',
                                           identity=self._auto_key_identity_state(),
                                           server_url=str(config.get('server_url') or DEFAULT_AUTO_KEY_SERVER_URL))
            self._sync_auto_key_menu()
            return json.dumps({'ok': True, 'state': self._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def set_auto_key_server_url(self, url):
        try:
            config = self._load_auto_key_config()
            config['server_url'] = str(url or '').strip()
            self._save_auto_key_config(config)
            self._set_auto_key_upload_auth(identity=self._auto_key_identity_state(),
                                           server_url=str(config.get('server_url') or DEFAULT_AUTO_KEY_SERVER_URL))
            self._sync_auto_key_menu()
            return json.dumps({'ok': True, 'state': self._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def refresh_auto_key_upload_auth(self, force=False):
        try:
            auth = self._refresh_auto_key_upload_auth(force=bool(force))
            self._sync_auto_key_menu()
            return json.dumps({'ok': bool(auth.get('ready')), 'message': str(auth.get('error') or ''),
                               'upload_auth': auth, 'state': self._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc, state=self._get_auto_key_menu_state())

    def search_remote_profiles(self, query_payload):
        try:
            if isinstance(query_payload, str):
                query_payload = json.loads(query_payload or '{}')
            config = self._load_auto_key_config()
            client = AutoKeyCloudClient(config.get('server_url') or DEFAULT_AUTO_KEY_SERVER_URL)
            query = {k: str((query_payload or {}).get(k, '') or '').strip()
                     for k in ('q', 'profile_name', 'player_uid', 'player_name', 'profession_name')}
            query['page'] = int((query_payload or {}).get('page', 1) or 1)
            query['page_size'] = int((query_payload or {}).get('page_size', 20) or 20)
            result = client.search_scripts(query)
            config['last_remote_search'] = {'query': query, 'results': result.get('items', []), 'error': '',
                                            'fetched_at': time.strftime('%Y-%m-%d %H:%M:%S', time.localtime())}
            self._save_auto_key_config(config)
            self._sync_auto_key_menu()
            return json.dumps({'ok': True, 'results': result.get('items', []), 'state': self._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc, state=self._get_auto_key_menu_state())

    def download_remote_profile(self, remote_id):
        try:
            config = self._load_auto_key_config()
            result = AutoKeyCloudClient(config.get('server_url') or DEFAULT_AUTO_KEY_SERVER_URL).get_script(remote_id)
            profile = normalize_profile(result.get('profile') or {}, author_snapshot=self._auto_key_author_snapshot(), source='downloaded')
            if not profile.get('id') or profile.get('id') in {item.get('id') for item in config.get('profiles', []) or []}:
                profile['id'] = f'profile_{int(time.time())}'
            profile['remote_id'] = result.get('id')
            profile['source'] = 'downloaded'
            profile['updated_at'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
            upsert_profile(config, profile, activate=False)
            self._save_auto_key_config(config)
            self._sync_auto_key_menu()
            return json.dumps({'ok': True, 'state': self._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def upload_auto_key_profile(self, profile_id=None):
        try:
            config = self._load_auto_key_config()
            profile = find_auto_key_profile(config, profile_id) if profile_id else None
            profile = profile or find_auto_key_profile(config, config.get('active_profile_id'))
            if profile is None:
                raise RuntimeError('No active profile')
            auth = self._refresh_auto_key_upload_auth(force=False)
            token = str((auth or {}).get('token') or '').strip()
            if not token:
                raise RuntimeError(str((auth or {}).get('error') or 'Upload token is empty'))
            author = self._auto_key_author_snapshot()
            payload = {
                'profile_name': profile.get('profile_name', ''),
                'description': profile.get('description', ''),
                'profession_id': author.get('profession_id') or profile.get('profession_id', 0),
                'profession_name': author.get('profession_name') or profile.get('profession_name', ''),
                'player_uid': author.get('player_uid', ''),
                'player_name': author.get('player_name', ''),
                'schema_version': 1,
                'profile': profile,
            }
            result = AutoKeyCloudClient(config.get('server_url') or DEFAULT_AUTO_KEY_SERVER_URL).upload_script(payload, token)
            profile['source'] = 'uploaded'
            profile['remote_id'] = result.get('id')
            upsert_profile(config, profile, activate=str(config.get('active_profile_id') or '') == str(profile.get('id') or ''))
            self._save_auto_key_config(config)
            self._sync_auto_key_menu()
            return json.dumps({'ok': True, 'remote_id': result.get('id'), 'state': self._get_auto_key_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc, state=self._get_auto_key_menu_state())

    # ── WebView API: BossRaid ──
    def get_boss_raid_state(self):
        try:
            return json.dumps({'ok': True, 'state': self._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def set_boss_raid_enabled(self, enabled):
        try:
            config = self._load_boss_raid_config()
            config['enabled'] = bool(enabled)
            self._save_boss_raid_config(config)
            self._sync_boss_raid_menu()
            return json.dumps({'ok': True, 'state': self._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def set_boss_raid_server_url(self, url):
        try:
            config = self._load_boss_raid_config()
            config['server_url'] = str(url or '').strip()
            self._set_boss_raid_upload_auth(identity=self._boss_raid_identity_state(),
                                            server_url=str(config.get('server_url') or DEFAULT_BOSS_RAID_SERVER_URL))
            self._save_boss_raid_config(config)
            self._sync_boss_raid_menu()
            return json.dumps({'ok': True, 'state': self._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def activate_boss_raid_profile(self, profile_id):
        try:
            config = self._load_boss_raid_config()
            if not find_br_profile(config, profile_id):
                raise RuntimeError('Profile not found')
            config['active_profile_id'] = str(profile_id or '')
            self._save_boss_raid_config(config)
            self._sync_boss_raid_menu()
            return json.dumps({'ok': True, 'state': self._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def create_boss_raid_profile(self):
        try:
            config = self._load_boss_raid_config()
            upsert_br_profile(config, make_default_br_profile(self._boss_raid_author_snapshot()), activate=True)
            self._save_boss_raid_config(config)
            self._sync_boss_raid_menu()
            return json.dumps({'ok': True, 'state': self._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def save_boss_raid_profile(self, profile_payload):
        try:
            if isinstance(profile_payload, str):
                profile_payload = json.loads(profile_payload)
            config = self._load_boss_raid_config()
            profile = normalize_br_profile(profile_payload, author_snapshot=self._boss_raid_author_snapshot())
            existing = find_br_profile(config, profile.get('id'))
            if existing and existing.get('created_at'):
                profile['created_at'] = existing.get('created_at')
            upsert_br_profile(config, profile, activate=str(config.get('active_profile_id') or '') == str(profile.get('id') or ''))
            self._save_boss_raid_config(config)
            self._sync_boss_raid_menu()
            return json.dumps({'ok': True, 'state': self._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def delete_boss_raid_profile(self, profile_id):
        try:
            config = self._load_boss_raid_config()
            delete_br_profile(config, profile_id)
            self._save_boss_raid_config(config)
            self._sync_boss_raid_menu()
            return json.dumps({'ok': True, 'state': self._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def export_boss_raid_profile(self, profile_id=None):
        try:
            config = self._load_boss_raid_config()
            profile = find_br_profile(config, profile_id) if profile_id else None
            profile = profile or find_br_profile(config, config.get('active_profile_id'))
            if profile is None:
                raise RuntimeError('No active profile')
            return json.dumps({'ok': True, 'path': export_br_profile_path(profile)}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def start_boss_raid_import_picker(self, path=None):
        try:
            root = str(path or (os.path.dirname(sys.executable) if getattr(sys, 'frozen', False) else os.path.dirname(os.path.abspath(__file__))))
            self.owner._boss_raid_picker_purpose = 'boss_raid_import'
            return json.dumps({'ok': True, 'browser': self._browse(root)}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def boss_raid_start(self):
        try:
            self._toggle_boss_raid()
            return json.dumps({'ok': True, 'state': self._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def boss_raid_stop(self):
        try:
            engine = getattr(self.owner, '_boss_raid_engine', None)
            if engine:
                engine.stop()
            self._sync_boss_raid_menu()
            return json.dumps({'ok': True, 'state': self._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def boss_raid_next_phase(self):
        try:
            self._boss_raid_next_phase()
            return json.dumps({'ok': True, 'state': self._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def boss_raid_reset(self):
        try:
            engine = getattr(self.owner, '_boss_raid_engine', None)
            if engine:
                engine.reset()
            self._sync_boss_raid_menu()
            return json.dumps({'ok': True, 'state': self._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def search_boss_raid_remote(self, query_payload):
        try:
            if isinstance(query_payload, str):
                query_payload = json.loads(query_payload or '{}')
            config = self._load_boss_raid_config()
            query = {k: str((query_payload or {}).get(k, '') or '').strip()
                     for k in ('q', 'profile_name', 'player_uid', 'player_name')}
            query['page'] = int((query_payload or {}).get('page', 1) or 1)
            query['page_size'] = int((query_payload or {}).get('page_size', 20) or 20)
            result = BossRaidCloudClient(config.get('server_url') or DEFAULT_BOSS_RAID_SERVER_URL).search(query)
            return json.dumps({'ok': True, 'results': result.get('items', []), 'state': self._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc, state=self._get_boss_raid_menu_state())

    def download_boss_raid_remote(self, remote_id):
        try:
            config = self._load_boss_raid_config()
            result = BossRaidCloudClient(config.get('server_url') or DEFAULT_BOSS_RAID_SERVER_URL).get(remote_id)
            profile = normalize_br_profile(result.get('profile') or {}, author_snapshot=self._boss_raid_author_snapshot(), source='downloaded')
            if not profile.get('id') or profile.get('id') in {item.get('id') for item in config.get('profiles', []) or []}:
                import uuid
                profile['id'] = f'boss_{uuid.uuid4().hex[:12]}'
            profile['remote_id'] = result.get('id')
            profile['source'] = 'downloaded'
            profile['updated_at'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
            upsert_br_profile(config, profile, activate=False)
            self._save_boss_raid_config(config)
            self._sync_boss_raid_menu()
            return json.dumps({'ok': True, 'state': self._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def upload_boss_raid_profile(self, profile_id=None):
        try:
            config = self._load_boss_raid_config()
            profile = find_br_profile(config, profile_id) if profile_id else None
            profile = profile or find_br_profile(config, config.get('active_profile_id'))
            if profile is None:
                raise RuntimeError('No active profile')
            auth = self._refresh_boss_raid_upload_auth(force=False)
            token = str((auth or {}).get('token') or '').strip()
            if not token:
                raise RuntimeError(str((auth or {}).get('error') or 'Upload token is empty'))
            author = self._boss_raid_author_snapshot()
            payload = {
                'profile_name': profile.get('profile_name', ''),
                'description': profile.get('description', ''),
                'boss_total_hp': int(profile.get('boss_total_hp') or 0),
                'enrage_time_s': int(profile.get('enrage_time_s') or 0),
                'player_uid': author.get('player_uid', ''),
                'player_name': author.get('player_name', ''),
                'schema_version': 1,
                'profile': profile,
            }
            result = BossRaidCloudClient(config.get('server_url') or DEFAULT_BOSS_RAID_SERVER_URL).upload(payload, token)
            profile['source'] = 'uploaded'
            profile['remote_id'] = result.get('id')
            upsert_br_profile(config, profile, activate=str(config.get('active_profile_id') or '') == str(profile.get('id') or ''))
            self._save_boss_raid_config(config)
            self._sync_boss_raid_menu()
            return json.dumps({'ok': True, 'remote_id': result.get('id'), 'state': self._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc, state=self._get_boss_raid_menu_state())

    def refresh_boss_raid_upload_auth(self, force=False):
        try:
            auth = self._refresh_boss_raid_upload_auth(force=bool(force))
            self._sync_boss_raid_menu()
            return json.dumps({'ok': bool(auth.get('ready')), 'message': str(auth.get('error') or ''),
                               'upload_auth': auth, 'state': self._get_boss_raid_menu_state()}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc, state=self._get_boss_raid_menu_state())

    # ── WebView API: linkage/reactions/mechanics ──
    def get_linkage_state(self):
        try:
            config = load_linkage_config(self.owner._cfg_settings_ref)
            linkage = getattr(self.owner, '_boss_autokey_linkage', None)
            status = linkage.get_status() if linkage else {}
            return json.dumps({'ok': True, 'state': build_linkage_state(config, status)}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def set_linkage_enabled(self, enabled):
        return self._save_linkage_value('enabled', bool(enabled))

    def set_linkage_debug(self, enabled):
        return self._save_linkage_value('debug_log', bool(enabled))

    def set_linkage_global_cooldown(self, seconds):
        try:
            return self._save_linkage_value('global_cooldown_s', max(0.0, min(60.0, float(seconds))))
        except Exception as exc:
            return self._json_error(exc)

    def _save_linkage_value(self, key, value):
        try:
            config = load_linkage_config(self.owner._cfg_settings_ref)
            config[key] = value
            save_linkage_config(self.owner._cfg_settings_ref, config)
            return json.dumps({'ok': True}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def save_linkage_mappings(self, mappings_json):
        try:
            mappings = json.loads(mappings_json) if isinstance(mappings_json, str) else mappings_json
            config = load_linkage_config(self.owner._cfg_settings_ref)
            config['mappings'] = list(mappings) if isinstance(mappings, list) else []
            save_linkage_config(self.owner._cfg_settings_ref, config)
            return json.dumps({'ok': True}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def reset_linkage(self):
        try:
            linkage = getattr(self.owner, '_boss_autokey_linkage', None)
            if linkage:
                linkage.reset()
            return json.dumps({'ok': True}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def _boss_reactions_state(self, scene_key=None, boss_base_id=None):
        return build_boss_reactions_state(
            self.owner._cfg_settings_ref,
            getattr(self.owner, '_boss_raid_engine', None),
            getattr(self.owner, '_state_mgr', None),
            scene_key=scene_key,
            boss_base_id=boss_base_id)

    def get_boss_reactions(self, scene_key=None, boss_base_id=None):
        try:
            return json.dumps({'ok': True, 'state': self._boss_reactions_state(scene_key, boss_base_id)}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def save_boss_reaction(self, mapping_json, scene_key=None, boss_base_id=None):
        try:
            upsert_mapping(self.owner._cfg_settings_ref, json.loads(mapping_json) if isinstance(mapping_json, str) else mapping_json)
            return json.dumps({'ok': True, 'state': self._boss_reactions_state(scene_key, boss_base_id)}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def delete_boss_reaction(self, mapping_id, scene_key=None, boss_base_id=None):
        try:
            delete_mapping(self.owner._cfg_settings_ref, str(mapping_id))
            return json.dumps({'ok': True, 'state': self._boss_reactions_state(scene_key, boss_base_id)}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def import_observed_skills(self, base_id=0, scene_key=None):
        try:
            engine = getattr(self.owner, '_boss_raid_engine', None)
            obs = engine.get_observed_boss_skills(int(base_id) or None, scene_key) if engine else {}
            return json.dumps({'ok': True, 'observed_skills': obs}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def _mechanics_state(self, scene_key=None, boss_base_id=None):
        from plugins.star_resonance_plugin.engines import boss_mechanics_state as bms
        return bms.build_mechanics_state(
            self.owner._cfg_settings_ref,
            getattr(self.owner, '_boss_raid_engine', None),
            getattr(self.owner, '_state_mgr', None),
            scene_key=scene_key,
            boss_base_id=boss_base_id)

    def _mechanics_reply(self, scene_key=None, boss_base_id=None):
        return json.dumps({'ok': True, 'state': self._mechanics_state(scene_key, boss_base_id)}, ensure_ascii=False)

    def get_mechanics(self, scene_key=None, boss_base_id=None):
        try:
            return self._mechanics_reply(scene_key, boss_base_id)
        except Exception as exc:
            return self._json_error(exc)

    def _mechanics_hot_apply(self):
        try:
            from plugins.star_resonance_plugin.engines import boss_mechanics_state as bms
            bms.hot_apply_to_engine(self.owner._cfg_settings_ref, getattr(self.owner, '_boss_raid_engine', None))
        except Exception:
            pass

    def save_mechanic(self, mech_json, scene_key=None, boss_base_id=None):
        try:
            from plugins.star_resonance_plugin.engines import boss_mechanics_state as bms
            bms.upsert_mechanic(self.owner._cfg_settings_ref, json.loads(mech_json) if isinstance(mech_json, str) else mech_json)
            try:
                self._presynthesize_active_profile_web()
            except Exception:
                pass
            self._mechanics_hot_apply()
            return self._mechanics_reply(scene_key, boss_base_id)
        except Exception as exc:
            return self._json_error(exc)

    def delete_mechanic(self, mechanic_id, scene_key=None, boss_base_id=None):
        try:
            from plugins.star_resonance_plugin.engines import boss_mechanics_state as bms
            bms.delete_mechanic(self.owner._cfg_settings_ref, str(mechanic_id))
            self._mechanics_hot_apply()
            return self._mechanics_reply(scene_key, boss_base_id)
        except Exception as exc:
            return self._json_error(exc)

    def create_mechanic_from_skill(self, skill_id, skill_name='', cast_duration_ms=None,
                                   scene_key=None, boss_base_id=None):
        try:
            from plugins.star_resonance_plugin.engines import boss_mechanics_state as bms
            bms.create_mechanic_from_skill(self.owner._cfg_settings_ref, skill_id, skill_name, cast_duration_ms)
            self._mechanics_hot_apply()
            return self._mechanics_reply(scene_key, boss_base_id)
        except Exception as exc:
            return self._json_error(exc)

    def bind_mechanic_skill(self, mechanic_id, skill_id, scene_key=None, boss_base_id=None):
        try:
            from plugins.star_resonance_plugin.engines import boss_mechanics_state as bms
            bms.bind_skill_to_mechanic(self.owner._cfg_settings_ref, mechanic_id, skill_id)
            self._mechanics_hot_apply()
            return self._mechanics_reply(scene_key, boss_base_id)
        except Exception as exc:
            return self._json_error(exc)

    def unbind_mechanic_skill(self, mechanic_id, skill_id, scene_key=None, boss_base_id=None):
        try:
            from plugins.star_resonance_plugin.engines import boss_mechanics_state as bms
            bms.unbind_skill_from_mechanic(self.owner._cfg_settings_ref, mechanic_id, skill_id)
            self._mechanics_hot_apply()
            return self._mechanics_reply(scene_key, boss_base_id)
        except Exception as exc:
            return self._json_error(exc)

    def set_mechanics_master(self, flags_json):
        try:
            from plugins.star_resonance_plugin.engines import boss_mechanics_state as bms
            flags = json.loads(flags_json) if isinstance(flags_json, str) else flags_json
            return json.dumps({'ok': True, 'master': bms.set_mechanics_master(self.owner._cfg_settings_ref, flags)}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def search_skill_catalog(self, query, limit=30):
        try:
            from plugins.star_resonance_plugin.engines import boss_mechanics_state as bms
            return json.dumps({'ok': True, 'results': bms.search_skill_catalog(query, int(limit or 30))}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def test_mechanic(self, mech_json, kinds_json='["tts","banner"]'):
        try:
            mech = json.loads(mech_json) if isinstance(mech_json, str) else mech_json
            kinds = json.loads(kinds_json) if isinstance(kinds_json, str) else (kinds_json or [])
            kinds = [str(k) for k in kinds]
            result = {'ok': True}
            msgs = []
            tts_fb = {'played': 'TTS已播', 'queued': 'TTS排队中', 'synthesizing': 'TTS合成中(首次稍候再试)',
                      'disabled': 'TTS未启用或静音', 'error': 'TTS失败'}
            controller = getattr(self.owner, '_mech_alert_controller', None)
            if {'tts', 'banner'} & set(kinds):
                if controller is None:
                    msgs.append('提醒控制器未就绪')
                else:
                    result.update(controller.test_mechanic(mech, kinds))
                    if 'tts' in kinds:
                        msgs.append(tts_fb.get(result.get('tts'), str(result.get('tts') or 'TTS无结果')))
                    if 'banner' in kinds:
                        msgs.append('横幅已显示' if result.get('banner') else '横幅未显示')
            if 'dodge' in kinds:
                linkage = getattr(self.owner, '_boss_autokey_linkage', None)
                inline = ((mech.get('dodge') or {}).get('inline') if isinstance(mech, dict) else None)
                if linkage is None or not isinstance(inline, dict):
                    msgs.append('按键联动不可用')
                else:
                    fired = bool(linkage.fire_mapping_test(inline))
                    result['dodge'] = fired
                    msgs.append('按键已发到当前前台窗口' if fired else '按键未配置(无键/无序列)')
            result['message'] = ' · '.join(msgs)
            return json.dumps(result, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    # ── WebView API: editor window commands ──
    def set_entity_role(self, uuid, role):
        try:
            engine = getattr(self.owner, '_boss_raid_engine', None)
            if engine:
                engine.set_entity_role(int(uuid), str(role))
            return json.dumps({'ok': True}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def raid_next_phase(self):
        try:
            engine = getattr(self.owner, '_boss_raid_engine', None)
            if engine:
                engine.next_phase()
            return json.dumps({'ok': True}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def raid_reset(self):
        try:
            engine = getattr(self.owner, '_boss_raid_engine', None)
            if engine:
                engine.reset()
                self.owner._push_raid_editor_full()
            return json.dumps({'ok': True}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def save_autokey_actions(self, actions_json):
        try:
            actions = json.loads(actions_json) if isinstance(actions_json, str) else actions_json
            engine = getattr(self.owner, '_auto_key_engine', None)
            if engine and hasattr(engine, 'set_burst_actions'):
                engine.set_burst_actions(actions)
            self.owner._set_setting('autokey_burst_actions', actions)
            self.owner._sync_menu_settings()
            return json.dumps({'ok': True, 'count': len(actions or [])}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def get_autokey_actions(self):
        try:
            actions = self.owner._get_setting('autokey_burst_actions', [])
            return json.dumps(actions, ensure_ascii=False)
        except Exception:
            return '[]'

    # ── WebView API: display toggles/settings that remain platform-window backed ──
    def set_boss_bar_mode(self, mode):
        mode = str(mode or 'boss_raid').strip()
        if mode not in ('always', 'boss_raid', 'off'):
            mode = 'boss_raid'
        self.owner._set_setting('boss_bar_mode', mode)
        self.owner._sync_menu_settings()
        return json.dumps({'ok': True, 'mode': mode}, ensure_ascii=False)

    def get_boss_bar_mode(self):
        return self.owner._get_setting('boss_bar_mode', 'boss_raid') or 'boss_raid'

    def set_dps_enabled(self, enabled):
        on = bool(enabled)
        self.owner._set_setting('dps_enabled', on)
        if not on:
            self.owner._hide_dps_window()
        else:
            tracker = getattr(self.owner, '_dps_tracker', None)
            try:
                if tracker and tracker.has_recent_damage(self.owner._combat_damage_timeout_s()):
                    self.owner._show_dps_live_snapshot(tracker.get_snapshot())
            except Exception:
                pass
        self.owner._sync_menu_settings()
        return json.dumps({'ok': True, 'enabled': on}, ensure_ascii=False)

    def set_dps_fade_timeout(self, seconds):
        try:
            val = max(0, min(120, int(float(seconds))))
        except Exception:
            val = 0
        self.owner._set_setting('dps_fade_timeout_s', val)
        self.owner._sync_menu_settings()
        return json.dumps({'ok': True, 'timeout': val}, ensure_ascii=False)

    def get_dps_enabled(self):
        return bool(self.owner._get_setting('dps_enabled', True))

    def set_buffmon_enabled(self, enabled):
        on = bool(enabled)
        self.owner._set_setting('buffmon_enabled', on)
        for attr in ('_self_buff_overlay', '_boss_buff_overlay'):
            try:
                ov = getattr(self.owner, attr, None)
                if ov is not None:
                    ov.set_enabled(on)
            except Exception:
                pass
        self.owner._sync_menu_settings()
        return json.dumps({'ok': True, 'enabled': on}, ensure_ascii=False)

    def get_buffmon_enabled(self):
        return bool(self.owner._get_setting('buffmon_enabled', True))

    def show_last_dps_report(self):
        try:
            if self.owner._show_dps_last_report():
                self.owner._sync_dps_report_availability()
                return json.dumps({'ok': True}, ensure_ascii=False)
            return json.dumps({'ok': False, 'message': 'No last combat report yet.'}, ensure_ascii=False)
        except Exception as exc:
            return self._json_error(exc)

    def boss_hp_hit_regions(self, regions):
        return None

    def toggle_raid_editor(self):
        def _do():
            self.owner._hide_raid_editor() if self.owner._raid_editor_visible else self.owner._show_raid_editor()
        threading.Thread(target=_do, daemon=True).start()

    def get_raid_editor_visible(self):
        return bool(self.owner._raid_editor_visible)

    def toggle_autokey_editor(self):
        def _do():
            self.owner._hide_autokey_editor() if self.owner._autokey_editor_visible else self.owner._show_autokey_editor()
        threading.Thread(target=_do, daemon=True).start()

    def get_autokey_editor_visible(self):
        return bool(self.owner._autokey_editor_visible)

    def set_data_source(self, mode):
        normalized = str(mode or 'tcp').strip().lower()
        if normalized not in ('tcp', 'memory', 'hybrid', 'auto'):
            normalized = 'hybrid'
        try:
            ref = getattr(self.owner, '_cfg_settings_ref', None) or self.owner.settings
            ref.set('mem_data_source', normalized)
            try:
                ref.save()
            except Exception:
                pass
            self._reconfigure_data_engines(restart_packet=True)
        except Exception as exc:
            self.owner._sync_menu_info()
            return json.dumps({'ok': False, 'mode': normalized, 'message': str(exc)}, ensure_ascii=False)
        self.owner._sync_menu_info()
        return json.dumps({'ok': True, 'mode': normalized}, ensure_ascii=False)

    def set_component_source(self, component, mode):
        from sr_config import DATA_SOURCE_COMPONENTS, DEFAULT_DATA_SOURCE_MAP, normalize_source_map, normalize_source_mode
        component_name = str(component or '').strip().lower()
        if component_name not in DATA_SOURCE_COMPONENTS:
            self.owner._sync_menu_settings()
            return json.dumps({'ok': False, 'error': 'bad_component', 'component': component_name}, ensure_ascii=False)
        mode_name = normalize_source_mode(mode, DEFAULT_DATA_SOURCE_MAP.get(component_name, 'packet'))
        if component_name == 'stamina':
            mode_name = 'vision'
        elif component_name == 'skills':
            mode_name = 'packet'
        source_map = {}
        try:
            ref = getattr(self.owner, '_cfg_settings_ref', None) or getattr(self.owner, 'settings', None)
            if ref is None:
                raise RuntimeError('settings unavailable')
            if hasattr(ref, 'set_component_source') and hasattr(ref, 'get_data_source_map'):
                ref.set_component_source(component_name, mode_name)
                source_map = ref.get_data_source_map()
            else:
                raw_map = ref.get('data_source_map', {}) if hasattr(ref, 'get') else {}
                legacy_mode = ref.get('data_source', 'packet') if hasattr(ref, 'get') else 'packet'
                source_map = normalize_source_map(raw_map, legacy_mode)
                source_map[component_name] = mode_name
                source_map = normalize_source_map(source_map, 'packet')
                if hasattr(ref, 'set'):
                    ref.set('data_source_map', dict(source_map))
                    ref.set('data_source', 'mixed')
            if hasattr(ref, 'save'):
                try:
                    ref.save()
                except Exception:
                    pass
            try:
                self._reconfigure_data_engines(restart_packet=False)
            except Exception:
                pass
        except Exception as exc:
            self.owner._sync_menu_settings()
            return json.dumps({'ok': False, 'error': str(exc), 'component': component_name, 'mode': mode_name}, ensure_ascii=False)
        self.owner._sync_menu_settings()
        return json.dumps({'ok': True, 'component': component_name, 'mode': mode_name,
                           'data_source_map': dict(source_map)}, ensure_ascii=False)


class StarResonanceDpsWindowAPI:
    # pywebview js_api for the DPS meter window.

    def __init__(self, gui):
        self._g = gui

    def window_drag(self, dx, dy):
        try:
            win = self._g.dps_win
            if win:
                win.move(win.x + int(dx), win.y + int(dy))
        except Exception:
            pass

    def reset_dps(self):
        try:
            tracker = getattr(self._g, '_dps_tracker', None)
            if tracker:
                tracker.reset()
                self._g._sync_dps_report_availability()
                if getattr(self._g, '_dps_mode', 'hidden') != 'report':
                    self._g._eval_dps(
                        f'DpsMeter.showLive({json.dumps(tracker.get_snapshot(), ensure_ascii=False)})'
                    )
                    self._g._push_dps_act_snapshot()
        except Exception:
            pass

    def request_live_snapshot(self):
        def _push():
            try:
                tracker = getattr(self._g, '_dps_tracker', None)
                snapshot = tracker.get_snapshot() if tracker else None
                if snapshot is None:
                    snapshot = {
                        'encounter_active': False,
                        'elapsed_s': 0.0,
                        'total_damage': 0,
                        'total_heal': 0,
                        'total_dps': 0,
                        'total_hps': 0,
                        'entities': [],
                    }
                self._g._dps_mode = 'live'
                self._g._eval_dps(
                    f'DpsMeter.showLive({json.dumps(snapshot, ensure_ascii=False)})'
                )
                self._g._push_dps_act_snapshot()
            except Exception:
                pass
        threading.Thread(target=_push, daemon=True).start()

    def show_last_report(self):
        try:
            tracker = getattr(self._g, '_dps_tracker', None)
            report = tracker.get_last_report() if tracker else None
            if not report:
                store = getattr(self._g, '_dps_history_store', None)
                report = store.latest_report() if store else None
            if not report:
                return json.dumps({
                    'ok': False,
                    'message': 'No last combat report yet.',
                }, ensure_ascii=False)
            self._g._dps_mode = 'report'
            self._g._eval_dps(
                f'DpsMeter.showLastReport({json.dumps(report, ensure_ascii=False)})'
            )
            self._g._push_dps_act_snapshot()
            return json.dumps({'ok': True}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def get_act_snapshot(self):
        try:
            snapshot = self._g._build_dps_act_snapshot() or {}
            return json.dumps({
                'ok': True,
                **snapshot,
            }, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def list_history(self, limit=20):
        payload = act_history_status(self._g, limit=int(limit or 20))
        return json.dumps({**payload, 'items': payload.get('encounters') or []}, ensure_ascii=False)

    def export_last_report(self, fmt='json'):
        return json.dumps(act_report_export(self._g, fmt=str(fmt or 'json')), ensure_ascii=False)

    def get_entity_detail(self, uid):
        def _fetch():
            try:
                tracker = getattr(self._g, '_dps_tracker', None)
                if tracker:
                    detail = tracker.get_entity_detail(int(uid))
                    if detail:
                        self._g._eval_dps(
                            f'DpsMeter.updateDetail({json.dumps(detail, ensure_ascii=False)})'
                        )
            except Exception:
                pass
        threading.Thread(target=_fetch, daemon=True).start()

    def set_detail_mode(self, active):
        try:
            self._g._set_dps_detail_mode(bool(active))
            return json.dumps({'ok': True}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def resize_dps(self, width, height, commit=False):
        try:
            self._g._resize_dps_window(int(width), int(height), persist=bool(commit))
            return json.dumps({'ok': True}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({'ok': False, 'message': str(e)}, ensure_ascii=False)

    def play_sound(self, name: str):
        threading.Thread(target=self._g._play_sound, args=(name,), daemon=True).start()


class StarResonanceRaidEditorAPI:
    # pywebview js_api for the raid editor overlay.

    def __init__(self, gui):
        self._g = gui

    def window_drag(self, dx, dy):
        try:
            win = self._g.raid_editor_win
            if win:
                win.move(win.x + int(dx), win.y + int(dy))
        except Exception:
            pass

    def set_entity_role(self, uuid, role):
        try:
            return self._g._game_plugin_call('set_entity_role', uuid, role)
        except Exception as exc:
            print(f'[SAO] plugin action failed: set_entity_role: {exc}')
            pass

    def raid_next_phase(self):
        try:
            return self._g._game_plugin_call('raid_next_phase')
        except Exception as exc:
            print(f'[SAO] plugin action failed: raid_next_phase: {exc}')
            pass

    def raid_reset(self):
        try:
            return self._g._game_plugin_call('raid_reset')
        except Exception as exc:
            print(f'[SAO] plugin action failed: raid_reset: {exc}')
            pass

    def play_sound(self, name: str):
        threading.Thread(target=self._g._play_sound, args=(name,), daemon=True).start()

    def get_panel_themes(self):
        return self._g._api.get_panel_themes()

    def get_boss_reactions(self, scene_key=None, boss_base_id=None):
        return self._g._api.get_boss_reactions(scene_key, boss_base_id)

    def save_boss_reaction(self, mapping_json, scene_key=None, boss_base_id=None):
        return self._g._api.save_boss_reaction(mapping_json, scene_key, boss_base_id)

    def delete_boss_reaction(self, mapping_id, scene_key=None, boss_base_id=None):
        return self._g._api.delete_boss_reaction(mapping_id, scene_key, boss_base_id)

    def import_observed_skills(self, base_id=0, scene_key=None):
        return self._g._api.import_observed_skills(base_id, scene_key)

    def get_mechanics(self, scene_key=None, boss_base_id=None):
        return self._g._api.get_mechanics(scene_key, boss_base_id)

    def save_mechanic(self, mech_json, scene_key=None, boss_base_id=None):
        return self._g._api.save_mechanic(mech_json, scene_key, boss_base_id)

    def delete_mechanic(self, mechanic_id, scene_key=None, boss_base_id=None):
        return self._g._api.delete_mechanic(mechanic_id, scene_key, boss_base_id)

    def create_mechanic_from_skill(self, skill_id, skill_name='', cast_duration_ms=None,
                                   scene_key=None, boss_base_id=None):
        return self._g._api.create_mechanic_from_skill(
            skill_id, skill_name, cast_duration_ms, scene_key, boss_base_id)

    def bind_mechanic_skill(self, mechanic_id, skill_id, scene_key=None, boss_base_id=None):
        return self._g._api.bind_mechanic_skill(mechanic_id, skill_id, scene_key, boss_base_id)

    def unbind_mechanic_skill(self, mechanic_id, skill_id, scene_key=None, boss_base_id=None):
        return self._g._api.unbind_mechanic_skill(mechanic_id, skill_id, scene_key, boss_base_id)

    def set_mechanics_master(self, flags_json):
        return self._g._api.set_mechanics_master(flags_json)

    def search_skill_catalog(self, query, limit=30):
        return self._g._api.search_skill_catalog(query, limit)

    def test_mechanic(self, mech_json, kinds_json='["tts","banner"]'):
        return self._g._api.test_mechanic(mech_json, kinds_json)


class StarResonanceCommanderAPI:
    # pywebview js_api for the commander panel.

    def __init__(self, gui):
        self._g = gui

    def window_drag(self, dx, dy):
        try:
            win = self._g.commander_win
            if win:
                win.move(win.x + int(dx), win.y + int(dy))
        except Exception:
            pass

    def close_commander(self):
        try:
            self._g._hide_commander()
        except Exception:
            pass

    def request_data(self):
        def _push():
            try:
                self._g._push_commander_data()
            except Exception:
                pass
        threading.Thread(target=_push, daemon=True).start()

    def play_sound(self, name: str):
        threading.Thread(target=self._g._play_sound, args=(name,), daemon=True).start()


class StarResonanceAutoKeyEditorAPI:
    # pywebview js_api for the AutoKey editor overlay.

    def __init__(self, gui):
        self._g = gui

    def window_drag(self, dx, dy):
        try:
            win = self._g.autokey_editor_win
            if win:
                win.move(win.x + int(dx), win.y + int(dy))
        except Exception:
            pass

    def save_autokey_actions(self, actions_json):
        try:
            return self._g._game_plugin_call('save_autokey_actions', actions_json)
        except Exception as exc:
            return json.dumps({'ok': False, 'message': str(exc)})

    def get_autokey_actions(self):
        try:
            return self._g._game_plugin_call('get_autokey_actions')
        except Exception as exc:
            print(f'[SAO] plugin action failed: get_autokey_actions: {exc}')
            return '[]'

    def play_sound(self, name: str):
        threading.Thread(target=self._g._play_sound, args=(name,), daemon=True).start()
