"""gpu_compositor.py — GPU layer compositor for overlay panels (v2.2.11).

Phase 1 of the SAO overlay GPU acceleration plan. Provides a per-panel,
per-thread `LayerCompositor` that owns persistent FBO textures plus a
small registry of fragment shaders used by SkillFX, BossHP, and the SAO
menu reveal animation.

Design notes
------------
- One `LayerCompositor` instance per overlay panel. Lives on the panel's
  render-lane thread; reuses the per-thread WGL context that
  `gpu_renderer._tls.ctx` already manages.
- Texture pool is keyed by ``(tag, w, h)`` with a 16-entry LRU cap, so
  transient size changes (e.g. menu reveal animation) do not leak.
- Shader programs are lazy-compiled per-thread on first use and shared
  via the same `_tls` storage as `gpu_renderer.py`.
- Public API (all calls must be made from the owning render-lane thread):
    cmp = LayerCompositor(name="skillfx")
    if cmp.available:
        tex = cmp.tex('main', w, h, clear=True)
        cmp.render('halo_field', tex, uniforms={...})
        cmp.render('over', tex, uniforms={...}, inputs={'u_top': other_tex})
        bgra = cmp.read_bgra_premultiplied(tex)   # ready for ULW
        # or fallback when partial migration:
        pil = cmp.to_pil(tex)

Shaders provided
----------------
- ``over``           — straight-alpha "src over dst" composite of two textures.
- ``gradient_bar``   — 2/3-stop horizontal gradient + vertical shading.
- ``halo_field``     — exponential ring halo + core fill (SkillFX).
- ``sweep_arc``      — radial sweep band (SkillFX).
- ``light_sweep``    — angled highlight band (BossHP shield bar).
- ``shimmer_scan``   — moving brightness band (BossHP break shimmer).
- ``inset_shadow``   — invert + blur + clip to alpha (BossHP shell).
- ``beam``           — procedural beam sprite with rotation (SkillFX).

When `gpu_available()` is False the constructor returns an instance with
``.available == False`` and all render calls become no-ops; callers are
expected to fall back to their existing PIL/numpy paths.
"""

from __future__ import annotations

import collections
import threading
from typing import Dict, Optional, Tuple, Any, Mapping

import numpy as np
from PIL import Image

import gpu_renderer
from gpu_renderer import _tls, _try_init, _VS_FULLSCREEN


# ─────────────────────────────────────────────────────────────────────
# Fragment shaders
# ─────────────────────────────────────────────────────────────────────

_FS_OVER = """
#version 330
uniform sampler2D u_top;
uniform sampler2D u_bot;
uniform float u_top_alpha;
in vec2 v_uv;
out vec4 f_color;
void main() {
    vec4 t = texture(u_top, v_uv);
    vec4 b = texture(u_bot, v_uv);
    t.a *= u_top_alpha;
    float oa = t.a + b.a * (1.0 - t.a);
    vec3 oc = (t.rgb * t.a + b.rgb * b.a * (1.0 - t.a));
    if (oa > 0.0) oc /= oa;
    f_color = vec4(oc, oa);
}
"""

# A solid-into-target compositor. Use to clear a texture to a flat color
# (also handy for fades). u_color is straight alpha.
_FS_FILL = """
#version 330
uniform vec4 u_color;
out vec4 f_color;
void main() { f_color = u_color; }
"""

