# cython: language_level=3
# cython: boundscheck=False
# cython: wraparound=False
# cython: initializedcheck=False
# cython: nonecheck=False
"""Small Cython pixel kernels for SAO GUI hot paths.

The module intentionally keeps a tiny surface area. Large GUI modules stay
as Python because their dynamic Tk/PIL code does not benefit from whole-file
Cython compilation and is easier to keep correct in Python.
"""


cpdef bytes premultiply_bgra_ndarray(object rgba):
    """RGBA ndarray-like object -> premultiplied BGRA bytes.

    Matches overlay_render_worker's historical rounding:
    channel = (channel * alpha + 127) // 255
    """
    cdef const unsigned char[:, :, :] arr = rgba
    cdef Py_ssize_t h = arr.shape[0]
    cdef Py_ssize_t w = arr.shape[1]
    cdef Py_ssize_t y
    cdef Py_ssize_t x
    cdef Py_ssize_t i = 0
    cdef unsigned int a
    cdef bytearray out
    cdef unsigned char[:] dst

    if arr.shape[2] < 4:
        raise ValueError('expected RGBA input with at least 4 channels')

    out = bytearray(h * w * 4)
    dst = out
    with nogil:
        for y in range(h):
            for x in range(w):
                a = arr[y, x, 3]
                dst[i] = <unsigned char>((<unsigned int>arr[y, x, 2] * a + 127) // 255)
                dst[i + 1] = <unsigned char>((<unsigned int>arr[y, x, 1] * a + 127) // 255)
                dst[i + 2] = <unsigned char>((<unsigned int>arr[y, x, 0] * a + 127) // 255)
                dst[i + 3] = <unsigned char>a
                i += 4
    return bytes(out)


cpdef bytes premultiply_bgra_bytes_floor(
        object data, Py_ssize_t height, Py_ssize_t width,
        double master_alpha=1.0):
    """RGBA bytes -> premultiplied BGRA bytes.

    Matches ui_gpu.composer's floor semantics:
    - master alpha uses int(master_alpha * 255) only when master_alpha < 0.999
    - premultiply uses channel * alpha // 255
    """
    cdef const unsigned char[:] src = data
    cdef Py_ssize_t expected
    cdef Py_ssize_t y
    cdef Py_ssize_t x
    cdef Py_ssize_t si = 0
    cdef Py_ssize_t di = 0
    cdef unsigned int a
    cdef unsigned int mul = 255
    cdef bint apply_master = False
    cdef bytearray out
    cdef unsigned char[:] dst

    if height < 0 or width < 0:
        raise ValueError('height and width must be non-negative')
    expected = height * width * 4
    if src.shape[0] != expected:
        raise ValueError('RGBA byte length does not match height * width * 4')

    if master_alpha < 0.999:
        apply_master = True
        if master_alpha <= 0.0:
            mul = 0
        elif master_alpha >= 1.0:
            mul = 255
        else:
            mul = <unsigned int>(master_alpha * 255.0)

    out = bytearray(expected)
    dst = out
    with nogil:
        for y in range(height):
            for x in range(width):
                a = src[si + 3]
                if apply_master:
                    a = (a * mul) // 255
                dst[di] = <unsigned char>((<unsigned int>src[si + 2] * a) // 255)
                dst[di + 1] = <unsigned char>((<unsigned int>src[si + 1] * a) // 255)
                dst[di + 2] = <unsigned char>((<unsigned int>src[si] * a) // 255)
                dst[di + 3] = <unsigned char>a
                si += 4
                di += 4
    return bytes(out)


cpdef bytes multiply_alpha_rgba_ndarray_floor(object rgba, double alpha):
    """RGBA ndarray-like object -> RGBA bytes with alpha multiplied.

    Matches the HP/BossHP/DPS final fade:
    alpha_channel = alpha_channel * int(alpha * 255) // 255
    """
    cdef const unsigned char[:, :, :] arr = rgba
    cdef Py_ssize_t h = arr.shape[0]
    cdef Py_ssize_t w = arr.shape[1]
    cdef Py_ssize_t y
    cdef Py_ssize_t x
    cdef Py_ssize_t i = 0
    cdef unsigned int mul = 255
    cdef bytearray out
    cdef unsigned char[:] dst

    if arr.shape[2] < 4:
        raise ValueError('expected RGBA input with at least 4 channels')

    if alpha < 0.999:
        if alpha <= 0.0:
            mul = 0
        elif alpha >= 1.0:
            mul = 255
        else:
            mul = <unsigned int>(alpha * 255.0)

    out = bytearray(h * w * 4)
    dst = out
    with nogil:
        for y in range(h):
            for x in range(w):
                dst[i] = arr[y, x, 0]
                dst[i + 1] = arr[y, x, 1]
                dst[i + 2] = arr[y, x, 2]
                dst[i + 3] = <unsigned char>((<unsigned int>arr[y, x, 3] * mul) // 255)
                i += 4
    return bytes(out)


cpdef bytes multiply_alpha_mask_rgba_ndarray_floor(object rgba, object mask):
    """RGBA ndarray-like object -> RGBA bytes clipped by an L mask.

    Matches HP/BossHP _clip_alpha:
    alpha_channel = alpha_channel * mask // 255
    """
    cdef const unsigned char[:, :, :] arr = rgba
    cdef const unsigned char[:, :] m = mask
    cdef Py_ssize_t h = arr.shape[0]
    cdef Py_ssize_t w = arr.shape[1]
    cdef Py_ssize_t y
    cdef Py_ssize_t x
    cdef Py_ssize_t i = 0
    cdef unsigned int ma
    cdef bytearray out
    cdef unsigned char[:] dst

    if arr.shape[2] < 4:
        raise ValueError('expected RGBA input with at least 4 channels')
    if m.shape[0] != h or m.shape[1] != w:
        raise ValueError('mask shape must match RGBA height and width')

    out = bytearray(h * w * 4)
    dst = out
    with nogil:
        for y in range(h):
            for x in range(w):
                ma = m[y, x]
                dst[i] = arr[y, x, 0]
                dst[i + 1] = arr[y, x, 1]
                dst[i + 2] = arr[y, x, 2]
                dst[i + 3] = <unsigned char>((<unsigned int>arr[y, x, 3] * ma) // 255)
                i += 4
    return bytes(out)


cpdef bytes multiply_alpha_regions_rgba_bytes(object rgba, object rects, double alpha):
    """RGBA ndarray-like object -> RGBA bytes with alpha multiplied in rects.

    Mirrors ``sao_gui_hp._multiply_alpha_regions`` without numpy slice
    allocation/casts. RGB is copied unchanged; only alpha inside the
    clamped rectangles is multiplied by ``int(alpha * 255) // 255``.
    """
    cdef const unsigned char[:, :, :] arr = rgba
    cdef Py_ssize_t h = arr.shape[0]
    cdef Py_ssize_t w = arr.shape[1]
    cdef Py_ssize_t y, x, i = 0
    cdef unsigned int mul = 255
    cdef bytearray out
    cdef unsigned char[:] dst
    cdef object rect
    cdef Py_ssize_t rx0, ry0, rx1, ry1
    cdef Py_ssize_t yy, xx, pos

    if arr.shape[2] < 4:
        raise ValueError('expected RGBA input with at least 4 channels')

    if alpha < 0.999:
        if alpha <= 0.0:
            mul = 0
        elif alpha >= 1.0:
            mul = 255
        else:
            mul = <unsigned int>(alpha * 255.0)

    out = bytearray(h * w * 4)
    dst = out
    with nogil:
        for y in range(h):
            for x in range(w):
                dst[i] = arr[y, x, 0]
                dst[i + 1] = arr[y, x, 1]
                dst[i + 2] = arr[y, x, 2]
                dst[i + 3] = arr[y, x, 3]
                i += 4

    for rect in (rects or ()):
        try:
            rx0 = <long>int(rect[0])
            ry0 = <long>int(rect[1])
            rx1 = <long>int(rect[2])
            ry1 = <long>int(rect[3])
        except Exception:
            continue
        if rx0 < 0:
            rx0 = 0
        elif rx0 > w:
            rx0 = w
        if ry0 < 0:
            ry0 = 0
        elif ry0 > h:
            ry0 = h
        if rx1 < rx0:
            rx1 = rx0
        elif rx1 > w:
            rx1 = w
        if ry1 < ry0:
            ry1 = ry0
        elif ry1 > h:
            ry1 = h
        if rx1 <= rx0 or ry1 <= ry0:
            continue
        with nogil:
            for yy in range(ry0, ry1):
                pos = (yy * w + rx0) * 4 + 3
                for xx in range(rx0, rx1):
                    dst[pos] = <unsigned char>((<unsigned int>dst[pos] * mul) // 255)
                    pos += 4
    return bytes(out)


cpdef bytes scanline_texture_rgba_bytes(Py_ssize_t width, Py_ssize_t height,
                                        unsigned int alpha=10):
    """Return RGBA bytes for the HP scanline texture."""
    cdef Py_ssize_t expected = width * height * 4
    cdef bytearray out
    cdef unsigned char[:] dst
    cdef Py_ssize_t y, x, pos
    cdef unsigned char a = <unsigned char>(255 if alpha > 255 else alpha)
    if width <= 0 or height <= 0:
        return bytes(4)
    out = bytearray(expected)
    dst = out
    with nogil:
        for y in range(height):
            if y % 3 != 2:
                continue
            pos = y * width * 4
            for x in range(width):
                dst[pos] = 255
                dst[pos + 1] = 255
                dst[pos + 2] = 255
                dst[pos + 3] = a
                pos += 4
    return bytes(out)


cpdef bytes hgrad_bar_rgba_bytes(Py_ssize_t width, Py_ssize_t height,
                                 object ca, object cb):
    """Return RGBA bytes for HP/STA horizontal gradient bars."""
    cdef bytearray out
    cdef unsigned char[:] dst
    cdef Py_ssize_t y, x, pos = 0
    cdef double tx, shade
    cdef int ar, ag, ab, aa, br, bg, bb, ba
    cdef int rr, gg, bl, al
    if width <= 0 or height <= 0:
        return bytes(4)
    ar = <int>int(ca[0]); ag = <int>int(ca[1]); ab = <int>int(ca[2]); aa = <int>int(ca[3])
    br = <int>int(cb[0]); bg = <int>int(cb[1]); bb = <int>int(cb[2]); ba = <int>int(cb[3])
    out = bytearray(width * height * 4)
    dst = out
    with nogil:
        for y in range(height):
            if height > 1:
                shade = 1.02 + (0.88 - 1.02) * (<double>y / <double>(height - 1))
            else:
                shade = 1.02
            for x in range(width):
                tx = (<double>x / <double>(width - 1)) if width > 1 else 0.0
                rr = <int>((<double>ar + (<double>(br - ar)) * tx) * shade)
                gg = <int>((<double>ag + (<double>(bg - ag)) * tx) * shade)
                bl = <int>((<double>ab + (<double>(bb - ab)) * tx) * shade)
                al = <int>((<double>aa + (<double>(ba - aa)) * tx))
                if rr < 0: rr = 0
                elif rr > 255: rr = 255
                if gg < 0: gg = 0
                elif gg > 255: gg = 255
                if bl < 0: bl = 0
                elif bl > 255: bl = 255
                if al < 0: al = 0
                elif al > 255: al = 255
                dst[pos] = <unsigned char>rr
                dst[pos + 1] = <unsigned char>gg
                dst[pos + 2] = <unsigned char>bl
                dst[pos + 3] = <unsigned char>al
                pos += 4
    return bytes(out)
