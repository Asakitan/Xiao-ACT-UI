# -*- coding: utf-8 -*-
"""
skillfx_jit.py — Numba-JIT hot kernels for sao_gui_skillfx.

Phase 4 of the v2.3.0 overlay perf pass. Provides:

    fast_beam_rgba(L, H) -> np.ndarray  (H, L, 4) uint8

Pixel-identical to the original numpy `_fast_beam` body in
`sao_gui_skillfx.py` (verified at L=120/320/600/900, max diff = 0).
Numba runs 2-14x faster on long beams because it fuses the per-pixel
work (gradient, falloff, core overlay, clip, cast) into a single
parallel pass, instead of numpy's many full-array temporaries.

Falls back transparently to the numpy path when numba is missing
or the @njit compile fails, so this module is safe to import on any
environment.
"""

from __future__ import annotations

import os
import numpy as np

_JIT_DISABLED = os.environ.get("SAO_DISABLE_JIT", "").strip() in ("1", "true", "True")

_beam_kernel = None
_ring_kernel = None
_sweep_kernel = None
if not _JIT_DISABLED:
    try:
        from numba import njit, prange  # type: ignore[import-untyped]

        @njit(parallel=True, fastmath=False, boundscheck=False, cache=True)
        def _ring_kernel_impl(box, r_out, pulse_q, r_core, out):  # pragma: no cover - JIT path
            cc = box * 0.5
            halo_breath = 0.52 + 0.40 * pulse_q
            halo_amp = 46.0 * halo_breath
            inv_sigma_sq = 1.0 / (28.0 * 28.0)
            r_core_safe = r_core if r_core > 1.0 else 1.0
            inv_r_core = 1.0 / r_core_safe
            core_amp = 70.0 * (0.6 + 0.4 * pulse_q)
            r_core_p2 = r_core + 2.0
            for y in prange(box):
                dy = y - cc
                for x in range(box):
                    dx = x - cc
                    dist = (dx * dx + dy * dy) ** 0.5
                    drm = dist - r_out
                    halo_a = halo_amp * np.exp(-(drm * drm) * inv_sigma_sq)
                    if halo_a < 0.0:
                        halo_a = 0.0
                    elif halo_a > 255.0:
                        halo_a = 255.0
                    is_core = dist < r_core_p2
                    if is_core:
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
                    B = 255.0 * inv + 255.0 * src_a
                    A = halo_a if halo_a > core_a else core_a
                    if A > 255.0:
                        A = 255.0
                    out[y, x, 0] = np.uint8(R)
                    out[y, x, 1] = np.uint8(G)
                    out[y, x, 2] = np.uint8(B)
                    out[y, x, 3] = np.uint8(A)

        _ring_kernel = _ring_kernel_impl

        @njit(parallel=True, fastmath=False, boundscheck=False, cache=True)
        def _sweep_kernel_impl(box, band_x, clip_r, alpha_mul, out):  # pragma: no cover - JIT path
            cc = box * 0.5
            inv_sigma = 1.0 / 8.5
            for y in prange(box):
                dy = y - cc
                for x in range(box):
                    dx = x - cc
                    dist_sq = dx * dx + dy * dy
                    if dist_sq > clip_r * clip_r:
                        out[y, x, 0] = np.uint8(114)
                        out[y, x, 1] = np.uint8(238)
                        out[y, x, 2] = np.uint8(255)
                        out[y, x, 3] = np.uint8(0)
                        continue
                    bx = (x - band_x) * inv_sigma
                    a = np.exp(-(bx * bx)) * 210.0 * alpha_mul
                    if a < 0.0:
                        a = 0.0
                    elif a > 255.0:
                        a = 255.0
                    out[y, x, 0] = np.uint8(114)
                    out[y, x, 1] = np.uint8(238)
                    out[y, x, 2] = np.uint8(255)
                    out[y, x, 3] = np.uint8(a)

        _sweep_kernel = _sweep_kernel_impl

        @njit(parallel=True, fastmath=False, boundscheck=False, cache=True)
        def _beam_kernel_impl(L, H, out):  # pragma: no cover - JIT path
            cy = H * 0.5
            inv_cy = 1.0 / cy
            inv_L = 1.0 / max(1, L - 1)
            for y in prange(H):
                ny = (y - cy) * inv_cy
                falloff = np.exp(-(ny * ny) * 4.0)
                is_core = abs(y - cy) <= 1.5
                for x in range(L):
                    xs = np.float32(x * inv_L)
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
                    if is_core and 245.0 > A_glow:
                        finalA = 245.0
                    else:
                        finalA = A_glow
                    if finalA < 0.0:
                        finalA = 0.0
                    elif finalA > 255.0:
                        finalA = 255.0
                    if R < 0.0:
                        R = 0.0
                    elif R > 255.0:
                        R = 255.0
                    if G < 0.0:
                        G = 0.0
                    elif G > 255.0:
                        G = 255.0
                    if B < 0.0:
                        B = 0.0
                    elif B > 255.0:
                        B = 255.0
                    out[y, x, 0] = np.uint8(R)
                    out[y, x, 1] = np.uint8(G)
                    out[y, x, 2] = np.uint8(B)
                    out[y, x, 3] = np.uint8(finalA)

        _beam_kernel = _beam_kernel_impl
    except Exception:
        _beam_kernel = None


