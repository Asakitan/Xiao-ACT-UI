# cython: language_level=3
# cython: boundscheck=False
# cython: wraparound=False
# cython: initializedcheck=False
# cython: nonecheck=False
"""Cython UI helpers extracted from `sao_gui.py`.

These are pure-logic / pure-arithmetic hot paths that the recognition loop
and panel-float scheduler hit at 30-60 Hz. They have no Tk/PIL dependency,
so moving them out of Python avoids per-call attribute lookups, exception
machinery, and `int(...)` boxing.

Functions:
    pick_burst_trigger_slot — 9-slot burst-anchor state machine
    panel_float_offsets — sinusoidal dx/dy for the panel-float tick
    format_level_text — Lv display ('33' or '33(+5)')
    normalize_watched_skill_slots — dedupe/clip skill slot list
    is_dead_state — gs HP-based death predicate
    boss_monster_usable — boss-bar candidate predicate
    format_session_power — '12,345' or '--'
    session_int — defensive int parse
"""

from libc.math cimport sin


cdef inline long long _safe_i64(object value, long long default=0):
    if value is None:
        return default
    try:
        return <long long>int(value)
    except Exception:
        try:
            text = str(value).strip()
            if text.isdigit():
                return <long long>int(text)
            return default
        except Exception:
            return default


cdef inline double _safe_f64(object value, double default=0.0):
    if value is None:
        return default
    try:
        return <double>float(value)
    except Exception:
        return default


cpdef long long session_int(object value, long long default=0):
    """Defensive int parse used everywhere session player rows are touched."""
    return _safe_i64(value, default)


cpdef str format_session_power(object value):
    """Render the Power column. Empty / non-positive renders as '--'."""
    cdef long long v = _safe_i64(value, 0)
    if v <= 0:
        return '--'
    return f'{v:,}'


cpdef str format_level_text(object level_base, object level_extra,
                            object fallback_level=None):
    """Mirror `_format_level_text`: 'Lv' or 'Lv(+extra)'."""
    cdef long long lb = _safe_i64(level_base, 0)
    cdef long long le = _safe_i64(level_extra, 0)
    cdef long long fb
    if lb <= 0:
        fb = _safe_i64(fallback_level, 1)
        lb = fb if fb > 0 else 1
    if le > 0 and lb > 0:
        return f'{lb}(+{le})'
    return str(lb)


cpdef list normalize_watched_skill_slots(object slots):
    """Mirror `_normalize_watched_skill_slots`. Keep order, dedupe, clip 1-9."""
    cdef list normalized = []
    cdef set seen = set()
    cdef long long slot
    if not slots:
        return normalized
    for raw in slots:
        try:
            slot = <long long>int(raw)
        except Exception:
            continue
        if 1 <= slot <= 9 and slot not in seen:
            seen.add(slot)
            normalized.append(<int>slot)
    return normalized


cpdef bint is_dead_state(object hp_max, object hp_current, object hp_pct):
    """Death predicate used by the HP/vision lifecycle gating."""
    cdef long long mx = _safe_i64(hp_max, 0)
    cdef long long cur = _safe_i64(hp_current, 0)
    cdef double pct = _safe_f64(hp_pct, 1.0)
    return mx > 0 and cur <= 0 and pct <= 0.001


cpdef tuple boss_monster_usable(object hp, object max_hp, bint is_dead):
    """Decide whether a monster can drive the boss bar.

    Returns ``(usable, revive)``. ``revive`` is True when the caller should
    flip ``monster.is_dead`` from True to False because HP > 0 was observed
    on a "dead" entity (server reused the UUID for a respawn).
    """
    cdef long long h = _safe_i64(hp, 0)
    cdef long long mh = _safe_i64(max_hp, 0)
    cdef bint revive = False
    cdef bint dead = is_dead
    if dead and h > 0:
        revive = True
        dead = False
    if dead:
        return (False, revive)
    return ((mh > 0 or h > 0), revive)


cpdef tuple panel_float_offsets(double t, double phase, double amp):
    """Return ``(dx, dy)`` integer offsets for the panel-float idle wobble.

    Same constants as the original Python tick: phases 0.82 / 0.61 with a
    1.2 rad relative offset on the y axis.
    """
    cdef int dx = <int>(amp * sin(t * 0.82 + phase))
    cdef int dy = <int>(amp * sin(t * 0.61 + phase + 1.2))
    return (dx, dy)


cpdef long long pick_burst_trigger_slot(object slots, object watched,
                                        long long prev_slot):
    """Burst-anchor selection state machine.

    Inputs:
        slots: list of dicts (gs.skill_slots)
        watched: iterable of int slot indices (already normalized)
        prev_slot: last anchored slot for stability heuristic

    Returns the chosen slot index (long long); 0 means "no anchor".
    Selection priority matches the original Python:
        edge → prev (still ok) → first ready → first active → first low_cd
    """
    cdef set watched_set = set()
    cdef long long s
    for raw in (watched or []):
        try:
            s = <long long>int(raw)
        except Exception:
            continue
        if s > 0:
            watched_set.add(s)
    if not watched_set:
        watched_set.add(1)

    cdef long long edge_slot = 0
    cdef long long first_ready = 0
    cdef long long first_active = 0
    cdef long long first_low_cd = 0
    cdef bint prev_still_ok = False
    cdef long long idx
    cdef double cd
    cdef bint is_ready
    cdef str state
    cdef object slot
    cdef object state_obj
    cdef object cd_obj

    for slot in (slots or []):
        if not isinstance(slot, dict):
            continue
        try:
            idx = <long long>int(slot.get('index', 0) or 0)
        except Exception:
            continue
        if idx <= 0 or idx not in watched_set:
            continue

        state_obj = slot.get('state', '')
        if state_obj is None:
            state = ''
        else:
            state = str(state_obj).strip().lower()
        cd_obj = slot.get('cooldown_pct', 1.0)
        cd = _safe_f64(cd_obj, 1.0)
        is_ready = (state == 'ready') or (state == 'active') or cd <= 0.02

        if slot.get('ready_edge') and edge_slot == 0:
            edge_slot = idx
        if state == 'ready' and first_ready == 0:
            first_ready = idx
        if state == 'active' and first_active == 0:
            first_active = idx
        if cd <= 0.02 and first_low_cd == 0:
            first_low_cd = idx
        if idx == prev_slot and is_ready:
            prev_still_ok = True

    if edge_slot != 0:
        return edge_slot
    if prev_still_ok and prev_slot != 0:
        return prev_slot
    if first_ready != 0:
        return first_ready
    if first_active != 0:
        return first_active
    if first_low_cd != 0:
        return first_low_cd
    return 0
