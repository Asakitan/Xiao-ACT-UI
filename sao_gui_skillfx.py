# -*- coding: utf-8 -*-
"""
sao_gui_skillfx.py — SAO "Burst Mode Ready" overlay (tkinter + ULW).

Pixel-level port of `web/skillfx.html`. Draws the three visible elements of
the original webview:

  * Target ring at the active skill slot (cyan outer / gold inner /
    halo / core pulse).
  * Beam line connecting the ring to the caption panel with a cyan→gold
    gradient.
    * GPU glow layer behind the callout, matching the webview's `#gl-fx`
        beam / anchor / panel energy pass when ModernGL is available.
  * Caption panel (angular clipped hexagon) with "SYSTEM CALL" tag, big
      "BRUST MODE READY" headline, sub-line, 3 progress bars and the small
    accent circle on the top-right.

60 FPS tick with static-layer caching: the caption geometry is baked once
per (width, height) and only the enter/exit alpha + pulsing glow are
redrawn per frame.

Public API (mirrors the JS surface called from `sao_gui.py`):

    BurstReadyOverlay(root, settings=None)
      .set_layout(layout)         # dict with 'window','viewport','slots'
      .show_burst(slot_index)
      .hide_burst()
      .destroy()
"""

from __future__ import annotations

import os
import time
import ctypes
import math
import threading
from typing import Any, Dict, List, Optional, Tuple

import numpy as np
import tkinter as tk
from PIL import Image, ImageDraw, ImageFont, ImageFilter
from gpu_renderer import gaussian_blur_rgba as _gpu_blur
from overlay_scheduler import get_scheduler as _get_scheduler
from overlay_render_worker import AsyncFrameWorker, FrameBuffer, run_cpu_tasks, submit_ulw_commit
from overlay_subpixel import subpixel_alpha_composite
from render_capture_sync import wait_until_capture_idle

from sao_gui_dps import (
    _ulw_update, _user32, _load_font, _pick_font, _text_width,
    _ease_out_cubic, _lerp,
    GWL_EXSTYLE, WS_EX_LAYERED, WS_EX_TOOLWINDOW, WS_EX_TOPMOST,
    WS_EX_TRANSPARENT,
)


# ═══════════════════════════════════════════════
#  Palette (RGB from skillfx.html CSS)
# ═══════════════════════════════════════════════

CYAN_HI = (113, 238, 255, 250)
CYAN_MID = (97, 232, 255, 235)
CYAN_SOFT = (97, 232, 255, 110)
CYAN_GLOW = (97, 232, 255, 70)
GOLD_HI = (255, 189, 70, 245)
GOLD_MID = (255, 188, 66, 210)
GOLD_SOFT = (255, 188, 66, 95)

BG_DARK = (6, 18, 38, 255)
BG_MID = (12, 32, 62, 180)
TEXT_MAIN = (227, 251, 255, 255)
TEXT_TAG = (124, 235, 255, 255)
TEXT_SUB = (164, 239, 255, 255)
TEXT_STROKE = (182, 247, 255, 66)

# Caption panel dimensions (CSS fixed values)
CAP_W = 460
CAP_H = 128
CAP_PAD_L = 30
CAP_PAD_R = 28
CAP_PAD_T = 20
CAP_PAD_B = 18

# Target ring defaults
RING_SIZE_DEFAULT = 176

# Beam
BEAM_H = 30

# Animation timings (ms)
ENTER_DUR = 0.60
EXIT_DUR = 0.98
DISPLAY_MS = 4300
TICK_MS = 16
IDLE_TICK_MS = 40
# v2.1.17: motion-blur sample count was the dominant cost in compose_frame.
# Each extra sample reruns the ring/beam/caption inner draw loops AND a
# `_get_ring_layer` cache lookup, which often missed during enter/exit
# because `r_out` changed every pixel. Trimming to 2 samples for enter/exit
# and 1 for steady halves the compose cost without visible quality loss.
MOTION_SAMPLES_ENTER = ((0.0, 1.0), (0.022, 0.35))
MOTION_SAMPLES_EXIT = ((0.0, 1.0), (0.018, 0.28))
MOTION_SAMPLES_STEADY = ((0.0, 1.0),)


def _lerp_color(ca, cb, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(ca[i] + (cb[i] - ca[i]) * t) for i in range(4))


def _smoothstep(a: float, b: float, x: float) -> float:
    if b <= a:
        return 0.0 if x < a else 1.0
    t = (x - a) / (b - a)
    t = max(0.0, min(1.0, t))
    return t * t * (3.0 - 2.0 * t)


