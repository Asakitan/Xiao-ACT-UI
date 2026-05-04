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

from libc.math cimport exp, floor, sin


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


cpdef tuple player_panel_anim_size(object width, object height, double t):
    """Scale a panel rect by animation progress, clamped to at least 1px."""
    cdef int w = <int>_safe_i64(width, 1)
    cdef int h = <int>_safe_i64(height, 1)
    cdef int out_w = <int>(<double>w * t)
    cdef int out_h = <int>(<double>h * t)
    if out_w < 1:
        out_w = 1
    if out_h < 1:
        out_h = 1
    return (out_w, out_h)


cpdef double scan_phase(double now):
    """HUD scan phase matching ``(sin(time.time()*1.5)+1)/2``."""
    return (sin(now * 1.5) + 1.0) / 2.0


cpdef int scan_x(object width, double now):
    """Tk scan-bar x coordinate for the player info panel."""
    cdef int w = <int>_safe_i64(width, 0)
    return 10 + <int>((<double>(w - 20)) * scan_phase(now))


cpdef str short_session_name(object name):
    """Trim session-player names to the compact roster column width."""
    cdef str text = str(name or '').strip()
    if not text:
        return '--'
    if len(text) <= 14:
        return text
    return text[:13] + '…'


cpdef tuple session_rows_signature(list rows):
    """Build the roster change signature used by ``update_rows``."""
    cdef list sig = []
    cdef object row
    if not rows:
        return ()
    for row in rows:
        if not isinstance(row, dict):
            continue
        sig.append((
            str(row.get('uid') or ''),
            str(row.get('name') or ''),
            <int>_safe_i64(row.get('fight_power_value'), 0),
            bool(row.get('is_self')),
        ))
    return tuple(sig)


cpdef str session_self_uid(list rows):
    """Return the UID for the self row, or an empty string."""
    cdef object row
    if not rows:
        return ''
    for row in rows:
        if not isinstance(row, dict):
            continue
        if row.get('is_self'):
            return str(row.get('uid') or '')
    return ''


cpdef int clamp_session_first_index(object first, object total, object visible_count):
    """Clamp the first visible GPU roster row to the valid range."""
    cdef int f = <int>_safe_i64(first, 0)
    cdef int t = <int>_safe_i64(total, 0)
    cdef int vc = <int>_safe_i64(visible_count, 1)
    cdef int max_first = t - vc
    if max_first < 0:
        max_first = 0
    if f < 0:
        return 0
    if f > max_first:
        return max_first
    return f


cpdef int session_scroll_delta(object num, object delta_obj):
    """Normalize Windows/X11 mouse-wheel events to roster scroll units."""
    cdef long long n = _safe_i64(num, 0)
    cdef long long raw_delta
    cdef int delta
    if n == 4:
        return -3
    if n == 5:
        return 3
    raw_delta = _safe_i64(delta_obj, 0)
    delta = <int>(-1.0 * (<double>raw_delta / 120.0))
    if delta == 0:
        return -1 if raw_delta > 0 else 1
    return delta


cpdef int session_scroll_first_index(object first, object total,
                                     object visible_count, object delta):
    """Apply a scroll delta and clamp the GPU roster first row."""
    return clamp_session_first_index(
        _safe_i64(first, 0) + _safe_i64(delta, 0), total, visible_count)


cpdef list session_visible_rows(list rows, object first, object visible_count):
    """Build compact GPU roster rows from the source row dictionaries."""
    cdef int total = <int>len(rows) if rows is not None else 0
    cdef int vc = <int>_safe_i64(visible_count, 1)
    cdef int start = clamp_session_first_index(first, total, vc)
    cdef int end = start + vc
    cdef list out = []
    cdef object row
    cdef int idx
    if end > total:
        end = total
    for idx in range(start, end):
        row = rows[idx]
        if not isinstance(row, dict):
            continue
        out.append((
            short_session_name(row.get('name') or ''),
            str(row.get('uid') or '--'),
            str(row.get('fight_power') or '--'),
            bool(row.get('is_self')),
        ))
    return out


