# -*- coding: utf-8 -*-
"""
sao_gui_hp.py — SAO Player HP / STA / Identity overlay (tkinter + ULW).

Pixel-level port of `web/hp.html` to tkinter. Draws the three side-by-side
elements from the original HUD:

  * Identity Plate (left): SYSTEM | PROF · Lv.X · Name · UID · NErVGear ─
    LINK OK, with centre-overlay BOSS TIMER / SYSTEM CLOCK and the cyan /
    gold scan-line overlay animation.
  * HP Bar (right): olive "XTBox" with diagonal clip-path, skewed leading
    edge on the fill, green / yellow / red ramp, hp-bg-cover shell with
    cyan TL and gold BR corner brackets.
  * STA Bar (below HP): gold SAO stamina gauge.

60 FPS tick with aggressive static-layer caching: cover / plate / bar
geometry are baked once and only dynamic layers (fills, text, fx) are
redrawn per frame. Idle-tick back-off cuts CPU when nothing animates.

Public API (mirrors the webview's JS surface, called from sao_gui.py):
    HpOverlay(root, settings=None)
      .show() / .hide() / .destroy()
      .update_hp(current, total, level)
      .set_username(name)
      .set_player_info(info)           # {profession, uid, name}
      .set_boss_timer(text, urgency)   # '' → clock; 'urgent' → red pulse
      .update_sta(current, total)
      .set_sta_offline(offline: bool)
"""

from __future__ import annotations

import os
import time
import ctypes
import math
from typing import Any, Callable, Dict, Optional, Tuple

import numpy as np
import tkinter as tk
from PIL import Image, ImageDraw, ImageFont, ImageFilter
from gpu_renderer import gaussian_blur_rgba as _gpu_blur, render_shell_rgba as _gpu_shell
from overlay_scheduler import get_scheduler as _get_scheduler
from overlay_render_worker import AsyncFrameWorker, ulw_commit

from sao_gui_dps import (
    _ulw_update, _user32, _load_font, _pick_font, _text_width,
    _has_cjk, _ease_out_cubic, _lerp,
    GWL_EXSTYLE, WS_EX_LAYERED, WS_EX_TOOLWINDOW, WS_EX_TOPMOST,
)

# Win32 SetWindowPos constants for periodic topmost enforcement
HWND_TOPMOST = -1
SWP_NOMOVE = 0x0002
SWP_NOSIZE = 0x0001
SWP_NOACTIVATE = 0x0010
_TOPMOST_INTERVAL = 2.0  # re-assert topmost every 2 seconds


# ═══════════════════════════════════════════════
#  Constants — geometry matches web/hp.html CSS formulas
# ═══════════════════════════════════════════════
# Web hud-stage uses viewport-relative width/offset:
#   left: 4vw; width: 75vw; height: 520px.
# NOTE: the webview HP window is not fullscreen — it is sized to `sw * HUD_VW_PCT`
# and positioned at screen x = `sw * HUD_WINDOW_LEFT_PCT`. CSS `vw` therefore
# refers to the webview window width (≈ `sw * HUD_VW_PCT`), not the raw screen.
# The Tk entity overlay matches the webview by computing its stage using the
# same viewport so the ID plate / HP cover / STA bar land on identical pixels.
HUD_WINDOW_LEFT_PCT = 0.04     # webview window x = sw * 0.04
HUD_VW_PCT = 0.75              # webview window width = sw * 0.75 (= 1 viewport)
STAGE_LEFT_PCT = 0.04          # hud-stage left within viewport: 4vw
STAGE_WIDTH_PCT = 0.75         # hud-stage width within viewport: 75vw
STAGE_H = 520                  # reference stage height (hud-stage)

PANEL_SHADOW_GUTTER = 18
PANEL_H = 96                   # covers cover-top (y=446 of stage) ~ bottom
MARGIN = 8

# Identity plate — left: calc(3.2% - 25px), width: calc(39.6% + 55px),
# height: 74px, bottom: 2px of stage.
ID_X = 0
ID_W = 0
ID_H = 74
ID_Y = PANEL_H - 2 - ID_H

# HP background cover — left: calc(45.2% + 25px), width: 586,
# height: 74px, bottom: 0 of stage.
COVER_X = 0
COVER_W = 586
COVER_H = 74
COVER_Y = PANEL_H - COVER_H

# XTBox (HP bar) — left: calc(47% + 25px), width: 518,
# height: 48, bottom: 22 of stage (i.e. offset 22 up from stage bottom).
BOX_X = 0
BOX_W = 518
BOX_H = 48
BOX_Y = PANEL_H - 22 - BOX_H

# STA row — left: calc(47% + 25px), width: 500, height: 16,
# bottom: 2 of stage.
STA_X = 0
STA_W = 500
STA_H = 16
STA_Y = PANEL_H - 2 - STA_H

STAGE_W = 0
PANEL_W = 0


def _get_screen_metrics() -> Tuple[int, int]:
    try:
        sw = int(_user32.GetSystemMetrics(0))
        sh = int(_user32.GetSystemMetrics(1))
    except Exception:
        sw, sh = 1920, 1080
    return max(1, sw), max(1, sh)


def _recompute_layout(sw: Optional[int] = None) -> None:
    """Mirror the webview CSS layout using the current screen width."""
    global STAGE_W, PANEL_W, ID_X, ID_W, COVER_X, BOX_X, STA_X

    if sw is None:
        sw, _ = _get_screen_metrics()

    # The CSS `vw` unit resolves against the webview window, not the screen.
    viewport_w = int(round(sw * HUD_VW_PCT))
    STAGE_W = int(round(viewport_w * STAGE_WIDTH_PCT))
    ID_X = int(round(STAGE_W * 0.032 - 25))
    ID_W = int(round(STAGE_W * 0.396 + 55))
    COVER_X = int(round(STAGE_W * 0.452 + 25))
    BOX_X = int(round(STAGE_W * 0.47 + 25))
    STA_X = BOX_X

    right_edge = max(
        ID_X + ID_W,
        COVER_X + COVER_W,
        BOX_X + BOX_W,
        STA_X + STA_W,
    )
    PANEL_W = int(math.ceil(right_edge + PANEL_SHADOW_GUTTER))


_recompute_layout()

# Palette (RGB from hp.html CSS)
# v2.2.0: SAO Alert flat hi-tech — 纯白+略灰 (alpha 沿用)
COVER_A = (255, 255, 255, 255)
COVER_B = (234, 233, 233, 255)
COVER_BORDER = (178, 180, 182, 255)
COVER_BORDER_DEEP = (140, 142, 145, 255)
BOX_BG = (255, 255, 255, 255)
TEXT_MAIN = (100, 99, 100, 255)
TEXT_MUTED = (140, 135, 138, 255)
TEXT_UID = (140, 135, 138, 255)
TEXT_STA = (140, 135, 138, 255)
LINE = (220, 220, 220, 255)
LINE_SOFT = (250, 250, 250, 255)
HAIRLINE_LIGHT = (250, 250, 250, 255)
HAIRLINE_MID = (228, 228, 228, 255)
HAIRLINE_DARK = (140, 138, 138, 255)
CYAN = (104, 228, 255, 255)
CYAN_SOFT = (104, 228, 255, 255)
GOLD = (212, 156, 23, 255)
GOLD_SOFT = (212, 156, 23, 255)
GOLD_BRIGHT = (243, 175, 18, 210)
PROF_GOLD = (225, 165, 30, 255)
LINK_BLUE = (88, 152, 190, 255)
URGENT_RED = (255, 90, 60, 245)

# HP gradients
HP_GREEN = ((211, 234, 124, 245), (154, 211, 52, 248))
HP_YELLOW = ((235, 238, 112, 245), (244, 250, 73, 248))
HP_RED = ((248, 140, 122, 245), (239, 104, 78, 248))

# STA gradient
STA_A = (212, 170, 50, 250)
STA_B = (243, 195, 72, 250)


# ═══════════════════════════════════════════════
#  Small helpers
# ═══════════════════════════════════════════════

def _fmt_int(v: float) -> str:
    try:
        return f'{int(round(v)):,}'
    except Exception:
        return str(v)


def _draw_tracked(draw: ImageDraw.ImageDraw, xy, text: str,
                  font, fill, spacing: float = 1.0) -> int:
    """CSS letter-spacing: draw glyphs one-by-one. Returns total width."""
    x, y = xy
    x0 = x
    for i, ch in enumerate(text):
        draw.text((x, y), ch, fill=fill, font=font)
        try:
            cw = draw.textlength(ch, font=font)
        except Exception:
            cw = font.size // 2
        x += cw + (spacing if i < len(text) - 1 else 0)
    return int(x - x0)


def _tracked_width(draw: ImageDraw.ImageDraw, text: str,
                   font, spacing: float = 1.0) -> int:
    """Measure width of tracked text without drawing."""
    total = 0.0
    for i, ch in enumerate(text):
        try:
            cw = draw.textlength(ch, font=font)
        except Exception:
            cw = font.size // 2
        total += cw + (spacing if i < len(text) - 1 else 0)
    return int(total)


def _draw_text_shadow(img: Image.Image, xy, text: str, font,
                      shadow_color: Tuple[int, int, int, int],
                      blur: int = 3) -> None:
    """CSS text-shadow: render text in shadow_color, blur, composite."""
    g = Image.new('RGBA', img.size, (0, 0, 0, 0))
    ImageDraw.Draw(g).text(xy, text, fill=shadow_color, font=font)
    img.alpha_composite(_gpu_blur(g, blur))