_FS_GRADIENT_BAR = """
#version 330
// Horizontal-fill bar with vertical gradient and fractional trailing
// column.  Replaces _make_gradient_bar + subpixel_bar_width.
uniform vec2 u_size;          // (w, h) in px
uniform float u_fill_w;       // float fill width in px (subpixel)
uniform vec4  u_color_top;
uniform vec4  u_color_bot;
uniform vec4  u_highlight;    // narrow lighter band along the top
uniform float u_highlight_h;  // px height of highlight band (0 to skip)
in vec2 v_uv;
out vec4 f_color;
void main() {
    vec2 px = v_uv * u_size;
    // Horizontal fill mask with subpixel trailing edge AA.
    float xfill = clamp(u_fill_w - px.x, 0.0, 1.0);
    if (xfill <= 0.0) { f_color = vec4(0.0); return; }
    float ty = clamp(px.y / max(1.0, u_size.y - 1.0), 0.0, 1.0);
    vec4 col = mix(u_color_top, u_color_bot, ty);
    if (u_highlight_h > 0.0 && px.y < u_highlight_h) {
        float k = 1.0 - px.y / u_highlight_h;
        col.rgb = mix(col.rgb, u_highlight.rgb, u_highlight.a * k);
    }
    col.a *= xfill;
    f_color = col;
}
"""

_FS_HALO_FIELD = """
#version 330
// Ring halo with exponential falloff around an outer radius + core fill.
// Replaces SkillFX _get_ring_layer numpy build.
uniform vec2  u_size;       // (w, h) px
uniform vec2  u_center;     // ring center px
uniform float u_r_out;      // outer radius (where halo peaks)
uniform float u_falloff;    // exp falloff sigma (px)
uniform float u_peak_alpha; // 0..1 peak alpha
uniform vec4  u_color;      // halo color (rgb, a==1 baseline)
uniform float u_core_r;     // optional inner solid-fill radius (0 to skip)
uniform vec4  u_core_color;
in vec2 v_uv;
out vec4 f_color;
void main() {
    vec2 px = v_uv * u_size;
    float d = distance(px, u_center);
    float t = (d - u_r_out) / max(1.0, u_falloff);
    float halo_a = exp(-t * t) * u_peak_alpha;
    vec4 c = vec4(u_color.rgb, halo_a);
    if (u_core_r > 0.0 && d <= u_core_r) {
        float core_a = clamp(u_core_r - d, 0.0, 1.0);
        c.rgb = mix(c.rgb, u_core_color.rgb, core_a * u_core_color.a);
        c.a = max(c.a, core_a * u_core_color.a);
    }
    f_color = c;
}
"""

_FS_SWEEP_ARC = """
#version 330
// Per-frame rotating sweep band on a thin ring.
// Replaces SkillFX _draw_ring's per-frame numpy exp(-((xs-band_x)/8.5)^2).
uniform vec2  u_size;         // (w, h) px
uniform vec2  u_center;       // ring center
uniform float u_radius;       // ring radius (px)
uniform float u_thickness;    // ring thickness (px)
uniform float u_angle;        // sweep center angle (radians)
uniform float u_arc_half;     // sweep half-width (radians)
uniform float u_band_sigma;   // angular falloff sigma (radians)
uniform vec4  u_color;
in vec2 v_uv;
out vec4 f_color;
void main() {
    vec2 px = v_uv * u_size;
    vec2 d = px - u_center;
    float r = length(d);
    float r_band = 1.0 - smoothstep(0.5, u_thickness * 0.5 + 0.5, abs(r - u_radius));
    if (r_band <= 0.0) { f_color = vec4(0.0); return; }
    float a = atan(d.y, d.x) - u_angle;
    // wrap into [-pi, pi]
    a = mod(a + 3.14159265, 6.2831853) - 3.14159265;
    float a_band = exp(-(a * a) / max(1e-6, 2.0 * u_band_sigma * u_band_sigma));
    float arc_mask = 1.0 - smoothstep(u_arc_half, u_arc_half + 0.05, abs(a));
    float alpha = u_color.a * r_band * a_band * arc_mask;
    f_color = vec4(u_color.rgb, alpha);
}
"""

_FS_LIGHT_SWEEP = """
#version 330
// Angled highlight band for BossHP shield bar.
// Replaces _make_light_sweep numpy build.
uniform vec2  u_size;
uniform float u_band_x;     // band center x in px
uniform float u_band_w;     // half-width in px
uniform float u_skew;       // x-shift per y in px (slope of the band)
uniform vec4  u_color;
in vec2 v_uv;
out vec4 f_color;
void main() {
    vec2 px = v_uv * u_size;
    float bx = u_band_x + (px.y - u_size.y * 0.5) * u_skew;
    float t = (px.x - bx) / max(1.0, u_band_w);
    float a = exp(-t * t) * u_color.a;
    f_color = vec4(u_color.rgb, a);
}
"""