cpdef double cubic_open_reveal(double elapsed, double duration):
    """Ease-out cubic reveal used by the session-player open animation."""
    cdef double t
    if duration <= 0.0:
        return 1.0
    t = elapsed / duration
    if t < 0.0:
        t = 0.0
    elif t > 1.0:
        t = 1.0
    return 1.0 - ((1.0 - t) * (1.0 - t) * (1.0 - t))


cpdef tuple session_open_anim_geometry(object panel_h, double elapsed,
                                       double duration):
    """Return ``(t, ease, height, offset, highlight_on)`` for list open."""
    cdef double t
    cdef double ease
    cdef int ph = <int>_safe_i64(panel_h, 1)
    cdef int height
    cdef int offset
    if duration <= 0.0:
        t = 1.0
    else:
        t = elapsed / duration
        if t < 0.0:
            t = 0.0
        elif t > 1.0:
            t = 1.0
    ease = 1.0 - ((1.0 - t) * (1.0 - t) * (1.0 - t))
    height = <int>((<double>ph * ease) + 0.5)
    if height < 1:
        height = 1
    offset = <int>((64.0 * (1.0 - ease)) + 0.5)
    return (t, ease, height, offset, t < 0.55)


cpdef tuple sao_fx_coords(double tt, object panel_id, object width):
    """Integer SAO HUD decoration coordinates for the shared panel tick."""
    cdef long long pid = _safe_i64(panel_id, 0)
    cdef int pw = <int>_safe_i64(width, 80)
    cdef double t = tt + <double>(pid % 17) * 0.13
    cdef int left_far = <int>(10.0 + 5.0 * sin(t * 0.66))
    cdef int left_near = <int>(20.0 + 12.0 * sin(t * 1.35 + 0.8))
    cdef int right_far = <int>(<double>pw - 18.0 + 7.0 * sin(t * 0.72 + 1.1))
    cdef int right_near = <int>(<double>pw - 34.0 + 12.0 * sin(t * 1.45 + 2.1))
    return (left_far, left_near, right_far, right_near)


cpdef int popup_visible_count(object menu_count, object max_visible):
    """Visible menu-button count for the GPU entity popup."""
    cdef int n = <int>_safe_i64(menu_count, 0)
    cdef int m = <int>_safe_i64(max_visible, 7)
    if n < 0:
        n = 0
    if m < 0:
        m = 0
    return n if n < m else m


cpdef int popup_column_height(object menu_count, object max_visible, object slot):
    """Menu column height for the GPU entity popup."""
    cdef int visible = popup_visible_count(menu_count, max_visible)
    cdef int s = <int>_safe_i64(slot, 70)
    if visible < 1:
        visible = 1
    return s * visible


cpdef tuple popup_content_shift(object fade_alpha):
    """Small master slide used during popup open/close."""
    cdef double alpha = _safe_f64(fade_alpha, 1.0)
    if alpha < 0.0:
        alpha = 0.0
    elif alpha > 1.0:
        alpha = 1.0
    cdef int shift_x = <int>(-((1.0 - alpha) * 14.0 + 0.5))
    cdef int shift_y = <int>(((1.0 - alpha) * 10.0) + 0.5)
    return (shift_x, shift_y)


cpdef tuple popup_origins(object fade_alpha, object hud_pad,
                          object menu_x, object child_x):
    """Return menu/child origins after applying popup content shift."""
    cdef int dx, dy
    dx, dy = popup_content_shift(fade_alpha)
    cdef int pad = <int>_safe_i64(hud_pad, 24)
    cdef int mx = <int>_safe_i64(menu_x, 24)
    cdef int cx = <int>_safe_i64(child_x, 119)
    return ((mx + dx, pad + dy), (cx + dx, pad + dy))


cpdef tuple popup_content_size(object menu_count, object child_count,
                               object max_visible, object slot,
                               object row_stride, object menu_width,
                               object gap, object child_width):
    """Return ``(content_w, content_h)`` for the GPU popup frame."""
    cdef int menu_h = popup_column_height(menu_count, max_visible, slot)
    cdef int cc = <int>_safe_i64(child_count, 0)
    cdef int rs = <int>_safe_i64(row_stride, 47)
    cdef int child_h = cc * rs if cc > 0 else 0
    cdef int inner_h = menu_h
    if child_h > inner_h:
        inner_h = child_h
    if inner_h < 1:
        inner_h = 1
    cdef int inner_w = (<int>_safe_i64(menu_width, 70)
                        + <int>_safe_i64(gap, 25)
                        + <int>_safe_i64(child_width, 267))
    return (inner_w, inner_h)


