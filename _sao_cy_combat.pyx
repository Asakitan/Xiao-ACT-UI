# cython: language_level=3
# cython: boundscheck=False
# cython: wraparound=False
# cython: initializedcheck=False
# cython: nonecheck=False
"""Mandatory Cython helpers for combat packet parsing, DPS target gating,
and the per-entity / per-skill DPS aggregator hot paths.

v2.4.30: ``CySkillStats`` and ``CyEntityStats`` replace the original Python
classes in ``dps_tracker.py``. The Python module re-exports them under the
old names so external imports stay stable. ``build_entity_snapshot`` and
``classify_big_hit_tier`` move the per-tick snapshot/sorting/percentage
work out of Python.
"""

from cpython.dict cimport PyDict_GetItem, PyDict_SetItem
import time as _time


cdef inline bint _to_u64(object value, unsigned long long *out):
    cdef object py_value
    try:
        py_value = int(value or 0)
        if py_value <= 0:
            out[0] = 0
            return False
        out[0] = <unsigned long long>py_value
        return True
    except Exception:
        out[0] = 0
        return False


cdef inline long long _safe_i64(object value):
    try:
        return <long long>int(value or 0)
    except Exception:
        return 0


cdef inline double _safe_f64(object value):
    try:
        return <double>float(value or 0.0)
    except Exception:
        return 0.0


cdef inline double _now() nogil:
    # `time.time()` from Python; cannot run nogil but returning here lets
    # callers stay typed even when GIL is held.
    pass


cpdef bint is_player_uuid(object uuid):
    cdef unsigned long long u
    if not _to_u64(uuid, &u):
        return False
    return (u & 0xFFFF) == 640


cpdef bint is_monster_uuid(object uuid):
    cdef unsigned long long u
    cdef unsigned long long low
    if not _to_u64(uuid, &u):
        return False
    low = u & 0xFFFF
    return low == 64 or low == 32832


cpdef unsigned long long uuid_to_uid(object uuid):
    cdef unsigned long long u
    if not _to_u64(uuid, &u):
        return 0
    return u >> 16


cpdef long long combat_damage_amount(object value, object lucky_value,
                                     object actual_value, object hp_lessen,
                                     object shield_lessen):
    cdef long long amount
    cdef long long hp
    cdef long long shield

    amount = _safe_i64(value)
    if amount > 0:
        return amount
    amount = _safe_i64(lucky_value)
    if amount > 0:
        return amount
    amount = _safe_i64(actual_value)
    if amount > 0:
        return amount

    hp = _safe_i64(hp_lessen)
    if hp < 0:
        hp = 0
    shield = _safe_i64(shield_lessen)
    if shield < 0:
        shield = 0
    return hp + shield


cpdef bint target_is_combat_target(object target_uuid, bint target_is_player):
    cdef unsigned long long u
    return (not target_is_player) and _to_u64(target_uuid, &u)


cpdef bint attacker_is_self(object attacker_uuid, object current_uuid,
                            object current_uid):
    cdef unsigned long long attacker
    cdef unsigned long long current
    cdef unsigned long long uid
    if not _to_u64(attacker_uuid, &attacker):
        return False

    if _to_u64(current_uuid, &current) and attacker == current:
        return True

    if (attacker & 0xFFFF) != 640:
        return False

    if _to_u64(current_uid, &uid) and (attacker >> 16) == uid:
        return True
    return False


cpdef bint dps_target_is_combat(object target_uuid, bint has_target_is_player,
                                bint target_is_player, bint target_is_monster,
                                bint target_is_combat_target):
    cdef unsigned long long u
    if target_is_combat_target or target_is_monster:
        return True
    if has_target_is_player and (not target_is_player) and _to_u64(target_uuid, &u):
        return True
    return False


cpdef unsigned long long dps_attacker_uid(object attacker_uuid,
                                          bint attacker_is_self,
                                          object self_uid):
    cdef unsigned long long attacker
    cdef unsigned long long uid
    if _to_u64(attacker_uuid, &attacker):
        if (attacker & 0xFFFF) == 640:
            return attacker >> 16
    if attacker_is_self and _to_u64(self_uid, &uid):
        return uid
    return 0


# ───────────────────────────────────────────────
#  Per-Skill / Per-Entity stats
# ───────────────────────────────────────────────
#
# These two ``cdef class`` types replace ``SkillStats`` / ``EntityStats`` in
# ``dps_tracker.py``. The Python module re-exports them so the public class
# names stay the same.