_FS_SHIMMER_SCAN = """
#version 330
// Moving brightness band on a horizontal bar (BossHP break recover).
uniform vec2  u_size;
uniform float u_phase_x;    // band center x in px
uniform float u_band_w;     // half-width in px
uniform float u_fill_w;     // current bar fill width in px (mask)
uniform vec4  u_color;
in vec2 v_uv;
out vec4 f_color;
void main() {
    vec2 px = v_uv * u_size;
    if (px.x > u_fill_w) { f_color = vec4(0.0); return; }
    float t = (px.x - u_phase_x) / max(1.0, u_band_w);
    float a = exp(-t * t) * u_color.a;
    f_color = vec4(u_color.rgb, a);
}
"""

_FS_INSET_SHADOW = """
#version 330
// Inset shadow: sample blurred *inverse-alpha* texture, then clip to the
// host shape's alpha.  Used to draw a soft inner darkening on the BossHP
// shell without the CPU↔GPU bounce that _apply_inset_shadow does today.
uniform sampler2D u_blurred_inv;  // pre-blurred inverse-alpha mask
uniform sampler2D u_shape;        // original shape alpha
uniform vec4  u_color;            // shadow rgb + base alpha
uniform float u_intensity;        // 0..1 scale on shadow
in vec2 v_uv;
out vec4 f_color;
void main() {
    float shadow = texture(u_blurred_inv, v_uv).a;
    float clip   = texture(u_shape, v_uv).a;
    float a = shadow * clip * u_color.a * u_intensity;
    f_color = vec4(u_color.rgb, a);
}
"""

_FS_BEAM = """
#version 330
// Procedural beam sprite (replaces SkillFX _fast_beam numpy + PIL rotate).
// Beam is rendered axis-aligned along +X starting at (0, h/2); the caller
// pre-rotates by drawing into a container texture using a rotated quad,
// or by sampling u_dir for the orientation.
uniform vec2  u_size;
uniform vec2  u_origin;        // px (start of beam)
uniform vec2  u_dir;           // unit vector
uniform float u_length;        // beam length px
uniform float u_thickness;     // half-thickness px
uniform vec4  u_core_color;
uniform vec4  u_glow_color;
uniform float u_glow_thickness;// half-thickness px (glow > core)
in vec2 v_uv;
out vec4 f_color;
void main() {
    vec2 px = v_uv * u_size;
    vec2 d  = px - u_origin;
    float along = dot(d, u_dir);
    vec2 perp_v = d - along * u_dir;
    float perp = length(perp_v);
    if (along < 0.0 || along > u_length) { f_color = vec4(0.0); return; }
    // Soft caps near both ends (1px AA).
    float cap = min(
        smoothstep(0.0, 1.0, along),
        smoothstep(0.0, 1.0, u_length - along)
    );
    float core_a = (1.0 - smoothstep(u_thickness - 0.5, u_thickness + 0.5, perp))
                   * u_core_color.a * cap;
    float glow_a = exp(-(perp * perp) /
                       max(1.0, u_glow_thickness * u_glow_thickness))
                   * u_glow_color.a * cap;
    vec3 rgb = mix(u_glow_color.rgb, u_core_color.rgb, core_a);
    float a = max(core_a, glow_a);
    f_color = vec4(rgb, a);
}
"""

# ULW download path — same as gpu_renderer's premult shader but reads from
# an arbitrary input texture (not a pre-blitted FBO) so the compositor can
# avoid a redundant copy.
_FS_DOWNLOAD_BGRA = """
#version 330
uniform sampler2D u_tex;
in vec2 v_uv;
out vec4 f_color;
void main() {
    vec4 c = texture(u_tex, v_uv);
    f_color = vec4(c.b * c.a, c.g * c.a, c.r * c.a, c.a);
}
"""