cpdef tuple popup_window_size(object menu_count, object child_count,
                              object reserved_rows, object max_visible,
                              object slot, object row_stride,
                              object menu_width, object gap,
                              object child_width, object hud_pad):
    """Return reserved GPU popup window size."""
    cdef int iw, ih
    iw, ih = popup_content_size(menu_count, child_count, max_visible, slot,
                                row_stride, menu_width, gap, child_width)
    cdef int rr = <int>_safe_i64(reserved_rows, 0)
    cdef int rs = <int>_safe_i64(row_stride, 47)
    cdef int reserved_h = rr * rs if rr > 0 else 0
    cdef int menu_h = popup_column_height(menu_count, max_visible, slot)
    if reserved_h > ih:
        ih = reserved_h
    if menu_h > ih:
        ih = menu_h
    cdef int pad = <int>_safe_i64(hud_pad, 24)
    return (iw + pad * 2, ih + pad * 2)


cpdef tuple popup_menu_button_frame(object size_value, object slot,
                                    object max_size, object index):
    """Return ``(size_f, size_px, ox, oy)`` for a fisheye menu button."""
    cdef double size_f = _safe_f64(size_value, 54.0)
    cdef double mx = _safe_f64(max_size, 70.0)
    cdef double sl = _safe_f64(slot, 70.0)
    cdef int idx = <int>_safe_i64(index, 0)
    if size_f < 1.0:
        size_f = 1.0
    if size_f > mx:
        size_f = mx
    cdef int size_px = <int>size_f
    if size_f > <double>size_px:
        size_px += 1
    if size_px < 1:
        size_px = 1
    cdef int ox = <int>(((sl - size_f) / 2.0) + 0.5)
    cdef int oy = <int>(((<double>idx * sl + (sl - size_f) / 2.0)) + 0.5)
    return (size_f, size_px, ox, oy)


cpdef bint popup_advance_menu_animation(object btn_size, object btn_hover_t,
                                        object hover_idx, object menu_count,
                                        object max_visible, double base_size,
                                        double size_eps, double size_lerp,
                                        double hover_lerp):
    """Advance entity popup menu fisheye + hover arrays in-place."""
    cdef int n = popup_visible_count(menu_count, max_visible)
    if n <= 0:
        return False
    while len(btn_size) < n:
        btn_size.append(base_size)
    while len(btn_hover_t) < n:
        btn_hover_t.append(0.0)
    cdef bint has_hover = hover_idx is not None
    cdef int hi = <int>_safe_i64(hover_idx, -1)
    cdef bint keep = False
    cdef int i, dist
    cdef double target, delta, ht_target, ht_delta
    for i in range(n):
        if has_hover:
            dist = hi - i
            if dist < 0:
                dist = -dist
            target = base_size * (1.0 + 0.22 * exp(-0.9 * <double>(dist * dist)))
        else:
            target = base_size
        delta = target - _safe_f64(btn_size[i], base_size)
        if delta < 0.0:
            if -delta > size_eps:
                btn_size[i] = _safe_f64(btn_size[i], base_size) + delta * size_lerp
                keep = True
            else:
                btn_size[i] = target
        else:
            if delta > size_eps:
                btn_size[i] = _safe_f64(btn_size[i], base_size) + delta * size_lerp
                keep = True
            else:
                btn_size[i] = target
        ht_target = 1.0 if (has_hover and hi == i) else 0.0
        ht_delta = ht_target - _safe_f64(btn_hover_t[i], 0.0)
        if ht_delta < 0.0:
            if -ht_delta > 0.01:
                btn_hover_t[i] = _safe_f64(btn_hover_t[i], 0.0) + ht_delta * hover_lerp
                keep = True
            else:
                btn_hover_t[i] = ht_target
        else:
            if ht_delta > 0.01:
                btn_hover_t[i] = _safe_f64(btn_hover_t[i], 0.0) + ht_delta * hover_lerp
                keep = True
            else:
                btn_hover_t[i] = ht_target
    return keep


