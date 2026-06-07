# -*- coding: utf-8 -*-
"""PacketParser main class extracted from packet_parser (physical split).

All cross-module references use absolute sibling imports
(``from packet_parser.<sub> import ...``) to avoid the half-initialised
``packet_parser/__init__`` import cycle.
"""

import math
import os
import json
import time
import logging
from typing import Optional, Callable, Dict, Any, List

from packet_parser import helpers as _helpers
from packet_parser.enums import (
    MessageType, NotifyMethod, _NOTIFY_METHOD_NAMES, AttrType, BASE_LEVEL_CAP,
    SERVICE_UUID_C3SB, BuffEventType, _BOSS_BUFF_EVENTS, WIPE_BUFF_BASE_ID,
    DamageType, EntityType, SkillCDType, CHAR_FIELD_NAMES,
    _HANDLED_CHAR_FIELDS, _RESET_IGNORE_TARGETS,
)
from packet_parser.skills import (
    PROFESSION_NAMES, PROFESSION_NORMAL_ATTACK, PROFESSION_SKILL,
    PROFESSION_ULTIMATE, PROFESSION_SKILL_VARIANTS, SUB_PROFESSION_NAMES,
    _compose_skill_level_id, _SKILL_TO_PROFESSION,
)
from packet_parser.data import MonsterData, PlayerData
from packet_parser.helpers import (
    logger,
    _CY_COMBAT, _CY_PACKET, _ensure_zstd, _ensure_pb,
    _decode_fields, _parse_dungeon_dirty_buffer,
    _varint_to_int64, _varint_to_int32,
    _decode_string_from_raw, _decode_int32_from_raw, _decode_float32_from_raw,
    _parse_dirty_subfield_header, _DIRTY_CHARBASE_FIELDS,
    _append_packet_debug, _probe,
    _is_player, _is_monster, _attrs_look_monster_like,
    _combat_damage_amount, _compute_damage_key, _uuid_to_uid,
    _decode_buff_info_sync_pb, _decode_dirty_energy_value,
    _is_sane_attr_stamina_max, _normalize_season_medal_level,
    _set_level_extra_candidate, _decode_resource_value_map,
    _decode_packed_varints, _extract_scene_basic_id_from_attrs,
    _refresh_stamina_resource,
)