_SHADER_SRC: Dict[str, str] = {
    'over':         _FS_OVER,
    'fill':         _FS_FILL,
    'gradient_bar': _FS_GRADIENT_BAR,
    'halo_field':   _FS_HALO_FIELD,
    'sweep_arc':    _FS_SWEEP_ARC,
    'light_sweep':  _FS_LIGHT_SWEEP,
    'shimmer_scan': _FS_SHIMMER_SCAN,
    'inset_shadow': _FS_INSET_SHADOW,
    'beam':         _FS_BEAM,
    'download_bgra': _FS_DOWNLOAD_BGRA,
}


# ─────────────────────────────────────────────────────────────────────
# Per-thread shader / quad cache (one entry per shader name)
# ─────────────────────────────────────────────────────────────────────

def _ensure_program(name: str):
    """Lazy-compile a shader program in the current thread's GL context."""
    cache = getattr(_tls, 'compositor_progs', None)
    if cache is None:
        cache = {}
        _tls.compositor_progs = cache
    entry = cache.get(name)
    if entry is not None:
        return entry
    ctx = _tls.ctx
    src = _SHADER_SRC[name]
    prog = ctx.program(vertex_shader=_VS_FULLSCREEN, fragment_shader=src)

    quad_vbo = getattr(_tls, 'compositor_vbo', None)
    if quad_vbo is None:
        quad = np.array([-1, -1,  1, -1, -1,  1,
                          1, -1,  1,  1, -1,  1], dtype='f4')
        quad_vbo = ctx.buffer(quad.tobytes())
        _tls.compositor_vbo = quad_vbo
    vao = ctx.vertex_array(prog, [(quad_vbo, '2f', 'in_pos')])
    cache[name] = (prog, vao)
    return prog, vao


# ─────────────────────────────────────────────────────────────────────
# LayerCompositor
# ─────────────────────────────────────────────────────────────────────