cpdef list popup_menu_hit_rects(object menu_count, object max_visible,
                                object slot, object x_off, object y_off):
    """Return menu button hit rects for the GPU popup."""
    cdef int n = popup_visible_count(menu_count, max_visible)
    cdef int s = <int>_safe_i64(slot, 70)
    cdef int x = <int>_safe_i64(x_off, 0)
    cdef int y = <int>_safe_i64(y_off, 0)
    cdef list out = []
    cdef int i, y1
    for i in range(n):
        y1 = y + i * s
        out.append(((x, y1, x + s, y1 + s), i))
    return out


cpdef int popup_child_height(object child_count, object row_stride):
    """Child row column height for the GPU popup."""
    cdef int n = <int>_safe_i64(child_count, 0)
    if n <= 0:
        return 0
    return n * <int>_safe_i64(row_stride, 47)


cpdef bint popup_advance_child_animation(object row_hover_t, object row_anim_w,
                                         object hover_idx, object child_count,
                                         double now, double row_anim_t0,
                                         double target_row_w, double duration,
                                         double stagger, double hover_lerp):
    """Advance entity popup child-row slide/hover arrays in-place."""
    cdef int n = <int>_safe_i64(child_count, 0)
    while len(row_hover_t) < n:
        row_hover_t.append(0.0)
    while len(row_anim_w) < n:
        row_anim_w.append(0)
    cdef bint has_hover = hover_idx is not None
    cdef int hi = <int>_safe_i64(hover_idx, -1)
    cdef bint keep = False
    cdef int i, target_w
    cdef double local_t, st, ht_target, delta
    for i in range(n):
        if duration <= 0.0:
            local_t = 1.0
        else:
            local_t = (now - row_anim_t0 - <double>i * stagger) / duration
        if local_t < 0.0:
            local_t = 0.0
        elif local_t > 1.0:
            local_t = 1.0
        st = 1.0 - ((1.0 - local_t) * (1.0 - local_t) * (1.0 - local_t))
        target_w = <int>(target_row_w * st + 0.5)
        if _safe_i64(row_anim_w[i], 0) != target_w:
            row_anim_w[i] = target_w
            keep = True
        if local_t < 1.0:
            keep = True
        ht_target = 1.0 if (has_hover and hi == i) else 0.0
        delta = ht_target - _safe_f64(row_hover_t[i], 0.0)
        if delta < 0.0:
            if -delta > 0.01:
                row_hover_t[i] = _safe_f64(row_hover_t[i], 0.0) + delta * hover_lerp
                keep = True
            else:
                row_hover_t[i] = ht_target
        else:
            if delta > 0.01:
                row_hover_t[i] = _safe_f64(row_hover_t[i], 0.0) + delta * hover_lerp
                keep = True
            else:
                row_hover_t[i] = ht_target
    return keep


cpdef list popup_child_hit_rects(object child_count, object row_anim_w,
                                 object x_off, object y_off, object list_x,
                                 object row_stride, object row_h,
                                 object target_row_w):
    """Return child row hit rects for the GPU popup."""
    cdef int n = <int>_safe_i64(child_count, 0)
    cdef int x1 = <int>_safe_i64(x_off, 0) + <int>_safe_i64(list_x, 27)
    cdef int y = <int>_safe_i64(y_off, 0)
    cdef int rs = <int>_safe_i64(row_stride, 47)
    cdef int rh = <int>_safe_i64(row_h, 36)
    cdef int target_w = <int>_safe_i64(target_row_w, 240)
    cdef int i, y1, rw
    cdef list out = []
    for i in range(n):
        rw = <int>_safe_i64(row_anim_w[i] if i < len(row_anim_w) else target_w, target_w)
        if rw < 1:
            rw = 1
        y1 = y + i * rs
        out.append(((x1, y1, x1 + rw, y1 + rh), i))
    return out


