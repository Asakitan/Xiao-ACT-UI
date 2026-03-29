# -*- coding: utf-8 -*-
"""DPS / HPS tracker — accumulates damage events, computes per-entity and per-skill stats."""

import json
import os
import copy
import threading
import time
from collections import defaultdict
from typing import Any, Callable, Dict, List, Optional, Tuple


# ═══════════════════════════════════════════════
#  Player cache path
# ═══════════════════════════════════════════════
_PLAYER_CACHE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'player_cache.json')


# ═══════════════════════════════════════════════
#  Helpers
# ═══════════════════════════════════════════════

def _format_big_number(value: int) -> str:
    """Pretty-print large numbers with M/K suffixes."""
    if value >= 1_000_000:
        return f"{value / 1_000_000:.1f}M"
    if value >= 1_000:
        return f"{value / 1_000:.1f}K"
    return str(value)


def _safe_int(v, default: int = 0) -> int:
    try:
        return int(v)
    except Exception:
        return default


# ═══════════════════════════════════════════════
#  Per-Skill Stats
# ═══════════════════════════════════════════════

class SkillStats:
    __slots__ = ('skill_id', 'skill_name', 'total', 'hits', 'crit_hits',
                 'max_hit', 'heal_total', 'heal_hits')

    def __init__(self, skill_id: int, skill_name: str = ''):
        self.skill_id = skill_id
        self.skill_name = skill_name or str(skill_id)
        self.total = 0
        self.hits = 0
        self.crit_hits = 0
        self.max_hit = 0
        self.heal_total = 0
        self.heal_hits = 0

    def add_damage(self, value: int, is_crit: bool = False):
        self.total += value
        self.hits += 1
        if is_crit:
            self.crit_hits += 1
        if value > self.max_hit:
            self.max_hit = value

    def add_heal(self, value: int):
        self.heal_total += value
        self.heal_hits += 1

    def to_dict(self) -> Dict[str, Any]:
        return {
            'skill_id': self.skill_id,
            'skill_name': self.skill_name,
            'total': self.total,
            'hits': self.hits,
            'crit_hits': self.crit_hits,
            'crit_rate': round(self.crit_hits / max(self.hits, 1), 3),
            'max_hit': self.max_hit,
            'heal_total': self.heal_total,
            'heal_hits': self.heal_hits,
        }


# ═══════════════════════════════════════════════
#  Per-Entity Stats
# ═══════════════════════════════════════════════

