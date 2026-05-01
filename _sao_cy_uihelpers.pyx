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


cpdef tuple breath_offsets(double t):
    """Return ``(dx, dy)`` integer offsets for the floating widget idle breath.

    Mirrors `_breath_step`'s ``int(round(sin(t*1.25)*3.0))`` /
    ``int(round(sin(t*2.1)*2.0))``. Runs at 60 fps so the avoided per-call
    Python overhead (math.sin → builtin int → round) adds up.
    """
    cdef double dx_f = sin(t * 1.25) * 3.0
    cdef double dy_f = sin(t * 2.1) * 2.0
    # Match Python's built-in round() (banker's rounding to nearest even is
    # close enough for whole-pixel deltas; use simple truncation toward
    # nearest with explicit add 0.5 / sub 0.5 for sign).
    cdef int dx
    cdef int dy
    if dx_f >= 0.0:
        dx = <int>(dx_f + 0.5)
    else:
        dx = <int>(dx_f - 0.5)
    if dy_f >= 0.0:
        dy = <int>(dy_f + 0.5)
    else:
        dy = <int>(dy_f - 0.5)
    return (dx, dy)


cdef inline int _max_int(int a, int b):
    return a if a > b else b


cdef inline int _round_pos(double v):
    if v >= 0.0:
        return <int>(v + 0.5)
    return <int>(v - 0.5)


cpdef object compute_skillfx_layout(object client_rect, list slot_rects,
                                    list fallback_slot_rects=None):
    """Compute the SkillFX overlay window/viewport/slot layout.

    Inputs:
        client_rect: ``(left, top, right, bottom)`` of the game client.
        slot_rects: list of dicts ``{'index': int, 'screen_rect': {...}}``
            already mapped into screen coords. Pass empty when not yet
            available; we'll fall back to the static layout below.
        fallback_slot_rects: precomputed default ``{'index', 'screen_rect'}``
            list (e.g. from ``get_skill_slot_rects``). Used only when
            ``slot_rects`` is empty.

    Returns ``None`` when no slots are available, otherwise the same dict
    shape ``_get_skillfx_layout`` produced previously (window / viewport /
    slots).
    """
    if not client_rect:
        return None
    cdef int client_left, client_top, client_right, client_bottom
    try:
        client_left = <int>client_rect[0]
        client_top = <int>client_rect[1]
        client_right = <int>client_rect[2]
        client_bottom = <int>client_rect[3]
    except Exception:
        return None
    cdef int client_w = _max_int(1, client_right - client_left)
    cdef int client_h = _max_int(1, client_bottom - client_top)

    cdef list slots = []
    cdef object item
    cdef object rect
    cdef int sx, sy, sw, sh, idx

    if slot_rects:
        for item in slot_rects:
            rect = item.get('screen_rect') if isinstance(item, dict) else None
            if rect is None:
                continue
            try:
                sx = <int>int(rect.get('x', 0))
                sy = <int>int(rect.get('y', 0))
                sw = <int>int(rect.get('w', 0))
                sh = <int>int(rect.get('h', 0))
                idx = <int>int(item.get('index', 0) or 0)
            except Exception:
                continue
            if idx <= 0 or sw <= 0 or sh <= 0:
                continue
            slots.append({
                'index': idx,
                'screen_rect': {'x': sx, 'y': sy, 'w': sw, 'h': sh},
            })

    if not slots and fallback_slot_rects:
        for item in fallback_slot_rects:
            try:
                idx = <int>int(item.get('index', 0) or 0)
                rect = item.get('screen_rect') if isinstance(item, dict) else None
                if rect is None:
                    continue
                sx = <int>int(rect.get('x', 0))
                sy = <int>int(rect.get('y', 0))
                sw = <int>int(rect.get('w', 0))
                sh = <int>int(rect.get('h', 0))
            except Exception:
                continue
            if idx <= 0 or sw <= 0 or sh <= 0:
                continue
            slots.append({
                'index': idx,
                'screen_rect': {'x': sx, 'y': sy, 'w': sw, 'h': sh},
            })

    if not slots:
        return None

    cdef int min_x = (<dict>slots[0])['screen_rect']['x']
    cdef int max_y = (<dict>slots[0])['screen_rect']['y'] + (<dict>slots[0])['screen_rect']['h']
    cdef object s
    cdef int sxx, syy, shh
    for s in slots:
        sxx = (<dict>s)['screen_rect']['x']
        syy = (<dict>s)['screen_rect']['y']
        shh = (<dict>s)['screen_rect']['h']
        if sxx < min_x:
            min_x = sxx
        if syy + shh > max_y:
            max_y = syy + shh

    cdef int pad_x = _max_int(18, _round_pos(<double>client_w * 0.012))
    cdef int pad_y = _max_int(18, _round_pos(<double>client_h * 0.016))
    cdef int pad_left = _max_int(96, _round_pos(<double>client_w * 0.055))
    cdef int pad_right = _max_int(84, _round_pos(<double>client_w * 0.044))
    cdef int win_x = min_x - pad_left
    if win_x < 0:
        win_x = 0
    cdef int win_y = client_top
    if win_y < 0:
        win_y = 0
    cdef int width = (client_right - win_x) + pad_right
    if width < 420:
        width = 420
    cdef int height = (max_y - win_y) + pad_y
    if height < 220:
        height = 220
    cdef int callout_w = _max_int(440, _round_pos(<double>client_w * 0.29))
    cdef int callout_h = _max_int(128, _round_pos(<double>client_h * 0.115))
    cdef int callout_margin_x = _max_int(28, _round_pos(<double>client_w * 0.022))
    cdef int callout_margin_y = _max_int(24, _round_pos(<double>client_h * 0.040))
    cdef int callout_x = width - callout_w - callout_margin_x
    if callout_x < callout_margin_x:
        callout_x = callout_margin_x
    cdef int callout_y = callout_margin_y

    cdef list payload_slots = []
    cdef int rxw, ryw, rww, rhw
    for s in slots:
        rxw = (<dict>s)['screen_rect']['x']
        ryw = (<dict>s)['screen_rect']['y']
        rww = (<dict>s)['screen_rect']['w']
        rhw = (<dict>s)['screen_rect']['h']
        idx = (<dict>s)['index']
        payload_slots.append({
            'index': idx,
            'rect': {'x': rxw - win_x, 'y': ryw - win_y, 'w': rww, 'h': rhw},
        })
    payload_slots.sort(key=_slot_index_key)

    cdef int padding_x_final = pad_x
    if pad_left > padding_x_final:
        padding_x_final = pad_left
    if pad_right > padding_x_final:
        padding_x_final = pad_right

    return {
        'window': {'x': win_x, 'y': win_y, 'w': width, 'h': height},
        'viewport': {
            'width': width, 'height': height,
            'padding_x': padding_x_final, 'padding_y': pad_y,
            'callout': {'x': callout_x, 'y': callout_y,
                        'w': callout_w, 'h': callout_h},
        },
        'slots': payload_slots,
    }


