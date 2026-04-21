"""Diagnose where SkillFX shader pipeline time goes: shader vs readback."""
from __future__ import annotations
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
PARENT = os.path.dirname(HERE)
if PARENT not in sys.path:
    sys.path.insert(0, PARENT)

import numpy as np
import gpu_renderer as gr
import skillfx_pipeline as sp

# Init context + pipeline once
pipe = sp.get_skillfx_pipeline()
assert pipe is not None
ctx = gr._tls.ctx

PARAMS = dict(
    time=1.6, alpha_mul=1.0, anchor=(560.0, 800.0),
    r_out=88.0, r_in=72.0, r_core=50.0, pulse=0.62,
    beam_a=(594.0, 800.0), beam_b=(1280.0, 480.0), beam_h=30.0,
    show_age=1.6, exiting=False, glfx_intensity=0.7, seed=1.42,
)

for W, H in [(1920, 1080), (1280, 720), (640, 360), (256, 256)]:
    fbo = pipe._get_fbo(W, H)
    p = pipe._prog
    # warm
    for _ in range(3):
        with gr._render_lock, ctx:
            fbo.use(); ctx.viewport = (0, 0, W, H); ctx.clear()
            p['u_resolution'].value = (float(W), float(H))
            for k, v in PARAMS.items():
                key = 'u_' + k
                try:
                    p[key].value = v if not isinstance(v, bool) else (1.0 if v else 0.0)
                except Exception:
                    pass
            pipe._vao.render()
            ctx.finish()
    # Time render only (no readback)
    N = 30
    t0 = time.perf_counter()
    for _ in range(N):
        with gr._render_lock, ctx:
            fbo.use(); ctx.viewport = (0, 0, W, H); ctx.clear()
            pipe._vao.render()
            ctx.finish()
    t_render = (time.perf_counter() - t0) / N * 1000
    # Time readback only
    t0 = time.perf_counter()
    for _ in range(N):
        with gr._render_lock, ctx:
            fbo.use()
            data = fbo.read(components=4, alignment=1)
    t_read = (time.perf_counter() - t0) / N * 1000
    # Time render + readback
    t0 = time.perf_counter()
    for _ in range(N):
        with gr._render_lock, ctx:
            fbo.use(); ctx.viewport = (0, 0, W, H); ctx.clear()
            pipe._vao.render()
            data = fbo.read(components=4, alignment=1)
    t_both = (time.perf_counter() - t0) / N * 1000
    px = W * H
    print(f'{W:5}x{H:<5} ({px/1e6:.2f} Mpx)  render {t_render:6.2f} ms  read {t_read:6.2f} ms  both {t_both:6.2f} ms')
