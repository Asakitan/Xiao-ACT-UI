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
from types import SimpleNamespace
from typing import Any, Callable, Dict, Optional, Tuple

import threading

import numpy as np
import tkinter as tk
from PIL import Image, ImageDraw, ImageFont, ImageFilter
from gpu_renderer import gaussian_blur_rgba as _gpu_blur, render_shell_rgba as _gpu_shell
from overlay_scheduler import get_scheduler as _get_scheduler
from overlay_render_worker import (
    AsyncFrameWorker, multiply_alpha_image, submit_ulw_commit,
)
from overlay_subpixel import subpixel_bar_width

# v2.3.x: optional GPU presenter (mirrors SkillFX/MenuHud pattern).
# Env-gated via SAO_GPU_HP (defaults to SAO_GPU_OVERLAY). Falls back to
# the original ULW path if GLFW is unavailable. GPU mode now owns input
# callbacks, preserving drag/tap/context menu while keeping presentation
# off the ULW path.
try:
    import gpu_overlay_window as _gow  # type: ignore[import-untyped]
except Exception:
    _gow = None  # type: ignore[assignment]


def _gpu_hp_enabled() -> bool:
    if _gow is None or not _gow.glfw_supported():
        return False
    try:
        import os as _os
        flag = _os.environ.get('SAO_GPU_HP')
        if flag is None:
            return True
        return str(flag).strip() not in ('', '0', 'false', 'False')
    except Exception:
        return False

from perf_probe import gauge as _perf_gauge, phase as _phase_trace, probe as _probe

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
PANEL_SHADOW_BOTTOM = 24       # vertical gutter below content for downward drop shadow
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
COVER_A = (247, 248, 249, 255)
COVER_MID = (236, 238, 240, 255)
COVER_B = (226, 229, 232, 255)
COVER_BORDER = (186, 190, 196, 255)
COVER_BORDER_DEEP = (160, 165, 171, 255)
BOX_BG = (249, 249, 250, 255)
TEXT_MAIN = (100, 99, 100, 255)
TEXT_MUTED = (140, 135, 138, 255)
TEXT_UID = (140, 135, 138, 255)
TEXT_STA = (140, 135, 138, 255)
LINE = (214, 216, 219, 255)
LINE_SOFT = (246, 247, 248, 255)
HAIRLINE_LIGHT = (248, 249, 250, 255)
HAIRLINE_MID = (226, 229, 232, 255)
HAIRLINE_DARK = (160, 165, 171, 255)
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


def _offset_poly(points, dx: int, dy: int):
    return [(x + dx, y + dy) for x, y in points]


