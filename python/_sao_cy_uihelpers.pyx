# cython: language_level=3
# cython: boundscheck=False
# cython: wraparound=False
# cython: initializedcheck=False
# cython: nonecheck=False
"""Generic platform Cython UI helpers.

This root module is reserved for platform-only arithmetic used by the
SAO shell, popup/menu chrome, left-info panel, and generic overlay plumbing.
Extension-owned hot paths live in extension Cython modules discovered by
``build_cython_ext.py``.
"""

from libc.math cimport exp, floor, sin, pow


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


cdef inline double _abs_f64(double value):
    return -value if value < 0.0 else value


cpdef long long safe_int(object value, long long default=0):
    """Defensive int parse for shared UI row/index values."""
    return _safe_i64(value, default)


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


cpdef tuple popup_child_snapshot_sig(object out_w, object out_h,
                                     object line_w, object line_h,
                                     object arrow_w, object fade_t,
                                     object bg_hex, object rows):
    """Build the dedup signature for the GPU popup child-bar painter."""
    cdef list row_sig = []
    cdef object row
    cdef int hover_q
    if rows is not None:
        for row in rows:
            hover_q = <int>(_safe_f64(getattr(row, 'hover_t', 0.0), 0.0) * 16.0)
            row_sig.append((
                str(getattr(row, 'icon', '') or ''),
                str(getattr(row, 'label', '') or ''),
                hover_q,
                <int>_safe_i64(getattr(row, 'row_w', 0), 0),
            ))
    return (
        <int>_safe_i64(out_w, 0),
        <int>_safe_i64(out_h, 0),
        <int>_safe_i64(line_w, 0),
        <int>_safe_i64(line_h, 0),
        <int>_safe_i64(arrow_w, 0),
        <int>(_safe_f64(fade_t, 0.0) * 16.0),
        str(bg_hex or ''),
        tuple(row_sig),
    )


cpdef tuple left_info_snapshot_sig(object username, object description,
                                   object top_w, object top_h,
                                   object bottom_w, object bottom_h,
                                   object sweep_phase,
                                   object sweep_strength):
    """Build the dedup signature for the GPU left-info painter."""
    cdef double strength = _safe_f64(sweep_strength, 0.0)
    cdef double phase = _safe_f64(sweep_phase, 0.0)
    cdef double sp_q = 0.0
    cdef double ss_q = 0.0
    if strength > 0.005:
        sp_q = <double>_round_pos(phase * 16.0) / 16.0
        ss_q = <double>_round_pos(strength * 16.0) / 16.0
    return (
        str(username or ''),
        str(description or ''),
        <int>_safe_i64(top_w, 0),
        <int>_safe_i64(top_h, 0),
        <int>_safe_i64(bottom_w, 0),
        <int>_safe_i64(bottom_h, 0),
        sp_q,
        ss_q,
    )
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
cdef inline str _normalize_hex_rgb(object color):
    cdef str raw = str(color or '').strip()
    if raw.startswith('#'):
        raw = raw[1:]
    if len(raw) == 8:
        raw = raw[:6]
    elif len(raw) == 3:
        raw = raw[0] * 2 + raw[1] * 2 + raw[2] * 2
    if len(raw) != 6:
        return ''
    return raw.lower()


cpdef str lerp_hex_color(object c1, object c2, double t):
    """RGB hex lerp with support for #rgb / #rrggbb / #rrggbbaa inputs."""
    cdef str raw1
    cdef str raw2
    cdef int r1
    cdef int g1
    cdef int b1
    cdef int r2
    cdef int g2
    cdef int b2
    cdef int r
    cdef int g
    cdef int b
    if t < 0.0:
        t = 0.0
    elif t > 1.0:
        t = 1.0
    raw1 = _normalize_hex_rgb(c1)
    raw2 = _normalize_hex_rgb(c2)
    if not raw1 or not raw2:
        return str(c1 or '#000000')
    try:
        r1 = int(raw1[0:2], 16)
        g1 = int(raw1[2:4], 16)
        b1 = int(raw1[4:6], 16)
        r2 = int(raw2[0:2], 16)
        g2 = int(raw2[2:4], 16)
        b2 = int(raw2[4:6], 16)
    except Exception:
        return str(c1 or '#000000')
    r = <int>(r1 + (r2 - r1) * t)
    g = <int>(g1 + (g2 - g1) * t)
    b = <int>(b1 + (b2 - b1) * t)
    return f'#{r:02x}{g:02x}{b:02x}'