def _beam_numpy(L: int, H: int) -> np.ndarray:
    """Fallback numpy implementation (matches sao_gui_skillfx._fast_beam)."""
    xs = np.arange(L, dtype=np.float32) / max(1, L - 1)
    ys = (np.arange(H, dtype=np.float32) - H / 2.0) / (H / 2.0)
    falloff = np.exp(-(ys * ys) * 4.0)[:, None]
    t1 = np.clip((xs - 0.0) / 0.22, 0.0, 1.0)
    t2 = np.clip((xs - 0.35) / 0.65, 0.0, 1.0)
    R = 97 + (255 - 97) * t2
    G = 232 + (188 - 232) * t2
    B = 255 + (66 - 255) * t2
    A_base = 150 * (0.15 + 0.85 * t1)
    A_glow = A_base[None, :] * falloff
    core_mask = np.abs(np.arange(H) - H / 2.0) <= 1.5
    core_a = np.where(core_mask[:, None], 245.0, 0.0)
    core_R = np.where(core_mask[:, None], 97 + (255 - 97) * t2[None, :], 0)
    core_G = np.where(core_mask[:, None], 232 + (188 - 232) * t2[None, :], 0)
    core_B = np.where(core_mask[:, None], 255 + (66 - 255) * t2[None, :], 0)
    alpha = np.maximum(A_glow, core_a)
    final_R = np.where(core_mask[:, None], core_R, np.broadcast_to(R[None, :], (H, L)))
    final_G = np.where(core_mask[:, None], core_G, np.broadcast_to(G[None, :], (H, L)))
    final_B = np.where(core_mask[:, None], core_B, np.broadcast_to(B[None, :], (H, L)))
    return np.stack([
        np.clip(final_R, 0, 255),
        np.clip(final_G, 0, 255),
        np.clip(final_B, 0, 255),
        np.clip(alpha, 0, 255),
    ], axis=-1).astype(np.uint8)


def fast_beam_rgba(L: int, H: int) -> np.ndarray:
    """Generate the gradient beam RGBA buffer.

    Uses numba JIT when available, otherwise falls back to numpy.
    Returns an (H, L, 4) uint8 array suitable for `Image.fromarray(arr,
    'RGBA')`.
    """
    L = int(L)
    H = int(H)
    if L < 1:
        L = 1
    if H < 1:
        H = 1
    if _beam_kernel is not None:
        try:
            out = np.empty((H, L, 4), dtype=np.uint8)
            _beam_kernel(L, H, out)
            return out
        except Exception:
            pass
    return _beam_numpy(L, H)


def jit_available() -> bool:
    return _beam_kernel is not None