cdef class CySkillStats:
    cdef public long long skill_id
    cdef public str skill_name
    cdef public long long total
    cdef public long long hits
    cdef public long long crit_hits
    cdef public long long max_hit
    cdef public long long heal_total
    cdef public long long heal_hits

    def __init__(self, object skill_id, str skill_name=''):
        cdef long long sid = _safe_i64(skill_id)
        self.skill_id = sid
        if skill_name:
            self.skill_name = skill_name
        else:
            self.skill_name = str(sid)
        self.total = 0
        self.hits = 0
        self.crit_hits = 0
        self.max_hit = 0
        self.heal_total = 0
        self.heal_hits = 0

    cpdef void add_damage(self, object value, bint is_crit=False):
        cdef long long v = _safe_i64(value)
        self.total += v
        self.hits += 1
        if is_crit:
            self.crit_hits += 1
        if v > self.max_hit:
            self.max_hit = v

    cpdef void add_heal(self, object value):
        self.heal_total += _safe_i64(value)
        self.heal_hits += 1

    cpdef dict to_dict(self):
        cdef double crit_rate
        if self.hits > 0:
            crit_rate = <double>self.crit_hits / <double>self.hits
        else:
            crit_rate = 0.0
        return {
            'skill_id': self.skill_id,
            'skill_name': self.skill_name,
            'total': self.total,
            'hits': self.hits,
            'crit_hits': self.crit_hits,
            'crit_rate': round(crit_rate, 3),
            'max_hit': self.max_hit,
            'heal_total': self.heal_total,
            'heal_hits': self.heal_hits,
        }


cdef class CyEntityStats:
    cdef public unsigned long long uid
    cdef public str name
    cdef public str profession
    cdef public long long fight_point
    cdef public bint is_self
    cdef public long long damage_total
    cdef public long long damage_hits
    cdef public long long damage_crit_hits
    cdef public long long heal_total
    cdef public long long heal_hits
    cdef public long long taken_total
    cdef public long long taken_hits
    cdef public double first_damage_time
    cdef public double last_damage_time
    cdef public dict skills
    cdef public long long max_hit
    cdef public double created_at

    def __init__(self, object uid, str name='', str profession='',
                 bint is_self=False, object fight_point=0):
        cdef unsigned long long u
        if _to_u64(uid, &u):
            self.uid = u
        else:
            self.uid = 0
        if name:
            self.name = name
        else:
            self.name = 'Player_' + str(self.uid)
        self.profession = profession or ''
        self.fight_point = _safe_i64(fight_point)
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
        self.skills = {}
        self.max_hit = 0
        self.created_at = _time.time()

    cdef inline CySkillStats _get_or_create_skill(self, long long skill_id,
                                                   str skill_name):
        cdef CySkillStats sk
        cdef object existing = self.skills.get(skill_id)
        if existing is None:
            sk = CySkillStats(skill_id, skill_name)
            self.skills[skill_id] = sk
            return sk
        return <CySkillStats>existing

    cpdef void add_damage(self, object skill_id, object value,
                          bint is_crit=False, str skill_name='',
                          object timestamp=None):
        cdef long long v = _safe_i64(value)
        cdef long long sid = _safe_i64(skill_id)
        cdef double ts
        if timestamp is None:
            ts = _time.time()
        else:
            ts = _safe_f64(timestamp)
            if ts <= 0.0:
                ts = _time.time()
        self.damage_total += v
        self.damage_hits += 1
        if is_crit:
            self.damage_crit_hits += 1
        if v > self.max_hit:
            self.max_hit = v
        if self.first_damage_time == 0.0:
            self.first_damage_time = ts
        self.last_damage_time = ts
        cdef CySkillStats sk = self._get_or_create_skill(sid, skill_name)
        sk.add_damage(v, is_crit)

    cpdef void add_heal(self, object skill_id, object value,
                        str skill_name='', object timestamp=None):
        cdef long long v = _safe_i64(value)
        cdef long long sid = _safe_i64(skill_id)
        cdef double ts
        if timestamp is None:
            ts = _time.time()
        else:
            ts = _safe_f64(timestamp)
            if ts <= 0.0:
                ts = _time.time()
        self.heal_total += v
        self.heal_hits += 1
        if self.first_damage_time == 0.0:
            self.first_damage_time = ts
        self.last_damage_time = ts
        cdef CySkillStats sk = self._get_or_create_skill(sid, skill_name)
        sk.add_heal(v)

    cpdef void add_taken(self, object value):
        self.taken_total += _safe_i64(value)
        self.taken_hits += 1

    cpdef double get_elapsed_s(self):
        cdef double span
        if self.first_damage_time > 0.0 and self.last_damage_time > 0.0:
            span = self.last_damage_time - self.first_damage_time
            if span < 0.001:
                return 0.001
            return span
        return 0.001

    cpdef long long get_dps(self):
        if self.damage_total <= 0:
            return 0
        return <long long>(<double>self.damage_total / self.get_elapsed_s())

    cpdef long long get_hps(self):
        if self.heal_total <= 0:
            return 0
        return <long long>(<double>self.heal_total / self.get_elapsed_s())

    cpdef dict to_dict(self, bint include_skills=False):
        cdef double crit_rate
        cdef double elapsed = self.get_elapsed_s()
        cdef CySkillStats sk
        cdef list skill_list
        if self.damage_hits > 0:
            crit_rate = <double>self.damage_crit_hits / <double>self.damage_hits
        else:
            crit_rate = 0.0
        cdef dict d = {
            'uid': self.uid,
            'name': self.name,
            'profession': self.profession,
            'fight_point': self.fight_point,
            'is_self': self.is_self,
            'damage_total': self.damage_total,
            'damage_hits': self.damage_hits,
            'damage_crit_hits': self.damage_crit_hits,
            'crit_rate': round(crit_rate, 3),
            'heal_total': self.heal_total,
            'heal_hits': self.heal_hits,
            'taken_total': self.taken_total,
            'taken_hits': self.taken_hits,
            'dps': self.get_dps(),
            'hps': self.get_hps(),
            'max_hit': self.max_hit,
            'elapsed_s': round(elapsed, 1),
        }
        if include_skills:
            skill_list = []
            for sk in self.skills.values():
                skill_list.append(sk.to_dict())
            skill_list.sort(key=_skill_sort_key, reverse=True)
            d['skills'] = skill_list
        return d


