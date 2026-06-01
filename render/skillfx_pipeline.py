"""skillfx_pipeline.py — v2.3.0 GUI 渲染链路重置 Phase 1

GPU shader pipeline replacing the PIL/numpy ring + beam + tail + glow CPU
path inside sao_gui_skillfx.compose_frame.

Per-thread (TLS) lazy-init: each render-lane thread gets its own moderngl
standalone context (sharing the same TLS slot used by gpu_renderer's blur /
shell pipelines), so we coexist cleanly with gaussian_blur_rgba and
premultiply_bgra_bytes already on the same worker thread.

Public API:
    pipe = get_skillfx_pipeline()           # None if GPU unavailable
    rgba = pipe.render(width, height, params_dict)   # PIL.Image RGBA or None

Returns straight (non-premultiplied) RGBA so the caller can keep the
existing PIL `.alpha_composite()` path while we A/B test. Once visually
validated, the consumer can switch to direct moderngl-window blit
(Phase 2) and we can output premultiplied + skip the un-premultiply step.
"""
from __future__ import annotations

import os
import sys
import threading
from typing import Any, Dict, Optional, Tuple

import numpy as np
from PIL import Image

from render import gpu_renderer as _gr
import _sao_cy_uihelpers as _CY_UI  # type: ignore[import-not-found]

# C3 fix: do NOT wrap in try/except. _CY_UI is mandatory at runtime — a
# missing/stale .pyd would silently disable the GPU FX pipeline with a noisy
# per-frame log instead of failing loudly. If unpack_skillfx_params isn't yet
# in the compiled .pyd, this assertion fires once at import.
assert hasattr(_CY_UI, 'unpack_skillfx_params'), (
    'skillfx_pipeline requires _sao_cy_uihelpers.unpack_skillfx_params; '
    'rebuild via `python build_cython_ext.py build_ext --inplace`.'
)


# reorg 2026-06-02: 本模块从根目录下沉到 render/, __file__ 深一层,
# 故取上一级目录作为项目根 (dev: sao_auto/; frozen: runtime/), shaders/ 均在该层下.
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _resolve_shader_path() -> str:
    """v2.3.8: PyInstaller 下 __file__ 位于 _internal/, 但 ('shaders','shaders')
    打包后一般也落在 _internal/shaders/, 所以 HERE 依然能匹配. 但为防
    某些 PyInstaller 版本 / onefile 下 __file__ 被重定向到临时目录, 额外
    检查 sys._MEIPASS 和 可执行文件同级目录. 首个存在的路径作为返回值.
    """
    candidates = [os.path.join(HERE, 'shaders', 'skillfx.frag')]
    meipass = getattr(sys, '_MEIPASS', None)
    if meipass:
        candidates.append(os.path.join(meipass, 'shaders', 'skillfx.frag'))
    if getattr(sys, 'frozen', False):
        exe_dir = os.path.dirname(os.path.abspath(sys.executable))
        candidates.append(os.path.join(exe_dir, 'shaders', 'skillfx.frag'))
        candidates.append(os.path.join(exe_dir, '_internal', 'shaders', 'skillfx.frag'))
    for p in candidates:
        if os.path.isfile(p):
            return p
    # 返回默认 (让 _load_fragment 报 FileNotFoundError 以暴露问题)
    return candidates[0]


_SHADER_PATH = _resolve_shader_path()

_VS = """
#version 330
in vec2 in_pos;
out vec2 v_uv;
void main() {
    v_uv = in_pos * 0.5 + 0.5;
    gl_Position = vec4(in_pos, 0.0, 1.0);
}
"""

_tls = threading.local()  # per-thread: .pipe instance


def _load_fragment() -> str:
    with open(_SHADER_PATH, 'r', encoding='utf-8') as fh:
        return fh.read()


