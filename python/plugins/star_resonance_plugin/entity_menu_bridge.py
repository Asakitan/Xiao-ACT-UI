# -*- coding: utf-8 -*-
"""Entity/Tk menu bridge for the Star Resonance plugin.

The platform owns only the generic SAO menu shell. This bridge injects the
game-specific left panel, player/session state, and plugin status hooks at
runtime through owner attributes.
"""

from __future__ import annotations

import time
import types
from typing import Any


def _bind(target: Any, name: str, callback) -> None:
    def _wrapped(*args, **kwargs):
        return callback(*args, **kwargs)
    _wrapped.__name__ = name
    setattr(target, name, _wrapped)


def _bind_mixin_methods(owner: Any) -> None:
    """Bind plugin-owned Tk mixin methods onto the platform owner.

    This keeps the platform class free of game mixins while preserving the
    existing method surface expected by game engines and menu commands.
    """
    mixin_paths = (
        'plugins.star_resonance_plugin.panels.sao_gui_state_mixin:SAOPlayerGUIStateMixin',
        'plugins.star_resonance_plugin.panels.sao_gui_actions_mixin:SAOPlayerGUIActionsMixin',
        'plugins.star_resonance_plugin.panels.sao_gui_engine_toggles_mixin:SAOPlayerGUIEngineTogglesMixin',
        'plugins.star_resonance_plugin.panels.sao_gui_engine_lifecycle_mixin:SAOPlayerGUIEngineLifecycleMixin',
        'plugins.star_resonance_plugin.panels.sao_gui_dps_theme_mixin:SAOPlayerGUIDpsThemeMixin',
        'plugins.star_resonance_plugin.panels.sao_gui_packet_callbacks_mixin:SAOPlayerGUIPacketCallbacksMixin',
        'plugins.star_resonance_plugin.panels.sao_gui_damage_events_mixin:SAOPlayerGUIDamageEventsMixin',
    )
    for spec in mixin_paths:
        module_name, class_name = spec.split(':', 1)
        try:
            module = __import__(module_name, fromlist=[class_name])
            mixin_cls = getattr(module, class_name)
        except Exception:
            continue
        for name, value in vars(mixin_cls).items():
            if name.startswith('__') or not callable(value):
                continue
            if hasattr(owner, name):
                continue
            try:
                setattr(owner, name, types.MethodType(value, owner))
            except Exception:
                pass


def install_entity_menu_bridge(ctx) -> None:
    owner = getattr(ctx.engine, 'owner', None)
    if owner is None:
        return
    bridge = StarResonanceEntityMenuBridge(ctx)
    bridge.initialize_owner_state()
    setattr(owner, '_star_resonance_entity_menu_bridge', bridge)
    setattr(owner, '_plugin_menu_left_widget_factory', bridge.make_left_widget)
    setattr(owner, '_plugin_menu_header_provider', bridge.build_menu_header)
    setattr(owner, '_plugin_on_menu_open', bridge.on_menu_open)
    setattr(owner, '_plugin_on_menu_close', bridge.on_menu_close)
    setattr(owner, '_plugin_toggle_status_panel', bridge.toggle_status_panel)
    setattr(owner, '_plugin_update_status_panel', bridge.update_status_panel)
    setattr(owner, '_plugin_edit_profile', bridge.edit_profile)
    for name in (
        '_session_int', '_session_self_uid', '_merge_session_player',
        '_sync_session_players_cache', '_format_session_power',
        '_get_session_player_rows', '_refresh_session_players_panel',
        '_toggle_session_players_panel', '_get_skillfx_layout',
        '_get_game_window_rect', '_get_game_window_context',
        '_format_level_text', '_get_mem_data_source',
    ):
        _bind(owner, name, getattr(bridge, name))
    _bind_mixin_methods(owner)
    ctx.log('[SR] Entity menu bridge installed')