class PacketParser:
    """Parse game packets and notify the callback when self data changes."""

    def __init__(self, on_self_update: Callable[[PlayerData], None],
                 on_damage: Optional[Callable[[dict], None]] = None,
                 on_monster_update: Optional[Callable[[dict], None]] = None,
                 on_boss_event: Optional[Callable[[dict], None]] = None,
                 on_scene_change: Optional[Callable[[], None]] = None,
                 preferred_uid: int = 0,
                 on_skill_event: Optional[Callable[[dict], None]] = None,
                 on_dungeon_event: Optional[Callable[[dict], None]] = None):
        self._on_update = on_self_update
        self._on_damage = on_damage   # callback(DamageEvent dict)
        self._on_monster_update = on_monster_update  # callback(MonsterData.to_dict())
        self._on_boss_event = on_boss_event          # callback({event_type, host_uuid, ...})
        self._on_skill_event = on_skill_event        # callback({kind, skill_uuid, ...})
        self._on_dungeon_event = on_dungeon_event    # callback({kind, dungeon_id, ...})
        self._on_scene_change = on_scene_change      # callback() — 场景服务器切换时清理
        # Phase 9: optional message-name allowlist for anchor-only TCP mode.
        # When None (default), all messages are processed. When set to a
        # non-empty set, only messages whose name is in the set are dispatched
        # to handlers (others are decoded but their handler invocation is
        # short-circuited).
        self._subscribed_messages: Optional[set] = None
        self._current_uuid: int = 0   # Current player UUID
        self._current_uid: int = max(0, int(preferred_uid))    # Current player UID (uuid >> 16)
        self._current_uid_confirmed: bool = False
        self._current_uid_source: str = 'cache' if self._current_uid > 0 else ''
        self._pending_self_dirty_packets: List[bytes] = []
        self._pending_self_dirty_replaying: bool = False
        self._players: Dict[int, PlayerData] = {}  # uid -> PlayerData
        self._monsters: Dict[int, MonsterData] = {}  # uuid -> MonsterData
        # Team member cache: uid -> {uid, name, profession, profession_id,
        #                            fight_point, level, joined_at}
        # Populated from CharTeam (SyncContainerData / DirtyData).
        # Persists across scene changes so commander panel can always show info.
        self._team_members: Dict[int, Dict[str, Any]] = {}
        self._team_id: int = 0
        self._team_leader_uid: int = 0
        # Cache template_id → max_hp from observed monsters.
        # Survives scene resets so if a monster re-appears without MAX_HP in
        # its AttrCollection (only incremental deltas), we can restore max_hp
        # from this cache.  Limited to 1024 entries.
        self._monster_hp_cache: Dict[int, int] = {}  # template_id -> max_hp
        self._profession_skill_cache: Dict[int, Dict[int, int]] = {}  # profession_id -> slot map
        self._zstd = None
        self._server_time_offset_ms: Optional[float] = None
        self._last_dungeon_id: int = 0   # Track dungeon transitions
        self._last_scene_id: int = 0     # Track scene/map transitions
        self._last_scene_key: Optional[tuple] = None  # MapId + channel/plane/layer, for instanced sub-maps
        self._sync_container_count: int = 0  # Count SyncContainerData receives
        self._last_soft_scene_restart_ts: float = 0.0
        self._last_soft_scene_transition_ts: float = 0.0
        self._active_dungeon_target_id: int = 0
        self.stats = {
            'raw_frames': 0,
            'game_frames': 0,
            'unknown_message_types': 0,
            'unknown_notify_methods': 0,
            'zstd_failures': 0,
            'damage_events': 0,
            'combat_damage_events': 0,
            'self_damage_events': 0,
            'monster_updates': 0,
            'boss_events': 0,
            'scene_changes': 0,
        }
        if self._current_uid > 0:
            logger.info(
                f'[Parser] bootstrap tentative self UID from cache: {self._current_uid}'
            )
        try:
            debug_path = os.path.join(
                os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                'packet_debug.jsonl')
            if os.path.exists(debug_path):
                os.remove(debug_path)
        except Exception:
            pass

    def _get_player(self, uid: int) -> PlayerData:
        if uid not in self._players:
            self._players[uid] = PlayerData(uid)
        return self._players[uid]

    def _confirm_self_uid(self, uid: int, source: str, uuid: int = 0) -> bool:
        """Mark current UID as server-confirmed, not just cache-guessed."""
        uid = int(uid or 0)
        if uid <= 0:
            return False
        previous_uid = self._current_uid
        was_confirmed = self._current_uid_confirmed
        if previous_uid and previous_uid != uid:
            logger.info(
                f'[Parser] confirmed self UID changed: {previous_uid} -> {uid} '
                f'(source={source})'
            )
            if not uuid:
                self._current_uuid = 0
        self._current_uid = uid
        if uuid:
            self._current_uuid = int(uuid)
        self._current_uid_confirmed = True
        self._current_uid_source = str(source or 'confirmed')
        player = self._get_player(uid)
        player.self_uid_confirmed = True
        player.self_uid_source = self._current_uid_source
        if previous_uid != uid or not was_confirmed:
            self._replay_pending_self_dirty_packets(uid, source)
        return previous_uid != uid or not was_confirmed

    def _queue_pending_self_dirty_packet(self, data: bytes) -> None:
        if not isinstance(data, bytes) or not data:
            return
        pending = self._pending_self_dirty_packets
        pending.append(bytes(data))
        if len(pending) > 12:
            del pending[:-12]
        if len(pending) in (1, 12):
            logger.info(
                f'[Parser] queued self dirty packet until UID confirmation '
                f'(pending={len(pending)}, tentative_uid={self._current_uid})'
            )

    def _replay_pending_self_dirty_packets(self, uid: int, source: str) -> None:
        if self._pending_self_dirty_replaying:
            return
        pending = self._pending_self_dirty_packets
        if not pending:
            return
        self._pending_self_dirty_packets = []
        self._pending_self_dirty_replaying = True
        try:
            logger.info(
                f'[Parser] replaying {len(pending)} queued self dirty packets '
                f'after UID confirm uid={uid} source={source}'
            )
            for raw in pending:
                try:
                    self._on_sync_container_dirty(raw)
                except Exception as e:
                    logger.debug(f'[Parser] queued dirty replay error: {e}')
        finally:
            self._pending_self_dirty_replaying = False

    def _is_confirmed_self_uid(self, uid: int) -> bool:
        return bool(
            self._current_uid_confirmed
            and int(uid or 0) > 0
            and int(uid or 0) == self._current_uid
        )

    def _player_entry_has_identity(self, uid: int) -> bool:
        """Return True only for player rows backed by real player data.

        Refreshing overworld monsters can use UUID layouts that look like
        player entities. If an early delta creates an empty PlayerData row for
        such a UUID, treating that row as a confirmed player suppresses DPS and
        BossHP for everyone attacking it.
        """
        try:
            player = self._players.get(int(uid or 0))
        except Exception:
            player = None
        if not player:
            return False
        return bool(
            getattr(player, 'name', '')
            or int(getattr(player, 'level', 0) or 0) > 0
            or int(getattr(player, 'profession_id', 0) or 0) > 0
            or int(getattr(player, 'fight_point', 0) or 0) > 0
            or bool(getattr(player, 'hp_from_full_sync', False))
            or bool(getattr(player, 'skill_slot_map', None))
        )

    def _is_known_player_uuid(self, uuid: int) -> bool:
        """Return True for UUIDs that should block combat-target DPS.

        A raw player-looking suffix is not enough in overworld refresh maps:
        monsters can be seen first via damage before their attrs/entity type
        arrive. Confirm players through self identity, team cache, or actual
        parsed player data.
        """
        if not uuid or not _is_player(uuid):
            return False
        if uuid in self._monsters:
            return False
        uid = _uuid_to_uid(uuid)
        if self._is_confirmed_self_uid(uid):
            return True
        if uid in self._team_members:
            return True
        # Player rows are intentionally preserved across scene resets so DPS
        # names/professions survive map changes. They are not strong enough by
        # themselves to classify a fresh target UUID as a player, because some
        # bosses/monsters in new scenes can reuse player-looking UUID suffixes
        # before SyncNearEntities arrives. Let damage-target fallbacks classify
        # unknown post-transition targets as combat units instead.
        return False

    def _get_monster(self, uuid: int) -> MonsterData:
        if uuid not in self._monsters:
            self._monsters[uuid] = MonsterData(uuid)
        return self._monsters[uuid]

    def _classify_entity_uuid(self, uuid: int) -> tuple[bool, bool]:
        """Return (is_player, is_monster), preferring observed entity state.

        Some overworld/repeated-instance combat targets reuse UUID layouts that
        look player-like by suffix. Once SyncNearEntities has registered such a
        UUID as a monster, keep treating later deltas/damage as monster traffic
        so DPS/BossHP are not filtered out.
        """
        if not uuid:
            return False, False
        if uuid in self._monsters:
            return False, True
        return bool(self._is_known_player_uuid(uuid)), bool(_is_monster(uuid))

    def reset_scene(self):
        """场景服务器切换时重置场景数据。

        清除:
        - 怪物缓存 (旧场景的怪物不会出现在新场景)
        - 服务器时间偏移 (新服务器有独立时间)
        - 副本 ID 追踪 (防止后续 SyncDungeonData 二次重置)
        - 同步计数器 (新服务器需要重新接收首个 full sync)
        - 场景 ID (新服务器/地图的场景 ID 需要重新识别)

        保留:
        - 玩家数据 (player identity/profession 跨场景不变)
        - 玩家 UID (CharId, 跨场景不变)
        - 职业技能缓存 (profession_skill_cache 跨场景不变)

        v2.1.18: 必须清空 _current_uuid (entity UUID), 因为它是 scene-server-scoped:
        新场景里玩家的 entity UUID 会变, 旧 UUID 与新 SCDeltaInfo 中的 attacker_uuid
        永远对不上, 导致 attacker_is_self=False → DPS tracker 永远不会把自己的伤害
        累计 → 整个 DPS / boss 血条都失效. 清零后 _decode_sync_damage_info 会走
        UID-based fallback, 直到下一个 SyncToMeDeltaInfo 重新确认 _current_uuid.
        """
        old_count = len(self._monsters)
        # Save template_id → max_hp to cache before clearing
        for m in self._monsters.values():
            if m.template_id > 0 and m.max_hp > 0:
                self._monster_hp_cache[m.template_id] = m.max_hp
        # Limit cache size
        if len(self._monster_hp_cache) > 1024:
            # Keep only the most recently added entries
            items = list(self._monster_hp_cache.items())
            self._monster_hp_cache = dict(items[-1024:])
        self._monsters.clear()
        self._server_time_offset_ms = None
        # 重置副本 ID: 服务器切换后 SyncDungeonData 会带新 dungeon_id,
        # 若 _last_dungeon_id 还保留旧值则会触发二次 reset_scene,
        # 把已经由 SyncNearEntities 填充的新怪物清空 (包括 MAX_HP)
        # → 后续 AoiSyncDelta 只发 HP 增量 → max_hp=0 → boss bar 失效。
        self._last_dungeon_id = 0
        self._active_dungeon_target_id = 0
        # 重置同步计数: 新连接需要重新接收首个 full sync
        self._sync_container_count = 0
        # 重置场景 ID: 新服务器/地图的场景 ID 需要重新识别
        self._last_scene_id = 0
        self._last_scene_key = None
        # v2.1.18: entity UUID 在新场景里会变 (scene-server-scoped),
        # 旧 UUID 必须清空, 否则 _decode_sync_damage_info 中
        # `attacker_uuid == self._current_uuid` 永远 False, fallback 路径里的
        # `_uuid_to_uid(attacker_uuid) == self._current_uid` 又被外层 if 屏蔽,
        # 导致自己的伤害无法识别 → DPS=0 / boss 血条不出.
        old_uuid = self._current_uuid
        self._current_uuid = 0
        self._dbg_openscene_dmg_logged = False  # 诊断标志: 新场景重新采样首发伤害归因
        self.stats['scene_changes'] += 1
        logger.info(
            f'[Parser] 场景重置: 清除 {old_count} 个怪物, '
            f'保留 {len(self._players)} 个玩家, '
            f'current_uid={self._current_uid}, '
            f'cleared current_uuid={old_uuid}'
        )
        print(
            f'[Parser] 场景切换重置: 清除 {old_count} 个旧怪物, '
            f'等待新场景 SyncNearEntities / SyncContainerData',
            flush=True,
        )
        # 通知上层 (bridge/webview) 场景已切换
        self._emit_scene_change('hard', 'reset_scene', preserve_combat=False)

    def _emit_scene_change(self, kind: str, reason: str = '',
                           preserve_combat: bool = False, **extra):
        """Notify UI layers about scene transitions with backward compatibility."""
        if not self._on_scene_change:
            return
        event = {
            'kind': str(kind or 'hard'),
            'reason': str(reason or ''),
            'preserve_combat': bool(preserve_combat),
            'timestamp': time.time(),
        }
        if extra:
            event.update(extra)
        try:
            self._on_scene_change(event)
        except TypeError:
            try:
                self._on_scene_change()
            except Exception as e:
                logger.error(f'[Parser] on_scene_change callback error: {e}')
        except Exception as e:
            logger.error(f'[Parser] on_scene_change callback error: {e}')

    def _emit_skill_event(self, kind: str, **payload):
        """Emit normalized skill lifecycle events for ACT/triggers."""
        if not self._on_skill_event:
            return
        event = {
            'kind': str(kind or ''),
            'timestamp': time.time(),
            'source': 'tcp',
        }
        event.update(payload)
        try:
            self._on_skill_event(event)
        except Exception as e:
            logger.debug(f'[Parser] on_skill_event callback error: {e}')

    def _emit_dungeon_event(self, kind: str, **payload):
        """Emit normalized dungeon/scene context events for ACT/triggers."""
        if not self._on_dungeon_event:
            return
        event = {
            'kind': str(kind or ''),
            'timestamp': time.time(),
            'source': 'tcp',
        }
        event.update(payload)
        try:
            self._on_dungeon_event(event)
        except Exception as e:
            logger.debug(f'[Parser] on_dungeon_event callback error: {e}')

    def _notify_soft_scene_restart(self, reason: str):
        """Notify UI layers about a likely new encounter without clearing monsters.

        Upstream counters defer same-instance encounter resets until the next
        damage packet. Doing the same here avoids wiping DPS/BossHP when a
        dungeon emits restart-like packets during map/layer mechanics.
        """
        now = time.time()
        if now - self._last_soft_scene_restart_ts < 1.5:
            logger.info(f'[Parser] Soft scene restart suppressed: {reason}')
            return
        self._last_soft_scene_restart_ts = now
        old_uuid = self._current_uuid
        self._current_uuid = 0
        # v2.3.23: prune stale monsters that haven't been refreshed for >30 s.
        # SyncNearEntities Disappear packets are not always reliable across
        # restarts; without this purge the boss bar can adopt phantom monsters
        # from the previous instance via `_bb_recent_targets`.
        purged = self._purge_stale_monsters_locked(30.0)
        logger.info(
            f'[Parser] Soft scene restart: {reason}, cleared current_uuid={old_uuid}, '
            f'purged {purged} stale monsters'
        )
        print(
            f'[Parser] ♻ 同场景重开: {reason}, 等下一次伤害再重置 DPS/BossHP',
            flush=True,
        )
        self._emit_scene_change(
            'restart',
            reason,
            preserve_combat=True,
            reset_on_next_damage=True,
            reset_delay_s=3.0,
        )

    def _apply_dungeon_target_reset_rules(self, targets, source: str) -> bool:
        """Mirror upstream objective checks without hard-clearing scene entities."""
        matched = False
        for target_data in targets or []:
            try:
                target_id = int(target_data.get('target_id') or 0)
                nums = int(target_data.get('nums') or 0)
                complete = int(target_data.get('complete') or 0)
            except Exception:
                continue
            reason = ''
            effective_target_id = target_id
            if complete == 0 and nums == 0:
                self._active_dungeon_target_id = target_id
                reason = 'new_objective'
            elif complete == 1 and nums > 0:
                if target_id == 0 and self._active_dungeon_target_id:
                    effective_target_id = self._active_dungeon_target_id
                reason = 'target_completed'
            if not reason:
                continue
            if effective_target_id in _RESET_IGNORE_TARGETS:
                logger.info(
                    f'[Parser] Dungeon target reset ignored: source={source} '
                    f'target_id={target_id} effective={effective_target_id} '
                    f'complete={complete} nums={nums}'
                )
                continue
            matched = True
            logger.info(
                f'[Parser] Dungeon target reset rule: source={source} '
                f'reason={reason} target_id={target_id} effective={effective_target_id} '
                f'complete={complete} nums={nums}'
            )
            self._notify_soft_scene_restart(
                f'dungeon_{reason}:{source}:target={effective_target_id}'
            )
        return matched

    def _notify_soft_scene_transition(self, reason: str, old_scene_key=None,
                                      new_scene_key=None):
        """Handle same-instance map/layer changes without wiping live combat."""
        now = time.time()
        if now - self._last_soft_scene_transition_ts < 1.0:
            logger.info(f'[Parser] Soft scene transition suppressed: {reason}')
            return
        self._last_soft_scene_transition_ts = now
        old_uuid = self._current_uuid
        self._current_uuid = 0
        # v2.3.23: prune stale monsters that haven't been refreshed for >15 s.
        # Layer/sub-map transitions move us to a new region with new monsters;
        # the stale entries from the old layer must not survive into the new
        # boss-bar candidate list.
        purged = self._purge_stale_monsters_locked(15.0)
        logger.info(
            f'[Parser] Soft scene transition: {reason}, '
            f'cleared current_uuid={old_uuid}, purged {purged} stale monsters, preserve combat'
        )
        print(
            f'[Parser] ↔ 同副本小地图/分层切换: {reason}, 保留 DPS/BossHP 遭遇战',
            flush=True,
        )
        self._emit_scene_change(
            'transition',
            reason,
            preserve_combat=True,
            old_scene_key=old_scene_key,
            new_scene_key=new_scene_key,
        )

    def _purge_stale_monsters_locked(self, ttl_s: float) -> int:
        """Drop monsters that have not been updated for at least `ttl_s` seconds.

        Returns the number of evicted entries. Saves each evicted monster's
        template_id → max_hp into `_monster_hp_cache` first so an Appear
        packet for the same template can rehydrate max_hp.
        """
        if not self._monsters:
            return 0
        now = time.time()
        cutoff = now - max(0.0, float(ttl_s))
        stale_uuids = []
        for uuid, m in self._monsters.items():
            last = float(getattr(m, 'last_update', 0.0) or 0.0)
            if last and last < cutoff:
                stale_uuids.append(uuid)
        for uuid in stale_uuids:
            m = self._monsters.get(uuid)
            if not m:
                continue
            if m.template_id > 0 and m.max_hp > 0:
                self._monster_hp_cache[m.template_id] = m.max_hp
            self._monsters.pop(uuid, None)
        if stale_uuids:
            print(
                f'[Parser] 清理 {len(stale_uuids)} 个过期怪物 (>{ttl_s:.0f}s 未更新)',
                flush=True,
            )
        return len(stale_uuids)

    def get_monsters(self) -> Dict[int, MonsterData]:
        """Return the current monster tracking dict (uuid → MonsterData)."""
        return self._monsters

    def get_players(self) -> Dict[int, 'PlayerData']:
        """Return all tracked players (uid → PlayerData) for info sync."""
        return self._players

    def get_alive_monsters(self) -> list:
        """Return list of alive monster dicts (for UI consumption)."""
        return [m.to_dict() for m in self._monsters.values()
                if not m.is_dead and m.max_hp > 0]

    def _notify_monster(self, monster: MonsterData):
        """Fire on_monster_update callback if registered."""
        if self._on_monster_update:
            self.stats['monster_updates'] += 1
            try:
                self._on_monster_update(monster.to_dict())
            except Exception as e:
                logger.debug(f'[Parser] monster update callback error: {e}')

    def _notify_boss_event(self, event_type: int, host_uuid: int,
                           buff_uuid: int = 0, extra: Optional[dict] = None):
        """Fire on_boss_event callback for boss-relevant buff events."""
        if self._on_boss_event:
            self.stats['boss_events'] += 1
            event = {
                'event_type': event_type,
                'host_uuid': host_uuid,
                'buff_uuid': buff_uuid,
                'timestamp': time.time(),
            }
            if extra:
                event.update(extra)
            try:
                self._on_boss_event(event)
            except Exception as e:
                logger.debug(f'[Parser] boss event callback error: {e}')

    def _check_wipe_buff(self, buff_id: int, source: str = '',
                         host_uuid: int = 0, buff_uuid: int = 0) -> bool:
        try:
            buff_id = int(buff_id or 0)
        except Exception:
            buff_id = 0
        if buff_id != WIPE_BUFF_BASE_ID:
            return False
        logger.info(
            f'[Parser] Wipe buff detected: base_id={buff_id} source={source} '
            f'host={host_uuid} buff_uuid={buff_uuid}'
        )
        print(
            f'[Parser] ☠ 检测到团灭 buff({buff_id})，等待下一次伤害重置 DPS/BossHP',
            flush=True,
        )
        self._notify_soft_scene_restart(f'wipe_buff:{source}:base={buff_id}')
        return True

    def _check_wipe_buffs(self, buffs, source: str = '', host_uuid: int = 0) -> bool:
        matched = False
        for buff in buffs or []:
            if not isinstance(buff, dict):
                continue
            buff_id = buff.get('buff_id') or buff.get('base_id') or buff.get('id')
            buff_uuid = buff.get('buff_uuid') or buff.get('uuid') or 0
            if self._check_wipe_buff(buff_id, source, host_uuid, buff_uuid):
                matched = True
        return matched

    def set_subscribed_messages(self, names: Optional[set]) -> None:
        """Phase 9: restrict which TCP messages trigger upstream callbacks.

        Pass `None` to subscribe to everything (default). Pass a set of
        message names like {'SyncContainerData'} to dispatch only those.
        Other messages are still parsed (for internal state coherence)
        but their on_* callbacks are skipped, saving CPU on the consumer
        side (DpsTracker, BossRaidEngine, etc.).

        Used by mem_probe.tcp_source.TcpSnapshotSource(mode='anchor_only')
        to keep the TCP path cheap once mem_probe owns the data flow.
        """
        if names is None:
            self._subscribed_messages = None
        else:
            self._subscribed_messages = {str(n) for n in names}

    def _is_subscribed(self, name: str) -> bool:
        """Returns True if message `name` should fire upstream callbacks."""
        if self._subscribed_messages is None:
            return True
        return name in self._subscribed_messages

    def _notify_self(self):
        """Notify callback for current player if available."""
        if self._current_uid and self._current_uid in self._players:
            p = self._players[self._current_uid]
            p.self_uid_confirmed = bool(self._current_uid_confirmed)
            p.self_uid_source = str(self._current_uid_source or '')
            self._apply_cached_profession_slots(p)
            p.server_time_offset_ms = self._server_time_offset_ms
            logger.debug(f'[Parser] notify_self: name={p.name!r} lv={p.level} rank_lv={p.rank_level} '
                         f'hp={p.hp}/{p.max_hp} uid={self._current_uid}')
            try:
                self._on_update(p)
            except Exception as e:
                print(f'[Parser] !! 回调异常 (bridge callback): {e}', flush=True)
                import traceback
                print(traceback.format_exc(), flush=True)
                logger.error(f'[Parser] callback error: {e}')

    def _apply_cached_profession_slots(self, player: PlayerData) -> bool:
        """Reuse the last known slot map for a profession when only profession id is available."""
        profession_id = int(getattr(player, 'profession_id', 0) or 0)
        if profession_id <= 0:
            return False
        cached_slot_map = self._profession_skill_cache.get(profession_id) or {}
        if not cached_slot_map or cached_slot_map == player.skill_slot_map:
            return False
        player.skill_slot_map = dict(cached_slot_map)
        return True

    def _try_detect_profession(self, player: PlayerData, skill_level_id: int) -> bool:
        """Auto-detect profession from observed skill IDs when SyncContainerData was missed.

        Uses reverse lookup from PROFESSION_NORMAL_ATTACK / PROFESSION_SKILL /
        PROFESSION_ULTIMATE / PROFESSION_SKILL_VARIANTS tables to identify
        the player's current profession from any matching skill_level_id.
        Also detects sub-profession branch from SUB_PROFESSION_NAMES.
        """
        base = skill_level_id // 100 if skill_level_id >= 100 else skill_level_id

        # Try to detect sub-profession branch even if profession is already known
        if base in SUB_PROFESSION_NAMES:
            sub = SUB_PROFESSION_NAMES[base]
            if sub and sub != getattr(player, 'sub_profession', ''):
                player.sub_profession = sub
                logger.info(
                    f'[Parser] detected sub_profession={sub!r} '
                    f'from skill_level_id={skill_level_id} (base={base})'
                )

        if int(getattr(player, 'profession_id', 0) or 0) > 0:
            return False  # Profession already known
        pid = _SKILL_TO_PROFESSION.get(base, 0)
        if pid > 0:
            player.profession_id = pid
            player.profession = PROFESSION_NAMES.get(pid, '')
            logger.info(
                f'[Parser] auto-detected profession={pid} ({player.profession}) '
                f'from skill_level_id={skill_level_id} (base={base})'
            )
            self._apply_cached_profession_slots(player)
            return True
        return False

    def _remember_seen_skill(self, player: PlayerData, skill_level_id: int) -> bool:
        """Cache every observed skill id, including zero-duration attack pings."""
        skill_level_id = int(skill_level_id or 0)
        if skill_level_id <= 0:
            return False
        seen = getattr(player, 'skill_seen_ids', None)
        if seen is None:
            player.skill_seen_ids = []
            seen = player.skill_seen_ids
        if skill_level_id in seen:
            return False
        seen.append(skill_level_id)
        return True

    def _replace_skill_cds(self, player: PlayerData, skill_cds) -> bool:
        """Replace the full self cooldown snapshot from UserFightAttr.CdInfo."""
        normalized: Dict[int, Dict[str, Any]] = {}
        observed_at_ms = int(time.time() * 1000)
        previous = player.skill_cd_map
        seen_changed = False
        # Server time for expiry check
        _server_now_ms = 0
        _ts_2020 = 1577836800000
        if self._server_time_offset_ms is not None:
            _server_now_ms = int(observed_at_ms + self._server_time_offset_ms)
        for skill_cd in skill_cds or []:
            skill_level_id = int(skill_cd.get('skill_level_id') or 0)
            # duration = total CD length; valid_cd_time = elapsed or progress
            total_ms = int(skill_cd.get('duration') or 0)
            if skill_level_id <= 0 or total_ms <= 0:
                continue
            # Skip CDs that have already expired based on server clock
            begin_time_ms = int(skill_cd.get('begin_time') or 0)
            if _server_now_ms > 0 and begin_time_ms > _ts_2020 and (begin_time_ms + total_ms) < _server_now_ms:
                continue
            if self._remember_seen_skill(player, skill_level_id):
                seen_changed = True
            normalized[skill_level_id] = {
                'skill_level_id': skill_level_id,
                'begin_time': max(0, int(skill_cd.get('begin_time') or 0)),
                'duration': max(0, int(skill_cd.get('duration') or 0)),
                'valid_cd_time': max(0, int(skill_cd.get('valid_cd_time') or 0)),
                'skill_cd_type': max(0, int(skill_cd.get('skill_cd_type') or 0)),
                'charge_count': max(0, int(skill_cd.get('charge_count') or 0)),
                'sub_cd_ratio': max(0, int(skill_cd.get('sub_cd_ratio') or 0)),
                'sub_cd_fixed': max(0, int(skill_cd.get('sub_cd_fixed') or 0)),
                'accelerate_cd_ratio': max(0, int(skill_cd.get('accelerate_cd_ratio') or 0)),
                'observed_at_ms': observed_at_ms,
                'source': 'full_sync',
            }
            prev = previous.get(skill_level_id)
            if prev and prev.get('begin_time') == normalized[skill_level_id].get('begin_time'):
                # Same CD instance — carry forward first-observation time and speed
                normalized[skill_level_id]['observed_at_ms'] = prev.get('observed_at_ms', observed_at_ms)
                # VCD value changed → update last_vcd_update_ms to now
                normalized[skill_level_id]['last_vcd_update_ms'] = observed_at_ms
                # Carry forward VCD speed tracking
                if 'vcd_speed_ratio' in prev:
                    normalized[skill_level_id]['vcd_speed_ratio'] = prev['vcd_speed_ratio']

        if previous == normalized and not seen_changed:
            return False

        player.skill_cd_map = normalized
        return True

    def _update_skill_cd(self, player: PlayerData, skill_cd: Dict[str, Any]) -> bool:
        """Merge one cooldown delta from AoiSyncToMeDelta.SyncSkillCDs."""
        skill_level_id = int(skill_cd.get('skill_level_id') or 0)
        if skill_level_id <= 0:
            return False
        seen_changed = self._remember_seen_skill(player, skill_level_id)

        # Auto-detect profession from observed skill IDs when SyncContainerData missed
        self._try_detect_profession(player, skill_level_id)

        total_ms = int(skill_cd.get('duration') or 0)
        if total_ms <= 0:
            player.skill_last_use_at[skill_level_id] = time.time()
            if skill_level_id in player.skill_cd_map:
                del player.skill_cd_map[skill_level_id]
                return True
            return seen_changed

        # Expiry check: if begin_time + duration < server_time, CD already expired
        begin_time_ms = int(skill_cd.get('begin_time') or 0)
        if begin_time_ms > 0 and self._server_time_offset_ms is not None:
            server_now_ms = int(time.time() * 1000 + self._server_time_offset_ms)
            _ts_2020 = 1577836800000
            if begin_time_ms > _ts_2020 and (begin_time_ms + total_ms) < server_now_ms:
                # CD already expired — treat as ready
                player.skill_last_use_at[skill_level_id] = time.time()
                if skill_level_id in player.skill_cd_map:
                    del player.skill_cd_map[skill_level_id]
                    return True
                return seen_changed

        observed_at_ms = int(time.time() * 1000)

        # SkillCD proto (from SyncToMeDelta) only has 5 fields and does NOT
        # include ChargeCount, SubCDRatio, SubCDFixed, AccelerateCDRatio.
        # The caller hardcodes these as 0.  When a previous entry exists
        # (from UserFightAttr full-sync SkillCDInfo which *does* have those
        # fields), carry forward the full-sync values so we don't lose
        # charge state and CD-reduction data.
        prev = player.skill_cd_map.get(skill_level_id)
        _carry_charge   = max(0, int((prev or {}).get('charge_count') or 0))
        _carry_sub_r    = max(0, int((prev or {}).get('sub_cd_ratio') or 0))
        _carry_sub_f    = max(0, int((prev or {}).get('sub_cd_fixed') or 0))
        _carry_accel    = max(0, int((prev or {}).get('accelerate_cd_ratio') or 0))

        new_entry = {
            'skill_level_id': skill_level_id,
            'begin_time': max(0, int(skill_cd.get('begin_time') or 0)),
            'duration': max(0, int(skill_cd.get('duration') or 0)),
            'valid_cd_time': max(0, int(skill_cd.get('valid_cd_time') or 0)),
            'skill_cd_type': max(0, int(skill_cd.get('skill_cd_type') or 0)),
            'charge_count': max(0, int(skill_cd.get('charge_count') or 0)) or _carry_charge,
            'sub_cd_ratio': max(0, int(skill_cd.get('sub_cd_ratio') or 0)) or _carry_sub_r,
            'sub_cd_fixed': max(0, int(skill_cd.get('sub_cd_fixed') or 0)) or _carry_sub_f,
            'accelerate_cd_ratio': max(0, int(skill_cd.get('accelerate_cd_ratio') or 0)) or _carry_accel,
            'observed_at_ms': observed_at_ms,
            'source': 'delta',
        }
        if prev:
            same_timing = (
                prev.get('begin_time') == new_entry['begin_time'] and
                prev.get('duration') == new_entry['duration']
            )
            same_core = (
                same_timing and
                prev.get('valid_cd_time') == new_entry['valid_cd_time'] and
                prev.get('skill_cd_type') == new_entry['skill_cd_type'] and
                prev.get('charge_count') == new_entry['charge_count'] and
                prev.get('accelerate_cd_ratio') == new_entry['accelerate_cd_ratio'] and
                prev.get('sub_cd_ratio') == new_entry['sub_cd_ratio'] and
                prev.get('sub_cd_fixed') == new_entry['sub_cd_fixed']
            )
            if same_core:
                new_entry['observed_at_ms'] = prev.get('observed_at_ms', observed_at_ms)
                new_entry['last_vcd_update_ms'] = prev.get('last_vcd_update_ms', observed_at_ms)
                # Carry forward VCD speed ratio
                if 'vcd_speed_ratio' in prev:
                    new_entry['vcd_speed_ratio'] = prev['vcd_speed_ratio']
                player.skill_cd_map[skill_level_id] = new_entry
                return False

            # Mid-CD acceleration/reduction change (same begin_time, same duration,
            # but acceleration fields differ)
            accel_changed = same_timing and (
                prev.get('accelerate_cd_ratio') != new_entry['accelerate_cd_ratio'] or
                prev.get('sub_cd_ratio') != new_entry['sub_cd_ratio'] or
                prev.get('sub_cd_fixed') != new_entry['sub_cd_fixed']
            )
            if accel_changed:
                new_entry['last_vcd_update_ms'] = observed_at_ms

            # --- VCD speed tracking ---
            # Track how fast valid_cd_time progresses vs real time to estimate
            # real remaining CD (server bakes in CD acceleration).
            if same_timing and not accel_changed:
                prev_vcd = int(prev.get('valid_cd_time') or 0)
                new_vcd = new_entry['valid_cd_time']
                delta_vcd = new_vcd - prev_vcd
                # Use last_vcd_update_ms (not observed_at_ms) to avoid stale timing
                prev_vcd_time = int(prev.get('last_vcd_update_ms') or prev.get('observed_at_ms') or 0)
                delta_real_ms = observed_at_ms - prev_vcd_time
                if delta_vcd > 0 and delta_real_ms > 50:
                    sample_speed = delta_vcd / delta_real_ms
                    if 0.5 < sample_speed < 25.0:  # sanity bounds
                        old_skill = prev.get('vcd_speed_ratio') or player.cd_speed_ratio
                        new_entry['vcd_speed_ratio'] = 0.3 * sample_speed + 0.7 * old_skill
                        player.cd_speed_ratio = (
                            0.2 * sample_speed + 0.8 * player.cd_speed_ratio
                        )
                elif 'vcd_speed_ratio' in prev:
                    new_entry['vcd_speed_ratio'] = prev['vcd_speed_ratio']
            elif 'vcd_speed_ratio' in prev:
                new_entry['vcd_speed_ratio'] = prev['vcd_speed_ratio']
            # Always refresh last_vcd_update_ms when VCD changes
            new_entry['last_vcd_update_ms'] = observed_at_ms

            if new_entry['begin_time'] != prev.get('begin_time'):
                # Only treat a *new* begin_time as a fresh skill cast.
                player.skill_last_use_at[skill_level_id] = time.time()
                # Keep vcd_speed_ratio — player buff doesn't change per-cast
        else:
            player.skill_last_use_at[skill_level_id] = time.time()

        player.skill_cd_map[skill_level_id] = new_entry
        return True





    @_probe.decorate('parser.process_packet')
    def process_packet(self, frame: bytes):
        """Process one framed packet: `[4B size][2B type][payload]`."""
        if len(frame) < 6:
            return
        self.stats['raw_frames'] += 1
        if self.stats['raw_frames'] == 1:
            print(f'[Parser] 首个数据帧到达 (size={len(frame)})', flush=True)
        for msg_type, is_zstd, payload in _CY_PACKET.parse_game_frame_headers(frame):
            try:
                if msg_type == MessageType.NOTIFY:
                    self.stats['game_frames'] += 1
                    self._on_notify(payload, is_zstd)
                elif msg_type == MessageType.FRAME_DOWN:
                    self.stats['game_frames'] += 1
                    self._on_frame_down(payload, is_zstd)
                else:
                    self.stats['unknown_message_types'] += 1
                    # 全量 TCP dump: 顶层既非 NOTIFY 也非 FRAME_DOWN 的帧 (unknown_msg)
                    if _helpers._TCP_DUMP_ENABLED:
                        _helpers._dump_tcp(
                            'unknown_msgtype', mt=int(msg_type),
                            zstd=bool(is_zstd), n=len(payload or b''),
                            hex=(payload or b'').hex())

            except Exception as e:
                import traceback
                print(f'[Parser] !! 消息处理异常 (type={msg_type}): {e}', flush=True)
                print(traceback.format_exc(), flush=True)
                logger.error(f'[Parser] message handling error (type={msg_type}): {e}\n{traceback.format_exc()}')


    #  FrameDown


    def _on_frame_down(self, payload: bytes, is_zstd: bool):
        if len(payload) < 4:
            return
        nested = payload[4:]
        if not nested:
            return
        if is_zstd:
            nested = self._decompress(nested)
            if nested is None:
                return

        self.process_packet(nested)


    #  Notify


    def _on_notify(self, payload: bytes, is_zstd: bool):
        header = _CY_PACKET.parse_notify_header(payload, SERVICE_UUID_C3SB)
        if header is None:
            return
        method_id, msg_payload = header

        # 首次收到游戏消息时打印
        if not getattr(self, '_first_notify_printed', False):
            self._first_notify_printed = True
            print(f'[Parser] 首个游戏Notify: method=0x{method_id:02X} zstd={is_zstd}', flush=True)

        if is_zstd:
            msg_payload = self._decompress(msg_payload)
            if msg_payload is None:
                return

        # 全量 TCP dump (诊断): 解压后每个 notify 的 method_id + 原始 hex,
        # 便于离线确认伤害/场景包用的是哪个 method (回归分析)。
        if _helpers._TCP_DUMP_ENABLED:
            _helpers._dump_tcp(
                'notify', mid=int(method_id),
                name=_NOTIFY_METHOD_NAMES.get(method_id, ''),
                zstd=bool(is_zstd), n=len(msg_payload or b''),
                hex=(msg_payload or b'').hex())

        if method_id == NotifyMethod.ENTER_SCENE:
            self._on_enter_scene(msg_payload)
        elif method_id == NotifyMethod.SYNC_CONTAINER_DATA:
            self._on_sync_container_data(msg_payload)
        elif method_id == NotifyMethod.SYNC_CONTAINER_DIRTY_DATA:
            self._on_sync_container_dirty(msg_payload)
        elif method_id == NotifyMethod.SYNC_SERVER_TIME:
            self._on_sync_server_time(msg_payload)
        elif method_id == NotifyMethod.SYNC_NEAR_ENTITIES:
            self._on_sync_near_entities(msg_payload)
        elif method_id == NotifyMethod.SYNC_TO_ME_DELTA_INFO:
            self._on_sync_to_me_delta(msg_payload)
        elif method_id == NotifyMethod.SYNC_NEAR_DELTA_INFO:
            self._on_sync_near_delta(msg_payload)
        # ── Combat notify (battle server) ──
        elif method_id == NotifyMethod.NOTIFY_BUFF_CHANGE:
            self._on_notify_buff_change(msg_payload)
        elif method_id in (NotifyMethod.SYNC_CLIENT_USE_SKILL,
                           NotifyMethod.SYNC_CLIENT_USE_SKILL_WORLD):
            self._on_sync_client_use_skill(msg_payload)
        elif method_id == NotifyMethod.SYNC_SERVER_SKILL_END:
            self._on_sync_server_skill_end(msg_payload)
        elif method_id == NotifyMethod.SYNC_SERVER_SKILL_STAGE_END:
            self._on_sync_server_skill_stage_end(msg_payload)
        elif method_id == NotifyMethod.QTE_BEGIN:
            self._on_qte_begin(msg_payload)
        # ── Dungeon / Scene ──
        elif method_id == NotifyMethod.SYNC_DUNGEON_DATA:
            self._on_sync_dungeon_data(msg_payload)
        elif method_id == NotifyMethod.SYNC_DUNGEON_DIRTY_DATA:
            self._on_sync_dungeon_dirty_data(msg_payload)
        elif method_id == NotifyMethod.NOTIFY_START_PLAYING_DUNGEON:
            self._on_notify_start_playing_dungeon(msg_payload)
        # ── Login / Session ──
        elif method_id == NotifyMethod.ENTER_GAME:
            self._on_enter_game(msg_payload)
        elif method_id == NotifyMethod.NOTIFY_REVIVE_USER:
            self._on_notify_revive_user(msg_payload)
        elif method_id == NotifyMethod.NOTIFY_CLIENT_KICK_OFF:
            logger.info('[Parser] NotifyClientKickOff received — session ended')
        # ── Team notify ──
        elif method_id == NotifyMethod.NOTIFY_ALL_MEMBER_READY:
            self._on_notify_team_generic('AllMemberReady', msg_payload)
        elif method_id == NotifyMethod.NOTIFY_CAPTAIN_READY:
            self._on_notify_team_generic('CaptainReady', msg_payload)
        elif method_id == NotifyMethod.ENTER_MATCH_RESULT_NTF:
            self._on_notify_team_generic('EnterMatchResult', msg_payload)
        # ── All other known methods — log for completeness ──
        else:
            method_name = _NOTIFY_METHOD_NAMES.get(method_id)
            if method_name:
                logger.debug(f'[Parser] Unhandled notify: {method_name} (0x{method_id:X}) len={len(msg_payload)}')
            else:
                self.stats['unknown_notify_methods'] += 1
                logger.debug(f'[Parser] Unknown notify method 0x{method_id:X} len={len(msg_payload)}')


    # SyncContainerData (full character sync)


    def _on_sync_server_time(self, data: bytes):
        """Track server/client time delta for packet-only cooldown progress."""
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.SyncServerTime()
            msg.ParseFromString(data)
        except Exception as e:
            logger.debug(f'[Parser] SyncServerTime pb2 parse error: {e}')
            return
        client_ms = msg.ClientMilliseconds
        server_ms = msg.ServerMilliseconds
        if client_ms <= 0 and server_ms <= 0:
            return

        local_ms = int(time.time() * 1000)
        if client_ms > 0 and server_ms > 0:
            self._server_time_offset_ms = float(server_ms - client_ms)
        elif server_ms > 0:
            self._server_time_offset_ms = float(server_ms - local_ms)

        if self._current_uid and self._current_uid in self._players:
            player = self._players[self._current_uid]
            player.server_time_offset_ms = self._server_time_offset_ms
            if player.skill_cd_map:
                self._notify_self()


    # ── Combat notify handlers (battle server 0x3000 range) ──


    def _on_notify_buff_change(self, data: bytes):
        """Handle NotifyBuffChange (0x3003 / 12291) — buff replacement notification.

        NotifyBuffChange proto:
            int32 OldBuffId = 1;   // 被替换的buff
            int32 NewBuffId = 2;   // 新buff
        Note: Full buff state is tracked from AoiSyncDelta.BuffInfos and Entity.BuffInfos.
        """
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.NotifyBuffChange()
            msg.ParseFromString(data)
            old_id = msg.OldBuffId
            new_id = msg.NewBuffId
            logger.info(f'[Parser] NotifyBuffChange(pb2): old={old_id} new={new_id}')
            _append_packet_debug('buff_change', {'old_buff_id': old_id, 'new_buff_id': new_id})
            self._check_wipe_buff(new_id, 'notify_buff_change')
        except Exception as e:
            logger.debug(f'[Parser] NotifyBuffChange decode error: {e}')

    def _on_sync_client_use_skill(self, data: bytes):
        """Handle SyncClientUseSkill (0x3002 / 12290) — skill use confirmation.

        SyncClientUseSkill proto:
            int64 SkillTargetUuid = 1;   // 技能目标
            int32 SkillLevelId = 2;      // 技能等级ID
        Note: caster is implied to be the local player ("Client" use skill).
        """
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.SyncClientUseSkill()
            msg.ParseFromString(data)
            target_uuid = msg.SkillTargetUuid
            skill_level_id = msg.SkillLevelId
            logger.info(
                f'[Parser] SyncClientUseSkill(pb2): target={target_uuid} '
                f'skill_level_id={skill_level_id}'
            )
            _append_packet_debug('use_skill', {
                'target_uuid': target_uuid,
                'skill_level_id': skill_level_id,
            })
            self._emit_skill_event(
                'client_use',
                method_id=NotifyMethod.SYNC_CLIENT_USE_SKILL,
                skill_level_id=int(skill_level_id or 0),
                target_uuid=int(target_uuid or 0),
                caster_uid=int(self._current_uid or 0),
                caster_uuid=int(self._current_uuid or 0),
            )
            # Record skill use timestamp for CD tracking (caster = current player)
            uid = self._current_uid
            if uid and uid in self._players:
                player = self._players[uid]
                if skill_level_id > 0:
                    player.skill_last_use_at[skill_level_id] = time.time()
                    self._remember_seen_skill(player, skill_level_id)
                    self._try_detect_profession(player, skill_level_id)
        except Exception as e:
            logger.debug(f'[Parser] SyncClientUseSkill decode error: {e}')

    def _on_sync_server_skill_end(self, data: bytes):
        """Handle SyncServerSkillEnd (0x3005 / 12293) — skill cast completed.

        SyncServerSkillEnd proto:
            int32 SkillUuid = 1;    // 技能会话ID
        """
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.SyncServerSkillEnd()
            msg.ParseFromString(data)
            logger.debug(f'[Parser] SyncServerSkillEnd(pb2): skill_uuid={msg.SkillUuid}')
            _append_packet_debug('skill_end', {'skill_uuid': msg.SkillUuid})
            self._emit_skill_event(
                'server_end',
                method_id=NotifyMethod.SYNC_SERVER_SKILL_END,
                skill_uuid=int(msg.SkillUuid or 0),
            )
        except Exception as e:
            logger.debug(f'[Parser] SyncServerSkillEnd decode error: {e}')

    def _on_sync_server_skill_stage_end(self, data: bytes):
        """Handle SyncServerSkillStageEnd (0x3004 / 12292).

        SyncServerSkillStageEnd proto:
            ServerSkillStageEnd SkillStageEndInfo = 1;
        ServerSkillStageEnd proto:
            int32 SkillUuid = 1;
            uint32 StageId = 2;
            uint32 NewStageId = 3;
            uint32 ConditionId = 4;
        """
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.SyncServerSkillStageEnd()
            msg.ParseFromString(data)
            info = msg.SkillStageEndInfo
            logger.debug(
                f'[Parser] SyncServerSkillStageEnd(pb2): skill_uuid={info.SkillUuid} '
                f'stage={info.StageId} new_stage={info.NewStageId} cond={info.ConditionId}'
            )
            _append_packet_debug('skill_stage_end', {
                'skill_uuid': int(info.SkillUuid or 0),
                'stage_id': int(info.StageId or 0),
                'new_stage_id': int(info.NewStageId or 0),
                'condition_id': int(info.ConditionId or 0),
            })
            self._emit_skill_event(
                'server_stage_end',
                method_id=NotifyMethod.SYNC_SERVER_SKILL_STAGE_END,
                skill_uuid=int(info.SkillUuid or 0),
                stage_id=int(info.StageId or 0),
                new_stage_id=int(info.NewStageId or 0),
                condition_id=int(info.ConditionId or 0),
            )
        except Exception as e:
            logger.debug(f'[Parser] SyncServerSkillStageEnd decode error: {e}')

    def _on_qte_begin(self, data: bytes):
        """Handle QteBegin (0x3001 / 12289) — QTE event start.
        Note: QteBegin not in compiled proto — uses _decode_fields.
        """
        try:
            outer = _decode_fields(data)
            qte_id = outer.get(1, [0])[0]
            qte_type = outer.get(2, [0])[0]
            logger.info(f'[Parser] QteBegin: id={qte_id} type={qte_type}')
            _append_packet_debug('qte_begin', {
                'qte_id': qte_id, 'qte_type': qte_type,
            })
        except Exception as e:
            logger.debug(f'[Parser] QteBegin decode error: {e}')


    # ── Dungeon / Scene notify handlers ──


    def _on_enter_scene(self, data: bytes):
        """Handle EnterScene (0x03) — authoritative scene attribute sync."""
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.EnterScene()
            msg.ParseFromString(data)
            info = msg.EnterSceneInfo
            scene_id = _extract_scene_basic_id_from_attrs(info.SceneAttrs)
            if scene_id <= 0:
                scene_id = _extract_scene_basic_id_from_attrs(info.SubsceneAttrs)
            player_uuid = int(info.PlayerEnt.Uuid or 0) if info.HasField('PlayerEnt') else 0
            if player_uuid and _is_player(player_uuid):
                player_uid = _uuid_to_uid(player_uuid)
                self._confirm_self_uid(player_uid, 'enter_scene_player_ent', player_uuid)
                if info.PlayerEnt.HasField('Attrs'):
                    self._process_attr_collection(player_uid, info.PlayerEnt.Attrs)
            if scene_id > 0:
                scene_key = ('enter_scene', int(scene_id), str(info.SceneGuid or ''), str(info.ConnectGuid or ''))
                if self._last_scene_id and scene_id != self._last_scene_id:
                    self._notify_soft_scene_transition(
                        'enter_scene_basic_id_changed',
                        old_scene_key=self._last_scene_key,
                        new_scene_key=scene_key,
                    )
                self._last_scene_id = scene_id
                self._last_scene_key = scene_key
                if self._current_uid and self._current_uid in self._players:
                    self._players[self._current_uid].scene_id = scene_id
                logger.info(
                    f'[Parser] EnterScene: scene_id={scene_id} scene_guid={info.SceneGuid!r} '
                    f'connect_guid={info.ConnectGuid!r} player_uuid={player_uuid}'
                )
                _append_packet_debug('enter_scene', {
                    'scene_id': scene_id,
                    'scene_guid': str(info.SceneGuid or ''),
                    'connect_guid': str(info.ConnectGuid or ''),
                    'player_uuid': player_uuid,
                })
                self._emit_dungeon_event(
                    'enter_scene',
                    scene_id=int(scene_id or 0),
                    scene_guid=str(info.SceneGuid or ''),
                    connect_guid=str(info.ConnectGuid or ''),
                    player_uuid=int(player_uuid or 0),
                    player_uid=int(self._current_uid or 0),
                )
            else:
                # 诊断: 开放大地图等场景常提取不到 SCENE_BASIC_ID(0x155) → 不发
                # enter_scene 事件 → 中央地图名横幅不弹。打印实际带的属性 id /
                # 是否含 PlayerEnt, 以便定位大地图把 scene_id 放在了哪个字段。
                # 仅在提取失败时触发, 不影响正常副本/活动中心路径。
                try:
                    def _attr_ids(coll):
                        return [int(getattr(a, 'Id', 0) or 0)
                                for a in (getattr(coll, 'Attrs', None) or [])][:24]
                    print(
                        f'[Parser][MapBanner] EnterScene 无 scene_id: '
                        f'scene_attr_ids={_attr_ids(info.SceneAttrs)} '
                        f'sub_attr_ids={_attr_ids(info.SubsceneAttrs)} '
                        f'scene_guid={str(info.SceneGuid or "")!r} '
                        f'has_player_ent={info.HasField("PlayerEnt")}',
                        flush=True,
                    )
                except Exception:
                    pass
        except Exception as e:
            logger.debug(f'[Parser] EnterScene decode error: {e}')


    def _on_sync_dungeon_data(self, data: bytes):
        """Handle SyncDungeonData (0x17) — dungeon context sync.

        SyncDungeonData proto:
            DungeonSyncData VData = 1;
        DungeonSyncData proto:
            int64 SceneUuid = 1;
            DungeonFlowInfo FlowInfo = 2;
            DungeonSettlement Settlement = 7;
            DungeonSceneInfo DungeonSceneInfo = 21;
            ...
        """
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.SyncDungeonData()
            msg.ParseFromString(data)
            vd = msg.VData
            scene_uuid = vd.SceneUuid
            dungeon_difficulty = 0
            if vd.HasField('DungeonSceneInfo'):
                dungeon_difficulty = int(vd.DungeonSceneInfo.Difficulty or 0)
            logger.info(f'[Parser] SyncDungeonData(pb2): scene_uuid={scene_uuid} last={self._last_dungeon_id}')
            target_debug = []
            if vd.HasField('Target'):
                for _key, target_data in vd.Target.TargetData.items():
                    target_debug.append({
                        'target_id': int(target_data.TargetId),
                        'nums': int(target_data.Nums),
                        'complete': int(target_data.Complete),
                    })
            _append_packet_debug(
                'dungeon_data',
                {
                    'scene_uuid': scene_uuid,
                    'last_dungeon_id': self._last_dungeon_id,
                    'dungeon_difficulty': dungeon_difficulty,
                    'targets': target_debug[:16],
                },
            )
            if target_debug:
                self._apply_dungeon_target_reset_rules(target_debug, 'sync_dungeon_data')
            dungeon_id = scene_uuid  # Use scene_uuid as dungeon identity
            if dungeon_id != self._last_dungeon_id and self._last_dungeon_id != 0:
                print(
                    f'[Parser] ⚡ 副本上下文切换: dungeon {self._last_dungeon_id} → {dungeon_id}, '
                    f'保留实体并等待下一次伤害确认',
                    flush=True,
                )
                self._notify_soft_scene_transition(
                    'sync_dungeon_scene_uuid_changed',
                    old_scene_key=('dungeon', self._last_dungeon_id),
                    new_scene_key=('dungeon', dungeon_id),
                )
            self._last_dungeon_id = dungeon_id
            self._emit_dungeon_event(
                'sync_dungeon_data',
                dungeon_id=int(dungeon_id or 0),
                scene_uuid=int(scene_uuid or 0),
                dungeon_difficulty=int(dungeon_difficulty or 0),
                targets=target_debug[:16],
            )
            if self._current_uid and self._current_uid in self._players:
                player = self._players[self._current_uid]
                if dungeon_id > 0:
                    player.dungeon_id = dungeon_id
                if dungeon_difficulty > 0 and dungeon_difficulty != player.dungeon_difficulty:
                    old_difficulty = player.dungeon_difficulty
                    player.dungeon_difficulty = dungeon_difficulty
                    if old_difficulty > 0:
                        self._notify_soft_scene_transition(
                            'dungeon_difficulty_changed',
                            old_scene_key=('dungeon_difficulty', old_difficulty),
                            new_scene_key=('dungeon_difficulty', dungeon_difficulty),
                        )
        except Exception as e:
            logger.debug(f'[Parser] SyncDungeonData decode error: {e}')

    def _on_sync_dungeon_dirty_data(self, data: bytes):
        """Handle SyncDungeonDirtyData (0x18) target progress updates."""
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.SyncDungeonDirtyData()
            msg.ParseFromString(data)
            buffer_data = bytes(msg.VData.Buffer or b'')
        except Exception as e:
            logger.debug(f'[Parser] SyncDungeonDirtyData pb2 decode error: {e}')
            return
        if not buffer_data:
            return
        try:
            flow_state, targets = _parse_dungeon_dirty_buffer(buffer_data)
        except Exception as e:
            logger.debug(f'[Parser] SyncDungeonDirtyData dirty buffer decode error: {e}')
            _append_packet_debug(
                'dungeon_dirty_decode_error',
                {'error': str(e), 'buffer_len': len(buffer_data)},
            )
            return
        logger.info(
            f'[Parser] SyncDungeonDirtyData: flow_state={flow_state} '
            f'targets={targets[:8]} len={len(buffer_data)}'
        )
        _append_packet_debug(
            'dungeon_dirty_data',
            {
                'flow_state': flow_state,
                'targets': targets[:16],
                'buffer_len': len(buffer_data),
            },
        )
        self._emit_dungeon_event(
            'sync_dungeon_dirty_data',
            dungeon_id=int(self._last_dungeon_id or 0),
            flow_state=int(flow_state or 0),
            targets=targets[:16],
            buffer_len=len(buffer_data),
        )
        if targets:
            self._apply_dungeon_target_reset_rules(targets, 'sync_dungeon_dirty_data')

    def _on_notify_start_playing_dungeon(self, data: bytes):
        """Handle NotifyStartPlayingDungeon (0x37) — dungeon play started.

        NotifyStartPlayingDungeon proto:
            int32 DungeonId = 1;
        """
        try:
            outer = _decode_fields(data)
            dungeon_id = outer.get(1, [0])[0]
            logger.info(f'[Parser] NotifyStartPlayingDungeon: dungeon_id={dungeon_id} last={self._last_dungeon_id}')
            _append_packet_debug('start_dungeon', {'dungeon_id': dungeon_id, 'last_dungeon_id': self._last_dungeon_id})
            # NotifyStartPlayingDungeon definitively means new dungeon, but the
            # capture layer already handles true cross-server hard resets. Keep
            # entity/max HP data and defer encounter reset until the next hit,
            # matching resonance-logs-cn's pending_auto_reset behavior.
            if dungeon_id != self._last_dungeon_id and self._last_dungeon_id != 0:
                print(
                    f'[Parser] ⚡ 开始副本: dungeon {self._last_dungeon_id} → {dungeon_id}, '
                    f'等下一次伤害再重置 DPS/BossHP',
                    flush=True,
                )
                self._notify_soft_scene_restart(
                    f'notify_start_playing_dungeon:{self._last_dungeon_id}->{dungeon_id}'
                )
            elif dungeon_id == self._last_dungeon_id and self._last_dungeon_id != 0:
                # Same dungeon retry (刷本): UUID 不变, 需要重置死亡状态
                # 不调用 reset_scene() 以保留 max_hp 缓存,
                # 仅清除 is_dead 让 boss HP 面板可以重新呼出。
                # 但上层 DPS/BossHP 仍需软重置: 旧 boss_uuid / recent target
                # 如果保留, 同场景再进时 DPS 可能一直按旧 boss 过滤为 0。
                revived = 0
                for m in self._monsters.values():
                    if m.is_dead:
                        m.is_dead = False
                        m.hp = 0  # 等待新 HP 数据
                        m.last_update = time.time()
                        revived += 1
                if revived:
                    print(
                        f'[Parser] ♻ 同副本重开: dungeon={dungeon_id}, '
                        f'重置 {revived} 个死亡单位',
                        flush=True,
                    )
                    logger.info(
                        f'[Parser] Same dungeon restart: reset {revived} dead monsters '
                        f'dungeon_id={dungeon_id}'
                    )
                self._notify_soft_scene_restart(f'dungeon_id={dungeon_id}')
            self._last_dungeon_id = dungeon_id
            self._emit_dungeon_event(
                'start_playing_dungeon',
                dungeon_id=int(dungeon_id or 0),
            )
            if self._current_uid and self._current_uid in self._players:
                player = self._players[self._current_uid]
                player.dungeon_id = dungeon_id
        except Exception as e:
            logger.debug(f'[Parser] NotifyStartPlayingDungeon decode error: {e}')


    # ── Login / Session notify handlers ──


    def _on_enter_game(self, data: bytes):
        """Handle EnterGame (0x14) — login notification.

        EnterGame proto:
            int64 Uid = 1;
            string ServerName = 2;
        """
        try:
            outer = _decode_fields(data)
            uid_raw = outer.get(1, [0])[0]
            uid = _varint_to_int64(uid_raw) if isinstance(uid_raw, int) else 0
            server_raw = outer.get(2, [None])[0]
            server_name = server_raw.decode('utf-8', 'ignore') if isinstance(server_raw, bytes) else ''
            logger.info(f'[Parser] EnterGame: uid={uid} server={server_name!r}')
            print(f'[Parser] EnterGame: uid={uid} server={server_name!r}', flush=True)
            _append_packet_debug('enter_game', {
                'uid': uid, 'server_name': server_name,
            })
            # 重置同步计数: 重新登录时需要接收新的 full sync
            if self._sync_container_count > 0:
                logger.info(
                    f'[Parser] EnterGame 重置 sync_container_count '
                    f'({self._sync_container_count} → 0)'
                )
                self._sync_container_count = 0
            if uid > 0:
                self._confirm_self_uid(uid, 'enter_game')
                logger.info(f'[Parser] confirmed self UID from EnterGame: {uid}')
        except Exception as e:
            logger.debug(f'[Parser] EnterGame decode error: {e}')

    def _on_notify_revive_user(self, data: bytes):
        """Handle NotifyReviveUser (0x27) — player revived.

        NotifyReviveUser proto:
            int64 VActorUuid = 1;
        """
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.NotifyReviveUser()
            msg.ParseFromString(data)
            uuid = msg.VActorUuid
            logger.info(f'[Parser] NotifyReviveUser(pb2): uuid={uuid}')
            _append_packet_debug('revive_user', {'uuid': uuid})
            # Reset dead state for player
            if _is_player(uuid):
                uid = _uuid_to_uid(uuid)
                if uid in self._players:
                    player = self._players[uid]
                    player.hp = player.max_hp  # Assume full HP on revive
                    if self._is_confirmed_self_uid(uid):
                        self._notify_self()
        except Exception as e:
            logger.debug(f'[Parser] NotifyReviveUser decode error: {e}')

    def _on_notify_team_generic(self, notify_name: str, data: bytes):
        """Handle team-related notify packets (AllMemberReady, CaptainReady, MatchResult).

        These packets may carry team roster data. We log the raw payload and
        attempt to decode it as CharTeam or other team-related messages.
        """
        print(
            f'[Parser] 收到队伍通知: {notify_name} (len={len(data)})',
            flush=True,
        )
        logger.info(f'[Parser] Team notify {notify_name}: len={len(data)} hex={data.hex()[:256]}')
        _append_packet_debug(f'team_notify_{notify_name}', {
            'raw_hex': data.hex()[:1024],
            'raw_len': len(data),
        })
        # Attempt proto decode as CharTeam (some team notifies may wrap CharTeam)
        pb = _ensure_pb()
        if pb and len(data) > 4:
            for msg_cls_name in ('CharTeam',):
                try:
                    msg_cls = getattr(pb, msg_cls_name, None)
                    if not msg_cls:
                        continue
                    tmsg = msg_cls()
                    tmsg.ParseFromString(data)
                    char_ids = list(tmsg.CharIds)
                    if char_ids:
                        print(
                            f'[Parser] {notify_name} decoded as {msg_cls_name}: '
                            f'team_id={tmsg.TeamId} char_ids={char_ids} '
                            f'member_keys={list(tmsg.TeamMemberData.keys())}',
                            flush=True,
                        )
                        self._process_char_team(self._current_uid, tmsg)
                        return
                except Exception:
                    pass
            # If not CharTeam, log what we can
            logger.info(f'[Parser] {notify_name}: not decodable as CharTeam, raw logged')

    def _on_sync_container_data(self, data: bytes):
        """SyncContainerData { CharSerialize VData = 1 } — 纯 pb2 解析."""
        pb = _ensure_pb()
        if not pb:
            logger.error('[Parser] SyncContainerData: pb2 模块未加载，跳过')
            return

        try:
            msg = pb.SyncContainerData()
            msg.ParseFromString(data)
            char = msg.VData
        except Exception as e:
            logger.error(f'[Parser] SyncContainerData pb2 解析失败: {e}')
            return

        uid = char.CharId
        if uid <= 0:
            return

        logger.info(f'[Parser] SyncContainerData received: uid={uid}, current_uid={self._current_uid}')
        print(f'[Parser] SyncContainerData: uid={uid}, current_uid={self._current_uid}', flush=True)

        # Track how many SyncContainerData we've received.
        # First one = login. Subsequent ones = scene/dungeon transition re-sync.
        self._sync_container_count += 1

        # SyncContainerData is self-scoped full sync; it confirms UID even when
        # a cached tentative UID was wrong.
        self._confirm_self_uid(uid, 'sync_container')
        if self._current_uid == uid:
            logger.info(f'[Parser] confirmed self UID from SyncContainerData: {uid}')
        if self._sync_container_count > 1 and uid == self._current_uid:
            # NOTE: Do NOT call reset_scene() here!
            # SyncContainerData arrives AFTER SyncNearEntities has already populated
            # new monsters with full AttrCollection (including MAX_HP).
            # Calling reset_scene() here would wipe those monsters.
            # Subsequent SyncNearDelta only sends CHANGED attrs (HP changes but
            # MAX_HP is NOT re-sent) → monsters get recreated with max_hp=0
            # → boss bar check `max_hp > 0` fails → bar never shows.
            #
            # Scene resets are handled by:
            #   - Capture layer _on_server_change (cross-server transitions)
            #   - SyncDungeonData (dungeon_id change)
            #   - NotifyStartPlayingDungeon (dungeon start)
            print(
                f'[Parser] SyncContainerData 重新同步 (第{self._sync_container_count}次), '
                f'保留 {len(self._monsters)} 个怪物 (不重置场景)',
                flush=True,
            )
            self._notify_soft_scene_restart(f'sync_container_count={self._sync_container_count}')

        player = self._get_player(uid)
        changed = False

        # CharBase (field 2)
        if char.HasField('CharBase'):
            cb = char.CharBase
            if cb.Name:
                player.name = cb.Name
                changed = True
                logger.info(f'[Parser] SyncContainerData CharBase Name={cb.Name!r} uid={uid}')
            if cb.FightPoint > 0:
                player.fight_point = cb.FightPoint
                changed = True
            # ── CharTeam (field 20) — parse team members ──
            if cb.HasField('TeamInfo'):
                ti = cb.TeamInfo
                logger.info(
                    f'[Parser] SyncContainerData CharTeam: '
                    f'team_id={ti.TeamId} char_ids={list(ti.CharIds)} '
                    f'member_data_keys={list(ti.TeamMemberData.keys())}'
                )
                self._process_char_team(uid, ti)
            else:
                logger.info(f'[Parser] SyncContainerData: No TeamInfo in CharBase')

        # UserFightAttr (field 16)
        attr_resource_ids = []
        attr_resources = []
        attr_skill_cd_count = 0
        if char.HasField('Attr'):
            a = char.Attr
            # CurHp (field 1)
            if a.CurHp != 0:
                player.hp = a.CurHp
                player.hp_from_full_sync = True
                changed = True
            # MaxHp (field 2)
            if a.MaxHp != 0:
                player.max_hp = a.MaxHp
                changed = True
            # OriginEnergy (field 3, float)
            new_energy = float(a.OriginEnergy)
            new_energy_valid = math.isfinite(new_energy) and new_energy >= 0.0
            if (
                abs(new_energy - float(getattr(player, 'energy', 0.0) or 0.0)) > 0.001 or
                bool(getattr(player, 'energy_valid', False)) != bool(new_energy_valid)
            ):
                player.energy = new_energy
                player.energy_valid = bool(new_energy_valid)
                player.energy_source_priority = 99 if new_energy_valid else 0
                changed = True
            # ResourceIds / Resources (fields 4, 5)
            attr_resource_ids = list(a.ResourceIds)
            attr_resources = list(a.Resources)
            resource_values = _decode_resource_value_map(attr_resource_ids, attr_resources)
            if resource_values != player.resource_values:
                player.resource_values = resource_values
                changed = True
            # CdInfo (field 9, repeated SkillCDInfo)
            skill_cds = []
            for cd in a.CdInfo:
                if cd.SkillLevelId > 0:
                    # UserFightAttr sends SkillCDInfo (VCD at field 8).
                    # Also check field 5 (ValidCDTimeLegacy) for safety.
                    vcd_legacy = max(0, cd.ValidCDTimeLegacy)  # field 5
                    vcd_info   = max(0, cd.ValidCDTime)        # field 8
                    effective_vcd = vcd_info if vcd_info > 0 else vcd_legacy
                    skill_cds.append({
                        'skill_level_id': cd.SkillLevelId,
                        'begin_time': cd.SkillBeginTime,
                        'duration': cd.Duration,
                        'skill_cd_type': cd.SkillCDType,
                        'valid_cd_time': effective_vcd,
                        'charge_count': max(0, cd.ChargeCount),
                        'sub_cd_ratio': max(0, cd.SubCDRatio),
                        'sub_cd_fixed': max(0, int(cd.SubCDFixed)),
                        'accelerate_cd_ratio': max(0, cd.AccelerateCDRatio),
                    })
            attr_skill_cd_count = len(skill_cds)
            if self._replace_skill_cds(player, skill_cds):
                changed = True
            if _refresh_stamina_resource(player):
                changed = True

        # EnergyItem (field 13)
        energy_item = {
            'unlock_nums': [],
            'energy_info_map': {},
        }
        if char.HasField('EnergyItem'):
            ei = char.EnergyItem
            energy_limit = ei.EnergyLimit
            extra_energy_limit = ei.ExtraEnergyLimit
            energy_info_map = {}
            energy_values = []
            unlock_nums = []
            for eid, einfo in ei.EnergyInfo.items():
                ev = einfo.EnergyValue
                un = einfo.UnlockNum
                entry_info = {
                    'energy_value': ev,
                    'unlock_num': un,
                    'item_info_count': len(einfo.EnergyItemInfo),
                }
                if ev >= 0:
                    energy_values.append(ev)
                if un >= 0:
                    unlock_nums.append(un)
                if eid > 0:
                    energy_info_map[eid] = entry_info
            energy_item = {
                'unlock_nums': unlock_nums,
                'energy_info_map': energy_info_map,
            }
            if energy_info_map != player.energy_info_map:
                player.energy_info_map = dict(energy_info_map)
                changed = True
            total_limit = energy_limit + extra_energy_limit
            if total_limit > 0:
                player.energy_limit = energy_limit
                player.extra_energy_limit = extra_energy_limit
                changed = True
                logger.info(
                    f'[Parser] SyncContainerData EnergyItem '
                    f'limit={energy_limit} extra={extra_energy_limit} uid={uid}'
                )
            else:
                sane_limit = max(energy_values) if energy_values else 0
                if sane_limit > 0:
                    player.energy_limit = sane_limit
                    player.extra_energy_limit = 0
                    changed = True
                    logger.info(
                        f'[Parser] SyncContainerData EnergyItem derived_limit='
                        f'{sane_limit} uid={uid}'
                    )
            cur_energy = max(energy_values) if energy_values else 0
            if cur_energy > 0:
                player.energy_info_value = cur_energy
            if _refresh_stamina_resource(player):
                changed = True

        # ── Season / Level-related fields (50, 52, 56, 86, 102) ──
        # SeasonCenter (field 50) → BattlePass.Level
        battlepass_level = 0
        if char.HasField('SeasonCenter') and char.SeasonCenter.HasField('BattlePass'):
            battlepass_level = char.SeasonCenter.BattlePass.Level
        # SeasonMedalInfo (field 52) → CoreHoleInfo.HoleLevel
        season_medal_level = 0
        if char.HasField('SeasonMedalInfo') and char.SeasonMedalInfo.HasField('CoreHoleInfo'):
            raw_level = char.SeasonMedalInfo.CoreHoleInfo.HoleLevel
            season_medal_level = _normalize_season_medal_level(raw_level)
            if season_medal_level > 0:
                logger.info(
                    f'[Parser] SeasonMedalInfo core_raw={raw_level} '
                    f'core_norm={season_medal_level}'
                )
        # MonsterHuntInfo (field 56) → CurLevel
        monster_hunt_level = 0
        if char.HasField('MonsterHuntInfo'):
            monster_hunt_level = char.MonsterHuntInfo.CurLevel
        # BattlePassData (field 86) → max BattlePass.Level across entries
        battlepass_data_level = 0
        if char.HasField('BattlePassData'):
            bp_levels = [bp.Level for bp in char.BattlePassData.BattleMap.values() if bp.Level > 0]
            battlepass_data_level = max(bp_levels) if bp_levels else 0
        # DeepSleepResonance (field 102) → season_type==3 entry
        deep_sleep_level = 0
        deep_sleep_exp = 0
        if char.HasField('DeepSleepResonance'):
            for entry in char.DeepSleepResonance.Entries:
                if entry.SeasonType == 3 and entry.HasField('Info'):
                    ds_lv = entry.Info.Level
                    ds_exp = entry.Info.CurExp
                    if ds_lv > 0:
                        deep_sleep_level = ds_lv
                        deep_sleep_exp = ds_exp

        # Apply all level-related values (common for both paths)
        player.season_medal_level = season_medal_level
        player.monster_hunt_level = monster_hunt_level
        player.battlepass_level = battlepass_level
        player.battlepass_data_level = battlepass_data_level
        player.season_level = max(season_medal_level, monster_hunt_level)
        if _set_level_extra_candidate(player, 'season_medal', season_medal_level):
            changed = True
        if _set_level_extra_candidate(player, 'monster_hunt', monster_hunt_level):
            changed = True
        if _set_level_extra_candidate(player, 'battlepass', battlepass_level):
            changed = True
        if _set_level_extra_candidate(player, 'battlepass_data', battlepass_data_level):
            changed = True
        if deep_sleep_level > 0:
            if _set_level_extra_candidate(player, 'deep_sleep', deep_sleep_level):
                changed = True
            print(
                f'[Parser] SyncContainerData: 深眠心相仪等级 Lv.{deep_sleep_level} '
                f'经验={deep_sleep_exp} uid={uid}',
                flush=True,
            )
            _append_packet_debug('deep_sleep_level', {
                'uid': uid,
                'deep_sleep_level': deep_sleep_level,
                'deep_sleep_exp': deep_sleep_exp,
            })

        # ── Full dump of ALL CharSerialize fields for deep analysis (pb2) ──
        full_sync_dump = {'uid': uid, 'field_count': 0, 'fields': {}}
        if _helpers._MessageToDict:
            try:
                all_fields = _helpers._MessageToDict(char, preserving_proto_field_name=True)
                for fk, fv in all_fields.items():
                    if fk == 'CharId':
                        continue
                    full_sync_dump['fields'][fk] = fv
            except Exception as e:
                logger.debug(f'[Parser] MessageToDict error: {e}')
        else:
            # Fallback: list field names only
            for fd in char.DESCRIPTOR.fields:
                if fd.number == 1:
                    continue
                try:
                    if fd.message_type and char.HasField(fd.name):
                        full_sync_dump['fields'][fd.name] = f'<{fd.message_type.name}>'
                except (ValueError, AttributeError):
                    pass
        full_sync_dump['field_count'] = len(full_sync_dump['fields'])
        _append_packet_debug('sync_container_full_dump', full_sync_dump)

        _append_packet_debug(
            'sync_container_data',
            {
                'uid': uid,
                'energy_limit': player.energy_limit,
                'extra_energy_limit': player.extra_energy_limit,
                'energy': player.energy,
                'energy_valid': player.energy_valid,
                'energy_info_value': player.energy_info_value,
                'energy_info_map': player.energy_info_map,
                'energy_unlock_nums': energy_item.get('unlock_nums', []),
                'resource_values': player.resource_values,
                'stamina_resource_id': player.stamina_resource_id,
                'level_extra': player.level_extra,
                'level_extra_source': player.level_extra_source,
                'season_level': player.season_level,
                'season_medal_level': season_medal_level,
                'monster_hunt_level': monster_hunt_level,
                'battlepass_level': battlepass_level,
                'battlepass_data_level': battlepass_data_level,
                'attr_resource_ids': attr_resource_ids,
                'attr_resources': attr_resources,
                'attr_skill_cd_count': attr_skill_cd_count,
            }
        )

        # RoleLevel (field 22)
        if char.HasField('RoleLevel'):
            rl = char.RoleLevel
            role_level = rl.Level
            prev_season_max_lv = rl.PrevSeasonMaxLv
            role_level_debug = {
                'uid': uid,
                'role_level': role_level,
                'prev_season_max_lv': prev_season_max_lv,
                'last_season_day': rl.LastSeasonDay,
                'bless_exp_pool': rl.BlessExpPool,
                'grant_bless_exp': rl.GrantBlessExp,
                'accumulate_bless_exp': rl.AccumulateBlessExp,
                'accumulate_exp': rl.AccumulateExp,
            }
            logger.info(
                f'[Parser] SyncContainerData RoleLevel(pb2) '
                f'role_level={role_level}, prev_season_max_lv={prev_season_max_lv}, uid={uid}'
            )
            _append_packet_debug('role_level', role_level_debug)
            if role_level > 0:
                # 生体元等级 base is capped; a higher value is season-inclusive.
                player.level = min(role_level, BASE_LEVEL_CAP)
                changed = True
                print(f'[Parser] SyncContainerData: 等级 Lv.{player.level} uid={uid}', flush=True)
            else:
                logger.warning(
                    f'[Parser] SyncContainerData RoleLevel(pb2): level=0! uid={uid}'
                )

        # Slots (field 55) — full skill bar layout including resonance slots 7,8
        if char.HasField('Slots'):
            slot_bar = {}
            for slot_id, si in char.Slots.Slots.items():
                sid = si.SkillId
                if slot_id > 0 and sid > 0:
                    slot_bar[slot_id] = sid
            if slot_bar and slot_bar != player.slot_bar_map:
                player.slot_bar_map = dict(slot_bar)
                changed = True
                logger.info(f'[Parser] SyncContainerData Slots(55/pb2) bar={slot_bar} uid={uid}')
                _append_packet_debug(
                    'slot_bar',
                    {'uid': uid, 'slot_bar_map': slot_bar}
                )

        # ProfessionList (field 61)
        if char.HasField('ProfessionList'):
            pl = char.ProfessionList
            cur_prof_id = pl.CurProfessionId

            # Build all_skill_info from AoyiSkillInfoMap (field 7)
            all_skill_info: Dict[int, Dict[str, int]] = {}
            for key, psi in pl.AoyiSkillInfoMap.items():
                all_skill_info[key] = {
                    'skill_id': psi.SkillId,
                    'level': psi.Level,
                }

            # Find current profession data from ProfessionList_ (field 4)
            current_prof = None
            profession_entries = list(pl.ProfessionList_.items())
            for pid_key, pinfo in profession_entries:
                if pid_key == cur_prof_id:
                    current_prof = pinfo
                    break
            if current_prof is None and len(profession_entries) == 1:
                cur_prof_id = profession_entries[0][0]
                current_prof = profession_entries[0][1]

            profession_data: Dict[str, Any] = {
                'profession_id': cur_prof_id,
                'slot_skill_level_map': {},
                'active_skill_ids': [],
                'skill_info_map': {},
                'skill_level_info_map': {},
            }

            if current_prof is not None:
                # Current profession's SkillInfoMap (field 4 of ProfessionInfo)
                current_skill_info: Dict[int, Dict[str, int]] = {}
                for sk, psi in current_prof.SkillInfoMap.items():
                    current_skill_info[sk] = {
                        'skill_id': psi.SkillId,
                        'level': psi.Level,
                    }
                for sk, info in all_skill_info.items():
                    current_skill_info.setdefault(sk, info)

                profession_data['skill_info_map'] = current_skill_info

                # Build skill_level_info_map
                skill_level_info_map: Dict[int, Dict[str, int]] = {}
                for info_key, info in current_skill_info.items():
                    skill_id = int(info.get('skill_id') or info_key or 0)
                    skill_level_id = _compose_skill_level_id(skill_id, info.get('level', 0))
                    if skill_level_id <= 0:
                        skill_level_id = int(info_key or 0)
                    if skill_level_id > 0:
                        skill_level_info_map[skill_level_id] = {
                            'skill_id': skill_id,
                            'level': int(info.get('level', 0) or 0),
                        }
                profession_data['skill_level_info_map'] = skill_level_info_map

                profession_data['active_skill_ids'] = list(current_prof.ActiveSkillIds)

                # SlotSkillInfoMap (field 7 of ProfessionInfo) → map<int32, int32>
                raw_slot_map = dict(current_prof.SlotSkillInfoMap)
                normalized_slot_map: Dict[int, int] = {}
                for slot, mapped_id in raw_slot_map.items():
                    if slot < 0 or mapped_id <= 0:
                        continue
                    skill_info = current_skill_info.get(mapped_id)
                    skill_level_id = 0
                    if skill_info:
                        skill_level_id = _compose_skill_level_id(mapped_id, skill_info.get('level', 0))
                    if skill_level_id <= 0:
                        for ik, iv in current_skill_info.items():
                            if iv.get('skill_id') == mapped_id:
                                skill_level_id = _compose_skill_level_id(ik, iv.get('level', 0))
                                break
                    if skill_level_id <= 0:
                        skill_level_id = mapped_id
                    normalized_slot_map[slot] = skill_level_id
                    if skill_level_id > 0 and skill_level_id not in skill_level_info_map:
                        si = current_skill_info.get(mapped_id) or {}
                        skill_level_info_map[skill_level_id] = {
                            'skill_id': int(si.get('skill_id') or mapped_id or 0),
                            'level': int(si.get('level', 0) or 0),
                        }
                profession_data['slot_skill_level_map'] = normalized_slot_map

            pid = profession_data['profession_id']
            if pid > 0:
                player.profession_id = pid
                player.profession = PROFESSION_NAMES.get(pid, '')
                changed = True
            slot_skill_map = profession_data.get('slot_skill_level_map') or {}
            skill_level_info_map = profession_data.get('skill_level_info_map') or {}
            if pid > 0 and slot_skill_map:
                self._profession_skill_cache[pid] = dict(slot_skill_map)
            elif pid > 0 and not slot_skill_map:
                slot_skill_map = self._profession_skill_cache.get(pid) or {}
            if slot_skill_map != player.skill_slot_map:
                player.skill_slot_map = dict(slot_skill_map)
                changed = True
            merged_skill_info = dict(getattr(player, 'skill_level_info_map', {}) or {})
            merged_skill_info.update(skill_level_info_map)
            if merged_skill_info != player.skill_level_info_map:
                player.skill_level_info_map = merged_skill_info
                changed = True
            _append_packet_debug(
                'profession_list',
                {
                    'uid': uid,
                    'profession_id': pid,
                    'slot_skill_level_map': slot_skill_map,
                    'active_skill_ids': profession_data.get('active_skill_ids') or [],
                    'skill_info_keys': sorted((profession_data.get('skill_info_map') or {}).keys()),
                    'skill_level_info_keys': sorted((profession_data.get('skill_level_info_map') or {}).keys()),
                }
            )

        # ── Parse ALL remaining CharSerialize fields via pb2 ──
        # Specific useful fields get dedicated pb2 decoders:
        #   3 (SceneData) → scene_id (MapId)
        #   6 (BuffDBInfo) → buff_list
        #   12 (EquipList) → equip info
        #   15 (DungeonList) → dungeon data
        #   28 (Resonance) → resonance data

        # SceneData (field 3) — current scene/map
        if char.HasField('SceneData'):
            sd = char.SceneData
            new_scene_id = sd.MapId
            scene_key = (int(sd.MapId or 0), int(sd.ChannelId or 0),
                         int(sd.PlaneId or 0), int(sd.SceneLayer or 0))
            scene = {
                'map_id': sd.MapId,
                'channel_id': sd.ChannelId,
                'plane_id': sd.PlaneId,
                'scene_layer': sd.SceneLayer,
            }
            if new_scene_id > 0:
                # 主城/开放大地图(阿斯特里斯=8 / 巴哈马尔高原=9 等)的 scene_id 走
                # 这条 SceneData.MapId, 而非 EnterScene 的 SCENE_BASIC_ID(0x155)。
                # 记录是否真的换了 MapId, 用于补发场景事件(供中央地图名横幅 / ACT)。
                map_id_changed = (int(new_scene_id) != int(self._last_scene_id or 0))
                player.scene_id = new_scene_id
                changed = True
                if (self._last_scene_key is not None
                        and scene_key != self._last_scene_key):
                    logger.info(
                        f'[Parser] 场景变更: {self._last_scene_key} → {scene_key} uid={uid}'
                    )
                    print(
                        f'[Parser] ⚡ 地图/副本小地图切换: {self._last_scene_key} → {scene_key}',
                        flush=True,
                    )
                    # 同服地图/副本分层切换: 不 reset_scene (SyncNearEntities 已填充新怪物),
                    # 也不要硬清 UI DPS/BossHP。长副本里 layer/plane 变化可能发生在
                    # Boss 机制/转阶段期间，硬 reset 会把输出和 BossHP 误收掉。
                    # 有些副本小地图 MapId 不变, 只变 channel/plane/layer;
                    # 仅看 MapId 会漏掉第二次进图的软重置。
                    self._notify_soft_scene_transition(
                        'scene_key_changed',
                        old_scene_key=self._last_scene_key,
                        new_scene_key=scene_key,
                    )
                self._last_scene_id = new_scene_id
                self._last_scene_key = scene_key
                # 补发场景事件: 开放地图/主城不发 SyncDungeonData、EnterScene 的
                # SCENE_BASIC_ID 又常为空 → 横幅/ACT 一直拿不到地图名。这里在 MapId
                # 变化时发 dungeon 事件(scene_id=MapId), 让 _on_dungeon_event 用
                # names.dungeon(scene_id) 解析出地图名 → 中央横幅可弹。仅在真正换图
                # 时触发, 同图重复 full-sync 不会重发, 配合 UI 5s 去重不会刷屏。
                if map_id_changed:
                    self._emit_dungeon_event(
                        'container_scene',
                        scene_id=int(new_scene_id),
                        cur_map_id=int(new_scene_id),
                        channel_id=int(sd.ChannelId or 0),
                        plane_id=int(sd.PlaneId or 0),
                        scene_layer=int(sd.SceneLayer or 0),
                    )
            player.extended_data['SCENE_DATA'] = scene
            logger.info(f'[Parser] CharSerialize SceneData(pb2): {scene} uid={uid}')

        # BuffDBInfo (field 6) — active buffs on self
        if char.HasField('BuffInfo'):
            bi = char.BuffInfo
            buffs = []
            for buf_id, buf_data in bi.AllBuffDbData.items():
                buffs.append({
                    'buff_id': buf_id,
                    'config_id': buf_data.BuffConfigId,
                    'level': buf_data.Level,
                    'layer': buf_data.Layer,
                    'duration': buf_data.Duration,
                })
            player.buff_list = buffs
            player.extended_data['BUFF_DB_INFO'] = {'buff_count': len(buffs)}
            changed = True
            logger.info(f'[Parser] CharSerialize BuffDBInfo(pb2): {len(buffs)} buffs uid={uid}')

        # EquipList (field 12) — equipped items
        if char.HasField('Equip'):
            eq = char.Equip
            equip_count = len(eq.EquipList_)
            player.extended_data['EQUIP_LIST'] = {'equip_count': equip_count}
            logger.info(f'[Parser] CharSerialize EquipList(pb2): {equip_count} items uid={uid}')

        # DungeonList (field 15) — dungeon data
        if char.HasField('DungeonList'):
            dl = char.DungeonList
            dungeon_data = {
                'complete_count': len(dl.CompleteDungeon),
                'reset_time': dl.ResetTime,
            }
            player.extended_data['DUNGEON_LIST'] = dungeon_data
            logger.info(f'[Parser] CharSerialize DungeonList(pb2): {dungeon_data} uid={uid}')

        # Resonance (field 28) — resonance system
        if char.HasField('Resonance'):
            res = char.Resonance
            res_data = {
                'installed': res.Installed,
                'resonance_count': len(res.Resonances),
            }
            player.extended_data['RESONANCE'] = res_data
            logger.info(f'[Parser] CharSerialize Resonance(pb2): {res_data} uid={uid}')

        # Generic pb2 decode for ALL other CharSerialize fields
        _generic_decoded_fields = []
        for _fd in char.DESCRIPTOR.fields:
            if _fd.number <= 1:
                continue  # skip CharId
            if _fd.number in _HANDLED_CHAR_FIELDS:
                continue
            try:
                if _fd.message_type and char.HasField(_fd.name):
                    sub_msg = getattr(char, _fd.name)
                    if _helpers._MessageToDict:
                        player.extended_data[_fd.name] = _helpers._MessageToDict(
                            sub_msg, preserving_proto_field_name=True
                        )
                    else:
                        player.extended_data[_fd.name] = f'<{_fd.message_type.name}>'
                    _generic_decoded_fields.append(f'{_fd.name}({_fd.number})')
            except (ValueError, AttributeError):
                pass

        if _generic_decoded_fields:
            logger.info(
                f'[Parser] CharSerialize generic decoded {len(_generic_decoded_fields)} fields: '
                f'{", ".join(_generic_decoded_fields[:20])} uid={uid}'
            )
            _append_packet_debug('char_serialize_generic', {
                'uid': uid,
                'decoded_fields': _generic_decoded_fields,
            })

        if changed:
            if self._is_confirmed_self_uid(uid):
                self._notify_self()
            print(
                f'[Parser] SyncContainerData 完成: uid={uid} Lv.{player.level} '
                f'HP={player.hp}/{player.max_hp} 职业={player.profession!r}({player.profession_id}) '
                f'slots={len(player.skill_slot_map)} slot_bar={len(player.slot_bar_map)} '
                f'cd_count={len(player.skill_cd_map)} energy={player.energy:.1f}',
                flush=True,
            )


    # SyncContainerDirtyData (incremental updates)


    def _on_sync_container_dirty(self, data: bytes):
        """Handle the custom dirty-data stream wrapper."""
        if not self._current_uid_confirmed:
            self._queue_pending_self_dirty_packet(data)
            return

        outer = _decode_fields(data)
        buf_raw = outer.get(1, [None])[0]
        if not isinstance(buf_raw, bytes):
            return
        buf_fields = _decode_fields(buf_raw)
        buf_bytes = buf_fields.get(1, [None])[0]
        if not isinstance(buf_bytes, bytes) or len(buf_bytes) < 8:
            return

        self._parse_dirty_stream(buf_bytes)

    @_probe.decorate('parser._parse_dirty_stream')
    def _parse_dirty_stream(self, data: bytes):
        """Parse the custom dirty-data binary stream used by V3.3.6."""
        pos = 0
        uid = self._current_uid
        player = self._get_player(uid)
        changed = False


        if pos + 8 > len(data):
            return
        ident = _CY_PACKET.read_le_u32_at(data, pos)
        if ident != 0xFFFFFFFE:
            return
        pos += 4
        # skip validation int32BE
        pos += 4

        if pos + 4 > len(data):
            return
        field_index = _CY_PACKET.read_le_u32_at(data, pos)
        pos += 4
        _fname = CHAR_FIELD_NAMES.get(field_index, f'FIELD_{field_index}')
        debug_info = {
            'uid': uid,
            'field_index': field_index,
        }
        # Track DirtyData field distribution
        if not hasattr(self, '_dirty_field_counts'):
            self._dirty_field_counts = {}
        self._dirty_field_counts[field_index] = self._dirty_field_counts.get(field_index, 0) + 1
        total_dirty = sum(self._dirty_field_counts.values())
        if total_dirty <= 5 or total_dirty % 50 == 0:
            logger.info(f'[Parser] DirtyData field={field_index}({_fname}) total_dirty={total_dirty} dist={dict(sorted(self._dirty_field_counts.items()))}')

        if field_index == 2:  # CharBase - fully parsed for ALL sub_fields from proto CharBaseInfo
            debug_info['field_name'] = 'CharBase'
            # Dump full raw hex for deep analysis
            debug_info['full_raw_hex'] = data[pos:].hex()[:256]
            debug_info['remaining_len'] = len(data) - pos
            ok, pos, sub_field = _parse_dirty_subfield_header(data, pos)
            if not ok:
                return
            debug_info['sub_field'] = sub_field

            if sub_field in _DIRTY_CHARBASE_FIELDS:
                fname, ftype = _DIRTY_CHARBASE_FIELDS[sub_field]
                debug_info['sub_name'] = fname
                if ftype == 'str':
                    if pos + 4 > len(data):
                        debug_info['tail_hex'] = data[pos:].hex()
                    else:
                        slen = _CY_PACKET.read_le_u32_at(data, pos)
                        pos += 4
                        if pos + slen <= len(data):
                            val = data[pos:pos+slen].decode('utf-8', 'ignore')
                            pos += slen
                            debug_info['value'] = val
                            if fname == 'Name':
                                player.name = val
                                changed = True
                        else:
                            debug_info['tail_hex'] = data[pos:].hex()
                elif ftype == 'I':
                    if pos + 4 <= len(data):
                        val = _CY_PACKET.read_le_u32_at(data, pos)
                        pos += 4
                        debug_info['u32'] = val
                        if fname == 'FightPoint':
                            player.fight_point = val
                            changed = True
                    else:
                        debug_info['tail_hex'] = data[pos:].hex()
                elif ftype == 'Q':
                    if pos + 8 <= len(data):
                        val = _CY_PACKET.read_le_u64_at(data, pos)
                        pos += 8
                        debug_info['u64'] = val
                    else:
                        debug_info['tail_hex'] = data[pos:].hex()
                elif ftype == 'f':
                    if pos + 4 <= len(data):
                        val = _CY_PACKET.read_le_f32_at(data, pos)
                        pos += 4
                        debug_info['float'] = round(val, 4)
                    else:
                        debug_info['tail_hex'] = data[pos:].hex()
                elif ftype == 'B':
                    if pos + 1 <= len(data):
                        val = data[pos]
                        pos += 1
                        debug_info['bool'] = bool(val)
                    else:
                        debug_info['tail_hex'] = data[pos:].hex()
                elif ftype in ('msg', 'rep_i32'):
                    # For message/repeated types, dump remaining raw bytes
                    debug_info['msg_hex'] = data[pos:].hex()[:256]
                    # ── Parse CharTeam (sub_field 20) via pb2 ──
                    if sub_field == 20 and ftype == 'msg':
                        try:
                            pb = _ensure_pb()
                            if pb:
                                team_msg = pb.CharTeam()
                                team_msg.ParseFromString(data[pos:])
                                logger.info(
                                    f'[Parser] DirtyData CharTeam received: '
                                    f'team_id={team_msg.TeamId} char_ids={list(team_msg.CharIds)} '
                                    f'member_data_keys={list(team_msg.TeamMemberData.keys())}'
                                )
                                self._process_char_team(self._current_uid, team_msg)
                        except Exception as e:
                            print(f'[Parser] DirtyData CharTeam 解析失败: {e}', flush=True)
                            logger.warning(f'[Parser] DirtyData CharTeam parse error: {e}')
                            _append_packet_debug('char_team_parse_error', {
                                'error': str(e),
                                'raw_hex': data[pos:].hex()[:512],
                                'raw_len': len(data) - pos,
                            })
            else:
                debug_info['unknown_sub'] = True
                debug_info['raw_hex'] = data[pos:].hex()[:256]

            # Always dump remaining bytes after parsed value
            if pos < len(data):
                debug_info['after_value_hex'] = data[pos:].hex()[:256]
                debug_info['after_value_len'] = len(data) - pos

        elif field_index == 16:  # UserFightAttr
            ok, pos, sub_field = _parse_dirty_subfield_header(data, pos)
            if not ok:
                return
            debug_info['sub_field'] = sub_field
            if sub_field == 1:  # CurHp
                if pos + 4 > len(data):
                    return
                hp = _CY_PACKET.read_le_u32_at(data, pos)
                debug_info['u32'] = hp

                if hp > 0 or player.max_hp == 0:
                    player.hp = hp
                    changed = True
                else:
                    logger.debug(f'[Parser] DirtyData ignored CurHp=0 (max_hp={player.max_hp})')
            elif sub_field == 2:  # MaxHp
                if pos + 4 > len(data):
                    return
                max_hp = _CY_PACKET.read_le_u32_at(data, pos)
                debug_info['u32'] = max_hp
                if max_hp > 0:
                    player.max_hp = max_hp
                    changed = True
                else:
                    logger.debug(f'[Parser] DirtyData ignored MaxHp=0 (current={player.max_hp})')
            elif sub_field == 3:  # OriginEnergy (stamina)
                if pos + 4 > len(data):
                    return

                try:
                    energy_f = _CY_PACKET.read_le_f32_at(data, pos)
                    energy_i = _CY_PACKET.read_le_u32_at(data, pos)
                    stamina_max = max(0, player.energy_limit) + max(0, player.extra_energy_limit)
                    energy_v = _decode_dirty_energy_value(energy_i, energy_f, stamina_max=stamina_max)
                    if energy_v is not None:
                        player.energy = energy_v
                        player.energy_valid = True
                        player.energy_source_priority = 99
                        changed = True
                        debug_info.update({
                            'energy_float': energy_f,
                            'energy_int': energy_i,
                            'energy_picked': energy_v,
                            'stamina_max': stamina_max,
                        })
                        logger.debug(
                            f'[Parser] DirtyData OriginEnergy: '
                            f'f={energy_f}, i={energy_i}, picked={energy_v}'
                        )
                except Exception:
                    pass
            else:
                debug_info['raw_hex'] = data[pos:].hex()[:128]

        elif field_index == 22:  # RoleLevel
            ok, pos, sub_field = _parse_dirty_subfield_header(data, pos)
            if not ok:
                return
            debug_info['sub_field'] = sub_field
            if sub_field == 1:  # Level
                if pos + 4 > len(data):
                    return
                lv = _CY_PACKET.read_le_u32_at(data, pos)
                debug_info['u32'] = lv
                if lv > 0:
                    # 生体元等级 base is capped; a higher value is season-inclusive.
                    player.level = min(lv, BASE_LEVEL_CAP)
                    changed = True
                    debug_info['role_level'] = lv
                    logger.info(f'[Parser] DirtyData Level -> {lv} (base={player.level})')
            elif pos + 4 <= len(data):
                debug_info['u32'] = _CY_PACKET.read_le_u32_at(data, pos)
                debug_info['raw_hex'] = data[pos:].hex()[:128]

        elif field_index == 50:  # SeasonCenter
            ok, pos, sub_field = _parse_dirty_subfield_header(data, pos)
            if not ok:
                return
            debug_info['sub_field'] = sub_field
            if sub_field == 2:  # BattlePass
                ok, pos, bp_sub_field = _parse_dirty_subfield_header(data, pos)
                if not ok:
                    return
                debug_info['nested_sub_field'] = bp_sub_field
                if bp_sub_field == 2 and pos + 4 <= len(data):  # BattlePass.Level
                    battlepass_lv = _CY_PACKET.read_le_u32_at(data, pos)
                    debug_info['u32'] = battlepass_lv
                    if battlepass_lv > 0:
                        player.battlepass_level = battlepass_lv
                        debug_info['battlepass_level'] = battlepass_lv
                        logger.info(f'[Parser] DirtyData SeasonCenter.BattlePass.Level -> {battlepass_lv}')
                        if _set_level_extra_candidate(player, 'battlepass', battlepass_lv):
                            changed = True
                else:
                    debug_info['raw_hex'] = data[pos:].hex()[:128]
            else:
                debug_info['raw_hex'] = data[pos:].hex()[:128]

        elif field_index == 52:  # SeasonMedalInfo
            ok, pos, sub_field = _parse_dirty_subfield_header(data, pos)
            if not ok:
                return
            debug_info['sub_field'] = sub_field
            if sub_field == 3:  # CoreHoleInfo
                ok, pos, hole_sub_field = _parse_dirty_subfield_header(data, pos)
                if not ok:
                    return
                debug_info['nested_sub_field'] = hole_sub_field
                if hole_sub_field == 2 and pos + 4 <= len(data):  # HoleLevel
                    medal_lv_raw = _CY_PACKET.read_le_u32_at(data, pos)
                    medal_lv = _normalize_season_medal_level(medal_lv_raw)
                    debug_info['u32'] = medal_lv_raw
                    debug_info['season_medal_level_raw'] = medal_lv_raw
                    if medal_lv > 0:
                        player.season_medal_level = medal_lv
                        player.season_level = max(player.season_level, medal_lv)
                        debug_info['season_medal_level'] = medal_lv
                        logger.info(
                            f'[Parser] DirtyData SeasonMedalInfo.CoreHoleInfo.HoleLevel '
                            f'raw={medal_lv_raw} normalized={medal_lv}'
                        )
                        if _set_level_extra_candidate(player, 'season_medal', medal_lv):
                            changed = True
                else:
                    debug_info['raw_hex'] = data[pos:].hex()[:128]
            else:
                debug_info['raw_hex'] = data[pos:].hex()[:128]

        elif field_index == 56:  # MonsterHuntInfo
            ok, pos, sub_field = _parse_dirty_subfield_header(data, pos)
            if not ok:
                return
            debug_info['sub_field'] = sub_field
            if sub_field == 2 and pos + 4 <= len(data):  # CurLevel
                hunt_lv = _CY_PACKET.read_le_u32_at(data, pos)
                debug_info['u32'] = hunt_lv
                if hunt_lv > 0:
                    player.monster_hunt_level = hunt_lv
                    player.season_level = max(player.season_level, hunt_lv)
                    debug_info['monster_hunt_level'] = hunt_lv
                    logger.info(f'[Parser] DirtyData MonsterHuntInfo.CurLevel -> {hunt_lv}')
                    if _set_level_extra_candidate(player, 'monster_hunt', hunt_lv):
                        changed = True
            else:
                debug_info['raw_hex'] = data[pos:].hex()[:128]

        elif field_index == 86:  # BattlePassData
            debug_info['raw_hex'] = data[pos:].hex()[:128]

        elif field_index == 102:  # 深眠心相仪等级 (Deep Sleep Resonance Level)
            debug_info['field_name'] = 'DEEP_SLEEP_LEVEL'
            debug_info['full_raw_hex'] = data[pos:].hex()[:256]
            # Binary dirty format: 0xFFFFFFFE + validation + sub_field + nested data
            _ok_h102, _pos_h102, sub_field = _parse_dirty_subfield_header(data, pos)
            if _ok_h102:
                pos = _pos_h102
                debug_info['sub_field'] = sub_field
                # sub_field=3 is 深眠心相仪
                remaining = data[pos:]
                if len(remaining) > 0:
                    try:
                        # Use pb2 DeepSleepSeasonEntry for inner protobuf decode
                        pb = _ensure_pb()
                        if pb:
                            entry = pb.DeepSleepSeasonEntry()
                            entry.ParseFromString(remaining)
                            ds_lv = entry.Info.Level if entry.HasField('Info') else 0
                            ds_exp = entry.Info.CurExp if entry.HasField('Info') else 0
                            if ds_lv > 0:
                                debug_info['level'] = ds_lv
                                debug_info['exp'] = ds_exp
                                if sub_field == 3:
                                    if _set_level_extra_candidate(player, 'deep_sleep', ds_lv):
                                        changed = True
                                    print(f'[Parser] DirtyData 深眠心相仪等级 -> Lv.{ds_lv} 经验={ds_exp}', flush=True)
                        else:
                            # fallback to manual decode
                            nested = _decode_fields(remaining)
                            ds_data_raw = nested.get(2, [None])[0]
                            if isinstance(ds_data_raw, bytes):
                                ds_data = _decode_fields(ds_data_raw)
                                ds_lv = ds_data.get(1, [0])[0]
                                ds_exp = ds_data.get(2, [0])[0]
                                if isinstance(ds_lv, int) and ds_lv > 0:
                                    debug_info['level'] = ds_lv
                                    debug_info['exp'] = ds_exp
                                    if sub_field == 3:
                                        if _set_level_extra_candidate(player, 'deep_sleep', ds_lv):
                                            changed = True
                                        print(f'[Parser] DirtyData 深眠心相仪等级 -> Lv.{ds_lv} 经验={ds_exp}', flush=True)
                    except Exception:
                        debug_info['nested_hex'] = remaining.hex()[:128]

        elif field_index == 61:  # ProfessionList
            ok, pos, sub_field = _parse_dirty_subfield_header(data, pos)
            if not ok:
                return
            debug_info['sub_field'] = sub_field
            if sub_field == 1:  # CurProfessionId
                if pos + 4 > len(data):
                    return
                pid = _CY_PACKET.read_le_u32_at(data, pos)
                debug_info['u32'] = pid
                if pid > 0:
                    player.profession_id = pid
                    player.profession = PROFESSION_NAMES.get(pid, '')
                    changed = True
                    if self._apply_cached_profession_slots(player):
                        changed = True

        else:
            # Generic handler for ALL other CharSerialize dirty fields
            _fname = CHAR_FIELD_NAMES.get(field_index, f'FIELD_{field_index}')
            debug_info['field_name'] = _fname
            debug_info['raw_hex'] = data[pos:].hex()[:128]
            logger.debug(f'[Parser] DirtyData unhandled field {_fname} (idx={field_index})')

        # Log ALL dirty updates (not just the handled ones)
        _append_packet_debug('dirty_update', debug_info)
        if changed:
            self._notify_self()


    # SyncNearEntities (0x06)


    def _on_sync_near_entities(self, data: bytes):
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.SyncNearEntities()
            msg.ParseFromString(data)
        except Exception as e:
            logger.debug(f'[Parser] SyncNearEntities pb2 parse error: {e}')
            return

        # ── Disappear ──
        # v2.3.23: Treat all non-Dead disappear types (FAR_AWAY/REGION/TELEPORT/
        # ENTER_VEHICLE/ENTER_RIDE) as "no longer relevant in current view" and
        # evict from `_monsters`. Previously only EDisappearDead updated state,
        # so out-of-range monsters lingered in cache → boss bar would re-display
        # them via `_bb_recent_targets` lookups after map switches.
        # Save template_id → max_hp first so future Appear can rehydrate.
        for de in msg.Disappear:
            uuid = de.Uuid
            if not uuid:
                continue
            disappear_type = int(de.Type)

            if uuid in self._monsters:
                monster = self._monsters[uuid]
                if disappear_type == 1:  # EDisappearDead
                    monster.is_dead = True
                    monster.hp = 0
                    monster.last_update = time.time()
                    logger.info(f'[Parser] Monster DEAD (disappear) uuid={uuid} name={monster.name!r}')
                    self._notify_monster(monster)
                else:
                    if monster.template_id > 0 and monster.max_hp > 0:
                        self._monster_hp_cache[monster.template_id] = monster.max_hp
                    logger.info(
                        f'[Parser] Monster left view type={disappear_type} '
                        f'uuid={uuid} name={monster.name!r} — evicting from cache'
                    )
                    self._monsters.pop(uuid, None)

        # ── Appear ──
        player_uids_appeared = []
        monster_uuids_appeared = []
        for entity in msg.Appear:
            uuid = entity.Uuid
            if not uuid:
                continue

            has_attrs = entity.HasField('Attrs')
            has_temp_attrs = entity.HasField('TempAttrs')
            has_buffs = entity.HasField('BuffInfos')

            ent_type = int(getattr(entity, 'EntType', 0) or 0)
            ent_type_player = ent_type == EntityType.CHAR
            ent_type_monster = ent_type == EntityType.MONSTER

            if (
                    ent_type_monster
                    or _is_monster(uuid)
                    or (not ent_type_player and has_attrs
                        and _attrs_look_monster_like(entity.Attrs))):
                monster_uuids_appeared.append(uuid)
                if _is_player(uuid):
                    stale_uid = _uuid_to_uid(uuid)
                    if (not self._is_confirmed_self_uid(stale_uid)
                            and stale_uid not in self._team_members
                            and not self._player_entry_has_identity(stale_uid)):
                        self._players.pop(stale_uid, None)
                monster = self._get_monster(uuid)
                monster.is_dead = False
                if has_attrs:
                    self._process_monster_attr_collection(uuid, entity.Attrs)
                if monster.max_hp == 0 and monster.template_id > 0:
                    cached_max = self._monster_hp_cache.get(monster.template_id)
                    if cached_max and cached_max > 0:
                        monster.max_hp = cached_max
                        logger.info(
                            f'[Parser] Monster appear: max_hp from cache '
                            f'tid={monster.template_id} max_hp={cached_max} uuid={uuid}'
                        )
                # Decode initial buff state on monster
                if has_buffs:
                    buffs = _decode_buff_info_sync_pb(entity.BuffInfos)
                    if buffs:
                        self._check_wipe_buffs(buffs, 'entity_appear_monster', uuid)
                        monster.buff_list = buffs
                        logger.debug(f'[Parser] Entity appear: {len(buffs)} buffs on monster uuid={uuid}')
                monster.last_update = time.time()
                self._notify_monster(monster)
            elif ent_type_player or _is_player(uuid):
                uid = _uuid_to_uid(uuid)
                player_uids_appeared.append(uid)
                if has_attrs:
                    self._process_attr_collection(uid, entity.Attrs)
                    # Debug: log what attrs were present for this player
                    attr_ids = [a.Id for a in entity.Attrs.Attrs]
                    has_name_attr = 1 in attr_ids
                    p = self._get_player(uid)
                    logger.info(
                        f'[Parser] Entity appear player uid={uid} attrs={attr_ids[:15]} '
                        f'has_name={has_name_attr} resolved_name={p.name!r}'
                    )
                    if uid != self._current_uid:
                        print(
                            f'[Parser] 其他玩家出现: uid={uid} name={p.name!r} '
                            f'has_name_attr={has_name_attr} n_attrs={len(attr_ids)}',
                            flush=True,
                        )
                else:
                    logger.info(f'[Parser] Entity appear player uid={uid} NO attrs')
                    if uid != self._current_uid:
                        print(f'[Parser] 其他玩家出现: uid={uid} NO attrs', flush=True)
                if has_temp_attrs:
                    if self._is_confirmed_self_uid(uid):
                        self._process_temp_attr_collection(uid, entity.TempAttrs)
                # Decode initial buff state from entity appear
                if has_buffs:
                    if self._is_confirmed_self_uid(uid):
                        player = self._get_player(uid)
                        buffs = _decode_buff_info_sync_pb(entity.BuffInfos)
                        if buffs:
                            self._check_wipe_buffs(buffs, 'entity_appear_self', uuid)
                            player.buff_list = buffs
                            logger.info(f'[Parser] Entity appear: {len(buffs)} buffs on player uid={uid}')
                # Log player state after entity appear for debugging
                if self._is_confirmed_self_uid(uid):
                    player = self._get_player(uid)
                    logger.info(
                        f'[Parser] Entity appear SELF: uid={uid} lv={player.level} '
                        f'hp={player.hp}/{player.max_hp} prof={player.profession!r} '
                        f'slots={len(player.skill_slot_map)}'
                    )
                    print(
                        f'[Parser] 玩家实体出现: uid={uid} Lv.{player.level} '
                        f'HP={player.hp}/{player.max_hp} 职业={player.profession!r}',
                        flush=True
                    )
                    self._notify_self()

        if player_uids_appeared:
            is_self = self._current_uid in player_uids_appeared
            logger.info(
                f'[Parser] SyncNearEntities: {len(player_uids_appeared)} players appeared '
                f'(self={is_self}, current_uid={self._current_uid}, '
                f'appeared_uids={player_uids_appeared[:5]})'
            )
        if monster_uuids_appeared:
            logger.info(
                f'[Parser] SyncNearEntities: {len(monster_uuids_appeared)} monsters appeared '
                f'(uuids={monster_uuids_appeared[:5]})'
            )


    #  SyncToMeDeltaInfo (0x2E)


    def _on_sync_to_me_delta(self, data: bytes):
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.SyncToMeDeltaInfo()
            msg.ParseFromString(data)
        except Exception as e:
            logger.debug(f'[Parser] SyncToMeDeltaInfo pb2 parse error: {e}')
            return
        di = msg.DeltaInfo  # AoiSyncToMeDelta
        player = None

        # UUID from AoiSyncToMeDelta.Uuid (field 5)
        uuid = di.Uuid
        if uuid != 0:
            if _is_player(uuid):
                new_uid = _uuid_to_uid(uuid)
                player = self._get_player(new_uid)
                if self._current_uuid != uuid:
                    self._confirm_self_uid(new_uid, 'sync_to_me_uuid', uuid)
                    logger.info(f'[Parser] confirmed self UUID={uuid}, UID={new_uid}')
                    if new_uid in self._players:
                        self._notify_self()
        else:
            # Uuid field 5 is 0 — try BaseDelta.Uuid, then fall back to _current_uid
            base_uuid = di.BaseDelta.Uuid if di.HasField('BaseDelta') else 0
            if base_uuid != 0 and _is_player(base_uuid):
                fallback_uid = _uuid_to_uid(base_uuid)
                player = self._get_player(fallback_uid)
                if self._current_uuid != base_uuid:
                    self._confirm_self_uid(fallback_uid, 'sync_to_me_base_uuid', base_uuid)
                    logger.info(f'[Parser] confirmed self UUID={base_uuid} (from BaseDelta), UID={fallback_uid}')
                    if fallback_uid in self._players:
                        self._notify_self()
            elif self._current_uid:
                # SyncToMeDelta is always about self — use known UID
                player = self._get_player(self._current_uid)

        # Debug: log SyncToMeDelta summary
        n_cds = len(di.SyncSkillCDs)
        n_fres = len(di.FightResCDs)
        has_base = di.HasField('BaseDelta')
        if n_cds > 0 or n_fres > 0:
            logger.info(
                f'[Parser] SyncToMeDelta: uuid={uuid} player={"yes" if player else "NO"} '
                f'skill_cds={n_cds} fight_res_cds={n_fres} has_base={has_base} '
                f'current_uid={self._current_uid}'
            )

        skill_cd_changed = False
        sync_skill_cds = []
        fight_res_cds = []
        if player is not None:
            for cd in di.SyncSkillCDs:
                # Capture VCD from BOTH field 5 (SkillCD wire format used by
                # SyncToMeDelta) and field 8 (SkillCDInfo format used by
                # UserFightAttr).  The server sends SkillCD format for
                # SyncToMeDelta, where ValidCDTime lives at proto field 5.
                vcd_legacy = max(0, cd.ValidCDTimeLegacy)  # field 5
                vcd_info   = max(0, cd.ValidCDTime)        # field 8
                effective_vcd = vcd_legacy if vcd_legacy > 0 else vcd_info
                decoded_cd = {
                    'skill_level_id': cd.SkillLevelId,
                    'begin_time': cd.SkillBeginTime,
                    'duration': cd.Duration,
                    'skill_cd_type': int(cd.SkillCDType),
                    'valid_cd_time': effective_vcd,
                    'vcd_f5': vcd_legacy,
                    'vcd_f8': vcd_info,
                    'charge_count': cd.ChargeCount,
                    'sub_cd_ratio': cd.SubCDRatio,
                    'sub_cd_fixed': cd.SubCDFixed,
                    'accelerate_cd_ratio': cd.AccelerateCDRatio,
                }
                if decoded_cd['skill_level_id'] <= 0:
                    continue
                sync_skill_cds.append(decoded_cd)
                if self._update_skill_cd(player, decoded_cd):
                    skill_cd_changed = True
            if sync_skill_cds:
                # One-time diagnostic: log first VCD capture from field 5
                sample_vcd = [(c['skill_level_id'], c['vcd_f5'], c['vcd_f8'])
                              for c in sync_skill_cds[:5]]
                if any(v5 > 0 for _, v5, _ in sample_vcd):
                    logger.info(f'[Parser] VCD field5 captured: {sample_vcd}')
                if _helpers._PACKET_DEBUG_ENABLED:
                    _append_packet_debug('sync_skill_cd', {
                        'uid': player.uid, 'skill_cds': sync_skill_cds,
                    })
            for fcd in di.FightResCDs:
                decoded_fight_cd = {
                    'res_id': fcd.ResId,
                    'begin_time': fcd.BeginTime,
                    'duration': fcd.Duration,
                    'valid_cd_time': fcd.ValidCDTime,
                }
                res_id = decoded_fight_cd['res_id']
                if res_id > 0:
                    fight_res_cds.append(decoded_fight_cd)
                    player.fight_res_cd_map[res_id] = {
                        'res_id': res_id,
                        'begin_time': decoded_fight_cd['begin_time'],
                        'duration': decoded_fight_cd['duration'],
                        'valid_cd_time': decoded_fight_cd['valid_cd_time'],
                        'observed_at_ms': int(time.time() * 1000),
                    }
            if fight_res_cds and _helpers._PACKET_DEBUG_ENABLED:
                _append_packet_debug('fight_res_cd', {
                    'uid': player.uid, 'fight_res_cds': fight_res_cds,
                })

        # AoiSyncDelta handling — BaseDelta (field 1)
        if di.HasField('BaseDelta'):
            self._process_aoi_sync_delta(di.BaseDelta)
        if skill_cd_changed and player is not None and self._is_confirmed_self_uid(player.uid):
            self._notify_self()


    #  SyncNearDeltaInfo (0x2D)


    def _on_sync_near_delta(self, data: bytes):
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.SyncNearDeltaInfo()
            msg.ParseFromString(data)
        except Exception as e:
            logger.debug(f'[Parser] SyncNearDeltaInfo pb2 parse error: {e}')
            return
        for delta in msg.DeltaInfos:
            self._process_aoi_sync_delta(delta)


    # AoiSyncDelta handling


    @_probe.decorate('parser._process_aoi_sync_delta')
    def _process_aoi_sync_delta(self, delta):
        """Process an AoiSyncDelta pb2 object."""
        uuid = delta.Uuid
        if uuid == 0:
            return
        target_is_player, target_is_monster = self._classify_entity_uuid(uuid)
        uid = _uuid_to_uid(uuid)

        # Attrs (field 2) — players and monsters
        if delta.HasField('Attrs'):
            if target_is_player:
                self._process_attr_collection(uid, delta.Attrs)
            elif target_is_monster or _attrs_look_monster_like(delta.Attrs):
                target_is_monster = True
                self._process_monster_attr_collection(uuid, delta.Attrs)

        # TempAttrs (field 3) — buff-based temporary attributes (CD modifiers etc.)
        if delta.HasField('TempAttrs') and target_is_player:
            if self._is_confirmed_self_uid(uid):
                self._process_temp_attr_collection(uid, delta.TempAttrs)

        # BuffEffectSync (field 11) — boss buff events
        if delta.HasField('BuffEffect') and target_is_monster:
            self._process_buff_effect_sync(uuid, delta.BuffEffect)

        # BuffInfoSync (field 10) — buff state updates
        if delta.HasField('BuffInfos'):
            buffs = _decode_buff_info_sync_pb(delta.BuffInfos)
            if buffs:
                self._check_wipe_buffs(buffs, 'aoi_delta_buffs', uuid)
                if target_is_player:
                    if uid in self._players:
                        self._players[uid].buff_list = buffs
                elif target_is_monster and uuid in self._monsters:
                    self._monsters[uuid].buff_list = buffs

        # PassiveSkillInfos (field 8) — passive skill triggers
        if delta.HasField('PassiveSkillInfos'):
            logger.debug(f'[Parser] AoiDelta PassiveSkillInfos uuid={uuid}')

        # PassiveSkillEndInfos (field 9) — passive skill end
        if delta.HasField('PassiveSkillEndInfos'):
            logger.debug(f'[Parser] AoiDelta PassiveSkillEndInfos uuid={uuid}')

        # SkillEffect (field 7) — damage extraction. Match upstream counters:
        # player damage to any non-player target should count, because some
        # overworld / city-edge combat targets do not use the usual monster
        # UUID suffix.
        if delta.HasField('SkillEffects'):
            # v2.5.4: align with StarResonanceDps / resonance-logs-cn —
            # classify target by UUID encoding, not by the _classify_entity_uuid
            # result. After scene resets / dungeon re-entries the _monsters
            # cache is empty for one or two ticks and stale _team_members /
            # _players entries can flip a fresh combat target to "player" via
            # the _is_known_player_uuid fallback. SRDPS / SRLOGS only check
            # `(uuid & 0xFFFF) == 640` for player vs combat target; mirroring
            # that here keeps BOSS HP / DPS routing alive across map changes.
            uuid_suffix_player = (int(uuid) & 0xFFFF) == 640
            if not uuid_suffix_player:
                # Definitely a combat target by encoding — override any stale
                # cache mis-classification.
                target_is_player = False
                target_is_monster = True
                target_is_combat_target = True
            else:
                target_is_combat_target = not target_is_player
            self._process_skill_effect(
                uuid, target_is_player, target_is_monster,
                target_is_combat_target, delta.SkillEffects)

    @_probe.decorate('parser._process_skill_effect')
    def _process_skill_effect(self, target_uuid: int, target_is_player: bool,
                              target_is_monster: bool,
                              target_is_combat_target: bool, se):
        """Decode SkillEffect (AoiSyncDelta field 7) and emit damage events.
        se is a pb2 SkillEffect object.
        """
        for dmg in se.Damages:
            try:
                self._decode_sync_damage_info(target_uuid, target_is_player,
                                              target_is_monster,
                                              target_is_combat_target, dmg)
            except Exception as e:
                logger.debug(f'[Parser] damage decode error: {e}')

    def _decode_sync_damage_info(self, target_uuid: int, target_is_player: bool,
                                 target_is_monster: bool,
                                 target_is_combat_target: bool, dmg):
        """Decode a single SyncDamageInfo pb2 object and fire on_damage callback."""
        damage_type = int(dmg.Type)
        if damage_type in (DamageType.MISS, DamageType.FALL):
            return
        is_heal = damage_type == DamageType.HEAL
        is_immune = damage_type == DamageType.IMMUNE
        is_absorbed = damage_type == DamageType.ABSORBED
        type_flag = dmg.TypeFlag
        value = dmg.Value
        actual_value = dmg.ActualValue
        lucky_value = dmg.LuckyValue
        hp_lessen = dmg.HpLessenValue
        shield_lessen = dmg.ShieldLessenValue
        attacker_uuid_raw = int(dmg.AttackerUuid or 0)
        skill_id = dmg.OwnerId
        base_skill_id = _varint_to_int32(skill_id) if skill_id else 0
        damage_source = int(dmg.DamageSource)
        owner_level = int(dmg.OwnerLevel or 0)
        owner_stage = int(dmg.OwnerStage or 0)
        hit_event_id = int(dmg.HitEventId or 0)
        passive_uuid = int(dmg.PassiveUuid or 0)
        damage_mode = int(getattr(dmg, 'DamageMode', 0) or 0)
        is_dead = dmg.IsDead
        element = int(dmg.Property)
        top_summoner = int(dmg.TopSummonerId or 0)

        # Use TopSummonerId as the real owner if set. Some server damage
        # events send it as a player UUID, while summon / owner-routed hits
        # can send the plain character UID. Keep both forms so downstream
        # DPS/BossHP logic does not lose self damage on those packets.
        attacker_uuid = attacker_uuid_raw
        attacker_uid = 0
        if top_summoner:
            if _is_player(top_summoner):
                attacker_uuid = top_summoner
                attacker_uid = _uuid_to_uid(top_summoner)
            else:
                owner_uid = int(top_summoner)
                if (
                    owner_uid == self._current_uid
                    or owner_uid in self._players
                    or owner_uid in self._team_members
                ):
                    attacker_uid = owner_uid
                if attacker_uuid_raw and _is_player(attacker_uuid_raw):
                    attacker_uuid = attacker_uuid_raw
                else:
                    attacker_uuid = top_summoner
        elif attacker_uuid and _is_player(attacker_uuid):
            attacker_uid = _uuid_to_uid(attacker_uuid)
        damage_amount = _combat_damage_amount(
            value, lucky_value, actual_value, hp_lessen, shield_lessen)

        # For Immune/Absorbed events, allow zero-damage through (they signal invincibility)
        if not (is_immune or is_absorbed):
            if damage_amount <= 0:
                return

        # Determine if this is self-outgoing damage (self attacks monster)
        attacker_is_self = bool(_CY_COMBAT.attacker_is_self(
            attacker_uuid, self._current_uuid, self._current_uid))
        if (not attacker_is_self) and attacker_uid and self._current_uid:
            if int(attacker_uid) == int(self._current_uid):
                attacker_is_self = True

        # v3.x: 退出副本进入开放大地图后 reset_scene 会把 _current_uuid 清零,
        # 而该场景的 SyncToMeDeltaInfo 常常迟迟不来 → 自身伤害只能靠上面的 uid
        # 兜底命中 (attacker_is_self=True) 却拿不回 entity uuid → 之后基于 uuid 的
        # 归因 / 技能 caster 追踪失效, 大地图 DPS/ACT 长时间为 0。这里用首个已
        # 确认的自身伤害提前恢复 _current_uuid。仅在 _current_uuid==0 且 attacker
        # 是真实玩家 uuid 时触发, 副本 (uuid 已知) 路径完全不受影响。
        if (attacker_is_self and not self._current_uuid
                and attacker_uuid and _is_player(attacker_uuid)):
            self._confirm_self_uid(
                _uuid_to_uid(attacker_uuid), 'self_damage_recover', int(attacker_uuid))

        # 诊断(一次性/scene): _current_uuid 未恢复时, 首个"玩家攻击者"伤害的归因
        # 情况。大地图 DPS=0 时据此判断是 uid 兜底没命中 (is_self=False / uid 不符)
        # 还是别的原因。打印后置标志, 由 reset_scene 复位以便切图后再次采样。
        if (not self._current_uuid and attacker_uuid and _is_player(attacker_uuid)
                and not getattr(self, '_dbg_openscene_dmg_logged', False)):
            self._dbg_openscene_dmg_logged = True
            print(
                f'[Parser][MapBanner] open-scene first dmg: attacker_uuid={attacker_uuid} '
                f'attacker_uid={attacker_uid} current_uid={self._current_uid} '
                f'is_self={attacker_is_self}',
                flush=True,
            )

        entity_target_fallback = ''
        if attacker_is_self and target_uuid and target_is_player:
            target_uid = _uuid_to_uid(target_uuid) if _is_player(target_uuid) else 0
            known_player = bool(target_uid and target_uid in self._players)
            is_self_target = bool(
                target_uid and self._current_uid and int(target_uid) == int(self._current_uid))
            if (not known_player) and (not is_self_target):
                target_is_player = False
                target_is_monster = True
                target_is_combat_target = True
                entity_target_fallback = 'self_unknown_player_suffix_target'

        event = {
            'target_uuid': target_uuid,
            'target_is_player': target_is_player,
            'target_is_monster': target_is_monster,
            'target_is_combat_target': target_is_combat_target,
            'attacker_uuid': attacker_uuid,
            'attacker_uuid_raw': attacker_uuid_raw,
            'attacker_uid': int(attacker_uid or 0),
            'attacker_is_self': attacker_is_self,
            'top_summoner_id': top_summoner,
            'self_uid': int(self._current_uid or 0),
            'self_uuid': int(self._current_uuid or 0),
            'skill_id': base_skill_id,
            'skill_key': _compute_damage_key(base_skill_id, damage_source, owner_level, hit_event_id),
            'damage_source': damage_source,
            'owner_level': owner_level,
            'owner_stage': owner_stage,
            'hit_event_id': hit_event_id,
            'passive_uuid': passive_uuid,
            'is_normal': bool(getattr(dmg, 'IsNormal', False)),
            'is_rainbow': bool(getattr(dmg, 'IsRainbow', False)),
            'damage_mode': damage_mode,
            'damage': int(damage_amount),
            'hp_lessen': int(hp_lessen),
            'shield_lessen': int(shield_lessen),
            'damage_type': int(damage_type),
            'is_heal': is_heal,
            'is_immune': is_immune,
            'is_absorbed': is_absorbed,
            'is_crit': bool(type_flag & 1),
            'is_dead': is_dead,
            'element': int(element),
            'timestamp': time.time(),
        }
        if entity_target_fallback:
            event['entity_target_fallback'] = entity_target_fallback
        self.stats['damage_events'] += 1
        if target_is_combat_target:
            self.stats['combat_damage_events'] += 1
        if attacker_is_self:
            self.stats['self_damage_events'] += 1

        # Same-map / same-dungeon retries can reuse a monster UUID while our
        # cached MonsterData still carries the previous run's dead flag. Any
        # non-final damage event proves a live unit currently owns that UUID.
        if target_is_monster:
            monster = self._monsters.get(target_uuid)
            if monster and monster.is_dead and not is_dead:
                monster.is_dead = False
                if monster.hp == 0 and monster.max_hp > 0:
                    monster.hp = monster.max_hp
                monster.last_update = time.time()
                logger.info(
                    f'[Parser] Monster REVIVED by damage event '
                    f'uuid={target_uuid} name={monster.name!r}'
                )
                self._notify_monster(monster)
                self._notify_soft_scene_restart(f'damage_revived_uuid={target_uuid}')

        # ── Track shield depletion via damage events ──
        # When ShieldLessenValue > 0, the target's shield absorbed that much damage.
        # Subtract from the tracked shield_total. If it reaches 0, auto-clear the
        # shield_active flag (handles case where server doesn't send a final
        # AttrShieldList update or SHIELD_BROKEN buff event).
        if target_is_monster and shield_lessen > 0:
            monster = self._monsters.get(target_uuid)
            if monster and monster.shield_active:
                monster.shield_total = max(0, monster.shield_total - int(shield_lessen))
                if monster.shield_total <= 0:
                    monster.shield_active = False
                    monster.shield_total = 0
                    logger.info(
                        f'[Parser] Shield depleted via damage events, '
                        f'auto-clearing shield_active uuid={target_uuid}'
                    )
                    self._notify_monster(monster)

        if self._on_damage:
            try:
                self._on_damage(event)
            except Exception as e:
                logger.debug(f'[Parser] damage callback error: {e}')


    # AttrCollection parsing — Monster


    @_probe.decorate('parser._process_monster_attr_collection')
    def _process_monster_attr_collection(self, uuid: int, ac):
        """Decode AttrCollection pb2 object from a monster delta and update MonsterData."""
        if not ac.Attrs:
            return

        monster = self._get_monster(uuid)
        changed = False

        for attr in ac.Attrs:
            attr_id = attr.Id
            raw_data = attr.RawData
            if not raw_data or not attr_id:
                continue
            int_value = _decode_int32_from_raw(raw_data)

            if attr_id == AttrType.NAME:
                name = _decode_string_from_raw(raw_data)
                if name and name != monster.name:
                    monster.name = name
                    changed = True
                    logger.info(f'[Parser] Monster NAME={name!r} uuid={uuid}')
            elif attr_id == AttrType.ID:
                tid = int_value
                if tid > 0 and tid != monster.template_id:
                    monster.template_id = tid
                    changed = True
            elif attr_id == AttrType.HP:
                hp = int_value
                if hp >= 0 and hp != monster.hp:
                    monster.hp = hp
                    # ── max_hp estimation ──
                    # The game server never sends AttrMaxHp for monsters.
                    # Estimate max_hp from the first HP observation (monster
                    # appears at full health) and track the highest HP seen
                    # to handle healing / regen.
                    if monster.max_hp == 0 and hp > 0:
                        monster.max_hp = hp
                        logger.info(
                            f'[Parser] Monster max_hp estimated from first HP: '
                            f'{hp} uuid={uuid} name={monster.name!r}'
                        )
                        # Save estimated max to cache
                        if monster.template_id > 0:
                            self._monster_hp_cache[monster.template_id] = hp
                    elif hp > monster.max_hp > 0:
                        monster.max_hp = hp  # HP exceeded estimate (healing)
                        logger.info(
                            f'[Parser] Monster max_hp raised to {hp} '
                            f'(healing/regen) uuid={uuid}'
                        )
                    if hp == 0 and monster.max_hp > 0:
                        monster.is_dead = True
                    elif hp > 0 and monster.is_dead:
                        # Auto-revive: same UUID re-used in dungeon retry
                        monster.is_dead = False
                        logger.info(
                            f'[Parser] Monster REVIVED (hp>0 on dead unit) '
                            f'uuid={uuid} hp={hp} name={monster.name!r}'
                        )
                    changed = True
            elif attr_id == AttrType.MAX_HP:
                # Server rarely sends this for monsters, but handle it
                # properly when it does arrive.
                mhp = int_value
                if mhp > 0 and mhp != monster.max_hp:
                    monster.max_hp = mhp
                    changed = True
                    # Save to template cache for future scene-change recovery
                    if monster.template_id > 0:
                        self._monster_hp_cache[monster.template_id] = mhp
            elif attr_id == AttrType.MONSTER_SEASON_LEVEL:
                # AttrMonsterSeasonLevel (462) — monster's season level
                if int_value > 0 and int_value != monster.season_level:
                    monster.season_level = int_value
                    changed = True
                    logger.info(f'[Parser] Monster SEASON_LEVEL={int_value} uuid={uuid}')
            elif attr_id == AttrType.BREAKING_STAGE:
                if int_value != monster.breaking_stage:
                    monster.breaking_stage = int_value
                    changed = True
                    logger.info(f'[Parser] Monster BREAKING_STAGE={int_value} uuid={uuid}')
            elif attr_id == AttrType.EXTINCTION:
                if int_value != monster.extinction:
                    monster.extinction = int_value
                    changed = True
                    # Estimate max_extinction if server never sent it
                    # (same pattern as the max_hp estimation)
                    if monster.max_extinction == 0 and int_value > 0:
                        monster.max_extinction = int_value
                        logger.info(
                            f'[Parser] Monster max_extinction estimated from first value: '
                            f'{int_value} uuid={uuid}'
                        )
                    elif int_value > monster.max_extinction > 0:
                        monster.max_extinction = int_value  # recovery exceeded old max
                    logger.info(f'[Parser] Monster EXTINCTION={int_value} max={monster.max_extinction} uuid={uuid}')
            elif attr_id == AttrType.MAX_EXTINCTION:
                if int_value > 0 and int_value != monster.max_extinction:
                    monster.max_extinction = int_value
                    changed = True
                    logger.info(f'[Parser] Monster MAX_EXTINCTION={int_value} uuid={uuid}')
            elif attr_id == AttrType.STUNNED:
                if int_value != monster.stunned:
                    monster.stunned = int_value
                    changed = True
                    # Estimate max_stunned if server never sent it
                    if monster.max_stunned == 0 and int_value > 0:
                        monster.max_stunned = int_value
                        logger.info(
                            f'[Parser] Monster max_stunned estimated from first value: '
                            f'{int_value} uuid={uuid}'
                        )
                    elif int_value > monster.max_stunned > 0:
                        monster.max_stunned = int_value  # new phase with higher max
                    logger.info(f'[Parser] Monster STUNNED={int_value} max={monster.max_stunned} uuid={uuid}')
            elif attr_id == AttrType.MAX_STUNNED:
                if int_value > 0 and int_value != monster.max_stunned:
                    monster.max_stunned = int_value
                    changed = True
                    logger.info(f'[Parser] Monster MAX_STUNNED={int_value} uuid={uuid}')
            elif attr_id == AttrType.IN_OVERDRIVE:
                flag = bool(int_value)
                if flag != monster.in_overdrive:
                    monster.in_overdrive = flag
                    changed = True
                    logger.info(f'[Parser] Monster IN_OVERDRIVE={flag} uuid={uuid}')
            elif attr_id == AttrType.IS_LOCK_STUNNED:
                flag = bool(int_value)
                if flag != monster.is_lock_stunned:
                    monster.is_lock_stunned = flag
                    changed = True
            elif attr_id == AttrType.STOP_BREAKING_TICKING:
                flag = bool(int_value)
                if flag != monster.stop_breaking_ticking:
                    monster.stop_breaking_ticking = flag
                    changed = True
            elif attr_id == AttrType.SHIELD_LIST:
                # AttrShieldList = repeated ShieldInfo {uuid=1, shield_type=2, value=3, initial_value=4, max_value=5}
                self._decode_shield_list(monster, raw_data)
                changed = True
            # ── Extended monster attrs ──
            elif attr_id == AttrType.STATE:
                monster.state = int_value
                changed = True
            elif attr_id == AttrType.DEAD_TYPE:
                monster.dead_type = int_value
                changed = True
            elif attr_id == AttrType.DEAD_TIME:
                monster.dead_time = int_value
                changed = True
            elif attr_id == AttrType.FIRST_ATTACK:
                monster.first_attack = bool(int_value)
                changed = True
            elif attr_id == AttrType.HATED_CHAR_ID:
                monster.hated_char_id = int_value
                changed = True
                logger.debug(f'[Parser] Monster aggro target uid={int_value} uuid={uuid}')
            elif attr_id == AttrType.HATED_CHAR_NAME:
                name = _decode_string_from_raw(raw_data)
                if name:
                    monster.hated_char_name = name
                    changed = True
                    # ── Use aggro name to populate player data ──
                    if monster.hated_char_id > 0:
                        _aggro_player = self._get_player(monster.hated_char_id)
                        if not _aggro_player.name:
                            _aggro_player.name = name
                            logger.info(f'[Parser] Player name from aggro: {name!r} uid={monster.hated_char_id}')
            elif attr_id == AttrType.HATED_CHAR_JOB:
                _job_id = int_value
                # Use aggro job to populate player profession
                if _job_id > 0 and monster.hated_char_id > 0:
                    _aggro_player = self._get_player(monster.hated_char_id)
                    if not _aggro_player.profession_id or _aggro_player.profession_id != _job_id:
                        _aggro_player.profession_id = _job_id
                        _aggro_player.profession = PROFESSION_NAMES.get(_job_id, '')
                        logger.info(f'[Parser] Player profession from aggro: {_job_id} uid={monster.hated_char_id}')
                logger.debug(f'[Parser] Monster aggro job 0x{attr_id:X}={int_value} uuid={uuid}')
            elif attr_id in AttrType._MONSTER_EXTENDED_IDS:
                # Remaining extended monster attrs — logged
                logger.debug(f'[Parser] Monster extended attr 0x{attr_id:X}={int_value} uuid={uuid}')
            elif attr_id == AttrType.HATED_CHAR_LIST:
                # Aggro list — logged
                logger.debug(f'[Parser] Monster aggro attr 0x{attr_id:X}={int_value} uuid={uuid}')
            else:
                # Log unknown monster attrs at debug level for future analysis
                logger.debug(f'[Parser] Unknown monster AttrType 0x{attr_id:X} '
                             f'int={int_value} len={len(raw_data)} uuid={uuid}')

        if changed:
            # If monster has HP data but still no max_hp, try to recover from cache
            if monster.max_hp == 0 and monster.hp > 0 and monster.template_id > 0:
                cached_max = self._monster_hp_cache.get(monster.template_id)
                if cached_max and cached_max > 0:
                    monster.max_hp = cached_max
                    logger.info(
                        f'[Parser] Monster max_hp recovered from cache: '
                        f'tid={monster.template_id} max_hp={cached_max} uuid={uuid}'
                    )
            # Final fallback: if max_hp is still 0 but HP is known, use HP as max
            if monster.max_hp == 0 and monster.hp > 0:
                monster.max_hp = monster.hp
                logger.info(
                    f'[Parser] Monster max_hp fallback from HP: '
                    f'{monster.hp} uuid={uuid}'
                )
            monster.last_update = time.time()
            self._notify_monster(monster)
            # Log break-related attrs to packet_debug for diagnosis
            if _helpers._PACKET_DEBUG_ENABLED and (monster.max_extinction > 0 or monster.max_stunned > 0
                    or monster.extinction > 0 or monster.stunned > 0
                    or monster.breaking_stage >= 0 or monster.shield_active):
                _append_packet_debug('monster_break', {
                    'uuid': uuid,
                    'uid': monster.uid,
                    'name': monster.name,
                    'breaking_stage': monster.breaking_stage,
                    'extinction': monster.extinction,
                    'max_extinction': monster.max_extinction,
                    'stunned': monster.stunned,
                    'max_stunned': monster.max_stunned,
                    'in_overdrive': monster.in_overdrive,
                    'stop_ticking': monster.stop_breaking_ticking,
                    'shield_active': monster.shield_active,
                    'shield_total': monster.shield_total,
                    'shield_max': monster.shield_max_total,
                    'hp': monster.hp,
                    'max_hp': monster.max_hp,
                })

    # ── CharTeam parsing — party / team members ──

    def _process_char_team(self, self_uid: int, team_info):
        """Parse CharTeam pb2 message and populate _players + _team_members.

        CharTeam fields:
          TeamId(1), LeaderId(2), TeamTargetId(3), TeamNum(4),
          CharIds(5, repeated int64), IsMatching(6), CharTeamVersion(7),
          TeamMemberData(8, map<int64, TeamMemData>)

        TeamMemData → SocialData(9, TeamMemberSocialData) →
          BasicData(1) → Name(3), Level(6)
          ProfessionData(4) → ProfessionId(1)
          UserAttrData(8) → FightPoint(2)
        """
        team_id = team_info.TeamId
        leader_id = team_info.LeaderId
        char_ids = list(team_info.CharIds)
        member_map = team_info.TeamMemberData  # map<int64, TeamMemData>
        team_num = team_info.TeamNum
        is_matching = team_info.IsMatching
        team_version = team_info.CharTeamVersion

        # ── 详细日志: 原始 CharTeam 字段 ──
        print(
            f'[Parser] CharTeam 原始数据: team_id={team_id} leader={leader_id} '
            f'char_ids={char_ids} member_map_keys={list(member_map.keys())} '
            f'team_num={team_num} matching={is_matching} ver={team_version}',
            flush=True,
        )

        # ── 队伍解散检测 ──
        if not char_ids and not member_map:
            if self._team_id != 0:
                print('[Parser] 队伍已解散 (CharTeam char_ids 为空)', flush=True)
                logger.info('[Parser] Team disbanded (empty CharIds + empty MemberData)')
            self._team_id = 0
            self._team_leader_uid = 0
            self._team_members = {}
            _append_packet_debug('char_team_disband', {
                'self_uid': self_uid,
                'old_team_id': self._team_id,
            })
            return

        # Update team-level state
        self._team_id = team_id
        self._team_leader_uid = leader_id

        # Build new team member cache — preserve existing entries for
        # members still in the team, add new ones, remove departed ones.
        new_team: Dict[int, Dict[str, Any]] = {}
        now_ts = time.time()

        # Add self to team if present in char_ids
        if self_uid > 0 and self_uid in char_ids:
            self_player = self._players.get(self_uid)
            new_team[self_uid] = {
                'uid': self_uid,
                'name': (self_player.name if self_player else '') or '',
                'profession': (self_player.profession if self_player else '') or '',
                'profession_id': (self_player.profession_id if self_player else 0) or 0,
                'fight_point': (self_player.fight_point if self_player else 0) or 0,
                'level': (self_player.level if self_player else 0) or 0,
                'is_self': True,
                'joined_at': self._team_members.get(self_uid, {}).get('joined_at', now_ts),
            }

        members_parsed = []
        _social_missing_uids = []
        for char_id, mem_data in member_map.items():
            if char_id <= 0:
                continue
            if char_id == self_uid:
                continue  # self already added above

            # ── 调试: 输出 TeamMemData 所有字段 ──
            _mem_fields = {
                'CharId': mem_data.CharId,
                'EnterTime': mem_data.EnterTime,
                'CallStatus': mem_data.CallStatus,
                'TalentId': mem_data.TalentId,
                'OnlineStatus': mem_data.OnlineStatus,
                'SceneId': mem_data.SceneId,
                'VoiceIsOpen': mem_data.VoiceIsOpen,
                'GroupId': mem_data.GroupId,
                'HasSocialData': mem_data.HasField('SocialData'),
            }
            logger.info(f'[Parser] TeamMemData uid={char_id}: {_mem_fields}')

            social = mem_data.SocialData if mem_data.HasField('SocialData') else None

            name = ''
            level = 0
            prof_id = 0
            fight_point = 0
            _data_source = 'none'

            if social:
                _has_basic = social.HasField('BasicData')
                _has_prof = social.HasField('ProfessionData')
                _has_attr = social.HasField('UserAttrData')
                logger.info(
                    f'[Parser] TeamMemberSocialData uid={char_id}: '
                    f'HasBasicData={_has_basic} HasProfessionData={_has_prof} '
                    f'HasUserAttrData={_has_attr}'
                )
                if _has_basic:
                    bd = social.BasicData
                    name = bd.Name or ''
                    level = bd.Level
                if _has_prof:
                    pd = social.ProfessionData
                    prof_id = pd.ProfessionId
                if _has_attr:
                    ua = social.UserAttrData
                    fight_point = int(ua.FightPoint)
                _data_source = 'social'
            else:
                _social_missing_uids.append(char_id)

            # ── Fallback: 从 AoI _players 缓存补充缺失字段 ──
            aoi_player = self._players.get(int(char_id))
            if aoi_player:
                if not name and aoi_player.name:
                    name = aoi_player.name
                    _data_source = 'aoi' if _data_source == 'none' else _data_source + '+aoi'
                if level <= 0 and aoi_player.level > 0:
                    level = aoi_player.level
                if prof_id <= 0 and aoi_player.profession_id > 0:
                    prof_id = aoi_player.profession_id
                if fight_point <= 0 and aoi_player.fight_point > 0:
                    fight_point = aoi_player.fight_point

            # ── Fallback: 从上一轮 _team_members 缓存补充 ──
            if not name or fight_point <= 0:
                prev = self._team_members.get(int(char_id), {})
                if not name and prev.get('name'):
                    name = prev['name']
                    _data_source = (_data_source + '+prev') if _data_source != 'none' else 'prev'
                if fight_point <= 0 and prev.get('fight_point', 0) > 0:
                    fight_point = prev['fight_point']
                if level <= 0 and prev.get('level', 0) > 0:
                    level = prev['level']
                if prof_id <= 0 and prev.get('profession_id', 0) > 0:
                    prof_id = prev['profession_id']

            profession_name = PROFESSION_NAMES.get(prof_id, '')

            # Populate PlayerData for this team member
            member_player = self._get_player(int(char_id))
            member_changed = False
            if name and name != member_player.name:
                member_player.name = name
                member_changed = True
            if level > 0 and level != member_player.level:
                member_player.level = level
                member_changed = True
            if prof_id > 0 and prof_id != member_player.profession_id:
                member_player.profession_id = prof_id
                member_player.profession = profession_name
                member_changed = True
            if fight_point > 0 and fight_point != member_player.fight_point:
                member_player.fight_point = fight_point
                member_changed = True

            member_info = {
                'uid': int(char_id),
                'name': name,
                'profession': profession_name,
                'profession_id': prof_id,
                'fight_point': fight_point,
                'level': level,
                'is_self': False,
                'joined_at': self._team_members.get(int(char_id), {}).get('joined_at', now_ts),
                '_data_source': _data_source,
            }
            new_team[int(char_id)] = member_info
            members_parsed.append(member_info)

        # ── Fallback B: char_ids 中有成员不在 member_map 中, 尝试从 AoI 补充 ──
        _missing_from_map = [cid for cid in char_ids
                             if cid != self_uid and cid > 0 and cid not in member_map]
        for cid in _missing_from_map:
            aoi_p = self._players.get(int(cid))
            prev_m = self._team_members.get(int(cid), {})
            _name = (aoi_p.name if aoi_p else '') or prev_m.get('name', '')
            _level = (aoi_p.level if aoi_p else 0) or prev_m.get('level', 0)
            _pid = (aoi_p.profession_id if aoi_p else 0) or prev_m.get('profession_id', 0)
            _fp = (aoi_p.fight_point if aoi_p else 0) or prev_m.get('fight_point', 0)
            member_info = {
                'uid': int(cid),
                'name': _name,
                'profession': PROFESSION_NAMES.get(_pid, ''),
                'profession_id': _pid,
                'fight_point': _fp,
                'level': _level,
                'is_self': False,
                'joined_at': self._team_members.get(int(cid), {}).get('joined_at', now_ts),
                '_data_source': 'aoi_fallback' if aoi_p else ('prev_fallback' if prev_m else 'empty'),
            }
            new_team[int(cid)] = member_info
            members_parsed.append(member_info)
            print(
                f'[Parser] 队伍成员 (AoI/缓存 fallback): uid={cid} name={_name!r} '
                f'fight_point={_fp} lv={_level} prof={_pid} '
                f'aoi_exists={aoi_p is not None} prev_exists={bool(prev_m)}',
                flush=True,
            )

        # Atomically update team cache
        self._team_members = new_team

        # ── 日志: 显示完整队伍信息 ──
        if _social_missing_uids:
            print(
                f'[Parser] CharTeam: {len(_social_missing_uids)} 个成员缺少 SocialData: '
                f'{_social_missing_uids}',
                flush=True,
            )
        if _missing_from_map:
            print(
                f'[Parser] CharTeam: {len(_missing_from_map)} 个 char_ids 不在 TeamMemberData 中: '
                f'{_missing_from_map}',
                flush=True,
            )

        logger.info(
            f'[Parser] CharTeam parsed: team_id={team_id} leader={leader_id} '
            f'总成员={len(new_team)} (含自己) map_keys={len(member_map)} '
            f'char_ids={len(char_ids)} social_missing={len(_social_missing_uids)} '
            f'map_missing={len(_missing_from_map)}'
        )
        for m in members_parsed:
            print(
                f'[Parser] 队伍成员: {m["name"]} (UID={m["uid"]}) '
                f'职业={m["profession"]}({m["profession_id"]}) '
                f'战力={m["fight_point"]} Lv.{m["level"]} '
                f'[来源={m.get("_data_source", "?")}]',
                flush=True,
            )
        _append_packet_debug('char_team', {
            'self_uid': self_uid,
            'team_id': team_id,
            'leader_id': leader_id,
            'char_ids': char_ids,
            'member_map_keys': list(member_map.keys()),
            'social_missing': _social_missing_uids,
            'map_missing': _missing_from_map,
            'members': members_parsed,
        })

    def _decode_shield_list(self, monster: MonsterData, raw_data: bytes):
        """Decode AttrShieldList (60050) — repeated ShieldInfo messages."""
        # The raw_data for AttrShieldList is a protobuf with repeated ShieldInfo
        # ShieldInfo: uuid=1(int32), shield_type=2(int32), value=3(int64),
        #             initial_value=4(int64), max_value=5(int64)
        try:
            if not raw_data:
                monster.shield_total = 0
                monster.shield_max_total = 0
                monster.shield_active = False
                return
            shields = _decode_fields(raw_data)
            total_value = 0
            total_max = 0
            # ShieldList can be a single message or we treat raw_data as repeated
            # Try reading as a container of repeated shield entries
            for shield_raw in shields.get(1, []):
                if isinstance(shield_raw, bytes):
                    sf = _decode_fields(shield_raw)
                    value = sf.get(3, [0])[0]
                    max_val = sf.get(5, [0])[0]
                    total_value += max(0, int(value))
                    total_max += max(0, int(max_val))
                elif isinstance(shield_raw, int):
                    # Single shield inline — field 3 = value
                    value = shields.get(3, [0])[0]
                    max_val = shields.get(5, [0])[0]
                    total_value = max(0, int(value))
                    total_max = max(0, int(max_val))
                    break

            monster.shield_total = total_value
            monster.shield_max_total = total_max
            monster.shield_active = total_value > 0
            logger.debug(f'[Parser] Monster shield: {total_value}/{total_max} uuid={monster.uuid}')
        except Exception as e:
            # On decode failure, do NOT silently leave stale shield_active.
            # If the server sent an AttrShieldList update, treat it as
            # shield data being refreshed — force-clear if we can't parse.
            logger.debug(f'[Parser] shield list decode error: {e}')
            monster.shield_total = 0
            monster.shield_active = False


    # BuffEffectSync parsing


    def _process_buff_effect_sync(self, host_uuid: int, sync):
        """Decode BuffEffectSync pb2 object (AoiSyncDelta field 11) for boss events."""
        try:
            for be in sync.BuffEffects:
                event_type = int(be.Type)
                if event_type not in _BOSS_BUFF_EVENTS:
                    continue
                buff_uuid = be.BuffUuid
                buff_host = be.HostUuid if be.HostUuid != 0 else host_uuid

                logger.info(f'[Parser] BuffEvent type={event_type} buff={buff_uuid} '
                            f'host={buff_host} target_uuid={host_uuid}')

                # Update monster state based on event type
                monster = self._monsters.get(host_uuid)
                if monster:
                    if event_type == BuffEventType.ENTER_BREAKING:
                        logger.info(f'[Parser] Monster ENTER_BREAKING uuid={host_uuid}')
                        monster.breaking_stage = 0   # EBreakingStage.Breaking
                        monster.extinction = 0        # bar depleted — force 0%
                        monster.last_update = time.time()
                        self._notify_monster(monster)
                    elif event_type == BuffEventType.SHIELD_BROKEN:
                        monster.shield_active = False
                        monster.shield_total = 0
                        monster.last_update = time.time()
                        self._notify_monster(monster)
                    elif event_type == BuffEventType.HOST_DEATH:
                        monster.is_dead = True
                        monster.hp = 0
                        monster.last_update = time.time()
                        self._notify_monster(monster)

                # Fire boss event callback for all matching events
                self._notify_boss_event(event_type, host_uuid, buff_uuid)
        except Exception as e:
            logger.debug(f'[Parser] BuffEffectSync decode error: {e}')


    # TempAttrCollection parsing — Player CD buff modifiers


    def _process_temp_attr_collection(self, uid: int, tac):
        """Process TempAttrCollection pb2 object for CD-related buff modifiers.

        Relevant TempAttr types (from resonance-logs-cn skill_cd_monitor.rs):
          100 = percent CD reduction (万分比, /10000) — cumulative across buffs
          101 = flat CD reduction (ms) — cumulative
          103 = CD acceleration (万分比, /10000) — cumulative
        """
        try:
            if not tac.Attrs:
                return

            player = self._get_player(uid)
            cd_pct = 0
            cd_fixed = 0
            cd_accel = 0

            for attr in tac.Attrs:
                attr_id = attr.Id
                attr_val = attr.Value
                # protobuf int32 is already signed

                if attr_id == 100:
                    cd_pct += attr_val
                elif attr_id == 101:
                    cd_fixed += attr_val
                elif attr_id == 103:
                    cd_accel += attr_val

            changed = False
            if player.temp_attr_cd_pct != cd_pct:
                player.temp_attr_cd_pct = cd_pct
                changed = True
            if player.temp_attr_cd_fixed != cd_fixed:
                player.temp_attr_cd_fixed = cd_fixed
                changed = True
            if player.temp_attr_cd_accel != cd_accel:
                player.temp_attr_cd_accel = cd_accel
                changed = True

            if changed:
                logger.info(
                    f'[Parser] TempAttr CD modifiers: pct={cd_pct} fixed={cd_fixed} '
                    f'accel={cd_accel} uid={uid}'
                )
                _append_packet_debug(
                    'temp_attr_cd',
                    {
                        'uid': uid,
                        'cd_pct': cd_pct,
                        'cd_fixed': cd_fixed,
                        'cd_accel': cd_accel,
                    }
                )
                if self._is_confirmed_self_uid(uid):
                    self._notify_self()
        except Exception as e:
            logger.debug(f'[Parser] TempAttrCollection decode error: {e}')


    # AttrCollection parsing — Player


    @_probe.decorate('parser._process_attr_collection')
    def _process_attr_collection(self, uid: int, ac):
        """Process AttrCollection pb2 object for player attrs."""
        if not ac.Attrs:
            return

        player = self._get_player(uid)
        changed = False
        stamina_max_candidate = 0
        stamina_ratio_values = []

        for attr in ac.Attrs:
            attr_id = attr.Id
            raw_data = attr.RawData
            if not raw_data or not attr_id:
                continue
            int_value = _decode_int32_from_raw(raw_data)

            if _helpers._PACKET_DEBUG_ENABLED and self._is_confirmed_self_uid(uid):
                _append_packet_debug(
                    'attr_collection',
                    {
                        'uid': uid,
                        'attr_id': attr_id,
                        'raw_hex': raw_data.hex(),
                        'int32': int_value,
                        'float32': _decode_float32_from_raw(raw_data),
                    }
                )

            if attr_id == AttrType.NAME:
                name = _decode_string_from_raw(raw_data)
                if name:
                    player.name = name
                    changed = True
                    logger.info(f'[Parser] AttrCollection NAME={name!r} uid={uid}')
            elif attr_id == AttrType.LEVEL:
                # AttrLevel(10000) for self reports the season-inclusive 显示等级
                # (e.g. 92), not the 生体元等级 base (capped). Clamp so the season
                # level never overwrites the base; the (+XX) extra comes from
                # AttrSeasonLevel / season medal instead.
                lv = int_value
                logger.info(f'[Parser] AttrCollection LEVEL={lv} raw={raw_data.hex()} uid={uid}')
                if lv > 0:
                    player.level = min(lv, BASE_LEVEL_CAP)
                    changed = True
            elif attr_id == AttrType.RANK_LEVEL:
                rl = int_value
                logger.info(f'[Parser] AttrCollection RANK_LEVEL={rl} raw={raw_data.hex()} uid={uid}')
                if rl >= 0:
                    player.rank_level = rl
                    changed = True
            elif attr_id == AttrType.FIGHT_POINT:
                fp = int_value
                if fp > 0:
                    player.fight_point = fp
                    changed = True
            elif attr_id == AttrType.HP:
                hp = int_value
                # Ignore transient HP=0 attr updates to avoid false death states.
                # A real zero is accepted from the full sync path instead.
                if hp > 0 or player.max_hp == 0:
                    player.hp = hp
                    changed = True
                else:
                    logger.debug(f'[Parser] AttrCollection ignored HP=0 (max_hp={player.max_hp})')
            elif attr_id == AttrType.MAX_HP:
                mhp = int_value
                if mhp > 0:
                    player.max_hp = mhp
                    changed = True
            elif attr_id == AttrType.PROFESSION_ID:
                pid = int_value
                if pid > 0:
                    player.profession_id = pid
                    player.profession = PROFESSION_NAMES.get(pid, '')
                    changed = True
                    if self._apply_cached_profession_slots(player):
                        changed = True
            elif attr_id == AttrType.ENERGY_FLAG:
                # This is a flag field, not the actual stamina value.
                ef_i = int_value
                logger.debug(f'[Parser] AttrCollection EnergyFlag={ef_i} (flag only)')
            elif attr_id == AttrType.SEASON_LEVEL:
                # AttrSeasonLevel (10070) — server-calculated season level (supports up to +105+)
                sl = int_value
                if 0 < sl <= 110:
                    if _set_level_extra_candidate(player, 'season_attr', sl):
                        changed = True
                    logger.info(
                        f'[Parser] AttrCollection AttrSeasonLevel={sl} uid={uid}'
                    )
            elif attr_id == AttrType.SEASON_LV:
                # AttrSeasonLv (196) — per DPS project, treated same as AttrSeasonLevel (supports up to +105+)
                sl = int_value
                if 0 < sl <= 110:
                    if _set_level_extra_candidate(player, 'season_attr_lv', sl):
                        changed = True
                    logger.info(
                        f'[Parser] AttrCollection AttrSeasonLv={sl} uid={uid}'
                    )
            elif attr_id in (AttrType.CRI, AttrType.LUCKY, AttrType.ELEMENT_FLAG,
                             AttrType.REDUCTION_LEVEL, AttrType.ID):
                pass
            elif attr_id == AttrType.SKILL_ID:
                if int_value > 0:
                    player.attr_skill_id = int_value
                    changed = True
                    logger.info(f'[Parser] AttrCollection SkillId={int_value} uid={uid}')
            elif attr_id == AttrType.FIGHT_RESOURCES:
                values = _decode_packed_varints(raw_data)
                if values != player.fight_resource_values:
                    player.fight_resource_values = values
                    changed = True
                    logger.info(f'[Parser] AttrCollection FightResources={values[:8]} uid={uid}')
            elif attr_id == AttrType.COMBAT_STATE:
                # AttrCombatState (104) — 0=out of combat, 1=in combat
                flag = bool(int_value)
                if flag != player.in_combat:
                    player.in_combat = flag
                    changed = True
                    logger.info(f'[Parser] AttrCollection COMBAT_STATE={flag} uid={uid}')
            elif attr_id == AttrType.COMBAT_STATE_TIME:
                # AttrCombatStateTime (114) — transition timestamp in server ms
                if int_value > 0:
                    player.combat_state_time = int_value
                    changed = True
            elif attr_id in AttrType.SEASON_STRENGTH_VARIANTS:
                # AttrSeasonStrength (11440-11445) — 梦境强度 (any variant)
                if int_value > 0 and int_value > player.season_strength:
                    player.season_strength = int_value
                    changed = True
                    logger.info(f'[Parser] AttrCollection SeasonStrength={int_value} (0x{attr_id:X}) uid={uid}')
            elif attr_id in AttrType.MAX_HP_VARIANTS:
                # MaxHp sub-components (Base/Pct/Add/Rate/WithShield).
                # ONLY 11321 (base max HP) is close to the real max_hp.
                # 11323-11325 are HP rate/increment components with small values
                # (e.g. 345, 1800) that would corrupt player.max_hp → HP display jumps.
                # Log for debugging but do NOT set max_hp — only AttrType.MAX_HP (11320) does that.
                if int_value > 0:
                    logger.debug(
                        f'[Parser] AttrCollection MaxHP_Variant 0x{attr_id:X}={int_value} '
                        f'(current max_hp={player.max_hp}) uid={uid} — NOT applied'
                    )
            elif attr_id in (AttrType.SKILL_CD, AttrType.SKILL_CD_TOTAL):
                # Flat CD reduction in ms (from equipment/passives)
                # Prefer Total (11751) — server-computed sum of all contributions
                if int_value >= 0:
                    player.attr_skill_cd = int_value
                    changed = True
                    logger.info(f'[Parser] AttrCollection AttrSkillCD={int_value} (0x{attr_id:X}) uid={uid}')
            elif attr_id in (AttrType.SKILL_CD_PCT, AttrType.SKILL_CD_PCT_TOTAL):
                # Percent CD reduction (万分比, /10000)
                if int_value >= 0:
                    player.attr_skill_cd_pct = int_value
                    changed = True
                    logger.info(f'[Parser] AttrCollection AttrSkillCDPCT={int_value} (0x{attr_id:X}) uid={uid}')
            elif attr_id in (AttrType.CD_ACCELERATE_PCT, AttrType.CD_ACCELERATE_PCT_TOTAL):
                # CD acceleration percent (万分比, /10000) — includes passive bonuses
                if int_value >= 0:
                    player.attr_cd_accelerate_pct = int_value
                    changed = True
                    logger.info(f'[Parser] AttrCollection AttrCdAcceleratePct={int_value} (0x{attr_id:X}) uid={uid}')
            elif attr_id in (AttrType.FIGHT_RES_CD_SPEED_PCT, AttrType.FIGHT_RES_CD_SPEED_PCT_TOTAL):
                # FightResCdSpeedPct — CD speed/duration modifier (万分比, /10000)
                if int_value > 0:
                    player.attr_fight_res_cd_speed = int_value
                    changed = True
                    logger.info(f'[Parser] AttrCollection FightResCdSpeedPct={int_value} (0x{attr_id:X}) uid={uid}')
            # ── Extended combat stats (from EAttrType enum) ──
            elif attr_id in (AttrType.ATTACK, AttrType.ATTACK_TOTAL):
                if int_value > 0:
                    player.attack = int_value
                    changed = True
            elif attr_id in (AttrType.M_ATTACK, AttrType.M_ATTACK_TOTAL):
                if int_value > 0:
                    player.magic_attack = int_value
                    changed = True
            elif attr_id in (AttrType.DEFENSE, AttrType.DEFENSE_TOTAL):
                if int_value > 0:
                    player.defense = int_value
                    changed = True
            elif attr_id in (AttrType.M_DEFENSE, AttrType.M_DEFENSE_TOTAL):
                if int_value > 0:
                    player.magic_defense = int_value
                    changed = True
            elif attr_id in (AttrType.CRIT_RATE, AttrType.CRIT_RATE_TOTAL):
                if int_value >= 0:
                    player.crit_rate = int_value
                    changed = True
            elif attr_id in (AttrType.CRIT_DAMAGE, AttrType.CRIT_DAMAGE_TOTAL):
                if int_value >= 0:
                    player.crit_damage = int_value
                    changed = True
            elif attr_id in (AttrType.ATTACK_SPEED_PCT, AttrType.ATTACK_SPEED_PCT_TOTAL):
                if int_value >= 0:
                    player.attack_speed_pct = int_value
                    changed = True
            elif attr_id in (AttrType.CAST_SPEED_PCT, AttrType.CAST_SPEED_PCT_TOTAL):
                if int_value >= 0:
                    player.cast_speed_pct = int_value
                    changed = True
            elif attr_id in (AttrType.CHARGE_SPEED_PCT, AttrType.CHARGE_SPEED_PCT_TOTAL):
                if int_value >= 0:
                    player.charge_speed_pct = int_value
                    changed = True
            elif attr_id in (AttrType.HEAL_POWER, AttrType.HEAL_POWER_TOTAL):
                if int_value >= 0:
                    player.heal_power = int_value
                    changed = True
            elif attr_id in (AttrType.DAM_INC, AttrType.DAM_INC_TOTAL):
                if int_value >= 0:
                    player.dam_inc = int_value
                    changed = True
            elif attr_id in (AttrType.M_DAM_INC, AttrType.M_DAM_INC_TOTAL):
                if int_value >= 0:
                    player.mdam_inc = int_value
                    changed = True
            elif attr_id in (AttrType.BOSS_DAM_INC, AttrType.BOSS_DAM_INC_TOTAL):
                if int_value >= 0:
                    player.boss_dam_inc = int_value
                    changed = True
            elif attr_id in AttrType._BASE_STAT_IDS:
                # Base stats (STR/INT/DEX/VIT/Haste/Mastery/Versatility) — logged
                logger.debug(f'[Parser] AttrCollection BaseStat 0x{attr_id:X}={int_value} uid={uid}')
            elif attr_id in AttrType._DAMAGE_MOD_IDS:
                # Damage resistance modifiers — logged
                logger.debug(f'[Parser] AttrCollection DamageMod 0x{attr_id:X}={int_value} uid={uid}')
            elif attr_id in (AttrType.ORIGIN_ENERGY, AttrType.MAX_ORIGIN_ENERGY):
                # Origin energy as attr (separate from UserFightAttr stamina)
                logger.debug(f'[Parser] AttrCollection OriginEnergy 0x{attr_id:X}={int_value} uid={uid}')
            else:
                if attr_id == AttrType.STA_MAX_FALLBACK and _is_sane_attr_stamina_max(int_value):
                    stamina_max_candidate = max(stamina_max_candidate, int_value)
                elif attr_id in AttrType.STA_RATIO_SET and 0 <= int_value <= 1000:
                    stamina_ratio_values.append(int_value)

                logger.debug(f'[Parser] Unknown AttrType 0x{attr_id:X}, len={len(raw_data)}, uid={uid}')

        if _is_sane_attr_stamina_max(stamina_max_candidate):
            if player.energy_limit != stamina_max_candidate or player.extra_energy_limit != 0:
                player.energy_limit = stamina_max_candidate
                player.extra_energy_limit = 0
                changed = True
                logger.info(f'[Parser] AttrCollection STA max fallback={stamina_max_candidate} uid={uid}')

        if stamina_ratio_values:
            ratio_counts = {}
            for value in stamina_ratio_values:
                ratio_counts[value] = ratio_counts.get(value, 0) + 1
            picked_ratio, picked_count = max(ratio_counts.items(), key=lambda item: (item[1], item[0]))
            if picked_count >= 2:
                ratio_value = max(0.0, min(1.0, picked_ratio / 1000.0))
                now_ts = time.time()
                if (
                    abs(float(getattr(player, 'stamina_ratio', -1.0) or -1.0) - ratio_value) > 0.001 or
                    (now_ts - float(getattr(player, 'stamina_ratio_observed_at', 0.0) or 0.0)) > 0.8
                ):
                    player.stamina_ratio = ratio_value
                    player.stamina_ratio_observed_at = now_ts
                    changed = True
                logger.info(
                    f'[Parser] AttrCollection stamina_ratio={picked_ratio}/1000 '
                    f'(samples={picked_count}) uid={uid}'
                )

        if _refresh_stamina_resource(player):
            changed = True

        if changed and self._is_confirmed_self_uid(uid):
            self._notify_self()


    #  Zstd


    def _decompress(self, data: bytes) -> Optional[bytes]:
        """Zstd decompression helper matching the Node.js reference behavior."""
        try:
            if self._zstd is None:
                self._zstd = _ensure_zstd()

            with self._zstd.stream_reader(data) as reader:
                chunks = []
                while True:
                    chunk = reader.read(65536)
                    if not chunk:
                        break
                    chunks.append(chunk)
                return b''.join(chunks)
        except Exception as e:
            self.stats['zstd_failures'] += 1
            if self.stats['zstd_failures'] <= 3:
                print(f'[Parser] zstd解压失败 ({self.stats["zstd_failures"]}): {e}', flush=True)
            logger.debug(f'[Parser] zstd decompress failed: {e}')
            _append_packet_debug('zstd_failure', {
                'error': str(e),
                'input_size': len(data or b''),
            })
            return None
