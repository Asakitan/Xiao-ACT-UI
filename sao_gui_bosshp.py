# -*- coding: utf-8 -*-
"""
sao_gui_bosshp.py — SAO Boss HP Bar overlay (tkinter + ULW)

Pixel-level port of `web/boss_hp.html` to tkinter. Drawn via PIL onto a
layered window, animated at 60 FPS:

  * Olive cover with cyan / gold corner brackets and scan-lines
  * Slanted "name plate" with diagonal cuts (clip-path approximation)
  * HP bar with skewed leading edge and lagging trail (~280 ms latency)
  * Colour-ramp fill (green → yellow → red)
  * Shield overlay with moving light sweep, fracture burst on break
  * Damage flash, break burst particles, overdrive glow
  * Break / extinction sub-bar
  * Enter / exit animations (cubic-bezier slide+fade)

Public API (kept backward-compatible with sao_gui.py):
    BossHpOverlay(root, settings=None)
    .show() / .hide() / .destroy()
    .update(data)

`data` dict keys consumed (all optional unless noted):
    active, boss_name,
    hp_pct, current_hp, total_hp, hp_source,
    shield_active, shield_pct,
    breaking_stage, extinction_pct,
    in_overdrive, invincible,
    stage_text
"""

from __future__ import annotations

import os
import sys
import time
import ctypes
import math
from typing import Any, Dict, List, Optional, Tuple

import numpy as np
import tkinter as tk
from PIL import Image, ImageDraw, ImageFont, ImageFilter
from gpu_renderer import gaussian_blur_rgba as _gpu_blur
from overlay_scheduler import get_scheduler as _get_scheduler

# Reuse ULW glue + font helpers from sao_gui_dps so we keep the same
# premultiply path and font cache.
from sao_gui_dps import (  # noqa: F401
    _ulw_update, _user32, _load_font, _pick_font, _text_width,
    _has_cjk, _ease_out_cubic, _lerp,
    GWL_EXSTYLE, WS_EX_LAYERED, WS_EX_TOOLWINDOW, WS_EX_TOPMOST,
)


# ═══════════════════════════════════════════════
#  Formatting
# ═══════════════════════════════════════════════

def _fmt_hp(v: float) -> str:
    v = float(v or 0)
    if v >= 1_000_000_000:
        return f'{v / 1_000_000_000:.2f}B'
    if v >= 1_000_000:
        return f'{v / 1_000_000:.2f}M'
    if v >= 10_000:
        return f'{v / 1_000:.1f}K'
    return f'{int(round(v)):,}'


def _mix(a: Tuple[int, int, int, int],
         b: Tuple[int, int, int, int], t: float) -> Tuple[int, int, int, int]:
    t = max(0.0, min(1.0, t))
    return (
        int(_lerp(a[0], b[0], t)),
        int(_lerp(a[1], b[1], t)),
        int(_lerp(a[2], b[2], t)),
        int(_lerp(a[3], b[3], t)),
    )