class LayerCompositor:
    """Per-panel GPU compositor.

    Lives on the panel's render-lane thread. All `tex`/`render`/`read_*`
    calls must originate from that thread (the WGL context is thread-
    affine).
    """

    _LRU_CAP = 16

    def __init__(self, name: str = 'panel'):
        self.name = name
        self._available = _try_init()
        # Per-instance LRU of (tag, w, h) → (texture, fbo). Held weakly
        # via OrderedDict so eviction order is deterministic.
        self._tex_cache: 'collections.OrderedDict[Tuple[str, int, int], Tuple[Any, Any]]' = (
            collections.OrderedDict()
        )

    # ─── lifecycle ──────────────────────────────────────────────
    @property
    def available(self) -> bool:
        return self._available

    def release(self) -> None:
        """Release all owned textures + framebuffers."""
        if not self._available:
            return
        ctx = _tls.ctx
        if ctx is None:
            return
        with ctx:
            for _, (tex, fbo) in self._tex_cache.items():
                try:
                    fbo.release()
                except Exception:
                    pass
                try:
                    tex.release()
                except Exception:
                    pass
        self._tex_cache.clear()

    # ─── texture pool ───────────────────────────────────────────
    def tex(self, tag: str, w: int, h: int, *, clear: bool = True):
        """Get-or-allocate an RGBA8 texture+FBO bound to ``tag``.

        When ``clear`` is True (default) the returned framebuffer has been
        cleared to transparent; the caller can immediately render into it.
        """
        if not self._available:
            return None
        if w <= 0 or h <= 0:
            return None
        key = (tag, int(w), int(h))
        entry = self._tex_cache.get(key)
        if entry is None:
            ctx = _tls.ctx
            with ctx:
                tex = ctx.texture((w, h), 4, dtype='f1')
                tex.filter = (0x2601, 0x2601)  # GL_LINEAR
                tex.repeat_x = False
                tex.repeat_y = False
                fbo = ctx.framebuffer(color_attachments=[tex])
            entry = (tex, fbo)
            self._tex_cache[key] = entry
            # Evict LRU.
            while len(self._tex_cache) > self._LRU_CAP:
                _, (old_tex, old_fbo) = self._tex_cache.popitem(last=False)
                try:
                    old_fbo.release()
                except Exception:
                    pass
                try:
                    old_tex.release()
                except Exception:
                    pass
        else:
            # Refresh LRU position.
            self._tex_cache.move_to_end(key)
        if clear:
            ctx = _tls.ctx
            with ctx:
                entry[1].use()
                ctx.viewport = (0, 0, key[1], key[2])
                ctx.clear(0.0, 0.0, 0.0, 0.0)
        return entry  # (tex, fbo)

    # ─── upload ─────────────────────────────────────────────────
    def upload(self, tag: str, image) -> Optional[Tuple[Any, Any]]:
        """Upload a PIL/ndarray RGBA into the named texture (replacing it).

        Use sparingly — the whole point of the compositor is to keep
        intermediates on the GPU.
        """
        if not self._available:
            return None
        if isinstance(image, Image.Image):
            arr = np.asarray(image.convert('RGBA'), dtype=np.uint8)
        elif isinstance(image, np.ndarray):
            arr = np.ascontiguousarray(image)
            if arr.dtype != np.uint8 or arr.ndim != 3 or arr.shape[2] != 4:
                raise ValueError(f'expected uint8 RGBA, got {arr.shape} {arr.dtype}')
        else:
            raise TypeError(f'unsupported image type: {type(image)}')
        h, w, _ = arr.shape
        # Re-allocate every call when the size changes; otherwise just
        # write into the existing texture in place.
        key = (tag, int(w), int(h))
        entry = self._tex_cache.get(key)
        ctx = _tls.ctx
        with ctx:
            if entry is None:
                tex = ctx.texture((w, h), 4, arr.tobytes(), dtype='f1')
                tex.filter = (0x2601, 0x2601)
                tex.repeat_x = False
                tex.repeat_y = False
                fbo = ctx.framebuffer(color_attachments=[tex])
                entry = (tex, fbo)
                self._tex_cache[key] = entry
            else:
                entry[0].write(arr.tobytes())
                self._tex_cache.move_to_end(key)
        return entry

    # ─── render ─────────────────────────────────────────────────
    def render(self, shader: str, target,
               uniforms: Optional[Mapping[str, Any]] = None,
               inputs: Optional[Mapping[str, Any]] = None,
               *, blend: bool = False) -> None:
        """Run ``shader`` over the whole framebuffer of ``target``.

        ``target`` is the (texture, fbo) tuple returned by `tex()`.
        ``uniforms`` are scalar/vector uniforms; ``inputs`` is a
        ``{uniform_name: (texture, fbo) or texture}`` map of sampler
        bindings (textures bound to consecutive units starting at 0).
        ``blend=True`` enables straight-alpha source-over blending so
        subsequent draws can composite onto an existing target.
        """
        if not self._available or target is None:
            return
        prog, vao = _ensure_program(shader)
        ctx = _tls.ctx
        tex_target, fbo_target = target
        with ctx:
            fbo_target.use()
            ctx.viewport = (0, 0, tex_target.width, tex_target.height)
            if blend:
                # GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA — straight-alpha.
                ctx.enable(0x0BE2)  # GL_BLEND
                ctx.blend_func = (0x0302, 0x0303)
            else:
                ctx.disable(0x0BE2)
            # Bind input textures.
            unit = 0
            if inputs:
                for u_name, src in inputs.items():
                    src_tex = src[0] if isinstance(src, tuple) else src
                    if src_tex is None:
                        continue
                    src_tex.use(unit)
                    if u_name in prog:
                        prog[u_name].value = unit
                    unit += 1
            # Set scalar uniforms.
            if uniforms:
                for name, value in uniforms.items():
                    if name not in prog:
                        continue
                    uni = prog[name]
                    if isinstance(value, (tuple, list)):
                        uni.value = tuple(float(v) for v in value)
                    else:
                        uni.value = float(value) if isinstance(value, (int, float)) else value
            vao.render()
            if blend:
                ctx.disable(0x0BE2)

    # ─── readback ───────────────────────────────────────────────
    def read_bgra_premultiplied(self, target) -> Optional[bytes]:
        """Download ``target`` as premultiplied BGRA bytes (top-down).

        Result is suitable as ``FrameBuffer.bgra_bytes`` for
        UpdateLayeredWindow without any further CPU work.
        """
        if not self._available or target is None:
            return None
        tex_src, _fbo_src = target
        w, h = tex_src.width, tex_src.height
        # Reuse a dedicated staging texture for the swizzle pass.
        out = self.tex('__bgra_out', w, h, clear=False)
        if out is None:
            return None
        self.render(
            'download_bgra', out,
            inputs={'u_tex': tex_src},
        )
        ctx = _tls.ctx
        with ctx:
            data = out[1].read(components=4, alignment=1)
        return bytes(data)

    def read_rgba(self, target) -> Optional[np.ndarray]:
        """Download ``target`` as a top-down RGBA ndarray (h, w, 4)."""
        if not self._available or target is None:
            return None
        tex_src, fbo_src = target
        w, h = tex_src.width, tex_src.height
        ctx = _tls.ctx
        with ctx:
            data = fbo_src.read(components=4, alignment=1)
        arr = np.frombuffer(data, dtype=np.uint8).reshape(h, w, 4)
        # FBO read is bottom-up; flip to top-down to match PIL convention.
        return np.flipud(arr).copy()

    def to_pil(self, target) -> Optional[Image.Image]:
        """Escape hatch: download ``target`` as a PIL RGBA image."""
        arr = self.read_rgba(target)
        if arr is None:
            return None
        return Image.fromarray(arr, 'RGBA')

    # ─── GPU-resident gaussian blur ─────────────────────────────
    def blur_tex(self, source, sigma: float, *,
                 out_tag: str = '__blur_out') -> Optional[Tuple[Any, Any]]:
        """Two-pass separable blur of an existing compositor texture.

        Reuses ``gpu_renderer``'s blur shader (no PIL roundtrip).
        Returns the (texture, fbo) for the blurred result, or None on
        failure.  ``source`` must be a (texture, fbo) tuple from this
        compositor.  Output and a temporary horizontal pass are
        allocated via the compositor's texture pool.
        """
        if not self._available or source is None or sigma <= 0.05:
            return source
        try:
            blur_prog = _tls.blur_prog
            blur_quad = _tls.blur_quad
        except AttributeError:
            return None
        ctx = _tls.ctx
        src_tex, _src_fbo = source
        w, h = src_tex.width, src_tex.height
        radius = min(64, max(1, int(round(sigma * 3.0))))
        h_pass = self.tex('__blur_h', w, h, clear=False)
        out = self.tex(out_tag, w, h, clear=False)
        if h_pass is None or out is None:
            return None
        with ctx:
            blur_prog['u_sigma'].value = float(sigma)
            blur_prog['u_radius'].value = int(radius)
            blur_prog['u_tex'].value = 0
            # Horizontal pass: src → h_pass.
            h_pass[1].use()
            ctx.viewport = (0, 0, w, h)
            ctx.disable(0x0BE2)
            ctx.clear(0.0, 0.0, 0.0, 0.0)
            src_tex.use(0)
            blur_prog['u_dir'].value = (1.0 / w, 0.0)
            blur_quad.render()
            # Vertical pass: h_pass → out.
            out[1].use()
            ctx.viewport = (0, 0, w, h)
            ctx.clear(0.0, 0.0, 0.0, 0.0)
            h_pass[0].use(0)
            blur_prog['u_dir'].value = (0.0, 1.0 / h)
            blur_quad.render()
        return out


def gpu_compositor_available() -> bool:
    """True when the current thread has a working GL context."""
    return gpu_renderer.gpu_available()