class StarResonanceEntityMenuBridge:
    def __init__(self, ctx) -> None:
        self.ctx = ctx
        self.owner = ctx.engine.owner
        self._session_players: dict[int, dict[str, Any]] = {}
        self._session_players_self_uid = 0
        self._session_players_version = 0
        self._session_players_rows_cache_sig = None
        self._session_players_rows_cache: list[dict[str, Any]] = []
        self._session_players_last_sync_ts = 0.0
        self._last_session_players_panel_sig = None
        self._last_session_players_panel_push_ts = 0.0
        self._status_panel = None

    def initialize_owner_state(self) -> None:
        owner = self.owner
        defaults = {
            '_player_panel': None,
            '_menu_left_stack': None,
            '_session_players_panel': None,
            '_profile_dialog_pending': False,
            '_profile_dialog_ref': None,
            '_status_panel': None,
            '_cached_auto_key_result': None,
            '_cached_auto_key_sig': None,
            '_cached_boss_raid_result': None,
            '_cached_boss_raid_sig': None,
            '_packet_engine': None,
            '_vision_engine': None,
            '_vision_paused_for_death': False,
            '_last_dead_state': False,
            '_state_mgr': None,
            '_game_state': None,
            '_auto_key_engine': None,
            '_boss_raid_engine': None,
            '_boss_autokey_linkage': None,
            '_dps_tracker': None,
            '_dps_history_store': None,
            '_encounter_mgr': None,
            '_last_skill_event': {},
            '_last_dungeon_event': {},
            '_last_boss_event': {},
            '_dps_visible': False,
            '_dps_enabled': True,
            '_dps_faded': False,
            '_dps_idle_reset_after_id': None,
            '_dps_mode': 'hidden',
            '_dps_last_report_available': False,
            '_last_burst_ready': False,
            '_last_burst_slot': 0,
            '_last_boss_timer_text': '',
            '_last_boss_timer_urgency': '',
            '_last_boss_bar_sig': None,
            '_last_skillfx_sig': None,
            '_profile_auto_saved': False,
            '_last_gs_name': '',
            '_last_gs_prof': '',
            '_last_gs_uid': '',
            '_last_fast_state_sig': None,
            '_last_player_panel_level_sig': None,
            '_last_boss_hp_push_sig': None,
            '_last_commander_push_sig': None,
            '_bb_last_target_uuid': 0,
            '_bb_last_damage_ts': 0.0,
            '_bb_recent_targets': {},
            '_bb_last_hp_motion_sig': None,
            '_bb_last_hp_motion_ts': 0.0,
            '_bb_damage_timeout': 60.0,
            '_pending_combat_reset_after': 0.0,
            '_pending_combat_reset_reason': '',
            '_scene_damage_grace_until': time.time() + 20.0,
            '_scene_hide_token': 0,
            '_damage_self_fallback_log_ts': 0.0,
            '_dps_overlay': None,
            '_boss_hp_overlay': None,
            '_skillfx_overlay': None,
            '_map_banner_overlay': None,
            '_map_banner_last_name': '',
            '_map_banner_last_ts': 0.0,
            '_map_banner_timer': None,
            '_mem_bridge': None,
            '_act_trigger_engine': None,
            '_skillfx_layout': None,
            '_self_buff_overlay': None,
            '_boss_buff_overlay': None,
            '_buffmon_target_uuid': 0,
            '_autokey_panel': None,
            '_bossraid_panel': None,
            '_autokey_detail_panel': None,
            '_bossraid_detail_panel': None,
            '_commander_panel': None,
            '_commander_last_push': 0.0,
            '_THEME_OVERLAY_MAP': {
                'dps': '_dps_overlay',
                'hp': '_hp_overlay',
                'bosshp': '_boss_hp_overlay',
                'skillfx': '_skillfx_overlay',
                'alert': '_alert_overlay',
                'act': '',
                'buffmon': '',
            },
        }
        for name, value in defaults.items():
            if hasattr(owner, name):
                continue
            setattr(owner, name, value.copy() if isinstance(value, dict) else value)

    def build_menu_header(self) -> dict[str, str]:
        owner = self.owner
        username = str(getattr(owner, '_username', '') or '').strip() or 'Player'
        profession = str(getattr(owner, '_profession', '') or '').strip()
        return {
            'username': username,
            'description': profession or 'Star Resonance',
        }

    def make_left_widget(self, parent):
        from .panels.sao_menu_left_stack import SAOMenuLeftStack

        owner = self.owner
        stack = SAOMenuLeftStack(
            parent,
            username=str(getattr(owner, '_username', '') or '').strip() or 'Player',
            profession=str(getattr(owner, '_profession', '') or '').strip(),
            rows_provider=self._get_session_player_rows,
        )
        panel = stack.player_panel
        owner._menu_left_stack = stack
        owner._player_panel = panel
        owner._session_players_panel = stack.session_panel
        try:
            stack.session_panel.bind_global_wheel_fallback()
        except Exception:
            pass
        try:
            panel.update_level(
                int(getattr(owner, '_level', 0) or 1),
                int(getattr(owner, '_level_extra', 0) or 0),
                int(getattr(owner, '_season_exp', 0) or 0),
            )
        except Exception:
            pass
        if hasattr(panel, 'update_vitals'):
            try:
                panel.update_vitals(
                    getattr(owner, '_sta_hp', (0, 0)),
                    getattr(owner, '_sta_sta', (0, 0)),
                    repaint=False,
                )
            except Exception:
                pass
        saved_mode = ''
        get_setting = getattr(owner, '_get_setting', None)
        if callable(get_setting):
            saved_mode = str(get_setting('shift_mode', '普通模式') or '')
        if saved_mode:
            panel._shift_mode = saved_mode
        set_setting = getattr(owner, '_set_setting', None)
        if callable(set_setting):
            panel._on_mode_change = lambda mode: set_setting('shift_mode', mode)
        self._refresh_session_players_panel(force=True)
        return stack

    def on_menu_open(self) -> None:
        self._refresh_session_players_panel(force=True)

    def on_menu_close(self) -> None:
        owner = self.owner
        try:
            stack = getattr(owner, '_menu_left_stack', None)
            panel = getattr(stack, 'session_panel', None) if stack is not None else None
            if panel is not None and hasattr(panel, 'unbind_global_wheel_fallback'):
                panel.unbind_global_wheel_fallback()
        except Exception:
            pass
        owner._player_panel = None
        owner._menu_left_stack = None
        owner._session_players_panel = None

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
        owner = self.owner
        for source in (
                getattr(owner, '_game_state', None),
                getattr(getattr(owner, '_state_mgr', None), 'state', None)):
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
            self._session_players_rows_cache_sig = None

    def _sync_session_players_cache(self, gs=None, min_interval: float = 0.0):
        now_chk = time.time()
        if min_interval > 0 and (now_chk - self._session_players_last_sync_ts) < min_interval:
            return
        self._session_players_last_sync_ts = now_chk
        owner = self.owner
        self_uid = self._session_self_uid()
        if self_uid > 0 and self_uid != self._session_players_self_uid:
            if self._session_players_self_uid:
                self._session_players.clear()
                self._session_players_version += 1
                self._session_players_rows_cache_sig = None
            self._session_players_self_uid = self_uid
        if gs is None:
            gs = getattr(owner, '_game_state', None)
        if gs is not None:
            gs_uid = self._session_int(getattr(gs, 'player_id', 0), 0)
            self._merge_session_player(
                gs_uid,
                getattr(gs, 'player_name', '') or getattr(owner, '_username', '') or '',
                getattr(gs, 'fight_point', 0) or 0,
                is_self=bool(gs_uid and gs_uid == self_uid),
            )
        bridge = getattr(owner, '_packet_engine', None)
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
        cache_sig = (
            len(self._session_players),
            self._session_players_version,
            self_uid,
        )
        if cache_sig == self._session_players_rows_cache_sig:
            return list(self._session_players_rows_cache)
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
        self._session_players_rows_cache_sig = cache_sig
        self._session_players_rows_cache = list(rows)
        return rows

    def _refresh_session_players_panel(self, force: bool = False):
        owner = self.owner
        panel = getattr(owner, '_session_players_panel', None)
        if panel is None:
            stack = getattr(owner, '_menu_left_stack', None)
            if stack is not None and hasattr(stack, 'is_session_players_visible'):
                try:
                    if stack.is_session_players_visible():
                        panel = getattr(stack, 'session_panel', None)
                        owner._session_players_panel = panel
                except Exception:
                    panel = None
        if panel is None:
            return
        try:
            self._sync_session_players_cache(getattr(owner, '_game_state', None), min_interval=0.05)
        except TypeError:
            self._sync_session_players_cache(getattr(owner, '_game_state', None))
        sig = (len(self._session_players), self._session_players_version, self._session_self_uid())
        now = time.time()
        if not force and sig == self._last_session_players_panel_sig:
            return
        if not force and now - self._last_session_players_panel_push_ts < 0.5:
            return
        self._last_session_players_panel_sig = sig
        self._last_session_players_panel_push_ts = now
        try:
            panel.update_rows(self._get_session_player_rows(sync=False), force=force)
        except Exception:
            pass

    def _toggle_session_players_panel(self):
        owner = self.owner
        stack = getattr(owner, '_menu_left_stack', None)
        if stack is None:
            return
        rows = self._get_session_player_rows(sync=True)
        try:
            panel = stack.toggle_session_players(rows=rows, force=True)
            owner._session_players_panel = panel or getattr(stack, 'session_panel', None)
            self._last_session_players_panel_sig = (
                len(self._session_players),
                self._session_players_version,
                self._session_self_uid(),
            )
            self._last_session_players_panel_push_ts = time.time()
        except Exception:
            pass

    def _get_skillfx_layout(self, gs=None):
        if gs is None:
            mgr = getattr(self.owner, '_state_mgr', None)
            gs = getattr(mgr, 'state', None) if mgr is not None else None
        client_rect = getattr(gs, 'window_rect', None) if gs else None
        if not client_rect:
            try:
                from plugins.star_resonance_plugin.windowing import get_window_rect
                client_rect = get_window_rect()
            except Exception:
                client_rect = None
        if not client_rect:
            return None
        return None

    def _get_game_window_rect(self):
        rect = None
        owner = self.owner
        try:
            mgr = getattr(owner, '_state_mgr', None)
            gs = getattr(mgr, 'state', None) if mgr is not None else None
            window_rect = getattr(gs, 'window_rect', None) if gs else None
            if isinstance(window_rect, (list, tuple)) and len(window_rect) == 4:
                rect = tuple(int(v) for v in window_rect)
        except Exception:
            rect = None

        try:
            from plugins.star_resonance_plugin.windowing import create_window_locator
            locator = getattr(owner, '_locator', None)
            if locator is None:
                locator = create_window_locator()
                owner._locator = locator
            result = locator.find_target_window()
            if result:
                _hwnd, _title, found_rect = result
                return tuple(int(v) for v in found_rect)
        except Exception:
            pass

        return rect

    def _get_game_window_context(self):
        rect = None
        hwnd = 0
        owner = self.owner
        try:
            mgr = getattr(owner, '_state_mgr', None)
            gs = getattr(mgr, 'state', None) if mgr is not None else None
            window_rect = getattr(gs, 'window_rect', None) if gs else None
            if isinstance(window_rect, (list, tuple)) and len(window_rect) == 4:
                rect = tuple(int(v) for v in window_rect)
        except Exception:
            rect = None

        try:
            from plugins.star_resonance_plugin.windowing import create_window_locator
            locator = getattr(owner, '_locator', None)
            if locator is None:
                locator = create_window_locator()
                owner._locator = locator
            result = locator.find_target_window()
            if result:
                hwnd, _title, found_rect = result
                return int(hwnd or 0), tuple(int(v) for v in found_rect)
        except Exception:
            pass

        return int(hwnd or 0), rect

    def _format_level_text(self, level_base: int, level_extra: int) -> str:
        import _sao_cy_uihelpers as _CY_UI  # type: ignore[import-not-found]
        return _CY_UI.format_level_text(level_base, level_extra, getattr(self.owner, '_level', 0) or 1)

    def _get_mem_data_source(self) -> str:
        getter = getattr(self.owner, '_get_setting', None)
        if callable(getter):
            return str(getter('mem_data_source', 'tcp') or 'tcp').strip().lower()
        return 'tcp'

    def toggle_status_panel(self):
        return None

    def update_status_panel(self):
        return None

    def edit_profile(self):
        return None