class EntityStats:
    __slots__ = ('uid', 'name', 'profession', 'fight_point', 'is_self',
                 'damage_total', 'damage_hits', 'damage_crit_hits',
                 'heal_total', 'heal_hits',
                 'taken_total', 'taken_hits',
                 'first_damage_time', 'last_damage_time',
                 'skills', 'max_hit')

    def __init__(self, uid: int, name: str = '', profession: str = '',
                 is_self: bool = False, fight_point: int = 0):
        self.uid = uid
        self.name = name or f'Player_{uid}'
        self.profession = profession
        self.fight_point = fight_point
        self.is_self = is_self
        self.damage_total = 0
        self.damage_hits = 0
        self.damage_crit_hits = 0
        self.heal_total = 0
        self.heal_hits = 0
        self.taken_total = 0
        self.taken_hits = 0
        self.first_damage_time = 0.0
        self.last_damage_time = 0.0
        self.skills: Dict[int, SkillStats] = {}
        self.max_hit = 0

    def add_damage(self, skill_id: int, value: int, is_crit: bool = False,
                   skill_name: str = '', timestamp: float = 0.0):
        self.damage_total += value
        self.damage_hits += 1
        if is_crit:
            self.damage_crit_hits += 1
        if value > self.max_hit:
            self.max_hit = value
        if not self.first_damage_time:
            self.first_damage_time = timestamp or time.time()
        self.last_damage_time = timestamp or time.time()
        # Per-skill
        sk = self.skills.get(skill_id)
        if not sk:
            sk = SkillStats(skill_id, skill_name)
            self.skills[skill_id] = sk
        sk.add_damage(value, is_crit)

    def add_heal(self, skill_id: int, value: int, skill_name: str = '',
                 timestamp: float = 0.0):
        self.heal_total += value
        self.heal_hits += 1
        if not self.first_damage_time:
            self.first_damage_time = timestamp or time.time()
        self.last_damage_time = timestamp or time.time()
        sk = self.skills.get(skill_id)
        if not sk:
            sk = SkillStats(skill_id, skill_name)
            self.skills[skill_id] = sk
        sk.add_heal(value)

    def add_taken(self, value: int):
        self.taken_total += value
        self.taken_hits += 1

    @property
    def elapsed_s(self) -> float:
        if self.first_damage_time and self.last_damage_time:
            return max(0.001, self.last_damage_time - self.first_damage_time)
        return 0.001

    @property
    def dps(self) -> int:
        return int(self.damage_total / self.elapsed_s) if self.damage_total else 0

    @property
    def hps(self) -> int:
        return int(self.heal_total / self.elapsed_s) if self.heal_total else 0

    def to_dict(self, include_skills: bool = False) -> Dict[str, Any]:
        d = {
            'uid': self.uid,
            'name': self.name,
            'profession': self.profession,
            'fight_point': self.fight_point,
            'is_self': self.is_self,
            'damage_total': self.damage_total,
            'damage_hits': self.damage_hits,
            'damage_crit_hits': self.damage_crit_hits,
            'crit_rate': round(self.damage_crit_hits / max(self.damage_hits, 1), 3),
            'heal_total': self.heal_total,
            'heal_hits': self.heal_hits,
            'taken_total': self.taken_total,
            'taken_hits': self.taken_hits,
            'dps': self.dps,
            'hps': self.hps,
            'max_hit': self.max_hit,
            'elapsed_s': round(self.elapsed_s, 1),
        }
        if include_skills:
            d['skills'] = sorted(
                [sk.to_dict() for sk in self.skills.values()],
                key=lambda s: s['total'],
                reverse=True,
            )
        return d


# ═══════════════════════════════════════════════
#  DPS Tracker
# ═══════════════════════════════════════════════

