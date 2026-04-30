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
