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
        "BURST MODE READY" headline, sub-line, 3 progress bars and the small
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
from typing import Any, Dict, List, Optional, Tuple

import numpy as np
import tkinter as tk
from PIL import Image, ImageDraw, ImageFont, ImageFilter
from gpu_renderer import gaussian_blur_rgba as _gpu_blur

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

BG_DARK = (6, 18, 38, 215)
BG_MID = (12, 32, 62, 180)
TEXT_MAIN = (227, 251, 255, 250)
TEXT_TAG = (124, 235, 255, 235)
TEXT_SUB = (164, 239, 255, 220)
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


def _lerp_color(ca, cb, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(ca[i] + (cb[i] - ca[i]) * t) for i in range(4))


def _smoothstep(a: float, b: float, x: float) -> float:
    if b <= a:
        return 0.0 if x < a else 1.0
    t = (x - a) / (b - a)
    t = max(0.0, min(1.0, t))
    return t * t * (3.0 - 2.0 * t)


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

        # Static cache of caption panel (without alpha fade) for current size
        self._cap_static: Optional[Image.Image] = None
        self._cap_sig: tuple = ()

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
        self._visible = True

    def destroy(self) -> None:
        self._cancel_tick()
        self._destroy_glfx()
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
    vec2 uv = vec2(gl_FragCoord.x, u_resolution.y - gl_FragCoord.y);
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

    def _draw_glfx(self, img: Image.Image, now: float, alpha_mul: float) -> None:
        if alpha_mul <= 0.02:
            return
        width = int(self._win_w)
        height = int(self._win_h)
        if width <= 0 or height <= 0:
            return
        if not self._ensure_glfx(width, height):
            return
        self._gl_anchor = (
            _lerp(self._gl_anchor[0], self._gl_target_anchor[0], 0.16),
            _lerp(self._gl_anchor[1], self._gl_target_anchor[1], 0.16),
        )
        self._gl_label = (
            _lerp(self._gl_label[0], self._gl_target_label[0], 0.16),
            _lerp(self._gl_label[1], self._gl_target_label[1], 0.16),
        )
        self._gl_panel_size = (
            _lerp(self._gl_panel_size[0], self._gl_target_panel_size[0], 0.16),
            _lerp(self._gl_panel_size[1], self._gl_target_panel_size[1], 0.16),
        )
        try:
            glfx = self._glfx
            prog = glfx['prog']
            fbo = glfx['fbo']
            vao = glfx['vao']
            ctx = glfx['ctx']
            moderngl = glfx['moderngl']
            fbo.use()
            ctx.clear(0.0, 0.0, 0.0, 0.0)
            prog['u_resolution'].value = (float(width), float(height))
            prog['u_time'].value = float(max(0.0, now - self._show_t))
            prog['u_anchor'].value = (float(self._gl_anchor[0]), float(self._gl_anchor[1]))
            prog['u_label'].value = (float(self._gl_label[0]), float(self._gl_label[1]))
            prog['u_panel_size'].value = (float(self._gl_panel_size[0]), float(self._gl_panel_size[1]))
            prog['u_intensity'].value = float(max(0.0, min(1.0, alpha_mul)))
            prog['u_seed'].value = float(self._gl_seed)
            vao.render(mode=moderngl.TRIANGLES, vertices=3)
            raw = fbo.read(components=4, alignment=1)
            arr = np.frombuffer(raw, dtype=np.uint8).reshape(height, width, 4)
            img.alpha_composite(Image.fromarray(arr[::-1], 'RGBA'))
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
        self._active = True
        self._exiting = False
        self._show_t = now
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
        self._schedule_tick(immediate=True)

    # ──────────────────────────────────────────
    #  Tick
    # ──────────────────────────────────────────

    def _cancel_tick(self) -> None:
        if self._tick_id is not None:
            try:
                self.root.after_cancel(self._tick_id)
            except Exception:
                pass
            self._tick_id = None

    def _schedule_tick(self, immediate: bool = False) -> None:
        if self._win is None:
            return
        self._cancel_tick()
        delay = 0 if immediate else TICK_MS
        self._tick_id = self.root.after(delay, self._tick)

    def _tick(self) -> None:
        self._tick_id = None
        if self._win is None:
            return
        now = time.time()
        if not self._active:
            return

        # Auto-hide after DISPLAY_MS.
        if not self._exiting and (now - self._show_t) * 1000 >= DISPLAY_MS:
            self.hide_burst()
            return

        self._render(now)

        # Finished exit?
        if self._exiting and (now - self._exit_t) >= EXIT_DUR:
            self._active = False
            self._exiting = False
            # Clear frame to transparent and keep window alive but empty.
            try:
                empty = Image.new(
                    'RGBA', (self._win_w, self._win_h), (0, 0, 0, 0))
                _ulw_update(self._hwnd, empty,
                            self._win_x, self._win_y)
            except Exception:
                pass
            return

        self._schedule_tick()

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

    def _render(self, now: float) -> None:
        W, H = int(self._win_w), int(self._win_h)
        img = Image.new('RGBA', (W, H), (0, 0, 0, 0))

        enter_t = _ease_out_cubic(self._enter_progress(now))
        exit_t = self._exit_progress(now)
        alpha_mul = enter_t * (1.0 - exit_t)
        if alpha_mul <= 0.01:
            _ulw_update(self._hwnd, img, self._win_x, self._win_y)
            return

        # Pulse phase for the ring / orbit glow.
        pulse = 0.5 + 0.5 * math.sin((now - self._show_t) * 5.4)

        # 0) Webview-like GPU energy pass.
        self._draw_glfx(img, now, alpha_mul)

        # 1) Caption panel
        self._draw_caption(img, alpha_mul, enter_t, exit_t, now)

        # 2) Ring target
        self._draw_ring(img, alpha_mul, enter_t, exit_t, pulse)

        # 3) Beam linking ring to caption
        self._draw_beam(img, alpha_mul, enter_t, exit_t)

        _ulw_update(self._hwnd, img, self._win_x, self._win_y)

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
        m = np.asarray(mask, dtype=np.float32) / 255.0
        arr[:, :, 3] *= m

        img = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), 'RGBA')

        # Border + hairlines (drawn after clipping)
        d2 = ImageDraw.Draw(img)
        d2.polygon(pts, outline=(89, 231, 255, 165))
        d2.line([(cx + 14, cy + 30), (cx + cw - 14, cy + 30)],
                fill=(96, 229, 255, 200), width=1)
        d2.line([(cx + cw - 18 - 108, cy + ch - 15),
                 (cx + cw - 18, cy + ch - 15)],
                fill=(96, 229, 255, 210), width=1)
        return img

    def _draw_caption(self, img: Image.Image, alpha_mul: float,
                      enter_t: float, exit_t: float, now: float) -> None:
        cb = self._callout
        cx, cy, cw, ch = cb['x'], cb['y'], cb['w'], cb['h']
        sig = (cw, ch)
        if self._cap_static is None or self._cap_sig != sig:
            self._cap_static = self._build_cap_static()
            self._cap_sig = sig
        panel = self._cap_static

        # Enter: translate3d(46,-24)→(0,0), blur(12→0)
        if enter_t < 1.0:
            dx = int((1.0 - enter_t) * 46)
            dy = int((1.0 - enter_t) * -24)
        else:
            dx = dy = 0
        if exit_t > 0:
            dx += int(exit_t * 50)
            dy += int(exit_t * -12)

        # Apply alpha
        panel_rgba = panel.copy()
        if alpha_mul < 0.999:
            a = np.asarray(panel_rgba, dtype=np.uint8).copy()
            a[:, :, 3] = (a[:, :, 3].astype(np.float32) *
                          alpha_mul).astype(np.uint8)
            panel_rgba = Image.fromarray(a, 'RGBA')

        img.alpha_composite(panel_rgba, (cx + dx, cy + dy))

        # Text layer on top (uses tinted panel alpha)
        d = ImageDraw.Draw(img)
        f_tag = _load_font('sao', 12)
        f_main = _load_font('sao', 42)
        f_sub = _load_font('sao', 13)

        def _a(c, a=None):
            base = a if a is not None else c[3]
            return (c[0], c[1], c[2],
                    int(base * alpha_mul))

        d.text((cx + dx + 30, cy + dy + 12),
               'SYSTEM CALL', font=f_tag,
               fill=_a(TEXT_TAG))

        # Glow pulse for title
        title_glow = 0.5 + 0.5 * math.sin((now - self._show_t) * 2.2)
        # Draw title with a cheap glow by stacking 2 blurred copies
        title = 'BURST MODE READY'
        tw = _text_width(d, title, f_main)
        tx = cx + dx + 30
        ty = cy + dy + 36
        if alpha_mul > 0.2:
            glow_img = Image.new('RGBA', (tw + 24, 60), (0, 0, 0, 0))
            gd = ImageDraw.Draw(glow_img)
            glow_col = (97, 232, 255, int(100 + 60 * title_glow))
            gd.text((6, 2), title, font=f_main, fill=glow_col)
            glow_img = _gpu_blur(glow_img, 4)
            # Apply alpha_mul
            ga = np.asarray(glow_img, dtype=np.uint8).copy()
            ga[:, :, 3] = (ga[:, :, 3].astype(np.float32) *
                           alpha_mul).astype(np.uint8)
            img.alpha_composite(Image.fromarray(ga, 'RGBA'),
                                (tx - 6, ty - 2))
        d.text((tx, ty), title, font=f_main, fill=_a(TEXT_MAIN))
        # Text stroke: -webkit-text-stroke 0.35px rgba(182,247,255,0.26)
        stroke_col = (182, 247, 255, int(66 * alpha_mul))
        for sdx, sdy in [(-1, 0), (1, 0), (0, -1), (0, 1)]:
            d.text((tx + sdx, ty + sdy), title, font=f_main, fill=stroke_col)

        d.text((cx + dx + 30, cy + dy + 86),
               'Combat skill sequence synchronized'.upper(),
               font=f_sub, fill=_a(TEXT_SUB))

        # 3 progress bars
        bx0 = cx + dx + 30
        by = cy + dy + ch - 18
        bar_max_widths = [78, 122, 158]
        bar_alphas = [0.42, 0.62, 0.90]
        gap = 8
        for i, (mw, ba) in enumerate(zip(bar_max_widths, bar_alphas)):
            bx = bx0 + sum(bar_max_widths[:i]) + i * gap
            # Horizontal gradient cyan→gold
            bar = Image.new('RGBA', (mw, 4), (0, 0, 0, 0))
            bd = ImageDraw.Draw(bar)
            for xi in range(mw):
                tt = xi / max(1, mw - 1)
                col = _lerp_color((97, 232, 255, 40),
                                   (97, 232, 255, 235), tt * 0.7)
                col = _lerp_color(col, (255, 188, 66, 184), max(0, tt - 0.4) * 1.7)
                col = (col[0], col[1], col[2],
                       int(col[3] * ba * alpha_mul))
                bd.line([(xi, 0), (xi, 3)], fill=col)
            img.alpha_composite(bar, (bx, by))

        # Accent circle at top-right
        acx = cx + dx + cw - 22 - 29
        acy = cy + dy + 16 + 29
        ar = 29
        d.ellipse([acx - ar, acy - ar, acx + ar, acy + ar],
                  outline=(97, 231, 255, int(87 * alpha_mul)), width=1)
        d.ellipse([acx - ar + 10, acy - ar + 10, acx + ar - 10, acy + ar - 10],
                  outline=(255, 184, 72, int(148 * alpha_mul)), width=1)
        # inner bright dot
        d.ellipse([acx - 4, acy - 4, acx + 4, acy + 4],
                  fill=(200, 245, 255, int(240 * alpha_mul)))

    # ----- Ring -----

    def _draw_ring(self, img: Image.Image, alpha_mul: float,
                   enter_t: float, exit_t: float, pulse: float) -> None:
        ax, ay = self._anchor
        size = self._ring_size
        scale_enter = 0.66 + 0.34 * enter_t
        scale = scale_enter * (1.0 + 0.20 * max(0.0, exit_t))
        r_out = int(size * 0.5 * scale)
        if r_out <= 8:
            return
        pad = 28
        box = r_out * 2 + pad * 2
        cc = (box // 2, box // 2)

        # Vectorized distance field for halo + core glow
        yy, xx = np.mgrid[0:box, 0:box].astype(np.float32)
        dx = xx - cc[0]
        dy = yy - cc[1]
        dist = np.sqrt(dx * dx + dy * dy)

        # Halo — cyan radial glow peaking at r_out
        halo_breath = 0.52 + 0.40 * pulse
        halo_a = 46.0 * halo_breath * np.exp(
            -((dist - r_out) ** 2) / (28.0 * 28.0))
        halo_a = np.clip(halo_a, 0, 255)
        arr = np.zeros((box, box, 4), dtype=np.float32)
        arr[:, :, 0] = 97
        arr[:, :, 1] = 232
        arr[:, :, 2] = 255
        arr[:, :, 3] = halo_a

        # Core soft glow
        r_core = max(2, r_out - 38)
        core_a = 70.0 * (0.6 + 0.4 * pulse) * np.clip(
            1.0 - dist / max(1.0, r_core), 0.0, 1.0)
        core_mask = dist < r_core + 2
        # over-composite core (176,247,255, core_a)
        src_a = (core_a * core_mask) / 255.0
        inv = 1.0 - src_a
        arr[:, :, 0] = arr[:, :, 0] * inv + 176 * src_a
        arr[:, :, 1] = arr[:, :, 1] * inv + 247 * src_a
        arr[:, :, 2] = arr[:, :, 2] * inv + 255 * src_a
        arr[:, :, 3] = np.maximum(arr[:, :, 3], core_a * core_mask)

        layer = Image.fromarray(
            np.clip(arr, 0, 255).astype(np.uint8), 'RGBA')
        # Soft-blur the halo/core pass
        layer = _gpu_blur(layer, 3.0)

        # Ring strokes drawn sharp on top
        d = ImageDraw.Draw(layer)
        outer_a = int(235 * (0.75 + 0.25 * pulse))
        d.ellipse([cc[0] - r_out, cc[1] - r_out,
                   cc[0] + r_out, cc[1] + r_out],
                  outline=(113, 238, 255, outer_a), width=2)
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

        if alpha_mul < 0.999:
            a = np.asarray(layer, dtype=np.uint8).copy()
            a[:, :, 3] = (a[:, :, 3].astype(np.float32) *
                          alpha_mul).astype(np.uint8)
            layer = Image.fromarray(a, 'RGBA')

        img.alpha_composite(layer,
                            (int(ax - box // 2), int(ay - box // 2)))

    # ----- Beam -----

    def _draw_beam(self, img: Image.Image, alpha_mul: float,
                   enter_t: float, exit_t: float) -> None:
        ax, ay = self._anchor
        cb = self._callout
        end_x = cb['x'] + 28
        end_y = cb['y'] + cb['h'] * 0.56
        dx = end_x - ax
        dy = end_y - ay
        dist = max(140.0, math.hypot(dx, dy))
        nx = dx / dist
        ny = dy / dist
        start_x = ax + nx * (self._ring_size * 0.38)
        start_y = ay + ny * (self._ring_size * 0.38)
        ddx = end_x - start_x
        ddy = end_y - start_y
        length = max(180.0, math.hypot(ddx, ddy))
        angle = math.atan2(ddy, ddx)

        # Build a horizontal beam image then rotate.
        L = int(length)
        H = BEAM_H
        beam = Image.new('RGBA', (L, H), (0, 0, 0, 0))
        bd = ImageDraw.Draw(beam)
        # Glow 16px tall
        beam = self._fast_beam(L, H)

        # Enter: scaleX(0.18→1), alpha already through alpha_mul.
        # We ignore scaleX and just fade (beam-enter animation drives this
        # via alpha_mul anyway).
        if alpha_mul < 0.999:
            a = np.asarray(beam, dtype=np.uint8).copy()
            a[:, :, 3] = (a[:, :, 3].astype(np.float32) *
                          alpha_mul).astype(np.uint8)
            beam = Image.fromarray(a, 'RGBA')

        rot = beam.rotate(-math.degrees(angle), resample=Image.BILINEAR,
                          expand=True)
        rw, rh = rot.size
        # After rotate+expand the original center (L/2, H/2) sits at
        # (rw/2, rh/2). The beam's start point (0, H/2) is displaced
        # by (-L/2, 0) from the center; after rotating by -angle (PIL
        # convention, y-down counterclockwise) it maps to:
        rdx = -L / 2.0 * math.cos(angle)
        rdy = -L / 2.0 * math.sin(angle)
        sx_in_rot = rw / 2.0 + rdx
        sy_in_rot = rh / 2.0 + rdy
        px = int(round(start_x - sx_in_rot))
        py = int(round(start_y - sy_in_rot))
        img.alpha_composite(rot, (px, py))

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