def _clip_alpha(img: Image.Image, mask: Image.Image) -> Image.Image:
    if img.size != mask.size:
        mask = mask.resize(img.size)
    a = np.asarray(img, dtype=np.uint8).copy()
    m = np.asarray(mask, dtype=np.uint16)
    a[:, :, 3] = (a[:, :, 3].astype(np.uint16) * m // 255).astype(np.uint8)
    return Image.fromarray(a, 'RGBA')


def _multiply_alpha_regions(img: Image.Image,
                            rects: Tuple[Tuple[int, int, int, int], ...],
                            alpha: float) -> Image.Image:
    if alpha >= 0.999:
        return img
    arr = np.asarray(img, dtype=np.uint8).copy()
    h, w = arr.shape[:2]
    mul = int(max(0, min(255, alpha * 255)))
    for x0, y0, x1, y1 in rects:
        rx0 = max(0, min(w, int(x0)))
        ry0 = max(0, min(h, int(y0)))
        rx1 = max(rx0, min(w, int(x1)))
        ry1 = max(ry0, min(h, int(y1)))
        if rx1 <= rx0 or ry1 <= ry0:
            continue
        arr[ry0:ry1, rx0:rx1, 3] = (
            arr[ry0:ry1, rx0:rx1, 3].astype(np.uint16) * mul // 255
        ).astype(np.uint8)
    return Image.fromarray(arr, 'RGBA')


def _make_scanline_texture(w: int, h: int, alpha: int = 10) -> Image.Image:
    """Web-parity horizontal scan-line overlay: 2px transparent + 1px white at
    the given alpha (CSS repeating-linear-gradient 0deg 0-2px transparent,
    2-3px rgba(255,255,255,0.04))."""
    tex = np.zeros((h, w, 4), dtype=np.uint8)
    rows = np.arange(h) % 3 == 2
    tex[rows, :, :3] = 255
    tex[rows, :, 3] = alpha
    return Image.fromarray(tex, 'RGBA')


def _make_hgrad_bar(w: int, h: int,
                    ca: Tuple[int, int, int, int],
                    cb: Tuple[int, int, int, int]) -> Image.Image:
    if w <= 0 or h <= 0:
        return Image.new('RGBA', (1, 1), (0, 0, 0, 0))
    xs = np.linspace(0, 1, w)[None, :]
    rr = ca[0] + (cb[0] - ca[0]) * xs
    gg = ca[1] + (cb[1] - ca[1]) * xs
    bb = ca[2] + (cb[2] - ca[2]) * xs
    aa = ca[3] + (cb[3] - ca[3]) * xs
    arr = np.broadcast_to(
        np.stack([rr, gg, bb, aa], axis=-1), (h, w, 4)).copy()
    # subtle vertical shading to mimic box-shadow inset gradient
    ys = np.linspace(1.02, 0.88, h)[:, None, None]
    arr[:, :, :3] = np.clip(arr[:, :, :3] * ys, 0, 255)
    return Image.fromarray(arr.astype(np.uint8), 'RGBA')


def _make_skew_cap(h: int,
                   col: Tuple[int, int, int, int],
                   skew_px: int = 6,
                   extra: int = 7) -> Image.Image:
    """Build the CSS `::after { right:-11px; skewX(-14deg) }` angled tip."""
    w = max(18, skew_px * 2 + extra)
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(img).polygon(
        [(skew_px, 0), (w, 0), (w - skew_px, h), (0, h)], fill=col,
    )
    return img


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


# ═══════════════════════════════════════════════
#  Overlay
# ═══════════════════════════════════════════════

class HpOverlay:
    """Animated SAO-styled Player HP / Identity / STA overlay."""

    WIDTH = PANEL_W
    HEIGHT = PANEL_H

    # Animation tuning
    HP_TWEEN = 0.32
    STA_TWEEN = 0.28
    TICK_MS = 16          # ~60 FPS while animating
    IDLE_TICK_MS = 60
    FADE_IN = 0.60
    FADE_OUT = 0.28

    def __init__(self, root: tk.Tk, settings: Any = None,
                 on_click: Optional[Callable[[], None]] = None,
                 on_menu: Optional[Callable[[int, int], None]] = None):
        self.root = root
        self.settings = settings
        self._on_click = on_click
        self._on_menu = on_menu
        self._win: Optional[tk.Toplevel] = None
        self._hwnd: int = 0
        self._visible = False
        self._screen_sig: Optional[Tuple[int, int]] = None
        self._sync_layout()

        # Default position — matches web: hud-stage left:4vw, bottom:0.
        # Our PANEL_H is the bottom-slice of the stage (96px containing
        # cover + id-plate + XT box + STA row), so panel bottom aligns
        # exactly with screen bottom (bottom:0 in CSS).
        self._x, self._y = self._default_panel_pos()
        # Settings-cached position: only load if it was saved with the
        # current panel width so stale layouts from older geometry don't
        # re-appear after layout changes.
        if settings is not None:
            try:
                saved_w = int(settings.get('hp_ov_panel_w', 0))
                if saved_w == self.WIDTH:
                    self._x = int(settings.get('hp_ov_x', self._x))
                    self._y = int(settings.get('hp_ov_y', self._y))
            except Exception:
                pass

        # State
        self._name = 'Player'
        self._profession = ''
        self._uid = ''
        self._level = '1'
        self._hp_cur = 0.0
        self._hp_max = 0.0
        self._hp_pct_target = 1.0
        self._hp_pct_disp = 1.0
        self._sta_cur = 100.0
        self._sta_max = 100.0
        self._sta_text = '100%'
        self._sta_pct_target = 1.0
        self._sta_pct_disp = 1.0
        self._sta_offline = False
        self._sta_offline_pending = False
        self._hp_group_hidden = False
        self._boss_timer_text = ''
        self._boss_timer_urgent = False

        # Entry / exit
        self._fade_alpha = 0.0
        self._fade_target = 0.0
        self._fade_from = 0.0
        self._fade_start = 0.0
        self._fade_duration = self.FADE_IN
        self._enter_scale_t = 0.0   # 0..1 progress for slide-in
        self._exiting = False
        self._hide_after_exit = False

        # FX time bases
        self._spawn_time = 0.0
        self._last_render_t = 0.0
        self._last_hp_pct = 1.0
        self._hp_flash_start = 0.0

        self._tick_after_id: Optional[str] = None
        self._registered: bool = False
        self._drag_ox = 0
        self._drag_oy = 0
        self._last_topmost_t = 0.0  # last SetWindowPos topmost call

        # HP group auto-hide on offline (web parity: debounce + fade)
        self._offline_debounce_t = 0.0   # when offline first detected
        self._offline_hide_t = 0.0       # when hide timer started
        self._hp_group_fade_t = 0.0      # when fade-out started
        self._hp_group_restore_t = 0.0   # when restore started
        self._offline_debounce_delay = 0.10
        self._offline_hide_delay = 0.20
        self._hp_group_fade_duration = 0.50
        self._hp_group_restore_duration = 0.18

        # Static layer cache: cover + id-plate background (expensive).
        self._static_cache: Optional[Image.Image] = None
        self._static_sig: tuple = ()

        # Async render worker — compose + premult off main thread.
        self._render_worker = AsyncFrameWorker()

    def _default_panel_pos(self) -> Tuple[int, int]:
        sw, sh = _get_screen_metrics()
        # Webview HP window sits at `sw * HUD_WINDOW_LEFT_PCT`; the hud-stage
        # inside it is offset `4vw` (= `sw * HUD_VW_PCT * STAGE_LEFT_PCT`)
        # from that window's left edge. Match it on screen exactly.
        stage_screen_x = int(round(sw * HUD_WINDOW_LEFT_PCT
                                   + sw * HUD_VW_PCT * STAGE_LEFT_PCT))
        return stage_screen_x, max(0, sh - self.HEIGHT)

    def _sync_layout(self) -> None:
        """Keep the entity overlay geometry in lockstep with webview CSS."""
        sw, sh = _get_screen_metrics()
        sig = (sw, sh)
        if sig == self._screen_sig:
            return

        prev_sig = self._screen_sig
        prev_size = (getattr(self, 'WIDTH', PANEL_W), getattr(self, 'HEIGHT', PANEL_H))
        prev_default = None
        if prev_sig is not None:
            prev_stage_x = int(round(prev_sig[0] * HUD_WINDOW_LEFT_PCT
                                      + prev_sig[0] * HUD_VW_PCT * STAGE_LEFT_PCT))
            prev_default = (
                prev_stage_x,
                max(0, prev_sig[1] - prev_size[1]),
            )

        _recompute_layout(sw)
        self.WIDTH = PANEL_W
        self.HEIGHT = PANEL_H
        new_stage_x = int(round(sw * HUD_WINDOW_LEFT_PCT
                                 + sw * HUD_VW_PCT * STAGE_LEFT_PCT))
        new_default = (new_stage_x, max(0, sh - self.HEIGHT))

        if prev_sig is None or (getattr(self, '_x', None), getattr(self, '_y', None)) == prev_default:
            self._x, self._y = new_default

        self._screen_sig = sig
        self._static_cache = None
        self._static_sig = ()

        if self._win is not None and self._win.winfo_exists():
            try:
                self._win.geometry(f'{self.WIDTH}x{self.HEIGHT}+{self._x}+{self._y}')
            except Exception:
                pass

    # ──────────────────────────────────────────
    #  Lifecycle
    # ──────────────────────────────────────────

    def show(self) -> None:
        self._sync_layout()
        if self._win is not None:
            return
        self._win = tk.Toplevel(self.root)
        self._win.overrideredirect(True)
        self._win.attributes('-topmost', True)
        self._win.geometry(
            f'{self.WIDTH}x{self.HEIGHT}+{self._x}+{self._y}')
        self._win.update_idletasks()
        try:
            self._hwnd = _user32.GetParent(self._win.winfo_id()) or \
                self._win.winfo_id()
        except Exception:
            self._hwnd = self._win.winfo_id()

        ex = _user32.GetWindowLongW(
            ctypes.c_void_p(self._hwnd), GWL_EXSTYLE)
        _user32.SetWindowLongW(
            ctypes.c_void_p(self._hwnd), GWL_EXSTYLE,
            ex | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        )
        # Exclude from screen capture so vision engine never sees this overlay
        try:
            _user32.SetWindowDisplayAffinity(ctypes.c_void_p(self._hwnd), 0x00000011)
        except Exception:
            pass
        # Disable DWM non-client rendering (incl. system drop shadow).
        # _apply_panel_style() in sao_gui.py sets CS_DROPSHADOW on the Tk
        # window CLASS which affects every Toplevel in the process.  Without
        # this call the window's system shadow would linger after the ULW
        # bitmap fades to transparent (until destroy() is called).
        try:
            _ncr_disabled = ctypes.c_int(1)   # DWMNCRP_DISABLED
            ctypes.windll.dwmapi.DwmSetWindowAttribute(
                ctypes.c_void_p(self._hwnd), 2,
                ctypes.byref(_ncr_disabled), ctypes.sizeof(_ncr_disabled))
        except Exception:
            pass

        self._win.bind('<Button-1>', self._on_drag_start)
        self._win.bind('<B1-Motion>', self._on_drag_move)
        self._win.bind('<ButtonRelease-1>', self._on_drag_end)
        self._win.bind('<Button-3>', self._on_context_menu)

        self._visible = True
        self._spawn_time = time.time()
        self._fade_from = 0.0
        self._fade_alpha = 0.0
        self._fade_target = 1.0
        self._fade_start = time.time()
        self._fade_duration = self.FADE_IN
        self._enter_scale_t = 0.0
        self._exiting = False
        self._hide_after_exit = False
        self._schedule_tick(immediate=True)

    def hide(self) -> None:
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
        if hasattr(self, '_render_worker') and self._render_worker is not None:
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
        self._exiting = False
        self._hide_after_exit = False

    # ──────────────────────────────────────────
    #  Public setters (mirror the JS API)
    # ──────────────────────────────────────────

    def update_hp(self, current: float, total: float,
                  level: Any = None) -> None:
        if not self._visible:
            self.show()
        self._hp_cur = float(current or 0)
        self._hp_max = float(total or 0)
        pct = (self._hp_cur / self._hp_max) if self._hp_max > 0 else 1.0
        pct = max(0.0, min(1.0, pct))
        if pct < self._hp_pct_target - 0.002:
            self._hp_flash_start = time.time()
        self._hp_pct_target = pct
        if level is not None:
            self._level = str(level)
        self._schedule_tick(immediate=True)

    def set_username(self, name: str) -> None:
        self._name = str(name or 'Player')
        self._schedule_tick(immediate=True)

    def set_player_info(self, info: Dict[str, Any]) -> None:
        if not isinstance(info, dict):
            return
        if info.get('profession'):
            self._profession = str(info['profession'])
        if info.get('uid'):
            self._uid = f'UID {info["uid"]}'
        if info.get('name'):
            self._name = str(info['name'])
        self._schedule_tick(immediate=True)

    def set_boss_timer(self, text: str,
                       urgency: str = 'normal') -> None:
        self._boss_timer_text = str(text or '')
        self._boss_timer_urgent = (urgency == 'urgent')
        self._schedule_tick(immediate=True)

    def _format_sta_text(self) -> str:
        cur = self._sta_cur
        tot = self._sta_max
        if tot == 100 and 0 <= cur <= 100:
            return f'{int(round(cur))}%'
        return f'{int(cur)}/{int(tot)}'

    def update_sta(self, current: float, total: float) -> None:
        cur = float(current or 0)
        tot = float(total or 0)
        pct = (cur / tot) if tot > 0 else 1.0
        self._sta_pct_target = max(0.0, min(1.0, pct))
        self._sta_cur = cur
        self._sta_max = tot
        if not self._sta_offline and not self._sta_offline_pending:
            self._sta_text = self._format_sta_text()
        self._schedule_tick(immediate=True)

    def set_sta_offline(self, offline: bool) -> None:
        offline = bool(offline)
        now = time.time()
        if offline:
            if self._sta_offline or self._sta_offline_pending:
                return
            self._sta_offline_pending = True
            self._offline_debounce_t = now
            if not self._hp_group_hidden and self._hp_group_fade_t <= 0.0:
                self._offline_hide_t = now
        else:
            if (not self._sta_offline and not self._sta_offline_pending
                    and not self._hp_group_hidden and self._hp_group_fade_t <= 0.0):
                self._sta_text = self._format_sta_text()
                return
            self._sta_offline_pending = False
            self._offline_debounce_t = 0.0
            self._offline_hide_t = 0.0
            if self._sta_offline:
                self._sta_offline = False
                self._sta_text = self._format_sta_text()
            else:
                self._sta_text = self._format_sta_text()
            if self._hp_group_hidden or self._hp_group_fade_t > 0.0:
                self._hp_group_hidden = False
                self._hp_group_restore_t = now
                self._hp_group_fade_t = 0.0
        self._schedule_tick(immediate=True)

    # ──────────────────────────────────────────
    #  Tick loop
    # ──────────────────────────────────────────

    def _schedule_tick(self, immediate: bool = False) -> None:
        if not self._visible or self._win is None:
            return
        if not self._registered:
            try:
                _get_scheduler(self.root).register(
                    'hp', self._tick, self._is_animating,
                )
                self._registered = True
            except Exception as exc:
                print(f'[HP-OV] scheduler register error: {exc}')

    def _cancel_tick(self) -> None:
        if self._registered:
            try:
                _get_scheduler(self.root).unregister('hp')
            except Exception:
                pass
            self._registered = False

    def _is_animating(self) -> bool:
        now = time.time()
        if abs(self._fade_alpha - self._fade_target) > 1e-3:
            return True
        if abs(self._hp_pct_disp - self._hp_pct_target) > 4e-4:
            return True
        if abs(self._sta_pct_disp - self._sta_pct_target) > 4e-4:
            return True
        if self._sta_offline_pending or self._offline_hide_t > 0.0:
            return True
        if self._hp_group_fade_t and (now - self._hp_group_fade_t) < self._hp_group_fade_duration:
            return True
        if self._hp_group_restore_t and (now - self._hp_group_restore_t) < self._hp_group_restore_duration:
            return True
        if self._boss_timer_urgent and self._boss_timer_text:
            return True
        if self._hp_flash_start and (now - self._hp_flash_start) < 0.45:
            return True
        if self._fade_target >= 1.0 and self._enter_scale_t < 0.999:
            return True
        return False

    def _tick(self, now: Optional[float] = None) -> None:
        if not self._visible or self._win is None:
            return
        self._sync_layout()
        if now is None:
            now = time.time()
        dt = min(0.1, max(0.001, now - (self._last_render_t or now)))
        self._last_render_t = now
        self._advance(now, dt)

        # ── Async render pipeline ──
        # 1. Commit the most recent off-thread frame (if ready).
        if self._hwnd:
            fb = self._render_worker.take_result()
            if fb is not None:
                try:
                    ulw_commit(self._hwnd, fb)
                except Exception as e:
                    print(f'[HP-OV] ulw error: {e}')

            # Periodic topmost enforcement
            if now - self._last_topmost_t > _TOPMOST_INTERVAL:
                self._last_topmost_t = now
                try:
                    _user32.SetWindowPos(
                        ctypes.c_void_p(self._hwnd),
                        ctypes.c_void_p(HWND_TOPMOST),
                        0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE,
                    )
                except Exception:
                    pass

            # 2. Submit next frame for off-thread composition.
            self._render_worker.submit(
                self.compose_frame, now, self._hwnd, self._x, self._y)

        if self._hide_after_exit and self._fade_alpha <= 0.01:
            self.destroy()

    def _decay(self, cur: float, tgt: float, tween: float) -> float:
        if abs(cur - tgt) < 0.0004:
            return tgt
        k = 1.0 - pow(0.05, self.TICK_MS / 1000.0 / max(0.05, tween))
        return cur + (tgt - cur) * k

    def _advance(self, now: float, dt: float) -> bool:
        animating = False

        # Fade / entry
        if abs(self._fade_alpha - self._fade_target) > 1e-3:
            t = (now - self._fade_start) / max(1e-3, self._fade_duration)
            k = _ease_out_cubic(min(1.0, t))
            self._fade_alpha = _lerp(self._fade_from, self._fade_target, k)
            if self._fade_target >= 1.0:
                self._enter_scale_t = k
            else:
                self._enter_scale_t = 1.0 - 0.5 * k
            if t < 1.0:
                animating = True
            else:
                self._fade_alpha = self._fade_target

        prev = (self._hp_pct_disp, self._sta_pct_disp)
        self._hp_pct_disp = self._decay(
            self._hp_pct_disp, self._hp_pct_target, self.HP_TWEEN)
        self._sta_pct_disp = self._decay(
            self._sta_pct_disp, self._sta_pct_target, self.STA_TWEEN)
        if prev != (self._hp_pct_disp, self._sta_pct_disp):
            animating = True

        if self._sta_offline_pending and self._offline_debounce_t > 0.0:
            if now - self._offline_debounce_t >= self._offline_debounce_delay:
                self._sta_offline_pending = False
                self._offline_debounce_t = 0.0
                if not self._sta_offline:
                    self._sta_offline = True
                    self._sta_text = 'OFFLINE'
            else:
                animating = True

        # HP group auto-hide: start fading 200 ms after offline begins,
        # even if the OFFLINE label is still in its debounce window.
        if self._offline_hide_t > 0.0 and not self._hp_group_hidden:
            if now - self._offline_hide_t >= self._offline_hide_delay:
                self._offline_hide_t = 0.0
                if self._sta_offline or self._sta_offline_pending:
                    self._hp_group_hidden = True
                    self._hp_group_fade_t = now
                    self._hp_group_restore_t = 0.0
            else:
                animating = True

        if self._hp_group_fade_t and now - self._hp_group_fade_t < self._hp_group_fade_duration:
            animating = True
        elif self._hp_group_fade_t:
            self._hp_group_fade_t = 0.0
        if self._hp_group_restore_t and now - self._hp_group_restore_t < self._hp_group_restore_duration:
            animating = True
        elif self._hp_group_restore_t:
            self._hp_group_restore_t = 0.0

        # Continuous subtle effects
        if self._boss_timer_urgent and self._boss_timer_text:
            animating = True
        # FX timers
        if self._hp_flash_start and now - self._hp_flash_start < 0.45:
            animating = True

        return animating

    # ──────────────────────────────────────────
    #  Rendering
    # ──────────────────────────────────────────

    def _render(self, now: float) -> None:
        if not self._hwnd:
            return
        # Periodic topmost enforcement — keeps panel above taskbar
        if now - self._last_topmost_t > _TOPMOST_INTERVAL:
            self._last_topmost_t = now
            try:
                _user32.SetWindowPos(
                    ctypes.c_void_p(self._hwnd),
                    ctypes.c_void_p(HWND_TOPMOST),
                    0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE,
                )
            except Exception:
                pass
        img = self.compose_frame(now)
        try:
            _ulw_update(self._hwnd, img, self._x, self._y)
        except Exception as e:
            print(f'[HP-OV] ulw error: {e}')

    def compose_frame(self, now: Optional[float] = None) -> Image.Image:
        """Render one HP frame to an RGBA PIL image without touching Win32.

        Used by both the live ULW path (``_render``) and the off-screen
        test harness in ``temp/_hp_render_compare.py``.
        """
        if now is None:
            now = time.time()
        w, h = self.WIDTH, self.HEIGHT

        # Static layer cache — cover + id-plate shell + bar masks.
        # Signature depends only on the entry-translate offset.
        y_off = int(round(8 * (1.0 - self._enter_scale_t)))
        sig = (y_off,)
        if self._static_cache is None or self._static_sig != sig:
            base = Image.new('RGBA', (w, h), (0, 0, 0, 0))
            self._draw_id_plate_bg(base, y_off)
            self._draw_hp_cover_bg(base, y_off)
            self._draw_hp_bar_shell(base, y_off)
            self._draw_sta_track(base, y_off)
            self._static_cache = base
            self._static_sig = sig
        img = self._static_cache.copy()
        cover_rect = (COVER_X, COVER_Y + y_off, COVER_X + COVER_W, COVER_Y + COVER_H + y_off)
        box_rect = (BOX_X, BOX_Y + y_off, BOX_X + BOX_W, BOX_Y + BOX_H + y_off)
        sta_rect = (STA_X, STA_Y + y_off, STA_X + STA_W, STA_Y + STA_H + y_off)

        hp_group_alpha = 1.0
        if self._hp_group_hidden:
            if self._hp_group_fade_t > 0:
                t = min(1.0, (now - self._hp_group_fade_t) / self._hp_group_fade_duration)
                hp_group_alpha = max(0.0, 1.0 - _ease_out_cubic(t))
            else:
                hp_group_alpha = 0.0
        elif self._hp_group_restore_t > 0:
            t = min(1.0, (now - self._hp_group_restore_t) / self._hp_group_restore_duration)
            hp_group_alpha = min(1.0, _ease_out_cubic(t))

        if hp_group_alpha > 0.02:
            self._draw_root_outer_pulse(img, y_off, now, hp_group_alpha)
            self._draw_root_cover_pulse(img, y_off, now, hp_group_alpha)

        # Dynamic layers
        self._draw_brackets(img, y_off, now)
        self._draw_id_plate_text(img, y_off, now)
        self._draw_id_plate_scanline(img, y_off, now)
        self._draw_hp_fill(img, y_off, now)
        self._draw_hp_text(img, y_off)
        self._draw_sta_fill(img, y_off)
        self._draw_sta_text(img, y_off)
        if self._hp_flash_start and now - self._hp_flash_start < 0.45:
            self._draw_hp_flash(img, y_off, now)

        if self._sta_offline:
            img = _multiply_alpha_regions(img, (sta_rect,), 0.35)

        # HP group auto-hide opacity (web: _setHPGroupHidden — 500ms
        # fade / 180ms restore on cover + XTBox + STA, id-plate stays)
        if hp_group_alpha < 0.999:
            img = _multiply_alpha_regions(img, (cover_rect, box_rect, sta_rect), hp_group_alpha)

        # Global fade
        if self._fade_alpha < 0.999:
            a = np.asarray(img, dtype=np.uint8).copy()
            mul = int(max(0, min(255, self._fade_alpha * 255)))
            a[:, :, 3] = (a[:, :, 3].astype(np.uint16) * mul // 255
                          ).astype(np.uint8)
            img = Image.fromarray(a, 'RGBA')
        return img

    def _draw_root_outer_pulse(self, img: Image.Image, y_off: int,
                               now: float, alpha_scale: float = 1.0) -> None:
        pulse = (now - self._spawn_time) / 3.2
        env = 0.5 - 0.5 * math.cos((pulse % 1.0) * 2 * math.pi)
        low_hp = max(0.0, min(1.0, (0.42 - self._hp_pct_disp) / 0.42))
        flash = 0.0
        if self._hp_flash_start:
            flash_age = now - self._hp_flash_start
            if flash_age < 0.45:
                flash = (1.0 - flash_age / 0.45) ** 2
        strength = (0.10 + 0.10 * env + 0.18 * low_hp + 0.22 * flash) * alpha_scale
        if strength <= 0.01:
            return

        layer = Image.new('RGBA', img.size, (0, 0, 0, 0))
        ld = ImageDraw.Draw(layer, 'RGBA')
        ld.rounded_rectangle(
            (ID_X - 18, ID_Y - 8 + y_off,
             ID_X + ID_W + 10, ID_Y + ID_H + 8 + y_off),
            radius=12,
            fill=(104, 228, 255, int(34 * strength)),
        )
        ld.rounded_rectangle(
            (COVER_X - 14, COVER_Y - 8 + y_off,
             COVER_X + COVER_W + 18, COVER_Y + COVER_H + 10 + y_off),
            radius=10,
            fill=(243, 175, 18, int(26 * strength)),
        )
        ld.rounded_rectangle(
            (STA_X - 8, STA_Y - 4 + y_off,
             STA_X + STA_W + 10, STA_Y + STA_H + 6 + y_off),
            radius=8,
            fill=(212, 156, 23, int(18 * strength)),
        )
        layer = _gpu_blur(layer, 8)
        img.alpha_composite(layer)

        ring = Image.new('RGBA', img.size, (0, 0, 0, 0))
        rd = ImageDraw.Draw(ring, 'RGBA')
        rd.rounded_rectangle(
            (COVER_X - 6, COVER_Y - 4 + y_off,
             COVER_X + COVER_W + 6, COVER_Y + COVER_H + 4 + y_off),
            radius=9,
            outline=(208, 244, 255, int(52 * strength)),
            width=1,
        )
        rd.rounded_rectangle(
            (ID_X - 4, ID_Y - 3 + y_off,
             ID_X + ID_W + 4, ID_Y + ID_H + 3 + y_off),
            radius=8,
            outline=(255, 226, 154, int(38 * strength)),
            width=1,
        )
        img.alpha_composite(ring)

    def _draw_root_cover_pulse(self, img: Image.Image, y_off: int,
                               now: float, alpha_scale: float = 1.0) -> None:
        pulse = ((now - self._spawn_time + 0.45) % 2.8) / 2.8
        env = 0.5 - 0.5 * math.cos(pulse * 2 * math.pi)
        low_hp = max(0.0, min(1.0, (0.48 - self._hp_pct_disp) / 0.48))
        flash = 0.0
        if self._hp_flash_start:
            flash_age = now - self._hp_flash_start
            if flash_age < 0.45:
                flash = (1.0 - flash_age / 0.45) ** 2
        strength = (0.08 + 0.10 * env + 0.14 * low_hp + 0.18 * flash) * alpha_scale
        if strength <= 0.01:
            return

        id_glow = Image.new('RGBA', img.size, (0, 0, 0, 0))
        idd = ImageDraw.Draw(id_glow, 'RGBA')
        idd.rounded_rectangle(
            (ID_X + 2, ID_Y + 2 + y_off,
             ID_X + ID_W - 3, ID_Y + ID_H - 3 + y_off),
            radius=6,
            fill=(104, 228, 255, int(26 * strength)),
        )
        id_glow = _gpu_blur(id_glow, 5)
        img.alpha_composite(_clip_alpha(id_glow, self._id_plate_mask(y_off)))

        cover_glow = Image.new('RGBA', img.size, (0, 0, 0, 0))
        cgd = ImageDraw.Draw(cover_glow, 'RGBA')
        cgd.rounded_rectangle(
            (COVER_X + 2, COVER_Y + 2 + y_off,
             COVER_X + COVER_W - 3, COVER_Y + COVER_H - 3 + y_off),
            radius=6,
            fill=(255, 220, 132, int(24 * strength)),
        )
        sweep_x = COVER_X - 42 + int((COVER_W + 84) * (((now - self._spawn_time) * 0.22) % 1.0))
        cgd.rectangle(
            (sweep_x, COVER_Y + 4 + y_off,
             sweep_x + 54, COVER_Y + COVER_H - 4 + y_off),
            fill=(255, 255, 255, int(10 + 12 * strength)),
        )
        cover_glow = _gpu_blur(cover_glow, 5)
        img.alpha_composite(_clip_alpha(cover_glow, self._cover_mask(y_off)))

    # ── identity plate background ───────────────────────────────────

    def _id_plate_mask(self, y_off: int) -> Image.Image:
        """Rounded-rect mask for id-plate."""
        m = Image.new('L', (self.WIDTH, self.HEIGHT), 0)
        ImageDraw.Draw(m).rounded_rectangle(
            (ID_X, ID_Y + y_off,
             ID_X + ID_W - 1, ID_Y + ID_H - 1 + y_off),
            radius=6, fill=255,
        )
        return m

    def _draw_id_plate_bg(self, img: Image.Image, y_off: int) -> None:
        w, h = self.WIDTH, self.HEIGHT
        # Gradient 175deg ≈ top→bottom-right lean (approx vertical)
        grad = np.zeros((h, 1, 4), dtype=np.uint8)
        ys = np.linspace(0, 1, h)
        mid = np.clip((ys - 0.0) / 0.45, 0, 1)
        # Three-stop gradient: A (0%) → mid (45%) → B (100%)
        MID = (178, 180, 168, 255)
        r = np.where(ys < 0.45,
                     COVER_A[0] + (MID[0] - COVER_A[0]) * (ys / 0.45),
                     MID[0] + (COVER_B[0] - MID[0]) *
                     ((ys - 0.45) / 0.55))
        g = np.where(ys < 0.45,
                     COVER_A[1] + (MID[1] - COVER_A[1]) * (ys / 0.45),
                     MID[1] + (COVER_B[1] - MID[1]) *
                     ((ys - 0.45) / 0.55))
        b = np.where(ys < 0.45,
                     COVER_A[2] + (MID[2] - COVER_A[2]) * (ys / 0.45),
                     MID[2] + (COVER_B[2] - MID[2]) *
                     ((ys - 0.45) / 0.55))
        grad[:, 0, 0] = r
        grad[:, 0, 1] = g
        grad[:, 0, 2] = b
        grad[:, 0, 3] = 255
        plate = Image.fromarray(grad, 'RGBA').resize((w, h))
        mask = self._id_plate_mask(y_off)

        # Shadow — clipped to plate-and-below so blur cannot leak upward
        # into the area above the panel where it would (a) be visible as a
        # halo above the ID plate and (b) capture clicks that should pass
        # through to the game window.
        shadow = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        sd = ImageDraw.Draw(shadow)
        sd.rounded_rectangle(
            (ID_X + 5, ID_Y + 10 + y_off,
             ID_X + ID_W + 2, ID_Y + ID_H + 3 + y_off),
            radius=8, fill=(12, 14, 10, 46),
        )
        sd.rounded_rectangle(
            (ID_X + 2, ID_Y + 5 + y_off,
             ID_X + ID_W - 1, ID_Y + ID_H - 1 + y_off),
            radius=6, fill=(0, 0, 0, 82),
        )
        shadow = _gpu_blur(shadow, 7)
        # Zero out everything above the plate's top edge so the blurred
        # halo cannot leak upward beyond the panel content.
        clip_top = max(0, ID_Y + y_off)
        if clip_top > 0:
            sh_arr = np.array(shadow, dtype=np.uint8)
            sh_arr[:clip_top, :, 3] = 0
            shadow = Image.fromarray(sh_arr, 'RGBA')
        img.alpha_composite(shadow)

        img.paste(plate, (0, 0), mask)

        # Inset highlight (top 1 px line)
        ov = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        od = ImageDraw.Draw(ov)
        od.rounded_rectangle(
            (ID_X + 3, ID_Y + 3 + y_off,
             ID_X + ID_W - 4, ID_Y + 18 + y_off),
            radius=5, fill=(255, 255, 255, 16),
        )
        od.rounded_rectangle(
            (ID_X + 8, ID_Y + ID_H // 2 + y_off,
             ID_X + ID_W - 8, ID_Y + ID_H - 6 + y_off),
            radius=5, fill=(24, 26, 18, 10),
        )
        img.alpha_composite(_clip_alpha(ov, mask))

        # Horizontal scan-line texture (web: id-scanline-overlay::after)
        sl = _make_scanline_texture(w, h, alpha=10)
        img.alpha_composite(_clip_alpha(sl, mask))

        # Border
        d = ImageDraw.Draw(img, 'RGBA')
        d.rounded_rectangle(
            (ID_X, ID_Y + y_off,
             ID_X + ID_W - 1, ID_Y + ID_H - 1 + y_off),
            radius=6, outline=COVER_BORDER, width=1,
        )
        d.rounded_rectangle(
            (ID_X + 1, ID_Y + 1 + y_off,
             ID_X + ID_W - 2, ID_Y + ID_H - 2 + y_off),
            radius=5, outline=COVER_BORDER_DEEP, width=1,
        )
        # TL cyan bracket — drawn dynamically in _draw_brackets()
        # BR gold bracket — drawn dynamically in _draw_brackets()

    def _draw_brackets(self, img: Image.Image, y_off: int,
                       now: float) -> None:
        """Animated corner brackets: 2.5s sine pulse (web parity)."""
        d = ImageDraw.Draw(img, 'RGBA')

        def _corner_dot(x: int, y: int, col: Tuple[int, int, int, int]) -> None:
            d.rounded_rectangle((x - 1, y - 1, x + 1, y + 1), radius=1, fill=col)

        # TL cyan bracket — web: 2.5s ease-in-out infinite pulse (0.6→1→0.6)
        tl_pulse = (now % 2.5) / 2.5
        tl_env = 0.6 + 0.4 * (0.5 - 0.5 * math.cos(tl_pulse * 2 * math.pi))
        tl_gain = 0.62 + 0.38 * tl_env
        tl_col = (
            int(CYAN_SOFT[0] * tl_gain),
            int(CYAN_SOFT[1] * tl_gain),
            int(CYAN_SOFT[2] * tl_gain),
            255,
        )
        id_tlx = ID_X + 1
        id_tly = ID_Y + 1 + y_off
        d.line((id_tlx, id_tly, id_tlx + 16, id_tly), fill=tl_col, width=2)
        d.line((id_tlx, id_tly, id_tlx, id_tly + 16), fill=tl_col, width=2)
        _corner_dot(id_tlx, id_tly, tl_col)
        # BR gold bracket — web: 2.5s, 1.2s delay
        br_pulse = ((now + 1.3) % 2.5) / 2.5
        br_env = 0.6 + 0.4 * (0.5 - 0.5 * math.cos(br_pulse * 2 * math.pi))
        br_gain = 0.62 + 0.38 * br_env
        br_col = (
            int(GOLD_SOFT[0] * br_gain),
            int(GOLD_SOFT[1] * br_gain),
            int(GOLD_SOFT[2] * br_gain),
            255,
        )
        brx = ID_X + ID_W - 2
        bry = ID_Y + ID_H - 2 + y_off
        d.line((brx, bry, brx - 16, bry), fill=br_col, width=2)
        d.line((brx, bry, brx, bry - 16), fill=br_col, width=2)
        _corner_dot(brx, bry, br_col)
        # ── Cover panel corners are static in web CSS ──
        cover_tl = (CYAN[0], CYAN[1], CYAN[2], 255)
        cover_br = (GOLD[0], GOLD[1], GOLD[2], 255)
        cover_tlx = COVER_X + 2
        cover_tly = COVER_Y + 2 + y_off
        d.line((cover_tlx, cover_tly,
            cover_tlx + 16, cover_tly), fill=cover_tl, width=2)
        d.line((cover_tlx, cover_tly,
            cover_tlx, cover_tly + 16), fill=cover_tl, width=2)
        _corner_dot(cover_tlx, cover_tly, cover_tl)
        c_brx = COVER_X + COVER_W - 3
        c_bry = COVER_Y + COVER_H - 3 + y_off
        d.line((c_brx, c_bry, c_brx - 16, c_bry), fill=cover_br, width=2)
        d.line((c_brx, c_bry, c_brx, c_bry - 16), fill=cover_br, width=2)
        _corner_dot(c_brx, c_bry, cover_br)

    def _draw_id_plate_scanline(self, img: Image.Image, y_off: int,
                                now: float) -> None:
        """Vertical cyan scan band that travels top→bottom in 3.5 s."""
        t = (now - self._spawn_time) / 3.5
        t = t % 1.0
        band_y = int(ID_H * t) + ID_Y + y_off
        band_h = max(6, int(ID_H * 0.14))
        # Build a vertical gradient the full width of id-plate
        ov = Image.new('RGBA', (ID_W, band_h), (0, 0, 0, 0))
        arr = np.zeros((band_h, ID_W, 4), dtype=np.uint8)
        ys = np.linspace(0, 1, band_h)
        # peak mid of band — keep it subtle
        a_env = np.sin(ys * math.pi) * 18
        arr[:, :, 0] = 104
        arr[:, :, 1] = 228
        arr[:, :, 2] = 255
        arr[:, :, 3] = a_env[:, None].astype(np.uint8)
        band = Image.fromarray(arr, 'RGBA')
        # composite onto a temp that matches panel size then clip
        canvas = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        canvas.paste(band, (ID_X, band_y - band_h // 2), band)
        img.alpha_composite(_clip_alpha(canvas, self._id_plate_mask(y_off)))

    def _draw_id_plate_text(self, img: Image.Image, y_off: int,
                            now: float) -> None:
        draw = ImageDraw.Draw(img, 'RGBA')
        # Top row: "SYSTEM" + profession
        sys_font = _load_font('sao', 10)
        _draw_tracked(
            draw, (ID_X + 20, ID_Y + 8 + y_off),
            'SYSTEM', font=sys_font, fill=TEXT_MUTED,
            spacing=3,  # web: letter-spacing 3px
        )
        if self._profession:
            prof_font = _pick_font(self._profession, 13)
            prof_text = _truncate(
                draw, self._profession, prof_font, ID_W // 2)
            # web: text-shadow 0 0 6px rgba(243,175,18,0.3)
            _draw_text_shadow(
                img, (ID_X + 78, ID_Y + 6 + y_off),
                prof_text, prof_font,
                shadow_color=(243, 175, 18, 38), blur=2,
            )
            _draw_tracked(
                draw, (ID_X + 78, ID_Y + 6 + y_off),
                prof_text, font=prof_font, fill=PROF_GOLD,
                spacing=1.5,  # web: letter-spacing 1.5px
            )
        # Lv.X (top-right, gold)  — web: 16px, letter-spacing 2px
        lv_font = _load_font('sao', 16)
        lv_txt = f'Lv.{self._level}'
        lv_w = _tracked_width(draw, lv_txt, lv_font, spacing=2)
        _draw_tracked(
            draw, (ID_X + ID_W - 22 - lv_w, ID_Y + 6 + y_off),
            lv_txt, font=lv_font, fill=(212, 156, 23, 255),
            spacing=2,
        )

        # ── Boss timer / clock (upper-center, above Name) ──
        # Web: 30px clock with em-dashes "─ HH:MM:SS ─", color muted olive.
        #      Boss-timer font 13-18px cyan or urgent-red.
        if self._boss_timer_text:
            bt_font = _load_font('sao', 18)
            txt = self._boss_timer_text
            if self._boss_timer_urgent:
                pulse = (now % 0.8) / 0.8
                env = 0.55 + 0.45 * (0.5 - 0.5 *
                                     math.cos(pulse * 2 * math.pi))
                gain = 0.62 + 0.38 * env
                col = (
                    int(URGENT_RED[0] * gain),
                    int(URGENT_RED[1] * gain),
                    int(URGENT_RED[2] * gain),
                    255,
                )
            else:
                col = CYAN
            tw = _text_width(draw, txt, bt_font)
            bx = ID_X + (ID_W - tw) // 2
            by = ID_Y + 26 + y_off
            g = Image.new('RGBA', img.size, (0, 0, 0, 0))
            ImageDraw.Draw(g).text(
                (bx, by), txt,
                fill=(col[0], col[1], col[2], 70), font=bt_font,
            )
            img.alpha_composite(_gpu_blur(g, 2))
            draw.text((bx, by), txt, fill=col, font=bt_font)
        else:
            # Clock — web renders at 30px with em-dash decorators on
            # either side. The SAO font lacks U+2500 glyphs so we draw
            # the dashes as actual line strokes to match the web visual.
            now_t = time.localtime()
            clock = (f'{now_t.tm_hour:02d}:{now_t.tm_min:02d}:'
                     f'{now_t.tm_sec:02d}')
            cfont = _load_font('sao', 30)
            cw = _text_width(draw, clock, cfont)
            cx = ID_X + (ID_W - cw) // 2
            cy = ID_Y + (ID_H - 34) // 2
            col_clock = TEXT_MUTED
            draw.text((cx, cy + y_off), clock,
                      fill=col_clock, font=cfont)
            # Dash decorators: 18px long horizontal strokes flanking
            # the digits, vertically centred on clock mid-line.
            dash_y = cy + 18 + y_off
            draw.line(
                (cx - 26, dash_y, cx - 8, dash_y),
                fill=col_clock, width=2,
            )
            draw.line(
                (cx + cw + 8, dash_y, cx + cw + 26, dash_y),
                fill=col_clock, width=2,
            )

        # ── Name (bottom, large bold) ──  web: 24px, text-shadow
        name_font = _pick_font(self._name, 24)
        name = _truncate(draw, self._name, name_font, max(32, ID_W - 190))
        # web: text-shadow 0 0 6px rgba(255,255,255,0.2)
        _draw_text_shadow(
            img, (ID_X + 18, ID_Y + ID_H - 34 + y_off),
            name, name_font,
            shadow_color=(255, 255, 255, 22), blur=2,
        )
        draw.text(
            (ID_X + 18, ID_Y + ID_H - 34 + y_off),
            name, fill=TEXT_MAIN, font=name_font,
        )

        # UID (right column, top:34 of id-plate, muted olive, tiny)
        if self._uid:
            uid_font = _load_font('sao', 11)
            uid_w = _tracked_width(draw, self._uid, uid_font, spacing=1)
            _draw_tracked(
                draw, (ID_X + ID_W - 18 - uid_w, ID_Y + 34 + y_off),
                self._uid, font=uid_font, fill=TEXT_UID,
                spacing=1,  # web: letter-spacing 1px
            )

        # id-link "NErVGear - LINK OK" — bottom-right, cyan-blue pulse.
        # Web hides it when boss-timer is active, otherwise always visible.
        if not self._boss_timer_text:
            # Split around the dash so we can draw a line stroke that
            # renders consistently regardless of font glyph coverage.
            left_txt = 'NErVGear'
            right_txt = 'LINK OK'
            link_font = _load_font('sao', 9)
            lw_l = _text_width(draw, left_txt, link_font)
            lw_r = _text_width(draw, right_txt, link_font)
            gap = 10   # space reserved for the dash
            total = lw_l + gap + lw_r
            # Web: base alpha 0.85 with id-pulse opacity 0.6 → 1.0.
            t = ((now - self._spawn_time + 0.5) % 4.0) / 4.0
            env = 0.6 + 0.4 * (0.5 - 0.5 *
                                 math.cos(t * 2 * math.pi))
            gain = 0.64 + 0.36 * env
            col = (
                int(LINK_BLUE[0] * gain),
                int(LINK_BLUE[1] * gain),
                int(LINK_BLUE[2] * gain),
                255,
            )
            bx = ID_X + ID_W - 18 - total
            by = ID_Y + ID_H - 16 + y_off
            draw.text((bx, by), left_txt, fill=col, font=link_font)
            # Dash glyph via line stroke (U+2500 absent in SAO font)
            dy = by + 6
            draw.line(
                (bx + lw_l + 2, dy, bx + lw_l + gap - 2, dy),
                fill=col, width=1,
            )
            draw.text(
                (bx + lw_l + gap, by), right_txt,
                fill=col, font=link_font,
            )

    # ── hp-bg-cover ─────────────────────────────────────────────────

    def _cover_mask(self, y_off: int) -> Image.Image:
        m = Image.new('L', (self.WIDTH, self.HEIGHT), 0)
        ImageDraw.Draw(m).rounded_rectangle(
            (COVER_X, COVER_Y + y_off,
             COVER_X + COVER_W - 1, COVER_Y + COVER_H - 1 + y_off),
            radius=6, fill=255,
        )
        return m

    def _draw_hp_cover_bg(self, img: Image.Image, y_off: int) -> None:
        w, h = self.WIDTH, self.HEIGHT
        grad = np.zeros((h, 1, 4), dtype=np.uint8)
        ys = np.linspace(0, 1, h)
        MID = (178, 180, 168, 255)
        r = np.where(ys < 0.45,
                     COVER_A[0] + (MID[0] - COVER_A[0]) * (ys / 0.45),
                     MID[0] + (COVER_B[0] - MID[0]) *
                     ((ys - 0.45) / 0.55))
        g = np.where(ys < 0.45,
                     COVER_A[1] + (MID[1] - COVER_A[1]) * (ys / 0.45),
                     MID[1] + (COVER_B[1] - MID[1]) *
                     ((ys - 0.45) / 0.55))
        b = np.where(ys < 0.45,
                     COVER_A[2] + (MID[2] - COVER_A[2]) * (ys / 0.45),
                     MID[2] + (COVER_B[2] - MID[2]) *
                     ((ys - 0.45) / 0.55))
        grad[:, 0, 0] = r
        grad[:, 0, 1] = g
        grad[:, 0, 2] = b
        grad[:, 0, 3] = 255
        cover = Image.fromarray(grad, 'RGBA').resize((w, h))
        mask = self._cover_mask(y_off)

        # Shadow — clipped above the cover so blur halo cannot leak past
        # the panel boundary into the area above (which would be visible
        # as a halo and capture mouse clicks).
        shadow = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        sd = ImageDraw.Draw(shadow)
        sd.rounded_rectangle(
            (COVER_X + 6, COVER_Y + 10 + y_off,
             COVER_X + COVER_W + 2, COVER_Y + COVER_H + 5 + y_off),
            radius=8, fill=(18, 20, 14, 40),
        )
        sd.rounded_rectangle(
            (COVER_X + 2, COVER_Y + 5 + y_off,
             COVER_X + COVER_W - 1, COVER_Y + COVER_H - 1 + y_off),
            radius=6, fill=(0, 0, 0, 60),
        )
        shadow = _gpu_blur(shadow, 6)
        clip_top = max(0, COVER_Y + y_off)
        if clip_top > 0:
            sh_arr = np.array(shadow, dtype=np.uint8)
            sh_arr[:clip_top, :, 3] = 0
            shadow = Image.fromarray(sh_arr, 'RGBA')
        img.alpha_composite(shadow)

        img.paste(cover, (0, 0), mask)

        ov = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        od = ImageDraw.Draw(ov)
        od.rounded_rectangle(
            (COVER_X + 3, COVER_Y + 3 + y_off,
             COVER_X + COVER_W - 4, COVER_Y + 18 + y_off),
            radius=5, fill=(255, 255, 255, 14),
        )
        od.rounded_rectangle(
            (COVER_X + 8, COVER_Y + COVER_H // 2 + y_off,
             COVER_X + COVER_W - 8, COVER_Y + COVER_H - 6 + y_off),
            radius=5, fill=(24, 26, 18, 10),
        )
        img.alpha_composite(_clip_alpha(ov, mask))

        # Horizontal scan-line texture (web: hp-bg-cover::before)
        sl = _make_scanline_texture(w, h, alpha=10)
        img.alpha_composite(_clip_alpha(sl, mask))

        d = ImageDraw.Draw(img, 'RGBA')
        d.rounded_rectangle(
            (COVER_X, COVER_Y + y_off,
             COVER_X + COVER_W - 1, COVER_Y + COVER_H - 1 + y_off),
            radius=6, outline=COVER_BORDER, width=1,
        )
        d.rounded_rectangle(
            (COVER_X + 1, COVER_Y + 1 + y_off,
             COVER_X + COVER_W - 2, COVER_Y + COVER_H - 2 + y_off),
            radius=5, outline=COVER_BORDER_DEEP, width=1,
        )
        # TL cyan + BR gold corner brackets — drawn dynamically in _draw_brackets()

    # ── HP bar shell (XTBox) ────────────────────────────────────────

    def _draw_hp_bar_shell(self, img: Image.Image, y_off: int) -> None:
        draw = ImageDraw.Draw(img, 'RGBA')
        w, h = img.size
        # xt_left — 26×48 stepped polygon
        bx = BOX_X
        by = BOX_Y + y_off
        lh = BOX_H
        left_poly = [
            (bx, by),
            (bx + 26, by),
            (bx + 26, by + lh),
            (bx, by + lh),
            (bx, by + int(lh * 0.75)),
            (bx + 13, by + int(lh * 0.75)),
            (bx + 13, by + int(lh * 0.25)),
            (bx, by + int(lh * 0.25)),
        ]
        draw.polygon(left_poly, fill=BOX_BG)

        # xt_right — full clip-path polygon with horizontal gradient.
        # Web CSS: polygon(85px 22%, 100% 22%, 100% 0%, 0% 0%,
        #   0 100%, 228px 100%, 234px 77%, 100% 77%,
        #   100% 60%, 233px 60%, 228px 77%, 85px 77%)
        rx0 = bx + 29
        rw = BOX_W - 29
        clip_poly = [
            (rx0 + 85,  by + int(lh * 0.22)),
            (rx0 + rw,  by + int(lh * 0.22)),
            (rx0 + rw,  by),
            (rx0,       by),
            (rx0,       by + lh),
            (rx0 + 228, by + lh),
            (rx0 + 234, by + int(lh * 0.77)),
            (rx0 + rw,  by + int(lh * 0.77)),
            (rx0 + rw,  by + int(lh * 0.60)),
            (rx0 + 233, by + int(lh * 0.60)),
            (rx0 + 228, by + int(lh * 0.77)),
            (rx0 + 85,  by + int(lh * 0.77)),
        ]
        # Build polygon mask
        shape_mask = Image.new('L', (w, h), 0)
        ImageDraw.Draw(shape_mask).polygon(clip_poly, fill=255)
        # Horizontal shell fill: keep the XT box opaque so the HUD never
        # shows desktop/game pixels through the panel body.
        grad = np.zeros((h, w, 4), dtype=np.uint8)
        xs = np.arange(rw, dtype=np.float32) / max(1, rw - 1)
        shade = (1.0 - 0.06 * xs).astype(np.float32)
        grad[:, rx0:rx0 + rw, 0] = np.clip(BOX_BG[0] * shade, 0, 255).astype(np.uint8)
        grad[:, rx0:rx0 + rw, 1] = np.clip(BOX_BG[1] * shade, 0, 255).astype(np.uint8)
        grad[:, rx0:rx0 + rw, 2] = np.clip(BOX_BG[2] * shade, 0, 255).astype(np.uint8)
        grad[:, rx0:rx0 + rw, 3] = 255
        # Multiply gradient alpha by polygon mask
        m_arr = np.asarray(shape_mask, dtype=np.uint16)
        grad[:, :, 3] = (grad[:, :, 3].astype(np.uint16)
                         * m_arr // 255).astype(np.uint8)
        img.alpha_composite(Image.fromarray(grad, 'RGBA'))

        shell_fx = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        sfd = ImageDraw.Draw(shell_fx, 'RGBA')
        sfd.polygon(
            [
                (rx0 + 86, by + 2),
                (rx0 + rw - 10, by + 2),
                (rx0 + rw - 15, by + 12),
                (rx0 + 90, by + 12),
            ],
            fill=(255, 255, 255, 12),
        )
        img.alpha_composite(_clip_alpha(shell_fx, shape_mask))

        # svg_border stroke — polygon outline
        border_x = bx + 114
        border_y = by + 10
        poly = [
            (border_x, border_y),
            (border_x + 350, border_y),
            (border_x + 345, border_y + 19),
            (border_x + 145, border_y + 19),
            (border_x + 141, border_y + 27),
            (border_x, border_y + 27),
        ]
        draw.polygon(poly, outline=LINE)
        # top/bottom hairlines inside
        draw.line((border_x, border_y + 1,
                   border_x + 350, border_y + 1), fill=LINE)
        draw.line((border_x, border_y + 25,
                   border_x + 340, border_y + 25), fill=LINE)

    def _hp_bar_mask(self, y_off: int) -> Image.Image:
        m = Image.new('L', (self.WIDTH, self.HEIGHT), 0)
        bx = BOX_X + 114
        by = BOX_Y + 10 + y_off
        # Inner fill region: between the two tb_lines inside xt_border
        poly = [
            (bx + 1, by + 1),
            (bx + 349, by + 1),
            (bx + 344, by + 19),
            (bx + 145, by + 19),
            (bx + 141, by + 26),
            (bx + 1, by + 26),
        ]
        ImageDraw.Draw(m).polygon(poly, fill=255)
        return m

    def _hp_gradient(self, pct: float) -> Tuple[Tuple, Tuple]:
        if pct >= 0.60:
            return HP_GREEN
        if pct >= 0.25:
            return HP_YELLOW
        return HP_RED

    def _hp_fill_width_px(self, pct: float) -> int:
        value = max(0.0, min(1.0, float(pct or 0.0)))
        if value <= 0.0:
            return 0
        if value >= 0.997:
            return int(round(350 + 6.0))
        return int(round(350 * value + 8.0))

    def _draw_hp_fill(self, img: Image.Image, y_off: int,
                      now: float) -> None:
        pct = max(0.0, min(1.0, self._hp_pct_disp))
        if pct <= 0.002:
            return
        # Web width math: 0 → 0, full → 100% + 6px, otherwise % + 8px.
        fill_w = self._hp_fill_width_px(pct)
        if fill_w <= 0:
            return
        ca, cb = self._hp_gradient(pct)
        bar = _make_hgrad_bar(fill_w, 25, ca, cb)
        cap = _make_skew_cap(25, cb, skew_px=7, extra=7)
        canvas = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        canvas.paste(bar, (BOX_X + 114 + 1, BOX_Y + 10 + 1 + y_off))
        if pct < 0.995:
            canvas.paste(
                cap,
                (BOX_X + 114 + fill_w - 7, BOX_Y + 10 + 1 + y_off),
                cap,
            )
        img.alpha_composite(_clip_alpha(canvas, self._hp_bar_mask(y_off)))

    def _draw_hp_flash(self, img: Image.Image, y_off: int,
                       now: float) -> None:
        age = now - self._hp_flash_start
        t = age / 0.45
        if t >= 1.0:
            return
        env = (1.0 - t) ** 2
        a = int(170 * env)
        if a <= 3:
            return
        fx = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        ImageDraw.Draw(fx).rectangle(
            (BOX_X + 114, BOX_Y + 10 + y_off,
             BOX_X + 114 + 350, BOX_Y + 10 + 27 + y_off),
            fill=(255, 240, 240, a),
        )
        img.alpha_composite(_clip_alpha(fx, self._hp_bar_mask(y_off)))

    def _draw_hp_text(self, img: Image.Image, y_off: int) -> None:
        draw = ImageDraw.Draw(img, 'RGBA')
        # username — inside xt_right, vertically centred (CSS: flex
        # align-items:center, font-size:14px inherited from .XTBox).
        name_font = _pick_font(self._name, 14)
        name = _truncate(draw, self._name, name_font, 85)
        # xt_right starts at BOX_X + 26(xt_left) + 3(margin) = +29.
        # span has padding-left:10 → text x ≈ +39.
        nx = BOX_X + 39
        # Centre on box vertical mid-line (name font ≈ 18px tall)
        ny = BOX_Y + (BOX_H - 18) // 2 + y_off
        draw.text((nx, ny), name, fill=TEXT_MAIN, font=name_font)

        # number_xt — HP cur/total (big) + Lv (small) below
        cur = self._hp_max * self._hp_pct_disp if self._hp_max > 0 else 0
        val_txt = f'{int(round(cur))}/{int(round(self._hp_max))}'
        val_font = _load_font('sao', 14)
        vw = _text_width(draw, val_txt, val_font)
        # number_xt position inside box: left:58%, top:82%
        nx0 = BOX_X + int(BOX_W * 0.58) + 4
        ny0 = BOX_Y + int(BOX_H * 0.82) - 3 + y_off
        # right aligned inside first cell (55% of 220px)
        cell_w = int(220 * 0.55)
        draw.text((nx0 + cell_w - vw - 6, ny0), val_txt,
                  fill=TEXT_MAIN, font=val_font)
        # Lv in second cell
        lv_font = _load_font('sao', 12)
        lv_txt = f'lv.{self._level}'
        draw.text((nx0 + cell_w + 6, ny0 + 1), lv_txt,
                  fill=TEXT_MAIN, font=lv_font)

    # ── STA row ─────────────────────────────────────────────────────

    def _draw_sta_track(self, img: Image.Image, y_off: int) -> None:
        draw = ImageDraw.Draw(img, 'RGBA')
        # Diamond pip
        cx = STA_X + 6
        cy = STA_Y + STA_H // 2 + y_off
        draw.polygon(
            [(cx, cy - 4), (cx + 4, cy), (cx, cy + 4), (cx - 4, cy)],
            outline=(212, 156, 23, 255), fill=(212, 156, 23, 255),
        )
        draw.polygon(
            [(cx, cy - 2), (cx + 2, cy), (cx, cy + 2), (cx - 2, cy)],
            fill=(255, 236, 180, 196),
        )
        # Label "STA"  — web: letter-spacing 2px
        lbl_font = _load_font('sao', 9)
        _draw_tracked(
            draw, (STA_X + 18, STA_Y + 2 + y_off),
            'STA', font=lbl_font, fill=(212, 156, 23, 255),
            spacing=2,
        )
        # Track
        tx0 = STA_X + 50
        ty0 = STA_Y + (STA_H - 6) // 2 + y_off
        tx1 = STA_X + STA_W - 60
        draw.rounded_rectangle(
            (tx0, ty0, tx1, ty0 + 6), radius=1,
            fill=BOX_BG, outline=(212, 156, 23, 255), width=1,
        )

    def _draw_sta_fill(self, img: Image.Image, y_off: int) -> None:
        if self._sta_offline:
            return
        pct = max(0.0, min(1.0, self._sta_pct_disp))
        if pct <= 0.002:
            return
        tx0 = STA_X + 50
        ty0 = STA_Y + (STA_H - 6) // 2 + y_off
        tx1 = STA_X + STA_W - 60
        fw = int(round((tx1 - tx0) * pct))
        if fw <= 0:
            return
        bar = _make_hgrad_bar(fw - 2, 4, STA_A, STA_B)
        img.alpha_composite(bar, (tx0 + 1, ty0 + 1))
        highlight = Image.new('RGBA', (max(1, fw - 2), 1), (255, 242, 202, 72))
        img.alpha_composite(highlight, (tx0 + 1, ty0 + 1))
        # Soft glow above bar (box-shadow: 0 0 6px gold)
        glow = Image.new('RGBA',
                         (fw + 12, 14), (0, 0, 0, 0))
        ImageDraw.Draw(glow).rectangle(
            (6, 4, fw + 6, 10), fill=(243, 175, 18, 60),
        )
        glow = _gpu_blur(glow, 3)
        img.alpha_composite(glow, (tx0 - 6, ty0 - 4))

    def _draw_sta_text(self, img: Image.Image, y_off: int) -> None:
        draw = ImageDraw.Draw(img, 'RGBA')
        font = _load_font('sao', 9)
        txt = self._sta_text
        ty = STA_Y + 2 + y_off
        col = ((212, 90, 60, 255) if self._sta_offline
             else TEXT_STA)
        # web: letter-spacing 1px, right-aligned at STA_X + STA_W - 54
        tw = _tracked_width(draw, txt, font, spacing=1)
        tx0 = STA_X + STA_W - 54
        _draw_tracked(draw, (tx0, ty), txt, font=font, fill=col, spacing=1)

    # ──────────────────────────────────────────
    #  Dragging / click / menu
    # ──────────────────────────────────────────

    # Click-vs-drag threshold: pointer movement in px before a
    # Button-1 sequence is considered a drag rather than a tap.
    _TAP_THRESHOLD_PX = 4

    def _on_drag_start(self, ev) -> None:
        try:
            self._drag_ox = ev.x_root - self._x
            self._drag_oy = ev.y_root - self._y
            self._drag_origin = (ev.x_root, ev.y_root)
            self._drag_moved = False
        except Exception:
            self._drag_ox = 0
            self._drag_oy = 0
            self._drag_origin = (0, 0)
            self._drag_moved = False

    def _on_drag_move(self, ev) -> None:
        try:
            ox, oy = self._drag_origin
            if not self._drag_moved:
                if (abs(ev.x_root - ox) < self._TAP_THRESHOLD_PX and
                        abs(ev.y_root - oy) < self._TAP_THRESHOLD_PX):
                    return
                self._drag_moved = True
            self._x = int(ev.x_root - self._drag_ox)
            self._y = int(ev.y_root - self._drag_oy)
            if self._win is not None:
                self._win.geometry(f'+{self._x}+{self._y}')
            self._schedule_tick(immediate=True)
        except Exception:
            pass

    def _point_in_click_zones(self, lx: int, ly: int) -> bool:
        """Web parity: only id-plate and hp-bar (XTBox) regions are
        clickable. Cover, STA row, and blank gutters are inert."""
        if ID_X <= lx <= ID_X + ID_W and ID_Y <= ly <= ID_Y + ID_H:
            return True
        if BOX_X <= lx <= BOX_X + BOX_W and BOX_Y <= ly <= BOX_Y + BOX_H:
            return True
        return False

    def _on_drag_end(self, ev) -> None:
        moved = getattr(self, '_drag_moved', False)
        if moved:
            # Persist new position only when actually dragged.
            if self.settings is not None:
                try:
                    self.settings.set('hp_ov_x', int(self._x))
                    self.settings.set('hp_ov_y', int(self._y))
                    self.settings.set('hp_ov_panel_w', int(self.WIDTH))
                    save = getattr(self.settings, 'save', None)
                    if callable(save):
                        save()
                except Exception:
                    pass
            return
        # Tap — match web: only id-plate / hp-bar regions open SAO menu.
        try:
            lx = int(ev.x_root - self._x)
            ly = int(ev.y_root - self._y)
        except Exception:
            return
        if not self._point_in_click_zones(lx, ly):
            return
        cb = self._on_click
        if callable(cb):
            try:
                cb()
            except Exception:
                pass

    def _on_context_menu(self, ev) -> None:
        try:
            lx = int(ev.x_root - self._x)
            ly = int(ev.y_root - self._y)
        except Exception:
            return
        if not self._point_in_click_zones(lx, ly):
            return
        cb = self._on_menu
        if callable(cb):
            try:
                # Web offsets ctx menu by (+4, -26) from pointer.
                cb(int(ev.x_root) + 4, int(ev.y_root) - 26)
            except Exception:
                pass

    def restore_position(self) -> None:
        """Reset panel to its default (web-matching) location."""
        self._sync_layout()
        self._x, self._y = self._default_panel_pos()
        if self._win is not None:
            try:
                self._win.geometry(f'+{self._x}+{self._y}')
            except Exception:
                pass
        if self.settings is not None:
            try:
                self.settings.set('hp_ov_x', int(self._x))
                self.settings.set('hp_ov_y', int(self._y))
                self.settings.set('hp_ov_panel_w', int(self.WIDTH))
                save = getattr(self.settings, 'save', None)
                if callable(save):
                    save()
            except Exception:
                pass
        self._schedule_tick(immediate=True)