def _clip_alpha(img: Image.Image, mask: Image.Image) -> Image.Image:
    if img.size != mask.size:
        mask = mask.resize(img.size)
    a = np.asarray(img, dtype=np.uint8).copy()
    m = np.asarray(mask, dtype=np.uint16)
    a[:, :, 3] = (a[:, :, 3].astype(np.uint16) * m // 255).astype(np.uint8)
    return Image.fromarray(a, 'RGBA')


# v2.2.12 — Per-thread compositor handle for HP. Mirrors BossHP's
# `_inset_tls` so the GPU inset-shadow shader (single-pass blur +
# clip + composite) is reachable from the render-lane thread that
# owns its own GL context.
_inset_tls = threading.local()


def _get_thread_compositor():
    comp = getattr(_inset_tls, 'compositor', None)
    if comp is not None:
        return comp
    try:
        from gpu_compositor import LayerCompositor
        comp = LayerCompositor('hp')
    except Exception:
        comp = None
    _inset_tls.compositor = comp
    return comp


def _apply_inset_shadow_gpu(img: Image.Image, mask: Image.Image,
                            color: Tuple[int, int, int],
                            alpha: int, blur_radius: float) -> bool:
    """v2.2.12: single-pass GPU inset shadow.

    Mirrors `sao_gui_bosshp._apply_inset_shadow_gpu` exactly — keeps the
    entire inverted-mask → blur → clip → composite chain on the GPU,
    eliminating two PIL↔numpy roundtrips per call. Returns False to let
    the CPU path take over on any failure.
    """
    try:
        from gpu_compositor import LayerCompositor  # noqa: F401
    except Exception:
        return False
    try:
        comp = _get_thread_compositor()
        if comp is None or not comp.available:
            return False
        w, h = img.size
        mask_arr = np.asarray(mask, dtype=np.uint8)
        if mask_arr.shape != (h, w):
            return False
        # 1) Inverted-mask RGBA → upload.
        inv_rgba = np.empty((h, w, 4), dtype=np.uint8)
        inv_rgba[:, :, 0] = color[0]
        inv_rgba[:, :, 1] = color[1]
        inv_rgba[:, :, 2] = color[2]
        inv_rgba[:, :, 3] = 255 - mask_arr
        inv_tex = comp.upload('__hp_inset_inv', inv_rgba)
        if inv_tex is None:
            return False
        # 2) GPU-resident two-pass separable blur.
        blurred_tex = comp.blur_tex(inv_tex, blur_radius,
                                    out_tag='__hp_inset_blur')
        if blurred_tex is None:
            return False
        # 3) Shape mask + fused inset_shadow shader.
        shape_rgba = np.empty((h, w, 4), dtype=np.uint8)
        shape_rgba[:, :, 0] = color[0]
        shape_rgba[:, :, 1] = color[1]
        shape_rgba[:, :, 2] = color[2]
        shape_rgba[:, :, 3] = mask_arr
        shape_tex = comp.upload('__hp_inset_shape', shape_rgba)
        out_tex = comp.tex('__hp_inset_out', w, h, clear=True)
        if shape_tex is None or out_tex is None:
            return False
        comp.render(
            'inset_shadow', out_tex,
            uniforms={
                'u_color': (color[0] / 255.0, color[1] / 255.0,
                            color[2] / 255.0, 1.0),
                'u_intensity': max(0.0, min(1.0, alpha / 255.0)),
            },
            inputs={'u_blurred_inv': blurred_tex,
                    'u_shape': shape_tex},
        )
        glow = comp.to_pil(out_tex)
        if glow is None:
            return False
        img.alpha_composite(glow)
        return True
    except Exception:
        return False


def _apply_inset_shadow(img: Image.Image, mask: Image.Image,
                        color: Tuple[int, int, int],
                        alpha: int, blur_radius: float) -> None:
    """CSS-style `inset 0 0 Npx rgba(color, alpha)` glow on `img`,
    clipped by `mask`.

    v2.2.12: tries the fused GPU shader first (saves 2 PIL roundtrips
    + numpy mask invert + numpy alpha multiply); falls back to the
    original CPU/PIL pipeline on any error so the panel always renders.
    """
    if _apply_inset_shadow_gpu(img, mask, color, alpha, blur_radius):
        return
    inv = Image.eval(mask, lambda v: 255 - v)
    rgba = np.zeros((img.size[1], img.size[0], 4), dtype=np.uint8)
    rgba[:, :, 0] = color[0]
    rgba[:, :, 1] = color[1]
    rgba[:, :, 2] = color[2]
    rgba[:, :, 3] = np.asarray(inv, dtype=np.uint8)
    blurred = _gpu_blur(Image.fromarray(rgba, 'RGBA'), blur_radius)
    glow_alpha = np.asarray(blurred, dtype=np.uint16)[:, :, 3]
    glow_alpha = (glow_alpha * alpha // 255).astype(np.uint8)
    out = np.zeros((img.size[1], img.size[0], 4), dtype=np.uint8)
    out[:, :, 0] = color[0]
    out[:, :, 1] = color[1]
    out[:, :, 2] = color[2]
    out[:, :, 3] = glow_alpha
    img.alpha_composite(_clip_alpha(Image.fromarray(out, 'RGBA'), mask))


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


# ── Theme definitions ──────────────────────────────────────────
# Light theme = current default palette values
HP_THEME_LIGHT = {
    'COVER_A':          (247, 248, 249, 255),
    'COVER_MID':        (236, 238, 240, 255),
    'COVER_B':          (226, 229, 232, 255),
    'COVER_BORDER':     (186, 190, 196, 255),
    'COVER_BORDER_DEEP':(160, 165, 171, 255),
    'BOX_BG':           (249, 249, 250, 255),
    'TEXT_MAIN':        (100, 99, 100, 255),
    'TEXT_MUTED':       (140, 135, 138, 255),
    'TEXT_UID':         (140, 135, 138, 255),
    'TEXT_STA':         (140, 135, 138, 255),
    'LINE':             (214, 216, 219, 255),
    'LINE_SOFT':        (246, 247, 248, 255),
    'HAIRLINE_LIGHT':   (248, 249, 250, 255),
    'HAIRLINE_MID':     (226, 229, 232, 255),
    'HAIRLINE_DARK':    (160, 165, 171, 255),
}

HP_THEME_DARK = {
    'COVER_A':          (20, 26, 36, 255),
    'COVER_MID':        (16, 20, 30, 255),
    'COVER_B':          (12, 16, 24, 255),
    'COVER_BORDER':     (50, 80, 110, 200),
    'COVER_BORDER_DEEP':(40, 65, 90, 200),
    'BOX_BG':           (22, 30, 42, 255),
    'TEXT_MAIN':        (210, 220, 230, 255),
    'TEXT_MUTED':       (120, 140, 160, 255),
    'TEXT_UID':         (120, 140, 160, 255),
    'TEXT_STA':         (120, 140, 160, 255),
    'LINE':             (50, 70, 90, 255),
    'LINE_SOFT':        (30, 42, 58, 255),
    'HAIRLINE_LIGHT':   (35, 50, 70, 255),
    'HAIRLINE_MID':     (45, 62, 82, 255),
    'HAIRLINE_DARK':    (60, 85, 110, 255),
}

from sao_theme import register_panel_theme
register_panel_theme('hp', 'light', HP_THEME_LIGHT)
register_panel_theme('hp', 'dark', HP_THEME_DARK)


# ═══════════════════════════════════════════════
#  Overlay
# ═══════════════════════════════════════════════

class HpOverlay:
    """Animated SAO-styled Player HP / Identity / STA overlay."""

    WIDTH = PANEL_W
    HEIGHT = PANEL_H

    # Animation tuning
    HP_TWEEN = 0.32
    STA_TWEEN = 0.22
    TICK_MS = 16          # damping coefficient base; not scheduling rate (overlay_scheduler owns Hz)
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
        # v2.3.x GPU presenter fields. _gpu_managed=True swaps the
        # ULW commit/destroy/geometry sites to GLFW equivalents and
        # `self._win` becomes the BurstReadyOverlay-style self sentinel.
        self._gpu_window: Optional[Any] = None
        self._gpu_presenter: Optional[Any] = None
        self._gpu_managed: bool = False
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
        self._hp_last_update_t = 0.0
        self._sta_cur = 100.0
        self._sta_max = 100.0
        self._sta_text = '100%'
        self._sta_pct_target = 1.0
        self._sta_pct_disp = 1.0
        self._sta_last_update_t = 0.0
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
        self._gpu_drag_active = False
        self._last_topmost_t = 0.0  # last SetWindowPos topmost call
        self._idle_submit_q = -1     # quantized idle submit gate (perf)
        self._hover_zone: Optional[str] = None
        self._hover_t = {'id': 0.0, 'hp': 0.0}
        self._press_zone: Optional[str] = None
        self._press_t = {'id': 0.0, 'hp': 0.0}
        self._press_flash_t = {'id': 0.0, 'hp': 0.0}
        self._hover_sound_zone: Optional[str] = None
        self._last_interaction_sound_t = 0.0

        # HP group auto-hide on offline (web parity: debounce + fade)
        self._offline_debounce_t = 0.0   # when offline first detected
        self._offline_hide_t = 0.0       # when hide timer started
        self._hp_group_fade_t = 0.0      # when fade-out started
        self._hp_group_restore_t = 0.0   # when restore started
        self._offline_debounce_delay = 0.10
        self._offline_hide_delay = 0.20
        self._hp_group_fade_duration = 0.50
        self._hp_group_restore_duration = 0.18

        # Static layer caches (panels + shell, split for shadow z-order).
        self._panels_cache: Optional[Image.Image] = None
        self._panels_sig: tuple = ()
        self._shell_cache: Optional[Image.Image] = None
        self._shell_sig: tuple = ()
        # v2.2.17: cache the text-shadow layer (was rebuilt + reblurred
        # every frame). Sig captures every input that influences the
        # shadow pixels: y_off, ID-plate text/state, HP digits, boss
        # timer text + quantized urgent-pulse phase.
        self._shadow_cache: Optional[Image.Image] = None
        self._shadow_sig: tuple = ()
        # v2.2.20: pulse glow caches. _draw_root_outer_pulse +
        # _draw_root_cover_pulse each allocate a full-screen RGBA layer
        # and run an 8/5px GPU blur EVERY frame. Strength is the only
        # input that varies smoothly with time; quantizing it to ~50
        # buckets keeps the visual breathing identical (alpha delta
        # 0.02 invisible at the underlying glow opacities) and lets
        # ~70-90% of frames hit the cache.
        self._outer_pulse_cache: Optional[Image.Image] = None
        self._outer_pulse_sig: tuple = ()
        self._cover_pulse_cache: Optional[Image.Image] = None
        self._cover_pulse_sig: tuple = ()
        self._interaction_fx_cache: Optional[Image.Image] = None
        self._interaction_fx_sig: tuple = ()
        # v2.3.0 Phase 1: full-frame cache. The HP panel runs at 60 Hz
        # but most consecutive frames differ only in animation phase
        # (bracket pulse, scanline sweep) and continuous tween (root
        # outer/cover pulse). Quantize each animation source to a
        # perceptually invisible bucket and reuse the previous compose
        # output when the resulting signature matches. The result is
        # consumed read-only by _premultiply_to_bgra in the worker, so
        # the same Image instance is safe to return across frames.
        self._frame_cache: Optional[Image.Image] = None
        self._frame_sig: tuple = ()
        self._frame_version: int = 0
        self._last_compose_sig: Optional[tuple] = None

        # Async render worker — compose + premult off main thread.
        # v2.2.12: prefer_isolation so HP gets a dedicated heavy lane,
        # matching BossHP / Burst / MenuHud. Without it, HP shared a
        # lane with idle panels and got serialized behind their compose
        # work during heavy fights.
        self._render_worker = AsyncFrameWorker(prefer_isolation=True)

        # Theme: load saved preference
        self._theme_name: str = 'light'
        if settings is not None:
            try:
                saved = settings.get('panel_themes', {}).get('hp', 'light')
                if saved in ('light', 'dark'):
                    self._apply_theme(saved)
            except Exception:
                pass

    # ── Theme ──

    def _apply_theme(self, theme_name: str) -> None:
        """切换 HP 面板主题并清除所有渲染缓存。"""
        from sao_theme import get_panel_theme
        theme = get_panel_theme('hp', theme_name)
        if not theme:
            return
        # HP 用模块级常量，直接 setattr 本模块
        import sao_gui_hp as _mod
        for key, value in theme.items():
            setattr(_mod, key, value)
        self._theme_name = theme_name
        # 清除所有缓存
        self._panels_cache = None; self._panels_sig = ()
        self._shell_cache = None; self._shell_sig = ()
        self._shadow_cache = None; self._shadow_sig = ()
        self._outer_pulse_cache = None; self._outer_pulse_sig = ()
        self._cover_pulse_cache = None; self._cover_pulse_sig = ()
        self._interaction_fx_cache = None; self._interaction_fx_sig = ()
        self._frame_cache = None; self._frame_sig = ()
        self._last_compose_sig = None
        self._frame_version += 1
        # Force next compose — visible panels re-render immediately.
        self._schedule_tick(immediate=True)

    def _default_panel_pos(self) -> Tuple[int, int]:
        sw, sh = _get_screen_metrics()
        # Webview HP window sits at `sw * HUD_WINDOW_LEFT_PCT`; the hud-stage
        # inside it is offset `4vw` (= `sw * HUD_VW_PCT * STAGE_LEFT_PCT`)
        # from that window's left edge. Match it on screen exactly.
        stage_screen_x = int(round(sw * HUD_WINDOW_LEFT_PCT
                                   + sw * HUD_VW_PCT * STAGE_LEFT_PCT))
        return stage_screen_x, max(0, sh - PANEL_H)

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
                max(0, prev_sig[1] - PANEL_H),
            )

        _recompute_layout(sw)
        self.WIDTH = PANEL_W
        self.HEIGHT = PANEL_H + PANEL_SHADOW_BOTTOM
        new_stage_x = int(round(sw * HUD_WINDOW_LEFT_PCT
                                 + sw * HUD_VW_PCT * STAGE_LEFT_PCT))
        new_default = (new_stage_x, max(0, sh - PANEL_H))

        if prev_sig is None or (getattr(self, '_x', None), getattr(self, '_y', None)) == prev_default:
            self._x, self._y = new_default

        self._screen_sig = sig
        self._panels_cache = None
        self._panels_sig = ()
        self._shell_cache = None
        self._shell_sig = ()
        self._shadow_cache = None
        self._shadow_sig = ()
        self._outer_pulse_cache = None
        self._outer_pulse_sig = ()
        self._cover_pulse_cache = None
        self._cover_pulse_sig = ()
        self._interaction_fx_cache = None
        self._interaction_fx_sig = ()
        self._frame_cache = None
        self._frame_sig = ()
        self._last_compose_sig = None

        if self._gpu_managed and self._gpu_window is not None:
            try:
                self._gpu_window.set_geometry(
                    self._x, self._y, self.WIDTH, self.HEIGHT)
            except Exception:
                pass
        elif self._win is not None and self._win is not self \
                and self._win.winfo_exists():
            try:
                self._win.geometry(f'{self.WIDTH}x{self.HEIGHT}+{self._x}+{self._y}')
            except Exception:
                pass

    # ──────────────────────────────────────────
    #  Lifecycle
    # ──────────────────────────────────────────

    def show(self) -> None:
        _phase_trace('hp.overlay.show.begin', f'gpu={int(bool(_gpu_hp_enabled()))}')
        self._sync_layout()
        if self._win is not None:
            return
        # v2.3.x: try GPU presenter path first when env-enabled.
        if _gpu_hp_enabled():
            try:
                pump = _gow.get_glfw_pump(self.root)
                presenter = _gow.BgraPresenter()
                gpu_win = _gow.GpuOverlayWindow(
                    pump,
                    w=int(self.WIDTH), h=int(self.HEIGHT),
                    x=int(self._x), y=int(self._y),
                    render_fn=presenter.render,
                    click_through=False,
                    title='sao_hp_gpu',
                )
                gpu_win.set_input_callbacks(
                    cursor_pos_fn=self._on_gpu_cursor_pos,
                    cursor_leave_fn=self._on_gpu_cursor_leave,
                    mouse_button_fn=self._on_gpu_mouse_button,
                )
                gpu_win.show()
                self._gpu_window = gpu_win
                self._gpu_presenter = presenter
                self._gpu_managed = True
                self._win = self  # type: ignore[assignment]  # sentinel
                self._hwnd = 0
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
                self._last_compose_sig = None
                _phase_trace('hp.overlay.show.gpu', f'xy={self._x},{self._y}')
                self._schedule_tick(immediate=True)
                return
            except Exception:
                self._gpu_window = None
                self._gpu_presenter = None
                self._gpu_managed = False
        self._win = tk.Toplevel(self.root)
        self._win.overrideredirect(True)
        self._win.attributes('-topmost', True)
        # v2.3.0: keep the Toplevel hidden until the layered ex-style is
        # applied. Without withdraw(), tk creates a default-bg (white)
        # opaque window for a few ms before WS_EX_LAYERED is set, which
        # the user perceives as a white flash next to the ID plate.
        self._win.withdraw()
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
        # 防御性清理：移除可能被 _apply_panel_style() 设置的 CS_DROPSHADOW
        try:
            _GCL_STYLE, _CS_DS = -26, 0x00020000
            _cls = ctypes.windll.user32.GetClassLongW(self._hwnd, _GCL_STYLE)
            if _cls & _CS_DS:
                ctypes.windll.user32.SetClassLongW(
                    self._hwnd, _GCL_STYLE, _cls & ~_CS_DS)
        except Exception:
            pass
        # v2.3.0: now that WS_EX_LAYERED is in effect (the very next
        # ULW commit drives the per-pixel alpha), it's safe to show.
        try:
            self._win.deiconify()
        except Exception:
            pass
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
        self._win.bind('<Motion>', self._on_pointer_move)
        self._win.bind('<Leave>', self._on_pointer_leave)

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
        self._last_compose_sig = None
        _phase_trace('hp.overlay.show.ulw', f'hwnd={self._hwnd} xy={self._x},{self._y}')
        self._schedule_tick(immediate=True)

    def hide(self) -> None:
        if self._win is None:
            return
        if self._exiting:
            return
        _phase_trace('hp.overlay.hide.begin', f'alpha={self._fade_alpha:.3f}')
        self._exiting = True
        self._fade_from = self._fade_alpha
        self._fade_target = 0.0
        self._fade_start = time.time()
        self._fade_duration = self.FADE_OUT
        self._hide_after_exit = True
        self._schedule_tick(immediate=True)

    def destroy(self) -> None:
        _phase_trace('hp.overlay.destroy', f'gpu={int(bool(self._gpu_managed))} hwnd={self._hwnd}')
        self._cancel_tick()
        if hasattr(self, '_render_worker') and self._render_worker is not None:
            try:
                self._render_worker.stop()
            except Exception:
                pass
        # GPU presenter teardown first (mirrors SkillFX pattern).
        if self._gpu_presenter is not None:
            try:
                self._gpu_presenter.release()
            except Exception:
                pass
            self._gpu_presenter = None
        if self._gpu_window is not None:
            try:
                self._gpu_window.destroy()
            except Exception:
                pass
            self._gpu_window = None
        if self._win is not None and self._win is not self:
            try:
                self._win.destroy()
            except Exception:
                pass
        self._win = None
        self._hwnd = 0
        self._gpu_managed = False
        self._gpu_drag_active = False
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
        self._hp_last_update_t = time.time()
        self._idle_submit_q = -1
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
        self._sta_last_update_t = time.time()
        self._idle_submit_q = -1
        self._schedule_tick(immediate=True)

    def set_sta_offline(self, offline: bool) -> None:
        offline = bool(offline)
        now = time.time()
        if offline:
            if self._sta_offline or self._sta_offline_pending:
                return
            _phase_trace('hp.sta_offline.set', 'offline=1')
            self._sta_offline_pending = True
            self._offline_debounce_t = now
            if not self._hp_group_hidden and self._hp_group_fade_t <= 0.0:
                self._offline_hide_t = now
                _phase_trace('hp.group.hide_arm', f'fade_t={self._hp_group_fade_t:.3f}')
        else:
            if (not self._sta_offline and not self._sta_offline_pending
                    and not self._hp_group_hidden and self._hp_group_fade_t <= 0.0):
                self._sta_text = self._format_sta_text()
                return
            _phase_trace('hp.sta_offline.set', 'offline=0')
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
                _phase_trace('hp.group.restore', f't={now:.3f}')
        self._schedule_tick(immediate=True)

    # ──────────────────────────────────────────
    #  Tick loop
    # ──────────────────────────────────────────

    def _schedule_tick(self, immediate: bool = False) -> None:
        if not self._visible or self._win is None:
            return
        # v2.3.x: in GPU mode `_win` is the self sentinel; treat as live.
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
        if self._hp_last_update_t and (now - self._hp_last_update_t) < 0.55:
            return True
        if self._sta_last_update_t and (now - self._sta_last_update_t) < 0.55:
            return True
        for zone in ('id', 'hp'):
            hover_target = 1.0 if self._hover_zone == zone else 0.0
            press_target = 1.0 if self._press_zone == zone else 0.0
            if abs(float(self._hover_t.get(zone, 0.0) or 0.0) - hover_target) > 1e-3:
                return True
            if abs(float(self._press_t.get(zone, 0.0) or 0.0) - press_target) > 1e-3:
                return True
            if float(self._press_flash_t.get(zone, 0.0) or 0.0) > now:
                return True
        if self._fade_target >= 1.0 and self._enter_scale_t < 0.999:
            return True
        if self._visible and self._fade_target >= 1.0 and not self._exiting:
            return True
        return False

    def _compose_signature(self, now: float, is_animating: Optional[bool] = None) -> Optional[tuple]:
        """Coarse output fingerprint for pre-submit dirty-skip.

        Returns None while an animation is actively moving, which forces
        a normal submit path. In steady state we quantize all visible
        idle effects so unchanged output does not re-run compose,
        premultiply, or ULW enqueue.
        """
        if is_animating is None:
            is_animating = self._is_animating()
        if is_animating:
            return None
        if self.WIDTH <= 0 or self.HEIGHT <= 0 or self._spawn_time <= 0:
            return None
        try:
            y_off = int(round(8 * (1.0 - self._enter_scale_t)))
            hp_pct = max(0.0, min(1.0, self._hp_pct_disp))
            max_int = int(round(self._hp_max))
            bucket = max(1, max_int // 1000) if max_int > 0 else 1
            hp_int = (int(round(self._hp_max * hp_pct)) // bucket) * bucket
            sta_pct_q = int(round(self._sta_pct_disp * 100))
            hp_group_alpha_q = int(self._hp_group_alpha_now(now) * 24)
            tl_pulse_q = int(((now % 2.5) / 2.5) * 8)
            br_pulse_q = int((((now + 1.3) % 2.5) / 2.5) * 8)
            scan_q = int((((now - self._spawn_time) / 3.5) % 1.0) * 10)
            outer_q = int(self._root_outer_pulse_strength(now) * 16)
            cover_q, cover_sweep_q = self._root_cover_pulse_keys(now)
            cover_q = int((cover_q / 50.0) * 16)
            cover_sweep_q = int(cover_sweep_q // 12)
            if self._boss_timer_text:
                clock_q = 0
                link_q = 0
            else:
                clock_q = int(now)
                link_q = int((((now - self._spawn_time + 0.5) % 4.0) / 4.0) * 10)
            return (
                y_off,
                int(round(self._fade_alpha * 100)),
                hp_group_alpha_q,
                self._profession, self._name, self._uid, self._level,
                hp_int, max_int, sta_pct_q, self._sta_text,
                bool(self._sta_offline),
                self._boss_timer_text, self._boss_timer_urgent,
                clock_q, link_q,
                tl_pulse_q, br_pulse_q, scan_q,
                outer_q, cover_q, cover_sweep_q,
                int(round(self._hover_t.get('id', 0.0) * 32)),
                int(round(self._hover_t.get('hp', 0.0) * 32)),
                int(round(self._press_t.get('id', 0.0) * 32)),
                int(round(self._press_t.get('hp', 0.0) * 32)),
                int(max(0.0, min(0.25, (self._press_flash_t.get('id', 0.0) or 0.0) - now)) * 48),
                int(max(0.0, min(0.25, (self._press_flash_t.get('hp', 0.0) or 0.0) - now)) * 48),
            )
        except Exception:
            return None

    @_probe.decorate('ui.hp.tick')
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
        if self._hwnd or self._gpu_managed:
            # v2.2.23: allow_during_capture=True so vision PrintWindow
            # ticks (10 Hz × 30-60 ms each) don't drop our commits — the
            # async ulw queue is per-HWND and can't conflict with the
            # game-window capture.
            fb = self._render_worker.take_result(allow_during_capture=True)
            if fb is not None:
                if self._gpu_managed and self._gpu_presenter is not None \
                        and self._gpu_window is not None:
                    try:
                        # Track moves the user made via set_position too.
                        if (fb.x, fb.y) != (self._x, self._y):
                            self._gpu_window.set_geometry(
                                self._x, self._y,
                                self.WIDTH, self.HEIGHT)
                        self._gpu_presenter.set_frame(
                            fb.bgra_bytes, fb.width, fb.height)
                        self._gpu_window.request_redraw()
                        _perf_gauge('ui.hp.presented', 1)
                    except Exception as e:
                        print(f'[HP-OV] gpu present error: {e}')
                elif self._hwnd:
                    try:
                        submit_ulw_commit(self._hwnd, fb, allow_during_capture=True)
                        _perf_gauge('ui.hp.presented', 1)
                    except Exception as e:
                        print(f'[HP-OV] ulw error: {e}')
            else:
                _perf_gauge('ui.hp.presented', 0)

            # Periodic topmost enforcement (HWND only — GLFW window is
            # already TOPMOST via WS_EX_TOPMOST in gpu_overlay_window).
            if self._hwnd and now - self._last_topmost_t > _TOPMOST_INTERVAL:
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
            #    v2.2.16: idle rate cap — when nothing is animating, the
            #    HP panel only needs ~10 Hz to keep the HH:MM:SS clock
            #    and the slow NErVGear LINK pulse smooth. The previous
            #    20 Hz idle cadence was burning ~one full CPU core in
            #    compose_frame (~40 ms each). Animating frames pass
            #    through unchanged so combat HP/shield tweens stay 60 Hz.
            is_animating = self._is_animating()
            if is_animating:
                self._idle_submit_q = -1
                submit_now = True
            else:
                q = int(now * 10.0)
                submit_now = q != self._idle_submit_q
                if submit_now:
                    self._idle_submit_q = q
            if submit_now:
                sig = self._compose_signature(now, is_animating=is_animating)
                if sig is None or sig != self._last_compose_sig:
                    self._last_compose_sig = sig
                    self._render_worker.submit(
                        self.compose_frame, now, self._hwnd, self._x, self._y)
                    _perf_gauge('ui.hp.submitted', 1)
                else:
                    _perf_gauge('ui.hp.skipped_sig', 1)
            else:
                _perf_gauge('ui.hp.skipped_idle', 1)

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
                    _phase_trace('hp.sta_offline.commit', f't={now:.3f}')
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
                    _phase_trace(
                        'hp.group.hidden',
                        f'offline={int(bool(self._sta_offline))} pending={int(bool(self._sta_offline_pending))}',
                    )
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
        for zone in ('id', 'hp'):
            hover_target = 1.0 if self._hover_zone == zone else 0.0
            hover_cur = self._hover_t.get(zone, 0.0)
            hover_tween = 0.25 if hover_target > hover_cur else 0.45
            next_hover = self._decay(hover_cur, hover_target, hover_tween)
            if abs(next_hover - self._hover_t.get(zone, 0.0)) > 1e-4:
                self._hover_t[zone] = next_hover
                animating = True
            press_target = 1.0 if self._press_zone == zone else 0.0
            press_cur = self._press_t.get(zone, 0.0)
            press_tween = 0.07 if press_target > press_cur else 0.16
            next_press = self._decay(press_cur, press_target, press_tween)
            if abs(next_press - self._press_t.get(zone, 0.0)) > 1e-4:
                self._press_t[zone] = next_press
                animating = True
            flash_until = float(self._press_flash_t.get(zone, 0.0) or 0.0)
            if flash_until > now:
                animating = True
            elif flash_until:
                self._press_flash_t[zone] = 0.0

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

    @_probe.decorate('ui.hp.compose')
    def compose_frame(self, now: Optional[float] = None) -> Image.Image:
        """Render one HP frame to an RGBA PIL image without touching Win32.

        Used by both the live ULW path (``_render``) and the off-screen
        test harness in ``temp/_hp_render_compare.py``.
        """
        if now is None:
            now = time.time()
        w, h = self.WIDTH, self.HEIGHT

        # Static layer caches.
        # Signature depends only on the entry-translate offset.
        y_off = int(round(8 * (1.0 - self._enter_scale_t)))
        sig = (y_off,)

        # ── v2.3.0 Phase 1: full-frame cache ──────────────────────────
        # Build a quantized signature of EVERYTHING that affects the
        # output pixels. If it matches the previous compose, we can skip
        # the entire 30 ms PIL/numpy pipeline and reuse the prior image.
        # Quantization buckets are sized below the perceptual threshold
        # so visual quality is unaffected (\u201c\u4e0d\u80fd\u7ed9\u7279\u6548\u505a\u51cf\u6cd5\u201d).
        frame_sig = self._compute_frame_sig(now, y_off)
        if frame_sig is not None and self._frame_cache is not None \
                and self._frame_sig == frame_sig:
            return self._frame_cache

        # Cache A: outer panel shadows → panel backgrounds → content shadows
        if self._panels_cache is None or self._panels_sig != sig:
            base = Image.new('RGBA', (w, h), (0, 0, 0, 0))
            self._draw_panel_shadows(base, y_off)
            self._draw_id_plate_bg(base, y_off)
            self._draw_hp_cover_bg(base, y_off)
            self._draw_content_shadows(base, y_off)
            self._panels_cache = base
            self._panels_sig = sig

        # Cache B: bar shell + STA track (composited ABOVE text shadows)
        if self._shell_cache is None or self._shell_sig != sig:
            shell = Image.new('RGBA', (w, h), (0, 0, 0, 0))
            self._draw_hp_bar_shell(shell, y_off)
            self._draw_sta_track(shell, y_off)
            self._shell_cache = shell
            self._shell_sig = sig

        img = self._panels_cache.copy()
        # Unified HP-group fade region: covers cover panel, XT box, STA
        # track AND all shadow bleed (outer drop-shadow, content shadows,
        # text shadows).  One rect avoids double-multiplication where the
        # old separate rects overlapped.
        sg = PANEL_SHADOW_GUTTER          # 18 — matches blur bleed
        hp_fade_rect = (
            COVER_X - sg,
            min(COVER_Y, BOX_Y) - sg + y_off,
            max(COVER_X + COVER_W, BOX_X + BOX_W, STA_X + STA_W) + sg,
            max(COVER_Y + COVER_H, STA_Y + STA_H) + PANEL_SHADOW_BOTTOM + y_off,
        )

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

        # Text shadows — batched onto one layer, one blur, composited
        # between panels and shell (above panel bg, below bar-shell).
        # v2.2.17: cache by content sig. shadow pixels depend only on
        # y_off, profession, name, level, HP digits and boss timer
        # state (with quantized urgent pulse). Idle / steady combat
        # frames now skip Image.new + 2 draw_text + _gpu_blur entirely.
        # v2.2.19: quantize hp_int into ~1000 buckets so the shadow
        # cache hits during HP tweens (previously every tween frame
        # missed). The shadow uses 2px blur, so 0.1% HP differences
        # are invisible; the exact value still draws into the
        # unblurred content layer drawn AFTER the shadow composite.
        max_int = int(round(self._hp_max))
        hp_int_raw = int(round(self._hp_max * self._hp_pct_disp)) if self._hp_max > 0 else 0
        bucket = max(1, max_int // 1000) if max_int > 0 else 1
        hp_int = (hp_int_raw // bucket) * bucket
        if self._boss_timer_text and self._boss_timer_urgent:
            urgent_q = int((now % 0.8) * 12.5)  # ~10 buckets / 0.8s
        else:
            urgent_q = 0
        shadow_sig = (
            y_off,
            self._profession,
            self._name,
            self._level,
            hp_int,
            max_int,
            self._boss_timer_text,
            self._boss_timer_urgent,
            urgent_q,
        )
        if self._shadow_cache is None or self._shadow_sig != shadow_sig:
            shadow_layer = Image.new('RGBA', (w, h), (0, 0, 0, 0))
            self._draw_id_plate_text(shadow_layer, y_off, now, mode='shadow')
            self._draw_hp_text(shadow_layer, y_off, mode='shadow')
            shadow_layer = _gpu_blur(shadow_layer, 2)
            self._shadow_cache = shadow_layer
            self._shadow_sig = shadow_sig
        img.alpha_composite(self._shadow_cache)

        # Shell on top of shadows
        img.alpha_composite(self._shell_cache)

        # Dynamic layers — text content (no shadows), fills, etc.
        self._draw_brackets(img, y_off, now)
        self._draw_id_plate_text(img, y_off, now, mode='content')
        self._draw_id_plate_scanline(img, y_off, now)
        self._draw_hp_fill(img, y_off, now)
        self._draw_hp_text(img, y_off, mode='content')
        self._draw_sta_fill(img, y_off)
        self._draw_sta_text(img, y_off)
        self._draw_interaction_fx(img, y_off, now)
        if self._hp_flash_start and now - self._hp_flash_start < 0.45:
            self._draw_hp_flash(img, y_off, now)

        if self._sta_offline and hp_group_alpha >= 0.999:
            sta_rect = (STA_X, STA_Y + y_off,
                        STA_X + STA_W, STA_Y + STA_H + y_off)
            img = _multiply_alpha_regions(img, (sta_rect,), 0.35)

        # HP group auto-hide opacity (web: _setHPGroupHidden — 500ms
        # fade / 180ms restore on cover + XTBox + STA, id-plate stays)
        if hp_group_alpha < 0.999:
            img = _multiply_alpha_regions(img, (hp_fade_rect,), hp_group_alpha)

        # Global fade
        if self._fade_alpha < 0.999:
            img = multiply_alpha_image(img, self._fade_alpha)
        # v2.3.0 Phase 1: store frame for cache reuse on next call.
        if frame_sig is not None:
            self._frame_cache = img
            self._frame_sig = frame_sig
            self._frame_version += 1
            try:
                img._sao_premult_safe = True  # type: ignore[attr-defined]
                img._sao_content_version = self._frame_version  # type: ignore[attr-defined]
            except Exception:
                pass
        return img

    def _compute_frame_sig(self, now: float, y_off: int) -> Optional[tuple]:
        """Build a quantized signature of all per-frame pixel inputs.

        Returns None if any input is in a regime where caching would be
        unsafe (e.g. no spawn time, freshly resized).  Otherwise returns
        a hashable tuple suitable for self._frame_sig comparison.

        Quantization rationale:
        - Animation phases (bracket/scanline/root pulses) bucket to
          intervals where 1-bucket alpha/position deltas are below the
          ~1 / 255 luminance step on already-soft glow primitives.
        - HP and STA fills quantize to integer pixel widths because the
          underlying bar is rendered with subpixel_bar_width \u2014 sub-pixel
          changes within the same integer width are lost in the cap blit.
        - Continuous tweens (hp_pct_disp, sta_pct_disp) become integers
          so steady-state combat doesn't churn the cache on every micro-
          tween step.
        """
        if self.WIDTH <= 0 or self.HEIGHT <= 0:
            return None
        if self._spawn_time <= 0:
            return None

        hp_pct = max(0.0, min(1.0, self._hp_pct_disp))
        max_int = int(round(self._hp_max))
        bucket = max(1, max_int // 1000) if max_int > 0 else 1
        hp_int = (int(round(self._hp_max * hp_pct)) // bucket) * bucket
        # HP fill bucket = 1/2 px (subpixel_bar_width fades trailing
        # column; 0.5 px alpha delta on ~64-alpha gradient stays \u2264 8/255).
        try:
            hp_fill_q = int(round(self._hp_fill_width_px(hp_pct) * 2.0))
        except Exception:
            hp_fill_q = int(round(hp_pct * 700))

        sta_pct_q = int(round(self._sta_pct_disp * 200))  # 0.5 % buckets

        # Bracket pulse \u2014 16 buckets per 2.5 s cycle (each ~156 ms).
        # Gain delta per bucket: 0.4 / 16 = 0.025 \u2192 alpha delta ~ 6/255 on
        # CYAN_SOFT/GOLD_SOFT base; multiplied by 0.62..1.0 stays \u2264 6.
        tl_pulse_q = int(((now % 2.5) / 2.5) * 16)
        br_pulse_q = int((((now + 1.3) % 2.5) / 2.5) * 16)

        # Scanline sweep \u2014 quantize band_y to 4-px buckets (band is
        # 14 px tall on a 96-px plate; 4-px steps of a soft gradient
        # are below visual discrimination during the 3.5 s sweep).
        scan_t = ((now - self._spawn_time) / 3.5) % 1.0
        scan_q = int(scan_t * (PANEL_H // 4))

        # Root outer pulse strength quantized identically to its own
        # cache (50 buckets).  Same for root cover pulse + sweep_q.
        outer_strength = self._root_outer_pulse_strength(now)
        outer_q = int(outer_strength * 50)
        cover_q, cover_sweep_q = self._root_cover_pulse_keys(now)

        # HP flash window \u2014 bucket age to ~30 ms (compose runs at
        # 60 Hz, so we cache the flash phase for ~2 frames at most).
        flash_age = -1.0
        if self._hp_flash_start:
            age = now - self._hp_flash_start
            if 0 <= age < 0.45:
                flash_age = int(age * 33.3)  # 30 ms buckets
        # HP-group fade & restore: bucket alpha to 50 buckets.
        hp_group_alpha_q = int(self._hp_group_alpha_now(now) * 50)

        # Boss timer urgent quantization (mirrors shadow cache).
        if self._boss_timer_text and self._boss_timer_urgent:
            urgent_q = int((now % 0.8) * 12.5)
        else:
            urgent_q = 0

        return (
            int(self.WIDTH), int(self.HEIGHT), y_off,
            self._profession, self._name, self._level,
            hp_int, max_int, hp_fill_q, sta_pct_q,
            self._boss_timer_text, self._boss_timer_urgent, urgent_q,
            tl_pulse_q, br_pulse_q, scan_q,
            outer_q, cover_q, cover_sweep_q,
            flash_age, hp_group_alpha_q,
            int(round(self._fade_alpha * 100)),
            bool(self._sta_offline),
            int(round(self._hover_t.get('id', 0.0) * 20)),
            int(round(self._hover_t.get('hp', 0.0) * 20)),
            int(round(self._press_t.get('id', 0.0) * 20)),
            int(round(self._press_t.get('hp', 0.0) * 20)),
            int(max(0.0, min(0.25, (self._press_flash_t.get('id', 0.0) or 0.0) - now)) * 40),
            int(max(0.0, min(0.25, (self._press_flash_t.get('hp', 0.0) or 0.0) - now)) * 40),
        )

    def _root_outer_pulse_strength(self, now: float) -> float:
        pulse = (now - self._spawn_time) / 3.2
        env = 0.5 - 0.5 * math.cos((pulse % 1.0) * 2 * math.pi)
        low_hp = max(0.0, min(1.0, (0.42 - self._hp_pct_disp) / 0.42))
        flash = 0.0
        if self._hp_flash_start:
            flash_age = now - self._hp_flash_start
            if flash_age < 0.45:
                flash = (1.0 - flash_age / 0.45) ** 2
        alpha_scale = self._hp_group_alpha_now(now)
        return (0.10 + 0.10 * env + 0.18 * low_hp + 0.22 * flash) * alpha_scale

    def _root_cover_pulse_keys(self, now: float) -> Tuple[int, int]:
        pulse = ((now - self._spawn_time + 0.45) % 2.8) / 2.8
        env = 0.5 - 0.5 * math.cos(pulse * 2 * math.pi)
        low_hp = max(0.0, min(1.0, (0.48 - self._hp_pct_disp) / 0.48))
        flash = 0.0
        if self._hp_flash_start:
            flash_age = now - self._hp_flash_start
            if flash_age < 0.45:
                flash = (1.0 - flash_age / 0.45) ** 2
        alpha_scale = self._hp_group_alpha_now(now)
        strength = (0.08 + 0.10 * env + 0.14 * low_hp + 0.18 * flash) * alpha_scale
        s_q = int(strength * 50)
        sweep_raw = COVER_X - 42 + int((COVER_W + 84) * (((now - self._spawn_time) * 0.22) % 1.0))
        sweep_q = sweep_raw & ~3
        return s_q, sweep_q

    def _hp_group_alpha_now(self, now: float) -> float:
        if self._hp_group_hidden:
            if self._hp_group_fade_t > 0:
                t = min(1.0, (now - self._hp_group_fade_t) / self._hp_group_fade_duration)
                return max(0.0, 1.0 - _ease_out_cubic(t))
            return 0.0
        if self._hp_group_restore_t > 0:
            t = min(1.0, (now - self._hp_group_restore_t) / self._hp_group_restore_duration)
            return min(1.0, _ease_out_cubic(t))
        return 1.0

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

        # v2.2.20: quantize strength to 50 buckets (Δ=0.02 alpha steps).
        # Below the perceptual threshold for these underlying glow
        # alphas (max contribution 34*0.02 ≈ 1/255), but lets cache hit
        # on most idle/steady frames.
        s_q = int(strength * 50)
        sig = (img.size, y_off, s_q)
        if self._outer_pulse_cache is None or self._outer_pulse_sig != sig:
            strength_q = s_q / 50.0
            layer = Image.new('RGBA', img.size, (0, 0, 0, 0))
            ld = ImageDraw.Draw(layer, 'RGBA')
            ld.rounded_rectangle(
                (ID_X - 18, ID_Y - 8 + y_off,
                 ID_X + ID_W + 10, ID_Y + ID_H + 8 + y_off),
                radius=12,
                fill=(104, 228, 255, int(34 * strength_q)),
            )
            ld.rounded_rectangle(
                (COVER_X - 14, COVER_Y - 8 + y_off,
                 COVER_X + COVER_W + 18, COVER_Y + COVER_H + 10 + y_off),
                radius=10,
                fill=(243, 175, 18, int(26 * strength_q)),
            )
            ld.rounded_rectangle(
                (STA_X - 8, STA_Y - 4 + y_off,
                 STA_X + STA_W + 10, STA_Y + STA_H + 6 + y_off),
                radius=8,
                fill=(212, 156, 23, int(18 * strength_q)),
            )
            layer = _gpu_blur(layer, 8)

            ring = Image.new('RGBA', img.size, (0, 0, 0, 0))
            rd = ImageDraw.Draw(ring, 'RGBA')
            rd.rounded_rectangle(
                (COVER_X - 6, COVER_Y - 4 + y_off,
                 COVER_X + COVER_W + 6, COVER_Y + COVER_H + 4 + y_off),
                radius=9,
                outline=(208, 244, 255, int(52 * strength_q)),
                width=1,
            )
            rd.rounded_rectangle(
                (ID_X - 4, ID_Y - 3 + y_off,
                 ID_X + ID_W + 4, ID_Y + ID_H + 3 + y_off),
                radius=8,
                outline=(255, 226, 154, int(38 * strength_q)),
                width=1,
            )
            layer.alpha_composite(ring)
            self._outer_pulse_cache = layer
            self._outer_pulse_sig = sig

        img.alpha_composite(self._outer_pulse_cache)

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

        # v2.2.20: cache by quantized strength (50 buckets) +
        # quantized sweep_x (4 px). Δalpha 0.02 invisible at the
        # underlying glow opacities; 4 px sweep granularity is below
        # the perceived motion of a 5px-blurred rectangle. ~30-50%
        # cache hit ratio in steady state.
        s_q = int(strength * 50)
        sweep_raw = COVER_X - 42 + int((COVER_W + 84) * (((now - self._spawn_time) * 0.22) % 1.0))
        sweep_q = sweep_raw & ~3
        sig = (img.size, y_off, s_q, sweep_q)
        if self._cover_pulse_cache is None or self._cover_pulse_sig != sig:
            strength_q = s_q / 50.0
            combined = Image.new('RGBA', img.size, (0, 0, 0, 0))

            id_glow = Image.new('RGBA', img.size, (0, 0, 0, 0))
            idd = ImageDraw.Draw(id_glow, 'RGBA')
            idd.rounded_rectangle(
                (ID_X + 2, ID_Y + 2 + y_off,
                 ID_X + ID_W - 3, ID_Y + ID_H - 3 + y_off),
                radius=6,
                fill=(104, 228, 255, int(26 * strength_q)),
            )
            id_glow = _gpu_blur(id_glow, 5)
            combined.alpha_composite(_clip_alpha(id_glow, self._id_plate_mask(y_off)))

            cover_glow = Image.new('RGBA', img.size, (0, 0, 0, 0))
            cgd = ImageDraw.Draw(cover_glow, 'RGBA')
            cgd.rounded_rectangle(
                (COVER_X + 2, COVER_Y + 2 + y_off,
                 COVER_X + COVER_W - 3, COVER_Y + COVER_H - 3 + y_off),
                radius=6,
                fill=(255, 220, 132, int(24 * strength_q)),
            )
            cgd.rectangle(
                (sweep_q, COVER_Y + 4 + y_off,
                 sweep_q + 54, COVER_Y + COVER_H - 4 + y_off),
                fill=(255, 255, 255, int(10 + 12 * strength_q)),
            )
            cover_glow = _gpu_blur(cover_glow, 5)
            combined.alpha_composite(_clip_alpha(cover_glow, self._cover_mask(y_off)))

            self._cover_pulse_cache = combined
            self._cover_pulse_sig = sig

        img.alpha_composite(self._cover_pulse_cache)

    # ── panel drop shadows (drawn BEFORE any content) ─────────────

    def _draw_panel_shadows(self, img: Image.Image, y_off: int) -> None:
        """Draw drop shadows for both id-plate and cover panels.

        Must be called BEFORE any panel content is composited so that
        shadows always sit below content in the z-order.
        """
        w, h = self.WIDTH, self.HEIGHT
        all_mask = self._all_content_mask(y_off)
        inv_mask = 255 - np.asarray(all_mask, dtype=np.uint8)

        shadows = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        sd = ImageDraw.Draw(shadows)

        # Id-plate: CSS drop-shadow(2px 3px 14px rgba(18,24,32,0.22))
        sd.rounded_rectangle(
            (ID_X + 2, ID_Y + 3 + y_off,
             ID_X + ID_W + 2, ID_Y + ID_H + 3 + y_off),
            radius=6, fill=(18, 24, 32, 56),
        )
        # Cover: CSS 2px 3px 18px rgba(18,24,32,0.20)
        sd.rounded_rectangle(
            (COVER_X + 2, COVER_Y + 3 + y_off,
             COVER_X + COVER_W + 2, COVER_Y + COVER_H + 3 + y_off),
            radius=6, fill=(18, 24, 32, 51),
        )

        shadows = _gpu_blur(shadows, 6)

        # Zero shadow alpha wherever any panel content will be drawn.
        sh_arr = np.array(shadows, dtype=np.uint8)
        sh_arr[:, :, 3] = (sh_arr[:, :, 3].astype(np.uint16)
                           * inv_mask.astype(np.uint16)
                           // 255).astype(np.uint8)
        img.alpha_composite(Image.fromarray(sh_arr, 'RGBA'))

    # ── content-element shadows (between panel bg and bar shell) ──

    def _draw_content_shadows(self, img: Image.Image, y_off: int) -> None:
        """Bar-shell + number-plate shadows.  Drawn ABOVE panel bg but
        BELOW bar-shell content so they sit on the cover surface without
        darkening through the bar-shell polygons."""
        w, h = self.WIDTH, self.HEIGHT
        shadows = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        sd = ImageDraw.Draw(shadows)

        # Bar-shell (XT box) shadow
        bx = BOX_X
        by = BOX_Y + y_off
        lh = BOX_H
        left_poly = [
            (bx, by), (bx + 26, by), (bx + 26, by + lh), (bx, by + lh),
            (bx, by + int(lh * 0.75)), (bx + 13, by + int(lh * 0.75)),
            (bx + 13, by + int(lh * 0.25)), (bx, by + int(lh * 0.25)),
        ]
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
        sd.polygon(_offset_poly(left_poly, 2, 4), fill=(22, 28, 38, 34))
        sd.polygon(_offset_poly(clip_poly, 2, 4), fill=(22, 28, 38, 32))

        # Number-plate shadow
        num_x = BOX_X + int(BOX_W * 0.58) + 4
        num_y = BOX_Y + int(BOX_H * 0.82) - 3 + y_off
        num_h = 20
        left_w = int(220 * 0.55)
        gap_n = 3
        right_w = 220 - left_w - gap_n
        right_x = num_x + left_w + gap_n
        sd.rectangle(
            (num_x - 4, num_y + 2, num_x + left_w - 1, num_y + num_h - 1),
            fill=(0, 0, 0, 22),
        )
        sd.rectangle(
            (right_x - 4, num_y + 2, right_x + right_w - 1, num_y + num_h - 1),
            fill=(0, 0, 0, 20),
        )

        shadows = _gpu_blur(shadows, 6)
        img.alpha_composite(shadows)

    # ── identity plate background ───────────────────────────────────

    def _id_plate_mask(self, y_off: int) -> Image.Image:
        """Rounded-rect mask for id-plate (cached per y_off)."""
        c = getattr(self, '_id_plate_mask_cache', {})
        if y_off in c:
            return c[y_off]
        m = Image.new('L', (self.WIDTH, self.HEIGHT), 0)
        ImageDraw.Draw(m).rounded_rectangle(
            (ID_X, ID_Y + y_off,
             ID_X + ID_W - 1, ID_Y + ID_H - 1 + y_off),
            radius=6, fill=255,
        )
        c[y_off] = m
        self._id_plate_mask_cache = c
        return m

    def _all_content_mask(self, y_off: int) -> Image.Image:
        """Combined mask of all HP panel content areas (id-plate + cover).

        Used for shadow clipping so that *no* drop shadow composites on top
        of *any* content surface, even when one element's shadow blur extends
        into a neighbouring element's region.  Cached per y_off.
        """
        c = getattr(self, '_all_content_mask_cache', {})
        if y_off in c:
            return c[y_off]
        m = Image.new('L', (self.WIDTH, self.HEIGHT), 0)
        d = ImageDraw.Draw(m)
        d.rounded_rectangle(
            (ID_X, ID_Y + y_off,
             ID_X + ID_W - 1, ID_Y + ID_H - 1 + y_off),
            radius=6, fill=255,
        )
        d.rounded_rectangle(
            (COVER_X, COVER_Y + y_off,
             COVER_X + COVER_W - 1, COVER_Y + COVER_H - 1 + y_off),
            radius=6, fill=255,
        )
        c[y_off] = m
        self._all_content_mask_cache = c
        return m

    def _draw_id_plate_bg(self, img: Image.Image, y_off: int) -> None:
        w, h = self.WIDTH, self.HEIGHT
        # Gradient 175deg ≈ top→bottom-right lean (approx vertical)
        grad = np.zeros((h, 1, 4), dtype=np.uint8)
        ys = np.linspace(0, 1, h)
        mid = np.clip((ys - 0.0) / 0.45, 0, 1)
        # Three-stop gradient: A (0%) → mid (45%) → B (100%)
        MID = COVER_MID
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

        # Shadow is drawn earlier by _draw_panel_shadows().
        # Build content layer: keep RGB at full intensity, set alpha = mask.
        # (paste-with-mask would halve RGB at AA edges, leaking shadow.)
        plate_arr = np.array(plate, dtype=np.uint8)
        plate_arr[:, :, 3] = np.array(mask, dtype=np.uint8)
        img.alpha_composite(Image.fromarray(plate_arr, 'RGBA'))

        # CSS inset 0 0 22px rgba(255,255,255,0.14) — soft inner glow.
        _apply_inset_shadow(
            img, mask, (255, 255, 255), alpha=36, blur_radius=11.0,
        )
        # CSS inset 0 1px 0 rgba(255,255,255,0.38) — crisp 1px top hi-lite.
        top_hi = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(top_hi).rectangle(
            (ID_X, ID_Y + y_off, ID_X + ID_W - 1, ID_Y + y_off),
            fill=(255, 255, 255, 97),
        )
        img.alpha_composite(_clip_alpha(top_hi, mask))

        # Inset highlight (top 1 px line) — kept for the lower mid wash
        ov = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        od = ImageDraw.Draw(ov)
        od.rounded_rectangle(
            (ID_X + 8, ID_Y + ID_H // 2 + y_off,
             ID_X + ID_W - 8, ID_Y + ID_H - 6 + y_off),
            radius=5, fill=(74, 80, 90, 8),
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
        cache = getattr(self, '_id_scanline_band_cache', {})
        band = cache.get(band_h)
        if band is None:
            arr = np.zeros((band_h, ID_W, 4), dtype=np.uint8)
            ys = np.linspace(0, 1, band_h)
            a_env = np.sin(ys * math.pi) * 18
            arr[:, :, 0] = 104
            arr[:, :, 1] = 228
            arr[:, :, 2] = 255
            arr[:, :, 3] = a_env[:, None].astype(np.uint8)
            band = Image.fromarray(arr, 'RGBA')
            cache[band_h] = band
            self._id_scanline_band_cache = cache
        bx = ID_X
        by = band_y - band_h // 2
        mask = self._id_plate_mask(y_off).crop((bx, by, bx + ID_W, by + band_h))
        img.alpha_composite(_clip_alpha(band, mask), (bx, by))

    def _draw_id_plate_text(self, img: Image.Image, y_off: int,
                            now: float, mode: str = 'both') -> None:
        draw_shadow = mode != 'content'
        draw_content = mode != 'shadow'
        draw = ImageDraw.Draw(img, 'RGBA')
        # Top row: "SYSTEM" + profession
        if draw_content:
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
            if draw_shadow:
                draw.text(
                    (ID_X + 78, ID_Y + 6 + y_off),
                    prof_text, fill=(243, 175, 18, 38), font=prof_font,
                )
            if draw_content:
                _draw_tracked(
                    draw, (ID_X + 78, ID_Y + 6 + y_off),
                    prof_text, font=prof_font, fill=PROF_GOLD,
                    spacing=1.5,  # web: letter-spacing 1.5px
                )
        # Lv.X (top-right, gold)  — web: 16px, letter-spacing 2px
        if draw_content:
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
            if draw_shadow:
                draw.text(
                    (bx, by), txt,
                    fill=(col[0], col[1], col[2], 70), font=bt_font,
                )
            if draw_content:
                draw.text((bx, by), txt, fill=col, font=bt_font)
        elif draw_content:
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
        if draw_shadow:
            draw.text(
                (ID_X + 18, ID_Y + ID_H - 34 + y_off),
                name, fill=(255, 255, 255, 22), font=name_font,
            )
        if draw_content:
            draw.text(
                (ID_X + 18, ID_Y + ID_H - 34 + y_off),
                name, fill=TEXT_MAIN, font=name_font,
            )

        if not draw_content:
            return
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
        c = getattr(self, '_cover_mask_cache', {})
        if y_off in c:
            return c[y_off]
        m = Image.new('L', (self.WIDTH, self.HEIGHT), 0)
        ImageDraw.Draw(m).rounded_rectangle(
            (COVER_X, COVER_Y + y_off,
             COVER_X + COVER_W - 1, COVER_Y + COVER_H - 1 + y_off),
            radius=6, fill=255,
        )
        c[y_off] = m
        self._cover_mask_cache = c
        return m

    def _draw_hp_cover_bg(self, img: Image.Image, y_off: int) -> None:
        w, h = self.WIDTH, self.HEIGHT
        grad = np.zeros((h, 1, 4), dtype=np.uint8)
        ys = np.linspace(0, 1, h)
        MID = COVER_MID
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

        # Shadow is drawn earlier by _draw_panel_shadows().
        # Build content layer: keep RGB at full intensity, set alpha = mask.
        cover_arr = np.array(cover, dtype=np.uint8)
        cover_arr[:, :, 3] = np.array(mask, dtype=np.uint8)
        img.alpha_composite(Image.fromarray(cover_arr, 'RGBA'))

        # CSS inset 0 0 22px rgba(255,255,255,0.14) — soft inner glow.
        _apply_inset_shadow(
            img, mask, (255, 255, 255), alpha=36, blur_radius=11.0,
        )
        # CSS inset 0 1px 0 rgba(255,255,255,0.38) — crisp 1px top hi-lite.
        top_hi = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(top_hi).rectangle(
            (COVER_X, COVER_Y + y_off,
             COVER_X + COVER_W - 1, COVER_Y + y_off),
            fill=(255, 255, 255, 97),
        )
        img.alpha_composite(_clip_alpha(top_hi, mask))

        ov = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        od = ImageDraw.Draw(ov)
        od.rounded_rectangle(
            (COVER_X + 8, COVER_Y + COVER_H // 2 + y_off,
             COVER_X + COVER_W - 8, COVER_Y + COVER_H - 6 + y_off),
            radius=5, fill=(74, 80, 90, 8),
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

        # Bar shell shadow is drawn in _draw_panel_shadows() (below
        # all panel content) to avoid darkening the cover surface.

        # Build polygon mask
        shape_mask = Image.new('L', (w, h), 0)
        ImageDraw.Draw(shape_mask).polygon(clip_poly, fill=255)
        # Web parity: xt_right keeps a solid left half, then fades away on the
        # right so the hp-bg-cover shell shows through under the number plate.
        grad = np.zeros((h, w, 4), dtype=np.uint8)
        xs = np.arange(rw, dtype=np.float32) / max(1, rw - 1)
        shade = (1.0 - 0.03 * np.clip(xs, 0.0, 1.0)).astype(np.float32)
        alpha_profile = np.ones(rw, dtype=np.float32)
        fade_mask = xs > 0.50
        alpha_profile[fade_mask] = np.clip(1.0 - (xs[fade_mask] - 0.50) / 0.50, 0.0, 1.0)
        grad[:, rx0:rx0 + rw, 0] = np.clip(BOX_BG[0] * shade, 0, 255).astype(np.uint8)
        grad[:, rx0:rx0 + rw, 1] = np.clip(BOX_BG[1] * shade, 0, 255).astype(np.uint8)
        grad[:, rx0:rx0 + rw, 2] = np.clip(BOX_BG[2] * shade, 0, 255).astype(np.uint8)
        grad[:, rx0:rx0 + rw, 3] = np.clip(alpha_profile * 255.0, 0, 255).astype(np.uint8)
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

        self._draw_hp_number_shell(img, y_off)

    def _number_plate_mask(self, y_off: int) -> Image.Image:
        c = getattr(self, '_number_plate_mask_cache', {})
        if y_off in c:
            return c[y_off]
        mask = Image.new('L', (self.WIDTH, self.HEIGHT), 0)
        num_x = BOX_X + int(BOX_W * 0.58) + 4
        num_y = BOX_Y + int(BOX_H * 0.82) - 3 + y_off
        num_w = 220
        num_h = 20
        poly = [
            (num_x + 14, num_y),
            (num_x + num_w, num_y),
            (num_x + num_w, num_y + num_h),
            (num_x, num_y + num_h),
            (num_x, num_y + int(num_h * 0.58)),
        ]
        ImageDraw.Draw(mask).polygon(poly, fill=255)
        c[y_off] = mask
        self._number_plate_mask_cache = c
        return mask

    def _draw_hp_number_shell(self, img: Image.Image, y_off: int) -> None:
        num_x = BOX_X + int(BOX_W * 0.58) + 4
        num_y = BOX_Y + int(BOX_H * 0.82) - 3 + y_off
        num_h = 20
        left_w = int(220 * 0.55)
        gap = 3
        right_w = 220 - left_w - gap
        right_x = num_x + left_w + gap
        mask = self._number_plate_mask(y_off)

        # Number plate shadow is drawn in _draw_panel_shadows()
        # (below all panel content) to avoid darkening the cover surface.

        plate = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        pd = ImageDraw.Draw(plate, 'RGBA')
        pd.rectangle(
            (num_x, num_y, num_x + left_w - 1, num_y + num_h - 1),
            fill=BOX_BG,
        )
        pd.rectangle(
            (right_x, num_y, right_x + right_w - 1, num_y + num_h - 1),
            fill=BOX_BG,
        )
        pd.line((num_x + 1, num_y, num_x + left_w - 3, num_y), fill=(255, 255, 255, 150))
        pd.line((right_x + 1, num_y, right_x + right_w - 3, num_y), fill=(255, 255, 255, 150))
        pd.line((num_x + 2, num_y + num_h - 1, num_x + left_w - 2, num_y + num_h - 1), fill=(220, 220, 220, 120))
        pd.line((right_x + 2, num_y + num_h - 1, right_x + right_w - 2, num_y + num_h - 1), fill=(220, 220, 220, 120))
        img.alpha_composite(_clip_alpha(plate, mask))

    def _hp_bar_mask(self, y_off: int) -> Image.Image:
        c = getattr(self, '_hp_bar_mask_cache', {})
        if y_off in c:
            return c[y_off]
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
        c[y_off] = m
        self._hp_bar_mask_cache = c
        return m

    def _hp_gradient(self, pct: float) -> Tuple[Tuple, Tuple]:
        if pct >= 0.60:
            return HP_GREEN
        if pct >= 0.25:
            return HP_YELLOW
        return HP_RED

    def _hp_fill_width_px(self, pct: float) -> float:
        # v2.2.10: returns float so subpixel_bar_width can fade the trailing
        # column instead of snapping the bar 1 px at a time during slow HP
        # drains/regens.
        value = max(0.0, min(1.0, float(pct or 0.0)))
        if value <= 0.0:
            return 0.0
        if value >= 0.997:
            return 350.0 + 6.0
        return 350.0 * value + 8.0

    def _draw_hp_fill(self, img: Image.Image, y_off: int,
                      now: float) -> None:
        pct = max(0.0, min(1.0, self._hp_pct_disp))
        if pct <= 0.002:
            return
        # Web width math: 0 → 0, full → 100% + 6px, otherwise % + 8px.
        fill_w = self._hp_fill_width_px(pct)
        if fill_w <= 0.0:
            return
        fw_int = max(1, int(math.ceil(fill_w)))
        ca, cb = self._hp_gradient(pct)
        bar = _make_hgrad_bar(fw_int, 25, ca, cb)
        bar = subpixel_bar_width(bar, fill_w) or bar
        cap = _make_skew_cap(25, cb, skew_px=7, extra=7)
        canvas = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        canvas.paste(bar, (BOX_X + 114 + 1, BOX_Y + 10 + 1 + y_off))
        if pct < 0.995:
            canvas.paste(
                cap,
                (BOX_X + 114 + fw_int - 7, BOX_Y + 10 + 1 + y_off),
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

    def _interaction_zone_at(self, lx: int, ly: int) -> Optional[str]:
        if ID_X <= lx <= ID_X + ID_W and ID_Y <= ly <= ID_Y + ID_H:
            return 'id'
        if (self._hp_group_clickable()
                and BOX_X <= lx <= BOX_X + BOX_W
                and BOX_Y <= ly <= BOX_Y + BOX_H):
            return 'hp'
        return None

    def _hp_group_clickable(self) -> bool:
        """Return whether the HP/STA group should accept pointer input."""
        return not bool(self._hp_group_hidden or self._hp_group_fade_t > 0.0)

    def _interaction_strength(self, zone: str, now: float) -> Tuple[float, float]:
        hover = max(0.0, min(1.0, float(self._hover_t.get(zone, 0.0) or 0.0)))
        press = max(0.0, min(1.0, float(self._press_t.get(zone, 0.0) or 0.0)))
        flash_until = float(self._press_flash_t.get(zone, 0.0) or 0.0)
        flash = 0.0
        if flash_until > now:
            flash = max(0.0, min(1.0, (flash_until - now) / 0.22))
        return hover, max(press, flash)

    def _click_flash_progress(self, zone: str, now: float) -> float:
        flash_until = float(self._press_flash_t.get(zone, 0.0) or 0.0)
        if flash_until <= now:
            return 0.0
        return max(0.0, min(1.0, 1.0 - ((flash_until - now) / 0.22)))

    def _draw_rounded_outline_fade(
        self,
        img: Image.Image,
        rect: Tuple[int, int, int, int],
        radius: int,
        color: Tuple[int, int, int],
        alpha: int,
    ) -> None:
        a = max(0, min(255, int(alpha)))
        if a <= 0:
            return
        layer = Image.new('RGBA', img.size, (0, 0, 0, 0))
        d = ImageDraw.Draw(layer, 'RGBA')
        for inset, gain, width in ((2, 0.22, 1), (1, 0.52, 1), (0, 1.0, 2)):
            x0, y0, x1, y1 = rect
            d.rounded_rectangle(
                (x0 - inset, y0 - inset, x1 + inset, y1 + inset),
                radius=max(1, radius + inset),
                outline=(color[0], color[1], color[2], int(a * gain)),
                width=width,
            )
        img.alpha_composite(layer)

    def _draw_polygon_outline_fade(
        self,
        img: Image.Image,
        points,
        color: Tuple[int, int, int],
        alpha: int,
    ) -> None:
        a = max(0, min(255, int(alpha)))
        if a <= 0:
            return
        layer = Image.new('RGBA', img.size, (0, 0, 0, 0))
        d = ImageDraw.Draw(layer, 'RGBA')
        for gain, width in ((0.22, 4), (0.52, 3), (1.0, 2)):
            d.polygon(points, outline=(color[0], color[1], color[2], int(a * gain)), width=width)
        img.alpha_composite(layer)

    def _zone_bbox(self, zone: str, y_off: int) -> Tuple[int, int, int, int]:
        if zone == 'id':
            return (ID_X, ID_Y + y_off, ID_X + ID_W, ID_Y + ID_H + y_off)
        num_x = BOX_X + int(BOX_W * 0.58) + 4
        num_y = BOX_Y + int(BOX_H * 0.82) - 3 + y_off
        return (
            BOX_X - 2,
            BOX_Y + y_off - 2,
            max(BOX_X + BOX_W + 2, num_x + 220 + 4),
            max(BOX_Y + BOX_H + y_off + 2, num_y + 20 + 4),
        )

    def _zone_center(self, zone: str, y_off: int) -> Tuple[float, float]:
        x0, y0, x1, y1 = self._zone_bbox(zone, y_off)
        return ((x0 + x1) * 0.5, (y0 + y1) * 0.5)

    def _scale_region_overlay(
        self,
        src: Image.Image,
        zone: str,
        y_off: int,
        scale: float,
        alpha: float,
    ) -> Optional[Tuple[Image.Image, Tuple[int, int]]]:
        if scale <= 1.001 or alpha <= 0.001:
            return None
        x0, y0, x1, y1 = self._zone_bbox(zone, y_off)
        x0 = max(0, min(self.WIDTH, int(x0)))
        y0 = max(0, min(self.HEIGHT, int(y0)))
        x1 = max(x0 + 1, min(self.WIDTH, int(x1)))
        y1 = max(y0 + 1, min(self.HEIGHT, int(y1)))
        region = src.crop((x0, y0, x1, y1))
        rw, rh = region.size
        tw = max(1, int(round(rw * scale)))
        th = max(1, int(round(rh * scale)))
        enlarged = region.resize((tw, th), Image.LANCZOS)
        if alpha < 0.999:
            arr = np.asarray(enlarged, dtype=np.uint8).copy()
            mul = int(max(0, min(255, alpha * 255)))
            arr[:, :, 3] = (arr[:, :, 3].astype(np.uint16) * mul // 255).astype(np.uint8)
            enlarged = Image.fromarray(arr, 'RGBA')
        dx = x0 - (tw - rw) // 2
        dy = y0 - (th - rh) // 2
        return enlarged, (dx, dy)

    def _draw_click_sweep(self, fx: Image.Image, zone: str, y_off: int, now: float) -> None:
        prog = self._click_flash_progress(zone, now)
        if prog <= 0.0:
            return
        x0, y0, x1, y1 = self._zone_bbox(zone, y_off)
        x0 = max(0, min(self.WIDTH, int(x0)))
        y0 = max(0, min(self.HEIGHT, int(y0)))
        x1 = max(x0 + 1, min(self.WIDTH, int(x1)))
        y1 = max(y0 + 1, min(self.HEIGHT, int(y1)))
        rw = max(1, x1 - x0)
        rh = max(1, y1 - y0)
        arr = np.zeros((rh, rw, 4), dtype=np.uint8)
        xs = np.arange(rw, dtype=np.float32)[None, :]
        ys = np.arange(rh, dtype=np.float32)[:, None]
        band_x = (-0.22 + 1.44 * prog) * rw
        skew = (ys - rh * 0.5) * 0.20
        sigma = max(10.0, rw * 0.055)
        env = np.exp(-((xs - (band_x + skew)) ** 2) / (2.0 * sigma * sigma))
        alpha_scale = self._hp_group_alpha_now(now) if zone == 'hp' else 1.0
        alpha = np.clip(env * (70.0 * (1.0 - prog) + 38.0) * alpha_scale, 0, 255).astype(np.uint8)
        arr[:, :, 0] = 243
        arr[:, :, 1] = 175
        arr[:, :, 2] = 18
        arr[:, :, 3] = alpha
        sweep = Image.fromarray(arr, 'RGBA')
        if zone == 'id':
            mask = self._id_plate_mask(y_off).crop((x0, y0, x1, y1))
        else:
            mask_cache = getattr(self, '_hp_interaction_mask_cache', {})
            mask = mask_cache.get(y_off)
            if mask is None:
                mask = Image.new('L', (self.WIDTH, self.HEIGHT), 0)
                md = ImageDraw.Draw(mask)
                bx = BOX_X
                by = BOX_Y + y_off
                lh = BOX_H
                left_poly = [
                    (bx, by), (bx + 26, by), (bx + 26, by + lh), (bx, by + lh),
                    (bx, by + int(lh * 0.75)), (bx + 13, by + int(lh * 0.75)),
                    (bx + 13, by + int(lh * 0.25)), (bx, by + int(lh * 0.25)),
                ]
                rx0 = bx + 29
                rw2 = BOX_W - 29
                clip_poly = [
                    (rx0 + 85, by + int(lh * 0.22)),
                    (rx0 + rw2, by + int(lh * 0.22)),
                    (rx0 + rw2, by),
                    (rx0, by),
                    (rx0, by + lh),
                    (rx0 + 228, by + lh),
                    (rx0 + 234, by + int(lh * 0.77)),
                    (rx0 + rw2, by + int(lh * 0.77)),
                    (rx0 + rw2, by + int(lh * 0.60)),
                    (rx0 + 233, by + int(lh * 0.60)),
                    (rx0 + 228, by + int(lh * 0.77)),
                    (rx0 + 85, by + int(lh * 0.77)),
                ]
                md.polygon(left_poly, fill=255)
                md.polygon(clip_poly, fill=255)
                num_x = BOX_X + int(BOX_W * 0.58) + 4
                num_y = BOX_Y + int(BOX_H * 0.82) - 3 + y_off
                left_w = int(220 * 0.55)
                gap = 3
                right_w = 220 - left_w - gap
                right_x = num_x + left_w + gap
                md.rectangle((num_x, num_y, num_x + left_w - 1, num_y + 19), fill=255)
                md.rectangle((right_x, num_y, right_x + right_w - 1, num_y + 19), fill=255)
                mask_cache[y_off] = mask
                self._hp_interaction_mask_cache = mask_cache
            mask = mask.crop((x0, y0, x1, y1))
        fx.alpha_composite(_clip_alpha(sweep, mask), (x0, y0))

    def _draw_interaction_fx(self, img: Image.Image, y_off: int, now: float) -> None:
        id_hover, id_press = self._interaction_strength('id', now)
        hp_hover, hp_press = self._interaction_strength('hp', now)
        if id_hover <= 0.001 and id_press <= 0.001 and hp_hover <= 0.001 and hp_press <= 0.001:
            return
        hp_alpha_scale = self._hp_group_alpha_now(now)
        def _q(v: float, steps: int = 64) -> int:
            v = max(0.0, min(1.0, float(v or 0.0)))
            if v <= 0.001:
                return 0
            return max(1, int(math.ceil(v * steps - 1e-6)))
        sig = (
            y_off,
            _q(id_hover),
            _q(id_press),
            _q(hp_hover),
            _q(hp_press),
            _q(hp_alpha_scale, 72),
            _q(self._click_flash_progress('id', now), 72),
            _q(self._click_flash_progress('hp', now), 72),
        )
        if self._interaction_fx_cache is not None and self._interaction_fx_sig == sig:
            img.alpha_composite(self._interaction_fx_cache)
            return

        w, h = self.WIDTH, self.HEIGHT
        fx = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        draw = ImageDraw.Draw(fx, 'RGBA')
        scale_layers = []

        id_scale = 1.0 + 0.060 * id_press
        id_overlay = self._scale_region_overlay(
            img, 'id', y_off, id_scale, 0.20 * id_press)
        if id_overlay is not None:
            scale_layers.append(id_overlay)

        hp_scale = 1.0 + 0.072 * hp_press
        hp_overlay = self._scale_region_overlay(
            img, 'hp', y_off, hp_scale, 0.22 * hp_press * hp_alpha_scale)
        if hp_overlay is not None:
            scale_layers.append(hp_overlay)

        for layer, pos in scale_layers:
            fx.alpha_composite(layer, pos)

        if id_hover > 0.001 or id_press > 0.001:
            mask = self._id_plate_mask(y_off)
            alpha = int(40 * id_hover + 64 * id_press)
            shade = Image.new('RGBA', (w, h), (246, 248, 250, max(0, min(84, alpha))))
            fx.alpha_composite(_clip_alpha(shade, mask))
            x0, y0, x1, y1 = self._zone_bbox('id', y_off)
            ring_alpha = int(92 * id_hover + 132 * id_press)
            halo = Image.new('RGBA', (w, h), (0, 0, 0, 0))
            hd = ImageDraw.Draw(halo, 'RGBA')
            hd.rounded_rectangle(
                (x0 - 5, y0 - 5, x1 + 5, y1 + 5),
                radius=10,
                outline=(243, 175, 18, max(0, min(255, ring_alpha))),
                width=2,
            )
            halo = _gpu_blur(halo, 3)
            fx.alpha_composite(halo)
            self._draw_rounded_outline_fade(
                fx,
                (ID_X, ID_Y + y_off, ID_X + ID_W - 1, ID_Y + ID_H - 1 + y_off),
                radius=6,
                color=(243, 175, 18),
                alpha=int(84 * id_hover + 126 * id_press),
            )
            self._draw_click_sweep(fx, 'id', y_off, now)

        if hp_hover > 0.001 or hp_press > 0.001:
            bx = BOX_X
            by = BOX_Y + y_off
            lh = BOX_H
            left_poly = [
                (bx, by), (bx + 26, by), (bx + 26, by + lh), (bx, by + lh),
                (bx, by + int(lh * 0.75)), (bx + 13, by + int(lh * 0.75)),
                (bx + 13, by + int(lh * 0.25)), (bx, by + int(lh * 0.25)),
            ]
            rx0 = bx + 29
            rw = BOX_W - 29
            clip_poly = [
                (rx0 + 85, by + int(lh * 0.22)),
                (rx0 + rw, by + int(lh * 0.22)),
                (rx0 + rw, by),
                (rx0, by),
                (rx0, by + lh),
                (rx0 + 228, by + lh),
                (rx0 + 234, by + int(lh * 0.77)),
                (rx0 + rw, by + int(lh * 0.77)),
                (rx0 + rw, by + int(lh * 0.60)),
                (rx0 + 233, by + int(lh * 0.60)),
                (rx0 + 228, by + int(lh * 0.77)),
                (rx0 + 85, by + int(lh * 0.77)),
            ]
            shell_overlay = Image.new('RGBA', (w, h), (0, 0, 0, 0))
            sd = ImageDraw.Draw(shell_overlay, 'RGBA')
            shell_fill = (
                243, 245, 247,
                int((12 + 24 * hp_hover + 20 * hp_press) * hp_alpha_scale),
            )
            sd.polygon(left_poly, fill=shell_fill)
            sd.polygon(clip_poly, fill=shell_fill)
            num_x = BOX_X + int(BOX_W * 0.58) + 4
            num_y = BOX_Y + int(BOX_H * 0.82) - 3 + y_off
            num_h = 20
            left_w = int(220 * 0.55)
            gap = 3
            right_w = 220 - left_w - gap
            right_x = num_x + left_w + gap
            plate_fill = (
                243, 245, 247,
                int((14 + 26 * hp_hover + 24 * hp_press) * hp_alpha_scale),
            )
            sd.rectangle((num_x, num_y, num_x + left_w - 1, num_y + num_h - 1), fill=plate_fill)
            sd.rectangle((right_x, num_y, right_x + right_w - 1, num_y + num_h - 1), fill=plate_fill)
            fx.alpha_composite(shell_overlay)
            x0, y0, x1, y1 = self._zone_bbox('hp', y_off)
            ring_alpha = int((88 * hp_hover + 136 * hp_press) * hp_alpha_scale)
            halo = Image.new('RGBA', (w, h), (0, 0, 0, 0))
            hd = ImageDraw.Draw(halo, 'RGBA')
            hd.rounded_rectangle(
                (x0 - 6, y0 - 6, x1 + 6, y1 + 6),
                radius=10,
                outline=(243, 175, 18, max(0, min(255, ring_alpha))),
                width=2,
            )
            halo = _gpu_blur(halo, 3)
            fx.alpha_composite(halo)
            border_alpha = int((90 * hp_hover + 132 * hp_press) * hp_alpha_scale)
            bar_border_alpha = int((110 * hp_hover + 152 * hp_press) * hp_alpha_scale)
            self._draw_polygon_outline_fade(
                fx, left_poly, (243, 175, 18), border_alpha)
            self._draw_polygon_outline_fade(
                fx, clip_poly, (243, 175, 18), border_alpha)
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
            self._draw_polygon_outline_fade(
                fx, poly, (243, 175, 18), bar_border_alpha)
            self._draw_click_sweep(fx, 'hp', y_off, now)

        self._interaction_fx_cache = fx
        self._interaction_fx_sig = sig
        img.alpha_composite(fx)

    def _play_interaction_sound(self, volume: float) -> None:
        now = time.time()
        if now - self._last_interaction_sound_t < 0.05:
            return
        self._last_interaction_sound_t = now
        try:
            from sao_sound import play_sound as _ps
            _ps('click', volume=volume)
        except Exception:
            pass

    def _draw_hp_text(self, img: Image.Image, y_off: int,
                      mode: str = 'both') -> None:
        draw_shadow = mode != 'content'
        draw_content = mode != 'shadow'
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
        if draw_shadow:
            draw.text(
                (nx + 1, ny + 1), name,
                fill=(28, 34, 42, 58), font=name_font,
            )
        if draw_content:
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
        if draw_shadow:
            draw.text(
                (nx0 + cell_w - vw - 5, ny0 + 1), val_txt,
                fill=(28, 34, 42, 54), font=val_font,
            )
        if draw_content:
            draw.text((nx0 + cell_w - vw - 6, ny0), val_txt,
                      fill=TEXT_MAIN, font=val_font)
        # Lv in second cell
        lv_font = _load_font('sao', 12)
        lv_txt = f'lv.{self._level}'
        if draw_shadow:
            draw.text(
                (nx0 + cell_w + 7, ny0 + 2), lv_txt,
                fill=(28, 34, 42, 48), font=lv_font,
            )
        if draw_content:
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
        # v2.1.17: float width + subpixel trailing column so STA drain looks
        # continuous instead of stepping 1 px every few ticks.
        fw = (tx1 - tx0) * pct
        if fw <= 0.0:
            return
        fw_int = max(1, int(math.ceil(fw)))
        fill_q = int(round(fw * 2.0))
        cache = getattr(self, '_sta_fill_cache', {})
        asset = cache.get(fill_q)
        if asset is None:
            bar = _make_hgrad_bar(max(1, fw_int - 2), 4, STA_A, STA_B)
            bar = subpixel_bar_width(bar, max(0.0, fw - 2.0)) or bar
            highlight = Image.new('RGBA', (max(1, fw_int - 2), 1), (255, 242, 202, 72))
            glow = Image.new('RGBA', (fw_int + 12, 14), (0, 0, 0, 0))
            ImageDraw.Draw(glow).rectangle(
                (6, 4, fw_int + 6, 10), fill=(243, 175, 18, 60),
            )
            glow = _gpu_blur(glow, 3)
            layer_w = max(bar.size[0], glow.size[0])
            layer_h = max(5, glow.size[1])
            asset = Image.new('RGBA', (layer_w, layer_h), (0, 0, 0, 0))
            asset.alpha_composite(glow, (0, 0))
            asset.alpha_composite(bar, (6 if glow.size[0] >= bar.size[0] + 12 else 0, 5))
            asset.alpha_composite(highlight, (6 if glow.size[0] >= highlight.size[0] + 12 else 0, 5))
            cache[fill_q] = asset
            self._sta_fill_cache = cache
        img.alpha_composite(asset, (tx0 - 6, ty0 - 4))

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

    def _gpu_event(self, x: float, y: float, delta: int = 0):
        lx = int(round(x))
        ly = int(round(y))
        return SimpleNamespace(
            x=lx, y=ly,
            x_root=int(self._x + lx),
            y_root=int(self._y + ly),
            delta=int(delta),
        )

    def _on_gpu_cursor_pos(self, x: float, y: float) -> None:
        ev = self._gpu_event(x, y)
        if self._gpu_drag_active:
            self._on_drag_move(ev)
        else:
            self._on_pointer_move(ev)

    def _on_gpu_cursor_leave(self) -> None:
        if not self._gpu_drag_active:
            self._on_pointer_leave(None)

    def _on_gpu_mouse_button(self, button: int, action: int,
                             _mods: int, x: float, y: float) -> None:
        ev = self._gpu_event(x, y)
        if button == 0:
            if action == 1:
                self._gpu_drag_active = True
                self._on_drag_start(ev)
            elif action == 0:
                self._gpu_drag_active = False
                self._on_drag_end(ev)
        elif button == 1 and action == 1:
            self._on_context_menu(ev)

    def _on_drag_start(self, ev) -> None:
        try:
            self._drag_ox = ev.x_root - self._x
            self._drag_oy = ev.y_root - self._y
            self._drag_origin = (ev.x_root, ev.y_root)
            self._drag_moved = False
            lx = int(ev.x_root - self._x)
            ly = int(ev.y_root - self._y)
            self._press_zone = self._interaction_zone_at(lx, ly)
            self._hover_zone = self._press_zone
            self._schedule_tick(immediate=True)
        except Exception:
            self._drag_ox = 0
            self._drag_oy = 0
            self._drag_origin = (0, 0)
            self._drag_moved = False
            self._press_zone = None

    def _on_drag_move(self, ev) -> None:
        try:
            ox, oy = self._drag_origin
            if not self._drag_moved:
                if (abs(ev.x_root - ox) < self._TAP_THRESHOLD_PX and
                        abs(ev.y_root - oy) < self._TAP_THRESHOLD_PX):
                    self._hover_zone = self._interaction_zone_at(
                        int(ev.x_root - self._x), int(ev.y_root - self._y))
                    return
                self._drag_moved = True
                self._press_zone = None
            self._x = int(ev.x_root - self._drag_ox)
            self._y = int(ev.y_root - self._drag_oy)
            if self._gpu_managed and self._gpu_window is not None:
                try:
                    self._gpu_window.set_geometry(
                        self._x, self._y, self.WIDTH, self.HEIGHT)
                except Exception:
                    pass
            elif self._win is not None and self._win is not self:
                self._win.geometry(f'+{self._x}+{self._y}')
            self._schedule_tick(immediate=True)
        except Exception:
            pass

    def _point_in_click_zones(self, lx: int, ly: int) -> bool:
        """Web parity: only id-plate and hp-bar (XTBox) regions are
        clickable. Cover, STA row, and blank gutters are inert."""
        if ID_X <= lx <= ID_X + ID_W and ID_Y <= ly <= ID_Y + ID_H:
            return True
        if (self._hp_group_clickable()
                and BOX_X <= lx <= BOX_X + BOX_W
                and BOX_Y <= ly <= BOX_Y + BOX_H):
            return True
        return False

    def _on_pointer_move(self, ev) -> None:
        try:
            zone = self._interaction_zone_at(int(ev.x), int(ev.y))
            if zone and zone != self._hover_sound_zone:
                self._hover_sound_zone = zone
                self._play_interaction_sound(0.3)
            elif zone is None:
                self._hover_sound_zone = None
            self._hover_zone = zone
            self._schedule_tick(immediate=True)
        except Exception:
            pass

    def _on_pointer_leave(self, _ev) -> None:
        self._hover_zone = None
        self._hover_sound_zone = None
        if self._press_zone is None:
            self._schedule_tick(immediate=True)

    def _on_drag_end(self, ev) -> None:
        moved = getattr(self, '_drag_moved', False)
        self._press_zone = None
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
        zone = self._interaction_zone_at(lx, ly)
        self._hover_zone = zone
        if zone is None:
            self._schedule_tick(immediate=True)
            return
        self._press_flash_t[zone] = time.time() + 0.22
        self._play_interaction_sound(0.5)
        self._schedule_tick(immediate=True)
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
        if self._gpu_managed and self._gpu_window is not None:
            try:
                self._gpu_window.set_geometry(
                    self._x, self._y, self.WIDTH, self.HEIGHT)
            except Exception:
                pass
        elif self._win is not None and self._win is not self:
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
