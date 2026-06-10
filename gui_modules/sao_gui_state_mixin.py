# -*- coding: utf-8 -*-
"""
SAOPlayerGUIStateMixin — second mixin extracted from SAOPlayerGUI
(round 32 of the sao_gui split refactor). 730 lines moved out.

This is the heart of the user's combat-lag complaint:
  * ``_on_game_state_update`` — GameStateManager listener; runs on
    whichever thread called ``state_mgr.update()`` (bridge worker,
    recognition worker, etc.).
  * ``_apply_fast_state_update`` — main-thread continuation of
    identity / level changes, dispatched via ``root.after(0, ...)``.
  * ``_push_packet_overlays`` — the big main-thread per-tick push
    (DPS / Boss HP / commander / HP overlay / SkillFX), called from
    ``_recognition_loop``.
  * ``_recognition_loop`` — the 200 ms Tk-after-driven main-thread
    loop that pulls game state and triggers overlay refreshes.

All four are methods on SAOPlayerGUI; they remain methods on the
SAOPlayerGUI MRO via this mixin. Combat-lag fix work will land
inside this module in later rounds (the goal is to push the heavy
compute portions of `_push_packet_overlays` onto a background
worker, leaving only Tk widget mutations on main).
"""

from __future__ import annotations

import threading
import time
from typing import Any, Dict, List

from engines.combat_analytics import mem_boss_break_override


def _boss_bar_main_key(_m, _recent_targets):
    """Boss-bar primary-target sort key. Hoisted from _compute_boss_hp_delta
    so the daemon worker doesn't rebind a fresh closure per tick."""
    _max_hp = int(getattr(_m, 'max_hp', 0) or 0)
    _hp = int(getattr(_m, 'hp', 0) or 0)
    _mh_for_pct = _max_hp if _max_hp > 0 else (_hp if _hp > 0 else 1)
    _hp_pct = (_hp / _mh_for_pct) if _mh_for_pct > 0 else 0.0
    _last_ts = float(_recent_targets.get(getattr(_m, 'uuid', 0), 0) or 0.0)
    return (-_max_hp, -_hp_pct, -_last_ts)


_BB_BAD_NAMES = frozenset({'', 'unit', 'target'})


def _bb_resolve_unit_name(direct_data, target_uuid, tracker):
    """Best-effort CN name for the boss-bar enemy slot.

    Order: explicit name on the monster object -> MonsterTable/BossTable lookup
    by template_id -> tracker uuid->name side map (fed by update_monster_info).
    Only READS the name tables, so it never conflicts with the cross-repo
    name-table regeneration flow.
    """
    if direct_data:
        nm = str(direct_data.get('name') or direct_data.get('monster_name')
                 or direct_data.get('display_name') or '').strip()
        if nm and nm.lower() not in _BB_BAD_NAMES:
            return nm[:20]
        try:
            tid = int(direct_data.get('template_id') or 0)
        except (TypeError, ValueError):
            tid = 0
        if tid > 0:
            try:
                from tools.tablekit.name_tables import names as _names
                rn = _names.boss(tid, default='') or _names.monster(tid, default='')
                if rn:
                    return str(rn)[:20]
            except Exception:
                pass
    try:
        u = int(target_uuid or 0)
    except (TypeError, ValueError):
        u = 0
    if u and tracker is not None:
        try:
            rn = tracker.get_target_name(u)
            if rn:
                return str(rn)[:20]
        except Exception:
            pass
    return ''

import _sao_cy_packet as _CY_PACKET  # type: ignore[import-not-found]
import _sao_cy_uihelpers as _CY_UI  # type: ignore[import-not-found]
from utils.perf_probe import probe as _probe