cdef inline object _skill_sort_key(dict s):
    return s['total']


cdef inline object _entity_sort_key(dict e):
    return e['damage_total']


cpdef list build_entity_snapshot(dict entities, double now,
                                 double idle_remove_s,
                                 bint include_skills,
                                 dict player_cache,
                                 long long total_damage):
    """Filter, serialize, sort, and percent-fill the per-entity entries.

    Mirrors the original `_build_snapshot_locked` body: drops idle freshly-
    created entities after `idle_remove_s`, sorts by `damage_total` desc,
    fills `damage_pct` / `bar_pct`, backfills `fight_point` from the player
    cache. Returns a fresh list.
    """
    cdef list serialized = []
    cdef CyEntityStats e
    cdef dict d
    cdef object cached
    cdef long long max_damage = 0
    cdef long long et_total
    cdef long long denom_total = total_damage if total_damage > 0 else 1
    cdef long long denom_max
    cdef long long fp
    cdef str cache_key

    for obj in entities.values():
        e = <CyEntityStats>obj
        if (e.damage_total > 0 or e.heal_total > 0
                or e.is_self
                or (now - e.created_at) < idle_remove_s):
            d = e.to_dict(include_skills)
            serialized.append(d)

    serialized.sort(key=_entity_sort_key, reverse=True)

    if serialized:
        max_damage = <long long>serialized[0]['damage_total']
    denom_max = max_damage if max_damage > 0 else 1

    for d in serialized:
        et_total = <long long>d['damage_total']
        d['damage_pct'] = round(<double>et_total / <double>denom_total, 3)
        d['bar_pct'] = round(<double>et_total / <double>denom_max, 3)
        if not d.get('fight_point'):
            cache_key = str(d.get('uid', 0))
            cached = player_cache.get(cache_key) if player_cache is not None else None
            if cached is not None:
                fp = _safe_i64(cached.get('fight_point'))
                if fp > 0:
                    d['fight_point'] = fp
    return serialized


cpdef tuple classify_big_hit_tier(object damage,
                                  object big_threshold,
                                  object mega_threshold,
                                  object starburst_threshold):
    """Return ``(emit, tier)`` for a damage value vs. tier thresholds.

    `tier` is one of ``''`` / ``'impact'`` / ``'mega'`` / ``'starburst'``.
    `emit=False` means damage is below the big-hit threshold and the caller
    should skip the FX update entirely.
    """
    cdef long long d = _safe_i64(damage)
    cdef long long big = _safe_i64(big_threshold)
    cdef long long mega = _safe_i64(mega_threshold)
    cdef long long starburst = _safe_i64(starburst_threshold)
    if d < big:
        return (False, '')
    if d >= starburst:
        return (True, 'starburst')
    if d >= mega:
        return (True, 'mega')
    return (True, 'impact')
