# -*- coding: utf-8 -*-
"""
SAOPlayerGUIDamageEventsMixin — eighteenth mixin extracted from
SAOPlayerGUI (round 67 of the sao_gui split refactor). 5 methods,
~287 lines.

The damage-event normalization + pending-combat-reset cluster.
These methods are called from the packet damage callback path
(see PacketCallbacks mixin) to:
  1. defer DPS encounter resets until the next real player-vs-
     monster hit (so a scene change doesn't blank the bar mid-fight),
  2. normalize incoming damage events so SELF / friendly / enemy
     attribution is consistent regardless of which packet variant
     the bridge emitted.

Methods (in source order):
  * _maybe_apply_pending_combat_reset(event, is_self_combat_target)
    (88) — if a pending reset window is open and the new hit is a
    real self-combat damage event, apply the reset. Otherwise let
    it expire silently. Avoids resetting on heal-only / immune-
    only packets.
  * _current_player_uid_int (16) — best-effort fetch of the
    SELF UID as int (used by the normalizers below).
  * _is_known_friendly_uid (42) — predicate: is this UID one of
    the in-session player roster (so a packet hitting it isn't an
    enemy attack on us).
  * _normalize_damage_event_for_self (47) — patches event['attacker_is_self']
    / target attribution + cleans up the SELF UID when the bridge
    used a stale uid.
  * _normalize_damage_event_target_for_entity (94) — heavier
    variant: rewrites target attribution so the entity HUD sees
    the right "target" for Boss-HP lock, even when the bridge
    emitted a friendly-side-effect packet.

Required SAOPlayerGUI attrs:
  * self._pending_combat_reset_after, self._pending_combat_reset_reason
  * self._dps_tracker, self._game_state, self._session_players,
    self._session_players_self_uid
  * self._packet_engine

Required SAOPlayerGUI methods (via MRO):
  * _session_int (Session mixin, staticmethod)
  * _session_self_uid (Session mixin)
"""

from __future__ import annotations

import math
import time
from typing import Any, Dict, Optional