class SAOPlayerGUIStateMixin:
    """State-pull + overlay-push helpers (recognition_loop + push_packet_overlays + fast-state).

    SAOPlayerGUI must initialise the broad set of ``self.X`` attributes
    these methods touch (overlays, signature caches, throttle
    timestamps, dps tracker handle, boss-HP tracker, etc.) in its
    ``__init__`` — this mixin only contains method definitions.
    """

    def _on_game_state_update(self, gs):
        """Fast path for identity/level updates from packet or vision threads."""
        if self._destroyed or gs is None:
            return
        # Buff 监视器 (自身):
        # v3.1.9 round 23: state_mgr snapshots are shallow copies (round 13),
        # so `gs.self_buffs` IS the same list object as the previous snapshot
        # until the bridge actually pushes a fresh `self_buffs=...` kwarg.
        # Identity-compare against the cached reference to skip update_buffs
        # entirely on the ~30-60 Hz of state updates that touch only HP/STA
        # /skills (saves ~10-20 us each call, ~300-1000 us/sec total).
        try:
            _raw_buffs = getattr(gs, 'self_buffs', None)
            _prev_buffs_ref = getattr(self, '_last_self_buffs_ref', None)
            if _raw_buffs is not _prev_buffs_ref:
                self._last_self_buffs_ref = _raw_buffs
                _buf_list = list(_raw_buffs or [])
                # ACT buff/debuff coverage: feed the tracker FIRST (the
                # snapshot-diff accumulator credits the interval since the
                # previous change to the still-active buffs), then read the
                # uptime back so the overlay can show coverage%.
                _uptime = None
                tr = getattr(self, '_dps_tracker', None)
                if tr is not None:
                    tr.update_self_buffs(_buf_list)
                    try:
                        _uptime = tr.get_buff_uptime()
                    except Exception:
                        _uptime = None
                ov = getattr(self, '_self_buff_overlay', None)
                if ov is not None:
                    ov.update_buffs(
                        _buf_list,
                        float(getattr(gs, 'server_time_offset_ms', 0.0) or 0.0),
                        _uptime,
                    )
        except Exception:
            pass

        try:
            sig = (
                int(getattr(gs, 'level_base', 0) or 0),
                int(getattr(gs, 'level_extra', 0) or 0),
                int(getattr(gs, 'season_exp', 0) or 0),
                str(getattr(gs, 'player_name', '') or ''),
                str(getattr(gs, 'profession_name', '') or ''),
                str(getattr(gs, 'player_id', '') or ''),
            )
        except Exception:
            return
        if sig == getattr(self, '_last_fast_state_sig', None):
            return
        self._last_fast_state_sig = sig
        try:
            self.root.after(0, lambda snap=gs: self._apply_fast_state_update(snap))
        except Exception:
            pass

    def _apply_fast_state_update(self, gs):
        if self._destroyed or gs is None:
            return
        try:
            self._sync_session_players_cache(gs, min_interval=0.05)
            level_base = int(getattr(gs, 'level_base', 0) or self._level or 1)
            level_extra = int(getattr(gs, 'level_extra', 0) or 0)
            season_exp = int(getattr(gs, 'season_exp', 0) or 0)
            self._level = max(1, level_base)
            self._level_extra = max(0, level_extra)
            self._season_exp = max(0, season_exp)

            player_name = str(getattr(gs, 'player_name', '') or '')
            profession = str(getattr(gs, 'profession_name', '') or '')
            uid = str(getattr(gs, 'player_id', '') or '')
            if player_name:
                self._username = player_name
                disp = player_name
                if len(disp) > 10:
                    disp = disp[:9] + '...'
                self._hp_display_name = disp
            if profession:
                self._profession = profession
            if self._sao_menu:
                self._sao_menu.username = self._username or 'Player'
                self._sao_menu.description = self._profession or 'SAO Auto'

            panel = getattr(self, '_player_panel', None)
            if panel:
                try:
                    _level_sig = (self._level, self._level_extra, self._season_exp)
                    _level_changed = _level_sig != self._last_player_panel_level_sig
                    if hasattr(panel, 'update_profile'):
                        panel.update_profile(
                            self._username or 'Player',
                            self._profession or '',
                            repaint=not _level_changed,
                        )
                    else:
                        panel._username = self._username or 'Player'
                        panel._profession = self._profession or ''
                    if _level_sig != self._last_player_panel_level_sig:
                        self._last_player_panel_level_sig = _level_sig
                        panel.update_level(self._level, self._level_extra, self._season_exp)
                except Exception:
                    pass
            self._refresh_session_players_panel()

            if self._hp_overlay and getattr(self, '_hp_ov_visible', True):
                hp = int(getattr(gs, 'hp_current', 0) or 0)
                hp_max = int(getattr(gs, 'hp_max', 0) or 0)
                if hp_max <= 0:
                    hp, hp_max = getattr(self, '_sta_hp', (0, 1))
                _lv_text = self._format_level_text(self._level, self._level_extra)
                _hp_sig = (int(hp), max(1, int(hp_max)), _lv_text)
                if _hp_sig != self._last_hp_overlay_hp_sig:
                    self._last_hp_overlay_hp_sig = _hp_sig
                    self._hp_overlay.update_hp(
                        int(hp), max(1, int(hp_max)), _lv_text)
                if player_name or profession or uid:
                    _pi_sig = (
                        player_name or self._username or '',
                        profession or self._profession or '',
                        uid,
                    )
                    if _pi_sig != getattr(self, '_last_hp_player_info_sig', None):
                        self._last_hp_player_info_sig = _pi_sig
                        self._hp_overlay.set_player_info({
                            'name': _pi_sig[0],
                            'profession': _pi_sig[1],
                            'uid': _pi_sig[2],
                        })
        except Exception:
            pass

    @_probe.decorate('ui.push_packet_overlays')
    def _push_packet_overlays(self, gs):
        """始终运行的 DPS / Boss HP 覆盖板推送 — 数据完全由 packet on_damage 回调驱动,
        与 recognition_ok / packet_active 闸门解耦, 避免抓包链路里任何一处中断都拖累弹出.

        v2.3.16: consolidated DPS tracker access into a single lock acquisition
        via poll_overlay_state() to reduce lock contention from 9 → 1.
        """
        # ── DPS tracker: 更新自身玩家信息 ──
        _pp_now = time.time()
        # v3.1.8 round 20: throttle the in-session roster sync to 4 Hz.
        # The recognition_loop calls _push_packet_overlays every 200 ms and
        # `_session_players` doesn't change faster than humans can join /
        # leave a raid, so syncing more often than 250 ms is wasted work
        # on the Tk main thread (~20-50 us each call, ~3-5 calls/sec).
        self._sync_session_players_cache(gs, min_interval=0.25)
        self._sync_boss_hp_revive_hold(gs)
        menu = getattr(self, '_sao_menu', None)
        if menu and getattr(menu, 'visible', False):
            self._refresh_session_players_panel()
        if self._dps_tracker and gs is not None and getattr(gs, 'player_id', ''):
            try:
                _p_uid = int(gs.player_id) if str(gs.player_id).isdigit() else 0
                if _p_uid:
                    # v2.3.16: skip set_self_uid if unchanged (avoids lock)
                    if _p_uid != getattr(self, '_last_self_uid_pushed', 0):
                        self._last_self_uid_pushed = _p_uid
                        self._dps_tracker.set_self_uid(_p_uid)
                    if self._dps_overlay:
                        if _p_uid != getattr(self, '_last_dps_overlay_self_uid', 0):
                            self._last_dps_overlay_self_uid = _p_uid
                            self._dps_overlay.set_self_uid(_p_uid)
                    _self_fp = 0
                    _bridge = getattr(self, '_packet_engine', None)
                    if _bridge:
                        _all_p = _bridge.get_players()
                        _sp = _all_p.get(_p_uid)
                        if _sp:
                            _self_fp = getattr(_sp, 'fight_point', 0) or 0
                    # v2.3.16: skip update_player_info if sig unchanged
                    _pi_sig = (_p_uid, gs.player_name or '',
                               gs.profession_name or '', _self_fp,
                               int(gs.level_base or 0))
                    if _pi_sig != getattr(self, '_last_self_pi_sig', None):
                        self._last_self_pi_sig = _pi_sig
                        self._dps_tracker.update_player_info(
                            _p_uid,
                            gs.player_name or '',
                            gs.profession_name or '',
                            _self_fp,
                            int(gs.level_base or 0),
                        )
            except Exception:
                pass

        # ── 同步队伍中所有玩家信息 (batch, 1 lock) ──
        if self._dps_tracker:
            _bridge = getattr(self, '_packet_engine', None)
            if (_bridge and (
                    _pp_now - getattr(self, '_last_batch_pi_scan_ts', 0.0) >= 1.0
                    or getattr(self, '_last_batch_pi_sig', None) is None)):
                self._last_batch_pi_scan_ts = _pp_now
                try:
                    _batch = []
                    for _pu, _pd in _bridge.get_players().items():
                        if _pu and _pd.name:
                            _batch.append((
                                _pu,
                                _pd.name or '',
                                _pd.profession or '',
                                getattr(_pd, 'fight_point', 0) or 0,
                                getattr(_pd, 'level', 0) or 0,
                            ))
                    # v2.3.16: skip batch update if player roster unchanged
                    _batch_sig = _CY_UI.build_batch_sig(_batch)
                    if _batch_sig != getattr(self, '_last_batch_pi_sig', None):
                        self._last_batch_pi_sig = _batch_sig
                        self._dps_tracker.update_player_info_batch(_batch)
                except Exception:
                    pass

        # ── DPS Overlay push (single lock acquisition via poll_overlay_state) ──
        if self._dps_tracker:
            try:
                _dps_enabled = bool(self._get_setting('dps_enabled', True))
                _dps_fade_timeout = self._combat_damage_timeout_s()

                # Single lock acquisition for all DPS queries
                _detail_uid = 0
                if (self._dps_overlay
                        and getattr(self._dps_overlay, '_detail_visible', False)):
                    _detail_uid = int(getattr(self._dps_overlay, '_detail_uid', 0) or 0)

                _poll = self._dps_tracker.poll_overlay_state(
                    max_age_ms=150,
                    idle_timeout_s=_dps_fade_timeout,
                    detail_uid=_detail_uid,
                )

                if self._dps_overlay:
                    _has_report = bool(_poll['has_report'])
                    if _has_report != self._dps_last_report_available:
                        self._dps_last_report_available = _has_report
                        self._dps_overlay.set_report_available(_has_report)

                _dps_snap = _poll['snapshot']
                _dps_has_live = _poll['has_live']
                _dirty = _poll['dirty']

                _dps_should_push = bool(_dps_snap and (
                    _dirty
                    or (_dps_enabled and _dps_has_live and not self._dps_visible)
                    or (self._dps_visible and self._dps_mode != 'report')
                ))

                if _dps_should_push:
                    if _dps_enabled and _dps_has_live and self._dps_overlay and self._dps_mode != 'report':
                        self._cancel_dps_idle_reset_after()
                        self._scene_hide_token = int(getattr(self, '_scene_hide_token', 0) or 0) + 1
                        if not self._dps_visible:
                            self._show_dps_live_snapshot(_dps_snap)
                        else:
                            try:
                                self._dps_overlay.fade_in()
                            except Exception:
                                pass
                        self._dps_overlay.update(_dps_snap)
                        try:
                            self._push_dps_act_snapshot()
                        except Exception:
                            pass
                    elif self._dps_overlay and (self._dps_visible or self._dps_mode == 'report'):
                        self._dps_overlay.update(_dps_snap)
                        try:
                            self._push_dps_act_snapshot()
                        except Exception:
                            pass

                # Entity detail popup
                if _detail_uid > 0 and _poll['detail'] and self._dps_overlay:
                    try:
                        self._dps_overlay.update_detail(_poll['detail'])
                    except Exception:
                        pass

                # Fade management
                if self._dps_overlay and self._dps_visible and self._dps_mode != 'report':
                    if _poll['should_fade_out']:
                        if not self._dps_faded:
                            self._dps_overlay.fade_out()
                            self._dps_faded = True
                            self._schedule_dps_idle_reset_after_fade(
                                getattr(self._dps_overlay, 'FADE_OUT', 0.26) + 0.25,
                                float(_poll.get('last_event_time') or 0.0))
                        self._dps_visible = False
                        self._dps_mode = 'hidden'
                    elif self._dps_faded and _dps_has_live:
                        self._cancel_dps_idle_reset_after()
                        self._dps_overlay.fade_in()
                        self._dps_faded = False
            except Exception:
                pass

        # ── Boss HP Overlay push (镜像 webview target-based 追踪) ──
        # Round 35: compute dispatched to a daemon worker. The Tk main
        # thread does only two cheap things here:
        #   1. enqueue the latest (gs, now) snapshot for the worker (O(1))
        #   2. consume whatever payload the worker produced LAST tick and
        #      apply it via overlay.update() on main (the only Tk call)
        # One tick (~200 ms) of latency is added, well below human
        # perception, in exchange for moving ~260 lines of compute off
        # the main thread during heavy combat.
        if self._boss_hp_overlay is not None and gs is not None:
            self._ensure_boss_hp_worker()
            self._enqueue_boss_hp_compute(gs, _pp_now)
            _bb_payload = self._consume_boss_hp_payload()
            if _bb_payload is not None:
                try:
                    self._boss_hp_overlay.update(_bb_payload)
                except Exception:
                    pass

    # ------------------------------------------------------------------
    # Round 34 extraction: pure-compute boss-HP delta helper.
    # Currently still invoked on the Tk main thread (same-thread call from
    # _push_packet_overlays — wired up in the round-34 follow-up commit).
    # Round 35 will swap the call site to a daemon worker + root.after(0,)
    # re-entry; this method only touches `self.bb_*` state + bridge data
    # (no Tk widgets), so a single .update() call can stay on main.
    # ------------------------------------------------------------------
    @_probe.decorate('ui.compute_boss_hp_delta')
    def _compute_boss_hp_delta(self, gs, _pp_now):
        """Compute the boss-HP overlay payload for one tick.

        Returns the dict to pass to ``self._boss_hp_overlay.update(...)``,
        or ``None`` if no push is needed (sig unchanged, no overlay, or
        gs missing). Mutates ``self._bb_*`` state in-place to track the
        target lock + motion sig — these mutations are still on main
        thread today; round 35 will relocate them when this method
        moves to a worker.
        """
        if not self._boss_hp_overlay or gs is None:
            return None
        try:
            _bb_raid_active = getattr(gs, 'boss_raid_active', False)
            _bb_mode = self._get_setting('boss_bar_mode', 'boss_raid') or 'boss_raid'
            _bb_src = getattr(gs, 'boss_hp_source', 'none') or 'none'

            _now = _pp_now
            _bb_timeout = self._boss_hp_hold_timeout_s()
            try:
                _bb_scene_grace = _now < float(
                    getattr(self, '_scene_damage_grace_until', 0.0) or 0.0)
            except Exception:
                _bb_scene_grace = False
            for uuid in list(self._bb_recent_targets.keys()):
                if _now - self._bb_recent_targets.get(uuid, 0) > _bb_timeout:
                    self._bb_recent_targets.pop(uuid, None)

            _has_recent_self_damage = (_now - self._bb_last_damage_ts) < _bb_timeout
            if not _has_recent_self_damage and not self._bb_recent_targets:
                self._bb_last_target_uuid = 0
            _bb_last_target_damage_ts = float(
                self._bb_recent_targets.get(self._bb_last_target_uuid, 0.0) or 0.0)
            _bb_target_damage_live = bool(
                self._bb_last_target_uuid
                and _bb_last_target_damage_ts > 0.0
                and (_now - _bb_last_target_damage_ts) < _bb_timeout
            )
            _bb_has_damage_target = bool(
                _has_recent_self_damage
                and _bb_target_damage_live
            )

            _bb_direct_data = None
            _bb_direct_hp = 0
            _bb_direct_max = 0
            _bb_additional = []
            _bridge = getattr(self, '_packet_engine', None)

            if not _bb_raid_active:
                if _bridge and _has_recent_self_damage and self._bb_recent_targets:
                    _recent_monsters = []
                    for uuid, dmg_ts in list(self._bb_recent_targets.items()):
                        if _now - dmg_ts < _bb_timeout:
                            # Defense-in-depth: never let a player-suffix
                            # UUID drive the boss bar (suffix 0xFFFF==640
                            # is the parser's player marker). The damage-
                            # events mixin already filters these at write
                            # time, but stale entries pre-fix could still
                            # be sitting in the dict.
                            if (int(uuid) & 0xFFFF) == 640:
                                continue
                            m = _bridge.get_monster(uuid)
                            if self._boss_monster_usable(m):
                                _recent_monsters.append(m)
                    if _recent_monsters:
                        _rt = self._bb_recent_targets
                        _recent_monsters.sort(key=lambda _m: _boss_bar_main_key(_m, _rt))
                        main_m = _recent_monsters[0]
                        self._bb_last_target_uuid = getattr(main_m, 'uuid', 0)
                        _bb_direct_max = int(getattr(main_m, 'max_hp', 0)) or int(getattr(main_m, 'hp', 0))
                        _bb_direct_hp = max(0, int(getattr(main_m, 'hp', 0)))
                        _bb_direct_data = main_m.to_dict() if hasattr(main_m, 'to_dict') else {}
                        _bb_src = 'packet'
                        for m in _recent_monsters[1:]:
                            if len(_bb_additional) >= 4:
                                break
                            d = m.to_dict() if hasattr(m, 'to_dict') else {}
                            # hybrid: fall back to the MEM-resolved name (uuid->name fed by the
                            # mem bridge) when TCP has no name yet -- same resolver as the main slot.
                            _add_name = (_bb_resolve_unit_name(d, getattr(m, 'uuid', 0), self._dps_tracker)
                                         or str(d.get('name', 'Unit'))[:20])
                            _bb_additional.append({
                                'name': _add_name,
                                'hp_pct': round(float(d.get('hp_pct', 0.0)), 3),
                                'extinction_pct': round(float(d.get('extinction_pct', 0.0)), 3),
                                'has_break_data': bool(d.get('has_break_data', False)),
                                'breaking_stage': int(d.get('breaking_stage', -1)),
                                'shield_active': bool(d.get('shield_active', False)),
                                'shield_pct': round(float(d.get('shield_pct', 0.0)), 3),
                            })
                elif self._bb_last_target_uuid and not _has_recent_self_damage:
                    try:
                        _m = _bridge.get_monster(self._bb_last_target_uuid) if _bridge else None
                        if self._boss_monster_usable(_m):
                            _bb_direct_max = int(getattr(_m, 'max_hp', 0)) or int(getattr(_m, 'hp', 0))
                            _bb_direct_hp = max(0, int(getattr(_m, 'hp', 0)))
                            _bb_direct_data = _m.to_dict() if hasattr(_m, 'to_dict') else {}
                            _bb_src = 'packet'
                    except Exception:
                        pass

            if _bb_mode == 'off':
                _bb_show = False
            else:
                _bb_target_locked = bool(
                    self._bb_last_target_uuid
                    and self._bb_last_target_uuid in self._bb_recent_targets
                    and _bb_target_damage_live
                )
                _bb_has_tracked_packet_boss = bool(
                    _bb_direct_data is not None
                    and (_bb_target_locked or _has_recent_self_damage)
                )
                _bb_has_estimated_tracked_boss = bool(
                    _bb_target_locked
                    and _bb_src != 'none'
                    and _has_recent_self_damage
                    and (getattr(gs, 'boss_current_hp', 0)
                         or getattr(gs, 'boss_total_hp', 0)
                         or getattr(gs, 'boss_breaking_stage', -1) != -1)
                )
                _bb_has_packet_boss = bool(
                    _bb_has_tracked_packet_boss
                    or _bb_has_estimated_tracked_boss
                )
                _bb_show = (
                    (_has_recent_self_damage
                     or (_bb_raid_active and _bb_has_packet_boss)
                     or (_bb_scene_grace and _bb_has_tracked_packet_boss))
                    and (_bb_has_packet_boss
                         or (_bb_src != 'none'
                             and _bb_has_damage_target
                               and (getattr(gs, 'boss_current_hp', 0)
                                   or getattr(gs, 'boss_total_hp', 0)
                                   or getattr(gs, 'boss_breaking_stage', -1) != -1)))
                )

            if _bb_direct_data and not _bb_raid_active:
                _bb_hp_pct = _bb_direct_hp / _bb_direct_max if _bb_direct_max > 0 else 1.0
                _bb_data = {
                    'active': _bb_show,
                    'hp_pct': round(_bb_hp_pct, 3),
                    'hp_source': _bb_src,
                    'current_hp': _bb_direct_hp,
                    'total_hp': _bb_direct_max,
                    'shield_active': bool(_bb_direct_data.get('shield_active')),
                    'shield_pct': round(float(_bb_direct_data.get('shield_pct') or 0.0), 3),
                    'breaking_stage': int(_bb_direct_data.get('breaking_stage') or 0),
                    'has_break_data': bool(_bb_direct_data.get('has_break_data')),
                    'extinction_pct': round(float(_bb_direct_data.get('extinction_pct') or 0.0), 3),
                    'extinction': int(_bb_direct_data.get('extinction') or 0),
                    'max_extinction': int(_bb_direct_data.get('max_extinction') or 0),
                    'stop_breaking_ticking': bool(_bb_direct_data.get('stop_breaking_ticking')),
                    'in_overdrive': bool(_bb_direct_data.get('in_overdrive')),
                    'invincible': False,
                    'boss_name': _bb_resolve_unit_name(
                        _bb_direct_data, self._bb_last_target_uuid,
                        getattr(self, '_dps_tracker', None)) or '',
                }
            elif _bb_show and not _bb_raid_active:
                _bb_target_uuid = int(self._bb_last_target_uuid or 0)
                if not _bb_target_uuid and self._bb_recent_targets:
                    try:
                        _bb_target_uuid = int(next(iter(self._bb_recent_targets.keys())) or 0)
                    except Exception:
                        _bb_target_uuid = 0
                _bb_target_label = ''
                if _bb_target_uuid:
                    _bb_target_label = f'{_bb_target_uuid:X}'[-6:]
                _bb_data = {
                    'active': True,
                    'hp_pct': round(getattr(gs, 'boss_hp_est_pct', 1.0), 3),
                    'hp_source': 'estimate',
                    'current_hp': 0,
                    'total_hp': 0,
                    'shield_active': False,
                    'shield_pct': 0.0,
                    'breaking_stage': -1,
                    'has_break_data': False,
                    'extinction_pct': 0.0,
                    'extinction': 0,
                    'max_extinction': 0,
                    'stop_breaking_ticking': False,
                    'in_overdrive': False,
                    'invincible': False,
                    'boss_name': _bb_resolve_unit_name(
                        None, _bb_target_uuid, getattr(self, '_dps_tracker', None))
                    or (f'Target {_bb_target_label}' if _bb_target_label else 'Target'),
                }
            else:
                _bb_breaking_stage_gs = getattr(gs, 'boss_breaking_stage', -1)
                _bb_data = {
                    'active': _bb_show,
                    'hp_pct': round(getattr(gs, 'boss_hp_est_pct', 1.0), 3),
                    'hp_source': _bb_src,
                    'current_hp': getattr(gs, 'boss_current_hp', 0),
                    'total_hp': getattr(gs, 'boss_total_hp', 0),
                    'shield_active': getattr(gs, 'boss_shield_active', False),
                    'shield_pct': round(getattr(gs, 'boss_shield_pct', 0.0), 3),
                    'breaking_stage': _bb_breaking_stage_gs,
                    'has_break_data': _bb_breaking_stage_gs != -1,
                    'extinction_pct': round(getattr(gs, 'boss_extinction_pct', 0.0), 3),
                    'extinction': 0,
                    'max_extinction': 0,
                    'stop_breaking_ticking': False,
                    'in_overdrive': getattr(gs, 'boss_in_overdrive', False),
                    'invincible': getattr(gs, 'boss_invincible', False),
                    'boss_name': _bb_resolve_unit_name(
                        None, getattr(self, '_bb_last_target_uuid', 0),
                        getattr(self, '_dps_tracker', None)),
                }
            # Break source priority: in hybrid/auto/memory, once the MEM base is acquired
            # (sticky), boss break (stage + gauge%) comes from MEM; in TCP mode / before
            # base it stays TCP. Shield is NEVER overridden — TCP-only. Applied before the
            # motion-sig / build_boss_bar_sig push gate so MEM-only break changes still push.
            _bb_mem_break = mem_boss_break_override(_bridge)
            if _bb_mem_break is not None:
                (_bb_data['breaking_stage'],
                 _bb_data['has_break_data'],
                 _bb_data['extinction_pct']) = _bb_mem_break
                _bb_data['extinction'] = 0
                _bb_data['max_extinction'] = 0
                _bb_data['stop_breaking_ticking'] = False
            if _bb_show and not _bb_raid_active:
                _bb_hp_motion_sig = (
                    int(self._bb_last_target_uuid or 0),
                    str(_bb_data.get('hp_source') or ''),
                    int(_bb_data.get('current_hp') or 0),
                    int(_bb_data.get('total_hp') or 0),
                    round(float(_bb_data.get('hp_pct') or 0.0), 4),
                    round(float(_bb_data.get('shield_pct') or 0.0), 4),
                    int(_bb_data.get('breaking_stage') or -1),
                    round(float(_bb_data.get('extinction_pct') or 0.0), 4),
                )
                _bb_prev_motion_sig = getattr(self, '_bb_last_hp_motion_sig', None)
                _bb_is_new_target = (
                    _bb_prev_motion_sig is None
                    or _bb_prev_motion_sig[0] != _bb_hp_motion_sig[0]
                )
                if _bb_is_new_target:
                    self._bb_last_hp_motion_sig = _bb_hp_motion_sig
                    self._bb_last_hp_motion_ts = _now
                elif _bb_hp_motion_sig != _bb_prev_motion_sig:
                    self._bb_last_hp_motion_sig = _bb_hp_motion_sig
                    self._bb_last_hp_motion_ts = _now
                try:
                    _bb_stable_hide_raw = self._get_setting(
                        'boss_hp_stable_hide_s', 2.5)
                    _bb_stable_hide_s = float(
                        _bb_stable_hide_raw
                        if _bb_stable_hide_raw is not None else 2.5)
                except Exception:
                    _bb_stable_hide_s = 2.5
                if _bb_stable_hide_s > 0:
                    _bb_motion_ts = float(
                        getattr(self, '_bb_last_hp_motion_ts', 0.0) or 0.0)
                    _bb_damage_ts = float(
                        getattr(self, '_bb_last_damage_ts', 0.0) or 0.0)
                    try:
                        _dps_live_window_s = max(
                            float(_bb_timeout), float(_bb_stable_hide_s) * 2.0)
                        _dps_live_for_stable = bool(
                            self._dps_tracker
                            and self._dps_tracker.has_recent_damage(_dps_live_window_s))
                    except Exception:
                        _dps_live_for_stable = False
                    _bb_recent_damage_for_stable = (
                        _bb_damage_ts > 0.0
                        and (_now - _bb_damage_ts) < _bb_stable_hide_s
                    )
                    if (_bb_motion_ts > 0.0
                            and (_now - _bb_motion_ts) >= _bb_stable_hide_s
                            and not _bb_recent_damage_for_stable
                            and not _dps_live_for_stable
                            and not _bb_scene_grace):
                        _bb_data['active'] = False
            _bb_sig = _CY_UI.build_boss_bar_sig(_bb_data, _bb_additional)
            if _bb_sig != self._last_boss_hp_push_sig:
                self._last_boss_hp_push_sig = _bb_sig
                _bb_data['additional'] = _bb_additional
                return _bb_data
            return None
        except Exception:
            return None

    # ------------------------------------------------------------------
    # Round 35: off-main daemon worker for _compute_boss_hp_delta.
    #
    # The boss-HP compute (~260 lines of bridge.get_monster / sort / dict
    # building / sig hashing) used to run inline on the Tk main thread at
    # 5 Hz. In heavy combat (4-player commander panel + multi-mob target
    # tracking + raid bar), this could exceed the 200 ms tick budget,
    # causing visible UI hitches.
    #
    # The worker now consumes (gs, _now) snapshots from a single-slot
    # mailbox (latest-wins, drop-oldest), computes the overlay payload,
    # and stashes it in an output slot. The Tk main thread picks up
    # whatever payload the worker produced on the PREVIOUS tick and
    # calls _boss_hp_overlay.update() with it. One tick (~200 ms) of
    # latency is added — imperceptible vs. the human-noticeable jitter
    # we eliminate.
    #
    # Thread safety:
    #   * _bb_recent_targets dict — written by packet thread (insert on
    #     damage), now read AND popped by this worker. Worker uses
    #     `list(self._bb_recent_targets.items())` to snapshot before
    #     iterating (GIL-atomic per CPython); pop-during-other-write is
    #     also GIL-safe at the dict-op level.
    #   * _bb_last_target_uuid / _bb_last_damage_ts / _last_boss_hp_push_sig /
    #     _bb_last_hp_motion_sig / _bb_last_hp_motion_ts — scalar / tuple
    #     assignment, GIL-atomic. Cross-tick ordering may differ from
    #     before but the only visible effect is at most one extra
    #     "force push" frame, which is harmless (the next tick's sig
    #     compare absorbs it).
    #   * gs snapshot reference — already a shallow copy from
    #     GameStateManager (round-13 design); reading attributes from a
    #     different thread is safe.
    # ------------------------------------------------------------------
    def _ensure_boss_hp_worker(self):
        if getattr(self, '_boss_hp_worker_started', False):
            return
        self._boss_hp_worker_started = True
        self._boss_hp_worker_signal = threading.Event()
        self._boss_hp_worker_stop = threading.Event()
        self._boss_hp_worker_input_lock = threading.Lock()
        self._boss_hp_worker_input = None
        self._boss_hp_worker_output_lock = threading.Lock()
        self._boss_hp_worker_output = None
        try:
            t = threading.Thread(
                target=self._boss_hp_worker_loop,
                name='sao-boss-hp-worker',
                daemon=True,
            )
            t.start()
            self._boss_hp_worker_thread = t
        except Exception:
            self._boss_hp_worker_started = False

    def _boss_hp_worker_loop(self):
        signal = self._boss_hp_worker_signal
        stop = self._boss_hp_worker_stop
        in_lock = self._boss_hp_worker_input_lock
        out_lock = self._boss_hp_worker_output_lock
        while not stop.is_set():
            if not signal.wait(timeout=1.0):
                continue
            signal.clear()
            if stop.is_set():
                break
            with in_lock:
                inp = self._boss_hp_worker_input
                self._boss_hp_worker_input = None
            if inp is None:
                continue
            if getattr(self, '_destroyed', False):
                break
            gs, now = inp
            try:
                payload = self._compute_boss_hp_delta(gs, now)
            except Exception:
                payload = None
            with out_lock:
                self._boss_hp_worker_output = payload

    def _enqueue_boss_hp_compute(self, gs, now):
        """Push the latest (gs, now) into the worker mailbox (latest-wins)
        and wake the worker. O(1) main-thread cost.
        """
        with self._boss_hp_worker_input_lock:
            self._boss_hp_worker_input = (gs, now)
        self._boss_hp_worker_signal.set()

    def _consume_boss_hp_payload(self):
        """Read + clear the worker's latest output slot. Returns the
        overlay payload dict (to pass to ``self._boss_hp_overlay.update``)
        or ``None`` if the worker hasn't produced one since last consume.
        """
        with self._boss_hp_worker_output_lock:
            out = self._boss_hp_worker_output
            self._boss_hp_worker_output = None
        return out

    def _stop_boss_hp_worker(self):
        """Signal the worker to exit. Safe to call multiple times.
        Called from _finalize_close. The thread is daemon so it dies with
        the process anyway, but explicit shutdown lets it clean up
        promptly.
        """
        if not getattr(self, '_boss_hp_worker_started', False):
            return
        try:
            self._boss_hp_worker_stop.set()
            self._boss_hp_worker_signal.set()
        except Exception:
            pass

    @_probe.decorate('ui.recognition_loop')
    def _recognition_loop(self):
        """后台识别循环 — 读取 GameStateManager 并更新 HP 条 + 体力覆盖板 + DPS + Boss.

        注意: v2.1.2-f 起完全去除外层 `_recognition_active` 闸门 —
        即使 vision/packet 引擎初始化中途失败 (engines 列表为空导致
        `_recognition_active=False`), 只要 GameStateManager 还在运行,
        就应继续刷新 overlay。子模块 (DPS/Boss HP/BurstReady/HP overlay)
        各自做空数据检查, 不会因为闸门翻成 False 而集体卡死。
        """
        if self._destroyed:
            return
        if self._state_mgr is not None:
            try:
                gs = self._state_mgr.state
                # DPS / Boss HP 推送独立于 recognition gate, 完全由抓包驱动 ——
                # 只要 packet bridge 收到伤害事件就能弹出
                try:
                    self._push_packet_overlays(gs)
                except Exception as _e_pp:
                    if not getattr(self, '_pp_err_logged', False):
                        self._pp_err_logged = True
                        print(f'[SAO Entity] _push_packet_overlays error: {_e_pp}')
                        import traceback as _tb
                        _tb.print_exc()
                # ── 与 webview 对齐: HP/STA、SkillFX、commander、identity 持久化等
                # 均依靠各子模块自带的字段空检查来决定是否更新, 不再被 recognition_ok
                # / packet_active 总闸门拦下, 否则任意一路 vision/packet 闪断都会
                # 让整圈 overlay 卡住 (例如 BurstReady 不出来).
                if True:
                    # HP data
                    if gs.hp_max > 0:
                        hp, hp_max = gs.hp_current, gs.hp_max
                    elif gs.hp_pct > 0:
                        hp, hp_max = int(gs.hp_pct * 100), 100
                    else:
                        hp, hp_max = 0, 1
                    if gs.stamina_max > 0:
                        sta, sta_max = gs.stamina_current, gs.stamina_max
                    elif gs.stamina_pct > 0:
                        sta, sta_max = int(gs.stamina_pct * 100), 100
                    else:
                        sta, sta_max = 0, 1
                    pct = hp / hp_max if hp_max > 0 else 0
                    self._float_progress_pct = pct
                    self._sta_hp = (hp, hp_max)
                    self._sta_sta = (sta, sta_max)
                    self._game_state = gs
                    menu_level = int(gs.level_base or self._level or 1)
                    menu_level_extra = int(getattr(gs, 'level_extra', 0) or 0)
                    menu_season_exp = int(getattr(gs, 'season_exp', 0) or 0)
                    self._level = menu_level
                    self._level_extra = menu_level_extra
                    self._season_exp = menu_season_exp

                    # 同步 HP/STA 到菜单面板
                    _pp = getattr(self, '_player_panel', None)
                    if _pp:
                        _level_sig = (menu_level, menu_level_extra, menu_season_exp)
                        _level_changed = _level_sig != self._last_player_panel_level_sig
                        if hasattr(_pp, 'update_vitals'):
                            _pp.update_vitals(
                                (hp, hp_max),
                                (sta, sta_max),
                                repaint=not _level_changed,
                            )
                        else:
                            _pp._sta_hp = (hp, hp_max)
                            _pp._sta_sta = (sta, sta_max)
                            if (
                                not _level_changed and
                                getattr(_pp, '_active', False)
                            ):
                                _pp._redraw_top(_pp._target_w, _pp._top_h)
                        if _level_changed:
                            self._last_player_panel_level_sig = _level_sig
                            _pp.update_level(menu_level, menu_level_extra, menu_season_exp)

                    # DPS / Boss HP 推送已迁移到 _push_packet_overlays (在闸门外执行)

                    # ── Player HP Overlay push ──
                    if self._hp_overlay and getattr(self, '_hp_ov_visible', True):
                        try:
                            _lv = int(gs.level_base or 1)
                            _lv_extra = int(getattr(gs, 'level_extra', 0) or 0)
                            if _lv_extra > 0 and _lv > 0:
                                _lv_text = f'{_lv}(+{_lv_extra})'
                            else:
                                _lv_text = str(_lv)
                            # v2.3.17: HP and STA signatures are split so
                            # packet fast-path HP pushes do not suppress a
                            # later STA-only update from recognition.
                            _hp_sig = (int(hp), int(hp_max), _lv_text)
                            if _hp_sig != self._last_hp_overlay_hp_sig:
                                self._last_hp_overlay_hp_sig = _hp_sig
                                self._hp_overlay.update_hp(hp, hp_max, _lv_text)
                            _sta_sig = (int(sta), int(sta_max))
                            if _sta_sig != self._last_hp_overlay_sta_sig:
                                self._last_hp_overlay_sta_sig = _sta_sig
                                self._hp_overlay.update_sta(sta, sta_max)
                            _sta_offline = self._should_show_sta_offline(gs)
                            if _sta_offline != self._last_hp_sta_offline:
                                self._last_hp_sta_offline = _sta_offline
                                self._hp_overlay.set_sta_offline(_sta_offline)
                            if gs.player_name:
                                _pi_sig = (
                                    str(gs.player_name),
                                    str(gs.profession_name or ''),
                                    str(gs.player_id or ''),
                                )
                                if _pi_sig != getattr(self, '_last_hp_player_info_sig', None):
                                    self._last_hp_player_info_sig = _pi_sig
                                    self._hp_overlay.set_player_info({
                                        'name': _pi_sig[0],
                                        'profession': _pi_sig[1],
                                        'uid': _pi_sig[2],
                                    })
                            # Boss timer (use boss_timer_text + boss_enrage_remaining like webview)
                            _boss_text = getattr(gs, 'boss_timer_text', '') or ''
                            _boss_active = getattr(gs, 'boss_raid_active', False)
                            _boss_enrage = float(getattr(gs, 'boss_enrage_remaining', 0) or 0)
                            if _boss_active and _boss_text:
                                # engine pushes the threshold-config tier; legacy rule as fallback
                                _boss_urgency = getattr(gs, 'boss_enrage_urgency', '') or (
                                    'urgent' if 0 < _boss_enrage < 60 else 'normal')
                            else:
                                _boss_text = ''
                                _boss_urgency = 'normal'
                            if _boss_text != self._last_boss_timer_text or \
                               _boss_urgency != self._last_boss_timer_urgency:
                                self._last_boss_timer_text = _boss_text
                                self._last_boss_timer_urgency = _boss_urgency
                                self._hp_overlay.set_boss_timer(_boss_text, _boss_urgency)
                        except Exception:
                            pass

                    # ── SkillFX (Burst Mode Ready) Overlay push ──
                    if self._skillfx_overlay is not None:
                        try:
                            _burst_enabled = bool(self._get_setting('burst_enabled', True))
                            _burst_now = bool(getattr(gs, 'burst_ready', False))
                            _burst_prev = bool(getattr(self, '_last_burst_ready', False))
                            _burst_slot = self._pick_burst_trigger_slot(gs) if _burst_enabled else 0
                            _new_layout = self._get_skillfx_layout(gs)
                            if _new_layout and _new_layout != getattr(self, '_skillfx_layout', None):
                                self._skillfx_layout = _new_layout
                                self._skillfx_overlay.set_layout(_new_layout)
                            if _burst_enabled and _burst_now and _burst_slot > 0:
                                if not _burst_prev or _burst_slot != getattr(self, '_last_burst_slot_shown', 0):
                                    self._skillfx_overlay.show_burst(_burst_slot)
                                    self._last_burst_slot_shown = _burst_slot
                            elif _burst_prev and not _burst_now:
                                self._skillfx_overlay.hide_burst()
                                self._last_burst_slot_shown = 0
                            self._last_burst_ready = _burst_now
                        except Exception:
                            pass

                    # ── Commander panel refresh (300 ms) ──
                    try:
                        if self._commander_panel and self._commander_panel.is_visible():
                            if _pp_now - self._commander_last_push > 0.30:
                                self._commander_last_push = _pp_now
                                self._push_commander_data()
                    except Exception:
                        pass

                    # ── 同步玩家信息到显示变量 ──
                    if gs.player_name and gs.player_name != self._last_gs_name:
                        self._last_gs_name = gs.player_name
                        disp = gs.player_name
                        if len(disp) > 10:
                            disp = disp[:9] + '…'
                        self._hp_display_name = disp
                        self._username = gs.player_name
                    # ── 首次获取完整角色数据时自动保存 ──
                    # v3.1.8 round 19: save_profile reads + atomically writes
                    # settings.json (5-50 ms on slow disks). Previously this
                    # ran inline on the recognition_loop Tk callback and
                    # could visibly stall the UI for one frame. Off-main
                    # thread via a daemon thread keeps the main loop smooth.
                    if not self._profile_auto_saved and gs.player_name:
                        self._profile_auto_saved = True
                        _snap_name = gs.player_name
                        _snap_prof = gs.profession_name or ''
                        _snap_lv = gs.level_extra if gs.level_extra > 0 else gs.level_base
                        _snap_uid = gs.player_id or ''

                        def _save_profile_async(
                                name=_snap_name, prof=_snap_prof,
                                lv=_snap_lv, uid=_snap_uid):
                            try:
                                from engines.character_profile import save_profile
                                save_profile(
                                    username=name,
                                    profession=prof,
                                    level=lv if lv > 0 else 1,
                                    uid=uid,
                                )
                                print(f'[SAO-UI] 自动保存角色: {name}, '
                                      f'职业={prof}, LV={lv}, UID={uid}')
                            except Exception:
                                pass

                        try:
                            threading.Thread(
                                target=_save_profile_async,
                                name='sao-gui-save-profile',
                                daemon=True,
                            ).start()
                        except Exception:
                            pass
            except Exception as e:
                print(f'[SAO-UI] recognition loop error: {e}')
        if not self._destroyed:
            try:
                self.root.after(200, self._recognition_loop)
            except Exception:
                pass