cpdef tuple menu_circle_palette(bint active, double hover_t,
                                object circle_border, object active_border,
                                object circle_bg, object hover_bg,
                                object circle_icon, object hover_icon,
                                object active_bg, object active_icon):
    """Return ``(border, fill, icon)`` for the CPU menu-circle path."""
    if active:
        return (str(active_border or ''),
                str(active_bg or ''),
                str(active_icon or ''))
    if hover_t < 0.0:
        hover_t = 0.0
    elif hover_t > 1.0:
        hover_t = 1.0
    return (
        lerp_hex_color(circle_border, active_border, hover_t),
        lerp_hex_color(circle_bg, hover_bg, hover_t),
        lerp_hex_color(circle_icon, hover_icon, hover_t),
    )


cpdef tuple popup_child_hover_palette(double hover_t,
                                      object child_bg, object child_hover,
                                      object child_text,
                                      object child_hover_fg,
                                      object child_icon,
                                      object active_border):
    """Return ``(bg, fg, icon_fg, ind_color, arr_fg)`` for CPU child-row hover."""
    if hover_t < 0.0:
        hover_t = 0.0
    elif hover_t > 1.0:
        hover_t = 1.0
    return (
        lerp_hex_color(child_bg, child_hover, hover_t),
        lerp_hex_color(child_text, child_hover_fg, hover_t),
        lerp_hex_color(child_icon, child_hover_fg, hover_t),
        lerp_hex_color(child_bg, active_border, hover_t),
        lerp_hex_color(child_bg, child_hover_fg, hover_t),
    )


cpdef tuple popup_child_fade_palette(double hover_t, double fade_t,
                                     object fade_bg, object child_bg,
                                     object child_hover, object child_text,
                                     object child_hover_fg,
                                     object child_icon,
                                     object active_border):
    """Return ``(bg, fg, icon_fg, ind_color, arr_fg)`` for CPU child-row fade."""
    cdef str bg_now
    cdef str fg_now
    cdef str icon_now
    cdef str ind_now
    cdef str arr_now
    if fade_t < 0.0:
        fade_t = 0.0
    elif fade_t > 1.0:
        fade_t = 1.0
    bg_now, fg_now, icon_now, ind_now, arr_now = popup_child_hover_palette(
        hover_t, child_bg, child_hover, child_text,
        child_hover_fg, child_icon, active_border)
    return (
        lerp_hex_color(bg_now, fade_bg, fade_t),
        lerp_hex_color(fg_now, fade_bg, fade_t),
        lerp_hex_color(icon_now, fade_bg, fade_t),
        lerp_hex_color(ind_now, fade_bg, fade_t),
        lerp_hex_color(arr_now, fade_bg, fade_t),
    )


cpdef tuple popup_child_row_anim_step(double now, double anim_t0,
                                      object index, double stagger_s,
                                      double duration_s, object start_w,
                                      object target_w):
    """Return ``(width, keep_animating)`` for the CPU child-row width tween."""
    cdef int idx = <int>_safe_i64(index, 0)
    cdef int sw = <int>_safe_i64(start_w, 72)
    cdef int tw = <int>_safe_i64(target_w, 240)
    cdef double local_t
    cdef double eased
    if sw < 1:
        sw = 1
    if tw < 1:
        tw = 1
    if duration_s <= 0.0:
        return (tw, False)
    local_t = (now - anim_t0 - idx * stagger_s) / duration_s
    if local_t < 0.0:
        local_t = 0.0
    elif local_t > 1.0:
        local_t = 1.0
    eased = 1.0 - ((1.0 - local_t) * (1.0 - local_t) * (1.0 - local_t))
    return (<int>_round_even(sw + (tw - sw) * eased), local_t < 1.0)