class SkillFXShaderPipeline:
    """One instance per render-lane thread (created via get_skillfx_pipeline)."""

    def __init__(self, ctx: Any) -> None:
        self._ctx = ctx
        self._fbo_cache: Dict[Tuple[int, int], Any] = {}
        # Compile shader
        frag = _load_fragment()
        self._prog = ctx.program(vertex_shader=_VS, fragment_shader=frag)
        # Fullscreen quad
        quad = np.array([-1, -1,  1, -1, -1,  1,
                          1, -1,  1,  1, -1,  1], dtype='f4')
        self._vbo = ctx.buffer(quad.tobytes())
        self._vao = ctx.vertex_array(self._prog, [(self._vbo, '2f', 'in_pos')])

    def _get_fbo(self, w: int, h: int):
        key = (int(w), int(h))
        fbo = self._fbo_cache.get(key)
        if fbo is not None:
            return fbo
        ctx = self._ctx
        tex = ctx.texture((w, h), 4, dtype='f1')
        tex.filter = (0x2600, 0x2600)  # GL_NEAREST
        fbo = ctx.framebuffer(color_attachments=[tex])
        self._fbo_cache[key] = fbo
        return fbo

    def render(self, width: int, height: int,
               params: Dict[str, Any]) -> Optional[Image.Image]:
        """Render one frame of ring+beam+glow. Returns PIL RGBA (straight
        alpha) or None on any error (caller must fall back to CPU path)."""
        if width <= 0 or height <= 0:
            return None
        try:
            ctx = self._ctx
            with _gr._render_lock, ctx:
                fbo = self._get_fbo(width, height)
                fbo.use()
                ctx.viewport = (0, 0, width, height)
                ctx.clear(0.0, 0.0, 0.0, 0.0)
                p = self._prog
                # D5: typed cython unpack of the 21 params.get + float/tuple
                # boxings. The GL uniform .value sets still happen in Python
                # (moderngl driver call), but the per-field coerce is in cython.
                (u_time, u_alpha_mul, u_anchor, u_r_out, u_r_in, u_r_core,
                 u_pulse, u_beam_a, u_beam_b, u_beam_h, u_show_age, u_exiting,
                 u_glfx_intensity, u_seed, u_gl_anchor, u_gl_label,
                 u_gl_panel_size) = _CY_UI.unpack_skillfx_params(params)
                p['u_resolution'].value = (float(width), float(height))
                p['u_time'].value = u_time
                p['u_alpha_mul'].value = u_alpha_mul
                p['u_anchor'].value = u_anchor
                p['u_r_out'].value = u_r_out
                p['u_r_in'].value = u_r_in
                p['u_r_core'].value = u_r_core
                p['u_pulse'].value = u_pulse
                p['u_beam_a'].value = u_beam_a
                p['u_beam_b'].value = u_beam_b
                p['u_beam_h'].value = u_beam_h
                p['u_show_age'].value = u_show_age
                p['u_exiting'].value = u_exiting
                p['u_glfx_intensity'].value = u_glfx_intensity
                p['u_seed'].value = u_seed
                p['u_gl_anchor'].value = u_gl_anchor
                p['u_gl_label'].value = u_gl_label
                p['u_gl_panel_size'].value = u_gl_panel_size

                self._vao.render()
                data = fbo.read(components=4, alignment=1)

            # Shader emits STRAIGHT-alpha RGBA (top-down via shader's flip);
            # build PIL image directly via Image.frombuffer with the GL
            # 'raw' decoder + negative stride to flip the bottom-up GL
            # output into top-down orientation. Zero numpy postprocessing.
            return Image.frombuffer(
                'RGBA', (width, height), data, 'raw', 'RGBA', 0, -1,
            ).copy()
        except Exception as exc:
            try:
                print(f'[GPU] skillfx pipeline render failed, fallback: {exc}')
            except Exception:
                pass
            return None

    def render_premultiplied_bytes(self, width: int, height: int,
                                   params: Dict[str, Any]) -> Optional[bytes]:
        """Phase 2 entry: returns raw premultiplied RGBA bytes (top-down)
        suitable for direct upload into a moderngl-window framebuffer or
        UpdateLayeredWindow (after BGRA swizzle)."""
        if width <= 0 or height <= 0:
            return None
        try:
            ctx = self._ctx
            with _gr._render_lock, ctx:
                fbo = self._get_fbo(width, height)
                fbo.use()
                ctx.viewport = (0, 0, width, height)
                ctx.clear(0.0, 0.0, 0.0, 0.0)
                p = self._prog
                # D5: typed cython unpack — same as render() above.
                (u_time, u_alpha_mul, u_anchor, u_r_out, u_r_in, u_r_core,
                 u_pulse, u_beam_a, u_beam_b, u_beam_h, u_show_age, u_exiting,
                 u_glfx_intensity, u_seed, u_gl_anchor, u_gl_label,
                 u_gl_panel_size) = _CY_UI.unpack_skillfx_params(params)
                p['u_resolution'].value = (float(width), float(height))
                p['u_time'].value = u_time
                p['u_alpha_mul'].value = u_alpha_mul
                p['u_anchor'].value = u_anchor
                p['u_r_out'].value = u_r_out
                p['u_r_in'].value = u_r_in
                p['u_r_core'].value = u_r_core
                p['u_pulse'].value = u_pulse
                p['u_beam_a'].value = u_beam_a
                p['u_beam_b'].value = u_beam_b
                p['u_beam_h'].value = u_beam_h
                p['u_show_age'].value = u_show_age
                p['u_exiting'].value = u_exiting
                p['u_glfx_intensity'].value = u_glfx_intensity
                p['u_seed'].value = u_seed
                p['u_gl_anchor'].value = u_gl_anchor
                p['u_gl_label'].value = u_gl_label
                p['u_gl_panel_size'].value = u_gl_panel_size
                self._vao.render()
                # GL bottom-up; for ULW (top-down) need flip.
                data = fbo.read(components=4, alignment=1)
            arr = np.frombuffer(data, dtype=np.uint8).reshape(height, width, 4)
            return np.flipud(arr).copy().tobytes()
        except Exception:
            return None

    def release(self) -> None:
        try:
            self._vao.release()
            self._vbo.release()
            self._prog.release()
            for fbo in self._fbo_cache.values():
                try:
                    for att in fbo.color_attachments:
                        att.release()
                    fbo.release()
                except Exception:
                    pass
            self._fbo_cache.clear()
        except Exception:
            pass


def get_skillfx_pipeline() -> Optional[SkillFXShaderPipeline]:
    """Get-or-create the calling thread's SkillFXShaderPipeline.

    Returns None if the per-thread GL context cannot be established (caller
    must fall back to PIL path)."""
    pipe = getattr(_tls, 'pipe', None)
    if pipe is not None:
        return pipe
    if getattr(_tls, 'failed', False):
        return None
    if not _gr._try_init():
        _tls.failed = True
        return None
    try:
        ctx = _gr._tls.ctx
        pipe = SkillFXShaderPipeline(ctx)
        _tls.pipe = pipe
        return pipe
    except FileNotFoundError as exc:
        # v2.3.8: 区分资源缺失 (打包问题) 与 GL 初始化失败.
        try:
            print(
                f'[GPU] skillfx pipeline init failed: shader missing ({exc}); '
                f'expected at {_SHADER_PATH}',
                flush=True,
            )
        except Exception:
            pass
        _tls.failed = True
        return None
    except Exception as exc:
        try:
            print(f'[GPU] skillfx pipeline init failed: {exc}')
        except Exception:
            pass
        _tls.failed = True
        return None