class SAOPlayerGUIDamageEventsMixin:
    """Mixin bundling damage-event normalization + pending combat reset."""

    @staticmethod
    def _finite_combat_float(value: Any, default: float = 0.0, *, lo: Optional[float] = None) -> float:
        try:
            num = float(default if value is None or value == '' else value)
        except Exception:
            num = float(default or 0.0)
        if not math.isfinite(num):
            num = float(default or 0.0)
        if lo is not None:
            num = max(float(lo), num)
        return num

    def _arm_pending_combat_reset(self, scene_event=None):
        """Defer same-instance encounter reset until the next real damage."""
        reason = 'restart'
        delay_s = 3.0
        if isinstance(scene_event, dict):
            reason = str(scene_event.get('reason') or scene_event.get('kind') or reason)
            delay_s = self._finite_combat_float(scene_event.get('reset_delay_s'), delay_s, lo=0.0)
        self._pending_combat_reset_after = time.time() + max(0.0, delay_s)
        self._pending_combat_reset_reason = reason
        try:
            mgr = getattr(self, '_encounter_mgr', None)
            if mgr is not None:
                mgr.arm_pending_reset(reason, delay_s=delay_s)
        except Exception:
            pass
        self._scene_damage_grace_until = max(
            self._finite_combat_float(getattr(self, '_scene_damage_grace_until', 0.0), 0.0, lo=0.0),
            time.time() + max(8.0, delay_s + 8.0),
        )
        self._last_boss_hp_push_sig = None
        try:
            tracker = getattr(self, '_dps_tracker', None)
            if tracker:
                tracker.invalidate_snapshot_cache()
        except Exception:
            pass
        print(
            f'[SAO Entity] ♻ 同副本重开候选({reason}) — 等下一次伤害再重置 DPS/BossHP',
            flush=True,
        )

    def _maybe_apply_pending_combat_reset(self, event, is_self_combat_target: bool) -> bool:
        """Apply deferred encounter reset immediately before the first new hit."""
        try:
            reset_after = float(getattr(self, '_pending_combat_reset_after', 0.0) or 0.0)
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

        has_existing_encounter = False
        try:
            has_existing_encounter = bool(
                self._dps_tracker and self._dps_tracker.has_active_encounter())
        except Exception:
            has_existing_encounter = False
        if not has_existing_encounter:
            reason = str(getattr(self, '_pending_combat_reset_reason', '') or 'restart')
            self._pending_combat_reset_after = 0.0
            self._pending_combat_reset_reason = ''
            print(
                f'[SAO Entity] ♻ 延迟重置跳过: {reason} 当前遭遇战无有效伤害',
                flush=True,
            )
            return False

        reason = str(getattr(self, '_pending_combat_reset_reason', '') or 'restart')
        try:
            damage_ts = float(event.get('timestamp') or time.time())
        except Exception:
            damage_ts = time.time()
        self._pending_combat_reset_after = 0.0
        self._pending_combat_reset_reason = ''
        print(
            f'[SAO Entity] ♻ 下一次伤害到达，执行延迟重置: {reason}',
            flush=True,
        )

        self._bb_last_target_uuid = 0
        self._bb_last_damage_ts = 0.0
        self._bb_recent_targets = {}
        self._bb_last_hp_motion_sig = None
        self._bb_last_hp_motion_ts = 0.0
        self._last_boss_hp_push_sig = None
        self._scene_damage_grace_until = damage_ts + 8.0
        try:
            if self._state_mgr:
                self._state_mgr.update(
                    boss_breaking_stage=-1,
                    boss_extinction_pct=0.0,
                    boss_current_hp=0,
                    boss_total_hp=0,
                    boss_hp_source='none',
                    boss_hp_est_pct=1.0,
                    boss_shield_active=False,
                    boss_shield_pct=0.0,
                    boss_in_overdrive=False,
                    boss_invincible=False,
                )
        except Exception:
            pass
        if self._dps_tracker:
            try:
                if self._dps_tracker.has_active_encounter():
                    self._dps_tracker.reset()
                else:
                    self._dps_tracker.invalidate_snapshot_cache()
            except Exception:
                pass
        self._dps_visible = False
        self._dps_faded = False
        self._dps_mode = 'hidden'
        self._scene_hide_token = int(getattr(self, '_scene_hide_token', 0) or 0) + 1
        if self._boss_raid_engine:
            try:
                if getattr(self._boss_raid_engine, '_state', '') != 'running':
                    self._boss_raid_engine.reset()
            except Exception:
                pass
        return True

    def _current_player_uid_int(self) -> int:
        for source in (
            getattr(self, '_game_state', None),
            getattr(getattr(self, '_state_mgr', None), 'state', None),
        ):
            try:
                uid = getattr(source, 'player_id', '') if source is not None else ''
                if str(uid).isdigit():
                    return int(uid)
            except Exception:
                pass
        try:
            return int(getattr(self, '_last_self_uid_pushed', 0) or 0)
        except Exception:
            return 0

    def _is_known_friendly_uid(self, uid: int) -> bool:
        uid = self._session_int(uid, 0)
        if uid <= 0:
            return False
        self_uid = self._current_player_uid_int()
        if self_uid > 0 and uid == self_uid:
            return True
        bridge = getattr(self, '_packet_engine', None)
        if bridge is not None:
            try:
                team_members = getattr(bridge, '_team_members', {}) or {}
                if uid in team_members:
                    return True
            except Exception:
                pass
            try:
                pdata = (bridge.get_players() or {}).get(uid)
            except Exception:
                pdata = None
            if pdata is not None:
                try:
                    if (getattr(pdata, 'name', '')
                            or int(getattr(pdata, 'level', 0) or 0) > 0
                            or int(getattr(pdata, 'profession_id', 0) or 0) > 0
                            or int(getattr(pdata, 'fight_point', 0) or 0) > 0):
                        return True
                except Exception:
                    pass
        entry = getattr(self, '_session_players', {}).get(uid)
        if entry is not None:
            try:
                return bool(
                    entry.get('is_self')
                    or entry.get('name')
                    or int(entry.get('fight_point') or 0) > 0
                    or int(entry.get('level') or 0) > 0
                    or int(entry.get('profession_id') or 0) > 0
                )
            except Exception:
                return False
        return False

    def _normalize_damage_event_for_self(self, event):
        if not isinstance(event, dict):
            return event
        self_uid = self._current_player_uid_int()
        if event.get('attacker_is_self'):
            try:
                attacker_uid = int(event.get('attacker_uid') or 0)
            except Exception:
                attacker_uid = 0
            if attacker_uid or self_uid <= 0:
                return event
            fixed = dict(event)
            fixed['attacker_uid'] = self_uid
            fixed.setdefault('self_uid', self_uid)
            return fixed
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
        if now - float(getattr(self, '_damage_self_fallback_log_ts', 0.0) or 0.0) > 10.0:
            self._damage_self_fallback_log_ts = now
            print(
                f'[SAO Entity] 修正伤害归属: attacker_uid -> self_uid={self_uid}',
                flush=True,
            )
        return fixed

    def _normalize_damage_event_target_for_entity(self, event):
        """Recover combat-target classification after scene/dungeon switches.

        Some instance transitions clear the monster cache before the first
        damage packet of the next dungeon arrives. If that target UUID has a
        player-looking suffix, the parser can conservatively mark it as a
        player until SyncNearEntities catches up, which suppresses both DPS
        and BossHP. For self-outgoing damage to a target that is not present
        in the known player table, treat it as a combat target.

        v2.5.4: also force combat_target on by UUID encoding alone for any
        non-player-suffix target (StarResonanceDps / resonance-logs-cn parity).
        Stale `_players` entries preserved across scene resets used to make the
        `known_player` early return swallow the very first hit on a new boss
        whose encoded uid happened to collide with an old teammate's uid.
        """
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
            # Pure UUID-encoding routing — no caches involved, so no stale
            # state can mis-flag a fresh boss after a map change.
            fixed = dict(event)
            fixed['target_is_player'] = False
            fixed['target_is_monster'] = True
            fixed['target_is_combat_target'] = True
            fixed['entity_target_fallback'] = 'uuid_suffix_combat_target'
            return fixed

        if not event.get('attacker_is_self'):
            return event

        target_uid = 0
        try:
            target_uid = target_uuid >> 16
        except Exception:
            target_uid = 0
        self_uid = self._current_player_uid_int()
        if target_uid and self_uid and target_uid == self_uid:
            return event
        if self._is_known_friendly_uid(target_uid):
            return event

        try:
            in_scene_damage_grace = time.time() < float(
                getattr(self, '_scene_damage_grace_until', 0.0) or 0.0)
        except Exception:
            in_scene_damage_grace = False
        if in_scene_damage_grace:
            try:
                damage = int(event.get('damage') or 0)
            except Exception:
                damage = 0
            if damage > 0 or event.get('is_immune') or event.get('is_absorbed'):
                fixed = dict(event)
                fixed['target_is_player'] = False
                fixed['target_is_monster'] = True
                fixed['target_is_combat_target'] = True
                fixed['entity_target_fallback'] = 'post_scene_self_damage_grace'
                return fixed

        known_player = False
        bridge = getattr(self, '_packet_engine', None)
        if bridge and target_uid:
            try:
                known_player = target_uid in (bridge.get_players() or {})
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
        fixed['entity_target_fallback'] = 'unknown_target_after_scene_change'
        return fixed