cpdef tuple menu_hud_breath_offsets(double elapsed, bint force_zero):
    """Return 2px-quantized HUD drift for the CPU menu overlay."""
    cdef double raw_dx
    cdef double raw_dy
    if force_zero:
        return (0, 0)
    raw_dx = 4.8 * sin(elapsed * 0.44) + 1.8 * sin(elapsed * 0.96)
    raw_dy = 3.8 * sin(elapsed * 0.32 + 1.0) + 1.4 * sin(elapsed * 0.79)
    return (
        <int>(_round_even(raw_dx / 2.0) * 2),
        <int>(_round_even(raw_dy / 2.0) * 2),
    )


cpdef double menu_hud_phase_quantized(double elapsed, bint force_full_rate):
    """Return a cache-friendly HUD phase for the CPU canvas path."""
    if force_full_rate:
        return elapsed
    if elapsed <= 0.0:
        return 0.0
    return _round_even(elapsed * 30.0) / 30.0


cpdef tuple menu_bar_button_size_step(double now, object current_size,
                                      bint enter_active, double enter_t0,
                                      object index, double enter_delay_s,
                                      double enter_duration_s, object hover_idx,
                                      double base_size, double size_eps,
                                      double size_lerp):
    """Return ``(new_size, keep_animating, cursor_ready)`` for one CPU menu button."""
    cdef int idx = <int>_safe_i64(index, 0)
    cdef double cur = _safe_f64(current_size, base_size)
    cdef double local_t
    cdef double target
    cdef double delta
    cdef int dist
    if enter_active:
        if enter_duration_s <= 0.0:
            local_t = 1.0
        else:
            local_t = (now - enter_t0 - idx * enter_delay_s) / enter_duration_s
        if local_t < 0.0:
            local_t = 0.0
        elif local_t > 1.0:
            local_t = 1.0
        return (
            <double>(1 if _round_even(base_size * (1.0 - ((1.0 - local_t) * (1.0 - local_t) * (1.0 - local_t)))) < 1
                     else _round_even(base_size * (1.0 - ((1.0 - local_t) * (1.0 - local_t) * (1.0 - local_t))))),
            local_t < 1.0,
            local_t >= 1.0,
        )
    if hover_idx is not None:
        dist = <int>_safe_i64(hover_idx, -1) - idx
        if dist < 0:
            dist = -dist
        target = base_size * (1.0 + 0.22 * exp(-0.9 * <double>(dist * dist)))
    else:
        target = base_size
    delta = target - cur
    if delta < 0.0:
        if -delta > size_eps:
            return (cur + delta * size_lerp, True, False)
        return (target, False, False)
    if delta > size_eps:
        return (cur + delta * size_lerp, True, False)
    return (target, False, False)


cpdef tuple menu_bar_advance_buttons(object buttons, double now,
                                     bint enter_active, double enter_t0,
                                     double enter_delay_s,
                                     double enter_duration_s,
                                     object hover_idx, double base_size,
                                     double size_eps, double size_lerp):
    """Advance ``btn._size`` across the whole CPU menu bar in one pass.

    Returns ``(keep_animating, ready_indices_tuple)`` where
    ``ready_indices_tuple`` contains buttons whose enter animation has just
    reached the steady cursor-ready state.
    """
    cdef object btn
    cdef list ready = []
    cdef bint keep = False
    cdef int idx = 0
    cdef int hi = <int>_safe_i64(hover_idx, -1)
    cdef int dist
    cdef double cur
    cdef double target
    cdef double delta
    cdef double local_t
    cdef double new_size
    if not buttons:
        return (False, ())
    for btn in buttons:
        cur = _safe_f64(getattr(btn, '_size', base_size), base_size)
        if enter_active:
            if enter_duration_s <= 0.0:
                local_t = 1.0
            else:
                local_t = (now - enter_t0 - <double>idx * enter_delay_s) / enter_duration_s
            if local_t < 0.0:
                local_t = 0.0
            elif local_t > 1.0:
                local_t = 1.0
            new_size = <double>(1 if _round_even(base_size * (1.0 - ((1.0 - local_t) * (1.0 - local_t) * (1.0 - local_t)))) < 1
                               else _round_even(base_size * (1.0 - ((1.0 - local_t) * (1.0 - local_t) * (1.0 - local_t)))))
            btn._size = new_size
            if local_t < 1.0:
                keep = True
            else:
                ready.append(idx)
        else:
            if hover_idx is not None:
                dist = hi - idx
                if dist < 0:
                    dist = -dist
                target = base_size * (1.0 + 0.22 * exp(-0.9 * <double>(dist * dist)))
            else:
                target = base_size
            delta = target - cur
            if delta < 0.0:
                if -delta > size_eps:
                    btn._size = cur + delta * size_lerp
                    keep = True
                else:
                    btn._size = target
            elif delta > size_eps:
                btn._size = cur + delta * size_lerp
                keep = True
            else:
                btn._size = target
        idx += 1
    return (keep, tuple(ready))