cpdef object popup_pick_hit(list regions, tuple bounds, object x, object y):
    """Pick a region from cached GPU popup hit rectangles."""
    cdef int ix = <int>_safe_i64(x, 0)
    cdef int iy = <int>_safe_i64(y, 0)
    cdef object item
    cdef object rect
    cdef int x1, y1, x2, y2
    for item in regions:
        rect = item[0]
        x1 = <int>rect[0]; y1 = <int>rect[1]
        x2 = <int>rect[2]; y2 = <int>rect[3]
        if x1 <= ix < x2 and y1 <= iy < y2:
            return (item[1], item[2])
    x1 = <int>bounds[0]; y1 = <int>bounds[1]
    x2 = <int>bounds[2]; y2 = <int>bounds[3]
    if x1 <= ix < x2 and y1 <= iy < y2:
        return ('background', -1)
    return None


cpdef tuple popup_hud_dynamic(object content_w, object content_h,
                              double phase, object plate_pad,
                              object hud_margin, object bracket_len):
    """Return dynamic HUD frame coordinates shared by Canvas/PIL paths."""
    cdef int cw = <int>_safe_i64(content_w, 260)
    cdef int ch = <int>_safe_i64(content_h, 180)
    cdef int pp = <int>_safe_i64(plate_pad, 16)
    cdef int hm = <int>_safe_i64(hud_margin, 6)
    cdef int bl = <int>_safe_i64(bracket_len, 16)
    cdef int cx1 = pp - hm
    cdef int cy1 = pp - hm
    cdef int cx2 = pp + cw + hm
    cdef int cy2 = pp + ch + hm
    cdef double scan_period = 6.0
    cdef double scan_pos = (phase % scan_period) / scan_period
    cdef int scan_y = <int>(<double>cy1 + <double>(cy2 - cy1) * scan_pos)
    cdef int dot_travel = cy2 - cy1 - bl * 2
    if dot_travel < 1:
        dot_travel = 1
    cdef int dot_y_l = cy1 + bl + <int>(<double>dot_travel * ((sin(phase * 0.8) + 1.0) * 0.5))
    cdef int dot_y_r = cy1 + bl + <int>(<double>dot_travel * ((sin(phase * 0.8 + 3.141592653589793) + 1.0) * 0.5))
    return (cx1, cy1, cx2, cy2, scan_y, dot_y_l, dot_y_r)


cpdef tuple popup_tick_dt(double tick_now, object last_tick_t):
    """Return ``(dt, tick_now)`` for popup's 60Hz animation clock."""
    cdef double last = _safe_f64(last_tick_t, 0.0)
    cdef double dt
    if last > 0.0:
        dt = tick_now - last
    else:
        dt = 1.0 / 60.0
    if dt < 0.0:
        dt = 0.0
    elif dt > 0.10:
        dt = 0.10
    return (dt, tick_now)


cpdef tuple popup_fade_alpha(double tick_now, object fade_t0,
                             object fade_duration, object fade_target):
    """Return ``(alpha, done, t)`` for popup fade in/out."""
    cdef double dur = _safe_f64(fade_duration, 0.0)
    cdef double target = _safe_f64(fade_target, 1.0)
    if dur <= 0.0:
        dur = 0.45 if target > 0.0 else 0.30
    cdef double t = (tick_now - _safe_f64(fade_t0, tick_now)) / dur
    if t < 0.0:
        t = 0.0
    elif t > 1.0:
        t = 1.0
    cdef double alpha = t if target > 0.0 else 1.0 - t
    return (alpha, t >= 1.0, t)


cpdef tuple popup_child_phase_step(object phase, object fade_t, double dt):
    """Advance child fade phase. Returns ``(phase, fade_t, completed)``."""
    cdef str ph = str(phase or 'idle')
    cdef double ft = _safe_f64(fade_t, 1.0)
    cdef bint completed = False
    if ph == 'fadeout':
        ft += dt / 0.16
        if ft > 1.0:
            ft = 1.0
        if ft >= 0.999:
            completed = True
    elif ph == 'fadein':
        ft -= dt / 0.22
        if ft < 0.0:
            ft = 0.0
        if ft <= 0.001:
            ft = 0.0
            ph = 'idle'
            completed = True
    return (ph, ft, completed)


