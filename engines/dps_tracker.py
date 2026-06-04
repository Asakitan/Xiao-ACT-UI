# -*- coding: utf-8 -*-
"""DPS / HPS tracker — accumulates damage events, computes per-entity and per-skill stats."""

import json
import os
import sys
import copy
import threading
import time
from collections import defaultdict
from functools import lru_cache
from typing import Any, Callable, Dict, List, Optional, Tuple

from utils.perf_probe import probe as _probe

import _sao_cy_combat as _CY_COMBAT  # type: ignore[import-not-found]


# ═══════════════════════════════════════════════
#  Player cache path
# ═══════════════════════════════════════════════
# 打包环境下 __file__ 位于只读的 _MEIPASS 临时目录，
# 需要将可写文件放在 exe 同级目录 (sys.executable)
_BASE_DIR = (
    os.path.dirname(sys.executable)
    if getattr(sys, 'frozen', False)
    else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
)
_PLAYER_CACHE_PATH = os.path.join(_BASE_DIR, 'player_cache.json')
_SKILL_FIGHT_LEVEL_TABLE_PATH = os.path.join(_BASE_DIR, 'assets', 'SkillFightLevelTable.json')
_SKILL_LEVEL_TO_EFFECT: Optional[Dict[int, int]] = None


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


def _load_skill_level_to_effect() -> Dict[int, int]:
    global _SKILL_LEVEL_TO_EFFECT
    if _SKILL_LEVEL_TO_EFFECT is not None:
        return _SKILL_LEVEL_TO_EFFECT
    result: Dict[int, int] = {}
    try:
        with open(_SKILL_FIGHT_LEVEL_TABLE_PATH, 'r', encoding='utf-8') as f:
            raw = json.load(f)
        if isinstance(raw, dict):
            for key, value in raw.items():
                try:
                    skill_level_id = int(key)
                    effect_id = int((value or {}).get('SkillEffectId') or 0)
                except Exception:
                    continue
                if skill_level_id > 0 and effect_id > 0:
                    result[skill_level_id] = effect_id
    except Exception:
        result = {}
    _SKILL_LEVEL_TO_EFFECT = result
    return result


def _decimal_digits(value: int) -> int:
    value = max(0, int(value or 0))
    return len(str(value)) if value >= 10 else 1


def _append_decimal(prefix: int, suffix: int, min_width: int) -> int:
    suffix = max(0, int(suffix or 0))
    width = max(_decimal_digits(suffix), int(min_width or 0))
    return int(prefix or 0) * (10 ** width) + suffix


@lru_cache(maxsize=4096)
def _skill_semantic_fact_cached(skill_id: int) -> Tuple[Tuple[str, Any], ...]:
    raw_sid = _safe_int(skill_id, 0)
    sid = _semantic_base_skill_id(raw_sid)
    if sid <= 0:
        return ()
    try:
        from tools.tablekit.combat_preparse import skill_role
        from tools.tablekit.name_table_classifier import classify_skill_id
        category = str(classify_skill_id(sid) or 'skill')
        role = str(skill_role(sid) or '')
    except Exception:
        category = 'skill'
        role = 'skill'
    if category in {
        'player_skill', 'monster_skill', 'environment_skill',
        'field_marker', 'boss_skill', 'ultimate_skill', 'roguelike_affix',
        'scripted_skill', 'virtual_skill', 'boss_mechanic_skill',
        'client_effect_skill', 'interaction_skill', 'companion_skill', 'projectile_skill',
        'passive_skill', 'test_skill', 'system_skill',
    }:
        role = category
    fact = {
        'semantic_skill_id': sid,
        'skill_category': category,
        'skill_kind': category,
        'skill_role': role,
        'is_ultimate': category == 'ultimate_skill' or role == 'ultimate',
        'is_player_skill': category in {'player_skill', 'profession_skill', 'ultimate_skill'},
        'is_monster_skill': category == 'monster_skill',
        'is_environment_skill': category == 'environment_skill',
        'is_boss_skill': category == 'boss_skill',
        'is_boss_mechanic_skill': category == 'boss_mechanic_skill',
        'is_scripted_skill': category == 'scripted_skill',
        'is_virtual_skill': category == 'virtual_skill',
        'is_roguelike_affix': category == 'roguelike_affix',
        'is_client_effect_skill': category == 'client_effect_skill',
        'is_interaction_skill': category == 'interaction_skill',
        'is_companion_skill': category == 'companion_skill',
        'is_projectile_skill': category == 'projectile_skill',
        'is_passive_skill': category == 'passive_skill',
        'is_test_skill': category == 'test_skill',
        'is_system_skill': category == 'system_skill',
    }
    return tuple(fact.items())


