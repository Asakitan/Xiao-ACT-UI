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