cpdef int popup_max_child_rows(object child_menus):
    """Find the largest child-menu row count."""
    cdef int max_rows = 0
    cdef object items
    try:
        iterable = child_menus.values()
    except Exception:
        return 0
    for items in iterable:
        try:
            if len(items) > max_rows:
                max_rows = <int>len(items)
        except Exception:
            continue
    return max_rows


cpdef object menu_bar_slot_index(object x, object y, object max_size,
                                 object slot, object button_count):
    """Map GPU menubar cursor coords to a button slot index."""
    cdef double fx = _safe_f64(x, -1.0)
    cdef double fy = _safe_f64(y, -1.0)
    cdef int mx = <int>_safe_i64(max_size, 0)
    cdef int sl = <int>_safe_i64(slot, 1)
    cdef int count = <int>_safe_i64(button_count, 0)
    cdef int idx
    if count <= 0 or sl <= 0:
        return None
    if fx < 0.0 or fy < 0.0 or fx >= <double>mx:
        return None
    idx = <int>(fy / <double>sl)
    if idx < 0 or idx >= count:
        return None
    return idx


cpdef tuple menu_bar_snapshot_sig(object strip_w, object strip_h, object snapshots):
    """Build the visual dedup signature for the GPU menu-bar painter."""
    cdef list sig_buttons = []
    cdef object s
    cdef double size_q, hover_q
    if snapshots is None:
        return (<int>_safe_i64(strip_w, 0), <int>_safe_i64(strip_h, 0), 0, ())
    for s in snapshots:
        size_q = <double>(<int>(_safe_f64(getattr(s, 'size', 0.0), 0.0) * 4.0 + 0.5)) / 4.0
        hover_q = <double>(<int>(_safe_f64(getattr(s, 'hover_t', 0.0), 0.0) * 20.0 + 0.5)) / 20.0
        sig_buttons.append((
            size_q,
            hover_q,
            bool(getattr(s, 'active', False)),
            str(getattr(s, 'icon', '') or ''),
        ))
    return (<int>_safe_i64(strip_w, 0), <int>_safe_i64(strip_h, 0),
            <int>len(sig_buttons), tuple(sig_buttons))


cdef inline long _round_even(double v):
    cdef double fl = floor(v)
    cdef double frac = v - fl
    cdef long base = <long>fl
    if frac > 0.5:
        return base + 1
    if frac < 0.5:
        return base
    if base & 1:
        return base + 1
    return base


cpdef tuple hp_layout_metrics(object screen_w, double hud_vw_pct,
                              double stage_width_pct, double shadow_gutter,
                              object cover_w, object box_w, object sta_w):
    """Compute HP overlay CSS-derived layout metrics."""
    cdef int sw = <int>_safe_i64(screen_w, 1920)
    if sw < 1:
        sw = 1
    cdef int viewport_w = <int>_round_even(<double>sw * hud_vw_pct)
    cdef int stage_w = <int>_round_even(<double>viewport_w * stage_width_pct)
    cdef int id_x = <int>_round_even(<double>stage_w * 0.032 - 25.0)
    cdef int id_w = <int>_round_even(<double>stage_w * 0.396 + 55.0)
    cdef int cover_x = <int>_round_even(<double>stage_w * 0.452 + 25.0)
    cdef int box_x = <int>_round_even(<double>stage_w * 0.47 + 25.0)
    cdef int sta_x = box_x
    cdef int right_edge = id_x + id_w
    cdef int tmp = cover_x + <int>_safe_i64(cover_w, 0)
    if tmp > right_edge:
        right_edge = tmp
    tmp = box_x + <int>_safe_i64(box_w, 0)
    if tmp > right_edge:
        right_edge = tmp
    tmp = sta_x + <int>_safe_i64(sta_w, 0)
    if tmp > right_edge:
        right_edge = tmp
    cdef int panel_w = <int>(<double>right_edge + shadow_gutter)
    if (<double>panel_w) < (<double>right_edge + shadow_gutter):
        panel_w += 1
    return (stage_w, panel_w, id_x, id_w, cover_x, box_x, sta_x)