def _skill_semantic_fact(skill_id: int) -> Dict[str, Any]:
    return dict(_skill_semantic_fact_cached(_safe_int(skill_id, 0)))


@lru_cache(maxsize=4096)
def _semantic_base_skill_id(skill_id: int) -> int:
    sid = _safe_int(skill_id, 0)
    if sid <= 0:
        return 0
    try:
        from tools.tablekit.name_table_classifier import aoyi_skill_names, damage_attr_names, skill_fallback_names, skill_table
        known_skill_ids = set(skill_table()) | set(skill_fallback_names()) | set(aoyi_skill_names()) | set(damage_attr_names())
        if sid in known_skill_ids:
            return sid
        digits = str(sid)
        candidates = []
        for base_id in known_skill_ids:
            if base_id <= 0:
                continue
            base_text = str(base_id)
            if len(base_text) >= 4 and base_text in digits:
                candidates.append(base_id)
        if candidates:
            return max(candidates, key=lambda value: len(str(value)))
    except Exception:
        pass
    return sid


def _annotate_skill_rows(rows: Any) -> None:
    if not isinstance(rows, list):
        return
    for row in rows:
        if not isinstance(row, dict):
            continue
        sid = _safe_int(row.get('skill_id'), 0)
        if sid <= 0:
            continue
        row.update(_skill_semantic_fact(sid))


def _annotate_entity_skill_rows(entities: Any) -> None:
    if not isinstance(entities, list):
        return
    for entity in entities:
        if isinstance(entity, dict):
            _annotate_skill_rows(entity.get('skills'))


# ═══════════════════════════════════════════════
#  Per-Skill / Per-Entity Stats
# ═══════════════════════════════════════════════
# v2.4.30: SkillStats and EntityStats are now Cython cdef classes living in
# `_sao_cy_combat`. The hot paths (add_damage / add_heal / add_taken / to_dict)
# run with C-typed fields and no Python attribute lookups. Keep the original
# names exported so external code (`from dps_tracker import ...`) still works.

SkillStats = _CY_COMBAT.CySkillStats
EntityStats = _CY_COMBAT.CyEntityStats


# ═══════════════════════════════════════════════
#  DPS Tracker
# ═══════════════════════════════════════════════