cpdef tuple menu_circle_visual_metrics(double size_f, double max_size):
    """Return clamped size/offset/signature helpers for CPU menu-circle draw."""
    cdef int size_px
    cdef double size_q
    cdef double off
    cdef int ioff
    cdef bint snap
    if size_f < 1.0:
        size_f = 1.0
    if size_f > max_size:
        size_f = max_size
    size_px = <int>size_f
    if size_f > <double>size_px:
        size_px += 1
    if size_px < 1:
        size_px = 1
    size_q = _round_even(size_f * 4.0) / 4.0
    off = (max_size - size_f) / 2.0
    ioff = <int>_round_even(off)
    snap = _abs_f64(off - <double>ioff) < 0.25
    return (size_f, size_px, size_q, off, ioff, snap)


cpdef tuple menu_plate_sweep_quantized(object sweep_phase,
                                       object sweep_strength,
                                       double strength_scale=1.0):
    """Return quantized ``(phase, strength)`` for CPU menu plate sweeps."""
    cdef double phase = _safe_f64(sweep_phase, 0.0)
    cdef double strength = _safe_f64(sweep_strength, 0.0) * strength_scale
    if strength <= 0.005:
        return (0.0, 0.0)
    return (
        _round_even(phase * 16.0) / 16.0,
        _round_even(strength * 16.0) / 16.0,
    )


cpdef tuple left_panel_progress_dims(double top_t, double bottom_t,
                                     object target_w, object top_h,
                                     object bottom_h, object sweep_phase,
                                     object sweep_strength):
    """Return clamped panel progresses, sweep inputs, and pixel sizes."""
    cdef double tt = top_t
    cdef double bt = bottom_t
    cdef double sp = _safe_f64(sweep_phase, 0.0)
    cdef double ss = _safe_f64(sweep_strength, 0.0)
    cdef int tw = <int>_safe_i64(target_w, 1)
    cdef int th = <int>_safe_i64(top_h, 1)
    cdef int bh = <int>_safe_i64(bottom_h, 1)
    cdef int out_top_w
    cdef int out_top_h
    cdef int out_bottom_w
    cdef int out_bottom_h
    cdef double bottom_ref
    if tt < 0.0:
        tt = 0.0
    elif tt > 1.0:
        tt = 1.0
    if bt < 0.0:
        bt = 0.0
    elif bt > 1.0:
        bt = 1.0
    if sp < 0.0:
        sp = 0.0
    elif sp > 1.0:
        sp = 1.0
    if ss < 0.0:
        ss = 0.0
    elif ss > 1.0:
        ss = 1.0
    bottom_ref = tt
    if bt * 0.94 > bottom_ref:
        bottom_ref = bt * 0.94
    out_top_w = <int>_round_even(<double>tw * tt)
    out_top_h = <int>_round_even(<double>th * tt)
    out_bottom_w = <int>_round_even(<double>tw * bottom_ref)
    out_bottom_h = <int>_round_even(<double>bh * bt)
    if out_top_w < 1:
        out_top_w = 1
    if out_top_h < 1:
        out_top_h = 1
    if out_bottom_w < 1:
        out_bottom_w = 1
    if out_bottom_h < 1:
        out_bottom_h = 1
    return (tt, bt, sp, ss, out_top_w, out_top_h, out_bottom_w, out_bottom_h)
cdef inline int _round_pos(double v):
    if v >= 0.0:
        return <int>(v + 0.5)
    return <int>(v - 0.5)
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