def _ring_numpy(box: int, r_out: float, pulse_q: float,
                r_core: float) -> np.ndarray:
    """Fallback numpy implementation matching `_get_ring_layer` halo+core."""
    cc = box * 0.5
    yy, xx = np.mgrid[0:box, 0:box].astype(np.float32)
    dx = xx - cc
    dy = yy - cc
    dist = np.sqrt(dx * dx + dy * dy)
    halo_breath = 0.52 + 0.40 * pulse_q
    halo_a = 46.0 * halo_breath * np.exp(
        -((dist - r_out) ** 2) / (28.0 * 28.0))
    halo_a = np.clip(halo_a, 0, 255)
    arr = np.zeros((box, box, 4), dtype=np.float32)
    arr[:, :, 0] = 97
    arr[:, :, 1] = 232
    arr[:, :, 2] = 255
    arr[:, :, 3] = halo_a
    core_a = 70.0 * (0.6 + 0.4 * pulse_q) * np.clip(
        1.0 - dist / max(1.0, r_core), 0.0, 1.0)
    core_mask = dist < r_core + 2
    src_a = (core_a * core_mask) / 255.0
    inv = 1.0 - src_a
    arr[:, :, 0] = arr[:, :, 0] * inv + 176 * src_a
    arr[:, :, 1] = arr[:, :, 1] * inv + 247 * src_a
    arr[:, :, 2] = arr[:, :, 2] * inv + 255 * src_a
    arr[:, :, 3] = np.maximum(arr[:, :, 3], core_a * core_mask)
    return np.clip(arr, 0, 255).astype(np.uint8)


def fast_ring_layer_rgba(box: int, r_out: float, pulse_q: float,
                         r_core: float) -> np.ndarray:
    """Build the ring halo + core RGBA buffer.

    JIT path runs in parallel across rows; the original numpy version
    does ~10 full-array temporaries on a (box, box) grid which dominates
    `_get_ring_layer` rebuilds (each box is typically 200-360 px square,
    so ~40-130k pixels × 4 channels). Pixel-identical to the original.
    """
    box = int(box)
    if box < 1:
        box = 1
    r_out = float(r_out)
    pulse_q = float(pulse_q)
    r_core = float(r_core)
    if _ring_kernel is not None:
        try:
            out = np.empty((box, box, 4), dtype=np.uint8)
            _ring_kernel(box, r_out, pulse_q, r_core, out)
            return out
        except Exception:
            pass
    return _ring_numpy(box, r_out, pulse_q, r_core)


def _sweep_numpy(box: int, band_x: float, clip_r: float,
                 alpha_mul: float) -> np.ndarray:
    cc = box * 0.5
    yy, xx = np.mgrid[0:box, 0:box].astype(np.float32)
    dx = xx - cc
    dy = yy - cc
    dist = np.sqrt(dx * dx + dy * dy)
    xs = np.arange(box, dtype=np.float32)[None, :]
    band = np.exp(-((xs - band_x) / 8.5) ** 2) * 210.0
    band = np.broadcast_to(band, (box, box))
    mask = (dist <= clip_r).astype(np.float32)
    arr = np.zeros((box, box, 4), dtype=np.uint8)
    arr[:, :, 0] = 114
    arr[:, :, 1] = 238
    arr[:, :, 2] = 255
    arr[:, :, 3] = np.clip(band * mask * alpha_mul, 0, 255).astype(np.uint8)
    return arr


def fast_ring_sweep_rgba(box: int, band_x: float, clip_r: float,
                         alpha_mul: float) -> np.ndarray:
    """Per-frame sweep band over the ring (BGR fixed: 114,238,255).

    Replaces the per-frame numpy mgrid + exp + broadcast in
    `_draw_ring`'s sweep branch (fires every frame during 0.10..1.35 s
    after show on each motion sample). Pixel-identical fallback.
    """
    box = int(box)
    if box < 1:
        box = 1
    if _sweep_kernel is not None:
        try:
            out = np.empty((box, box, 4), dtype=np.uint8)
            _sweep_kernel(box, float(band_x), float(clip_r),
                          float(alpha_mul), out)
            return out
        except Exception:
            pass
    return _sweep_numpy(box, float(band_x), float(clip_r), float(alpha_mul))


def warmup() -> None:
    """Pay the JIT compile cost up-front (eg. during app boot)."""
    if _beam_kernel is not None:
        try:
            out = np.empty((30, 64, 4), dtype=np.uint8)
            _beam_kernel(64, 30, out)
        except Exception:
            pass
    if _ring_kernel is not None:
        try:
            out = np.empty((64, 64, 4), dtype=np.uint8)
            _ring_kernel(64, 24.0, 0.5, 12.0, out)
        except Exception:
            pass
    if _sweep_kernel is not None:
        try:
            out = np.empty((64, 64, 4), dtype=np.uint8)
            _sweep_kernel(64, 32.0, 28.0, 1.0, out)
        except Exception:
            pass