class DpsTracker:
    """Thread-safe DPS/HPS tracker for all combat entities."""

    # Upstream counters do not reset encounters on short idle gaps. Keep the
    # UI fade/report behavior, but let explicit scene/deferred resets own data
    # lifetime so long boss mechanics do not split DPS.
    INACTIVITY_TIMEOUT = 0.0
    HIT_FX_WINDOW_S = 9.0
    BIG_HIT_THRESHOLD = 1_000_000
    MEGA_HIT_THRESHOLD = 5_000_000
    STARBURST_HIT_THRESHOLD = 8_000_000

    def __init__(self, skill_names: Optional[Dict[int, str]] = None,
                 on_update: Optional[Callable[[], None]] = None):
        self._lock = threading.Lock()
        self._entities: Dict[int, EntityStats] = {}
        self._self_uid: int = 0
        self._encounter_start: float = 0.0
        self._encounter_end: float = 0.0
        self._last_event_time: float = 0.0
        self._last_damage_time: float = 0.0
        self._last_finalized_event_time: float = 0.0
        self._total_damage: int = 0
        self._total_heal: int = 0
        self._boss_uuid: int = 0
        self._total_damage_boss: int = 0
        self._skill_names: Dict[int, str] = dict(skill_names or {})
        self._name_resolver = None
        self._on_update = on_update
        self._dirty = False
        self._last_report: Optional[Dict[str, Any]] = None
        self._hit_fx_seq: int = 0
        self._last_hit_fx: Optional[Dict[str, Any]] = None
        # v2.3.15: fast snapshot cache
        self._snapshot_cache: Optional[Dict[str, Any]] = None
        self._snapshot_cache_ts: float = 0.0
        self._finalized_hooks: List[Callable[[Dict[str, Any]], None]] = []
        # Player info cache: uid (str) → {name, profession, fight_point}
        self._player_cache: Dict[str, Dict[str, Any]] = {}
        self._player_cache_dirty: bool = False
        self._player_cache_last_save: float = 0.0
        self._load_player_cache()

    def set_self_uid(self, uid: int):
        with self._lock:
            self._self_uid = uid

    def set_boss_uuid(self, uuid: int):
        """Set the current boss monster UUID for boss-only damage filtering."""
        with self._lock:
            self._boss_uuid = int(uuid or 0)

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
                                     profession: str = '', fight_point: int = 0,
                                     level: int = 0):
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
        if level > 0 and entry.get('level') != level:
            entry['level'] = level
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

    def set_name_resolver(self, resolver: Any) -> None:
        with self._lock:
            self._name_resolver = resolver

    def _resolve_skill_name_locked(self, skill_key: int, skill_id: int) -> str:
        semantic_id = _semantic_base_skill_id(skill_key)
        for sid in (skill_id, skill_key, semantic_id):
            sid = _safe_int(sid, 0)
            if sid <= 0:
                continue
            resolver = self._name_resolver
            if resolver is None:
                try:
                    from tools.tablekit.name_tables import names as resolver
                except Exception:
                    resolver = None
            if resolver is not None:
                try:
                    name = resolver.skill(sid, default='')
                    if name:
                        return str(name)
                except Exception:
                    pass
            name = self._skill_names.get(sid)
            if name:
                return str(name)
        return str(skill_id or skill_key)

    def register_finalized_hook(self, callback: Callable[[Dict[str, Any]], None]) -> None:
        """Register a callback for finalized encounter reports.

        Hooks are invoked outside the DPS lock so history/export work cannot
        stall packet parsing or the overlay polling path.
        """
        if not callable(callback):
            return
        with self._lock:
            if callback not in self._finalized_hooks:
                self._finalized_hooks.append(callback)

    def unregister_finalized_hook(self, callback: Callable[[Dict[str, Any]], None]) -> None:
        with self._lock:
            try:
                self._finalized_hooks.remove(callback)
            except ValueError:
                pass

    def _dispatch_finalized_hooks(self, report: Optional[Dict[str, Any]],
                                  hooks: Optional[List[Callable[[Dict[str, Any]], None]]] = None) -> None:
        if not report:
            return
        callbacks = list(hooks) if hooks is not None else []
        if hooks is None:
            with self._lock:
                callbacks = list(self._finalized_hooks)
        if not callbacks:
            return
        for cb in callbacks:
            try:
                cb(copy.deepcopy(report))
            except Exception:
                pass

    @_probe.decorate('dps.on_damage_event')
    def on_damage_event(self, event: Dict[str, Any]):
        """Called for each damage/heal event from packet_parser."""
        if not event:
            return
        with self._lock:
            self._process_event(event)

    def _process_event(self, event: Dict[str, Any]):
        now = event.get('timestamp') or time.time()

        # Optional legacy auto-reset. Disabled by default; scene packets and
        # deferred next-damage resets are less likely to split long fights.
        if (self.INACTIVITY_TIMEOUT > 0
                and self._last_event_time
                and (now - self._last_event_time) > self.INACTIVITY_TIMEOUT):
            self._finalize_current_locked('idle_timeout')
            self._reset_locked()

        # H6: single cython call replaces ~10 event.get + 6 _safe_int + 3
        # round-trips (dps_target_is_combat + dps_attacker_uid + resolve_skill_key).
        # Lazy-loads the skill-effect table on first hit.
        _table = _SKILL_LEVEL_TO_EFFECT
        if _table is None:
            _table = _load_skill_level_to_effect()
        (target_uuid, attacker_uid, target_is_combat_target,
         attacker_is_self, skill_key, skill_id, damage,
         is_crit, is_heal, is_immune, is_absorbed) = (
            _CY_COMBAT.classify_damage_event(event, self._self_uid, _table)
        )
        attacker_uid = int(attacker_uid)
        skill_key = int(skill_key) or int(skill_id)
        target_is_player = bool(event.get('target_is_player', False))
        target_is_monster = event.get('target_is_monster', False)

        # Skip immune/absorbed for DPS tracking
        if is_immune or is_absorbed or damage <= 0:
            return

        skill_name = self._resolve_skill_name_locked(skill_key, skill_id)
        tracked = False

        if is_heal:
            # Any heal: credit the healer
            if attacker_uid:
                if not self._encounter_start:
                    self._encounter_start = now
                entity = self._get_or_create(attacker_uid, attacker_is_self)
                entity.add_heal(skill_key, damage, skill_name, now)
                self._total_heal += damage
                tracked = True
        elif target_is_combat_target:
            # Player -> non-player combat target damage: credit the attacker
            if attacker_uid:
                if not self._encounter_start:
                    self._encounter_start = now
                entity = self._get_or_create(attacker_uid, attacker_is_self)
                entity.add_damage(skill_key, damage, is_crit, skill_name, now)
                self._track_big_hit_fx_locked(entity, damage, now)
                self._total_damage += damage
                if self._boss_uuid and target_uuid == self._boss_uuid:
                    self._total_damage_boss += damage
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
        self._last_finalized_event_time = 0.0
        self._dirty = True

    def _track_big_hit_fx_locked(self, entity: EntityStats, damage: int, timestamp: float):
        # v2.4.30: tier classification is in cython.
        emit, tier = _CY_COMBAT.classify_big_hit_tier(
            damage,
            self.BIG_HIT_THRESHOLD,
            self.MEGA_HIT_THRESHOLD,
            self.STARBURST_HIT_THRESHOLD,
        )
        if not emit:
            return
        self._hit_fx_seq += 1
        # D4: cython dict builder pushes the per-field boxing
        # (int()/str()/float() + or-fallback) into typed cython.
        self._last_hit_fx = _CY_COMBAT.build_big_hit_fx_event(
            self._hit_fx_seq, entity.uid, entity.name, damage, tier, timestamp,
        )

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
                           profession: str = '', fight_point: int = 0,
                           level: int = 0):
        """Update display name/profession/fight_point/level for an entity.
        
        If no entity exists yet but we have a valid name, pre-create the entity
        so that when damage events arrive later, the name is already set.
        """
        with self._lock:
            entity = self._entities.get(uid)
            if not entity and name and uid:
                # Pre-create entity with name so it's ready when damage arrives
                entity = self._get_or_create(uid)
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
            if uid and (name or profession or fight_point > 0 or level > 0):
                self._update_player_cache_locked(uid, name, profession, fight_point, level)

    def update_player_info_batch(self, players: List[Tuple[int, str, str, int, int]]) -> None:
        """Batch update display info for multiple players with a single lock acquisition.
        
        v2.3.15: eliminates N lock acquisitions when updating the whole party.
        Each element of `players` is (uid, name, profession, fight_point, level).
        """
        if not players:
            return
        with self._lock:
            for uid, name, profession, fight_point, level in players:
                if not uid:
                    continue
                entity = self._entities.get(uid)
                if not entity and name:
                    entity = self._get_or_create(uid)
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
                if name or profession or fight_point > 0 or level > 0:
                    self._update_player_cache_locked(uid, name, profession, fight_point, level)

    def try_lock(self) -> bool:
        """Attempt to acquire the lock without blocking.
        
        v2.3.15: allows the main thread to skip DPS updates when the lock
        is held by the packet thread, avoiding blocking the UI.
        """
        return self._lock.acquire(blocking=False)

    def unlock(self) -> None:
        """Release the lock acquired via try_lock()."""
        self._lock.release()

    def reset(self):
        report = None
        hooks: List[Callable[[Dict[str, Any]], None]] = []
        with self._lock:
            report = self._finalize_current_locked('manual_reset')
            if report is not None:
                hooks = list(self._finalized_hooks)
            self._reset_locked()
        self._dispatch_finalized_hooks(report, hooks)

    def reset_idle_live_data(self, reason: str = 'idle_timeout',
                             expected_last_event_time: float = 0.0) -> bool:
        report = None
        hooks: List[Callable[[Dict[str, Any]], None]] = []
        with self._lock:
            if not self._has_meaningful_data_locked():
                return False
            if expected_last_event_time > 0:
                if abs(self._last_event_time - expected_last_event_time) > 1e-6:
                    return False
            report = self._finalize_current_locked(reason)
            if report is not None:
                self._last_finalized_event_time = self._last_event_time
                hooks = list(self._finalized_hooks)
            self._reset_locked()
        self._dispatch_finalized_hooks(report, hooks)
        return True

    def _reset_locked(self):
        self._entities.clear()
        self._encounter_start = 0.0
        self._encounter_end = 0.0
        self._last_event_time = 0.0
        self._last_damage_time = 0.0
        self._last_finalized_event_time = 0.0
        self._total_damage = 0
        self._total_heal = 0
        self._boss_uuid = 0
        self._total_damage_boss = 0
        self._last_hit_fx = None
        self._dirty = True

    def _has_meaningful_data_locked(self) -> bool:
        return bool(self._total_damage > 0 or self._total_heal > 0)

    def _build_snapshot_locked(self, include_skills: bool = False) -> Dict[str, Any]:
        elapsed = max(0.001, self._encounter_end - self._encounter_start) \
            if self._encounter_start else 0.001

        now = time.time()
        # v2.4.30: filter+serialize+sort+pct fill in cython.
        entities = _CY_COMBAT.build_entity_snapshot(
            self._entities,
            now,
            5.0,                        # idle remove (s) for never-acted entries
            bool(include_skills),
            self._player_cache,
            int(self._total_damage),
        )
        if include_skills:
            _annotate_entity_skill_rows(entities)

        display_damage = self._total_damage_boss if (
            self._boss_uuid and self._total_damage_boss > 0
        ) else self._total_damage

        snapshot = {
            'encounter_active': bool(self._encounter_start and self._has_meaningful_data_locked()),
            'encounter_started_at': self._encounter_start,
            'encounter_ended_at': self._encounter_end,
            'elapsed_s': round(elapsed, 1),
            'total_damage': display_damage,
            'total_damage_all': self._total_damage,
            'total_heal': self._total_heal,
            'total_dps': int(display_damage / elapsed),
            'total_hps': int(self._total_heal / elapsed),
            'entities': entities,
        }
        if self._last_hit_fx:
            try:
                _fx_age = time.time() - float(self._last_hit_fx.get('generated_at') or 0.0)
            except Exception:
                _fx_age = self.HIT_FX_WINDOW_S + 1.0
            if _fx_age <= self.HIT_FX_WINDOW_S:
                # v3.1.5 round 10: hit_fx is a flat dict of primitives
                # (seq/uid/name/amount/tier/generated_at); shallow copy
                # via `dict(...)` is equivalent to deepcopy and ~10x
                # faster than copy.deepcopy on every UI poll.
                snapshot['hit_fx'] = dict(self._last_hit_fx)
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
        report = None
        hooks: List[Callable[[Dict[str, Any]], None]] = []
        with self._lock:
            if timeout_s <= 0:
                return False
            if not self._encounter_start or not self._last_event_time:
                return False
            if (time.time() - self._last_event_time) < float(timeout_s):
                return False
            if self._last_finalized_event_time == self._last_event_time:
                return False
            report = self._finalize_current_locked(reason)
            finalized = report is not None
            if finalized:
                self._last_finalized_event_time = self._last_event_time
                hooks = list(self._finalized_hooks)
                self._dirty = True
        self._dispatch_finalized_hooks(report, hooks)
        return finalized

    @_probe.decorate('dps.get_snapshot')
    def get_snapshot(self, include_skills: bool = False) -> Dict[str, Any]:
        """Return a snapshot of the current encounter for UI rendering."""
        with self._lock:
            return self._build_snapshot_locked(include_skills=include_skills)

    def get_snapshot_fast(self, max_age_ms: float = 150.0) -> Dict[str, Any]:
        """Fast snapshot retrieval with short-lived cache.
        
        v2.3.15: avoids full deepcopy on every call. Returns a cached
        snapshot if less than max_age_ms has elapsed since the last
        build. The cache is invalidated on any data mutation (dirty flag).
        """
        now = time.time()
        _cached = self._snapshot_cache
        if _cached is not None and (now - self._snapshot_cache_ts) * 1000 < max_age_ms:
            return _cached
        with self._lock:
            snap = self._build_snapshot_locked(include_skills=False)
            self._snapshot_cache = snap
            self._snapshot_cache_ts = time.time()
            return snap

    def invalidate_snapshot_cache(self) -> None:
        """Force the next get_snapshot_fast to rebuild."""
        self._snapshot_cache = None

    def get_entity_detail(self, uid: int) -> Optional[Dict[str, Any]]:
        """Return detailed stats for a single entity (with skill breakdown)."""
        with self._lock:
            entity = self._entities.get(uid)
            if not entity:
                return None
            d = entity.to_dict(include_skills=True)
            _annotate_skill_rows(d.get('skills'))
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

    def poll_overlay_state(self, max_age_ms: float = 150.0,
                           idle_timeout_s: float = 15.0,
                           detail_uid: int = 0) -> Dict[str, Any]:
        """Single-lock acquisition for all data the DPS overlay needs.

        Combines: finalize_if_idle + is_dirty + get_snapshot_fast +
                  has_recent_damage + has_last_report + get_entity_detail.

        Returns dict with keys:
            snapshot, has_live, has_report, detail, dirty,
            should_fade_out, should_show
        """
        now = time.time()
        result: Dict[str, Any] = {
            'snapshot': None,
            'has_live': False,
            'has_report': False,
            'detail': None,
            'dirty': False,
            'should_fade_out': False,
        }
        finalized_report = None
        finalized_hooks: List[Callable[[Dict[str, Any]], None]] = []
        with self._lock:
            # 1) Finalize if idle
            if idle_timeout_s > 0 and self._encounter_start and self._last_event_time:
                if (now - self._last_event_time) >= idle_timeout_s:
                    if self._last_finalized_event_time != self._last_event_time:
                        finalized_report = self._finalize_current_locked('idle_timeout')
                        if finalized_report is not None:
                            self._last_finalized_event_time = self._last_event_time
                            finalized_hooks = list(self._finalized_hooks)
                            self._dirty = True

            # 2) Dirty check
            dirty = bool(self._dirty)
            if dirty:
                self._dirty = False
            result['dirty'] = dirty

            # 3) Snapshot (with cache).  A dirty poll must rebuild before
            # clearing the event path; otherwise a fresh damage tick can be
            # acknowledged while callers still receive the previous snapshot.
            _cached = self._snapshot_cache
            if (not dirty and _cached is not None
                    and (now - self._snapshot_cache_ts) * 1000 < max_age_ms):
                snap = _cached
            else:
                snap = self._build_snapshot_locked(include_skills=False)
                self._snapshot_cache = snap
                self._snapshot_cache_ts = now
            result['snapshot'] = snap

            # 4) Has live data
            total_damage = int(snap.get('total_damage') or 0)
            if total_damage > 0 and self._last_damage_time:
                result['has_live'] = (now - self._last_damage_time) < idle_timeout_s
            else:
                result['has_live'] = False

            # 5) Should fade out
            if total_damage > 0 and self._last_damage_time:
                result['should_fade_out'] = (now - self._last_damage_time) >= idle_timeout_s
            else:
                result['should_fade_out'] = False
            result['last_event_time'] = float(self._last_event_time or 0.0)

            # 6) Has report
            result['has_report'] = self._last_report is not None

            # 7) Entity detail (for detail popup)
            if detail_uid > 0:
                entity = self._entities.get(detail_uid)
                if entity:
                    d = entity.to_dict(include_skills=True)
                    _annotate_skill_rows(d.get('skills'))
                    d['damage_pct'] = round(
                        entity.damage_total / max(self._total_damage, 1), 3)
                    result['detail'] = d

        self._dispatch_finalized_hooks(finalized_report, finalized_hooks)
        return result
