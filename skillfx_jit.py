# -*- coding: utf-8 -*-
"""Cython-backed SkillFX hot kernels.

The module keeps the historical ``skillfx_jit`` import name so callers do not
need to know whether the implementation is JIT or ahead-of-time compiled.
Runtime fallback is intentionally not provided: ``_sao_cy_skillfx`` is required.
"""

from __future__ import annotations

import numpy as np

import _sao_cy_skillfx as _CY_SKILLFX  # type: ignore[import-not-found]


def _rgba_array(data: bytes, height: int, width: int) -> np.ndarray:
    return np.frombuffer(data, dtype=np.uint8).reshape((height, width, 4))


def fast_beam_rgba(length: int, height: int) -> np.ndarray:
    """Generate the gradient beam RGBA buffer via mandatory Cython."""
    length = max(1, int(length))
    height = max(1, int(height))
    return _rgba_array(_CY_SKILLFX.beam_rgba(length, height), height, length)


def fast_ring_layer_rgba(box: int, r_out: float, pulse_q: float,
                         r_core: float) -> np.ndarray:
    """Build the ring halo + core RGBA buffer via mandatory Cython."""
    box = max(1, int(box))
    return _rgba_array(
        _CY_SKILLFX.ring_layer_rgba(
            box, float(r_out), float(pulse_q), float(r_core)),
        box,
        box,
    )


def fast_ring_sweep_rgba(box: int, band_x: float, clip_r: float,
                         alpha_mul: float) -> np.ndarray:
    """Build the per-frame ring sweep RGBA buffer via mandatory Cython."""
    box = max(1, int(box))
    return _rgba_array(
        _CY_SKILLFX.ring_sweep_rgba(
            box, float(band_x), float(clip_r), float(alpha_mul)),
        box,
        box,
    )


def jit_available() -> bool:
    """Compatibility shim for older diagnostics."""
    return True


def warmup() -> None:
    """Exercise the Cython kernels once during app boot."""
    fast_beam_rgba(64, 30)
    fast_ring_layer_rgba(64, 24.0, 0.5, 12.0)
    fast_ring_sweep_rgba(64, 32.0, 28.0, 1.0)
