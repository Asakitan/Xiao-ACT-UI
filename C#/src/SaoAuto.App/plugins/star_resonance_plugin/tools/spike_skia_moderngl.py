"""
Spike: verify skia-python (CPU SkSurface for text/atlas baking) + moderngl
(standalone GL context for SDF shader rendering) can coexist in one process,
and that a skia-rendered RGBA atlas uploads cleanly into a moderngl texture.

Run:
    e:/Py/python.exe -m sao_auto.tools.spike_skia_moderngl
or:
    cd e:/VC/SAO-UI/sao_auto/tools && e:/Py/python.exe spike_skia_moderngl.py
"""
from __future__ import annotations

import os
import sys
import time

# Ensure we can import sibling modules whether run as script or as -m
HERE = os.path.dirname(os.path.abspath(__file__))
PARENT = os.path.dirname(HERE)
if PARENT not in sys.path:
    sys.path.insert(0, PARENT)

import numpy as np
import skia
import moderngl


W, H = 256, 64


def bake_skia_atlas() -> bytes:
    """CPU-side skia: draw 'BURST READY' to RGBA bytes."""
    info = skia.ImageInfo.Make(W, H, skia.ColorType.kRGBA_8888_ColorType,
                               skia.AlphaType.kPremul_AlphaType)
    surf = skia.Surface.MakeRaster(info)
    canvas = surf.getCanvas()
    canvas.clear(skia.ColorTRANSPARENT)
    paint = skia.Paint(AntiAlias=True, Color=skia.ColorSetARGB(255, 0, 220, 255))
    font = skia.Font(skia.Typeface('Arial'), 36)
    canvas.drawString('BURST READY', 8, 44, font, paint)
    img = surf.makeImageSnapshot()
    arr = img.toarray(colorType=skia.ColorType.kRGBA_8888_ColorType)
    return arr.tobytes()


def upload_to_moderngl(rgba: bytes) -> tuple[float, float]:
    """Create standalone GL context, upload texture, render to FBO, read back."""
    t0 = time.perf_counter()
    ctx = moderngl.create_standalone_context(require=330)
    t1 = time.perf_counter()

    tex = ctx.texture((W, H), 4, rgba)
    tex.use(0)

    fbo = ctx.framebuffer(color_attachments=[ctx.texture((W, H), 4)])
    fbo.use()
    ctx.clear(0.0, 0.0, 0.0, 0.0)

    prog = ctx.program(
        vertex_shader="""
            #version 330
            in vec2 in_pos;
            in vec2 in_uv;
            out vec2 v_uv;
            void main() {
                v_uv = in_uv;
                gl_Position = vec4(in_pos, 0.0, 1.0);
            }
        """,
        fragment_shader="""
            #version 330
            uniform sampler2D tex0;
            in vec2 v_uv;
            out vec4 fragColor;
            void main() {
                vec4 c = texture(tex0, v_uv);
                // Tint cyan + slight bloom approximation
                fragColor = vec4(c.rgb, c.a);
            }
        """,
    )
    prog['tex0'].value = 0

    quad = np.array([
        -1, -1, 0, 1,
         1, -1, 1, 1,
        -1,  1, 0, 0,
         1,  1, 1, 0,
    ], dtype='f4')
    vbo = ctx.buffer(quad.tobytes())
    vao = ctx.vertex_array(prog, [(vbo, '2f 2f', 'in_pos', 'in_uv')])
    vao.render(moderngl.TRIANGLE_STRIP)
    out = fbo.read(components=4)
    t2 = time.perf_counter()

    # Sanity: at least one pixel should be non-zero alpha
    arr = np.frombuffer(out, dtype=np.uint8).reshape(H, W, 4)
    nonzero = int((arr[..., 3] > 0).sum())

    fbo.release()
    vao.release()
    vbo.release()
    prog.release()
    tex.release()
    ctx.release()

    print(f'[spike] skia atlas bytes  : {len(rgba)}')
    print(f'[spike] mgl ctx create ms : {(t1 - t0) * 1000:.2f}')
    print(f'[spike] mgl draw + read ms: {(t2 - t1) * 1000:.2f}')
    print(f'[spike] non-zero alpha px : {nonzero} / {W * H}')
    if nonzero == 0:
        raise RuntimeError('skia->moderngl pipeline produced empty alpha')
    return (t1 - t0) * 1000, (t2 - t1) * 1000


def main() -> int:
    t0 = time.perf_counter()
    rgba = bake_skia_atlas()
    t1 = time.perf_counter()
    print(f'[spike] skia bake ms      : {(t1 - t0) * 1000:.2f}')
    upload_to_moderngl(rgba)
    print('[spike] OK — skia-python + moderngl coexist verified')
    return 0


if __name__ == '__main__':
    sys.exit(main())