def _clip_alpha(img: Image.Image, mask: Image.Image) -> Image.Image:
    """Return `img` with its alpha multiplied by `mask` (L-mode). Used to
    clip arbitrary layers to the rounded-rect panel interior."""
    if img.size != mask.size:
        mask = mask.resize(img.size)
    a = np.asarray(img, dtype=np.uint8).copy()
    m = np.asarray(mask, dtype=np.uint16)
    a[:, :, 3] = (a[:, :, 3].astype(np.uint16) * m // 255).astype(np.uint8)
    return Image.fromarray(a, 'RGBA')


# ═══════════════════════════════════════════════
#  Overlay
# ═══════════════════════════════════════════════

class BossHpOverlay:
    """Animated SAO-styled boss HP overlay (ULW + PIL)."""

    # Canvas size (panel 560×88 + break-row 22 + shadow/fx padding)
    WIDTH = 580
    HEIGHT = 120

    # Panel (the olive cover)
    PANEL_X = 10
    PANEL_Y = 4
    PANEL_W = 560
    PANEL_H = 88

    # Boss-box (content inside the cover)
    BOX_X = PANEL_X + 16
    BOX_Y = PANEL_Y + 12
    BOX_W = 492
    BOX_H = 45

    # HP bar geometry (relative to panel origin, matches CSS
    # xt_border left:108, top:9 inside boss-box which is at 16,12).
    BAR_X = PANEL_X + 16 + 108        # 134
    BAR_Y = PANEL_Y + 12 + 9          # 25
    BAR_W = 342
    BAR_H = 23

    # Break row (below the cover — mini gauge for extinction)
    BREAK_Y = PANEL_Y + PANEL_H + 4   # just under the cover
    BREAK_H = 10

    # ── Palette (matches CSS :root) ────────────────────────────────
    COVER_A = (188, 190, 178, 255)
    COVER_B = (172, 174, 162, 255)
    COVER_EDGE = (160, 162, 150, 133)
    LINE = (218, 215, 215, 255)
    TEXT_MAIN = (67, 68, 58, 255)
    TEXT_MUTED = (97, 98, 86, 200)
    CYAN = (104, 228, 255, 158)
    GOLD_CORNER = (212, 156, 23, 138)
    GOLD_STRONG = (222, 166, 32, 255)
    RED = (239, 104, 78, 255)
    BOX_BG = (207, 208, 197, 255)
    BAR_TRACK = (162, 165, 148, 46)
    TRAIL_COLOR = (255, 255, 255, 66)

    # HP gradients (left → right)
    HP_GREEN = ((211, 234, 124, 230), (154, 211, 52, 235))
    HP_YELLOW = ((235, 238, 112, 230), (244, 250, 73, 235))
    HP_RED = ((248, 140, 122, 230), (239, 104, 78, 235))

    # Shield colours
    SHIELD_A = (72, 156, 232, 107)
    SHIELD_B = (98, 208, 255, 133)
    SHIELD_C = (212, 248, 255, 61)

    # Animation tuning (seconds)
    HP_TWEEN = 0.32
    TRAIL_TWEEN = 0.82
    TRAIL_LAG = 0.28            # delay trail catch-up on HP drop
    SHIELD_TWEEN = 0.28
    BREAK_TWEEN = 0.24
    FADE_IN = 0.32
    FADE_OUT = 0.26
    DAMAGE_FLASH = 0.45
    BREAK_BURST = 2.5
    SHIELD_BREAK = 2.5

    TICK_MS = 16          # ~60 FPS while animating
    IDLE_TICK_MS = 50

    def __init__(self, root: tk.Tk, settings: Any = None):
        self.root = root
        self.settings = settings
        self._win: Optional[tk.Toplevel] = None
        self._hwnd: int = 0
        self._visible = False
        self._destroying = False
        self._last_data: Optional[dict] = None

        # Default position: top-centre
        sw = _user32.GetSystemMetrics(0)
        self._x = (sw - self.WIDTH) // 2
        self._y = 12
        if settings is not None:
            try:
                self._x = int(settings.get('boss_hp_ov_x', self._x))
                self._y = int(settings.get('boss_hp_ov_y', self._y))
            except Exception:
                pass

        # Animated state
        self._disp_hp_pct = 1.0
        self._target_hp_pct = 1.0
        self._disp_trail_pct = 1.0
        self._target_trail_pct = 1.0
        self._trail_pending_time = 0.0  # when to start moving the trail

        self._disp_shield_pct = 0.0
        self._target_shield_pct = 0.0
        self._shield_active = False
        self._last_shield_active = False

        self._disp_break_pct = 1.0
        self._target_break_pct = 1.0
        self._breaking_stage = 0
        self._last_breaking_stage = -1

        self._in_overdrive = False
        self._invincible = False
        self._boss_name = 'Enemy'
        self._stage_text = ''
        self._current_hp = 0.0
        self._total_hp = 0.0
        self._hp_source = ''

        # FX state
        self._damage_flash_start = 0.0
        self._break_burst_start = 0.0
        self._shield_break_start = 0.0
        self._shield_light_phase = 0.0  # accumulates for light sweep
        self._last_render_t = 0.0

        # Entry / exit
        self._fade_alpha = 0.0
        self._fade_target = 0.0
        self._fade_from = 0.0
        self._fade_start = 0.0
        self._fade_duration = self.FADE_IN
        self._enter_translate = 10.0  # px, animates 10→0 on enter
        self._exiting = False
        self._hide_after_exit = False

        self._tick_after_id: Optional[str] = None
        self._registered: bool = False
        self._drag_ox = 0
        self._drag_oy = 0

        # Cached panel layers (expensive to redraw every frame).
        self._cache_cover: Optional[Image.Image] = None
        self._cache_cover_mask: Optional[Image.Image] = None
        self._cache_bar_mask: Optional[Image.Image] = None
        self._cache_panel_mask: Optional[Image.Image] = None
        # Composited static base (cover + corners + boss box + bar track).
        self._static_cache: Optional[Image.Image] = None
        self._static_y_off: int = -9999

    # ──────────────────────────────────────────
    #  Lifecycle
    # ──────────────────────────────────────────

    def show(self) -> None:
        if self._win is not None:
            return
        self._win = tk.Toplevel(self.root)
        self._win.overrideredirect(True)
        self._win.attributes('-topmost', True)
        self._win.geometry(f'{self.WIDTH}x{self.HEIGHT}+{self._x}+{self._y}')
        self._win.update_idletasks()

        try:
            self._hwnd = _user32.GetParent(self._win.winfo_id()) or \
                self._win.winfo_id()
        except Exception:
            self._hwnd = self._win.winfo_id()

        ex = _user32.GetWindowLongW(ctypes.c_void_p(self._hwnd), GWL_EXSTYLE)
        _user32.SetWindowLongW(
            ctypes.c_void_p(self._hwnd), GWL_EXSTYLE,
            ex | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        )

        self._win.bind('<Button-1>', self._on_drag_start)
        self._win.bind('<B1-Motion>', self._on_drag_move)
        self._win.bind('<ButtonRelease-1>', self._on_drag_end)

        self._visible = True
        self._destroying = False
        # Entry animation
        self._fade_from = 0.0
        self._fade_alpha = 0.0
        self._fade_target = 1.0
        self._fade_start = time.time()
        self._fade_duration = self.FADE_IN
        self._enter_translate = 10.0
        self._exiting = False
        self._hide_after_exit = False
        self._schedule_tick(immediate=True)

    def hide(self) -> None:
        # Play the exit animation then destroy on completion.
        if self._win is None:
            return
        if self._exiting:
            return
        self._exiting = True
        self._fade_from = self._fade_alpha
        self._fade_target = 0.0
        self._fade_start = time.time()
        self._fade_duration = self.FADE_OUT
        self._hide_after_exit = True
        self._schedule_tick(immediate=True)

    def destroy(self) -> None:
        self._cancel_tick()
        if self._win is not None:
            try:
                self._win.destroy()
            except Exception:
                pass
        self._win = None
        self._hwnd = 0
        self._visible = False
        self._exiting = False
        self._hide_after_exit = False

    def update(self, data: dict) -> None:
        """Ingest a snapshot; drive the animation loop."""
        if data is None:
            return
        if not data.get('active', False):
            # Boss inactive → animate out.
            if self._visible and not self._exiting:
                self.hide()
            elif not self._visible:
                # Already hidden, nothing to do.
                pass
            return

        # Cancel any pending exit — boss re-appeared.
        if self._exiting:
            self._exiting = False
            self._hide_after_exit = False
            self._fade_from = self._fade_alpha
            self._fade_target = 1.0
            self._fade_start = time.time()
            self._fade_duration = self.FADE_IN

        if not self._visible or not self._hwnd:
            self.show()

        self._last_data = data

        # ── targets ──
        name = str(data.get('boss_name') or 'Enemy').strip() or 'Enemy'
        self._boss_name = name
        self._stage_text = str(data.get('stage_text') or '')

        hp_pct = max(0.0, min(1.0, float(data.get('hp_pct') or 0)))
        if hp_pct < self._target_hp_pct - 0.001:
            # Damage taken → schedule trail catch-up after a short lag.
            self._damage_flash_start = time.time()
            self._trail_pending_time = time.time() + self.TRAIL_LAG
            self._target_trail_pct_pending = hp_pct
        else:
            # Heal or first frame → snap trail forward with the fill.
            self._target_trail_pct = hp_pct
            self._trail_pending_time = 0.0
        self._target_hp_pct = hp_pct

        self._current_hp = float(data.get('current_hp') or 0)
        self._total_hp = float(data.get('total_hp') or 0)
        self._hp_source = str(data.get('hp_source') or '')

        shield_active = bool(data.get('shield_active', False))
        shield_pct = max(0.0, min(1.0, float(data.get('shield_pct') or 0)))
        if self._last_shield_active and not shield_active:
            # Shield just broke → trigger fracture burst.
            self._shield_break_start = time.time()
        self._last_shield_active = shield_active
        self._shield_active = shield_active
        self._target_shield_pct = shield_pct if shield_active else 0.0

        stage = int(data.get('breaking_stage') or 0)
        ext_pct = max(0.0, min(1.0, float(data.get('extinction_pct') or 0)))
        # Break burst when stage transitions 1→0 (boss entered broken state)
        if (self._last_breaking_stage > 0 and stage == 0) or \
           (self._last_breaking_stage == -1 and stage == 0 and ext_pct <= 0.05):
            self._break_burst_start = time.time()
        self._last_breaking_stage = stage
        self._breaking_stage = stage
        self._target_break_pct = ext_pct

        self._in_overdrive = bool(data.get('in_overdrive', False))
        self._invincible = bool(data.get('invincible', False))

        self._schedule_tick(immediate=True)

    # ──────────────────────────────────────────
    #  Tick
    # ──────────────────────────────────────────

    def _schedule_tick(self, immediate: bool = False) -> None:
        if not self._visible or self._win is None:
            return
        if not self._registered:
            try:
                _get_scheduler(self.root).register(
                    'bosshp', self._tick, self._is_animating,
                )
                self._registered = True
            except Exception as exc:
                print(f'[BOSSHP-OV] scheduler register error: {exc}')

    def _cancel_tick(self) -> None:
        if self._registered:
            try:
                _get_scheduler(self.root).unregister('bosshp')
            except Exception:
                pass
            self._registered = False

    def _is_animating(self) -> bool:
        # Boss HP runs constant scanlines, overdrive pulses, shield sweep →
        # always animating while visible.
        return True

    def _tick(self, now: Optional[float] = None) -> None:
        if not self._visible or self._win is None:
            return
        if now is None:
            now = time.time()
        dt = min(0.1, max(0.001, now - (self._last_render_t or now)))
        self._last_render_t = now

        self._advance(now, dt)
        try:
            self._render(now)
        except Exception as e:
            print(f'[BOSSHP-OV] render error: {e}')

        if self._hide_after_exit and self._fade_alpha <= 0.01:
            self.destroy()

    def _decay_toward(self, cur: float, tgt: float, tween: float) -> float:
        if abs(cur - tgt) < 0.0005:
            return tgt
        k = 1.0 - pow(0.05, self.TICK_MS / 1000.0 / max(0.05, tween))
        return cur + (tgt - cur) * k

    def _advance(self, now: float, dt: float) -> bool:
        animating = False

        # Fade
        if abs(self._fade_alpha - self._fade_target) > 1e-3:
            t = (now - self._fade_start) / max(1e-3, self._fade_duration)
            k = _ease_out_cubic(t)
            self._fade_alpha = _lerp(self._fade_from, self._fade_target, k)
            # enter translate: 10 → 0
            if self._fade_target >= 1.0:
                self._enter_translate = max(0.0, 10.0 * (1.0 - k))
            else:
                self._enter_translate = -10.0 * k * 0.6
            if t < 1.0:
                animating = True
            else:
                self._fade_alpha = self._fade_target
                if self._fade_target >= 1.0:
                    self._enter_translate = 0.0

        # Trail lag
        if self._trail_pending_time and now >= self._trail_pending_time:
            self._target_trail_pct = getattr(self, '_target_trail_pct_pending',
                                             self._target_hp_pct)
            self._trail_pending_time = 0.0

        prev = (self._disp_hp_pct, self._disp_trail_pct, self._disp_shield_pct,
                self._disp_break_pct)
        self._disp_hp_pct = self._decay_toward(
            self._disp_hp_pct, self._target_hp_pct, self.HP_TWEEN)
        self._disp_trail_pct = self._decay_toward(
            self._disp_trail_pct, self._target_trail_pct, self.TRAIL_TWEEN)
        self._disp_shield_pct = self._decay_toward(
            self._disp_shield_pct, self._target_shield_pct, self.SHIELD_TWEEN)
        self._disp_break_pct = self._decay_toward(
            self._disp_break_pct, self._target_break_pct, self.BREAK_TWEEN)
        if prev != (self._disp_hp_pct, self._disp_trail_pct,
                    self._disp_shield_pct, self._disp_break_pct):
            animating = True

        # Shield light sweep (continuous while shield is up)
        if self._shield_active and self._disp_shield_pct > 0.01:
            # 3.2s period, matches CSS @keyframes shield-light-sweep
            self._shield_light_phase = (self._shield_light_phase + dt / 3.2) % 1.0
            animating = True

        # FX timers
        if self._damage_flash_start and \
           now - self._damage_flash_start < self.DAMAGE_FLASH:
            animating = True
        if self._break_burst_start and \
           now - self._break_burst_start < self.BREAK_BURST:
            animating = True
        if self._shield_break_start and \
           now - self._shield_break_start < self.SHIELD_BREAK:
            animating = True
        # Overdrive breathes → always animating slowly
        if self._in_overdrive:
            animating = True

        return animating

    # ──────────────────────────────────────────
    #  Rendering
    # ──────────────────────────────────────────

    def _render(self, now: float) -> None:
        if not self._hwnd:
            return
        w, h = self.WIDTH, self.HEIGHT

        # Global Y-translate for entry animation.
        y_off = int(round(-self._enter_translate))

        # ── static-layer cache (cover + corners + boss box + bar track) ──
        # Rebuild only when the translate offset changes (i.e. enter/exit
        # animation running). Saves ~1.5 ms/frame at 60 FPS.
        if self._static_cache is None or self._static_y_off != y_off:
            base = Image.new('RGBA', (w, h), (0, 0, 0, 0))
            self._draw_cover(base, y_off)
            self._draw_corners(base, y_off)
            self._draw_boss_box(base, y_off)
            self._draw_bar_track(base, y_off)
            self._static_cache = base
            self._static_y_off = y_off
        img = self._static_cache.copy()

        # ── layers ──
        # (cover / corners / boss box / bar track now baked into `img`)
        self._draw_hp_trail(img, y_off)
        self._draw_hp_fill(img, y_off, now)
        if self._shield_active or self._disp_shield_pct > 0.01:
            self._draw_shield(img, y_off, now)
        if self._shield_break_start and \
           now - self._shield_break_start < self.SHIELD_BREAK:
            self._draw_shield_break(img, y_off, now)
        if self._damage_flash_start and \
           now - self._damage_flash_start < self.DAMAGE_FLASH:
            self._draw_damage_flash(img, y_off, now)
        self._draw_name_plate_text(img, y_off)
        self._draw_hp_text(img, y_off)
        self._draw_break_row(img, y_off, now)
        if self._break_burst_start and \
           now - self._break_burst_start < self.BREAK_BURST:
            self._draw_break_burst(img, y_off, now)
        if self._in_overdrive:
            self._draw_overdrive_glow(img, y_off, now)

        # Apply global fade (alpha multiply).
        if self._fade_alpha < 0.999:
            a = np.asarray(img, dtype=np.uint8).copy()
            mul = int(max(0, min(255, self._fade_alpha * 255)))
            a[:, :, 3] = (a[:, :, 3].astype(np.uint16) * mul // 255
                          ).astype(np.uint8)
            img = Image.fromarray(a, 'RGBA')

        try:
            _ulw_update(self._hwnd, img, self._x, self._y)
        except Exception as e:
            print(f'[BOSSHP-OV] ulw error: {e}')

    # ── cover ───────────────────────────────────────────────────────

    def _build_cover_mask(self) -> Image.Image:
        mask = Image.new('L', (self.WIDTH, self.HEIGHT), 0)
        ImageDraw.Draw(mask).rounded_rectangle(
            (self.PANEL_X, self.PANEL_Y,
             self.PANEL_X + self.PANEL_W - 1,
             self.PANEL_Y + self.PANEL_H - 1),
            radius=10, fill=255,
        )
        return mask

    def _draw_cover(self, img: Image.Image, y_off: int) -> None:
        w, h = self.WIDTH, self.HEIGHT

        # Vertical gradient (175deg ≈ top→bottom with tiny lean)
        grad = np.zeros((h, 1, 4), dtype=np.uint8)
        ys = np.linspace(0, 1, h)
        ease = np.where(ys < 0.48, ys / 0.48 * 0.48, ys)  # matches 175deg stop
        grad[:, 0, 0] = (self.COVER_A[0] +
                         (self.COVER_B[0] - self.COVER_A[0]) * ease)
        grad[:, 0, 1] = (self.COVER_A[1] +
                         (self.COVER_B[1] - self.COVER_A[1]) * ease)
        grad[:, 0, 2] = (self.COVER_A[2] +
                         (self.COVER_B[2] - self.COVER_A[2]) * ease)
        grad[:, 0, 3] = 255
        cover = Image.fromarray(grad, 'RGBA').resize((w, h))

        mask = self._build_cover_mask()
        if y_off:
            mask = mask.transform(
                mask.size, Image.AFFINE, (1, 0, 0, 0, 1, -y_off),
                fillcolor=0,
            )

        # Shadow (soft blur behind the cover)
        shadow = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(shadow).rounded_rectangle(
            (self.PANEL_X + 2, self.PANEL_Y + 6 + y_off,
             self.PANEL_X + self.PANEL_W - 1,
             self.PANEL_Y + self.PANEL_H - 1 + y_off),
            radius=10, fill=(20, 24, 10, 90),
        )
        shadow = _gpu_blur(shadow, 4)
        img.alpha_composite(shadow)

        # Paste clipped gradient
        img.paste(cover, (0, 0), mask)

        # Inner highlight (top edge) + subtle scan lines
        overlay = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        od = ImageDraw.Draw(overlay)
        # scan lines (every 2 px)
        for yy in range(
                self.PANEL_Y + y_off, self.PANEL_Y + self.PANEL_H + y_off, 2):
            od.line(
                (self.PANEL_X, yy, self.PANEL_X + self.PANEL_W - 1, yy),
                fill=(255, 255, 255, 10),
            )
        # top inner highlight
        od.line(
            (self.PANEL_X + 2, self.PANEL_Y + 1 + y_off,
             self.PANEL_X + self.PANEL_W - 3, self.PANEL_Y + 1 + y_off),
            fill=(255, 255, 255, 56),
        )
        img.alpha_composite(_clip_alpha(overlay, mask))

        # Border
        draw = ImageDraw.Draw(img, 'RGBA')
        draw.rounded_rectangle(
            (self.PANEL_X, self.PANEL_Y + y_off,
             self.PANEL_X + self.PANEL_W - 1,
             self.PANEL_Y + self.PANEL_H - 1 + y_off),
            radius=10, outline=self.COVER_EDGE, width=1,
        )

    def _draw_corners(self, img: Image.Image, y_off: int) -> None:
        draw = ImageDraw.Draw(img, 'RGBA')
        # Top-left cyan bracket (L-shape 16×16, 1.5 px stroke)
        tlx = self.PANEL_X + 2
        tly = self.PANEL_Y + 2 + y_off
        draw.line((tlx, tly, tlx + 16, tly), fill=self.CYAN, width=2)
        draw.line((tlx, tly, tlx, tly + 16), fill=self.CYAN, width=2)
        # Bottom-right gold bracket
        brx = self.PANEL_X + self.PANEL_W - 3
        bry = self.PANEL_Y + self.PANEL_H - 3 + y_off
        draw.line((brx, bry, brx - 16, bry), fill=self.GOLD_CORNER, width=2)
        draw.line((brx, bry, brx, bry - 16), fill=self.GOLD_CORNER, width=2)

    # ── boss-box name plate ─────────────────────────────────────────

    def _draw_boss_box(self, img: Image.Image, y_off: int) -> None:
        """The little tab on the left (xt_left) + the name bar (xt_right)
        with angular cuts. The CSS clip-path is intricate; we approximate
        with two filled polygons and a matching stroke."""
        draw = ImageDraw.Draw(img, 'RGBA')
        bx = self.BOX_X
        by = self.BOX_Y + y_off
        # xt_left — a 26×45 block with a U-notch carved out of the right half
        # Polygon (clockwise): top-left → along top → into notch → back out
        left_poly = [
            (bx, by),
            (bx + 26, by),
            (bx + 26, by + self.BOX_H),
            (bx, by + self.BOX_H),
            (bx, by + int(self.BOX_H * 0.75)),
            (bx + 13, by + int(self.BOX_H * 0.75)),
            (bx + 13, by + int(self.BOX_H * 0.25)),
            (bx, by + int(self.BOX_H * 0.25)),
        ]
        draw.polygon(left_poly, fill=self.BOX_BG)

        # xt_right — name bar: from x=29 to end of box, only the upper
        # section is opaque; the bottom has an angled cut.
        # Simplified polygon: rectangular top 0..22%, slanted cut 77..100 etc.
        rx0 = bx + 29
        rx1 = bx + self.BOX_W
        # Top bar (name area) – full opacity from rx0 to the diagonal
        # notch at around 228..234 px from rx0. The CSS clip keeps
        # top 0..22% and has a lower tab.
        top_h = int(self.BOX_H * 0.22)
        # Upper rounded region matching the tab
        # Upper rectangle
        # Fade the right half of the name bar to match the CSS
        # linear-gradient(to right,bgColor 50%, transparent)
        grad = np.zeros((top_h, rx1 - rx0, 4), dtype=np.uint8)
        xs = np.linspace(0, 1, rx1 - rx0)
        alpha = np.clip((0.5 - xs) / 0.5, 0, 1) * 255 + 0.0
        alpha = np.where(xs < 0.5, 255, alpha)
        grad[:, :, 0] = self.BOX_BG[0]
        grad[:, :, 1] = self.BOX_BG[1]
        grad[:, :, 2] = self.BOX_BG[2]
        grad[:, :, 3] = alpha.astype(np.uint8)
        img.alpha_composite(Image.fromarray(grad, 'RGBA'), (rx0, by))

        # Bottom tab (HP-text backer) — narrow strip on the right-third
        # between 60%..77% of box height, carved with diagonal
        tab_top = by + int(self.BOX_H * 0.60)
        tab_bot = by + int(self.BOX_H * 0.77)
        tab_x0 = bx + 228 + 29 - 29  # align with CSS 233px offset within box
        tab_x1 = rx1
        # The CSS clip makes a diagonal at the left edge of the tab.
        tab_poly = [
            (tab_x0, tab_top),
            (tab_x1, tab_top),
            (tab_x1, tab_bot),
            (tab_x0 - 6, tab_bot),
        ]
        # Push it inside the box boundary.
        if tab_x0 > rx0 + 60:
            draw.polygon(tab_poly, fill=self.BOX_BG)

    def _draw_name_plate_text(self, img: Image.Image, y_off: int) -> None:
        draw = ImageDraw.Draw(img, 'RGBA')
        # Boss name on top-bar (letter-spacing approximated)
        name = self._boss_name
        font = _pick_font(name, 15)
        tx = self.BOX_X + 36
        ty = self.BOX_Y + 3 + y_off
        # Keep name in main text colour; overdrive is signalled by the
        # bar glow + bg tint, not a red name (matches web behaviour).
        color = self.TEXT_MAIN
        # Truncate if too long
        max_w = 210
        name = _truncate(draw, name, font, max_w)
        draw.text((tx, ty), name, fill=color, font=font)

        # Stage marker \u2014 rendered in the top-right of the name plate so
        # it can't overlap the HP numeric readout at the bar's bottom.
        stage = self._stage_text
        if stage:
            sfont = _load_font('sao', 10)
            sw = _text_width(draw, stage, sfont)
            sx = self.BOX_X + 246 - sw
            sy = self.BOX_Y + 5 + y_off
            draw.text((sx, sy), stage,
                      fill=(97, 98, 86, 190), font=sfont)

    # ── HP bar ──────────────────────────────────────────────────────

    def _bar_mask(self) -> Image.Image:
        """The HP bar region with a diagonal cut at the right-bottom
        corner (matches xt_border clip-path). Cached — static geometry."""
        if self._cache_bar_mask is not None:
            return self._cache_bar_mask
        mask = Image.new('L', (self.WIDTH, self.HEIGHT), 0)
        d = ImageDraw.Draw(mask)
        x0 = self.BAR_X
        y0 = self.BAR_Y
        x1 = self.BAR_X + self.BAR_W
        y1 = self.BAR_Y + self.BAR_H
        # Polygon matching clip-path(0 0,100% 0,100%-5 19,145 19,141 23,0 23)
        poly = [
            (x0, y0), (x1, y0),
            (x1 - 5, y0 + 19),
            (x0 + 145, y0 + 19),
            (x0 + 141, y1),
            (x0, y1),
        ]
        d.polygon(poly, fill=255)
        self._cache_bar_mask = mask
        return mask

    def _draw_bar_track(self, img: Image.Image, y_off: int) -> None:
        track = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        td = ImageDraw.Draw(track)
        x0 = self.BAR_X
        y0 = self.BAR_Y + y_off
        x1 = self.BAR_X + self.BAR_W
        y1 = self.BAR_Y + self.BAR_H + y_off
        td.rectangle((x0, y0, x1, y1), fill=self.BAR_TRACK)
        # Top/bottom hairlines matching .tb_line
        td.line((x0, y0, x1, y0), fill=self.LINE, width=1)
        td.line((x0, y1, x1, y1), fill=self.LINE, width=1)
        # Clip to the polygon
        mask = self._bar_mask()
        if y_off:
            mask = mask.transform(mask.size, Image.AFFINE,
                                  (1, 0, 0, 0, 1, -y_off), fillcolor=0)
        img.alpha_composite(_clip_alpha(track, mask))

    def _draw_hp_trail(self, img: Image.Image, y_off: int) -> None:
        pct = max(0.0, min(1.0, self._disp_trail_pct))
        if pct <= 0:
            return
        trail = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        td = ImageDraw.Draw(trail)
        tw = int(self.BAR_W * pct)
        td.rectangle(
            (self.BAR_X, self.BAR_Y + y_off,
             self.BAR_X + tw, self.BAR_Y + self.BAR_H + y_off),
            fill=self.TRAIL_COLOR,
        )
        mask = self._bar_mask()
        if y_off:
            mask = mask.transform(mask.size, Image.AFFINE,
                                  (1, 0, 0, 0, 1, -y_off), fillcolor=0)
        img.alpha_composite(_clip_alpha(trail, mask))

    def _hp_gradient(self, pct: float) -> Tuple[Tuple[int, int, int, int],
                                                Tuple[int, int, int, int]]:
        if self._invincible:
            grey = (130, 130, 130, 220)
            return (grey, grey)
        if pct > 0.50:
            return self.HP_GREEN
        if pct > 0.25:
            return self.HP_YELLOW
        return self.HP_RED

    def _draw_hp_fill(self, img: Image.Image, y_off: int,
                      now: float) -> None:
        pct = max(0.0, min(1.0, self._disp_hp_pct))
        if pct <= 0:
            return
        fill_w = int(self.BAR_W * pct)
        if fill_w <= 0:
            return
        ca, cb = self._hp_gradient(pct)
        bar = _make_gradient_bar(fill_w, self.BAR_H, ca, cb)
        # Skewed leading edge (parallelogram) — extend ~12 px to the right
        edge = _make_skew_cap(self.BAR_H, ca, cb, skew_px=5)
        canvas = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        canvas.paste(bar, (self.BAR_X, self.BAR_Y + y_off))
        if pct < 0.995:
            canvas.paste(edge, (self.BAR_X + fill_w - 1, self.BAR_Y + y_off),
                         edge)
        mask = self._bar_mask()
        if y_off:
            mask = mask.transform(mask.size, Image.AFFINE,
                                  (1, 0, 0, 0, 1, -y_off), fillcolor=0)
        img.alpha_composite(_clip_alpha(canvas, mask))

    def _draw_shield(self, img: Image.Image, y_off: int,
                     now: float) -> None:
        pct = max(0.0, min(1.0, self._disp_shield_pct))
        if pct <= 0.005:
            return
        sw = int(self.BAR_W * pct)
        shield = _make_gradient_bar(
            sw, self.BAR_H, self.SHIELD_A, self.SHIELD_B, cc=self.SHIELD_C)
        edge = _make_skew_cap(self.BAR_H, self.SHIELD_B, self.SHIELD_C,
                              skew_px=5)

        # Moving light sweep ≈ a bright skew band that pans 0→1
        phase = self._shield_light_phase
        sweep = _make_light_sweep(sw, self.BAR_H, phase)
        shield.alpha_composite(sweep)

        canvas = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        canvas.paste(shield, (self.BAR_X, self.BAR_Y + y_off))
        canvas.paste(edge, (self.BAR_X + sw - 1, self.BAR_Y + y_off), edge)

        mask = self._bar_mask()
        if y_off:
            mask = mask.transform(mask.size, Image.AFFINE,
                                  (1, 0, 0, 0, 1, -y_off), fillcolor=0)
        img.alpha_composite(_clip_alpha(canvas, mask))

    def _draw_shield_break(self, img: Image.Image, y_off: int,
                           now: float) -> None:
        """Fracture burst played when the shield breaks (2.5 s)."""
        age = now - self._shield_break_start
        t = age / self.SHIELD_BREAK
        if t >= 1.0:
            return
        env = (1.0 - t) ** 1.8 * (0.6 + 0.4 * math.sin(t * math.pi * 3))
        a = int(160 * env)
        if a <= 4:
            return
        fx = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        fd = ImageDraw.Draw(fx)
        # Bright flash across bar
        fd.rectangle(
            (self.BAR_X, self.BAR_Y + y_off,
             self.BAR_X + self.BAR_W, self.BAR_Y + self.BAR_H + y_off),
            fill=(212, 248, 255, a),
        )
        # Crack lines: random-looking diagonals
        rng = np.random.default_rng(42)
        for _ in range(6):
            x = self.BAR_X + int(rng.integers(0, self.BAR_W))
            x2 = x + int(rng.integers(-20, 20))
            y1 = self.BAR_Y + y_off
            y2 = self.BAR_Y + self.BAR_H + y_off
            fd.line((x, y1, x2, y2),
                    fill=(255, 255, 255, min(255, a + 40)), width=1)
        mask = self._bar_mask()
        if y_off:
            mask = mask.transform(mask.size, Image.AFFINE,
                                  (1, 0, 0, 0, 1, -y_off), fillcolor=0)
        img.alpha_composite(_clip_alpha(fx, mask))

    def _draw_damage_flash(self, img: Image.Image, y_off: int,
                           now: float) -> None:
        age = now - self._damage_flash_start
        t = age / self.DAMAGE_FLASH
        if t >= 1.0:
            return
        env = (1.0 - t) ** 2
        a = int(170 * env)
        if a <= 4:
            return
        fx = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        ImageDraw.Draw(fx).rectangle(
            (self.BAR_X, self.BAR_Y + y_off,
             self.BAR_X + self.BAR_W, self.BAR_Y + self.BAR_H + y_off),
            fill=(255, 240, 240, a),
        )
        mask = self._bar_mask()
        if y_off:
            mask = mask.transform(mask.size, Image.AFFINE,
                                  (1, 0, 0, 0, 1, -y_off), fillcolor=0)
        img.alpha_composite(_clip_alpha(fx, mask))

    # ── text (HP numbers) ───────────────────────────────────────────

    def _draw_hp_text(self, img: Image.Image, y_off: int) -> None:
        draw = ImageDraw.Draw(img, 'RGBA')
        if self._hp_source == 'packet' and self._total_hp > 0:
            # Animate current_hp up/down with the pct tween.
            disp_cur = self._total_hp * self._disp_hp_pct
            text = f'{_fmt_hp(disp_cur)}/{_fmt_hp(self._total_hp)}'
        else:
            text = f'{int(round(self._disp_hp_pct * 100))}%'
        font = _load_font('sao', 12)
        tx = self.BOX_X + self.BOX_W - 14
        ty = self.BOX_Y + int(self.BOX_H * 0.82) - 3 + y_off
        tw = _text_width(draw, text, font)
        col = ((130, 130, 130, 255) if self._invincible
               else self.TEXT_MAIN)
        draw.text((tx - tw, ty), text, fill=col, font=font)

        # Source tag (small "PKT"/"EST") on the HP bar right-end
        tag = ('PKT' if self._hp_source == 'packet'
               else 'EST' if self._hp_source == 'estimate'
               else '')
        if tag:
            tfont = _load_font('sao', 9)
            tag_w = _text_width(draw, tag, tfont)
            draw.text(
                (self.BAR_X + self.BAR_W - tag_w - 4,
                 self.BAR_Y - 11 + y_off),
                tag, fill=self.TEXT_MUTED, font=tfont,
            )

    # ── break row ───────────────────────────────────────────────────

    def _draw_break_row(self, img: Image.Image, y_off: int,
                        now: float) -> None:
        x0 = self.PANEL_X + 20
        y0 = self.BREAK_Y + y_off
        track_w = self.PANEL_W - 40 - 60  # leave space for label + pct
        x1 = x0 + track_w
        draw = ImageDraw.Draw(img, 'RGBA')

        label_font = _load_font('sao', 9)
        draw.text((self.PANEL_X + 4, y0), 'BRK',
                  fill=(212, 156, 23, 209), font=label_font)

        # Track (web: bg rgba(207,208,197,0.92), border rgba(212,156,23,0.26))
        track_h = 6
        ty = y0 + (self.BREAK_H - track_h) // 2
        draw.rounded_rectangle(
            (x0, ty, x1, ty + track_h),
            radius=1, fill=(207, 208, 197, 235),
            outline=(212, 156, 23, 66), width=1,
        )
        pct = max(0.0, min(1.0, self._disp_break_pct))
        if pct > 0:
            fw = int((x1 - x0) * pct)
            bar = _make_gradient_bar(
                fw, track_h,
                (212, 170, 50, 255), (243, 195, 72, 255),
            )
            img.alpha_composite(bar, (x0, ty))
        # Percent text on the right
        pct_txt = f'{int(round(pct * 100))}%'
        tw = _text_width(draw, pct_txt, label_font)
        draw.text(
            (x1 + 8, y0), pct_txt,
            fill=(230, 200, 120, 255), font=label_font,
        )
        # Breaking stage pips
        if self._breaking_stage > 0:
            for i in range(3):
                cx = x1 - 10 - i * 10
                cy = y0 + self.BREAK_H // 2
                fill = ((255, 200, 50, 230)
                        if i < self._breaking_stage
                        else (100, 110, 120, 140))
                draw.ellipse((cx - 3, cy - 3, cx + 3, cy + 3), fill=fill)

    def _draw_break_burst(self, img: Image.Image, y_off: int,
                          now: float) -> None:
        """Radial burst + sparks when boss enters broken state."""
        age = now - self._break_burst_start
        t = age / self.BREAK_BURST
        if t >= 1.0:
            return
        env = (1.0 - t) ** 1.4
        # Radial glow centred on HP bar
        cx = self.BAR_X + self.BAR_W // 2
        cy = self.BAR_Y + self.BAR_H // 2 + y_off
        r = int(30 + 120 * t)
        glow = Image.new('RGBA', (r * 2 + 2, r * 2 + 2), (0, 0, 0, 0))
        gd = ImageDraw.Draw(glow)
        for k in range(6):
            rr = int(r * (1.0 - k / 6))
            aa = int(90 * env * (1.0 - k / 6))
            gd.ellipse(
                (r - rr, r - rr, r + rr, r + rr),
                fill=(255, 224, 148, aa),
            )
        glow = _gpu_blur(glow, 6)
        img.alpha_composite(glow, (cx - r, cy - r))

        # Sparks
        rng = np.random.default_rng(int(self._break_burst_start * 1000) % 10000)
        fx = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        fd = ImageDraw.Draw(fx)
        for _ in range(18):
            ang = rng.uniform(0, 2 * math.pi)
            dist = 20 + 160 * t + rng.uniform(-10, 10)
            px = int(cx + math.cos(ang) * dist)
            py = int(cy + math.sin(ang) * dist)
            a = int(200 * env)
            fd.ellipse((px - 1, py - 1, px + 2, py + 2),
                       fill=(255, 230, 150, a))
        img.alpha_composite(fx)

    def _draw_overdrive_glow(self, img: Image.Image, y_off: int,
                             now: float) -> None:
        """Breathing red glow around the cover when boss is overdriving."""
        phase = (now % 1.6) / 1.6
        env = 0.4 + 0.6 * (0.5 - 0.5 * math.cos(phase * 2 * math.pi))
        a = int(110 * env)
        glow = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        ImageDraw.Draw(glow).rounded_rectangle(
            (self.PANEL_X - 2, self.PANEL_Y - 2 + y_off,
             self.PANEL_X + self.PANEL_W + 1,
             self.PANEL_Y + self.PANEL_H + 1 + y_off),
            radius=12, outline=(239, 104, 78, a), width=3,
        )
        glow = _gpu_blur(glow, 3)
        img.alpha_composite(glow)

    # ──────────────────────────────────────────
    #  Dragging
    # ──────────────────────────────────────────

    def _on_drag_start(self, ev) -> None:
        try:
            self._drag_ox = ev.x_root - self._x
            self._drag_oy = ev.y_root - self._y
        except Exception:
            self._drag_ox = 0
            self._drag_oy = 0

    def _on_drag_move(self, ev) -> None:
        try:
            self._x = int(ev.x_root - self._drag_ox)
            self._y = int(ev.y_root - self._drag_oy)
            if self._win is not None:
                self._win.geometry(f'+{self._x}+{self._y}')
            self._schedule_tick(immediate=True)
        except Exception:
            pass

    def _on_drag_end(self, _ev) -> None:
        if self.settings is not None:
            try:
                self.settings.set('boss_hp_ov_x', int(self._x))
                self.settings.set('boss_hp_ov_y', int(self._y))
                save = getattr(self.settings, 'save', None)
                if callable(save):
                    save()
            except Exception:
                pass


# ═══════════════════════════════════════════════
#  Small drawing helpers shared by several methods
# ═══════════════════════════════════════════════

def _make_gradient_bar(w: int, h: int,
                       ca: Tuple[int, int, int, int],
                       cb: Tuple[int, int, int, int],
                       cc: Optional[Tuple[int, int, int, int]] = None
                       ) -> Image.Image:
    """Horizontal 2- or 3-stop linear gradient."""
    if w <= 0 or h <= 0:
        return Image.new('RGBA', (1, 1), (0, 0, 0, 0))
    xs = np.linspace(0, 1, w)[None, :]
    if cc is None:
        ts = xs
        rr = ca[0] + (cb[0] - ca[0]) * ts
        gg = ca[1] + (cb[1] - ca[1]) * ts
        bb = ca[2] + (cb[2] - ca[2]) * ts
        aa = ca[3] + (cb[3] - ca[3]) * ts
    else:
        rr = np.where(xs < 0.54,
                      ca[0] + (cb[0] - ca[0]) * (xs / 0.54),
                      cb[0] + (cc[0] - cb[0]) * ((xs - 0.54) / 0.46))
        gg = np.where(xs < 0.54,
                      ca[1] + (cb[1] - ca[1]) * (xs / 0.54),
                      cb[1] + (cc[1] - cb[1]) * ((xs - 0.54) / 0.46))
        bb = np.where(xs < 0.54,
                      ca[2] + (cb[2] - ca[2]) * (xs / 0.54),
                      cb[2] + (cc[2] - cb[2]) * ((xs - 0.54) / 0.46))
        aa = np.where(xs < 0.54,
                      ca[3] + (cb[3] - ca[3]) * (xs / 0.54),
                      cb[3] + (cc[3] - cb[3]) * ((xs - 0.54) / 0.46))
    arr = np.broadcast_to(
        np.stack([rr, gg, bb, aa], axis=-1), (h, w, 4)).copy()
    # Vertical shading (top brighter).
    ys = np.linspace(1.0, 0.82, h)[:, None, None]
    arr[:, :, :3] = np.clip(arr[:, :, :3] * ys, 0, 255)
    return Image.fromarray(arr.astype(np.uint8), 'RGBA')


def _make_skew_cap(h: int,
                   ca: Tuple[int, int, int, int],
                   cb: Tuple[int, int, int, int],
                   skew_px: int = 6) -> Image.Image:
    """Build the CSS `::after { right:-11px; skewX(-14deg) }` leading
    edge of the fill bar — a parallelogram that juts past the fill
    end to give the bar its signature angled tip."""
    w = skew_px * 2 + 2
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # Use the right-hand colour (cb) as base.
    col = (cb[0], cb[1], cb[2], cb[3])
    d.polygon(
        [(0, 0), (w, 0), (w - skew_px, h), (-skew_px, h)],
        fill=col,
    )
    return img


def _make_light_sweep(w: int, h: int, phase: float) -> Image.Image:
    """A skewed highlight band sweeping left→right over the shield fill.
    phase ∈ [0,1) controls its position."""
    if w <= 0 or h <= 0:
        return Image.new('RGBA', (max(w, 1), max(h, 1)), (0, 0, 0, 0))
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    band_w = max(24, w // 4)
    # band center x travels from -band_w to w+band_w
    cx = int(-band_w + phase * (w + 2 * band_w))
    # Peak brightness follows a sine envelope matching CSS sweep.
    peak = 0.6 + 0.4 * math.sin(phase * math.pi)
    arr = np.zeros((h, w, 4), dtype=np.uint8)
    for x in range(max(0, cx - band_w), min(w, cx + band_w)):
        d = abs(x - cx) / max(1, band_w)
        k = (1.0 - d) ** 2 * peak
        arr[:, x, 0] = 255
        arr[:, x, 1] = 255
        arr[:, x, 2] = 255
        arr[:, x, 3] = int(110 * k)
    return Image.fromarray(arr, 'RGBA')


def _truncate(draw: ImageDraw.ImageDraw, text: str, font,
              max_w: int) -> str:
    if not text:
        return ''
    if _text_width(draw, text, font) <= max_w:
        return text
    ell = '…'
    lo, hi = 0, len(text)
    while lo < hi:
        mid = (lo + hi) // 2
        if _text_width(draw, text[:mid] + ell, font) <= max_w:
            lo = mid + 1
        else:
            hi = mid
    return text[: max(0, lo - 1)] + ell
