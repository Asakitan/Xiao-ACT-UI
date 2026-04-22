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

import gpu_renderer as _gr


HERE = os.path.dirname(os.path.abspath(__file__))


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
                # Required uniforms — get with default no-op for missing
                p['u_resolution'].value = (float(width), float(height))
                p['u_time'].value = float(params.get('time', 0.0))
                p['u_alpha_mul'].value = float(params.get('alpha_mul', 1.0))
                p['u_anchor'].value = tuple(map(float, params.get('anchor', (0.0, 0.0))))
                p['u_r_out'].value = float(params.get('r_out', 0.0))
                p['u_r_in'].value = float(params.get('r_in', 0.0))
                p['u_r_core'].value = float(params.get('r_core', 0.0))
                p['u_pulse'].value = float(params.get('pulse', 0.5))
                p['u_beam_a'].value = tuple(map(float, params.get('beam_a', (0.0, 0.0))))
                p['u_beam_b'].value = tuple(map(float, params.get('beam_b', (0.0, 0.0))))
                p['u_beam_h'].value = float(params.get('beam_h', 30.0))
                p['u_show_age'].value = float(params.get('show_age', 0.0))
                p['u_exiting'].value = 1.0 if params.get('exiting') else 0.0
                p['u_glfx_intensity'].value = float(params.get('glfx_intensity', 0.0))
                p['u_seed'].value = float(params.get('seed', 1.0))
                # v2.3.0 (2026-04 fix): legacy lerped GLFX uniforms.
                p['u_gl_anchor'].value = tuple(map(float, params.get('gl_anchor', (0.0, 0.0))))
                p['u_gl_label'].value = tuple(map(float, params.get('gl_label', (0.0, 0.0))))
                p['u_gl_panel_size'].value = tuple(map(float, params.get('gl_panel_size', (0.0, 0.0))))

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
                p['u_resolution'].value = (float(width), float(height))
                p['u_time'].value = float(params.get('time', 0.0))
                p['u_alpha_mul'].value = float(params.get('alpha_mul', 1.0))
                p['u_anchor'].value = tuple(map(float, params.get('anchor', (0.0, 0.0))))
                p['u_r_out'].value = float(params.get('r_out', 0.0))
                p['u_r_in'].value = float(params.get('r_in', 0.0))
                p['u_r_core'].value = float(params.get('r_core', 0.0))
                p['u_pulse'].value = float(params.get('pulse', 0.5))
                p['u_beam_a'].value = tuple(map(float, params.get('beam_a', (0.0, 0.0))))
                p['u_beam_b'].value = tuple(map(float, params.get('beam_b', (0.0, 0.0))))
                p['u_beam_h'].value = float(params.get('beam_h', 30.0))
                p['u_show_age'].value = float(params.get('show_age', 0.0))
                p['u_exiting'].value = 1.0 if params.get('exiting') else 0.0
                p['u_glfx_intensity'].value = float(params.get('glfx_intensity', 0.0))
                p['u_seed'].value = float(params.get('seed', 1.0))
                p['u_gl_anchor'].value = tuple(map(float, params.get('gl_anchor', (0.0, 0.0))))
                p['u_gl_label'].value = tuple(map(float, params.get('gl_label', (0.0, 0.0))))
                p['u_gl_panel_size'].value = tuple(map(float, params.get('gl_panel_size', (0.0, 0.0))))
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
