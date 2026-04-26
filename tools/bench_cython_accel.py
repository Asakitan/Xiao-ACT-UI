# -*- coding: utf-8 -*-
"""Bench and parity-check the optional Cython pixel accelerators."""
from __future__ import annotations

import argparse
import os
import statistics
import sys
import time
from typing import Callable, Iterable, List, Tuple

import numpy as np

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_SAO_AUTO = os.path.dirname(_THIS_DIR)
if _SAO_AUTO not in sys.path:
    sys.path.insert(0, _SAO_AUTO)

try:
    import _sao_cy_pixels as cy_pixels
except Exception as exc:  # noqa: BLE001
    cy_pixels = None
    _IMPORT_ERROR = exc
else:
    _IMPORT_ERROR = None


def _ref_overlay_round(rgba: np.ndarray) -> bytes:
    a = rgba[:, :, 3:4].astype(np.uint16)
    rgb = (rgba[:, :, :3].astype(np.uint16) * a + 127) // 255
    bgra = np.empty_like(rgba)
    bgra[:, :, 0] = rgb[:, :, 2]
    bgra[:, :, 1] = rgb[:, :, 1]
    bgra[:, :, 2] = rgb[:, :, 0]
    bgra[:, :, 3] = rgba[:, :, 3]
    return bgra.tobytes()


def _ref_composer_floor(data: bytes, h: int, w: int,
                        master_alpha: float) -> bytes:
    arr = np.frombuffer(data, dtype=np.uint8).reshape((h, w, 4))
    r = arr[..., 0].astype(np.uint16)
    g = arr[..., 1].astype(np.uint16)
    b = arr[..., 2].astype(np.uint16)
    a = arr[..., 3].astype(np.uint16)
    if master_alpha < 0.999:
        a = (a * max(0, min(255, int(master_alpha * 255)))) // 255
    out = np.empty_like(arr)
    out[..., 0] = (b * a // 255).astype(np.uint8)
    out[..., 1] = (g * a // 255).astype(np.uint8)
    out[..., 2] = (r * a // 255).astype(np.uint8)
    out[..., 3] = a.astype(np.uint8)
    return out.tobytes()


def _ref_alpha_floor(rgba: np.ndarray, alpha: float) -> bytes:
    arr = rgba.copy()
    mul = int(max(0, min(255, alpha * 255)))
    arr[:, :, 3] = (
        arr[:, :, 3].astype(np.uint16) * mul // 255
    ).astype(np.uint8)
    return arr.tobytes()


def _time_call(fn: Callable[[], object], loops: int) -> List[float]:
    samples: List[float] = []
    for _ in range(loops):
        t0 = time.perf_counter()
        fn()
        samples.append(time.perf_counter() - t0)
    return samples


def _stats(samples: List[float]) -> Tuple[float, float, float]:
    ordered = sorted(samples)
    avg = statistics.fmean(ordered) * 1000.0
    p50 = ordered[int(round((len(ordered) - 1) * 0.50))] * 1000.0
    p95 = ordered[int(round((len(ordered) - 1) * 0.95))] * 1000.0
    return avg, p50, p95


def _print_result(label: str, py_samples: List[float],
                  cy_samples: List[float]) -> None:
    py_avg, py_p50, py_p95 = _stats(py_samples)
    cy_avg, cy_p50, cy_p95 = _stats(cy_samples)
    speed = py_avg / cy_avg if cy_avg > 0 else 0.0
    print(
        f'{label:38s} '
        f'py avg={py_avg:7.3f} p50={py_p50:7.3f} p95={py_p95:7.3f} ms  '
        f'cy avg={cy_avg:7.3f} p50={cy_p50:7.3f} p95={cy_p95:7.3f} ms  '
        f'{speed:5.2f}x'
    )


def _parity_or_raise(name: str, got: bytes, expected: bytes) -> None:
    if got != expected:
        for idx, (a, b) in enumerate(zip(got, expected)):
            if a != b:
                raise AssertionError(
                    f'{name} mismatch at byte {idx}: got={a}, expected={b}'
                )
        raise AssertionError(
            f'{name} length mismatch: got={len(got)}, expected={len(expected)}'
        )


def _sizes() -> Iterable[Tuple[int, int, int]]:
    return (
        (60, 220, 2000),
        (128, 620, 700),
        (420, 340, 500),
        (1080, 1920, 40),
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--assert-parity', action='store_true')
    args = parser.parse_args()

    if cy_pixels is None:
        print(f'! _sao_cy_pixels import failed: {_IMPORT_ERROR!r}')
        return 2

    rng = np.random.default_rng(0x53414F)
    print('Cython pixel accelerator benchmark')

    for h, w, loops in _sizes():
        rgba = rng.integers(0, 256, size=(h, w, 4), dtype=np.uint8)
        data = rgba.tobytes()

        expected = _ref_overlay_round(rgba)
        got = cy_pixels.premultiply_bgra_ndarray(rgba)
        if args.assert_parity:
            _parity_or_raise(f'overlay round {w}x{h}', got, expected)
        _print_result(
            f'overlay round {w}x{h}',
            _time_call(lambda: _ref_overlay_round(rgba), loops),
            _time_call(lambda: cy_pixels.premultiply_bgra_ndarray(rgba), loops),
        )

        for alpha in (1.0, 0.73):
            expected = _ref_composer_floor(data, h, w, alpha)
            got = cy_pixels.premultiply_bgra_bytes_floor(data, h, w, alpha)
            if args.assert_parity:
                _parity_or_raise(f'composer floor {w}x{h} a={alpha}', got, expected)
        _print_result(
            f'composer floor {w}x{h}',
            _time_call(lambda: _ref_composer_floor(data, h, w, 0.73), loops),
            _time_call(
                lambda: cy_pixels.premultiply_bgra_bytes_floor(data, h, w, 0.73),
                loops,
            ),
        )

        expected = _ref_alpha_floor(rgba, 0.42)
        got = cy_pixels.multiply_alpha_rgba_ndarray_floor(rgba, 0.42)
        if args.assert_parity:
            _parity_or_raise(f'alpha floor {w}x{h}', got, expected)
        _print_result(
            f'alpha floor {w}x{h}',
            _time_call(lambda: _ref_alpha_floor(rgba, 0.42), loops),
            _time_call(
                lambda: cy_pixels.multiply_alpha_rgba_ndarray_floor(rgba, 0.42),
                loops,
            ),
        )

    if args.assert_parity:
        print('Parity OK')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
