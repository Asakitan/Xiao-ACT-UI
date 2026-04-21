"""Visual + perf A/B comparison: PIL SkillFX render vs new shader pipeline.

Renders one steady-state burst frame (slot index = 1) at 1920×1080, dumps:
  - tools/out/skillfx_pil.png        (existing PIL/numpy compose path)
  - tools/out/skillfx_gpu.png        (new SkillFXShaderPipeline)
  - tools/out/skillfx_diff.png       (per-pixel abs-diff x4 for visibility)

Prints timing for both paths over N iterations to compare CPU/wall cost.
"""
from __future__ import annotations

import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PARENT = os.path.dirname(HERE)
if PARENT not in sys.path:
    sys.path.insert(0, PARENT)

OUT = os.path.join(HERE, 'out')
os.makedirs(OUT, exist_ok=True)

import math
import numpy as np
from PIL import Image

import skillfx_pipeline as sp


W, H = 1920, 1080


# ── Mock burst params (steady-state, alpha_mul ≈ 1.0) ──────────────────────

ANCHOR = (560.0, 800.0)        # ring center
RING_OUT = 88.0                # half of RING_SIZE=176
RING_IN = RING_OUT - 16.0
RING_CORE = RING_OUT - 38.0    # = 50
PULSE = 0.62
BEAM_A = (ANCHOR[0] + RING_OUT * 0.38 * 1.0, ANCHOR[1])  # roughly along ring-edge
BEAM_B = (1280.0, 480.0)        # caption anchor
BEAM_H = 30.0
SHOW_AGE = 1.6                  # past sweep but still in tail loop
TIME = 1.6
ALPHA = 1.0


def render_gpu() -> Image.Image:
    pipe = sp.get_skillfx_pipeline()
    assert pipe is not None, 'GPU pipeline init failed'
    params = dict(
        time=TIME, alpha_mul=ALPHA, anchor=ANCHOR,
        r_out=RING_OUT, r_in=RING_IN, r_core=RING_CORE,
        pulse=PULSE, beam_a=BEAM_A, beam_b=BEAM_B, beam_h=BEAM_H,
        show_age=SHOW_AGE, exiting=False, glfx_intensity=0.7, seed=1.42,
    )
    img = pipe.render(W, H, params)
    assert img is not None
    return img


def render_pil_via_overlay() -> Image.Image:
    """Drive the real BurstReadyOverlay.compose_frame to produce reference."""
    # Create a fake Tk root (headless) — sao_gui_skillfx imports tkinter and
    # uses _user32 / FrameWorker. We construct only what compose_frame needs.
    import tkinter as tk
    root = tk.Tk()
    root.withdraw()
    try:
        from sao_gui_skillfx import BurstReadyOverlay
        ov = BurstReadyOverlay(root)
        # Push layout
        ov.set_layout({
            'window': {'x': 0, 'y': 0, 'w': W, 'h': H},
            'viewport': {'callout': {'x': int(BEAM_B[0]) - 28,
                                      'y': int(BEAM_B[1] - 0.56 * 128),
                                      'w': 460, 'h': 128}},
            'slots': [{'index': 1, 'rect': {
                'x': int(ANCHOR[0]) - 42, 'y': int(ANCHOR[1]) - 42,
                'w': 84, 'h': 84,
            }}],
        })
        # Force into steady-state without actually showing
        ov._slot_index = 1
        ov._update_anchor(1)
        ov._anchor = ANCHOR
        ov._ring_size = int(RING_OUT * 2)
        ov._show_t = time.time() - SHOW_AGE
        ov._active = True
        ov._exiting = False
        ov._sync_gl_targets(reset=True)
        # Pre-bake caption
        ov._ensure_caption_layers()
        img = ov.compose_frame(time.time())
        ov.destroy()
        return img.copy()
    finally:
        try:
            root.destroy()
        except Exception:
            pass


def time_gpu(n: int = 20) -> float:
    pipe = sp.get_skillfx_pipeline()
    assert pipe is not None
    params = dict(
        time=TIME, alpha_mul=ALPHA, anchor=ANCHOR,
        r_out=RING_OUT, r_in=RING_IN, r_core=RING_CORE,
        pulse=PULSE, beam_a=BEAM_A, beam_b=BEAM_B, beam_h=BEAM_H,
        show_age=SHOW_AGE, exiting=False, glfx_intensity=0.7, seed=1.42,
    )
    # warm-up
    for _ in range(3):
        pipe.render(W, H, params)
    t0 = time.perf_counter()
    for _ in range(n):
        params['time'] = time.perf_counter()
        pipe.render(W, H, params)
    return (time.perf_counter() - t0) / n * 1000.0


def main() -> int:
    print(f'== SkillFX A/B render @ {W}x{H} ==')
    t0 = time.perf_counter()
    gpu_img = render_gpu()
    t_gpu_one = (time.perf_counter() - t0) * 1000.0
    print(f'[gpu] one-shot (incl. compile): {t_gpu_one:.2f} ms')

    gpu_img.save(os.path.join(OUT, 'skillfx_gpu.png'))
    print(f'[gpu] saved skillfx_gpu.png')

    avg = time_gpu(30)
    print(f'[gpu] steady-state avg over 30: {avg:.2f} ms')

    print('[pil] rendering reference via BurstReadyOverlay.compose_frame ...')
    t0 = time.perf_counter()
    pil_img = render_pil_via_overlay()
    t_pil = (time.perf_counter() - t0) * 1000.0
    print(f'[pil] one-shot: {t_pil:.2f} ms')
    pil_img.save(os.path.join(OUT, 'skillfx_pil.png'))
    print(f'[pil] saved skillfx_pil.png')

    # Diff
    a = np.asarray(gpu_img.convert('RGBA'), dtype=np.int16)
    b = np.asarray(pil_img.convert('RGBA'), dtype=np.int16)
    if a.shape == b.shape:
        diff = np.abs(a - b).astype(np.uint8)
        diff[..., 3] = 255
        diff_vis = np.minimum(diff.astype(np.uint16) * 4, 255).astype(np.uint8)
        Image.fromarray(diff_vis, 'RGBA').save(os.path.join(OUT, 'skillfx_diff.png'))
        nonzero = int((diff[..., :3].sum(axis=-1) > 0).sum())
        total = a.shape[0] * a.shape[1]
        print(f'[diff] non-zero pixels: {nonzero}/{total} ({100.0 * nonzero / total:.2f}%)')
        max_d = int(diff[..., :3].max())
        mean_d = float(diff[..., :3].mean())
        print(f'[diff] max channel delta: {max_d}, mean: {mean_d:.2f}')
    else:
        print(f'[diff] SHAPE MISMATCH: gpu {a.shape} vs pil {b.shape}')

    print(f'\nOpen tools/out/*.png to A/B compare visually.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
