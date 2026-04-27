# cython: language_level=3
# cython: boundscheck=False
# cython: wraparound=False
# cython: initializedcheck=False
# cython: nonecheck=False
"""Cython helpers for combat packet parsing and DPS target gating.

Keep this module dependency-free: packet capture and protobuf parsing stay in
Python so a missing/ABI-mismatched .pyd only disables acceleration.
"""


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
