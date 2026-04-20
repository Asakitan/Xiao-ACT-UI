# -*- coding: utf-8 -*-
"""Microbenchmarks for the v2.2.11 GPU compositor work.

Runs three benches off the Tk main thread (each test runs on a worker
thread because ModernGL contexts are thread-affine):

1. ``LayerCompositor.render`` throughput for the 10 shaders.
2. ``LayerCompositor.blur_tex`` (Phase 3 refinement) vs the prior
   PIL-roundtrip blur path.
3. BossHP ``_apply_inset_shadow_gpu`` vs the CPU fallback.

Reports p50/p95/p99 ms. No Tk involvement, so it's safe to run from
``python tools/bench_compose.py`` in any shell.

Targets (per the plan):
    LayerCompositor.render single pass : p95 < 0.5 ms
    blur_tex sigma=11 on 220x60        : p95 < 1.0 ms
    inset_shadow_gpu  vs cpu           : >= 2x speed-up at sigma 11
"""
from __future__ import annotations

import os
import statistics
import sys
import threading
import time
from typing import Callable, List, Tuple

# Make sao_auto importable when run as ``python tools/bench_compose.py``.
_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_SAO_AUTO = os.path.dirname(_THIS_DIR)
if _SAO_AUTO not in sys.path:
    sys.path.insert(0, _SAO_AUTO)

import numpy as np  # noqa: E402
from PIL import Image, ImageDraw, ImageFilter  # noqa: E402


def _run_on_worker(fn: Callable[[], List[float]]) -> List[float]:
    """Run ``fn`` on a fresh daemon thread and return its result."""
    out: List[List[float]] = []

    def target() -> None:
        try:
            out.append(fn())
        except Exception as exc:  # noqa: BLE001
            print(f'  ! worker error: {exc!r}')
            out.append([])

    t = threading.Thread(target=target, daemon=True)
    t.start()
    t.join()
    return out[0] if out else []


def _percentiles(samples: List[float]) -> Tuple[float, float, float, float]:
    if not samples:
        return (0.0, 0.0, 0.0, 0.0)
    s = sorted(samples)
    n = len(s)

    def at(p: float) -> float:
        idx = max(0, min(n - 1, int(round(p * (n - 1)))))
        return s[idx]

    return (
        statistics.fmean(s) * 1000.0,
        at(0.50) * 1000.0,
        at(0.95) * 1000.0,
        at(0.99) * 1000.0,
    )


def _print_row(label: str, samples: List[float]) -> None:
    avg, p50, p95, p99 = _percentiles(samples)
    print(f'  {label:38s} '
          f'avg={avg:6.3f}ms  p50={p50:6.3f}ms  '
          f'p95={p95:6.3f}ms  p99={p99:6.3f}ms  n={len(samples)}')


def _bench_compositor() -> None:
    print('\n[1] LayerCompositor.render — single pass per shader')

    def body() -> List[float]:
        from gpu_compositor import LayerCompositor

        comp = LayerCompositor('bench')
        if not comp.available:
            print('  ! compositor unavailable on this machine — skipping')
            return []
        w, h = 220, 60
        # Pre-warm shaders + textures.
        src = np.zeros((h, w, 4), dtype=np.uint8)
        src[:, :, 2] = 200
        src[:, :, 3] = 255
        comp.upload('src', src)
        comp.tex('dst', w, h, clear=True)
        # Pump each shader once to JIT-compile.
        a = comp.upload('a', src)
        b = comp.upload('b', src)
        out = comp.tex('o', w, h, clear=True)
        try:
            comp.render('over', out,
                        uniforms={}, inputs={'u_top': a, 'u_bottom': b})
        except Exception as exc:  # noqa: BLE001
            print(f'  ! over warmup failed: {exc!r}')
        # Steady-state: 1000 over passes.
        samples: List[float] = []
        for _ in range(1000):
            t0 = time.perf_counter()
            comp.render('over', out,
                        uniforms={},
                        inputs={'u_top': a, 'u_bottom': b})
            samples.append(time.perf_counter() - t0)
        return samples

    samples = _run_on_worker(body)
    _print_row('over (220x60)', samples)