class DpsTracker:
    """Thread-safe DPS/HPS tracker for all combat entities."""

    # Inactivity auto-reset (seconds)
    INACTIVITY_TIMEOUT = 30.0
    HIT_FX_WINDOW_S = 1.6
    BIG_HIT_THRESHOLD = 1_000_000
    MEGA_HIT_THRESHOLD = 5_000_000

    def __init__(self, skill_names: Optional[Dict[int, str]] = None,
                 on_update: Optional[Callable[[], None]] = None):
        self._lock = threading.Lock()
        self._entities: Dict[int, EntityStats] = {}
        self._self_uid: int = 0
        self._encounter_start: float = 0.0
        self._encounter_end: float = 0.0
        self._last_event_time: float = 0.0
        self._last_damage_time: float = 0.0
        self._total_damage: int = 0
        self._total_heal: int = 0
        self._skill_names: Dict[int, str] = dict(skill_names or {})
        self._on_update = on_update
        self._dirty = False
        self._last_report: Optional[Dict[str, Any]] = None
        self._hit_fx_seq: int = 0
        self._last_hit_fx: Optional[Dict[str, Any]] = None
        # Player info cache: uid (str) → {name, profession, fight_point}
        self._player_cache: Dict[str, Dict[str, Any]] = {}
        self._player_cache_dirty: bool = False
        self._player_cache_last_save: float = 0.0
        self._load_player_cache()

    def set_self_uid(self, uid: int):
        with self._lock:
            self._self_uid = uid

    # ── Player info cache (persistence) ──

    def _load_player_cache(self):
        """Load cached player info from disk (called once at init, no lock needed)."""
        try:
            if os.path.isfile(_PLAYER_CACHE_PATH):
                with open(_PLAYER_CACHE_PATH, 'r', encoding='utf-8') as f:
                    data = json.load(f)
                if isinstance(data, dict):
                    self._player_cache = data
        except Exception:
            self._player_cache = {}

    def _save_player_cache_if_dirty(self):
        """Flush cache to disk if changed (throttled: max once per 3 s)."""
        if not self._player_cache_dirty:
            return
        now = time.time()
        if now - self._player_cache_last_save < 3.0:
            return
        self._player_cache_last_save = now
        self._player_cache_dirty = False
        try:
            tmp = _PLAYER_CACHE_PATH + '.tmp'
            with open(tmp, 'w', encoding='utf-8') as f:
                json.dump(self._player_cache, f, ensure_ascii=False, indent=1)
            os.replace(tmp, _PLAYER_CACHE_PATH)
        except Exception:
            pass

    def save_player_cache(self):
        """Public flush — call on shutdown or map change."""
        with self._lock:
            if self._player_cache_dirty:
                self._player_cache_last_save = 0
                self._save_player_cache_if_dirty()

    def _update_player_cache_locked(self, uid: int, name: str = '',
                                     profession: str = '', fight_point: int = 0):
        """Merge new info into the persistent player cache (caller holds _lock)."""
        key = str(uid)
        entry = self._player_cache.get(key, {})
        changed = False
        if name and entry.get('name') != name:
            entry['name'] = name
            changed = True
        if profession and entry.get('profession') != profession:
            entry['profession'] = profession
            changed = True
        if fight_point > 0 and entry.get('fight_point') != fight_point:
            entry['fight_point'] = fight_point
            changed = True
        if changed:
            entry['uid'] = uid
            entry['updated_at'] = time.time()
            self._player_cache[key] = entry
            self._player_cache_dirty = True
            self._save_player_cache_if_dirty()

    def _apply_cache_to_entity(self, entity: 'EntityStats'):
        """Fill entity fields from cache if missing (caller holds _lock)."""
        cached = self._player_cache.get(str(entity.uid))
        if not cached:
            return
        if not entity.name or entity.name.startswith('Player_'):
            entity.name = cached.get('name') or entity.name
        if not entity.profession:
            entity.profession = cached.get('profession') or ''
        if not entity.fight_point:
            entity.fight_point = int(cached.get('fight_point') or 0)

    @property
    def idle_seconds(self) -> float:
        """Seconds since last damage/heal event (0 if no events yet)."""
        with self._lock:
            if not self._last_event_time:
                return 0.0
            return time.time() - self._last_event_time

    def set_skill_names(self, names: Dict[int, str]):
        with self._lock:
            self._skill_names.update(names)

    def on_damage_event(self, event: Dict[str, Any]):
        """Called for each damage/heal event from packet_parser."""
        if not event:
            return
        with self._lock:
            self._process_event(event)

    def _process_event(self, event: Dict[str, Any]):
        now = event.get('timestamp') or time.time()

        # Auto-reset on long inactivity
        if self._last_event_time and (now - self._last_event_time) > self.INACTIVITY_TIMEOUT:
            self._finalize_current_locked('idle_timeout')
            self._reset_locked()

        target_uuid = _safe_int(event.get('target_uuid'))
        attacker_uuid = _safe_int(event.get('attacker_uuid'))
        target_is_monster = event.get('target_is_monster', False)
        attacker_is_self = event.get('attacker_is_self', False)
        is_heal = event.get('is_heal', False)
        is_immune = event.get('is_immune', False)
        is_absorbed = event.get('is_absorbed', False)
        damage = max(0, _safe_int(event.get('damage')))
        skill_id = _safe_int(event.get('skill_id'))
        is_crit = event.get('is_crit', False)

        # Derive attacker_uid from attacker_uuid (uuid >> 16 for player entities)
        attacker_uid = 0
        if attacker_uuid:
            if (attacker_uuid & 0xFFFF) == 640:
                attacker_uid = attacker_uuid >> 16
            elif attacker_is_self and self._self_uid:
                attacker_uid = self._self_uid

        # Skip immune/absorbed for DPS tracking
        if is_immune or is_absorbed or damage <= 0:
            return

        skill_name = self._skill_names.get(skill_id, str(skill_id))
        tracked = False

        if is_heal:
            # Any heal: credit the healer
            if attacker_uid:
                if not self._encounter_start:
                    self._encounter_start = now
                entity = self._get_or_create(attacker_uid, attacker_is_self)
                entity.add_heal(skill_id, damage, skill_name, now)
                self._total_heal += damage
                tracked = True
        elif target_is_monster:
            # Player→Monster damage: credit the attacker
            if attacker_uid:
                if not self._encounter_start:
                    self._encounter_start = now
                entity = self._get_or_create(attacker_uid, attacker_is_self)
                entity.add_damage(skill_id, damage, is_crit, skill_name, now)
                self._track_big_hit_fx_locked(entity, damage, now)
                self._total_damage += damage
                self._last_damage_time = now
                tracked = True
        else:
            # Monster→Player damage taken
            if not self._encounter_start:
                return
            target_uid = target_uuid >> 16 if target_uuid else 0
            if target_uid and (target_uuid & 0xFFFF) == 640:
                entity = self._get_or_create(target_uid, target_uid == self._self_uid)
                entity.add_taken(damage)
                tracked = True

        if not tracked:
            return

        self._last_event_time = now
        self._encounter_end = now
        self._dirty = True

    def _track_big_hit_fx_locked(self, entity: EntityStats, damage: int, timestamp: float):
        if damage < self.BIG_HIT_THRESHOLD:
            return
        tier = 'mega' if damage >= self.MEGA_HIT_THRESHOLD else 'impact'
        self._hit_fx_seq += 1
        self._last_hit_fx = {
            'seq': self._hit_fx_seq,
            'uid': int(entity.uid or 0),
            'name': entity.name or f'Player_{entity.uid}',
            'amount': int(damage or 0),
            'tier': tier,
            'generated_at': float(timestamp or time.time()),
        }

    def _get_or_create(self, uid: int, is_self: bool = False) -> EntityStats:
        entity = self._entities.get(uid)
        if not entity:
            entity = EntityStats(uid, is_self=is_self)
            self._apply_cache_to_entity(entity)
            self._entities[uid] = entity
        if is_self:
            entity.is_self = True
        return entity

    def update_player_info(self, uid: int, name: str = '',
                           profession: str = '', fight_point: int = 0):
        """Update display name/profession/fight_point for an entity."""
        with self._lock:
            entity = self._entities.get(uid)
            if entity:
                changed = False
                if name and entity.name != name:
                    entity.name = name
                    changed = True
                if profession and entity.profession != profession:
                    entity.profession = profession
                    changed = True
                if fight_point > 0 and entity.fight_point != fight_point:
                    entity.fight_point = fight_point
                    changed = True
                if changed:
                    self._dirty = True
            # Persist to player cache (regardless of entity existing yet)
            if uid and (name or profession or fight_point > 0):
                self._update_player_cache_locked(uid, name, profession, fight_point)

    def reset(self):
        with self._lock:
            self._finalize_current_locked('manual_reset')
            self._reset_locked()

    def _reset_locked(self):
        self._entities.clear()
        self._encounter_start = 0.0
        self._encounter_end = 0.0
        self._last_event_time = 0.0
        self._last_damage_time = 0.0
        self._total_damage = 0
        self._total_heal = 0
        self._last_hit_fx = None
        self._dirty = True

    def _has_meaningful_data_locked(self) -> bool:
        return bool(self._total_damage > 0 or self._total_heal > 0)

    def _build_snapshot_locked(self, include_skills: bool = False) -> Dict[str, Any]:
        elapsed = max(0.001, self._encounter_end - self._encounter_start) \
            if self._encounter_start else 0.001

        entities = sorted(
            [e.to_dict(include_skills=include_skills)
             for e in self._entities.values()],
            key=lambda e: e['damage_total'],
            reverse=True,
        )

        max_damage = entities[0]['damage_total'] if entities else 0
        for e in entities:
            e['damage_pct'] = round(
                e['damage_total'] / max(self._total_damage, 1), 3
            )
            e['bar_pct'] = round(
                e['damage_total'] / max(max_damage, 1), 3
            )
            # Fill missing fight_point from cache
            if not e.get('fight_point'):
                cached = self._player_cache.get(str(e.get('uid', 0)))
                if cached and cached.get('fight_point'):
                    e['fight_point'] = int(cached['fight_point'])

        snapshot = {
            'encounter_active': bool(self._encounter_start and self._has_meaningful_data_locked()),
            'encounter_started_at': self._encounter_start,
            'encounter_ended_at': self._encounter_end,
            'elapsed_s': round(elapsed, 1),
            'total_damage': self._total_damage,
            'total_heal': self._total_heal,
            'total_dps': int(self._total_damage / elapsed),
            'total_hps': int(self._total_heal / elapsed),
            'entities': entities,
        }
        if self._last_hit_fx:
            try:
                _fx_age = time.time() - float(self._last_hit_fx.get('generated_at') or 0.0)
            except Exception:
                _fx_age = self.HIT_FX_WINDOW_S + 1.0
            if _fx_age <= self.HIT_FX_WINDOW_S:
                snapshot['hit_fx'] = copy.deepcopy(self._last_hit_fx)
        return snapshot

    def _finalize_current_locked(self, reason: str = 'completed') -> Optional[Dict[str, Any]]:
        if not self._encounter_start or not self._has_meaningful_data_locked():
            return None
        completed_at = time.time()
        report = self._build_snapshot_locked(include_skills=True)
        report.update({
            'encounter_active': False,
            'completed_at': completed_at,
            'completed_local_time': time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(completed_at)),
            'report_reason': str(reason or 'completed'),
        })
        self._last_report = copy.deepcopy(report)
        return report

    def has_active_encounter(self) -> bool:
        with self._lock:
            return bool(self._encounter_start and self._has_meaningful_data_locked())

    def has_recent_damage(self, timeout_s: float) -> bool:
        with self._lock:
            if timeout_s <= 0:
                return bool(self._total_damage > 0)
            if not self._last_damage_time or self._total_damage <= 0:
                return False
            return (time.time() - self._last_damage_time) < float(timeout_s)

    def has_last_report(self) -> bool:
        with self._lock:
            return self._last_report is not None

    def finalize_if_idle(self, timeout_s: float, reason: str = 'idle_timeout') -> bool:
        with self._lock:
            if timeout_s <= 0:
                return False
            if not self._encounter_start or not self._last_event_time:
                return False
            if (time.time() - self._last_event_time) < float(timeout_s):
                return False
            finalized = self._finalize_current_locked(reason) is not None
            self._reset_locked()
            return finalized

    def get_snapshot(self, include_skills: bool = False) -> Dict[str, Any]:
        """Return a snapshot of the current encounter for UI rendering."""
        with self._lock:
            return self._build_snapshot_locked(include_skills=include_skills)

    def get_entity_detail(self, uid: int) -> Optional[Dict[str, Any]]:
        """Return detailed stats for a single entity (with skill breakdown)."""
        with self._lock:
            entity = self._entities.get(uid)
            if not entity:
                return None
            d = entity.to_dict(include_skills=True)
            d['damage_pct'] = round(
                entity.damage_total / max(self._total_damage, 1), 3
            )
            return d

    def get_last_report(self) -> Optional[Dict[str, Any]]:
        with self._lock:
            if not self._last_report:
                return None
            return copy.deepcopy(self._last_report)

    def is_dirty(self) -> bool:
        with self._lock:
            if self._dirty:
                self._dirty = False
                return True
            return False