def _scale_alpha_image(img: Image.Image, alpha_mul: float) -> Image.Image:
    alpha_mul = max(0.0, min(1.0, float(alpha_mul)))
    if alpha_mul >= 0.999:
        return img
    mul = int(round(alpha_mul * 255.0))
    arr = np.asarray(img, dtype=np.uint8).copy()
    arr[:, :, 3] = (arr[:, :, 3].astype(np.uint16) * mul // 255).astype(np.uint8)
    return Image.fromarray(arr, 'RGBA')


# ═══════════════════════════════════════════════
#  Overlay
# ═══════════════════════════════════════════════

class BurstReadyOverlay:
    """Animated SAO 'Burst Mode Ready' callout."""

    def __init__(self, root: tk.Tk, settings: Any = None):
        self.root = root
        self.settings = settings
        self._win: Optional[tk.Toplevel] = None
        self._hwnd: int = 0
        self._visible = False                # HWND shown
        self._active = False                 # burst-ready animating

        # Layout (game-window relative)
        self._win_x = 0
        self._win_y = 0
        self._win_w = 640
        self._win_h = 360
        self._slots: List[Dict[str, Any]] = []
        self._callout = {'x': 28, 'y': 28, 'w': CAP_W, 'h': CAP_H}

        # Active burst state
        self._slot_index = 0
        self._anchor = (0.0, 0.0)
        self._anchor_radius = 0.0
        self._ring_size = RING_SIZE_DEFAULT

        # Timing
        self._show_t = 0.0
        self._exit_t = 0.0                    # when hide was requested
        self._exiting = False

        # Tick
        self._tick_id: Optional[str] = None
        self._registered: bool = False
        self._render_worker = AsyncFrameWorker(prefer_isolation=True)

        # Static cache of caption panel (without alpha fade) for current size
        self._cap_static: Optional[Image.Image] = None
        self._cap_sig: tuple = ()
        self._cap_base_static: Optional[Image.Image] = None
        self._cap_title_static: Optional[Image.Image] = None
        self._cap_glow_static: Optional[Image.Image] = None
        self._cap_mask_static: Optional[Image.Image] = None
        self._cap_shine_static: Optional[Image.Image] = None
        self._cap_glow_offset: Tuple[int, int] = (0, 0)
        self._beam_cache_sig: tuple = ()
        self._beam_cache_img: Optional[Image.Image] = None
        self._beam_cache_pos: Tuple[int, int] = (0, 0)
        self._beam_tail_cache_sig: tuple = ()
        self._beam_tail_cache_img: Optional[Image.Image] = None
        self._ring_field_cache: Dict[int, Tuple[np.ndarray, Tuple[int, int]]] = {}
        self._ring_layer_cache: Dict[Tuple[int, int], Image.Image] = {}
        self._clear_pending_frames: int = 0
        self._pending_fb: Optional[FrameBuffer] = None
        self._warm_sig: tuple = ()

        # Web-style GPU callout energy layer.
        self._glfx = None
        self._glfx_failed = False
        self._gl_anchor = (0.0, 0.0)
        self._gl_label = (0.0, 0.0)
        self._gl_panel_size = (float(CAP_W), float(CAP_H))
        self._gl_target_anchor = (0.0, 0.0)
        self._gl_target_label = (0.0, 0.0)
        self._gl_target_panel_size = (float(CAP_W), float(CAP_H))
        self._gl_seed = 1.0
        # SkillFX renders on a pinned worker lane, so the GL energy pass can
        # stay enabled without touching the Tk thread.

    # ──────────────────────────────────────────
    #  Lifecycle
    # ──────────────────────────────────────────

    def _ensure_window(self) -> None:
        if self._win is not None:
            return
        self._win = tk.Toplevel(self.root)
        self._win.overrideredirect(True)
        self._win.attributes('-topmost', True)
        self._win.geometry(
            f'{int(self._win_w)}x{int(self._win_h)}+'
            f'{int(self._win_x)}+{int(self._win_y)}')
        self._win.update_idletasks()
        try:
            self._hwnd = _user32.GetParent(self._win.winfo_id()) or \
                self._win.winfo_id()
        except Exception:
            self._hwnd = self._win.winfo_id()
        ex = _user32.GetWindowLongW(
            ctypes.c_void_p(self._hwnd), GWL_EXSTYLE)
        # fully click-through — never interacts with the player
        _user32.SetWindowLongW(
            ctypes.c_void_p(self._hwnd), GWL_EXSTYLE,
            ex | WS_EX_LAYERED | WS_EX_TOOLWINDOW
            | WS_EX_TOPMOST | WS_EX_TRANSPARENT,
        )
        try:
            _user32.SetWindowDisplayAffinity(ctypes.c_void_p(self._hwnd), 0x00000011)
        except Exception:
            pass
        self._visible = True

    def destroy(self) -> None:
        self._cancel_tick()
        self._destroy_glfx()
        self._pending_fb = None
        try:
            self._render_worker.stop()
        except Exception:
            pass
        if self._win is not None:
            try:
                self._win.destroy()
            except Exception:
                pass
        self._win = None
        self._hwnd = 0
        self._visible = False
        self._active = False

    def _destroy_glfx(self) -> None:
        glfx = self._glfx
        self._glfx = None
        if not glfx:
            return
        for key in ('fbo', 'tex', 'vao', 'prog', 'ctx'):
            obj = glfx.get(key)
            if not obj:
                continue
            try:
                obj.release()
            except Exception:
                pass

    def _callout_target_point(self) -> Tuple[float, float]:
        cb = self._callout
        return float(cb['x'] + 28), float(cb['y'] + cb['h'] * 0.56)

    def _sync_gl_targets(self, reset: bool = False) -> None:
        label = self._callout_target_point()
        panel_size = (float(self._callout['w']), float(self._callout['h']))
        self._gl_target_anchor = (float(self._anchor[0]), float(self._anchor[1]))
        self._gl_target_label = label
        self._gl_target_panel_size = panel_size
        self._gl_seed = 1.0 + self._gl_target_anchor[0] * 0.013 + self._gl_target_anchor[1] * 0.007
        if reset or self._gl_anchor == (0.0, 0.0):
            self._gl_anchor = self._gl_target_anchor
            self._gl_label = self._gl_target_label
            self._gl_panel_size = self._gl_target_panel_size

    def _current_warmup_signature(self) -> tuple:
        slot_index = self._slot_index
        if slot_index <= 0 and self._slots:
            try:
                slot_index = int(self._slots[0].get('index', 0) or 0)
            except Exception:
                slot_index = 0
        slot_rect = {}
        for slot in self._slots:
            try:
                if int(slot.get('index', 0) or 0) == slot_index:
                    slot_rect = dict(slot.get('rect') or {})
                    break
            except Exception:
                continue
        return (
            int(self._win_w), int(self._win_h),
            int(self._callout['x']), int(self._callout['y']),
            int(self._callout['w']), int(self._callout['h']),
            int(slot_index),
            int(slot_rect.get('x', 0) or 0), int(slot_rect.get('y', 0) or 0),
            int(slot_rect.get('w', 0) or 0), int(slot_rect.get('h', 0) or 0),
        )

    def _schedule_warmup(self) -> None:
        warm_sig = self._current_warmup_signature()
        if warm_sig == self._warm_sig:
            return
        self._warm_sig = warm_sig
        try:
            self._render_worker.submit(self._warmup_frame, time.time(), 0, 0, 0)
        except Exception:
            pass

    def _warmup_frame(self, now: float) -> Image.Image:
        slot_index = self._slot_index
        if slot_index <= 0 and self._slots:
            try:
                slot_index = int(self._slots[0].get('index', 0) or 0)
            except Exception:
                slot_index = 0
        if slot_index > 0:
            self._update_anchor(slot_index)
        self._sync_gl_targets(reset=True)
        sample_specs = ((0.0, 1.0),)
        self._ensure_caption_layers()
        self._prime_render_caches(now, sample_specs)
        self._get_rotated_beam(now, 1.0, 0.0, 1.0)
        dummy = Image.new('RGBA', (int(self._win_w), int(self._win_h)), (0, 0, 0, 0))
        self._draw_glfx(dummy, now, 1.0)
        return Image.new('RGBA', (1, 1), (0, 0, 0, 0))

    def _init_glfx(self, width: int, height: int):
        try:
            import moderngl  # type: ignore
        except Exception:
            self._glfx_failed = True
            return None
        try:
            ctx = moderngl.create_standalone_context(require=330)
            prog = ctx.program(
                vertex_shader='''
#version 330
void main() {
    vec2 pos[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);
}
''',
                fragment_shader='''
#version 330
uniform vec2 u_resolution;
uniform vec2 u_origin;
uniform float u_time;
uniform vec2 u_anchor;
uniform vec2 u_label;
uniform vec2 u_panel_size;
uniform float u_intensity;
uniform float u_seed;
out vec4 fragColor;

float sdSegment(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a;
    vec2 ba = b - a;
    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 0.0001), 0.0, 1.0);
    return length(pa - ba * h);
}

float boxGlow(vec2 p, vec2 c, vec2 size) {
    vec2 d = abs(p - c) - size * 0.5;
    float outside = length(max(d, 0.0));
    float inside = min(max(d.x, d.y), 0.0);
    return exp(-(outside + abs(inside) * 0.2) * 0.035);
}

void main() {
    vec2 uv = u_origin + vec2(gl_FragCoord.x, u_resolution.y - gl_FragCoord.y);
    float lineDist = sdSegment(uv, u_anchor, u_label);
    float lineGlow = exp(-lineDist * 0.052);
    float radial = exp(-length(uv - u_anchor) * 0.030);
    float terminal = exp(-length(uv - u_label) * 0.020);
    float panel = boxGlow(uv, u_label + vec2(u_panel_size.x * 0.15, 0.0), u_panel_size);
    float scan = 0.5 + 0.5 * sin(u_time * 7.2 + lineDist * 0.15 + uv.x * 0.010 + u_seed);
    float wave = 0.5 + 0.5 * sin((uv.x + uv.y) * 0.016 - u_time * 2.6 + u_seed * 1.7);
    float grid = 0.5 + 0.5 * sin(uv.y * 0.18 - u_time * 14.0);
    float pulse = 0.5 + 0.5 * sin(u_time * 4.6 + length(uv - u_anchor) * 0.032);
    vec3 cyan = vec3(0.22, 0.93, 1.00);
    vec3 gold = vec3(1.00, 0.77, 0.23);
    float blend = clamp(0.32 + scan * 0.42 + wave * 0.24, 0.0, 1.0);
    vec3 beamColor = mix(cyan, gold, blend);
    vec3 color = vec3(0.0);
    color += beamColor * lineGlow * (0.36 + 0.54 * pulse);
    color += cyan * radial * (1.05 + 0.28 * scan);
    color += gold * terminal * (0.82 + 0.26 * wave);
    color += mix(cyan, gold, 0.28) * panel * (0.22 + 0.18 * grid);
    float alpha = clamp((lineGlow * 0.82 + radial * 0.96 + terminal * 0.72 + panel * 0.34) * u_intensity, 0.0, 0.96);
    fragColor = vec4(color, alpha);
}
''')
            vao = ctx.vertex_array(prog, [])
            tex = ctx.texture((width, height), 4)
            fbo = ctx.framebuffer(color_attachments=[tex])
            return {
                'moderngl': moderngl,
                'ctx': ctx,
                'prog': prog,
                'vao': vao,
                'tex': tex,
                'fbo': fbo,
                'size': (width, height),
            }
        except Exception:
            try:
                ctx.release()
            except Exception:
                pass
            self._glfx_failed = True
            return None

    def _ensure_glfx(self, width: int, height: int) -> bool:
        if self._glfx_failed:
            return False
        if self._glfx and self._glfx.get('size') == (width, height):
            return True
        self._destroy_glfx()
        self._glfx = self._init_glfx(width, height)
        return self._glfx is not None

    def _glfx_bounds(self) -> Optional[Tuple[int, int, int, int]]:
        width = int(self._win_w)
        height = int(self._win_h)
        if width <= 0 or height <= 0:
            return None

        ax, ay = self._gl_anchor
        lx, ly = self._gl_label
        panel_w, panel_h = self._gl_panel_size
        panel_cx = lx + panel_w * 0.15
        panel_half_w = panel_w * 0.62 + 144.0
        panel_half_h = panel_h * 0.66 + 112.0
        anchor_pad_x = max(188.0, self._ring_size * 0.92)
        anchor_pad_y = max(168.0, self._ring_size * 0.78)
        beam_pad = 156.0
        glow_margin = 96.0

        min_x = int(math.floor(min(ax - anchor_pad_x, lx - beam_pad, panel_cx - panel_half_w) - glow_margin))
        min_y = int(math.floor(min(ay - anchor_pad_y, ly - beam_pad, ly - panel_half_h) - glow_margin))
        max_x = int(math.ceil(max(ax + anchor_pad_x, lx + beam_pad, panel_cx + panel_half_w) + glow_margin))
        max_y = int(math.ceil(max(ay + anchor_pad_y, ly + beam_pad, ly + panel_half_h) + glow_margin))

        min_x = max(0, min(width, min_x))
        min_y = max(0, min(height, min_y))
        max_x = max(min_x + 1, min(width, max_x))
        max_y = max(min_y + 1, min(height, max_y))

        snap = 64
        min_x = max(0, (min_x // snap) * snap)
        min_y = max(0, (min_y // snap) * snap)
        max_x = min(width, ((max_x + snap - 1) // snap) * snap)
        max_y = min(height, ((max_y + snap - 1) // snap) * snap)

        alloc_w = max_x - min_x
        alloc_h = max_y - min_y
        if alloc_w <= 0 or alloc_h <= 0:
            return None
        return min_x, min_y, alloc_w, alloc_h

    def _draw_glfx(self, img: Image.Image, now: float, alpha_mul: float) -> None:
        if alpha_mul <= 0.02:
            return
        if threading.current_thread() is threading.main_thread():
            return
        enter_t, exit_t, _sample_alpha, _sample_pulse = self._frame_anim_state(now)
        label_target = self._caption_target_point(now, enter_t, exit_t)
        self._gl_anchor = (
            _lerp(self._gl_anchor[0], self._anchor[0], 0.18),
            _lerp(self._gl_anchor[1], self._anchor[1], 0.18),
        )
        self._gl_label = (
            _lerp(self._gl_label[0], label_target[0], 0.18),
            _lerp(self._gl_label[1], label_target[1], 0.18),
        )
        self._gl_panel_size = (
            _lerp(self._gl_panel_size[0], self._gl_target_panel_size[0], 0.16),
            _lerp(self._gl_panel_size[1], self._gl_target_panel_size[1], 0.16),
        )
        bounds = self._glfx_bounds()
        if bounds is None:
            return
        box_x, box_y, width, height = bounds
        if not self._ensure_glfx(width, height):
            return
        try:
            glfx = self._glfx
            prog = glfx['prog']
            fbo = glfx['fbo']
            vao = glfx['vao']
            ctx = glfx['ctx']
            moderngl = glfx['moderngl']
            # Re-bind our own standalone GL context as current for this
            # thread. The shared gpu_renderer owns a separate standalone
            # context on the same worker thread, and whoever was used last
            # is the one currently bound on Windows WGL. Without this, our
            # fbo/vao calls may execute against the wrong context.
            with ctx:
                fbo.use()
                ctx.viewport = (0, 0, width, height)
                ctx.clear(0.0, 0.0, 0.0, 0.0)
                prog['u_resolution'].value = (float(width), float(height))
                prog['u_origin'].value = (float(box_x), float(box_y))
                prog['u_time'].value = float(max(0.0, now - self._show_t))
                prog['u_anchor'].value = (float(self._gl_anchor[0]), float(self._gl_anchor[1]))
                prog['u_label'].value = (float(self._gl_label[0]), float(self._gl_label[1]))
                prog['u_panel_size'].value = (float(self._gl_panel_size[0]), float(self._gl_panel_size[1]))
                prog['u_intensity'].value = float(max(0.0, min(1.0, alpha_mul)))
                prog['u_seed'].value = float(self._gl_seed)
                vao.render(mode=moderngl.TRIANGLES, vertices=3)
                raw = fbo.read(components=4, alignment=1)
            gl_img = Image.frombuffer('RGBA', (width, height), raw, 'raw', 'RGBA', 0, -1)
            img.alpha_composite(gl_img, (box_x, box_y))
        except Exception:
            self._destroy_glfx()
            self._glfx_failed = True

    # ──────────────────────────────────────────
    #  Public API (mirrors the JS surface)
    # ──────────────────────────────────────────

    def set_layout(self, layout: Dict[str, Any]) -> None:
        """Update the game-window relative layout. Called whenever the
        client rect changes. `layout` follows the same shape as the
        webview's `SkillFX.setViewport` / layout payload."""
        if not isinstance(layout, dict):
            return
        win = layout.get('window') or {}
        vp = layout.get('viewport') or {}
        slots = layout.get('slots') or []
        new_x = int(win.get('x', self._win_x))
        new_y = int(win.get('y', self._win_y))
        new_w = max(200, int(win.get('w', self._win_w)))
        new_h = max(200, int(win.get('h', self._win_h)))
        moved = (new_x, new_y) != (self._win_x, self._win_y)
        resized = (new_w, new_h) != (self._win_w, self._win_h)
        self._win_x, self._win_y = new_x, new_y
        self._win_w, self._win_h = new_w, new_h
        cb = vp.get('callout') or {}
        self._callout = {
            'x': int(cb.get('x', self._callout['x'])),
            'y': int(cb.get('y', self._callout['y'])),
            'w': int(cb.get('w', CAP_W)),
            'h': int(cb.get('h', CAP_H)),
        }
        self._slots = [
            {'index': int(s.get('index', 0) or 0),
             'rect': dict(s.get('rect') or {})}
            for s in slots if isinstance(s, dict)
        ]
        if self._win is not None and (moved or resized):
            try:
                self._win.geometry(
                    f'{self._win_w}x{self._win_h}+'
                    f'{self._win_x}+{self._win_y}')
            except Exception:
                pass
            self._cap_static = None
            self._cap_sig = ()
            if resized:
                self._destroy_glfx()
        self._sync_gl_targets(reset=not self._active)
        if not self._active and self._slots:
            self._schedule_warmup()

    def show_burst(self, slot_index: int) -> None:
        slot_index = int(slot_index or 0)
        if slot_index <= 0:
            return
        self._ensure_window()
        now = time.time()
        # If already showing for same slot, just refresh position.
        if self._active and slot_index == self._slot_index \
                and not self._exiting:
            self._update_anchor(slot_index)
            self._sync_gl_targets()
            return
        self._slot_index = slot_index
        self._update_anchor(slot_index)
        try:
            self._render_worker.reset()
        except Exception:
            pass
        self._pending_fb = None
        self._active = True
        self._exiting = False
        self._clear_pending_frames = 0
        self._show_t = now
        # v2.2.16: signal combat load so the scheduler throttles idle
        # entity panels harder, freeing render-lane / CPU bandwidth for
        # the burst animation and the menu open animation.
        try:
            _get_scheduler(self.root).set_combat_load(True)
        except Exception:
            pass
        self._sync_gl_targets(reset=True)
        try:
            from sao_sound import play_sound as _play_sound
            _play_sound('burst_ready')
        except Exception:
            pass
        self._schedule_tick(immediate=True)

    def hide_burst(self) -> None:
        if not self._active or self._exiting:
            if self._active and self._exiting:
                return
            if not self._active:
                return
        self._exiting = True
        self._exit_t = time.time()
        # v2.2.16: clear combat load when burst finishes (also cleared
        # in _tick_sched when the exit animation actually completes,
        # so a re-trigger mid-exit keeps it on).
        try:
            _get_scheduler(self.root).set_combat_load(False)
        except Exception:
            pass
        self._schedule_tick(immediate=True)

    def _try_clear_window(self) -> bool:
        if not self._hwnd:
            return False
        if not wait_until_capture_idle(0.0):
            return False
        try:
            self._render_worker.reset()
        except Exception:
            pass
        self._pending_fb = None
        try:
            empty = Image.new('RGBA', (self._win_w, self._win_h), (0, 0, 0, 0))
            _ulw_update(self._hwnd, empty, self._win_x, self._win_y)
            return True
        except Exception:
            return False

    def _try_present_frame(self, fb: Optional[FrameBuffer]) -> bool:
        if not self._hwnd or fb is None:
            return False
        try:
            return bool(submit_ulw_commit(
                self._hwnd,
                fb,
                allow_during_capture=True,
            ))
        except Exception:
            return False

    # ──────────────────────────────────────────
    #  Tick
    # ──────────────────────────────────────────

    def _cancel_tick(self) -> None:
        if self._registered:
            try:
                _get_scheduler(self.root).unregister('skillfx')
            except Exception:
                pass
            self._registered = False

    def _schedule_tick(self, immediate: bool = False) -> None:
        if self._win is None:
            return
        if not self._registered:
            try:
                _get_scheduler(self.root).register(
                    'skillfx', self._tick_sched, self._is_animating,
                )
                self._registered = True
            except Exception:
                pass

    def _is_animating(self) -> bool:
        return self._active or self._clear_pending_frames > 0

    def _tick_sched(self, now: float) -> None:
        """Called by overlay_scheduler at 60 FPS."""
        if self._win is None:
            return
        if self._clear_pending_frames > 0 and not self._active:
            if self._try_clear_window():
                self._clear_pending_frames -= 1
                if self._clear_pending_frames <= 0:
                    self._cancel_tick()
            return
        if not self._active:
            return

        # Auto-hide after DISPLAY_MS.
        if not self._exiting and (now - self._show_t) * 1000 >= DISPLAY_MS:
            self.hide_burst()
            return

        if self._hwnd:
            latest_fb = self._render_worker.take_result(
                allow_during_capture=True,
            )
            if latest_fb is not None:
                self._pending_fb = latest_fb
            if self._pending_fb is not None and self._try_present_frame(self._pending_fb):
                self._pending_fb = None
            self._render_worker.submit(
                self.compose_frame, now, self._hwnd, self._win_x, self._win_y)

        # Finished exit?
        if self._exiting and (now - self._exit_t) >= EXIT_DUR:
            self._active = False
            self._exiting = False
            self._clear_pending_frames = max(self._clear_pending_frames, 4)
            self._try_clear_window()

    # ──────────────────────────────────────────
    #  Geometry
    # ──────────────────────────────────────────

    def _update_anchor(self, slot_index: int) -> None:
        slot = next((s for s in self._slots
                     if int(s.get('index', 0)) == slot_index), None)
        if slot and slot.get('rect'):
            r = slot['rect']
            x = float(r.get('x', 0)) + float(r.get('w', 0)) / 2.0
            y = float(r.get('y', 0)) + float(r.get('h', 0)) / 2.0
            radius = max(float(r.get('w', 0)), float(r.get('h', 0))) * 0.66
        else:
            x = self._win_w * 0.5
            y = self._win_h * 0.74
            radius = 42.0
        self._anchor = (x, y)
        self._anchor_radius = radius
        self._ring_size = max(160, min(224, int(radius * 3.15)))
        self._sync_gl_targets(reset=not self._active)

    # ──────────────────────────────────────────
    #  Render
    # ──────────────────────────────────────────

    def _enter_progress(self, now: float) -> float:
        dt = now - self._show_t
        return max(0.0, min(1.0, dt / ENTER_DUR))

    def _exit_progress(self, now: float) -> float:
        if not self._exiting:
            return 0.0
        dt = now - self._exit_t
        return max(0.0, min(1.0, dt / EXIT_DUR))

    def _frame_anim_state(self, now: float) -> Tuple[float, float, float, float]:
        enter_t = _ease_out_cubic(self._enter_progress(now))
        exit_t = self._exit_progress(now)
        alpha_mul = enter_t * (1.0 - exit_t)
        pulse = 0.5 + 0.5 * math.sin((now - self._show_t) * 5.4)
        return enter_t, exit_t, alpha_mul, pulse

    def _motion_sample_specs(self, now: float, enter_t: float,
                             exit_t: float) -> Tuple[Tuple[float, float], ...]:
        if exit_t > 0.01:
            return MOTION_SAMPLES_EXIT
        if enter_t < 0.995:
            return MOTION_SAMPLES_ENTER
        return MOTION_SAMPLES_STEADY

    def _prime_render_caches(
        self,
        now: float,
        sample_specs: Tuple[Tuple[float, float], ...],
    ) -> None:
        self._ensure_caption_layers()
        for sample_offset, _sample_weight in sample_specs:
            sample_enter_t, sample_exit_t, sample_alpha, sample_pulse = \
                self._frame_anim_state(now - sample_offset)
            if sample_alpha <= 0.01:
                continue
            scale_enter = 0.66 + 0.34 * sample_enter_t
            scale = scale_enter * (1.0 + 0.20 * max(0.0, sample_exit_t))
            r_out = int(self._ring_size * 0.5 * scale)
            if r_out > 8:
                self._get_ring_layer(r_out, sample_pulse)

    def _get_layer_buf(self, name: str, w: int, h: int) -> Image.Image:
        """Get-or-allocate a recycled full-screen RGBA buffer.

        v2.2.11 Phase 2: replaces the per-frame ``Image.new('RGBA', (W,H))``
        in each of compose_frame's 4 layers, eliminating ~32 MB / frame
        allocator churn at 1080p (≈2 GB/s at 60 Hz). The buffer is
        alpha-zeroed in place before return; safe because each layer name
        is rendered by a single task at a time.
        """
        cache = getattr(self, '_layer_bufs', None)
        if cache is None:
            cache = {}
            self._layer_bufs = cache
        img = cache.get(name)
        if img is None or img.size != (w, h):
            img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
            cache[name] = img
            return img
        # Zero in place (memset under the hood, ~0.05 ms for 1080p).
        img.paste((0, 0, 0, 0), (0, 0, w, h))
        return img

    def _render_ring_layer(
        self,
        now: float,
        sample_specs: Tuple[Tuple[float, float], ...],
    ) -> Optional[Image.Image]:
        layer = self._get_layer_buf('ring', int(self._win_w), int(self._win_h))
        drew = False
        for sample_offset, sample_weight in sample_specs:
            sample_now = now - sample_offset
            sample_enter_t, sample_exit_t, sample_alpha, sample_pulse = \
                self._frame_anim_state(sample_now)
            sample_alpha *= sample_weight
            if sample_alpha <= 0.01:
                continue
            self._draw_ring(layer, sample_alpha, sample_enter_t,
                            sample_exit_t, sample_pulse, sample_now)
            drew = True
        return layer if drew else None

    def _render_beam_layer(
        self,
        now: float,
        sample_specs: Tuple[Tuple[float, float], ...],
    ) -> Optional[Image.Image]:
        layer = self._get_layer_buf('beam', int(self._win_w), int(self._win_h))
        drew = False
        for sample_offset, sample_weight in sample_specs:
            sample_now = now - sample_offset
            sample_enter_t, sample_exit_t, sample_alpha, _sample_pulse = \
                self._frame_anim_state(sample_now)
            sample_alpha *= sample_weight
            if sample_alpha <= 0.01:
                continue
            self._draw_beam(layer, sample_alpha, sample_enter_t,
                            sample_exit_t, sample_now)
            drew = True
        return layer if drew else None

    def _render_caption_layer(
        self,
        now: float,
        sample_specs: Tuple[Tuple[float, float], ...],
    ) -> Optional[Image.Image]:
        layer = self._get_layer_buf('caption', int(self._win_w), int(self._win_h))
        drew = False
        for sample_offset, sample_weight in sample_specs:
            sample_now = now - sample_offset
            sample_enter_t, sample_exit_t, sample_alpha, _sample_pulse = \
                self._frame_anim_state(sample_now)
            sample_alpha *= sample_weight
            if sample_alpha <= 0.01:
                continue
            self._draw_caption(layer, sample_alpha, sample_enter_t,
                               sample_exit_t, sample_now)
            drew = True
        return layer if drew else None

    def compose_frame(self, now: float) -> Image.Image:
        W, H = int(self._win_w), int(self._win_h)
        img = self._get_layer_buf('main', W, H)

        enter_t, exit_t, alpha_mul, _pulse = self._frame_anim_state(now)
        if alpha_mul <= 0.01:
            return img

        sample_specs = self._motion_sample_specs(now, enter_t, exit_t)
        self._prime_render_caches(now, sample_specs)

        # 0) Webview-like GPU energy pass.
        self._draw_glfx(img, now, alpha_mul)

        # v2.2.10: always fan out the three independent layers across the
        # shared CPU pool — the prior `if len(sample_specs) <= 2` gate
        # collapsed steady-state SkillFX into a single thread, so on multi-
        # core boxes we were leaving 2 cores idle while compose blocked the
        # render lane (visible as menu+SkillFX combat stutter).
        ring_layer, beam_layer, caption_layer = run_cpu_tasks([
            lambda: self._render_ring_layer(now, sample_specs),
            lambda: self._render_beam_layer(now, sample_specs),
            lambda: self._render_caption_layer(now, sample_specs),
        ])
        for layer in (ring_layer, beam_layer, caption_layer):
            if layer is not None:
                img.alpha_composite(layer)

        return img

    # ----- Caption panel -----

    def _caption_clip_points(self, cx: int, cy: int,
                             cw: int, ch: int) -> List[Tuple[int, int]]:
        # Mirrors `clip-path: polygon(0 18, 26 0, 100% 0, 100% calc(100% - 18px), calc(100% - 30px) 100%, 0 100%)`
        return [
            (cx + 0,        cy + 18),
            (cx + 26,       cy + 0),
            (cx + cw,       cy + 0),
            (cx + cw,       cy + ch - 18),
            (cx + cw - 30,  cy + ch),
            (cx + 0,        cy + ch),
        ]

    def _build_cap_static(self) -> Image.Image:
        cb = self._callout
        cx, cy, cw, ch = 0, 0, cb['w'], cb['h']
        W, H = cw + 2, ch + 2
        pts = self._caption_clip_points(cx, cy, cw, ch)

        # Build a single numpy RGBA buffer — all gradients vectorized.
        xs = np.arange(W, dtype=np.float32)[None, :]     # (1, W)
        ys = np.arange(H, dtype=np.float32)[:, None]     # (H, 1)
        t_x = xs / max(1, cw - 1)

        # Background fill (BG_DARK) as base
        arr = np.zeros((H, W, 4), dtype=np.float32)
        arr[:, :, 0] = BG_DARK[0]
        arr[:, :, 1] = BG_DARK[1]
        arr[:, :, 2] = BG_DARK[2]
        arr[:, :, 3] = BG_DARK[3]

        # Left→right dark gradient alpha overlay
        a_grad = np.clip(255.0 * (0.02 + 0.82 * t_x), 0, 255)
        a_grad = np.broadcast_to(a_grad, (H, W))
        # Apply as over-composite of (6,18,38, a_grad)
        src_a = a_grad / 255.0
        inv = 1.0 - src_a
        for c, sv in zip(range(3), (6, 18, 38)):
            arr[:, :, c] = arr[:, :, c] * inv + sv * src_a
        arr[:, :, 3] = arr[:, :, 3] * inv + 255 * src_a

        # Scanline texture every 8px
        scan_mask = ((ys.astype(np.int32) % 8) == 0).astype(np.float32)
        scan_a = np.broadcast_to(scan_mask * 12.0, (H, W))
        src_a = scan_a / 255.0
        inv = 1.0 - src_a
        for c, sv in zip(range(3), (96, 229, 255)):
            arr[:, :, c] = arr[:, :, c] * inv + sv * src_a
        arr[:, :, 3] = arr[:, :, 3] * inv + 255 * src_a

        # Top cyan highlight band (y<18)
        top_band = np.clip(22.0 * (1.0 - ys / 18.0), 0, 22)
        top_band = np.where(ys < 18, top_band, 0.0)
        top_a = np.broadcast_to(top_band, (H, W))
        src_a = top_a / 255.0
        inv = 1.0 - src_a
        for c, sv in zip(range(3), (84, 220, 255)):
            arr[:, :, c] = arr[:, :, c] * inv + sv * src_a
        arr[:, :, 3] = arr[:, :, 3] * inv + 255 * src_a

        # Bottom gold tint (y>=ch-20)
        bot_y = ys - (ch - 20)
        bot_band = np.clip(20.0 * (bot_y / 20.0), 0, 20)
        bot_band = np.where(ys >= (ch - 20), bot_band, 0.0)
        bot_a = np.broadcast_to(bot_band, (H, W))
        src_a = bot_a / 255.0
        inv = 1.0 - src_a
        for c, sv in zip(range(3), (255, 188, 66)):
            arr[:, :, c] = arr[:, :, c] * inv + sv * src_a
        arr[:, :, 3] = arr[:, :, 3] * inv + 255 * src_a

        # Clip to hexagon via mask
        mask = Image.new('L', (W, H), 0)
        ImageDraw.Draw(mask).polygon(pts, fill=255)
        self._cap_mask_static = mask
        m = np.asarray(mask, dtype=np.float32) / 255.0
        arr[:, :, 3] *= m

        img = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), 'RGBA')

        # Border + hairlines — draw at 2x and downscale for antialiased edges.
        border_2x = Image.new('RGBA', (W * 2, H * 2), (0, 0, 0, 0))
        bd2 = ImageDraw.Draw(border_2x)
        pts_2x = [(x * 2, y * 2) for x, y in pts]
        bd2.polygon(pts_2x, outline=(89, 231, 255, 155))
        bd2.line([(cx * 2 + 28, cy * 2 + 60), (cw * 2 - 28, cy * 2 + 60)],
                 fill=(96, 229, 255, 190), width=1)
        bd2.line([((cw - 18 - 108) * 2, (ch - 15) * 2),
                  ((cw - 18) * 2, (ch - 15) * 2)],
                 fill=(96, 229, 255, 200), width=1)
        border_1x = border_2x.resize((W, H), Image.LANCZOS)
        img.alpha_composite(border_1x)
        return img

    def _build_caption_shine(self, ch: int) -> Image.Image:
        sw = 72
        sh = ch + 20
        xs = np.linspace(-1.0, 1.0, sw, dtype=np.float32)
        alpha = np.clip(1.0 - np.abs(xs), 0.0, 1.0) ** 1.25
        alpha = (alpha * 58.0).astype(np.uint8)
        arr = np.zeros((sh, sw, 4), dtype=np.uint8)
        arr[:, :, 0] = 180
        arr[:, :, 1] = 248
        arr[:, :, 2] = 255
        arr[:, :, 3] = np.broadcast_to(alpha[None, :], (sh, sw))
        shine = Image.fromarray(arr, 'RGBA')
        return shine.transform(
            (sw + 24, sh),
            Image.AFFINE,
            (1.0, -0.286, 0.0, 0.0, 1.0, 0.0),
            resample=Image.BICUBIC,
        )

    def _ensure_caption_layers(self) -> None:
        cb = self._callout
        sig = (cb['w'], cb['h'])
        if (self._cap_base_static is not None
                and self._cap_title_static is not None
                and self._cap_glow_static is not None
                and self._cap_mask_static is not None
                and self._cap_shine_static is not None
                and self._cap_sig == sig):
            return

        self._cap_static = self._build_cap_static()
        self._cap_sig = sig
        base = self._cap_static.copy()
        title = Image.new('RGBA', base.size, (0, 0, 0, 0))

        f_tag = _load_font('sao', 12)
        f_main = _load_font('sao', 42)
        f_sub = _load_font('sao', 13)
        draw_base = ImageDraw.Draw(base)
        draw_title = ImageDraw.Draw(title)

        draw_base.text((30, 12), 'SYSTEM CALL', font=f_tag, fill=TEXT_TAG)
        draw_base.text(
            (30, 86),
            'Combat skill sequence synchronized'.upper(),
            font=f_sub,
            fill=TEXT_SUB,
        )

        title_text = 'BRUST MODE READY'
        tx = 30
        ty = 36
        draw_title.text((tx, ty), title_text, font=f_main, fill=TEXT_MAIN)
        for sdx, sdy in [(-1, 0), (1, 0), (0, -1), (0, 1)]:
            draw_title.text(
                (tx + sdx, ty + sdy),
                title_text,
                font=f_main,
                fill=TEXT_STROKE,
            )

        bar_max_widths = [78, 122, 158]
        bar_alphas = [0.42, 0.62, 0.90]
        gap = 8
        bx0 = 30
        by = cb['h'] - 18
        for i, (mw, ba) in enumerate(zip(bar_max_widths, bar_alphas)):
            bx = bx0 + sum(bar_max_widths[:i]) + i * gap
            bar = Image.new('RGBA', (mw, 4), (0, 0, 0, 0))
            bd = ImageDraw.Draw(bar)
            for xi in range(mw):
                tt = xi / max(1, mw - 1)
                col = _lerp_color((97, 232, 255, 40),
                                   (97, 232, 255, 235), tt * 0.7)
                col = _lerp_color(col, (255, 188, 66, 184), max(0, tt - 0.4) * 1.7)
                col = (col[0], col[1], col[2], int(col[3] * ba))
                bd.line([(xi, 0), (xi, 3)], fill=col)
            base.alpha_composite(bar, (bx, by))

        acx = cb['w'] - 22 - 29
        acy = 16 + 29
        ar = 29
        draw_base.ellipse([acx - ar, acy - ar, acx + ar, acy + ar],
                          outline=(97, 231, 255, 87), width=1)
        draw_base.ellipse([acx - ar + 10, acy - ar + 10, acx + ar - 10, acy + ar - 10],
                          outline=(255, 184, 72, 148), width=1)
        draw_base.ellipse([acx - 4, acy - 4, acx + 4, acy + 4],
                          fill=(200, 245, 255, 240))

        tw = _text_width(draw_base, title_text, f_main)
        glow = Image.new('RGBA', (tw + 24, 60), (0, 0, 0, 0))
        gd = ImageDraw.Draw(glow)
        gd.text((6, 2), title_text, font=f_main, fill=(97, 232, 255, 160))
        glow = _gpu_blur(glow, 4)

        self._cap_base_static = base
        self._cap_title_static = title
        self._cap_glow_static = glow
        self._cap_shine_static = self._build_caption_shine(cb['h'])
        self._cap_glow_offset = (tx - 6, ty - 2)

    def _draw_caption(self, img: Image.Image, alpha_mul: float,
                      enter_t: float, exit_t: float, now: float) -> None:
        cb = self._callout
        cx, cy, cw, ch = cb['x'], cb['y'], cb['w'], cb['h']
        self._ensure_caption_layers()
        base = self._cap_base_static
        title = self._cap_title_static
        glow = self._cap_glow_static
        mask = self._cap_mask_static
        shine = self._cap_shine_static
        if base is None or title is None or glow is None or mask is None:
            return

        age = max(0.0, now - self._show_t)
        dx, dy = self._caption_offset(now, enter_t, exit_t)

        panel_alpha_mul = alpha_mul * (1.0 if exit_t <= 0.0 else max(0.0, 1.0 - exit_t))
        subpixel_alpha_composite(
            img, _scale_alpha_image(base, panel_alpha_mul),
            cx + dx, cy + dy,
        )

        if (not self._exiting and shine is not None and age >= 1.10
                and alpha_mul > 0.12):
            shine_phase = ((age - 1.10) % 2.90) / 2.90
            if shine_phase <= 0.20:
                shine_opacity = shine_phase / 0.20
            else:
                shine_opacity = max(0.0, 1.0 - (shine_phase - 0.20) / 0.80)
            if shine_opacity > 0.01:
                shine_layer = Image.new('RGBA', base.size, (0, 0, 0, 0))
                shine_x = -80.0 + (cw + 152.0) * shine_phase
                subpixel_alpha_composite(
                    shine_layer,
                    _scale_alpha_image(shine, alpha_mul * shine_opacity),
                    shine_x, -10.0,
                )
                shine_clip = Image.new('RGBA', base.size, (0, 0, 0, 0))
                shine_clip.paste(shine_layer, (0, 0), mask)
                subpixel_alpha_composite(img, shine_clip, cx + dx, cy + dy)

        title_glow = 0.5 + 0.5 * math.sin((now - self._show_t) * 2.2)
        if alpha_mul > 0.2:
            glow_mul = alpha_mul * ((100.0 + 60.0 * title_glow) / 160.0)
            subpixel_alpha_composite(
                img,
                _scale_alpha_image(glow, glow_mul),
                cx + dx + self._cap_glow_offset[0],
                cy + dy + self._cap_glow_offset[1],
            )
        subpixel_alpha_composite(
            img, _scale_alpha_image(title, alpha_mul), cx + dx, cy + dy,
        )

    def _caption_offset(self, now: float, enter_t: float,
                        exit_t: float) -> Tuple[float, float]:
        # v2.2.10: keep dx/dy as floats so subpixel_alpha_composite can
        # bilinear-shift the caption layer between integer pixels. The
        # previous int(round(...)) snap turned a 60-frame drift across
        # 52 px into a visibly stair-stepped slide because most ticks saw
        # zero pixel change.
        age = max(0.0, now - self._show_t)

        if enter_t < 1.0:
            if enter_t < 0.54:
                t_mid = enter_t / 0.54
                dx = _lerp(46.0, -6.0, t_mid)
                dy = _lerp(-24.0, 2.0, t_mid)
            else:
                t_settle = (enter_t - 0.54) / 0.46
                dx = _lerp(-6.0, 0.0, t_settle)
                dy = _lerp(2.0, 0.0, t_settle)
        else:
            dx = 0.0
            dy = 0.0

        if not self._exiting and age >= 1.35:
            float_phase = ((age - 1.35) / 2.8) % 2.0
            float_t = float_phase if float_phase <= 1.0 else 2.0 - float_phase
            dx += _lerp(0.0, -4.0, float_t)
            dy += _lerp(0.0, 4.0, float_t)

        if exit_t > 0:
            dx += exit_t * 50.0
            dy += exit_t * -12.0

        return dx, dy

    def _caption_target_point(self, now: float, enter_t: float,
                              exit_t: float) -> Tuple[float, float]:
        cb = self._callout
        dx, dy = self._caption_offset(now, enter_t, exit_t)
        return (
            float(cb['x'] + 28 + dx),
            float(cb['y'] + cb['h'] * 0.56 + dy),
        )

    def _get_ring_field(self, box: int) -> Tuple[np.ndarray, Tuple[int, int]]:
        cached = self._ring_field_cache.get(box)
        if cached is not None:
            return cached
        cc = (box // 2, box // 2)
        yy, xx = np.mgrid[0:box, 0:box].astype(np.float32)
        dx = xx - cc[0]
        dy = yy - cc[1]
        dist = np.sqrt(dx * dx + dy * dy)
        cached = (dist, cc)
        self._ring_field_cache[box] = cached
        return cached

    def _get_ring_layer(self, r_out: int, pulse: float) -> Image.Image:
        # v2.1.17: r_out changes by ~1 px per frame during the 0.6s ENTER
        # transition (scale 0.66 -> 1.00). The previous unit-quantized cache
        # missed on EVERY enter/exit frame, so each frame rebuilt the ring
        # via numpy + ImageDraw + a synchronous GPU blur (~5-15 ms per build,
        # times the motion-sample count). Quantizing r_out to 4 px buckets
        # and pulse to 8 levels caps cache size while keeping animation
        # smooth (the quantization step is well below pixel-snapping noise).
        r_bucket = max(8, (int(r_out) + 2) // 4 * 4)
        pulse_bucket = int(round(max(0.0, min(1.0, pulse)) * 8.0))
        key = (r_bucket, pulse_bucket)
        cached = self._ring_layer_cache.get(key)
        if cached is not None:
            return cached

        r_out = r_bucket
        pulse_q = pulse_bucket / 8.0
        pad = 28
        box = r_out * 2 + pad * 2
        dist, cc = self._get_ring_field(box)

        halo_breath = 0.52 + 0.40 * pulse_q
        halo_a = 46.0 * halo_breath * np.exp(
            -((dist - r_out) ** 2) / (28.0 * 28.0))
        halo_a = np.clip(halo_a, 0, 255)
        arr = np.zeros((box, box, 4), dtype=np.float32)
        arr[:, :, 0] = 97
        arr[:, :, 1] = 232
        arr[:, :, 2] = 255
        arr[:, :, 3] = halo_a

        r_core = max(2, r_out - 38)
        core_a = 70.0 * (0.6 + 0.4 * pulse_q) * np.clip(
            1.0 - dist / max(1.0, r_core), 0.0, 1.0)
        core_mask = dist < r_core + 2
        src_a = (core_a * core_mask) / 255.0
        inv = 1.0 - src_a
        arr[:, :, 0] = arr[:, :, 0] * inv + 176 * src_a
        arr[:, :, 1] = arr[:, :, 1] * inv + 247 * src_a
        arr[:, :, 2] = arr[:, :, 2] * inv + 255 * src_a
        arr[:, :, 3] = np.maximum(arr[:, :, 3], core_a * core_mask)

        layer = Image.fromarray(
            np.clip(arr, 0, 255).astype(np.uint8), 'RGBA')
        layer = _gpu_blur(layer, 3.0)

        d = ImageDraw.Draw(layer)
        outer_a = int(235 * (0.75 + 0.25 * pulse_q))
        d.ellipse([cc[0] - r_out, cc[1] - r_out,
                   cc[0] + r_out, cc[1] + r_out],
                  outline=(113, 238, 255, outer_a), width=1)
        r_in = max(2, r_out - 16)
        d.ellipse([cc[0] - r_in, cc[1] - r_in,
                   cc[0] + r_in, cc[1] + r_in],
                  outline=(255, 189, 70, 220), width=1)
        if r_core > 2:
            d.ellipse([cc[0] - r_core, cc[1] - r_core,
                       cc[0] + r_core, cc[1] + r_core],
                      outline=(176, 247, 255, 200), width=1)
        rb = r_out + 12
        d.arc([cc[0] - rb, cc[1] - rb, cc[0] + rb, cc[1] + rb],
              start=200, end=260,
              fill=(112, 238, 255, 220), width=1)
        d.arc([cc[0] - rb, cc[1] - rb, cc[0] + rb, cc[1] + rb],
              start=20, end=80,
              fill=(255, 188, 66, 210), width=1)

        self._ring_layer_cache[key] = layer
        return layer

    # ----- Ring -----

    def _draw_ring(self, img: Image.Image, alpha_mul: float,
                   enter_t: float, exit_t: float, pulse: float,
                   now: float) -> None:
        ax, ay = self._anchor
        size = self._ring_size
        scale_enter = 0.66 + 0.34 * enter_t
        scale = scale_enter * (1.0 + 0.20 * max(0.0, exit_t))
        r_out = int(size * 0.5 * scale)
        if r_out <= 8:
            return
        pad = 28
        box = r_out * 2 + pad * 2
        layer = _scale_alpha_image(self._get_ring_layer(r_out, pulse), alpha_mul)

        # v2.2.10: subpixel composite so the ring tracks the smoothly
        # interpolated _gl_anchor (lerp 0.18 per frame) instead of snapping
        # to whole pixels each tick.
        subpixel_alpha_composite(img, layer,
                                 ax - box / 2.0, ay - box / 2.0)

        if not self._exiting:
            sweep_t = _smoothstep(0.10, 1.35, now - self._show_t)
            if 0.0 < sweep_t < 1.0:
                dist, _ = self._get_ring_field(box)
                clip_r = max(8.0, box * 0.5 - 12.0)
                band_x = -box * 0.18 + (box * 1.30) * sweep_t
                xs = np.arange(box, dtype=np.float32)[None, :]
                band = np.exp(-((xs - band_x) / 8.5) ** 2) * 210.0
                band = np.broadcast_to(band, (box, box))
                mask = (dist <= clip_r).astype(np.float32)
                arr = np.zeros((box, box, 4), dtype=np.uint8)
                arr[:, :, 0] = 114
                arr[:, :, 1] = 238
                arr[:, :, 2] = 255
                arr[:, :, 3] = np.clip(band * mask * alpha_mul, 0, 255).astype(np.uint8)
                sweep = Image.fromarray(arr, 'RGBA')
                img.alpha_composite(sweep,
                                    (int(ax - box // 2), int(ay - box // 2)))

    # ----- Beam -----

    def _get_beam_geometry(self, now: float, enter_t: float,
                           exit_t: float, ring_scale: float) -> Optional[Dict[str, float]]:
        ax, ay = self._anchor
        end_x, end_y = self._caption_target_point(now, enter_t, exit_t)
        dx = end_x - ax
        dy = end_y - ay
        dist = max(140.0, math.hypot(dx, dy))
        if dist <= 0.001:
            return None
        ux = dx / dist
        uy = dy / dist
        start_r = self._ring_size * 0.38 * max(0.72, ring_scale)
        start_x = ax + ux * start_r
        start_y = ay + uy * start_r
        ddx = end_x - start_x
        ddy = end_y - start_y
        length = max(180.0, math.hypot(ddx, ddy))
        angle = math.atan2(ddy, ddx)
        return {
            'start_x': start_x,
            'start_y': start_y,
            'end_x': end_x,
            'end_y': end_y,
            'length': length,
            'angle': angle,
            'px': -math.sin(angle),
            'py': math.cos(angle),
        }

    def _get_rotated_beam(self, now: float, enter_t: float,
                          exit_t: float, ring_scale: float) -> Tuple[Optional[Image.Image], Tuple[float, float]]:
        geom = self._get_beam_geometry(now, enter_t, exit_t, ring_scale)
        if geom is None:
            return None, (0.0, 0.0)
        start_x = geom['start_x']
        start_y = geom['start_y']
        end_x = geom['end_x']
        end_y = geom['end_y']
        length = geom['length']
        angle = geom['angle']
        L = int(length)
        sig = (
            int(round(length)), int(round(angle * 1000.0)),
        )
        if self._beam_cache_img is not None and self._beam_cache_sig == sig:
            return self._beam_cache_img, self._beam_cache_pos

        beam = self._fast_beam(L, BEAM_H)
        rot = beam.rotate(-math.degrees(angle), resample=Image.BILINEAR, expand=True)
        rw, rh = rot.size
        rdx = -L / 2.0 * math.cos(angle)
        rdy = -L / 2.0 * math.sin(angle)
        sx_in_rot = rw / 2.0 + rdx
        sy_in_rot = rh / 2.0 + rdy
        # v2.1.17: keep float position for subpixel composite.
        pos = (start_x - sx_in_rot, start_y - sy_in_rot)

        self._beam_cache_sig = sig
        self._beam_cache_img = rot
        self._beam_cache_pos = pos
        return rot, pos

    def _get_rotated_tail(self, angle: float) -> Image.Image:
        sig = (int(round(angle * 1000.0)),)
        if self._beam_tail_cache_img is not None and self._beam_tail_cache_sig == sig:
            return self._beam_tail_cache_img

        tw = 120
        th = 10
        xs = np.linspace(-1.0, 1.0, tw, dtype=np.float32)
        ys = (np.arange(th, dtype=np.float32) - th / 2.0) / max(1.0, th / 2.8)
        alpha_x = np.clip(1.0 - np.abs(xs), 0.0, 1.0) ** 1.2
        alpha_y = np.exp(-(ys * ys))[:, None]
        arr = np.zeros((th, tw, 4), dtype=np.uint8)
        arr[:, :, 0] = 255
        arr[:, :, 1] = 255
        arr[:, :, 2] = 255
        arr[:, :, 3] = np.clip(alpha_y * alpha_x[None, :] * 235.0, 0, 255).astype(np.uint8)
        tail = _gpu_blur(Image.fromarray(arr, 'RGBA'), 2.2)
        tail = tail.rotate(-math.degrees(angle), resample=Image.BILINEAR, expand=True)
        self._beam_tail_cache_sig = sig
        self._beam_tail_cache_img = tail
        return tail

    def _draw_beam(self, img: Image.Image, alpha_mul: float,
                   enter_t: float, exit_t: float, now: float) -> None:
        scale_enter = 0.66 + 0.34 * enter_t
        ring_scale = scale_enter * (1.0 + 0.20 * max(0.0, exit_t))
        rot, pos = self._get_rotated_beam(now, enter_t, exit_t, ring_scale)
        geom = self._get_beam_geometry(now, enter_t, exit_t, ring_scale)
        if rot is None or geom is None:
            return
        # v2.2.10: subpixel composite so the beam slides smoothly.
        subpixel_alpha_composite(
            img, _scale_alpha_image(rot, alpha_mul), pos[0], pos[1])

        draw = ImageDraw.Draw(img)
        trace_wave = 0.5 + 0.5 * math.sin(max(0.0, now - self._show_t - 0.9) * (math.tau / 2.45))
        trace_alpha = alpha_mul * (0.30 + 0.36 * trace_wave)
        for offset, color in ((-4.0, (97, 232, 255)), (4.0, (255, 188, 66))):
            sx = geom['start_x'] + geom['px'] * offset
            sy = geom['start_y'] + geom['py'] * offset
            ex = geom['end_x'] + geom['px'] * offset
            ey = geom['end_y'] + geom['py'] * offset
            draw.line(
                [(sx, sy), (ex, ey)],
                fill=(color[0], color[1], color[2], int(160 * trace_alpha)),
                width=1,
            )

        if not self._exiting:
            tail_age = now - self._show_t - 0.52
            if tail_age >= 0.0:
                tail_phase = (tail_age % 1.55) / 1.55
                if tail_phase <= 0.18:
                    tail_opacity = tail_phase / 0.18
                else:
                    tail_opacity = max(0.0, 1.0 - (tail_phase - 0.18) / 0.82)
                if tail_opacity > 0.01:
                    frac = -0.10 + 1.14 * tail_phase
                    cx = geom['start_x'] + (geom['end_x'] - geom['start_x']) * frac
                    cy = geom['start_y'] + (geom['end_y'] - geom['start_y']) * frac
                    tail = self._get_rotated_tail(geom['angle'])
                    tw, th = tail.size
                    subpixel_alpha_composite(
                        img,
                        _scale_alpha_image(tail, alpha_mul * tail_opacity),
                        cx - tw / 2.0, cy - th / 2.0,
                    )

    def _fast_beam(self, L: int, H: int) -> Image.Image:
        """Vectorized linear-gradient beam with vertical glow falloff."""
        xs = np.arange(L, dtype=np.float32) / max(1, L - 1)
        ys = (np.arange(H, dtype=np.float32) - H / 2.0) / (H / 2.0)
        # Vertical falloff (gaussian-ish)
        falloff = np.exp(-(ys * ys) * 4.0)[:, None]
        # cyan→gold gradient alpha (base glow 150)
        t1 = np.clip((xs - 0.0) / 0.22, 0.0, 1.0)
        t2 = np.clip((xs - 0.35) / 0.65, 0.0, 1.0)
        # color channels
        R = 97 + (255 - 97) * t2
        G = 232 + (188 - 232) * t2
        B = 255 + (66 - 255) * t2
        A_base = 150 * (0.15 + 0.85 * t1)
        A_glow = A_base[None, :] * falloff
        # Core bright line (3px tall)
        core_mask = np.abs(np.arange(H) - H / 2.0) <= 1.5
        core_a = np.where(core_mask[:, None], 245.0, 0.0)
        core_R = np.where(core_mask[:, None],
                          97 + (255 - 97) * t2[None, :], 0)
        core_G = np.where(core_mask[:, None],
                          232 + (188 - 232) * t2[None, :], 0)
        core_B = np.where(core_mask[:, None],
                          255 + (66 - 255) * t2[None, :], 0)
        # Combine (core over glow)
        alpha = np.maximum(A_glow, core_a)
        final_R = np.where(core_mask[:, None],
                           core_R, np.broadcast_to(R[None, :], (H, L)))
        final_G = np.where(core_mask[:, None],
                           core_G, np.broadcast_to(G[None, :], (H, L)))
        final_B = np.where(core_mask[:, None],
                           core_B, np.broadcast_to(B[None, :], (H, L)))
        arr = np.stack([
            np.clip(final_R, 0, 255),
            np.clip(final_G, 0, 255),
            np.clip(final_B, 0, 255),
            np.clip(alpha, 0, 255),
        ], axis=-1).astype(np.uint8)
        return Image.fromarray(arr, 'RGBA')