cdef inline object _slot_index_key(dict s):
    return s['index']


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


# ══════════════════════════════════════════════════════════
#  Phase 4: ULW RGBA → premultiplied BGRA (uint8, no float)
# ══════════════════════════════════════════════════════════

cpdef void premultiply_rgba_to_bgra(
    const unsigned char[:, :, :] rgba,
    unsigned char[:, :, :] bgra,
) noexcept:
    """Convert RGBA uint8 image to premultiplied-alpha BGRA uint8 in-place.

    Parameters
    ----------
    rgba : memoryview (h, w, 4) uint8 — source RGBA pixels.
    bgra : memoryview (h, w, 4) uint8 — destination buffer (must be same shape).

    The caller is responsible for allocating ``bgra`` with the correct shape.
    This function runs with the GIL released for maximum throughput.
    """
    cdef Py_ssize_t h = rgba.shape[0]
    cdef Py_ssize_t w = rgba.shape[1]
    cdef Py_ssize_t y, x
    cdef unsigned int r, g, b, a
    with nogil:
        for y in range(h):
            for x in range(w):
                r = rgba[y, x, 0]
                g = rgba[y, x, 1]
                b = rgba[y, x, 2]
                a = rgba[y, x, 3]
                # premultiply: channel = channel * alpha / 255
                # Use (c * a + 127) / 255 for better rounding
                bgra[y, x, 0] = <unsigned char>((b * a + 127) // 255)  # B
                bgra[y, x, 1] = <unsigned char>((g * a + 127) // 255)  # G
                bgra[y, x, 2] = <unsigned char>((r * a + 127) // 255)  # R
                bgra[y, x, 3] = <unsigned char>a


# ══════════════════════════════════════════════════════════
#  Phase 5: Batch signature building (replaces Python sorted+tuple)
# ══════════════════════════════════════════════════════════

cpdef tuple build_batch_sig(list batch):
    """Build a sorted signature tuple from a player roster batch.

    Each element of *batch* is a tuple ``(uid, name, prof, fp, lv)``.
    Returns ``tuple(sorted(batch))`` — equivalent to the Python code in
    ``_push_packet_overlays`` but avoids Python-level sort overhead for
    the common case of small rosters (4-8 players).
    """
    if not batch:
        return ()
    cdef list copy = list(batch)
    copy.sort()
    return tuple(copy)


cpdef tuple build_boss_bar_sig(dict data, list additional):
    """Build the boss-bar overlay signature tuple.

    Mirrors the ``_bb_sig`` construction in ``_push_packet_overlays``.
    Moves the many ``int()/float()/bool()/round()`` calls into Cython
    to reduce per-tick Python overhead.
    """
    cdef bint active = bool(data.get('active', False))
    cdef double hp_pct = _safe_f64(data.get('hp_pct'), 0.0)
    cdef str hp_source = str(data.get('hp_source') or '')
    cdef long long current_hp = _safe_i64(data.get('current_hp'), 0)
    cdef long long total_hp = _safe_i64(data.get('total_hp'), 0)
    cdef bint shield_active = bool(data.get('shield_active', False))
    cdef double shield_pct = _safe_f64(data.get('shield_pct'), 0.0)
    cdef long long breaking_stage = _safe_i64(data.get('breaking_stage'), 0)
    cdef bint has_break_data = bool(data.get('has_break_data', False))
    cdef double extinction_pct = _safe_f64(data.get('extinction_pct'), 0.0)
    cdef long long extinction = _safe_i64(data.get('extinction'), 0)
    cdef long long max_extinction = _safe_i64(data.get('max_extinction'), 0)
    cdef bint stop_breaking_ticking = bool(data.get('stop_breaking_ticking', False))
    cdef bint in_overdrive = bool(data.get('in_overdrive', False))
    cdef bint invincible = bool(data.get('invincible', False))
    cdef str boss_name = str(data.get('boss_name') or '')

    # Build additional sub-tuple
    cdef list add_items = []
    cdef dict u
    for u_obj in (additional or []):
        u = <dict>u_obj
        add_items.append((
            str(u.get('name') or ''),
            round(_safe_f64(u.get('hp_pct'), 0.0), 3),
            round(_safe_f64(u.get('extinction_pct'), 0.0), 3),
            bool(u.get('has_break_data', False)),
            <int>_safe_i64(u.get('breaking_stage'), -1),
            bool(u.get('shield_active', False)),
            round(_safe_f64(u.get('shield_pct'), 0.0), 3),
        ))

    return (
        active,
        hp_pct,
        hp_source,
        current_hp,
        total_hp,
        shield_active,
        shield_pct,
        breaking_stage,
        has_break_data,
        extinction_pct,
        extinction,
        max_extinction,
        stop_breaking_ticking,
        in_overdrive,
        invincible,
        boss_name,
        tuple(add_items),
    )


cpdef list sort_recent_monsters(list monsters, dict recent_targets):
    """Sort monster list by (hp_pct DESC, last_damage_ts DESC).

    Mirrors the ``_sort_key`` lambda in ``_push_packet_overlays``.
    Each element is a monster object with ``.hp``, ``.max_hp``, ``.uuid``
    attributes.
    """
    if not monsters:
        return []
    cdef list decorated = []
    cdef object m
    cdef long long hp, maxhp
    cdef double hp_pct, last_ts
    for m in monsters:
        hp = _safe_i64(getattr(m, 'hp', 0), 0)
        maxhp = _safe_i64(getattr(m, 'max_hp', 0), 0)
        if maxhp <= 0:
            maxhp = hp if hp > 0 else 1
        hp_pct = <double>hp / <double>maxhp if maxhp > 0 else 0.0
        last_ts = _safe_f64(recent_targets.get(getattr(m, 'uuid', 0), 0), 0.0)
        decorated.append((-hp_pct, -last_ts, m))
    decorated.sort()
    cdef list result = []
    for item in decorated:
        result.append(item[2])
    return result
