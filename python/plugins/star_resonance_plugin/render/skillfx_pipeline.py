# Star Resonance skillfx_pipeline — GPU SDF shader pipeline (plugin-local).
#
# Moved from platform ``render/skillfx_pipeline.py`` in the 5.0.0 platform/plugin
# separation pass.  The shader file now lives with the Star Resonance plugin at
# ``plugins/star_resonance_plugin/shaders/skillfx.frag``; the path resolver below
# prefers that plugin-local asset and keeps platform-root paths only as legacy
# compatibility fallbacks.
#
# Public API (preserved):
# pipe = get_skillfx_pipeline()           # None if GPU unavailable
# rgba = pipe.render(width, height, params_dict)   # PIL.Image RGBA or None
from __future__ import annotations

import os
import sys
import threading
from typing import Any, Dict, Optional, Tuple

import numpy as np
from PIL import Image

from render import gpu_renderer as _gr
import _sao_cy_sr_uihelpers as _CY_UI  # type: ignore[import-not-found]

assert hasattr(_CY_UI, 'unpack_skillfx_params'), (
    'skillfx_pipeline requires _sao_cy_sr_uihelpers.unpack_skillfx_params; '
    'rebuild via `python build_cython_ext.py build_ext --inplace`.'
)


def _resolve_shader_path() -> str:
    # Find the plugin-owned ``skillfx.frag`` shader asset.
    #
    # Dev mode:   sao_auto/python/plugins/star_resonance_plugin/render/  → plugin root
    # Frozen:     runtime/plugins/star_resonance_plugin/render/          → plugin root
    # Also checks sys._MEIPASS and exe dir for frozen plugin layouts, then legacy
    # platform-root shader paths for fallback compatibility.
    HERE = os.path.dirname(os.path.abspath(__file__))
    plugin_root = os.path.dirname(HERE)
    runtime_root = os.path.dirname(os.path.dirname(plugin_root))
    plugin_shader_rel = os.path.join(
        'plugins', 'star_resonance_plugin', 'shaders', 'skillfx.frag',
    )
    legacy_shader_rel = os.path.join('shaders', 'skillfx.frag')
    candidates = [
        os.path.join(plugin_root, 'shaders', 'skillfx.frag'),
        os.path.join(runtime_root, plugin_shader_rel),
    ]
    meipass = getattr(sys, '_MEIPASS', None)
    if meipass:
        candidates.append(os.path.join(meipass, plugin_shader_rel))
        candidates.append(os.path.join(meipass, '_internal', plugin_shader_rel))
    if getattr(sys, 'frozen', False):
        exe_dir = os.path.dirname(os.path.abspath(sys.executable))
        candidates.append(os.path.join(exe_dir, plugin_shader_rel))
        candidates.append(os.path.join(exe_dir, '_internal', plugin_shader_rel))
    candidates.append(os.path.join(runtime_root, legacy_shader_rel))
    if meipass:
        candidates.append(os.path.join(meipass, legacy_shader_rel))
        candidates.append(os.path.join(meipass, '_internal', legacy_shader_rel))
    if getattr(sys, 'frozen', False):
        exe_dir = os.path.dirname(os.path.abspath(sys.executable))
        candidates.append(os.path.join(exe_dir, legacy_shader_rel))
        candidates.append(os.path.join(exe_dir, '_internal', legacy_shader_rel))
    for p in candidates:
        if os.path.isfile(p):
            return p
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
    # One instance per render-lane thread (created via get_skillfx_pipeline).

    def __init__(self, ctx: Any) -> None:
        self._ctx = ctx
        self._fbo_cache: Dict[Tuple[int, int], Any] = {}
        frag = _load_fragment()
        self._prog = ctx.program(vertex_shader=_VS, fragment_shader=frag)
        quad = np.array([-1, -1,  1, -1, -1,  1,
                          1, -1,  1,  1, -1,  1], dtype='f4')
        self._vbo = ctx.buffer(quad.tobytes())
        self._vao = ctx.vertex_array(self._prog, [(self._vbo, '2f', 'in_pos')])

    _FBO_CAP = 16

    def _get_fbo(self, w: int, h: int):
        key = (int(w), int(h))
        fbo = self._fbo_cache.get(key)
        if fbo is not None:
            return fbo
        ctx = self._ctx
        tex = ctx.texture((w, h), 4, dtype='f1')
        tex.filter = (0x2600, 0x2600)
        fbo = ctx.framebuffer(color_attachments=[tex])
        self._fbo_cache[key] = fbo
        while len(self._fbo_cache) > self._FBO_CAP:
            old_key = next(iter(self._fbo_cache))
            old = self._fbo_cache.pop(old_key)
            try:
                for att in old.color_attachments:
                    att.release()
                old.release()
            except Exception:
                pass
        return fbo

    def render(self, width: int, height: int,
               params: Dict[str, Any]) -> Optional[Image.Image]:
        # Render one frame of ring+beam+glow. Returns PIL RGBA (straight
        # alpha) or None on any error (caller must fall back to CPU path).
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
            return Image.frombuffer(
                'RGBA', (width, height), data, 'raw', 'RGBA', 0, -1,
            ).copy()
        except Exception as exc:
            try:
                print(f'[GPU-SR] skillfx pipeline render failed, fallback: {exc}')
            except Exception:
                pass
            return None

    def render_premultiplied_bytes(self, width: int, height: int,
                                   params: Dict[str, Any]) -> Optional[bytes]:
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
    # Get-or-create the calling thread's SkillFXShaderPipeline.
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
        try:
            print(
                f'[GPU-SR] skillfx pipeline init failed: shader missing ({exc}); '
                f'expected at {_SHADER_PATH}',
                flush=True,
            )
        except Exception:
            pass
        _tls.failed = True
        return None
    except Exception as exc:
        try:
            print(f'[GPU-SR] skillfx pipeline init failed: {exc}')
        except Exception:
            pass
        _tls.failed = True
        return None