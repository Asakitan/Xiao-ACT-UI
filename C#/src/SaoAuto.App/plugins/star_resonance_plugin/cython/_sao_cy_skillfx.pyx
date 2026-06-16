# cython: language_level=3
# cython: boundscheck=False
# cython: wraparound=False
# cython: initializedcheck=False
# cython: nonecheck=False
"""Mandatory Cython kernels for SkillFX pixel generation."""

from libc.math cimport exp, fabs, sqrt


cdef inline unsigned char _u8(double value) nogil:
    if value < 0.0:
        return <unsigned char>0
    if value > 255.0:
        return <unsigned char>255
    return <unsigned char>value


cpdef bytes beam_rgba(int length, int height):
    """Return the cyan-gold beam as packed RGBA bytes."""
    cdef int L = length if length > 0 else 1
    cdef int H = height if height > 0 else 1
    cdef bytearray out = bytearray(L * H * 4)
    cdef unsigned char[:] dst = out
    cdef int x
    cdef int y
    cdef Py_ssize_t i = 0
    cdef double cy = H * 0.5
    cdef double inv_cy = 1.0 / cy
    cdef double inv_L = 1.0 / max(1, L - 1)
    cdef double ny
    cdef double falloff
    cdef double xs
    cdef double t1
    cdef double t2
    cdef double R
    cdef double G
    cdef double B
    cdef double A_base
    cdef double A_glow
    cdef double finalA
    cdef bint is_core

    with nogil:
        for y in range(H):
            ny = (y - cy) * inv_cy
            falloff = exp(-(ny * ny) * 4.0)
            is_core = fabs(y - cy) <= 1.5
            for x in range(L):
                xs = x * inv_L
                t1 = xs / 0.22
                if t1 < 0.0:
                    t1 = 0.0
                elif t1 > 1.0:
                    t1 = 1.0
                t2 = (xs - 0.35) / 0.65
                if t2 < 0.0:
                    t2 = 0.0
                elif t2 > 1.0:
                    t2 = 1.0
                R = 97.0 + 158.0 * t2
                G = 232.0 - 44.0 * t2
                B = 255.0 - 189.0 * t2
                A_base = 150.0 * (0.15 + 0.85 * t1)
                A_glow = A_base * falloff
                finalA = 245.0 if is_core and 245.0 > A_glow else A_glow
                dst[i] = _u8(R)
                dst[i + 1] = _u8(G)
                dst[i + 2] = _u8(B)
                dst[i + 3] = _u8(finalA)
                i += 4
    return bytes(out)


cpdef bytes ring_layer_rgba(int box, double r_out, double pulse_q,
                            double r_core):
    """Return the SkillFX ring halo/core layer as packed RGBA bytes."""
    cdef int size = box if box > 0 else 1
    cdef bytearray out = bytearray(size * size * 4)
    cdef unsigned char[:] dst = out
    cdef int x
    cdef int y
    cdef Py_ssize_t i = 0
    cdef double cc = size * 0.5
    cdef double halo_breath = 0.52 + 0.40 * pulse_q
    cdef double halo_amp = 46.0 * halo_breath
    cdef double inv_sigma_sq = 1.0 / (28.0 * 28.0)
    cdef double r_core_safe = r_core if r_core > 1.0 else 1.0
    cdef double inv_r_core = 1.0 / r_core_safe
    cdef double core_amp = 70.0 * (0.6 + 0.4 * pulse_q)
    cdef double r_core_p2 = r_core + 2.0
    cdef double dx
    cdef double dy
    cdef double dist
    cdef double drm
    cdef double halo_a
    cdef double ca
    cdef double core_a
    cdef double src_a
    cdef double inv
    cdef double R
    cdef double G
    cdef double B
    cdef double A

    with nogil:
        for y in range(size):
            dy = y - cc
            for x in range(size):
                dx = x - cc
                dist = sqrt(dx * dx + dy * dy)
                drm = dist - r_out
                halo_a = halo_amp * exp(-(drm * drm) * inv_sigma_sq)
                if halo_a < 0.0:
                    halo_a = 0.0
                elif halo_a > 255.0:
                    halo_a = 255.0
                if dist < r_core_p2:
                    ca = 1.0 - dist * inv_r_core
                    if ca < 0.0:
                        ca = 0.0
                    elif ca > 1.0:
                        ca = 1.0
                    core_a = core_amp * ca
                else:
                    core_a = 0.0
                src_a = core_a / 255.0
                inv = 1.0 - src_a
                R = 97.0 * inv + 176.0 * src_a
                G = 232.0 * inv + 247.0 * src_a
                B = 255.0
                A = halo_a if halo_a > core_a else core_a
                dst[i] = _u8(R)
                dst[i + 1] = _u8(G)
                dst[i + 2] = _u8(B)
                dst[i + 3] = _u8(A)
                i += 4
    return bytes(out)


cpdef bytes ring_sweep_rgba(int box, double band_x, double clip_r,
                            double alpha_mul):
    """Return the moving ring sweep band as packed RGBA bytes."""
    cdef int size = box if box > 0 else 1
    cdef bytearray out = bytearray(size * size * 4)
    cdef unsigned char[:] dst = out
    cdef int x
    cdef int y
    cdef Py_ssize_t i = 0
    cdef double cc = size * 0.5
    cdef double inv_sigma = 1.0 / 8.5
    cdef double clip_sq = clip_r * clip_r
    cdef double dx
    cdef double dy
    cdef double bx
    cdef double alpha

    with nogil:
        for y in range(size):
            dy = y - cc
            for x in range(size):
                dx = x - cc
                dst[i] = <unsigned char>114
                dst[i + 1] = <unsigned char>238
                dst[i + 2] = <unsigned char>255
                if dx * dx + dy * dy > clip_sq:
                    dst[i + 3] = <unsigned char>0
                else:
                    bx = (x - band_x) * inv_sigma
                    alpha = exp(-(bx * bx)) * 210.0 * alpha_mul
                    dst[i + 3] = _u8(alpha)
                i += 4
    return bytes(out)