cpdef int hp_stage_screen_x(object screen_w, double window_left_pct,
                            double hud_vw_pct, double stage_left_pct):
    """Screen X for the HP stage anchor."""
    cdef int sw = <int>_safe_i64(screen_w, 1920)
    return <int>_round_even(<double>sw * window_left_pct
                            + <double>sw * hud_vw_pct * stage_left_pct)


cpdef str hp_fmt_int(object value):
    """Comma-format a rounded HP/STA numeric value."""
    try:
        return f'{_round_even(float(value)):,}'
    except Exception:
        return str(value)


cdef inline long _round_half_up_nonneg(double value):
    if value <= 0.0:
        return 0
    return <long>(value + 0.5)


cpdef str dps_to_fixed_half_up(object value, int digits):
    """Fast non-negative half-up fixed formatter for DPS UI numbers."""
    cdef double v
    cdef double scale = 1.0
    cdef long rounded
    cdef long whole
    cdef long frac
    cdef int i
    try:
        v = float(value)
    except Exception:
        v = 0.0
    if digits <= 0:
        return str(_round_half_up_nonneg(v))
    for i in range(digits):
        scale *= 10.0
    rounded = _round_half_up_nonneg(v * scale)
    whole = rounded // <long>scale
    frac = rounded - whole * <long>scale
    return f'{whole}.{frac:0{digits}d}'


cpdef long dps_round_half_up_int(object value):
    try:
        return _round_half_up_nonneg(float(value))
    except Exception:
        return 0


cpdef str dps_fmt_num(object value):
    cdef double v
    try:
        v = float(value or 0)
    except Exception:
        v = 0.0
    if v >= 1000000.0:
        return f'{dps_to_fixed_half_up(v / 1000000.0, 0 if v >= 10000000.0 else 1)}M'
    if v >= 1000.0:
        return f'{dps_to_fixed_half_up(v / 1000.0, 0 if v >= 100000.0 else 1)}K'
    return f'{_round_half_up_nonneg(v):,}'


cpdef str dps_fmt_fp(object value):
    cdef double fp
    try:
        fp = float(value or 0)
    except Exception:
        fp = 0.0
    if fp <= 0.0:
        return ''
    if fp >= 1000000.0:
        return f'{dps_to_fixed_half_up(fp / 1000000.0, 2)}M'
    if fp >= 1000.0:
        return f'{dps_to_fixed_half_up(fp / 1000.0, 0 if fp >= 100000.0 else 1)}K'
    return f'{_round_half_up_nonneg(fp):,}'


cpdef str dps_fmt_time(object seconds):
    cdef long s
    try:
        s = <long>float(seconds or 0)
    except Exception:
        s = 0
    if s < 0:
        s = 0
    return f'{s // 60:02d}:{s % 60:02d}'


cpdef double ease_out_cubic(double t):
    if t < 0.0:
        t = 0.0
    elif t > 1.0:
        t = 1.0
    return 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t)


cpdef double lerp_clamped(double a, double b, double t):
    if t < 0.0:
        t = 0.0
    elif t > 1.0:
        t = 1.0
    return a + (b - a) * t


cpdef str bosshp_fmt_hp(object value):
    cdef double v
    try:
        v = float(value or 0)
    except Exception:
        v = 0.0
    if v >= 1000000000.0:
        return f'{v / 1000000000.0:.2f}B'
    if v >= 1000000.0:
        return f'{v / 1000000.0:.2f}M'
    if v >= 10000.0:
        return f'{v / 1000.0:.1f}K'
    return f'{_round_even(v):,}'


cpdef tuple mix_rgba(object a, object b, double t):
    if t < 0.0:
        t = 0.0
    elif t > 1.0:
        t = 1.0
    return (
        <int>(int(a[0]) + (int(b[0]) - int(a[0])) * t),
        <int>(int(a[1]) + (int(b[1]) - int(a[1])) * t),
        <int>(int(a[2]) + (int(b[2]) - int(a[2])) * t),
        <int>(int(a[3]) + (int(b[3]) - int(a[3])) * t),
    )


cpdef list offset_poly(object points, int dx, int dy):
    cdef list out = []
    cdef object p
    for p in points:
        out.append((int(p[0]) + dx, int(p[1]) + dy))
    return out


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