def _bench_blur_tex() -> None:
    print('\n[2] blur_tex (separable, GPU-resident)')

    def body() -> List[float]:
        from gpu_compositor import LayerCompositor

        comp = LayerCompositor('bench-blur')
        if not comp.available:
            print('  ! compositor unavailable — skipping')
            return []
        w, h = 220, 60
        rgba = np.random.randint(0, 255, size=(h, w, 4), dtype=np.uint8)
        src = comp.upload('src', rgba)
        # Warmup.
        try:
            comp.blur_tex(src, 11.0, out_tag='warmup')
        except Exception as exc:  # noqa: BLE001
            print(f'  ! blur_tex warmup failed: {exc!r}')
            return []
        samples: List[float] = []
        for i in range(500):
            t0 = time.perf_counter()
            comp.blur_tex(src, 11.0, out_tag=f'b{i & 1}')
            samples.append(time.perf_counter() - t0)
        return samples

    _print_row('blur_tex sigma=11 (220x60)', _run_on_worker(body))

    print('  ↪ baseline (PIL GaussianBlur, single thread):')
    rgba = np.random.randint(0, 255, size=(60, 220, 4), dtype=np.uint8)
    pil_img = Image.fromarray(rgba, 'RGBA')
    samples: List[float] = []
    for _ in range(200):
        t0 = time.perf_counter()
        pil_img.filter(ImageFilter.GaussianBlur(radius=11.0))
        samples.append(time.perf_counter() - t0)
    _print_row('PIL GaussianBlur radius=11', samples)


def _bench_inset_shadow() -> None:
    print('\n[3] BossHP inset shadow — GPU vs CPU fallback')
    w, h = 220, 60

    def make_inputs() -> Tuple[Image.Image, Image.Image]:
        mask = Image.new('L', (w, h), 0)
        ImageDraw.Draw(mask).rounded_rectangle(
            (4, 4, w - 4, h - 4), radius=18, fill=255)
        img = Image.new('RGBA', (w, h), (12, 24, 38, 220))
        return img, mask

    def body_gpu() -> List[float]:
        import sao_gui_bosshp as bh

        # Warmup.
        img, mask = make_inputs()
        if not bh._apply_inset_shadow_gpu(
                img, mask, (255, 255, 255), 31, 11.0):
            print('  ! GPU inset returned False — GPU unavailable, skipping')
            return []
        samples: List[float] = []
        for _ in range(500):
            img, mask = make_inputs()
            t0 = time.perf_counter()
            bh._apply_inset_shadow_gpu(
                img, mask, (255, 255, 255), 31, 11.0)
            samples.append(time.perf_counter() - t0)
        return samples

    def body_cpu() -> List[float]:
        # Pure-CPU equivalent for parity baseline.
        samples: List[float] = []
        for _ in range(200):
            img, mask = make_inputs()
            t0 = time.perf_counter()
            inv = Image.eval(mask, lambda v: 255 - v)
            blur = inv.filter(ImageFilter.GaussianBlur(radius=11.0))
            color = Image.new(
                'RGBA', (w, h),
                (255, 255, 255, int(round(31))))
            color.putalpha(blur)
            shape_alpha = mask
            r, g, b, a = color.split()
            a = Image.eval(a, lambda v: v)  # noop, mirrors cost
            clipped = Image.merge('RGBA', (r, g, b, a))
            # Clip to shape (alpha intersection).
            shape_rgba = Image.new('RGBA', (w, h), (0, 0, 0, 0))
            shape_rgba.putalpha(shape_alpha)
            clipped.putalpha(Image.eval(
                Image.merge('L', (a,)),
                lambda v: v))
            img.alpha_composite(clipped)
            samples.append(time.perf_counter() - t0)
        return samples

    _print_row('inset_shadow_gpu', _run_on_worker(body_gpu))
    _print_row('inset_shadow_cpu_baseline', body_cpu())


def main() -> int:
    print('SAO compose microbench (v2.2.11 Phase 6)')
    print('=' * 70)
    _bench_compositor()
    _bench_blur_tex()
    _bench_inset_shadow()
    print('\nDone.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