cdef inline int _clip_int(int v, int lo, int hi) nogil:
    if v < lo:
        return lo
    if v > hi:
        return hi
    return v


cpdef bytes sweep_overlay_rgba_bytes(int width, int height,
                                     int p0x, int p1x, int p2x, int p3x,
                                     int alpha, int tint_r, int tint_g, int tint_b,
                                     double blur_radius):
    """D2/E2: sweep overlay generator for MenuLeftInfoRenderer._apply_sweep.

    Replaces:
        overlay = Image.new('RGBA', (w, h))
        ImageDraw.Draw(overlay).polygon(<4 corners>, fill=(tint, alpha))
        overlay.filter(ImageFilter.GaussianBlur(radius=blur_radius))

    Pipeline (all nogil):
      1. Raster the 4-vertex convex polygon with edges
         left:  p0 -> p3   right: p1 -> p2  (top y=0, bottom y=height-1)
         producing alpha = ``alpha`` inside, 0 outside.
      2. Approximate Gaussian via 3 passes of separable box blur with
         radius ceil(blur_radius * 0.83). 3-pass box is within ~5% of a
         true Gaussian for this use (matches PIL's GaussianBlur internal
         trick at small sigma).
      3. Emit RGBA bytes where R/G/B = tint at every pixel (alpha gates
         visibility on alpha_composite, so a uniform tint everywhere is
         equivalent for our straight-alpha composite path).
    """
    cdef bytearray out
    cdef unsigned char[:] dst
    cdef Py_ssize_t y, x, pos, i, j, idx
    cdef int box_r
    cdef int pass_i
    cdef int denom
    cdef int row_sum
    cdef int col_sum
    cdef double inv_h
    cdef double left_x, right_x
    cdef int lx, rx
    cdef int target_alpha
    cdef int tr, tg, tb
    if width <= 0 or height <= 0:
        return bytes(4)
    target_alpha = _clip_int(<int>alpha, 0, 255)
    tr = _clip_int(<int>tint_r, 0, 255)
    tg = _clip_int(<int>tint_g, 0, 255)
    tb = _clip_int(<int>tint_b, 0, 255)
    box_r = <int>(blur_radius * 0.83 + 0.5)
    if box_r < 0:
        box_r = 0
    out = bytearray(width * height * 4)
    dst = out
    cdef bytearray a0 = bytearray(width * height)
    cdef bytearray a1 = bytearray(width * height)
    cdef unsigned char[:] alpha0 = a0
    cdef unsigned char[:] alpha1 = a1
    if height > 1:
        inv_h = 1.0 / <double>(height - 1)
    else:
        inv_h = 0.0
    with nogil:
        for y in range(height):
            left_x = <double>p0x + (<double>(p3x - p0x)) * (<double>y * inv_h)
            right_x = <double>p1x + (<double>(p2x - p1x)) * (<double>y * inv_h)
            if right_x < left_x:
                lx = <int>right_x
                rx = <int>left_x
            else:
                lx = <int>left_x
                rx = <int>right_x
            if lx < 0:
                lx = 0
            if rx >= width:
                rx = width - 1
            for x in range(width):
                if x >= lx and x <= rx:
                    alpha0[y * width + x] = <unsigned char>target_alpha
                else:
                    alpha0[y * width + x] = <unsigned char>0
        for pass_i in range(3):
            if box_r <= 0:
                break
            denom = 2 * box_r + 1
            for y in range(height):
                row_sum = 0
                for j in range(-box_r, box_r + 1):
                    idx = j
                    if idx < 0:
                        idx = 0
                    elif idx >= width:
                        idx = width - 1
                    row_sum += <int>alpha0[y * width + idx]
                for x in range(width):
                    alpha1[y * width + x] = <unsigned char>(row_sum // denom)
                    idx = x - box_r
                    if idx < 0:
                        idx = 0
                    row_sum -= <int>alpha0[y * width + idx]
                    idx = x + box_r + 1
                    if idx >= width:
                        idx = width - 1
                    row_sum += <int>alpha0[y * width + idx]
            for x in range(width):
                col_sum = 0
                for i in range(-box_r, box_r + 1):
                    idx = i
                    if idx < 0:
                        idx = 0
                    elif idx >= height:
                        idx = height - 1
                    col_sum += <int>alpha1[idx * width + x]
                for y in range(height):
                    alpha0[y * width + x] = <unsigned char>(col_sum // denom)
                    idx = y - box_r
                    if idx < 0:
                        idx = 0
                    col_sum -= <int>alpha1[idx * width + x]
                    idx = y + box_r + 1
                    if idx >= height:
                        idx = height - 1
                    col_sum += <int>alpha1[idx * width + x]
        for y in range(height):
            for x in range(width):
                pos = (y * width + x) * 4
                dst[pos] = <unsigned char>tr
                dst[pos + 1] = <unsigned char>tg
                dst[pos + 2] = <unsigned char>tb
                dst[pos + 3] = alpha0[y * width + x]
    return bytes(out)
