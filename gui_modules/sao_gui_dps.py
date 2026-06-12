# -*- coding: utf-8 -*-
"""
sao_gui_dps.py — Animated SAO DPS overlay (tkinter edition)

This is a full port of the webview DPS panel (`web/dps.html`) into tkinter.
Rendering is done via PIL onto a WS_EX_LAYERED window and committed with
UpdateLayeredWindow for per-pixel alpha. A 60 FPS animation loop tweens:

  - Entity bar widths (ease-out)
  - Entity damage / DPS numbers (roll-up)
  - Row Y positions on reorder
  - Panel hit-flash tint (impact / mega / starburst)
  - Shell fade-in / fade-out

Public API (kept backward-compatible):
    DpsOverlay(root, settings=None)
    .show() / .hide()
    .update(snapshot)
    .set_self_uid(uid)
    .fade_in() / .fade_out()
"""

from __future__ import annotations

import os
import sys
import time
import ctypes
import math
from types import SimpleNamespace
from typing import Any, Dict, List, Optional, Tuple
from decimal import Decimal, ROUND_HALF_UP

import numpy as np
import tkinter as tk
from PIL import Image, ImageDraw, ImageFont, ImageFilter
import _sao_cy_uihelpers as _CY_UI  # type: ignore[import-not-found]
from render.gpu_renderer import gaussian_blur_rgba as _gpu_blur
from render.overlay_scheduler import get_scheduler as _get_scheduler
from render.overlay_render_worker import (
    AsyncFrameWorker, multiply_alpha_image,
)
from render.render_capture_sync import wait_until_capture_idle
from config import FONTS_DIR
from gui_modules.entity_gpu_policy import require_entity_gpu

# DPS presentation is GPU-required. GPU mode owns drag/tab/scroll input
# callbacks; do not fall back to the legacy interactive ULW path.
try:
    from render import gpu_overlay_window as _gow
except Exception:
    _gow = None  # type: ignore[assignment]


def _gpu_dps_enabled() -> bool:
    return require_entity_gpu('DpsOverlay', _gow)

import _sao_cy_pixels as _CY_PIXELS  # type: ignore[import-not-found]

from utils.perf_probe import gauge as _perf_gauge, probe as _probe

# ═══════════════════════════════════════════════
#  Win32 / ULW glue
# ═══════════════════════════════════════════════

_user32 = ctypes.windll.user32
_gdi32 = ctypes.windll.gdi32

GWL_EXSTYLE = -20
WS_EX_LAYERED = 0x00080000
WS_EX_TRANSPARENT = 0x00000020
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_TOPMOST = 0x00000008
ULW_ALPHA = 2

# SetWindowPos flags — used to flush an ex-style change so the new
# WS_EX_TRANSPARENT hit-test behaviour takes effect for the Tk/ULW path.
HWND_TOPMOST = -1
SWP_NOMOVE = 0x0002
SWP_NOSIZE = 0x0001
SWP_NOACTIVATE = 0x0010
SWP_FRAMECHANGED = 0x0020


class _POINT(ctypes.Structure):
    _fields_ = [('x', ctypes.c_long), ('y', ctypes.c_long)]


class _SIZE(ctypes.Structure):
    _fields_ = [('cx', ctypes.c_long), ('cy', ctypes.c_long)]


class _BLENDFUNCTION(ctypes.Structure):
    _fields_ = [
        ('BlendOp', ctypes.c_byte),
        ('BlendFlags', ctypes.c_byte),
        ('SourceConstantAlpha', ctypes.c_byte),
        ('AlphaFormat', ctypes.c_byte),
    ]


class _BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ('biSize', ctypes.c_uint32),
        ('biWidth', ctypes.c_int32),
        ('biHeight', ctypes.c_int32),
        ('biPlanes', ctypes.c_uint16),
        ('biBitCount', ctypes.c_uint16),
        ('biCompression', ctypes.c_uint32),
        ('biSizeImage', ctypes.c_uint32),
        ('biXPelsPerMeter', ctypes.c_int32),
        ('biYPelsPerMeter', ctypes.c_int32),
        ('biClrUsed', ctypes.c_uint32),
        ('biClrImportant', ctypes.c_uint32),
    ]


def _ulw_update(hwnd: int, img: Image.Image, x: int, y: int,
                alpha: int = 255) -> None:
    """Commit a PIL RGBA image to a layered window with per-pixel alpha.

    RGB channels are premultiplied by the alpha channel via numpy for speed.
    The previous implementation used a Python per-pixel loop (~4 FPS for a
    260×220 panel) — the vectorised version is >100× faster.
    """
    if not wait_until_capture_idle(0.010):
        return
    w, h = img.size
    hdc_screen = _user32.GetDC(0)
    hdc_mem = _gdi32.CreateCompatibleDC(hdc_screen)

    bmi = _BITMAPINFOHEADER()
    bmi.biSize = ctypes.sizeof(_BITMAPINFOHEADER)
    bmi.biWidth = w
    bmi.biHeight = -h  # top-down
    bmi.biPlanes = 1
    bmi.biBitCount = 32
    bmi.biCompression = 0

    bits = ctypes.c_void_p()
    hbm = _gdi32.CreateDIBSection(
        hdc_mem, ctypes.byref(bmi), 0, ctypes.byref(bits), None, 0
    )
    old_bm = _gdi32.SelectObject(hdc_mem, hbm)

    # Build premultiplied BGRA buffer via mandatory Cython.
    rgba = np.asarray(img, dtype=np.uint8)           # shape (h, w, 4) RGBA
    raw = _CY_PIXELS.premultiply_bgra_ndarray(rgba)
    ctypes.memmove(bits, raw, len(raw))

    pt_dst = _POINT(x, y)
    sz = _SIZE(w, h)
    pt_src = _POINT(0, 0)
    blend = _BLENDFUNCTION(0, 0, max(0, min(255, int(alpha))), 1)

    _user32.UpdateLayeredWindow(
        ctypes.c_void_p(hwnd),
        hdc_screen,
        ctypes.byref(pt_dst),
        ctypes.byref(sz),
        hdc_mem,
        ctypes.byref(pt_src),
        0,
        ctypes.byref(blend),
        ULW_ALPHA,
    )

    _gdi32.SelectObject(hdc_mem, old_bm)
    _gdi32.DeleteObject(hbm)
    _gdi32.DeleteDC(hdc_mem)
    _user32.ReleaseDC(0, hdc_screen)


# ═══════════════════════════════════════════════
#  Font helpers
# ═══════════════════════════════════════════════

_FONT_DIR = FONTS_DIR

_FONT_CACHE: Dict[tuple, Any] = {}


def _load_font(kind: str, size: int):
    """Load a PIL font (SAOUI for ASCII, ZhuZiAYuanJWD for CJK fallback)."""
    key = (kind, size)
    cached = _FONT_CACHE.get(key)
    if cached is not None:
        return cached
    try:
        from utils.sao_sound import load_sao_fonts as _load_sao_fonts
        _load_sao_fonts()
    except Exception:
        pass
    font = None
    candidates = []
    if kind == 'sao':
        candidates.extend([
            os.path.join(_FONT_DIR, 'SAOUI.ttf'),
            'segoeui.ttf', 'arial.ttf',
            os.path.join(_FONT_DIR, 'ZhuZiAYuanJWD.ttf'),
            'msyh.ttc', 'msyhbd.ttc', 'simhei.ttf',
        ])
    else:
        candidates.extend([
            os.path.join(_FONT_DIR, 'ZhuZiAYuanJWD.ttf'),
            'msyh.ttc', 'msyhbd.ttc', 'simhei.ttf', 'simsun.ttc',
            'segoeui.ttf', 'arial.ttf',
            os.path.join(_FONT_DIR, 'SAOUI.ttf'),
        ])
    for path in candidates:
        if os.path.isabs(path) and not os.path.exists(path):
            continue
        try:
            font = ImageFont.truetype(path, size)
            break
        except Exception:
            continue
    if font is None:
        for sysname in ('msyh.ttc', 'msyhbd.ttc', 'simhei.ttf', 'segoeui.ttf', 'arial.ttf'):
            try:
                font = ImageFont.truetype(sysname, size)
                break
            except Exception:
                continue
    if font is None:
        font = ImageFont.load_default()
    _FONT_CACHE[key] = font
    return font


def _has_cjk(text: str) -> bool:
    for ch in text:
        o = ord(ch)
        if (0x3000 <= o <= 0x9FFF) or (0xAC00 <= o <= 0xD7AF) or \
           (0x3040 <= o <= 0x30FF) or (0xFF00 <= o <= 0xFFEF):
            return True
    return False


def _pick_font(text: str, size: int):
    return _load_font('cjk' if _has_cjk(text) else 'sao', size)


def _text_width(draw: ImageDraw.ImageDraw, text: str, font) -> int:
    try:
        return int(draw.textlength(text, font=font))
    except Exception:
        try:
            l, t, r, b = font.getbbox(text)
            return int(r - l)
        except Exception:
            return len(text) * 6


# H5/Q8: per-glyph width memoization. DPS combat sustains ~15-21k textlength
# calls/sec across _draw_tracked / _draw_tracked_centered / _tracked_text_width
# (3 inner loops × ~5 rows × 60Hz). Cache key is (id(font), ch); font objects
# stay alive via _load_font cache so id() is stable across renders.
# Fallback fb (font.size / 2) is preserved on miss to keep numeric parity.
_GLYPH_W_DPS: Dict[Tuple[int, str], float] = {}


def _glyph_w_dps(draw: ImageDraw.ImageDraw, ch: str, font) -> float:
    key = (id(font), ch)
    v = _GLYPH_W_DPS.get(key)
    if v is not None:
        return v
    try:
        v = draw.textlength(ch, font=font)
    except Exception:
        v = font.size / 2
    _GLYPH_W_DPS[key] = v
    return v


# ═══════════════════════════════════════════════
#  Formatting (parity with dps.html _fmtNum / _fmtTime)
# ═══════════════════════════════════════════════

def _fmt_num(v: float) -> str:
    return _CY_UI.dps_fmt_num(v)


def _fmt_time(s: float) -> str:
    return _CY_UI.dps_fmt_time(s)


def _fmt_fp(fp: float) -> str:
    return _CY_UI.dps_fmt_fp(fp)


def _to_fixed_half_up(value: float, digits: int) -> str:
    return _CY_UI.dps_to_fixed_half_up(value, digits)


def _round_half_up_int(value: float) -> int:
    return int(_CY_UI.dps_round_half_up_int(value))


def _safe_int(value: Any, default: int = 0,
              min_value: Optional[int] = None,
              max_value: Optional[int] = None) -> int:
    try:
        result = int(value or 0)
    except Exception:
        try:
            as_float = float(value)
            if not math.isfinite(as_float):
                return int(default)
            result = int(as_float)
        except Exception:
            return int(default)
    if min_value is not None and result < min_value:
        result = min_value
    if max_value is not None and result > max_value:
        result = max_value
    return result


def _safe_float(value: Any, default: float = 0.0,
                min_value: Optional[float] = None,
                max_value: Optional[float] = None) -> float:
    try:
        result = float(value or 0.0)
    except Exception:
        return float(default)
    if not math.isfinite(result):
        return float(default)
    if min_value is not None and result < min_value:
        result = min_value
    if max_value is not None and result > max_value:
        result = max_value
    return result


# ═══════════════════════════════════════════════
#  Easing
# ═══════════════════════════════════════════════

def _ease_out_cubic(t: float) -> float:
    return _CY_UI.ease_out_cubic(t)


def _lerp(a: float, b: float, t: float) -> float:
    return _CY_UI.lerp_clamped(a, b, t)


# ═══════════════════════════════════════════════
#  Row animation state
# ═══════════════════════════════════════════════

class _RowState:
    __slots__ = (
        'uid', 'name', 'profession', 'fight_point', 'is_self',
        'damage_total', 'dps', 'damage_pct', 'bar_pct',
        'heal_total', 'hps',
        'disp_damage', 'disp_dps', 'disp_heal', 'disp_hps', 'disp_bar_pct',
        'target_damage', 'target_dps', 'target_heal', 'target_hps',
        'target_bar_pct',
        'disp_y', 'target_y',
        'fx_tier', 'fx_start',
        'mem_damage_total', 'mem_dps',
    )

    def __init__(self, uid: int):
        self.uid = uid
        self.name = f'Player_{uid}'
        self.profession = ''
        self.fight_point = 0
        self.is_self = False
        self.damage_total = 0
        self.dps = 0
        self.damage_pct = 0.0
        self.bar_pct = 0.0
        self.heal_total = 0
        self.hps = 0
        self.disp_damage = 0.0
        self.disp_dps = 0.0
        self.disp_heal = 0.0
        self.disp_hps = 0.0
        self.disp_bar_pct = 0.0
        self.target_damage = 0.0
        self.target_dps = 0.0
        self.target_heal = 0.0
        self.target_hps = 0.0
        self.target_bar_pct = 0.0
        self.disp_y = 0.0
        self.target_y = 0.0
        self.fx_tier = ''
        self.fx_start = 0.0
        self.mem_damage_total = 0
        self.mem_dps = 0

    @_probe.decorate('ui.dps.update_targets')
    def update_targets(self, data: dict) -> None:
        self.name = str(data.get('name') or self.name)
        self.profession = str(data.get('profession') or '')
        self.fight_point = _safe_int(data.get('fight_point'))
        self.is_self = bool(data.get('is_self'))
        self.damage_total = _safe_int(data.get('damage_total'))
        self.mem_damage_total = _safe_int(data.get('mem_damage_total'))
        self.mem_dps = _safe_int(data.get('mem_dps'))
        self.heal_total = _safe_int(data.get('heal_total'))
        self.damage_pct = _safe_float(data.get('damage_pct'))
        self.target_damage = _safe_float(data.get('damage_total'))
        self.target_dps = _safe_float(data.get('dps'))
        self.target_heal = _safe_float(data.get('heal_total'))
        self.target_hps = _safe_float(data.get('hps'))
        self.target_bar_pct = _safe_float(data.get('bar_pct'), min_value=0.0, max_value=1.0)


# ═══════════════════════════════════════════════
#  HitFX tiers
# ═══════════════════════════════════════════════

# tier → (duration_s, panel tint RGB, row stripe RGB)
_HIT_FX_TIERS: Dict[str, tuple] = {
    'impact':    (2.5, (104, 228, 255), (104, 228, 255)),
    'mega':      (5.0, (255, 215, 120), (222, 166,  32)),
    'starburst': (8.0, (255, 170, 204), (255, 170, 204)),
}


def _tier_of(raw) -> str:
    raw = str(raw or '').lower()
    if raw in _HIT_FX_TIERS:
        return raw
    return 'impact'


def _empty_snapshot() -> dict:
    return {
        'encounter_active': False,
        'elapsed_s': 0.0,
        'total_damage': 0,
        'total_heal': 0,
        'total_dps': 0,
        'total_hps': 0,
        'entities': [],
    }


# ═══════════════════════════════════════════════
#  Main overlay
# ═══════════════════════════════════════════════

class DpsOverlay:
    """Animated SAO-styled DPS overlay (ULW + PIL).

    Pixel-for-pixel port of `web/dps.html`. The outer window is WIDTH×HEIGHT
    and contains a 10px body padding (equivalent to CSS `body{padding:10}`)
    around the `.dps-shell`. Inside the shell:

        ┌─ shell (cream gradient, gold/cyan corners, inner highlight border)
        │   ├─ dps-header   (eyebrow · title+badge · summary · ACT buttons)
        │   ├─ dps-tabs     (Damage | Healing)
        │   ├─ list-frame   (entity rows)
        │   └─ dps-footer   (ELAPSED … | TOTAL …)
    """

    # Outer panel (ULW window) size
    WIDTH = 380
    DEFAULT_HEIGHT = 420
    MAX_HEIGHT = 700
    DETAIL_DEFAULT_W = 760
    DETAIL_DEFAULT_H = 560
    DETAIL_MIN_W = 520
    DETAIL_MIN_H = 420
    DETAIL_MAX_W = 1180
    DETAIL_MAX_H = 900
    RESIZE_GRIP = 22

    # Body padding (web CSS: body{padding:10})
    BODY_PAD = 10

    # Shell inner layout
    CORNER_SIZE = 18
    HEADER_PAD_X = 14
    HEADER_PAD_TOP = 12
    HEADER_PAD_BOT = 10
    EYEBROW_H = 13
    TITLE_H = 26
    SUMMARY_H = 14
    BTN_H = 30
    BTN_GAP = 8
    TAB_H = 26
    TAB_PAD_TOP = 10
    TAB_PAD_BOT = 8
    CONTENT_PAD_X = 14
    CONTENT_PAD_BOT = 10
    ROW_H = 44            # entity-row outer height (margin 6 + padding 8/10)
    ROW_MARGIN = 6
    FOOTER_H = 40

    MAX_ROWS = 6
    PAD = BODY_PAD        # back-compat alias
    PANEL_OPACITY = 0.93
    CLICK_DRAG_THRESHOLD = 6

    # Colors (parity with web/dps.html CSS vars, alpha 0-255)
    # v2.2.0: SAO Alert flat hi-tech — 纯白+略灰, 透明度保持
    PANEL_BG_A = (250, 252, 253, 255)
    PANEL_BG_B = (220, 224, 229, 255)
    PANEL_EDGE = (128, 190, 220, 255)
    PANEL_LINE = (255, 255, 255, 255)
    INNER_HIGHLIGHT = (255, 255, 255, 255)
    HAIRLINE_LIGHT = (250, 250, 250, 255)
    HAIRLINE_MID = (228, 228, 228, 255)
    HAIRLINE_DARK = (140, 138, 138, 255)
    SCAN_LINE = (104, 228, 255, 24)
    TEXT_MAIN = (100, 99, 100, 255)
    TEXT_MUTED = (140, 135, 138, 255)
    GOLD = (222, 166, 32, 255)
    GOLD_SOFT = (222, 166, 32, 56)
    CYAN = (104, 228, 255, 255)
    DIVIDER = (178, 180, 182, 255)
    LIST_BG = (244, 248, 252, 196)
    LIST_BORDER = (120, 190, 225, 230)
    ROW_BG = (248, 247, 244, 214)
    ROW_BORDER = (156, 178, 194, 235)
    ROW_SELF_BAR = (222, 166, 32, 255)
    BTN_BG = (255, 255, 255, 112)
    BTN_BORDER = (178, 180, 182, 255)
    BTN_LIVE_ACTIVE = (104, 228, 255, 31)
    BTN_LIVE_BORDER = (104, 228, 255, 255)
    BTN_LIVE_COLOR = (68, 144, 162, 255)
    BTN_DANGER = (239, 104, 78, 255)
    BAR_OTHER_A = (222, 166, 32, 51)
    BAR_OTHER_B = (222, 166, 32, 8)
    BAR_HEAL_A = (154, 211, 52, 61)
    BAR_HEAL_B = (154, 211, 52, 8)
    BADGE_LIVE = (82, 140, 48, 255)
    BADGE_REPORT = (222, 166, 32, 255)
    # v2.3.x: previously hardcoded dark colors — now themeable
    FOOTER_BG = (238, 242, 247, 205)
    FOOTER_CYAN_TINT = (104, 228, 255, 18)
    FOOTER_SHADOW = (40, 55, 70, 0)
    TAB_ACTIVE_FILL = (222, 190, 80, 35)
    TAB_ACTIVE_BORDER = (222, 190, 80, 220)
    TAB_INACTIVE_FILL = (40, 55, 75, 120)
    TAB_INACTIVE_BORDER = (50, 100, 130, 100)
    BTN_ACTIVE_FILL = (222, 190, 80, 40)
    BTN_ACTIVE_BORDER = (222, 190, 80, 220)
    BTN_DISABLED_FILL = (30, 35, 45, 120)
    BTN_DISABLED_BORDER = (50, 60, 70, 100)
    BTN_DISABLED_FG = (80, 85, 95, 180)
    HEADER_BADGE_FILL = (230, 240, 248, 220)
    HEADER_BADGE_BORDER = (60, 140, 180, 160)
    LIST_CYAN_TINT = (104, 228, 255, 22)
    LIST_SHADOW = (45, 55, 70, 0)
    ROW_SHEEN_CYAN = (104, 228, 255, 20)
    ROW_LOWER_SHADOW = (60, 45, 38, 0)
    ROW_SELF_OUTLINE = (222, 190, 80, 220)
    ROW_SELF_TINT = (222, 190, 80, 28)
    DETAIL_CARD_BG = (35, 45, 60, 160)
    SKILL_ROW_BG = (30, 40, 55, 140)
    STAT_HEAL_GREEN = (92, 150, 44, 255)
    SKILL_BAR_HEAL = (154, 211, 52, 70)
    SKILL_BAR_DAMAGE = (222, 190, 80, 60)
    VAL_HEAL_GREEN = (92, 150, 44, 255)
    # Skill rank-board cockpit (parity with web/dps.html): gold/silver/bronze
    # rank badges + a heat-gradient track (cold→gold→hot by relative output).
    # Theme-independent — kept as class defaults so they survive _apply_theme
    # (which only setattr's keys present in the theme dict) and stay 1:1 across
    # light/dark with the webview which hard-codes the same colors.
    RANK_GOLD = (235, 185, 60, 255)
    RANK_SILVER = (200, 209, 221, 255)
    RANK_BRONZE = (209, 144, 80, 255)
    RANK_GOLD_FG = (42, 29, 5, 255)
    RANK_SILVER_FG = (28, 32, 38, 255)
    RANK_BRONZE_FG = (36, 18, 8, 255)
    SKILL_TRACK_BG = (128, 130, 120, 56)
    SKILL_RANK_MUTED = (150, 150, 156, 255)
    HEAT_COLD = (60, 176, 255)
    HEAT_MID = (255, 200, 72)
    HEAT_HOT = (255, 96, 78)
    # Detail skill-list smooth scroll: px advanced per wheel notch, and the
    # per-tick easing factor (disp += (target-disp)*ease at 60 Hz).
    SKILL_WHEEL_STEP = 96
    SKILL_SCROLL_EASE = 0.35
    # Shell layer
    SHELL_AMBIENT_SHADOW = (22, 24, 18, 0)
    SHELL_CONTACT_SHADOW = (31, 34, 16, 0)
    SHELL_SHEEN_CYAN = (104, 228, 255, 32)
    SHELL_SHEEN_SHADOW = (42, 52, 64, 34)
    CORNER_CYAN_ACCENT = (104, 228, 255, 120)
    CORNER_GOLD_ACCENT = (222, 190, 80, 120)
    CORNER_GOLD = (222, 190, 80, 255)
    RANK_COLORS = {
        0: (222, 190, 80),             # gold for #1
        1: (160, 170, 185),            # silver for #2
        2: (180, 140, 90),             # bronze for #3
    }

    # Animation timings (seconds)
    BAR_TWEEN = 0.35
    NUM_TWEEN = 0.45
    ROW_TWEEN = 0.30
    FADE_IN = 0.28
    FADE_OUT = 0.26

    TICK_MS = 16          # damping coefficient base; not scheduling rate (overlay_scheduler owns Hz)
    IDLE_TICK_MS = 60     # idle refresh still drives clock / fade-out

    def __init__(self, root: tk.Tk, settings: Any = None,
                 request_live_snapshot=None,
                 show_last_report=None,
                 reset_dps=None,
                 has_last_report=None,
                 request_entity_detail=None,
                 list_history=None,
                 export_last_report=None,
                 alert=None):
        self.root = root
        self.settings = settings
        self._request_live_snapshot = request_live_snapshot
        self._show_last_report_cb = show_last_report
        self._reset_dps_cb = reset_dps
        self._has_last_report_cb = has_last_report
        self._request_entity_detail_cb = request_entity_detail
        self._list_history_cb = list_history
        self._export_last_report_cb = export_last_report
        self._alert_cb = alert
        self._win: Optional[tk.Toplevel] = None
        self._hwnd: int = 0
        self._visible = False
        # v2.3.x GPU presenter fields.
        self._gpu_window: Optional[Any] = None
        self._gpu_presenter: Optional[Any] = None
        self._gpu_managed: bool = False
        # v2.3.x dirty-skip: store last submitted compose signature so
        # the unconditional 60 Hz submit collapses to value-changed
        # frames only. The displayed clock signature ticks 1 Hz.
        self._last_compose_sig: Optional[tuple] = None
        self._last_snapshot: Optional[dict] = None
        self._last_report: Optional[dict] = None
        self._act_snapshot: Optional[dict] = None
        self._self_uid = 0
        self._view_mode = 'live'
        self._current_tab = 'damage'
        self._report_available = False
        self._minimized = False
        self._panel_notice: str = ''
        self._panel_notice_until: float = 0.0
        # Detail view state (parity with web/dps.html _openDetail/_closeDetail)
        self._detail_visible = False
        self._detail_uid = 0
        self._detail_mode = False
        self._live_detail_cache: Dict[int, dict] = {}
        # Hit-test regions captured during the most recent compose pass.
        self._row_click_regions: List[Tuple[int, Tuple[int, int, int, int]]] = []
        self._detail_back_rect: Optional[Tuple[int, int, int, int]] = None
        self._resize_rect: Optional[Tuple[int, int, int, int]] = None
        # List area rect (x, y, w, h) in window-local coords — for drag-scroll
        self._list_rect: Optional[Tuple[int, int, int, int]] = None
        # Drag-scroll state
        self._list_drag_active: bool = False
        self._list_drag_start_y: int = 0
        self._list_drag_start_offset: int = 0
        self._list_drag_pending_acc: float = 0.0

        sw = _user32.GetSystemMetrics(0)
        sh = _user32.GetSystemMetrics(1)
        # Mirror the webview DPS window geometry (sao_webview.py): dimensions
        # are derived from screen size so the Tk ULW overlay lines up pixel-
        # for-pixel with the webview panel.
        dyn_w = max(320, int(min(sw, 1920) * 0.19))
        dyn_h = max(self.DEFAULT_HEIGHT, int(min(sh, 1080) * 0.48))
        self.WIDTH = dyn_w
        self.DEFAULT_HEIGHT = max(self.DEFAULT_HEIGHT, dyn_h)
        self._detail_w = max(self.DETAIL_DEFAULT_W, int(min(sw, 1920) * 0.40))
        self._detail_h = max(self.DETAIL_DEFAULT_H, int(min(sh, 1080) * 0.56))
        self._detail_w = max(self.DETAIL_MIN_W, min(self.DETAIL_MAX_W, self._detail_w))
        self._detail_h = max(self.DETAIL_MIN_H, min(self.DETAIL_MAX_H, self._detail_h))
        self._x = max(0, sw - self.WIDTH - max(16, int(sw * 0.012)))
        self._y = max(0, int(sh * 0.18))
        if settings is not None:
            try:
                self._x = int(settings.get('dps_ov_x', self._x))
                self._y = int(settings.get('dps_ov_y', self._y))
                self._detail_w = int(settings.get('dps_detail_w', self._detail_w))
                self._detail_h = int(settings.get('dps_detail_h', self._detail_h))
                self._minimized = bool(settings.get('dps_minimized', False))
                self._detail_w = max(self.DETAIL_MIN_W, min(self.DETAIL_MAX_W, self._detail_w))
                self._detail_h = max(self.DETAIL_MIN_H, min(self.DETAIL_MAX_H, self._detail_h))
            except Exception:
                pass

        # Animation state
        self._rows: Dict[int, _RowState] = {}
        self._row_order: List[int] = []
        self._disp_total_damage = 0.0
        self._disp_total_dps = 0.0
        self._disp_total_heal = 0.0
        self._disp_total_hps = 0.0
        self._disp_elapsed = 0.0
        self._target_total_damage = 0.0
        self._target_total_dps = 0.0
        self._target_total_heal = 0.0
        self._target_total_hps = 0.0
        self._target_elapsed = 0.0
        self._encounter_active = False

        self._panel_fx_tier = ''
        self._panel_fx_start = 0.0
        self._last_fx_seq = 0

        self._fade_alpha = 0.0
        self._fade_target = 0.0
        self._fade_from = 0.0
        self._fade_start = 0.0
        self._fade_duration = self.FADE_IN
        self._faded_out = False
        self._hide_after_fade = False
        # When True the next tick that observes ``fade_alpha <= 0.01`` will
        # flip the panel to mouse pass-through so faded-out idle DPS does
        # not swallow clicks meant for the game window underneath. Cleared
        # by fade_in() and by show().
        self._idle_passthrough_pending = False
        self._is_passthrough = False

        self._drag_ox = 0
        self._drag_oy = 0
        self._drag_start_root = (0, 0)
        self._drag_moved = False
        self._gpu_drag_active = False
        self._resize_active = False
        self._resize_start_root = (0, 0)
        self._resize_start_size = (0, 0)

        self._tick_after_id: Optional[str] = None
        self._last_rendered_size: tuple = (0, 0)
        self._registered: bool = False

        # Scroll offset for entity list (mouse wheel support)
        self._scroll_offset: int = 0
        self._scroll_offset_report: int = 0
        # Smooth pixel scroll for the detail-view skill list (independent of
        # the entity list; driven by the wheel while the detail view is open).
        # _disp eases toward _target every tick so it glides like the webview
        # .skill-frame instead of snapping one row at a time.
        self._skill_scroll_disp: float = 0.0     # displayed px offset (eased)
        self._skill_scroll_target: float = 0.0   # wheel target px offset
        self._skill_content_h: int = 0           # total skill content height
        self._skill_view_h: int = 0              # skill viewport height (px)
        self._skill_max_scroll: float = 0.0      # max(0, content_h - view_h)

        # Static shell layer cache (shadow + shell + corners).
        # Only depends on (w, h); reused as long as panel size is stable.
        self._shell_cache: Optional[Image.Image] = None
        self._shell_cache_size: tuple = (0, 0)

        # Async render worker — compose + premult off main thread.
        # v2.2.12: prefer_isolation so DPS gets a dedicated heavy lane,
        # matching BossHP / Burst / MenuHud / HP. Without it, DPS shared
        # a lane with idle panels and got serialized behind their compose
        # work during heavy fights.
        self._render_worker = AsyncFrameWorker(prefer_isolation=True)

        # Theme: load saved preference, apply before first compose
        self._theme_name: str = 'dark'
        if settings is not None:
            try:
                saved = settings.get('panel_themes', {}).get('dps', 'dark')
                if saved in ('light', 'dark'):
                    self._apply_theme(saved)
            except Exception:
                pass

    # ──────────────────────────────────────────
    #  Lifecycle
    # ──────────────────────────────────────────

    def show(self) -> None:
        if self._win is not None:
            return
        require_entity_gpu('DpsOverlay', _gow)
        try:
            pump = _gow.get_glfw_pump(self.root)
            presenter = _gow.BgraPresenter()
            # Initial 1×1 placeholder; tick will resize to compose dims.
            gpu_win = _gow.GpuOverlayWindow(
                pump,
                w=1, h=1,
                x=int(self._x), y=int(self._y),
                render_fn=presenter.render,
                click_through=False,
                title='sao_dps_gpu',
            )
            gpu_win.set_input_callbacks(
                cursor_pos_fn=self._on_gpu_cursor_pos,
                mouse_button_fn=self._on_gpu_mouse_button,
                scroll_fn=self._on_gpu_scroll,
            )
            gpu_win.show()
            self._gpu_window = gpu_win
            self._gpu_presenter = presenter
            self._gpu_managed = True
            self._win = self  # type: ignore[assignment]  # sentinel
            self._hwnd = 0
            self._visible = True
            self._faded_out = False
            self._hide_after_fade = False
            self._idle_passthrough_pending = False
            self._is_passthrough = False
            self._fade_from = 0.0
            self._fade_alpha = 0.0
            self._fade_target = 1.0
            self._fade_start = time.time()
            self._fade_duration = self.FADE_IN
            self._schedule_tick(immediate=True)
            return
        except Exception:
            self._gpu_window = None
            self._gpu_presenter = None
            self._gpu_managed = False
            raise

    def hide(self) -> None:
        self._hide_after_fade = False
        self._idle_passthrough_pending = False
        self._is_passthrough = False
        self._cancel_tick()
        # v2.3.x: tear down GPU presenter if active.
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
        self._last_compose_sig = None

    def _window_ready(self) -> bool:
        return bool(self._gpu_managed or self._hwnd)

    # Idle fade target: fully fade the panel out, but keep the ULW/GPU window
    # alive in the background so the next damage packet can fade_in() instantly
    # without rebuilding the presenter.
    FADE_IDLE_ALPHA = 0.0

    def _set_passthrough(self, passthrough: bool) -> None:
        """Toggle WS_EX_TRANSPARENT so a faded-out idle panel stops
        swallowing clicks meant for the game window. Re-enabled
        (interactive) on fade_in().

        v3.0.2: always re-assert the Win32 / GLFW state instead of
        early-returning on a cached ``_is_passthrough`` match. The
        cached flag could drift out of sync with the actual window
        ex-style if GLFW reset it on focus/activation, leaving the
        idle DPS panel quietly intercepting clicks even though we
        believed it was already pass-through.
        """
        passthrough = bool(passthrough)
        if self._gpu_managed and self._gpu_window is not None:
            # v3.0.4: set_click_through used to live on the wrong class
            # (BgraPresenter) so this call raised AttributeError, which we
            # silently swallowed — leaving the faded-out panel grabbing
            # clicks forever. Guard explicitly and only treat the toggle as
            # applied when it actually ran, so a regression surfaces in the
            # log instead of hiding as a dead click-zone.
            fn = getattr(self._gpu_window, 'set_click_through', None)
            if callable(fn):
                try:
                    fn(passthrough)
                    self._is_passthrough = passthrough
                    return
                except Exception as exc:
                    print(f'[DPS-OV] gpu set_click_through failed: {exc}')
            else:
                print('[DPS-OV] gpu window has no set_click_through; '
                      'falling back to hwnd ex-style')
        if not self._hwnd:
            return
        try:
            ex = _user32.GetWindowLongW(
                ctypes.c_void_p(self._hwnd), GWL_EXSTYLE)
            if passthrough:
                new_ex = ex | WS_EX_TRANSPARENT
            else:
                new_ex = ex & ~WS_EX_TRANSPARENT
            if new_ex != ex:
                _user32.SetWindowLongW(
                    ctypes.c_void_p(self._hwnd), GWL_EXSTYLE, new_ex)
                # Flush: WS_EX_TRANSPARENT hit-testing can stay cached on
                # the layered window until a frame-change pulse. Without
                # this, the Tk/ULW DPS panel had the same dead-zone bug.
                try:
                    _user32.SetWindowPos(
                        ctypes.c_void_p(self._hwnd),
                        ctypes.c_void_p(HWND_TOPMOST), 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE
                        | SWP_FRAMECHANGED)
                except Exception:
                    pass
            self._is_passthrough = passthrough
        except Exception:
            pass

    def fade_out(self) -> None:
        if self._faded_out:
            return
        self._faded_out = True
        # Soft idle fade — do NOT tear down the window. fade_in() will bring
        # it back. Real teardown goes through hide().
        self._hide_after_fade = False
        self._fade_from = self._fade_alpha
        self._fade_target = self.FADE_IDLE_ALPHA
        self._fade_start = time.time()
        self._fade_duration = self.FADE_OUT
        # Flip the panel to mouse pass-through immediately so the
        # invisible-but-still-alive ULW does not steal clicks meant for
        # the game window underneath. fade_in() reverses this.
        self._idle_passthrough_pending = False
        self._set_passthrough(True)
        self._schedule_tick(immediate=True)

    def fade_in(self) -> None:
        # Restore interactivity the moment the panel starts coming back,
        # so users can click it as soon as it begins reappearing instead
        # of waiting for the alpha to finish climbing.
        self._idle_passthrough_pending = False
        self._set_passthrough(False)
        if not self._faded_out and self._fade_target >= 1.0:
            return
        self._faded_out = False
        self._hide_after_fade = False
        self._fade_from = self._fade_alpha
        self._fade_target = 1.0
        self._fade_start = time.time()
        self._fade_duration = self.FADE_IN
        self._schedule_tick(immediate=True)

    def set_self_uid(self, uid: int) -> None:
        try:
            self._self_uid = int(uid or 0)
        except Exception:
            self._self_uid = 0
        for uid_, row in self._rows.items():
            row.is_self = (uid_ == self._self_uid)

    def set_report_available(self, available: bool) -> None:
        self._report_available = bool(available) or bool(self._last_report)
        self._schedule_tick(immediate=True)

    def set_act_snapshot(self, snapshot: Optional[dict]) -> None:
        self._act_snapshot = dict(snapshot or {}) if snapshot else None
        self._schedule_tick(immediate=True)

    def is_report_mode(self) -> bool:
        return self._view_mode == 'report'

    def show_live(self, snapshot: Optional[dict] = None) -> None:
        self._sync_report_available()
        self._view_mode = 'live'
        self._detail_visible = False
        self._detail_uid = 0
        self._detail_mode = False
        if snapshot is None and callable(self._request_live_snapshot):
            try:
                snapshot = self._request_live_snapshot()
            except Exception:
                snapshot = None
        self._ingest_live_snapshot(snapshot or self._last_snapshot or _empty_snapshot(),
                                   force_show=True)
        self.fade_in()
        self._schedule_tick(immediate=True)

    def show_last_report(self, report: Optional[dict]) -> bool:
        if not report:
            return False
        self._last_report = dict(report)
        self._report_available = True
        self._view_mode = 'report'
        self._detail_visible = False
        self._detail_uid = 0
        self._detail_mode = False
        if not self._visible or not self._window_ready():
            self.show()
        self.fade_in()
        self._schedule_tick(immediate=True)
        return True

    # ── Detail view (parity with web/dps.html _openDetail/_closeDetail) ──

    def open_detail(self, uid: int) -> None:
        try:
            uid = int(uid or 0)
        except Exception:
            uid = 0
        if uid <= 0:
            return
        self._detail_uid = uid
        self._detail_visible = True
        self._skill_scroll_disp = 0.0
        self._skill_scroll_target = 0.0
        # In live mode, request a fresh skill breakdown from the controller.
        if self._view_mode == 'live' and callable(self._request_entity_detail_cb):
            try:
                detail = self._request_entity_detail_cb(uid)
                if isinstance(detail, dict):
                    self._live_detail_cache[uid] = detail
            except Exception:
                pass
        self._schedule_tick(immediate=True)

    def _pick_detail_uid(self) -> int:
        """Pick the most useful entity for the panel-wide detail mode."""
        try:
            if self._detail_uid:
                return int(self._detail_uid)
        except Exception:
            pass
        if self._view_mode == 'report':
            entities = self._report_entities()
            for ent in entities:
                try:
                    uid = int(ent.get('uid') or 0)
                    if bool(ent.get('is_self')) or (self._self_uid and uid == self._self_uid):
                        return uid
                except Exception:
                    pass
            for ent in entities:
                try:
                    uid = int(ent.get('uid') or 0)
                    if uid > 0:
                        return uid
                except Exception:
                    pass
            return 0
        if self._self_uid and self._self_uid in self._rows:
            return int(self._self_uid)
        rows = self._build_view_rows()
        if rows:
            try:
                return int(rows[0].get('uid') or 0)
            except Exception:
                return 0
        return 0

    def enter_detail_mode(self, uid: int = 0) -> None:
        try:
            uid = int(uid or 0)
        except Exception:
            uid = 0
        if uid <= 0:
            uid = self._pick_detail_uid()
        self._detail_mode = True
        self._detail_uid = max(0, int(uid or 0))
        self._detail_visible = True
        self._skill_scroll_disp = 0.0
        self._skill_scroll_target = 0.0
        if self._detail_uid > 0 and self._view_mode == 'live' \
                and callable(self._request_entity_detail_cb):
            try:
                detail = self._request_entity_detail_cb(self._detail_uid)
                if isinstance(detail, dict):
                    self._live_detail_cache[self._detail_uid] = detail
            except Exception:
                pass
        self._last_compose_sig = None
        self._schedule_tick(immediate=True)

    def exit_detail_mode(self) -> None:
        self._detail_mode = False
        self._resize_active = False
        self._detail_visible = False
        self._detail_uid = 0
        self._skill_scroll_disp = 0.0
        self._skill_scroll_target = 0.0
        self._last_compose_sig = None
        self._schedule_tick(immediate=True)

    def close_detail(self) -> None:
        if self._detail_mode:
            self.exit_detail_mode()
            return
        if not self._detail_visible:
            return
        self._detail_visible = False
        self._detail_uid = 0
        self._skill_scroll_disp = 0.0
        self._skill_scroll_target = 0.0
        self._schedule_tick(immediate=True)

    @_probe.decorate('ui.dps.update_detail')
    def update_detail(self, data: Optional[dict]) -> None:
        """Push fresh per-entity skill breakdown (called by controller)."""
        if not isinstance(data, dict):
            return
        try:
            uid = int(data.get('uid') or 0)
        except Exception:
            uid = 0
        if uid <= 0:
            return
        self._live_detail_cache[uid] = dict(data)
        if (self._view_mode == 'live' and self._detail_visible
                and self._detail_uid == uid):
            self._schedule_tick(immediate=True)

    def _sync_report_available(self) -> None:
        available = bool(self._last_report)
        if callable(self._has_last_report_cb):
            try:
                available = bool(self._has_last_report_cb()) or available
            except Exception:
                pass
        self._report_available = available

    def _report_entities(self) -> List[dict]:
        if not isinstance(self._last_report, dict):
            return []
        entities = self._last_report.get('entities')
        if entities is None:
            entities = self._last_report.get('party') or []
        return list(entities or [])

    def _has_report_data(self) -> bool:
        return bool(self._report_entities())

    def _ingest_live_snapshot(self, snapshot: dict,
                              force_show: bool = False) -> None:
        snapshot = dict(snapshot or _empty_snapshot())
        self._sync_report_available()
        entities = snapshot.get('entities')
        if entities is None:
            entities = snapshot.get('party') or []
        entities = list(entities or [])
        has_content = bool(
            entities
            or _safe_int(snapshot.get('total_damage')) > 0
            or _safe_int(snapshot.get('total_heal')) > 0
        )
        if (not self._visible or not self._window_ready()) and not has_content and not force_show:
            self._last_snapshot = snapshot
            self._target_total_damage = _safe_float(snapshot.get('total_damage'))
            self._target_total_dps = _safe_float(snapshot.get('total_dps'))
            self._target_total_heal = _safe_float(snapshot.get('total_heal'))
            self._target_total_hps = _safe_float(snapshot.get('total_hps'))
            self._target_elapsed = _safe_float(snapshot.get('elapsed_s'))
            self._encounter_active = bool(snapshot.get('encounter_active'))
            return
        if (not self._visible or not self._window_ready()) and force_show:
            self.show()
        elif not self._visible or not self._window_ready():
            self.show()
        self._last_snapshot = snapshot

        self._target_total_damage = _safe_float(snapshot.get('total_damage'))
        self._target_total_dps = _safe_float(snapshot.get('total_dps'))
        self._target_total_heal = _safe_float(snapshot.get('total_heal'))
        self._target_total_hps = _safe_float(snapshot.get('total_hps'))
        self._target_elapsed = _safe_float(snapshot.get('elapsed_s'))
        self._encounter_active = bool(snapshot.get('encounter_active'))

        seen_uids: List[int] = []
        # Keep row state for the full ranked entity list. Rendering applies
        # the scroll window later; dropping offscreen rows here made the wheel
        # unable to ever reach rank 7+ because the model only retained 6 rows.
        self._scroll_offset = max(0, min(self._scroll_offset, max(0, len(entities) - self.MAX_ROWS)))
        for idx, ent in enumerate(entities):
            uid = _safe_int(ent.get('uid'))
            if not uid:
                continue
            row = self._rows.get(uid)
            if row is None:
                row = _RowState(uid)
                row.disp_y = float(idx + 0.6)
                row.target_y = float(idx)
                self._rows[uid] = row
            else:
                row.target_y = float(idx)
            row.update_targets(ent)
            if self._self_uid and uid == self._self_uid:
                row.is_self = True
            seen_uids.append(uid)

        for stale_uid in [u for u in self._rows if u not in seen_uids]:
            self._rows.pop(stale_uid, None)

        # Clamp scroll offset to the current entity count
        max_off = max(0, len(entities) - self.MAX_ROWS)
        if self._scroll_offset > max_off:
            self._scroll_offset = max_off

        self._row_order = seen_uids

        fx = snapshot.get('hit_fx')
        if isinstance(fx, dict):
            try:
                seq = int(fx.get('seq') or 0)
            except Exception:
                seq = 0
            if seq and seq > self._last_fx_seq:
                self._last_fx_seq = seq
                tier = _tier_of(fx.get('tier'))
                self._panel_fx_tier = tier
                self._panel_fx_start = time.time()
                try:
                    fx_uid = int(fx.get('uid') or 0)
                except Exception:
                    fx_uid = 0
                row = self._rows.get(fx_uid)
                if row:
                    row.fx_tier = tier
                    row.fx_start = self._panel_fx_start

        self._schedule_tick(immediate=True)

    # ── Theme ──

    def _apply_theme(self, theme_name: str) -> None:
        """切换 DPS 面板主题并清除所有渲染缓存。

        Args:
            theme_name: 'light' 或 'dark'
        """
        from sao_theme import get_panel_theme
        theme = get_panel_theme('dps', theme_name)
        if not theme:
            return
        for key, value in theme.items():
            setattr(self, key, value)
        self._theme_name = theme_name
        # 清除所有缓存 → 下次 compose 时用新主题重建
        self._shell_cache = None
        self._shell_cache_size = (0, 0)
        if hasattr(self, '_bar_cache'):
            self._bar_cache = {}
        # 强制下一帧重绘
        self._last_compose_sig = None
        self._schedule_tick(immediate=True)

    # ──────────────────────────────────────────
    #  Snapshot ingestion
    # ──────────────────────────────────────────

    @_probe.decorate('ui.dps.update')
    def update(self, snapshot: dict) -> None:
        if snapshot is None:
            return
        self._ingest_live_snapshot(snapshot)

    # ──────────────────────────────────────────
    #  Animation tick
    # ──────────────────────────────────────────

    def _schedule_tick(self, immediate: bool = False) -> None:
        """Register with the shared 60 Hz scheduler (idempotent).

        `immediate=True` used to force an out-of-band early tick; with the
        shared scheduler we just make sure we're subscribed — the next tick
        will happen on the global 60 Hz deadline anyway.
        """
        if not self._visible or self._win is None:
            return
        if not self._registered:
            try:
                sched = _get_scheduler(self.root)
                sched.register('dps', self._tick, self._is_animating)
                self._registered = True
            except Exception as exc:
                print(f'[DPS-OV] scheduler register error: {exc}')

    def _cancel_tick(self) -> None:
        if self._registered:
            try:
                _get_scheduler(self.root).unregister('dps')
            except Exception:
                pass
            self._registered = False

    def _is_animating(self) -> bool:
        # Keep ticking at 60 Hz while the detail skill list is gliding so the
        # eased scroll resubmits every frame (see _compose_signature -> None).
        if self._detail_visible and abs(
                float(self._skill_scroll_disp)
                - float(self._skill_scroll_target)) > 0.75:
            return True
        return bool(_CY_UI.dps_overlay_animating(
            self._hide_after_fade,
            self._fade_alpha, self._fade_target,
            self._panel_fx_tier, self._rows,
            self._disp_total_damage, self._target_total_damage,
            self._disp_total_dps, self._target_total_dps,
            self._disp_total_heal, self._target_total_heal,
            self._disp_total_hps, self._target_total_hps,
        ))

    def _compose_signature(self, now: float) -> Optional[tuple]:
        """v2.3.x: coarse fingerprint of frame inputs. Returns None
        when an animation is in flight (forces every-tick submit so
        tweens look smooth). Otherwise returns a tuple covering all
        observable state — if it equals the previous signature, the
        compose+ULW pass can be skipped entirely."""
        if self._is_animating():
            return None
        try:
            row_sig = _CY_UI.dps_rows_signature(self._rows)
            detail_sig = None
            if self._detail_visible:
                ent = self._get_detail_entity()
                if isinstance(ent, dict):
                    detail_sig = _CY_UI.dps_detail_signature(ent)
            return (
                int(self._disp_elapsed),  # 1 Hz tick
                int(self._target_total_damage),
                int(self._target_total_dps),
                int(self._target_total_heal),
                int(self._target_total_hps),
                self._view_mode,
                self._current_tab,
                bool(self._detail_visible),
                bool(self._detail_mode),
                int(self._detail_uid),
                bool(self._minimized),
                self._panel_notice_text(),
                int(self._detail_w),
                int(self._detail_h),
                self._act_badge_text(),
                detail_sig,
                round(float(self._fade_alpha), 3),
                row_sig,
            )
        except Exception:
            return None

    @_probe.decorate('ui.dps.tick')
    def _tick(self, now: Optional[float] = None) -> None:
        if not self._visible or self._win is None:
            return
        if now is None:
            now = time.time()
        self._advance_animations(now)

        # ── Async render pipeline ──
        if self._gpu_managed:
            # v2.2.23: don't let vision capture starve our commits.
            fb = self._render_worker.take_result(allow_during_capture=True)
            if fb is not None:
                sz = (fb.width, fb.height)
                if self._gpu_presenter is not None and self._gpu_window is not None:
                    try:
                        if sz != self._last_rendered_size \
                                or (fb.x, fb.y) != (self._x, self._y):
                            self._gpu_window.set_geometry(
                                self._x, self._y, fb.width, fb.height)
                            self._last_rendered_size = sz
                        self._gpu_presenter.set_frame(
                            fb.bgra_bytes, fb.width, fb.height)
                        self._gpu_window.request_redraw()
                        _perf_gauge('ui.dps.presented', 1)
                    except Exception as e:
                        print(f'[DPS-OV] gpu present error: {e}')
            else:
                _perf_gauge('ui.dps.presented', 0)

            # v2.3.x dirty-skip: previously this submitted compose_frame
            # every tick (60 Hz when animating, 10-20 Hz idle). Most
            # ticks produce visually identical output — the elapsed
            # clock only changes once a second and disp_total_* tweens
            # quantize at sub-pixel level. Build a coarse signature
            # and skip the submit when nothing changed since the last
            # one. Saves ~60-80% of compose_frame CPU during steady
            # state.
            sig = self._compose_signature(now)
            if sig is None or sig != self._last_compose_sig:
                self._last_compose_sig = sig
                self._render_worker.submit(
                    self.compose_frame, now,
                    self._hwnd if self._hwnd else 0,
                    self._x, self._y)
                _perf_gauge('ui.dps.submitted', 1)
            else:
                _perf_gauge('ui.dps.skipped_sig', 1)

        if self._hide_after_fade and self._fade_alpha <= 0.01:
            self.hide()

    def _advance_animations(self, now: float) -> bool:
        animating = False

        if abs(self._fade_alpha - self._fade_target) > 1e-3:
            t = (now - self._fade_start) / max(1e-3, self._fade_duration)
            k = _ease_out_cubic(t)
            self._fade_alpha = _lerp(self._fade_from, self._fade_target, k)
            if t < 1.0:
                animating = True
            else:
                self._fade_alpha = self._fade_target

        animating = self._step_toward_totals() or animating

        for row in list(self._rows.values()):
            if self._step_row(row, now):
                animating = True

        if self._panel_fx_tier:
            dur = _HIT_FX_TIERS[self._panel_fx_tier][0]
            if now - self._panel_fx_start > dur:
                self._panel_fx_tier = ''
            else:
                animating = True

        # Smooth skill-list scroll: ease the displayed px offset toward the
        # wheel target so the detail skill list glides like the webview
        # .skill-frame instead of snapping one row per notch.
        if self._detail_visible:
            disp = float(self._skill_scroll_disp)
            tgt = float(self._skill_scroll_target)
            if abs(tgt - disp) > 0.75:
                self._skill_scroll_disp = disp + (tgt - disp) * self.SKILL_SCROLL_EASE
                animating = True
            elif disp != tgt:
                self._skill_scroll_disp = tgt

        return animating

    def _step_toward_totals(self) -> bool:
        # Smoothly tween totals so the header counters slide instead of
        # snapping every snapshot. Elapsed is a clock and stays linear.
        (self._disp_total_damage,
         self._disp_total_dps,
         self._disp_total_heal,
         self._disp_total_hps,
         self._disp_elapsed,
         changed) = _CY_UI.dps_step_totals(
            self._disp_total_damage, self._target_total_damage,
            self._disp_total_dps, self._target_total_dps,
            self._disp_total_heal, self._target_total_heal,
            self._disp_total_hps, self._target_total_hps,
            self._disp_elapsed, self._target_elapsed,
            self.TICK_MS, self.NUM_TWEEN,
        )
        return bool(changed)

    def _step_row(self, row: _RowState, now: float) -> bool:
        # Tween numeric values, bar fill, and slot Y so reorders/value
        # changes visibly slide instead of snapping each frame.
        fx_duration = _HIT_FX_TIERS.get(row.fx_tier, (0,))[0] if row.fx_tier else 0.0
        return bool(_CY_UI.dps_step_row_state(
            row, now, self.TICK_MS, self.NUM_TWEEN,
            self.BAR_TWEEN, self.ROW_TWEEN, fx_duration,
        ))

    # ──────────────────────────────────────────
    #  Rendering — pixel-for-pixel port of web/dps.html
    # ──────────────────────────────────────────

    def _header_height(self) -> int:
        """Total .dps-header height = pad-top + eyebrow + 4 + title + 6 +
        summary + pad-bot + button row (below title cluster, wrapped)."""
        # Top cluster (eyebrow/title/summary) + button row + gap between
        button_rows = self._button_row_count()
        return (self.HEADER_PAD_TOP
                + self.EYEBROW_H + 4
                + self.TITLE_H + 6
                + self.SUMMARY_H
                + 8
            + button_rows * self.BTN_H
            + max(0, button_rows - 1) * 5
                + self.HEADER_PAD_BOT)

    def _tabs_height(self) -> int:
        return self.TAB_PAD_TOP + self.TAB_H + self.TAB_PAD_BOT

    def _view_row_total(self) -> int:
        if self._view_mode == 'report':
            return len(self._report_entities())
        act_rows = self._act_render_rows()
        if act_rows:
            return len(act_rows)
        return len(self._rows)

    def _current_row_count(self) -> int:
        return min(self._view_row_total(), self.MAX_ROWS)

    def _clamped_scroll(self) -> int:
        """Return the scroll offset clamped to current data bounds."""
        if self._view_mode == 'report':
            total = len(self._report_entities())
            return max(0, min(self._scroll_offset_report, max(0, total - self.MAX_ROWS)))
        return max(0, min(self._scroll_offset, max(0, len(self._rows) - self.MAX_ROWS)))

    def _act_render_rows(self) -> List[dict]:
        try:
            spec = (self._act_snapshot or {}).get('render_spec') or {}
            if str(spec.get('mode') or '') != 'live':
                return []
            rows = spec.get('rows') or []
            if not isinstance(rows, list):
                return []
            return [dict(row) for row in rows if isinstance(row, dict)]
        except Exception:
            return []

    def _build_act_view_rows(self, act_rows: List[dict], is_heal: bool) -> List[dict]:
        rows: List[dict] = []
        sorted_rows = list(act_rows or [])
        amount_key = 'heal' if is_heal else 'damage'
        rate_key = 'hps' if is_heal else 'dps'
        sorted_rows.sort(
            key=lambda ent: (_safe_float(ent.get(amount_key)), _safe_int(ent.get('uid'))),
            reverse=True,
        )
        scroll = max(0, min(self._scroll_offset, max(0, len(sorted_rows) - self.MAX_ROWS)))
        self._scroll_offset = scroll
        total = sum(_safe_float(ent.get(amount_key)) for ent in sorted_rows)
        if total <= 0:
            try:
                totals = ((self._act_snapshot or {}).get('render_spec') or {}).get('totals') or {}
                total = _safe_float(totals.get('heal' if is_heal else 'damage'))
            except Exception:
                total = 0.0
        max_amount = max([_safe_float(ent.get(amount_key)) for ent in sorted_rows], default=0.0)
        for ent in sorted_rows[scroll: scroll + self.MAX_ROWS]:
            uid = _safe_int(ent.get('uid'))
            amount = _safe_float(ent.get(amount_key))
            rows.append({
                'uid': uid,
                'name': str(ent.get('name') or f'Player_{uid or 0}'),
                'profession': str(ent.get('profession') or ''),
                'fight_point': _safe_int(ent.get('fight_point')),
                'is_self': bool(ent.get('is_self')) or (self._self_uid and uid == self._self_uid),
                'amount': amount,
                'rate': _safe_float(ent.get(rate_key)),
                'pct': (amount / total) if total > 0 else _safe_float(ent.get('damage_pct')),
                'bar_pct': (amount / max_amount) if max_amount > 0 else 0.0,
                # carry the MEM cross-check total straight from the render_spec ent (it is
                # already present per combat_analytics _entity_rows) -- the live path emits
                # it at row.mem_damage_total but the ACT path early-returns before that.
                'mem_damage_total': _safe_int(ent.get('mem_damage_total')),
                'mem_dps': _safe_int(ent.get('mem_dps')),
                'is_heal': is_heal,
                'fx_tier': '',
                'fx_start': 0.0,
                'fallback_sub': 'ACT RENDER ROW',
            })
        return rows

    def _build_view_rows(self) -> List[dict]:
        is_heal = self._current_tab == 'heal'
        rows: List[dict] = []

        if self._view_mode == 'report':
            entities = self._report_entities()
            entities.sort(
                key=lambda ent: _safe_float(ent.get('heal_total' if is_heal else 'damage_total')),
                reverse=True,
            )
            scroll = max(0, min(self._scroll_offset_report, max(0, len(entities) - self.MAX_ROWS)))
            self._scroll_offset_report = scroll
            total = _safe_float((self._last_report or {}).get(
                'total_heal' if is_heal else 'total_damage'
            ))
            max_amount = max(
                [_safe_float(ent.get('heal_total' if is_heal else 'damage_total'))
                 for ent in entities],
                default=0.0,
            )
            for ent in entities[scroll: scroll + self.MAX_ROWS]:
                uid = _safe_int(ent.get('uid'))
                amount = _safe_float(ent.get('heal_total' if is_heal else 'damage_total'))
                rows.append({
                    'uid': uid,
                    'name': str(ent.get('name') or f'Player_{uid or 0}'),
                    'profession': str(ent.get('profession') or ''),
                    'fight_point': _safe_int(ent.get('fight_point')),
                    'is_self': bool(ent.get('is_self')) or (self._self_uid and uid == self._self_uid),
                    'amount': amount,
                    'rate': _safe_float(ent.get('hps' if is_heal else 'dps')),
                    'pct': (amount / total) if total > 0 else 0.0,
                    'bar_pct': (amount / max_amount) if max_amount > 0 else 0.0,
                    'mem_damage_total': _safe_int(ent.get('mem_damage_total')),
                    'mem_dps': _safe_int(ent.get('mem_dps')),
                    'is_heal': is_heal,
                    'fx_tier': '',
                    'fx_start': 0.0,
                    'fallback_sub': 'LAST REPORT ENTRY',
                })
            return rows

        act_rows = self._act_render_rows()
        if act_rows:
            return self._build_act_view_rows(act_rows, is_heal)

        live_rows = list(self._rows.values())
        if is_heal:
            live_rows.sort(key=lambda row: (row.disp_heal, row.uid), reverse=True)
        else:
            live_rows.sort(key=lambda row: (row.disp_y, row.uid))
        scroll = max(0, min(self._scroll_offset, max(0, len(live_rows) - self.MAX_ROWS)))
        self._scroll_offset = scroll
        total = self._disp_total_heal if is_heal else self._disp_total_damage
        max_amount = max(
            [row.disp_heal if is_heal else row.disp_damage for row in live_rows],
            default=0.0,
        )
        for row in live_rows[scroll: scroll + self.MAX_ROWS]:
            amount = row.disp_heal if is_heal else row.disp_damage
            rows.append({
                'uid': row.uid,
                'name': row.name,
                'profession': row.profession,
                'fight_point': row.fight_point,
                'is_self': row.is_self,
                'amount': amount,
                'rate': row.disp_hps if is_heal else row.disp_dps,
                'pct': (amount / total) if total > 0 else 0.0,
                'bar_pct': (amount / max_amount) if max_amount > 0 else 0.0,
                'mem_damage_total': row.mem_damage_total,
                'mem_dps': row.mem_dps,
                'is_heal': is_heal,
                'fx_tier': row.fx_tier,
                'fx_start': row.fx_start,
                'fallback_sub': 'LIVE COMBAT ENTRY',
            })
        return rows

    def _compute_size(self) -> tuple:
        if self._detail_mode:
            w = max(self.DETAIL_MIN_W, min(self.DETAIL_MAX_W, int(self._detail_w)))
            h = max(self.DETAIL_MIN_H, min(self.DETAIL_MAX_H, int(self._detail_h)))
            self._detail_w, self._detail_h = w, h
            return (w, h)
        if self._minimized:
            total = (self.BODY_PAD * 2
                     + self._header_height()
                     + self.FOOTER_H)
            return (self.WIDTH, min(self.MAX_HEIGHT, max(150, total)))
        rows = self._current_row_count()
        rows = max(rows, 3)      # reserve a minimum list area for empty state
        list_h = rows * self.ROW_H + self.ROW_MARGIN
        total = (self.BODY_PAD * 2
                 + self._header_height()
                 + self._tabs_height()
                 + list_h
                 + self.CONTENT_PAD_BOT
                 + self.FOOTER_H)
        total = min(self.MAX_HEIGHT, max(self.DEFAULT_HEIGHT, total))
        return (self.WIDTH, total)

    @_probe.decorate('ui.dps.render')
    def _render(self, now: float) -> None:
        require_entity_gpu('DpsOverlay', _gow)
        raise RuntimeError('DpsOverlay legacy ULW render path is disabled; GPU presentation is required')

    def _build_shell_layer(self, w: int, h: int) -> Image.Image:
        """Compose the static shell (shadow + body + corners) once per size.

        阴影 polygon 沿用 shell 切角形状; 偏移 + blur 后会从切角区域漏出来,
        所以最后用切角三角形 mask 把那两块抠掉, 让 cut 区透明 (只露背景)。
        """
        layer = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        sx, sy = self.BODY_PAD, self.BODY_PAD
        sw, sh = w - 2 * self.BODY_PAD, h - 2 * self.BODY_PAD
        c = self.SHELL_CUT

        shadow_layers = []
        if self.SHELL_AMBIENT_SHADOW[3] > 0:
            ambient = Image.new('RGBA', (w, h), (0, 0, 0, 0))
            amb_poly = [(p[0] + 4, p[1] + 8) for p in
                        self._shell_polygon(sx, sy, sw, sh)]
            ImageDraw.Draw(ambient, 'RGBA').polygon(
                amb_poly, fill=self.SHELL_AMBIENT_SHADOW)
            shadow_layers.append(_gpu_blur(ambient, 8))

        if self.SHELL_CONTACT_SHADOW[3] > 0:
            contact = Image.new('RGBA', (w, h), (0, 0, 0, 0))
            ct_poly = [(p[0] + 2, p[1] + 4) for p in
                       self._shell_polygon(sx, sy, sw, sh)]
            ImageDraw.Draw(contact, 'RGBA').polygon(
                ct_poly, fill=self.SHELL_CONTACT_SHADOW)
            shadow_layers.append(_gpu_blur(contact, 3))

        # 3) 把 shell 自己的两个切角三角形从阴影里抠掉 (避免漏底)
        # mask: 255 = 保留, 0 = 抠掉
        cut_mask = Image.new('L', (w, h), 255)
        cd = ImageDraw.Draw(cut_mask)
        # 右上切角三角形 (从 shell 视角)
        cd.polygon([
            (sx + sw - c, sy),
            (sx + sw, sy),
            (sx + sw, sy + c),
        ], fill=0)
        # 左下切角三角形
        cd.polygon([
            (sx, sy + sh - c),
            (sx + c, sy + sh),
            (sx, sy + sh),
        ], fill=0)
        # 同时把 shell 主体内部也抠掉 (反正会被 shell 覆盖, 提前剔除避免 alpha 累积)
        cd.polygon(self._shell_polygon(sx, sy, sw, sh), fill=0)

        for shadow_layer in shadow_layers:
            a = shadow_layer.getchannel('A')
            from PIL import ImageChops
            a = ImageChops.multiply(a, cut_mask)
            shadow_layer.putalpha(a)
            layer.alpha_composite(shadow_layer)

        self._draw_shell(layer, sx, sy, sw, sh)
        self._draw_corners(ImageDraw.Draw(layer, 'RGBA'), sx, sy, sw, sh)
        return layer

    def compose_frame(self, now: Optional[float] = None) -> Image.Image:
        """Compose the current overlay frame to a PIL RGBA image.

        Used by the ULW renderer and by test harnesses. Does not touch
        any Win32 resources.
        """
        if now is None:
            now = time.time()
        w, h = self._compute_size()

        if self._shell_cache is None or self._shell_cache_size != (w, h):
            self._shell_cache = self._build_shell_layer(w, h)
            self._shell_cache_size = (w, h)
        img = self._shell_cache.copy()

        sx, sy = self.BODY_PAD, self.BODY_PAD
        sw, sh = w - 2 * self.BODY_PAD, h - 2 * self.BODY_PAD

        draw = ImageDraw.Draw(img, 'RGBA')
        hh = self._header_height()
        self._draw_header(draw, img, sx, sy, sw, hh)
        # Reset captured click regions for this frame.
        self._row_click_regions = []
        self._detail_back_rect = None
        self._resize_rect = None
        self._list_rect = None
        if self._minimized and not self._detail_mode:
            footer_y = sy + sh - self.FOOTER_H
        elif self._detail_visible:
            content_y = sy + hh
            footer_y = sy + sh - self.FOOTER_H
            list_x = sx + self.CONTENT_PAD_X
            list_w = sw - 2 * self.CONTENT_PAD_X
            list_h = footer_y - content_y - self.CONTENT_PAD_BOT
            self._list_rect = (list_x, content_y, list_w, list_h)
            self._draw_detail_view(draw, img, list_x, content_y, list_w, list_h)
        else:
            tabs_y = sy + hh
            self._draw_tabs(draw, sx, tabs_y, sw)
            content_y = tabs_y + self._tabs_height()
            footer_y = sy + sh - self.FOOTER_H
            list_x = sx + self.CONTENT_PAD_X
            list_w = sw - 2 * self.CONTENT_PAD_X
            list_h = footer_y - content_y - self.CONTENT_PAD_BOT
            self._list_rect = (list_x, content_y, list_w, list_h)
            self._draw_list_frame(draw, img, list_x, content_y, list_w, list_h)
        self._draw_footer(draw, sx, footer_y, sw, self.FOOTER_H)

        if self._panel_fx_tier:
            self._overlay_panel_flash(img, sx, sy, sw, sh, now)

        if self._detail_mode:
            self._draw_resize_grip(draw, w, h)

        final_alpha = max(0.0, min(1.0, self.PANEL_OPACITY * self._fade_alpha))
        if final_alpha < 0.999:
            img = multiply_alpha_image(img, final_alpha)
        return img

    # --------  Shell (gradient + scanlines + cut-corner borders, web/dps.html parity)  --------

    # SAO 切角尺寸 (web/dps.html 用 22px 对角剪切)
    SHELL_CUT = 22

    def _shell_polygon(self, sx: int, sy: int, sw: int, sh: int,
                        inset: int = 0):
        """web/dps.html clip-path:polygon 几何 — 右上角 + 左下角各切 22px 对角。"""
        c = max(2, self.SHELL_CUT - inset)
        x0, y0 = sx + inset, sy + inset
        x1, y1 = sx + sw - 1 - inset, sy + sh - 1 - inset
        return [
            (x0, y0),
            (x1 - c, y0),
            (x1, y0 + c),
            (x1, y1),
            (x0 + c, y1),
            (x0, y1 - c),
        ]

    def _draw_shell(self, img: Image.Image,
                    sx: int, sy: int, sw: int, sh: int) -> None:
        # 1) Vertical gradient A→B
        grad = np.zeros((sh, 1, 4), dtype=np.uint8)
        ys = np.linspace(0, 1, sh)
        for i in range(4):
            grad[:, 0, i] = (self.PANEL_BG_A[i]
                             + (self.PANEL_BG_B[i] - self.PANEL_BG_A[i]) * ys)
        grad_img = Image.fromarray(grad, 'RGBA').resize((sw, sh))

        # 2) Polygon mask — webview clip-path 切角形状
        local_poly = [(p[0] - sx, p[1] - sy) for p in
                      self._shell_polygon(sx, sy, sw, sh)]
        mask = Image.new('L', (sw, sh), 0)
        ImageDraw.Draw(mask).polygon(local_poly, fill=255)
        img.paste(grad_img, (sx, sy), mask)

        # 3) Top-cyan sheen + bottom shadow (clipped by polygon mask)
        sheen = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
        sd_sheen = ImageDraw.Draw(sheen, 'RGBA')
        sd_sheen.rectangle(
            (1, 1, sw - 2, max(16, int(sh * 0.18))),
            fill=self.SHELL_SHEEN_CYAN,
        )
        sd_sheen.rectangle(
            (2, max(12, int(sh * 0.42)), sw - 3, sh - 3),
            fill=self.SHELL_SHEEN_SHADOW,
        )
        sheen = _gpu_blur(sheen, 4)
        sheen_masked = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
        sheen_masked.paste(sheen, (0, 0), mask)
        img.alpha_composite(sheen_masked, (sx, sy))

        # 4) Subtle horizontal scanlines (every 4px, web parity)
        scan = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
        sd = ImageDraw.Draw(scan)
        for y in range(0, sh, 4):
            sd.line((0, y, sw, y), fill=self.SCAN_LINE)
        scan_masked = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
        scan_masked.paste(scan, (0, 0), mask)
        img.alpha_composite(scan_masked, (sx, sy))

        draw = ImageDraw.Draw(img, 'RGBA')

        # 5) Outer border — cyan-edged polygon
        outer_poly = self._shell_polygon(sx, sy, sw, sh)
        draw.line(outer_poly + [outer_poly[0]],
                  fill=self.PANEL_EDGE, width=1)

        # 6) Inner highlight — white polygon inset 1px
        inner_poly = self._shell_polygon(sx, sy, sw, sh, inset=1)
        draw.line(inner_poly + [inner_poly[0]],
                  fill=self.INNER_HIGHLIGHT, width=1)

        # 7) Top-edge cyan thin line (web/dps.html ::after top 2px)
        draw.line(
            (sx + 1, sy + 2, sx + sw - 2 - self.SHELL_CUT, sy + 2),
            fill=self.PANEL_LINE, width=1,
        )

        # 8) Bottom cyan→gold gradient line — SAO 招牌 (web/dps.html ::after bottom)
        line_y = sy + sh - 2
        line_x0 = sx + self.SHELL_CUT + 2
        line_x1 = sx + sw - 3
        line_w = max(0, line_x1 - line_x0)
        if line_w > 0:
            grad_line = Image.new('RGBA', (line_w, 1), (0, 0, 0, 0))
            arr = np.zeros((1, line_w, 4), dtype=np.uint8)
            for i in range(line_w):
                t = i / max(1, line_w - 1)
                if t < 0.30:
                    a = int(220 * (t / 0.30))
                    arr[0, i] = (104, 228, 255, a)
                elif t < 0.70:
                    u = (t - 0.30) / 0.40
                    r = int(104 + (243 - 104) * u)
                    g = int(228 + (175 - 228) * u)
                    b = int(255 + (18 - 255) * u)
                    a = int(220 + (200 - 220) * u)
                    arr[0, i] = (r, g, b, a)
                else:
                    a = int(200 * (1.0 - (t - 0.70) / 0.30))
                    arr[0, i] = (243, 175, 18, max(0, a))
            grad_line = Image.fromarray(arr, 'RGBA')
            img.alpha_composite(grad_line, (line_x0, line_y))

    def _draw_corners(self, draw: ImageDraw.ImageDraw,
                      sx: int, sy: int, sw: int, sh: int) -> None:
        """高亮 cut-corner 转折点 — 强化 SAO 切角观感。"""
        c = self.SHELL_CUT
        # Top-left full corner — bright cyan accent
        cyan = self.CYAN
        cs = self.CORNER_SIZE
        draw.line((sx + 2, sy + 2, sx + 2 + cs, sy + 2),
                  fill=cyan, width=2)
        draw.line((sx + 2, sy + 2, sx + 2, sy + 2 + cs),
                  fill=cyan, width=2)
        # Top-right diagonal cut — 沿对角线画 2px 青线
        draw.line(
            (sx + sw - 1 - c, sy + 1, sx + sw - 1, sy + 1 + c),
            fill=self.CORNER_CYAN_ACCENT, width=2,
        )
        # Bottom-left diagonal cut — 沿对角线画 2px 金线
        draw.line(
            (sx + 1, sy + sh - 1 - c, sx + 1 + c, sy + sh - 1),
            fill=self.CORNER_GOLD_ACCENT, width=2,
        )
        # Bottom-right full corner — gold accent
        gold_c = self.CORNER_GOLD
        bx = sx + sw - 2
        by = sy + sh - 2
        draw.line((bx - cs, by, bx, by), fill=gold_c, width=2)
        draw.line((bx, by - cs, bx, by), fill=gold_c, width=2)

    # --------  Header  --------

    def _act_badge_text(self) -> str:
        try:
            spec = (self._act_snapshot or {}).get('render_spec') or {}
            if not spec:
                return ''
            sources = spec.get('sources') or (self._act_snapshot or {}).get('sources') or {}
            summary = sources.get('summary') or {}
            packet = sources.get('packet') or {}
            source = str(summary.get('data_source') or packet.get('data_source') or 'tcp').upper()
            mode = str(spec.get('mode') or self._view_mode or 'live').upper()
            boss = spec.get('boss') or {}
            hp_source = str(boss.get('hp_source') or '')
            parts = [f"ACT V{int(spec.get('version') or 1)}", mode, source]
            if hp_source and hp_source != 'none':
                parts.append(f'BOSS {hp_source.upper()}')
            return ' · '.join(parts)
        except Exception:
            return ''

    def _act_trigger_text(self) -> str:
        try:
            triggers = (self._act_snapshot or {}).get('triggers') or {}
            emitted = triggers.get('emitted') or []
            recent = triggers.get('recent') or []
            events = emitted or recent
            if not events:
                return ''
            event = (events[-1] if emitted else events[0]) or {}
            text = str(event.get('message') or event.get('label') or event.get('rule_id') or event.get('trigger_type') or '').strip()
            if not text:
                return ''
            return f'ACT ALERT {text.upper()}'
        except Exception:
            return ''

    def _draw_header(self, draw: ImageDraw.ImageDraw, img: Image.Image,
                     sx: int, sy: int, sw: int, hh: int) -> None:
        x_left = sx + self.HEADER_PAD_X
        x_right = sx + sw - self.HEADER_PAD_X
        y = sy + self.HEADER_PAD_TOP

        # Eyebrow: "TACTICAL ANALYTICS" (uppercase, letter-spacing 2.2)
        font_eye = _load_font('sao', 10)
        self._draw_tracked(draw,
                           (x_left, y),
                           'TACTICAL ANALYTICS',
                           font_eye, self.TEXT_MUTED, 2.2)
        y += self.EYEBROW_H + 4

        # Title row: "DPS / HPS" + mode-badge (web CSS: font-size 18 but
        # SAOUI.ttf renders smaller-looking; scale up for visual parity)
        font_title = _load_font('sao', 22)
        title_text = 'DPS / HPS'
        self._draw_tracked(draw, (x_left, y - 2), title_text,
                           font_title, self.TEXT_MAIN, 1.6)
        title_w = self._tracked_text_width(draw, title_text, font_title, 1.6)

        badge_label = 'LIVE'
        badge_color = self.BADGE_LIVE
        if self._view_mode == 'report':
            badge_label = 'LAST REPORT'
            badge_color = self.BADGE_REPORT
        bx = x_left + title_w + 10
        by = y
        font_badge = _load_font('sao', 10)
        bw_text = self._tracked_text_width(draw, badge_label, font_badge, 1.1)
        bw = bw_text + 16
        bh_ = 20
        self._draw_clip_rect(draw, bx, by, bw, bh_,
                             fill=self.HEADER_BADGE_FILL,
                             outline=self.HEADER_BADGE_BORDER)
        self._draw_tracked(draw, (bx + 8, by + 4), badge_label,
                           font_badge, badge_color, 1.1)
        act_label = self._act_badge_text()
        if act_label:
            act_text_w = self._tracked_text_width(draw, act_label, font_badge, 1.0)
            act_w = act_text_w + 14
            act_x = bx + bw + 6
            if act_x + act_w <= x_right:
                self._draw_clip_rect(draw, act_x, by, act_w, bh_,
                                     fill=(230, 248, 252, 120),
                                     outline=(60, 160, 190, 150))
                self._draw_tracked(draw, (act_x + 7, by + 4), act_label,
                                   font_badge, self.BTN_LIVE_COLOR, 1.0)
        y += self.TITLE_H

        # Summary
        if self._view_mode == 'report':
            if self._has_report_data():
                completed = str((self._last_report or {}).get('completed_local_time') or 'COMPLETED')
                summary = f'LAST REPORT · {completed}'
            else:
                summary = 'NO LAST REPORT AVAILABLE'
        elif self._encounter_active:
            summary = (
                f'LIVE DPS {_fmt_num(self._disp_total_dps)} · '
                f'LIVE HPS {_fmt_num(self._disp_total_hps)}'
            )
        else:
            summary = 'WAITING FOR COMBAT DATA'
        trigger_summary = self._act_trigger_text()
        if trigger_summary and self._view_mode == 'live':
            summary = f'{summary} · {trigger_summary}'
        font_sum = _load_font('sao', 10)
        self._draw_tracked(draw, (x_left, y + 2),
                           summary, font_sum, self.TEXT_MUTED, 0.85)
        y += self.SUMMARY_H + 8

        # Button rows: wrap instead of shrinking into unreadable controls.
        buttons = self._button_specs()
        btn_font = _load_font('sao', 10)
        sizes, gap = self._button_layout_sizes(draw, btn_font, max(1, x_right - x_left))
        rows: List[List[Tuple[Tuple[str, str, bool, str, bool], int]]] = []
        cur: List[Tuple[Tuple[str, str, bool, str, bool], int]] = []
        cur_w = 0
        for spec, bw2 in zip(buttons, sizes):
            add = bw2 if not cur else bw2 + gap
            if cur and cur_w + add > max(1, x_right - x_left):
                rows.append(cur)
                cur = [(spec, bw2)]
                cur_w = bw2
            else:
                cur.append((spec, bw2))
                cur_w += add
        if cur:
            rows.append(cur)
        by = y
        for row in rows:
            row_w = sum(size for _spec, size in row) + gap * (len(row) - 1)
            bx = x_right - row_w
            for (_name, text, active, kind, enabled), bw2 in row:
                self._draw_button(draw, bx, by, bw2, self.BTN_H,
                                  text, active, kind, btn_font, enabled)
                bx += bw2 + gap
            by += self.BTN_H + 5

        notice = self._panel_notice_text()
        if notice:
            nf = _load_font('sao', 9)
            notice_y = max(sy + hh - 18, y + self.BTN_H + 3)
            self._draw_tracked(draw, (x_left, notice_y),
                               self._truncate(notice.upper(), nf, max(40, x_right - x_left), draw),
                               nf, self.GOLD, 0.75)

        # Bottom border of header
        draw.line((sx, sy + hh, sx + sw - 1, sy + hh),
                  fill=self.DIVIDER, width=1)

    def _button_specs(self) -> List[Tuple[str, str, bool, str, bool]]:
        report_ok = self._report_available or self._has_report_data()
        history_ok = report_ok or callable(self._list_history_cb)
        detail_label = 'NORMAL' if self._detail_mode else 'DETAIL'
        minimize_label = 'RESTORE' if self._minimized else 'MIN'
        return [
            ('minimize', minimize_label, bool(self._minimized), 'normal', True),
            ('live', 'LIVE', self._view_mode == 'live' and not self._detail_mode,
             'live', True),
            ('detail', detail_label, bool(self._detail_mode), 'normal', True),
            ('report', 'REPORT', self._view_mode == 'report' and not self._detail_mode,
             'normal', report_ok),
            ('history', 'HISTORY', False, 'normal', history_ok),
            ('export', 'EXPORT', False, 'normal', history_ok),
            ('reset', 'RESET', False, 'danger', True),
        ]

    def _button_layout_sizes(self, draw: ImageDraw.ImageDraw, font,
                             available_w: int) -> Tuple[List[int], int]:
        sizes = [max(58, _text_width(draw, label, font) + 18)
                 for _name, label, _active, _kind, _enabled in self._button_specs()]
        gap = self.BTN_GAP
        return sizes, gap

    def _button_row_count(self) -> int:
        dummy = ImageDraw.Draw(Image.new('RGBA', (1, 1), (0, 0, 0, 0)))
        font = _load_font('sao', 10)
        sizes, gap = self._button_layout_sizes(
            dummy, font, max(1, self.WIDTH - 2 * self.BODY_PAD - 2 * self.HEADER_PAD_X))
        available_w = max(1, self.WIDTH - 2 * self.BODY_PAD - 2 * self.HEADER_PAD_X)
        rows = 1
        used = 0
        for size in sizes:
            add = size if used <= 0 else size + gap
            if used > 0 and used + add > available_w:
                rows += 1
                used = size
            else:
                used += add
        return max(1, rows)

    def _draw_button(self, draw: ImageDraw.ImageDraw, bx: int, by: int,
                     bw: int, bh: int, text: str, active: bool,
                     kind: str, font, enabled: bool = True) -> None:
        if not enabled:
            fill = self.BTN_DISABLED_FILL
            border = self.BTN_DISABLED_BORDER
            fg = self.BTN_DISABLED_FG
        elif kind == 'live' and active:
            fill = self.BTN_LIVE_ACTIVE
            border = self.BTN_LIVE_BORDER
            fg = self.BTN_LIVE_COLOR
        elif active:
            fill = self.BTN_ACTIVE_FILL
            border = self.BTN_ACTIVE_BORDER
            fg = self.GOLD
        else:
            fill = self.BTN_BG
            border = self.BTN_BORDER
            fg = self.TEXT_MAIN
        self._draw_clip_rect(draw, bx, by, bw, bh,
                             fill=fill, outline=border, bevel=8)
        self._draw_tracked_centered(draw, text, font, fg,
                                    bx + bw // 2,
                                    by + (bh - 12) // 2, 1.0)

    # --------  Tabs  --------

    def _draw_tabs(self, draw: ImageDraw.ImageDraw,
                   sx: int, ty: int, sw: int) -> None:
        y = ty + self.TAB_PAD_TOP
        x0 = sx + self.HEADER_PAD_X
        x1 = sx + sw - self.HEADER_PAD_X
        tabs = [('DAMAGE', self._current_tab == 'damage'),
            ('HEALING', self._current_tab == 'heal')]
        gap = 8
        tab_w = (x1 - x0 - gap) // 2
        tf = _load_font('sao', 10)
        for i, (label, active) in enumerate(tabs):
            tx = x0 + i * (tab_w + gap)
            if active:
                fill = self.TAB_ACTIVE_FILL
                border = self.TAB_ACTIVE_BORDER
                fg = self.GOLD
            else:
                fill = self.TAB_INACTIVE_FILL
                border = self.TAB_INACTIVE_BORDER
                fg = self.TEXT_MUTED
            self._draw_clip_rect(draw, tx, y, tab_w, self.TAB_H,
                                 fill=fill, outline=border, bevel=10)
            tw = self._tracked_text_width(draw, label, tf, 1.2)
            self._draw_tracked_centered(draw, label, tf, fg,
                                        tx + tab_w // 2,
                                        y + (self.TAB_H - 12) // 2, 1.2)

    # --------  List frame + rows  --------

    def _draw_list_frame(self, draw: ImageDraw.ImageDraw, img: Image.Image,
                         lx: int, ly: int, lw: int, lh: int) -> None:
        # Frame background (dark)
        self._fill_rounded_rect(
            img, (lx, ly, lx + lw - 1, ly + lh - 1), radius=4, fill=self.LIST_BG
        )
        self._fill_rounded_rect(
            img, (lx + 2, ly + 2, lx + lw - 3, ly + min(lh // 3, 18)),
            radius=4, fill=self.LIST_CYAN_TINT,
        )
        self._fill_rounded_rect(
            img, (lx + 3, ly + max(14, lh // 2), lx + lw - 4, ly + lh - 4),
            radius=4, fill=self.LIST_SHADOW,
        )
        draw.rounded_rectangle(
            (lx, ly, lx + lw - 1, ly + lh - 1),
            radius=4, outline=self.LIST_BORDER, width=1,
        )
        view_rows = self._build_view_rows()
        if not view_rows:
            msg = 'NO REPORT DATA' if self._view_mode == 'report' else 'NO LIVE COMBAT DATA'
            font = _load_font('sao', 11)
            self._draw_tracked_centered(draw, msg, font, self.TEXT_MUTED,
                                        lx + lw // 2, ly + lh // 2 - 6, 2)
            return

        self._draw_scroll_affordance(draw, lx, ly, lw, lh)

        margin = self.ROW_MARGIN
        rank_base = self._current_scroll_offset()
        for rank_idx, row_data in enumerate(view_rows):
            ry = ly + margin + rank_idx * self.ROW_H
            rx = lx + margin
            rw = lw - 2 * margin
            rh = self.ROW_H - margin
            if ry + rh > ly + lh - 2:
                continue
            self._draw_row(draw, img, row_data, rx, ry, rw, rh, rank_base + rank_idx)
            uid = _safe_int(row_data.get('uid'))
            if uid > 0:
                self._row_click_regions.append(
                    (uid, (rx, ry, rx + rw, ry + rh))
                )

    def _draw_row(self, draw: ImageDraw.ImageDraw, img: Image.Image,
                  row: dict, x: int, y: int, w: int, h: int,
                  rank_idx: int) -> None:
        bevel = 10
        # Row background with clip-path:polygon(10px 0,100% 0,100% 100%,0 100%,0 10px)
        self._draw_clip_rect(draw, x, y, w, h,
                             fill=self.ROW_BG, outline=self.ROW_BORDER,
                             bevel=bevel)
        glow_color = self.GOLD_SOFT if not row['is_heal'] else self.SKILL_BAR_HEAL
        glow_alpha = 44 if row['is_self'] else 26
        self._fill_polygon(
            img,
            [
                (x + bevel + 2, y + h - 8),
                (x + w - 6, y + h - 8),
                (x + w - 2, y + h - 2),
                (x + 2, y + h - 2),
            ],
            (glow_color[0], glow_color[1], glow_color[2], glow_alpha),
        )
        top_sheen = [
            (x + bevel + 1, y + 1),
            (x + w - 2, y + 1),
            (x + w - 3, y + min(h // 3 + 1, h - 4)),
            (x + 2, y + min(h // 3 + 1, h - 4)),
            (x + 1, y + bevel),
        ]
        self._fill_polygon(img, top_sheen, self.ROW_SHEEN_CYAN)
        self._fill_polygon(
            img,
            [
                (x + bevel, y + max(8, h // 2)),
                (x + w - 2, y + max(8, h // 2)),
                (x + w - 2, y + h - 2),
                (x, y + h - 2),
                (x, y + bevel),
            ],
            self.ROW_LOWER_SHADOW,
        )
        # Self highlight: left gold border
        if row['is_self']:
            self._fill_polygon(
                img,
                [
                    (x, y + bevel),
                    (x + 3, y + bevel - 3),
                    (x + 3, y + h - 2),
                    (x, y + h - 2),
                ],
                self.ROW_SELF_BAR,
            )
            self._draw_clip_rect(
                draw, x, y, w, h,
                outline=self.ROW_SELF_OUTLINE,
                bevel=bevel,
            )
            self._fill_polygon(
                img,
                [
                    (x + bevel, y + 1),
                    (x + w - 2, y + 1),
                    (x + w - 2, y + h - 2),
                    (x, y + h - 2),
                    (x, y + bevel),
                ],
                self.ROW_SELF_TINT,
            )

        # Animated bar fill (within clip)
        bar_pct = _safe_float(row.get('bar_pct'), min_value=0.0, max_value=1.0)
        bar_w = int(max(0, (w - 4) * bar_pct))
        if bar_w > 0:
            bar_img = self._make_bar(bar_w, h - 4, row['is_self'], row['is_heal'])
            img.alpha_composite(bar_img, (x + 2, y + 2))
            lead_x = x + 2 + max(0, bar_w - 2)
            draw.line(
                (lead_x, y + 4, lead_x, y + h - 5),
                fill=self.CYAN if row['is_heal'] else self.GOLD,
                width=1,
            )

        # Hit-fx pulse outline
        if row['fx_tier']:
            dur, tint, _ = _HIT_FX_TIERS[row['fx_tier']]
            age = time.time() - row['fx_start']
            t = max(0.0, min(1.0, age / max(0.01, dur)))
            intensity = (1.0 - t) ** 2
            outline_a = int(220 * intensity)
            if outline_a > 8:
                self._draw_clip_rect(
                    draw, x, y, w, h,
                    outline=(tint[0], tint[1], tint[2], outline_a),
                    bevel=bevel,
                )

        # Rank number (web: plain digit coloured by rank, no #)
        rank_color = self.RANK_COLORS.get(rank_idx, self.TEXT_MUTED[:3])
        rf = _load_font('sao', 11)
        rank_txt = str(rank_idx + 1)
        rw = _text_width(draw, rank_txt, rf)
        draw.text((x + 10 + (20 - rw) // 2, y + 12), rank_txt,
                  fill=rank_color + (255,), font=rf)

        # Name + subtitle
        name_x = x + 38
        name_font = _pick_font(row['name'], 12)
        name_color = self.GOLD if row['is_self'] else self.TEXT_MAIN
        max_name_w = int(w * 0.55) - 40
        name = self._truncate(row['name'] or 'Unknown', name_font,
                              max_name_w, draw)

        # Hit-FX text color + shadow (web: .entity-row.impact-hit etc.)
        _fx_shadow = None
        if row['fx_tier']:
            _dur, _tint, _ = _HIT_FX_TIERS[row['fx_tier']]
            _age = time.time() - row['fx_start']
            _t = max(0.0, min(1.0, _age / max(0.01, _dur)))
            _int = max(0.0, (1.0 - _t) ** 2)
            if row['fx_tier'] == 'impact':
                name_color = (57, 126, 146, 255)
                _fx_shadow = (104, 228, 255, int(66 * _int))
            elif row['fx_tier'] == 'mega':
                name_color = (196, 135, 16, 255)
                _fx_shadow = (255, 220, 112, int(87 * _int))
            elif row['fx_tier'] == 'starburst':
                name_color = (110, 118, 182, 255)
                _fx_shadow = (88, 166, 255, int(77 * _int))

        self._draw_tracked(draw, (name_x, y + 7), name,
                           name_font, name_color, 0.7,
                           shadow_color=_fx_shadow, shadow_blur=5 if _fx_shadow else 0)

        sub_parts = []
        if row['profession']:
            sub_parts.append(row['profession'])
        fp = _fmt_fp(row['fight_point'])
        if fp:
            sub_parts.append(fp)
        sub_text = ' · '.join(sub_parts).upper() if sub_parts else row['fallback_sub']
        if sub_text:
            sub_font = _pick_font(sub_text, 9)
            sub = self._truncate(sub_text, sub_font, max_name_w, draw)
            self._draw_tracked(draw, (name_x, y + 22),
                               sub, sub_font, self.TEXT_MUTED, 0.75)

        # Right side: damage total (13px) + dps/pct (9px muted)
        font_val = _load_font('sao', 13)
        font_sub = _load_font('sao', 9)
        amount = _safe_float(row.get('amount'))
        rate = _safe_float(row.get('rate'))
        val_main = _fmt_num(amount)
        val_color = name_color if _fx_shadow else self.TEXT_MAIN
        vw = self._tracked_text_width(draw, val_main, font_val, 0.7)
        self._draw_tracked(draw, (x + w - 10 - vw, y + 6), val_main,
                           font_val, val_color, 0.7,
                           shadow_color=_fx_shadow, shadow_blur=5 if _fx_shadow else 0)
        pct = int(round(_safe_float(row.get('pct')) * 100))
        val_sub = f'{_fmt_num(rate)}/s · {pct}%'
        # MEM cross-check: per-encounter DPS from the game's own DamageDataMgr (cumulative total
        # scoped to the MEM combat window). The cumulative total is meaningless as a per-fight
        # figure (only accrues at the Association dummy), so show DPS. Parity with _memBadge.
        mem_dps = _safe_int(row.get('mem_dps'))
        if (not row.get('is_heal')) and mem_dps:
            val_sub += f' · MEM {_fmt_num(mem_dps)}/s'
        sw_ = self._tracked_text_width(draw, val_sub, font_sub, 0.75)
        self._draw_tracked(draw, (x + w - 10 - sw_, y + 22), val_sub,
                           font_sub, self.TEXT_MUTED, 0.75)

    def _draw_scroll_affordance(self, draw: ImageDraw.ImageDraw, lx: int, ly: int, lw: int, lh: int) -> None:
        max_off = self._max_scroll_offset()
        if max_off <= 0:
            return
        offset = self._current_scroll_offset()
        track_x = lx + lw - 8
        track_y = ly + 8
        track_h = max(24, lh - 16)
        draw.rounded_rectangle(
            (track_x, track_y, track_x + 3, track_y + track_h),
            radius=2, fill=(104, 228, 255, 70),
        )
        thumb_h = max(16, int(track_h * self.MAX_ROWS / max(self.MAX_ROWS + max_off, 1)))
        thumb_y = track_y + int((track_h - thumb_h) * offset / max(max_off, 1))
        draw.rounded_rectangle(
            (track_x - 1, thumb_y, track_x + 4, thumb_y + thumb_h),
            radius=2, fill=self.CORNER_GOLD,
        )
        visible = len(self._build_view_rows())
        total = max(self.MAX_ROWS, max_off + self.MAX_ROWS)
        hint = f'{offset + 1}-{offset + visible}/{total}'
        hint_font = _load_font('sao', 8)
        self._draw_tracked(draw, (lx + lw - 72, ly + lh - 15), hint,
                           hint_font, self.TEXT_MUTED, 0.6)

    # --------  Footer  --------

    def _draw_footer(self, draw: ImageDraw.ImageDraw,
                     sx: int, fy: int, sw: int, fh: int) -> None:
        # Top divider (cyan-tinted)
        draw.line((sx, fy, sx + sw - 1, fy), fill=self.DIVIDER, width=1)
        # Footer bg (dark tint)
        self._fill_rect(
            draw._image,
            (sx + 1, fy + 1, sx + sw - 2, fy + fh - 2),
            fill=self.FOOTER_BG,
        )
        self._fill_rect(
            draw._image,
            (sx + 2, fy + 2, sx + sw - 3, fy + min(fh // 2, 16)),
            fill=self.FOOTER_CYAN_TINT,
        )
        self._fill_rect(
            draw._image,
            (sx + 2, fy + max(14, fh // 2), sx + sw - 3, fy + fh - 3),
            fill=self.FOOTER_SHADOW,
        )
        font = _load_font('sao', 10)
        x_left = sx + self.HEADER_PAD_X
        x_right = sx + sw - self.HEADER_PAD_X
        if self._view_mode == 'report' and self._has_report_data():
            completed = str((self._last_report or {}).get('completed_local_time')
                            or _fmt_time(_safe_float((self._last_report or {}).get('elapsed_s'))))
            left = f'COMPLETED {completed}'
            total_val = _fmt_num(_safe_float((self._last_report or {}).get('total_damage')))
            heal_val = _fmt_num(_safe_float((self._last_report or {}).get('total_heal')))
        else:
            left = f'ELAPSED {_fmt_time(self._disp_elapsed)}'
            total_val = _fmt_num(self._disp_total_damage)
            heal_val = _fmt_num(self._disp_total_heal)
        self._draw_tracked(draw, (x_left, fy + (fh - 12) // 2),
                           left, font, self.TEXT_MUTED, 0.85)
        right = f'TOTAL {total_val} · HEAL {heal_val}'
        rw = self._tracked_text_width(draw, right, font, 0.85)
        self._draw_tracked(draw, (x_right - rw, fy + (fh - 12) // 2),
                           right, font, self.TEXT_MUTED, 0.85)

    # --------  Detail view  --------

    def _get_detail_entity(self) -> Optional[dict]:
        uid = _safe_int(self._detail_uid)
        if uid <= 0:
            return None
        if self._view_mode == 'report':
            for ent in self._report_entities():
                if _safe_int(ent.get('uid')) == uid:
                    return ent
            return None
        # Live mode: prefer cached skill breakdown, fall back to row.
        cached = self._live_detail_cache.get(uid)
        if isinstance(cached, dict):
            return cached
        row = self._rows.get(uid)
        if row is None:
            return None
        return {
            'uid': row.uid,
            'name': row.name,
            'profession': row.profession,
            'fight_point': row.fight_point,
            'is_self': row.is_self,
            'damage_total': row.disp_damage,
            'damage_hits': 0,
            'crit_rate': 0.0,
            'heal_total': row.disp_heal,
            'heal_hits': 0,
            'dps': row.disp_dps,
            'hps': row.disp_hps,
            'max_hit': 0,
            'elapsed_s': self._disp_elapsed,
            'skills': [],
        }

    def _draw_detail_view(self, draw: ImageDraw.ImageDraw, img: Image.Image,
                          lx: int, ly: int, lw: int, lh: int) -> None:
        # Outer detail frame (parity with .detail-view)
        self._fill_rounded_rect(
            img, (lx, ly, lx + lw - 1, ly + lh - 1),
            radius=4, fill=self.LIST_BG,
        )
        draw.rounded_rectangle(
            (lx, ly, lx + lw - 1, ly + lh - 1),
            radius=4, outline=self.LIST_BORDER, width=1,
        )

        entity = self._get_detail_entity()

        # Header strip: BACK button + title + badge
        head_h = 30
        head_x = lx + 8
        head_y = ly + 6
        head_w = lw - 16

        back_label = 'NORMAL' if self._detail_mode else 'BACK'
        back_w = 76 if self._detail_mode else 56
        back_rect = (head_x, head_y, head_x + back_w, head_y + head_h - 4)
        self._draw_clip_rect(draw, back_rect[0], back_rect[1],
                             back_rect[2] - back_rect[0],
                             back_rect[3] - back_rect[1],
                             fill=self.BTN_BG, outline=self.BTN_BORDER,
                             bevel=8)
        font_back = _load_font('sao', 10)
        self._draw_tracked_centered(draw, back_label, font_back, self.TEXT_MAIN,
                                    (back_rect[0] + back_rect[2]) // 2,
                                    back_rect[1] + ((back_rect[3] - back_rect[1]) - 12) // 2,
                                    1.0)
        self._detail_back_rect = back_rect

        # Title (entity name + profession + fight point)
        title_x = head_x + back_w + 10
        title_y = head_y + 2
        if entity:
            title = str(entity.get('name') or f'Player_{self._detail_uid}')
            prof = str(entity.get('profession') or '')
            if prof:
                title = f'{title} · {prof}'
            fp = _safe_int(entity.get('fight_point'))
            if fp > 0:
                title = f'{title} · {_fmt_fp(fp)}'
        else:
            title = 'NO DETAIL DATA'
        title_font = _pick_font(title, 12)
        self._draw_tracked(draw, (title_x, title_y), title,
                           title_font, self.TEXT_MAIN, 0.7)

        # Badge (LIVE DETAIL / LAST REPORT DETAIL)
        badge_text = ('LAST REPORT DETAIL' if self._view_mode == 'report'
                      else 'LIVE DETAIL')
        badge_font = _load_font('sao', 9)
        badge_color = (self.BADGE_REPORT if self._view_mode == 'report'
                       else self.BADGE_LIVE)
        bw = self._tracked_text_width(draw, badge_text, badge_font, 1.2)
        badge_x = head_x + head_w - bw - 6
        badge_y = head_y + 16
        self._draw_tracked(draw, (badge_x, badge_y), badge_text,
                           badge_font, badge_color, 1.2)

        body_y = head_y + head_h + 4
        body_h = ly + lh - body_y - 6
        if not entity:
            font = _load_font('sao', 11)
            self._draw_tracked_centered(draw, 'NO DETAIL DATA', font,
                                        self.TEXT_MUTED,
                                        lx + lw // 2,
                                        body_y + body_h // 2 - 6, 2)
            return

        # Stats grid: compact mode uses 2 cols; resizable detail mode spreads
        # the same data into an SRDPS-like wider dashboard.
        grid_x = lx + 10
        grid_w = lw - 20
        col_gap = 6
        cols = 4 if self._detail_mode and grid_w >= 520 else 2
        col_w = (grid_w - col_gap * (cols - 1)) // cols
        card_h = 44 if cols >= 4 else 30
        card_gap = 4
        rows = (8 + cols - 1) // cols
        grid_h = rows * card_h + (rows - 1) * card_gap
        stats = [
            ('DAMAGE', _fmt_num(_safe_float(entity.get('damage_total'))), self.GOLD),
            ('DPS', _fmt_num(_safe_float(entity.get('dps'))), self.TEXT_MAIN),
            ('HEALING', _fmt_num(_safe_float(entity.get('heal_total'))), self.STAT_HEAL_GREEN),
            ('HPS', _fmt_num(_safe_float(entity.get('hps'))), self.TEXT_MAIN),
            ('CRIT', f"{int(round(_safe_float(entity.get('crit_rate')) * 100))}%", self.TEXT_MAIN),
            ('MAX HIT', _fmt_num(_safe_float(entity.get('max_hit'))), self.GOLD),
            ('HITS', str(_safe_int(entity.get('damage_hits'))), self.TEXT_MAIN),
            ('TIME', _fmt_time(_safe_float(entity.get('elapsed_s'))), self.TEXT_MAIN),
        ]
        lbl_font = _load_font('sao', 8)
        val_font = _load_font('sao', 13)
        for i, (label, value, color) in enumerate(stats):
            r = i // cols
            c = i % cols
            cx = grid_x + c * (col_w + col_gap)
            cy = body_y + r * (card_h + card_gap)
            self._fill_rounded_rect(
                img, (cx, cy, cx + col_w - 1, cy + card_h - 1),
                radius=3, fill=self.DETAIL_CARD_BG,
            )
            draw.rounded_rectangle(
                (cx, cy, cx + col_w - 1, cy + card_h - 1),
                radius=3, outline=self.ROW_BORDER, width=1,
            )
            self._draw_tracked(draw, (cx + 6, cy + 3), label,
                               lbl_font, self.TEXT_MUTED, 1.1)
            vw = self._tracked_text_width(draw, value, val_font, 0.7)
            val_y = cy + (20 if cols >= 4 else 13)
            self._draw_tracked(draw, (cx + col_w - 6 - vw, val_y),
                               value, val_font, color, 0.7)

        # Skill rows below the stats grid — "rank board · layered card" mirror
        # of web/dps.html _renderSkillRows: rank badge + name + value/share on
        # the head line, sub-meta below, an independent heat-gradient track at
        # the bottom. The whole list is rendered into a tall content image and
        # the visible window is cropped out at a sub-pixel scroll offset, so it
        # glides smoothly (like the webview .skill-frame) and never spills the
        # panel — partial rows at the top/bottom are clipped by the crop.
        sk_y = body_y + grid_h + 6
        sk_h = ly + lh - sk_y - 4
        if sk_h <= 14:
            self._skill_content_h = 0
            self._skill_view_h = 0
            self._skill_max_scroll = 0.0
            return
        skills_raw = entity.get('skills') or []
        if isinstance(skills_raw, (list, tuple)):
            skills = [sk for sk in skills_raw if isinstance(sk, dict)]
        else:
            skills = []
        if not skills:
            self._skill_content_h = 0
            self._skill_view_h = 0
            self._skill_max_scroll = 0.0
            font = _load_font('sao', 10)
            msg = ('NO SKILL DATA IN LAST REPORT' if self._view_mode == 'report'
                   else 'WAITING FOR LIVE SKILL DETAIL')
            self._draw_tracked_centered(draw, msg, font, self.TEXT_MUTED,
                                        lx + lw // 2, sk_y + sk_h // 2 - 6, 1.5)
            return

        # Sort by max(damage_total, heal_total) like the webview
        skills_sorted = sorted(
            list(skills),
            key=lambda s: max(_safe_float(s.get('total')),
                              _safe_float(s.get('heal_total'))),
            reverse=True,
        )
        amounts = [max(_safe_float(s.get('total')), _safe_float(s.get('heal_total')))
                   for s in skills_sorted]
        max_val = max(amounts, default=0.0) or 1.0   # bar length basis
        sum_val = sum(amounts) or 1.0                 # share-of-total basis

        sk_row_h = 44
        sk_row_gap = 4
        step = sk_row_h + sk_row_gap
        total_sk = len(skills_sorted)
        content_h = total_sk * step - sk_row_gap      # last row has no gap
        view_h = sk_h
        max_scroll = float(max(0, content_h - view_h))

        # Publish geometry for the wheel handler + clamp the eased offsets to
        # the current bounds (the list may have shrunk since the last notch).
        self._skill_content_h = content_h
        self._skill_view_h = view_h
        self._skill_max_scroll = max_scroll
        self._skill_scroll_target = max(0.0, min(self._skill_scroll_target, max_scroll))
        self._skill_scroll_disp = max(0.0, min(self._skill_scroll_disp, max_scroll))
        scroll = self._skill_scroll_disp

        # Reserve a right gutter for the scrollbar only when the list overflows.
        gutter = 8 if max_scroll > 0 else 0
        row_x0 = lx + 6
        row_x1 = lx + lw - 7 - gutter
        view_w = max(1, row_x1 - row_x0 + 1)
        cx1 = view_w - 1                               # card right edge (local)

        # Render every row into a transparent content image (local coords, all
        # >= 0 so _fill_rounded_rect's alpha_composite never sees a negative
        # dest); the visible slice is cropped + composited after the loop.
        content_img = Image.new('RGBA', (view_w, max(1, content_h)), (0, 0, 0, 0))
        cdraw = ImageDraw.Draw(content_img, 'RGBA')

        sk_font_extra = _load_font('sao', 8)
        sk_font_val = _load_font('sao', 12)
        sk_font_pct = _load_font('sao', 9)
        sk_font_rank = _load_font('sao', 9)
        rank_colors = (self.RANK_GOLD, self.RANK_SILVER, self.RANK_BRONZE)
        rank_fg_colors = (self.RANK_GOLD_FG, self.RANK_SILVER_FG, self.RANK_BRONZE_FG)

        for i, sk in enumerate(skills_sorted):
            ry = i * step                              # content-local top (>=0)
            rank = i + 1
            dmg = _safe_float(sk.get('total'))
            heal = _safe_float(sk.get('heal_total'))
            amount = max(dmg, heal)
            is_heal = heal > dmg
            hits = _safe_int(sk.get('heal_hits' if is_heal else 'hits'))
            raw_damage_hits = _safe_int(sk.get('hits'))
            crit = (_safe_float(sk.get('crit_rate'))
                    if not is_heal and raw_damage_hits > 0 else 0.0)
            ratio = amount / max_val if max_val > 0 else 0    # bar length
            share = amount / sum_val if sum_val > 0 else 0     # share of total
            heat = self._heat_color(ratio)
            heat_rgba = (heat[0], heat[1], heat[2], 255)

            # Card background + hairline border (local coords on content_img)
            self._fill_rounded_rect(
                content_img, (0, ry, cx1, ry + sk_row_h - 1),
                radius=5, fill=self.SKILL_ROW_BG,
            )
            cdraw.rounded_rectangle(
                (0, ry, cx1, ry + sk_row_h - 1),
                radius=5, outline=self.ROW_BORDER, width=1,
            )

            # ── Head line: rank badge + name … value + share% ──
            rank_w = 20
            rank_h = 16
            rbx = 7
            rby = ry + 5
            if rank <= 3:
                self._fill_rounded_rect(
                    content_img, (rbx, rby, rbx + rank_w, rby + rank_h),
                    radius=4, fill=rank_colors[rank - 1],
                )
                rank_fg = rank_fg_colors[rank - 1]
            else:
                rank_fg = self.SKILL_RANK_MUTED
            self._draw_tracked_centered(
                cdraw, str(rank), sk_font_rank, rank_fg,
                rbx + rank_w // 2, rby + 3, 0.5,
            )
            name_x = rbx + rank_w + 8

            # Right side: share% on the far right, value to its left
            pct_text = f'{int(round(share * 100))}%'
            pw = self._tracked_text_width(cdraw, pct_text, sk_font_pct, 0.5)
            self._draw_tracked(cdraw, (cx1 - 8 - pw, ry + 7),
                               pct_text, sk_font_pct, self.TEXT_MUTED, 0.5)
            val_text = _fmt_num(amount)
            vw = self._tracked_text_width(cdraw, val_text, sk_font_val, 0.6)
            val_color = self.VAL_HEAL_GREEN if is_heal else heat_rgba
            val_x = cx1 - 8 - pw - 8 - vw
            self._draw_tracked(cdraw, (val_x, ry + 5),
                               val_text, sk_font_val, val_color, 0.6)

            # Name — CJK-aware font (星辉剑制 / 岚刃 need ZhuZiAYuanJWD, not
            # SAOUI.ttf which renders CJK as tofu boxes). Truncate to the gap
            # left of the value so it never overlaps.
            sk_name_raw = str(sk.get('skill_name') or sk.get('skill_id') or 'Unknown')
            sk_font_name = _pick_font(sk_name_raw, 11)
            name_avail = max(20, val_x - 6 - name_x)
            sk_name = self._truncate(sk_name_raw, sk_font_name, name_avail, cdraw)
            self._draw_tracked(cdraw, (name_x, ry + 6), sk_name,
                               sk_font_name, self.TEXT_MAIN, 0.5)

            # ── Sub line: DMG/HEAL · ×hits · CRIT n% ──
            extras = [('HEAL' if is_heal else 'DMG'), f'×{hits}']
            if not is_heal and crit > 0:
                extras.append(f'CRIT {int(round(crit * 100))}%')
            extra_text = ' · '.join(extras)
            self._draw_tracked(cdraw, (name_x, ry + 23),
                               extra_text, sk_font_extra, self.TEXT_MUTED, 0.5)

            # ── Bottom line: independent thin heat-gradient track ──
            track_x0 = name_x
            track_x1 = cx1 - 8
            track_y = ry + sk_row_h - 10
            track_h = 5
            self._fill_rounded_rect(
                content_img, (track_x0, track_y, track_x1, track_y + track_h),
                radius=2, fill=self.SKILL_TRACK_BG,
            )
            fill_w = int((track_x1 - track_x0) * ratio)
            if fill_w > 1:
                self._fill_rounded_rect(
                    content_img, (track_x0, track_y, track_x0 + fill_w, track_y + track_h),
                    radius=2, fill=(heat[0], heat[1], heat[2], 235),
                )

        # Crop the visible window at the eased scroll offset and composite it
        # into the panel — partial top/bottom rows are clipped here, so the
        # list never paints outside its region.
        top = max(0, min(int(round(scroll)), int(max_scroll)))
        window = content_img.crop((0, top, view_w, top + view_h))
        img.alpha_composite(window, (row_x0, sk_y))

        # Scrollbar affordance for the skill list (only when it overflows).
        # Thumb height = viewport/content ratio; position tracks the eased
        # scroll offset so it glides with the list.
        if max_scroll > 0:
            sb_x = row_x1 + 3
            sb_y0 = sk_y
            sb_y1 = sk_y + view_h
            sb_h = view_h
            self._fill_rounded_rect(
                img, (sb_x, sb_y0, sb_x + 3, sb_y1),
                radius=2, fill=self.SKILL_TRACK_BG,
            )
            thumb_h = max(20, int(sb_h * view_h / max(content_h, 1)))
            thumb_y = sb_y0 + int((sb_h - thumb_h) * (scroll / max_scroll))
            self._fill_rounded_rect(
                img, (sb_x, thumb_y, sb_x + 3, thumb_y + thumb_h),
                radius=2, fill=self.GOLD,
            )

    # --------  Helpers  --------

    @staticmethod
    def _lerp_rgb(a, b, t):
        return (
            int(round(a[0] + (b[0] - a[0]) * t)),
            int(round(a[1] + (b[1] - a[1]) * t)),
            int(round(a[2] + (b[2] - a[2]) * t)),
        )

    def _heat_color(self, ratio: float):
        """Heat gradient by relative output: low → cold blue, mid → gold,
        high → hot red. Byte-for-byte mirror of web/dps.html _heatColor."""
        r = max(0.0, min(1.0, float(ratio or 0.0)))
        if r < 0.5:
            return self._lerp_rgb(self.HEAT_COLD, self.HEAT_MID, r / 0.5)
        return self._lerp_rgb(self.HEAT_MID, self.HEAT_HOT, (r - 0.5) / 0.5)

    def _draw_clip_rect(self, draw: ImageDraw.ImageDraw,
                        x: int, y: int, w: int, h: int,
                        fill=None, outline=None, bevel: int = 8) -> None:
        """Render a CSS clip-path:polygon(Npx 0,100% 0,100% 100%,0 100%,0 Npx)
        shape — a rectangle with a bevelled top-left corner."""
        b = max(2, min(bevel, min(w, h) // 2))
        poly = [
            (x + b, y),
            (x + w - 1, y),
            (x + w - 1, y + h - 1),
            (x, y + h - 1),
            (x, y + b),
            (x + b, y),
        ]
        if fill is not None:
            self._fill_polygon(draw._image, poly, fill)
        if outline is not None:
            draw.line(poly, fill=outline, width=1)

    @staticmethod
    def _fill_rect(img: Image.Image, box, fill) -> None:
        x0, y0, x1, y1 = [int(v) for v in box]
        if x1 < x0:
            x0, x1 = x1, x0
        if y1 < y0:
            y0, y1 = y1, y0
        w = x1 - x0 + 1
        h = y1 - y0 + 1
        if w <= 0 or h <= 0:
            return
        overlay = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(overlay, 'RGBA').rectangle(
            (0, 0, w - 1, h - 1), fill=fill
        )
        img.alpha_composite(overlay, (x0, y0))

    @staticmethod
    def _fill_rounded_rect(img: Image.Image, box, radius: int, fill) -> None:
        x0, y0, x1, y1 = [int(v) for v in box]
        if x1 < x0:
            x0, x1 = x1, x0
        if y1 < y0:
            y0, y1 = y1, y0
        w = x1 - x0 + 1
        h = y1 - y0 + 1
        if w <= 0 or h <= 0:
            return
        overlay = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(overlay, 'RGBA').rounded_rectangle(
            (0, 0, w - 1, h - 1), radius=radius, fill=fill
        )
        img.alpha_composite(overlay, (x0, y0))

    @staticmethod
    def _fill_polygon(img: Image.Image, poly, fill) -> None:
        if not poly:
            return
        xs = [int(p[0]) for p in poly]
        ys = [int(p[1]) for p in poly]
        x0 = min(xs)
        y0 = min(ys)
        x1 = max(xs)
        y1 = max(ys)
        w = x1 - x0 + 1
        h = y1 - y0 + 1
        if w <= 0 or h <= 0:
            return
        local_poly = [(int(px) - x0, int(py) - y0) for px, py in poly]
        overlay = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(overlay, 'RGBA').polygon(local_poly, fill=fill)
        img.alpha_composite(overlay, (x0, y0))

    def _draw_tracked(self, draw: ImageDraw.ImageDraw, xy, text: str,
                      font, fill, spacing: float = 1,
                      shadow_color=None, shadow_blur: int = 0) -> None:
        """Approximate CSS letter-spacing by drawing glyphs one-by-one.
        Optional text-shadow via *shadow_color* + *shadow_blur*."""
        x, y = xy
        if shadow_color and shadow_blur > 0:
            self._draw_text_shadow(draw, text, font, shadow_color,
                                   shadow_blur, x, y, spacing)
        acc = 0.0
        for ch in text:
            draw.text((int(round(x + acc)), y), ch, fill=fill, font=font)
            acc += _glyph_w_dps(draw, ch, font) + spacing

    @staticmethod
    def _draw_text_shadow(draw, text, font, color, blur, x, y, spacing):
        from PIL import Image as _Img, ImageFilter as _IF, ImageDraw as _ID
        acc = 0.0
        widths = []
        for ch in text:
            cw = _glyph_w_dps(draw, ch, font)
            widths.append(cw)
            acc += cw + spacing
        tw = int(round(acc)) + blur * 4
        th = (font.size or 14) + blur * 4
        tmp = _Img.new('RGBA', (tw, th), (0, 0, 0, 0))
        td = _ID.Draw(tmp, 'RGBA')
        ox = blur * 2
        oy = blur * 2
        a = 0.0
        for ch, cw in zip(text, widths):
            td.text((int(round(ox + a)), oy), ch, fill=color, font=font)
            a += cw + spacing
        tmp = tmp.filter(_IF.GaussianBlur(blur))
        draw._image.alpha_composite(tmp, (int(round(x)) - blur * 2, y - blur * 2))

    def _draw_tracked_centered(self, draw: ImageDraw.ImageDraw, text: str,
                               font, fill, cx: int, cy: int,
                               spacing: float = 1) -> None:
        widths = [_glyph_w_dps(draw, ch, font) for ch in text]
        total = sum(widths) + spacing * (len(widths) - 1 if widths else 0)
        x = cx - total / 2
        acc = 0.0
        for ch, cw in zip(text, widths):
            draw.text((int(round(x + acc)), cy), ch, fill=fill, font=font)
            acc += cw + spacing

    def _tracked_text_width(self, draw, text: str, font,
                            spacing: float = 1) -> int:
        """Return the total pixel width of tracked text."""
        if not text:
            return 0
        total = 0.0
        for ch in text:
            total += _glyph_w_dps(draw, ch, font) + spacing
        total -= spacing
        return int(round(total))

    def _make_bar(self, bw: int, bh: int,
                  is_self: bool, is_heal: bool) -> Image.Image:
        if bw <= 0 or bh <= 0:
            return Image.new('RGBA', (1, 1), (0, 0, 0, 0))
        # Cache key: (bw, bh, is_self, is_heal)
        key = (bw, bh, is_self, is_heal)
        cache = getattr(self, '_bar_cache', {})
        if key in cache:
            return cache[key].copy()
        if is_heal:
            ca, cb = self.BAR_HEAL_A, self.BAR_HEAL_B
        elif is_self:
            ca, cb = (255, 215, 132, 104), (222, 190, 80, 14)
        else:
            ca, cb = self.BAR_OTHER_A, self.BAR_OTHER_B

        # Horizontal gradient (vectorised) — matches CSS linear-gradient.
        xs = np.linspace(0, 1, bw)[None, :]
        ys = np.linspace(0, 1, bh)[:, None]
        rr = (ca[0] + (cb[0] - ca[0]) * xs) * (1.0 - 0.12 * ys)
        gg = (ca[1] + (cb[1] - ca[1]) * xs) * (1.0 - 0.12 * ys)
        bb = (ca[2] + (cb[2] - ca[2]) * xs) * (1.0 - 0.12 * ys)
        aa = (ca[3] + (cb[3] - ca[3]) * xs) * np.ones_like(ys)
        arr = np.stack([rr, gg, bb, aa], axis=-1).clip(0, 255).astype(np.uint8)
        bar = Image.fromarray(arr, 'RGBA')

        mask = Image.new('L', (bw, bh), 0)
        ImageDraw.Draw(mask).rounded_rectangle(
            (0, 0, bw - 1, bh - 1),
            radius=min(3, max(1, bh // 2)), fill=255,
        )
        out = Image.new('RGBA', (bw, bh), (0, 0, 0, 0))
        out.paste(bar, (0, 0), mask)

        highlight = Image.new('RGBA', (bw, bh), (0, 0, 0, 0))
        hd = ImageDraw.Draw(highlight, 'RGBA')
        hd.rounded_rectangle(
            (0, 0, bw - 1, max(2, bh // 3)),
            radius=min(3, max(1, bh // 2)),
            fill=(80, 140, 180, 16 if is_self else 10),
        )
        hd.line(
            (1, max(1, bh - 2), max(1, bw - 2), max(1, bh - 2)),
            fill=(28, 24, 16, 20 if is_self else 14), width=1,
        )
        highlight = _gpu_blur(highlight, 1.1)
        out.alpha_composite(highlight)

        leading = Image.new('RGBA', (bw, bh), (0, 0, 0, 0))
        ld = ImageDraw.Draw(leading, 'RGBA')
        ld.rounded_rectangle(
            (0, 0, min(bw - 1, 4), bh - 1),
            radius=min(3, max(1, bh // 2)),
            fill=(255, 240, 200, 26 if is_self else 14),
        )
        out.alpha_composite(leading)
        cache[key] = out
        self._bar_cache = cache
        return out.copy()

    def _truncate(self, text: str, font, max_w: int,
                  draw: ImageDraw.ImageDraw) -> str:
        if max_w <= 0:
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

    def _overlay_panel_flash(self, img: Image.Image,
                             sx: int, sy: int, sw: int, sh: int,
                             now: float) -> None:
        tier = self._panel_fx_tier
        dur, tint, _ = _HIT_FX_TIERS[tier]
        age = now - self._panel_fx_start
        if age >= dur:
            self._panel_fx_tier = ''
            return
        t = age / dur
        env = max(0.0, (1.0 - t) ** 1.6) * (
            0.62 + 0.38 * (1.0 - abs(0.5 - t) * 2))
        a = int(90 * env)
        if a <= 2:
            return
        overlay = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
        od = ImageDraw.Draw(overlay)
        od.rounded_rectangle(
            (0, 0, sw - 1, sh - 1), radius=6,
            fill=(tint[0], tint[1], tint[2], a),
        )
        ring_a = int(200 * env)
        od.rounded_rectangle(
            (0, 0, sw - 1, sh - 1), radius=6,
            outline=(tint[0], tint[1], tint[2], ring_a), width=2,
        )
        mask = Image.new('L', (sw, sh), 0)
        ImageDraw.Draw(mask).rounded_rectangle(
            (0, 0, sw - 1, sh - 1), radius=6, fill=255,
        )
        img.alpha_composite(Image.composite(
            overlay, Image.new('RGBA', (sw, sh), (0, 0, 0, 0)), mask),
            (sx, sy))

    def _resize_hit_rect(self, w: int = 0, h: int = 0) -> Tuple[int, int, int, int]:
        if not w or not h:
            w, h = self._last_rendered_size
        if not w or not h:
            w, h = self._compute_size()
        grip = self.RESIZE_GRIP
        return (w - self.BODY_PAD - grip,
                h - self.BODY_PAD - grip,
                w - self.BODY_PAD + 2,
                h - self.BODY_PAD + 2)

    def _draw_resize_grip(self, draw: ImageDraw.ImageDraw,
                          w: int, h: int) -> None:
        rect = self._resize_hit_rect(w, h)
        self._resize_rect = rect
        x0, y0, x1, y1 = rect
        for i, a in enumerate((92, 150, 220)):
            off = 5 + i * 5
            draw.line(
                (x1 - off - 8, y1 - 3, x1 - 3, y1 - off - 8),
                fill=(222, 166, 32, a), width=1,
            )
        draw.line((x0 + 4, y1 - 2, x1 - 2, y1 - 2),
                  fill=self.CORNER_GOLD, width=1)
        draw.line((x1 - 2, y0 + 4, x1 - 2, y1 - 2),
                  fill=self.CORNER_GOLD, width=1)

    def _control_regions(self) -> dict:
        w, h = self._last_rendered_size
        if not w or not h:
            w, h = self._compute_size()
        sx, sy = self.BODY_PAD, self.BODY_PAD
        sw = w - 2 * self.BODY_PAD
        hh = self._header_height()
        dummy = ImageDraw.Draw(Image.new('RGBA', (1, 1), (0, 0, 0, 0)))

        btn_font = _load_font('sao', 10)
        buttons = self._button_specs()
        sizes, gap = self._button_layout_sizes(
            dummy, btn_font, max(1, sw - 2 * self.HEADER_PAD_X))
        start_y = (sy + self.HEADER_PAD_TOP + self.EYEBROW_H + 4
                   + self.TITLE_H + 6 + self.SUMMARY_H + 8)

        button_regions = {}
        available_w = max(1, sw - 2 * self.HEADER_PAD_X)
        row_specs: List[List[Tuple[Tuple[str, str, bool, str, bool], int]]] = []
        cur: List[Tuple[Tuple[str, str, bool, str, bool], int]] = []
        cur_w = 0
        for spec, bw in zip(buttons, sizes):
            add = bw if not cur else bw + gap
            if cur and cur_w + add > available_w:
                row_specs.append(cur)
                cur = [(spec, bw)]
                cur_w = bw
            else:
                cur.append((spec, bw))
                cur_w += add
        if cur:
            row_specs.append(cur)
        cur_y = start_y
        for row in row_specs:
            row_w = sum(size for _spec, size in row) + gap * (len(row) - 1)
            cur_x = sx + sw - self.HEADER_PAD_X - row_w
            for (name, _label, _active, _kind, _enabled), bw in row:
                button_regions[name] = (cur_x, cur_y, cur_x + bw, cur_y + self.BTN_H)
                cur_x += bw + gap
            cur_y += self.BTN_H + 5

        tabs_y = sy + hh + self.TAB_PAD_TOP
        x0 = sx + self.HEADER_PAD_X
        x1 = sx + sw - self.HEADER_PAD_X
        gap = 8
        tab_w = (x1 - x0 - gap) // 2
        tab_regions = {
            'damage': (x0, tabs_y, x0 + tab_w, tabs_y + self.TAB_H),
            'heal': (x0 + tab_w + gap, tabs_y,
                     x0 + tab_w + gap + tab_w, tabs_y + self.TAB_H),
        }
        return {'buttons': button_regions, 'tabs': tab_regions}

    @staticmethod
    def _point_in_rect(x: int, y: int, rect) -> bool:
        x0, y0, x1, y1 = rect
        return x0 <= x <= x1 and y0 <= y <= y1

    def _notify_no_report(self) -> None:
        self._set_panel_notice('暂无上一场战斗报告 / No last combat report yet.', seconds=4.0)
        if not callable(self._alert_cb):
            return
        try:
            self._alert_cb(
                'DPS METER',
                '暂无上一场战斗报告 / No last combat report yet.',
                display_time=3.0,
            )
        except TypeError:
            try:
                self._alert_cb(
                    'DPS METER',
                    '暂无上一场战斗报告 / No last combat report yet.',
                    3.0,
                )
            except Exception:
                pass
        except Exception:
            pass

    def _notify_export_result(self, ok: bool, message: str) -> None:
        self._set_panel_notice(message, seconds=6.0)
        if not callable(self._alert_cb):
            return
        title = 'DPS EXPORT' if ok else 'DPS METER'
        try:
            self._alert_cb(title, message, display_time=4.0)
        except TypeError:
            try:
                self._alert_cb(title, message, 4.0)
            except Exception:
                pass
        except Exception:
            pass

    def _activate_live(self) -> None:
        snapshot = None
        if callable(self._request_live_snapshot):
            try:
                snapshot = self._request_live_snapshot()
            except Exception:
                snapshot = None
        self.show_live(snapshot)

    def _activate_report(self) -> None:
        self._sync_report_available()
        report = None
        if callable(self._show_last_report_cb):
            try:
                report = self._show_last_report_cb()
            except Exception:
                report = None
        if report is None:
            report = self._last_report
        if not self.show_last_report(report):
            self._notify_no_report()

    def _activate_history(self) -> None:
        report = None
        count = 0
        if callable(self._list_history_cb):
            try:
                items = self._list_history_cb(20)
                if items:
                    count = len(items)
                    report = items[0]
            except Exception:
                report = None
        if report is None:
            report = self._last_report
        if self.show_last_report(report):
            if count > 1:
                self._set_panel_notice(f'Loaded latest history report · {count} reports found', seconds=5.0)
            else:
                self._set_panel_notice('Loaded last combat report', seconds=4.0)
        else:
            self._notify_no_report()

    def _activate_export(self) -> None:
        if not callable(self._export_last_report_cb):
            self._notify_export_result(False, 'DPS export is not initialized.')
            return
        try:
            result = self._export_last_report_cb('json')
        except Exception as exc:
            self._notify_export_result(False, str(exc))
            return
        if isinstance(result, dict):
            ok = bool(result.get('ok'))
            path = str(result.get('path') or '')
            message = path if ok else str(result.get('message') or 'No report to export.')
        else:
            ok = bool(result)
            message = str(result or 'No report to export.')
        self._notify_export_result(ok, message)

    def _activate_reset(self) -> None:
        snapshot = None
        if callable(self._reset_dps_cb):
            try:
                snapshot = self._reset_dps_cb()
            except Exception:
                snapshot = None
        self.show_live(snapshot or _empty_snapshot())

    def _activate_minimize(self) -> None:
        self._minimized = not self._minimized
        if self._minimized:
            self._detail_visible = False
            self._detail_mode = False
            self._resize_active = False
            self._set_panel_notice('DPS minimized · click RESTORE to expand', seconds=4.0)
        else:
            self._set_panel_notice('DPS restored', seconds=3.0)
        if self.settings is not None:
            try:
                self.settings.set('dps_minimized', bool(self._minimized))
                save = getattr(self.settings, 'save', None)
                if callable(save):
                    save()
            except Exception:
                pass
        self._shell_cache = None
        self._last_compose_sig = None
        self._schedule_tick(immediate=True)

    def _set_panel_notice(self, message: str, *, seconds: float = 4.0) -> None:
        self._panel_notice = str(message or '')
        self._panel_notice_until = time.time() + max(0.5, float(seconds or 4.0))
        self._last_compose_sig = None
        self._schedule_tick(immediate=True)

    def _panel_notice_text(self) -> str:
        if self._panel_notice and time.time() <= float(self._panel_notice_until or 0.0):
            return self._panel_notice
        return ''

    def _handle_click(self, x: int, y: int) -> None:
        # Detail-view back button has highest priority
        if (self._detail_visible and self._detail_back_rect
                and self._point_in_rect(x, y, self._detail_back_rect)):
            self.close_detail()
            return
        regions = self._control_regions()
        for name, rect in regions['buttons'].items():
            if self._point_in_rect(x, y, rect):
                if name == 'minimize':
                    self._activate_minimize()
                elif name == 'live':
                    self._activate_live()
                elif name == 'detail':
                    if self._detail_mode:
                        self.exit_detail_mode()
                    else:
                        self.enter_detail_mode()
                elif name == 'report':
                    if self._report_available or self._has_report_data():
                        self._activate_report()
                    else:
                        self._notify_no_report()
                elif name == 'history':
                    self._activate_history()
                elif name == 'export':
                    self._activate_export()
                elif name == 'reset':
                    self._activate_reset()
                return
        if self._minimized and not self._detail_mode:
            return
        if self._detail_visible:
            return
        for name, rect in regions['tabs'].items():
            if self._point_in_rect(x, y, rect):
                self._current_tab = name
                self._schedule_tick(immediate=True)
                return
        # Entity row → open detail
        for uid, rect in self._row_click_regions:
            if self._point_in_rect(x, y, rect):
                self.open_detail(uid)
                return

    # ──────────────────────────────────────────
    #  Dragging
    # ──────────────────────────────────────────

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
        if self._gpu_drag_active:
            self._on_drag_move(self._gpu_event(x, y))

    def _on_gpu_mouse_button(self, button: int, action: int,
                             _mods: int, x: float, y: float) -> None:
        if button != 0:
            return
        ev = self._gpu_event(x, y)
        if action == 1:
            self._gpu_drag_active = True
            self._on_drag_start(ev)
        elif action == 0:
            self._gpu_drag_active = False
            self._on_drag_end(ev)

    def _on_gpu_scroll(self, _xoff: float, yoff: float) -> None:
        if abs(yoff) <= 1e-6:
            return
        self._set_passthrough(False)
        self._on_mouse_wheel(
            self._gpu_event(0, 0, delta=120 if yoff > 0 else -120)
        )

    def _on_drag_start(self, ev) -> None:
        try:
            self._resize_active = False
            self._list_drag_active = False
            self._list_drag_pending_acc = 0.0
            if self._detail_mode and self._point_in_rect(
                    int(ev.x), int(ev.y), self._resize_hit_rect()):
                self._resize_active = True
                self._resize_start_root = (int(ev.x_root), int(ev.y_root))
                self._resize_start_size = (int(self._detail_w), int(self._detail_h))
                self._drag_start_root = self._resize_start_root
                self._drag_moved = False
                return
            if self._minimized and not self._detail_mode:
                self._drag_ox = ev.x_root - self._x
                self._drag_oy = ev.y_root - self._y
                self._drag_start_root = (int(ev.x_root), int(ev.y_root))
                self._drag_moved = False
                return
            # 落点在 entity 列表区域 → 拖动滚动列表 (上下), 不移动窗口
            if self._list_rect is not None and self._point_in_rect(
                    int(ev.x), int(ev.y), self._list_rect):
                # 仅当列表实际可滚动 (内容多于可见行) 才进入 drag-scroll
                if self._max_scroll_offset() > 0:
                    self._list_drag_active = True
                    self._list_drag_start_y = int(ev.y_root)
                    self._list_drag_start_offset = self._current_scroll_offset()
                    self._list_drag_pending_acc = 0.0
                    self._drag_start_root = (int(ev.x_root), int(ev.y_root))
                    self._drag_moved = False
                    return
            self._drag_ox = ev.x_root - self._x
            self._drag_oy = ev.y_root - self._y
            self._drag_start_root = (int(ev.x_root), int(ev.y_root))
            self._drag_moved = False
        except Exception:
            self._drag_ox = 0
            self._drag_oy = 0
            self._drag_start_root = (0, 0)
            self._drag_moved = False
            self._list_drag_active = False

    def _on_drag_move(self, ev) -> None:
        try:
            if self._list_drag_active:
                dy = int(ev.y_root) - self._list_drag_start_y
                # 反向: 向上拖 = 向下滚 (内容向上移)
                rows_delta = -dy / float(self.ROW_H + self.ROW_MARGIN)
                target_offset = int(round(self._list_drag_start_offset + rows_delta))
                max_off = self._max_scroll_offset()
                target_offset = max(0, min(max_off, target_offset))
                cur = self._current_scroll_offset()
                if target_offset != cur:
                    self._set_scroll_offset(target_offset)
                self._drag_moved = True
                return
            if self._resize_active:
                dx = int(ev.x_root) - self._resize_start_root[0]
                dy = int(ev.y_root) - self._resize_start_root[1]
                if not self._drag_moved and \
                   abs(dx) < self.CLICK_DRAG_THRESHOLD and \
                   abs(dy) < self.CLICK_DRAG_THRESHOLD:
                    return
                self._drag_moved = True
                old_size = (int(self._detail_w), int(self._detail_h))
                self._detail_w = max(
                    self.DETAIL_MIN_W,
                    min(self.DETAIL_MAX_W, self._resize_start_size[0] + dx),
                )
                self._detail_h = max(
                    self.DETAIL_MIN_H,
                    min(self.DETAIL_MAX_H, self._resize_start_size[1] + dy),
                )
                if old_size != (int(self._detail_w), int(self._detail_h)):
                    self._shell_cache = None
                    self._last_compose_sig = None
                    if self._gpu_managed and self._gpu_window is not None:
                        try:
                            self._gpu_window.set_geometry(
                                self._x, self._y,
                                int(self._detail_w), int(self._detail_h))
                        except Exception:
                            pass
                    elif self._win is not None and self._win is not self:
                        try:
                            self._win.geometry(
                                f'{int(self._detail_w)}x{int(self._detail_h)}+{self._x}+{self._y}')
                        except Exception:
                            pass
                    self._schedule_tick(immediate=True)
                return
            dx = int(ev.x_root) - self._drag_start_root[0]
            dy = int(ev.y_root) - self._drag_start_root[1]
            if not self._drag_moved and \
               abs(dx) < self.CLICK_DRAG_THRESHOLD and \
               abs(dy) < self.CLICK_DRAG_THRESHOLD:
                return
            self._drag_moved = True
            self._x = int(ev.x_root - self._drag_ox)
            self._y = int(ev.y_root - self._drag_oy)
            if self._gpu_managed and self._gpu_window is not None:
                try:
                    w, h = self._last_rendered_size or (0, 0)
                    if w > 0 and h > 0:
                        self._gpu_window.set_geometry(
                            self._x, self._y, w, h)
                except Exception:
                    pass
            elif self._win is not None and self._win is not self:
                self._win.geometry(f'+{self._x}+{self._y}')
            self._schedule_tick(immediate=True)
        except Exception:
            pass

    def _on_drag_end(self, ev) -> None:
        if self._list_drag_active:
            self._list_drag_active = False
            # 列表拖动完成, 不当作 click; 若没移动则 fall through 当作普通 click
            if self._drag_moved:
                return
            try:
                self._handle_click(int(ev.x), int(ev.y))
            except Exception:
                pass
            return
        if self._resize_active:
            moved = bool(self._drag_moved)
            self._resize_active = False
            if moved and self.settings is not None:
                try:
                    self.settings.set('dps_detail_w', int(self._detail_w))
                    self.settings.set('dps_detail_h', int(self._detail_h))
                    save = getattr(self.settings, 'save', None)
                    if callable(save):
                        save()
                except Exception:
                    pass
            self._schedule_tick(immediate=True)
            return
        if not self._drag_moved:
            try:
                self._handle_click(int(ev.x), int(ev.y))
            except Exception:
                pass
            return
        if self.settings is not None:
            try:
                self.settings.set('dps_ov_x', int(self._x))
                self.settings.set('dps_ov_y', int(self._y))
                save = getattr(self.settings, 'save', None)
                if callable(save):
                    save()
            except Exception:
                pass

    # ──────────────────────────────────────────
    #  Mouse wheel scroll
    # ──────────────────────────────────────────

    def _on_mouse_wheel(self, ev) -> None:
        """Handle mouse wheel.

        In detail view the wheel scrolls the skill list (parity with
        web/dps.html .skill-frame overflow:auto); otherwise it scrolls the
        entity list. ev.delta > 0 = scroll up, < 0 = scroll down on Windows.
        """
        delta = getattr(ev, 'delta', 0)
        if delta == 0:
            return
        direction = -1 if delta > 0 else 1
        if self._detail_visible:
            self._scroll_skills(direction)
        else:
            self._scroll(direction)

    def _scroll(self, direction: int) -> None:
        """Scroll the entity list by one row in the given direction (+1 down, -1 up)."""
        max_offset = self._max_scroll_offset()
        old = self._current_scroll_offset()
        new = max(0, min(max_offset, old + direction))
        if new != old:
            self._set_scroll_offset(new)

    def _scroll_skills(self, direction: int) -> None:
        """Nudge the detail-view skill-scroll target by one wheel notch
        (+1 down, -1 up). The displayed offset eases toward this target in
        _advance_animations, so it glides instead of snapping. Bounds come
        from the last _draw_detail_view pass (_skill_max_scroll), and the
        draw pass re-clamps in case the geometry changed since.
        """
        max_scroll = float(getattr(self, '_skill_max_scroll', 0.0) or 0.0)
        old = float(getattr(self, '_skill_scroll_target', 0.0) or 0.0)
        new = max(0.0, min(max_scroll, old + direction * self.SKILL_WHEEL_STEP))
        if new != old:
            self._skill_scroll_target = new
            self._schedule_tick(immediate=True)

    def _scroll_offset_attr(self) -> str:
        return '_scroll_offset_report' if self._view_mode == 'report' else '_scroll_offset'

    def _max_scroll_offset(self) -> int:
        total = self._view_row_total()
        return max(0, total - self.MAX_ROWS)

    def _current_scroll_offset(self) -> int:
        return int(getattr(self, self._scroll_offset_attr(), 0) or 0)

    def _set_scroll_offset(self, value: int) -> None:
        max_offset = self._max_scroll_offset()
        v = max(0, min(max_offset, int(value)))
        setattr(self, self._scroll_offset_attr(), v)
        self._schedule_tick(immediate=True)


# ────────────────────────────────────────────────────────────
# Theme dictionaries & registration
# ────────────────────────────────────────────────────────────

DPS_THEME_LIGHT = {
    'PANEL_BG_A':      (250, 252, 253, 255),
    'PANEL_BG_B':      (220, 224, 229, 255),
    'PANEL_EDGE':      (128, 190, 220, 255),
    'PANEL_LINE':      (255, 255, 255, 255),
    'INNER_HIGHLIGHT': (255, 255, 255, 255),
    'HAIRLINE_LIGHT':  (250, 250, 250, 255),
    'HAIRLINE_MID':    (228, 228, 228, 255),
    'HAIRLINE_DARK':   (140, 138, 138, 255),
    'SCAN_LINE':       (104, 228, 255, 24),
    'TEXT_MAIN':       (100, 99, 100, 255),
    'TEXT_MUTED':      (140, 135, 138, 255),
    'GOLD':            (222, 166, 32, 255),
    'GOLD_SOFT':       (222, 166, 32, 56),
    'CYAN':            (104, 228, 255, 255),
    'DIVIDER':         (178, 180, 182, 255),
    'LIST_BG':         (244, 248, 252, 196),
    'LIST_BORDER':     (120, 190, 225, 230),
    'ROW_BG':          (248, 247, 244, 214),
    'ROW_BORDER':      (156, 178, 194, 235),
    'ROW_SELF_BAR':    (222, 166, 32, 255),
    'BTN_BG':          (255, 255, 255, 112),
    'BTN_BORDER':      (178, 180, 182, 255),
    'BTN_LIVE_ACTIVE': (104, 228, 255, 31),
    'BTN_LIVE_BORDER': (104, 228, 255, 255),
    'BTN_LIVE_COLOR':  (68, 144, 162, 255),
    'BTN_DANGER':      (239, 104, 78, 255),
    'BAR_OTHER_A':     (222, 166, 32, 51),
    'BAR_OTHER_B':     (222, 166, 32, 8),
    'BAR_HEAL_A':      (154, 211, 52, 61),
    'BAR_HEAL_B':      (154, 211, 52, 8),
    'BADGE_LIVE':      (82, 140, 48, 255),
    'BADGE_REPORT':    (222, 166, 32, 255),
    # v2.3.x: previously hardcoded dark colors that broke light-theme
    'FOOTER_BG':       (238, 242, 247, 205),
    'FOOTER_CYAN_TINT':(104, 228, 255, 18),
    'FOOTER_SHADOW':   (40, 55, 70, 0),
    'TAB_ACTIVE_FILL': (222, 190, 80, 35),
    'TAB_ACTIVE_BORDER':(222, 190, 80, 220),
    'TAB_INACTIVE_FILL':(40, 55, 75, 120),
    'TAB_INACTIVE_BORDER':(50, 100, 130, 100),
    'BTN_ACTIVE_FILL': (222, 190, 80, 40),
    'BTN_ACTIVE_BORDER':(222, 190, 80, 220),
    'BTN_DISABLED_FILL':(30, 35, 45, 120),
    'BTN_DISABLED_BORDER':(50, 60, 70, 100),
    'BTN_DISABLED_FG': (80, 85, 95, 180),
    'HEADER_BADGE_FILL':(230, 240, 248, 220),
    'HEADER_BADGE_BORDER':(60, 140, 180, 160),
    'LIST_CYAN_TINT':  (104, 228, 255, 22),
    'LIST_SHADOW':     (45, 55, 70, 0),
    'ROW_SHEEN_CYAN':  (104, 228, 255, 20),
    'ROW_LOWER_SHADOW':(60, 45, 38, 0),
    'ROW_SELF_OUTLINE':(222, 190, 80, 220),
    'ROW_SELF_TINT':   (222, 190, 80, 28),
    'DETAIL_CARD_BG':  (35, 45, 60, 160),
    'SKILL_ROW_BG':    (30, 40, 55, 140),
    'STAT_HEAL_GREEN': (92, 150, 44, 255),
    'SKILL_BAR_HEAL':  (154, 211, 52, 70),
    'SKILL_BAR_DAMAGE':(222, 190, 80, 60),
    'VAL_HEAL_GREEN':  (92, 150, 44, 255),
    'SHELL_AMBIENT_SHADOW': (22, 24, 18, 0),
    'SHELL_CONTACT_SHADOW': (31, 34, 16, 0),
    'SHELL_SHEEN_CYAN': (104, 228, 255, 32),
    'SHELL_SHEEN_SHADOW': (42, 52, 64, 34),
    'CORNER_CYAN_ACCENT': (104, 228, 255, 120),
    'CORNER_GOLD_ACCENT': (222, 190, 80, 120),
    'CORNER_GOLD': (222, 190, 80, 255),
}

DPS_THEME_DARK = {
    'PANEL_BG_A':      (20, 24, 32, 245),
    'PANEL_BG_B':      (16, 18, 26, 250),
    'PANEL_EDGE':      (60, 180, 220, 200),
    'PANEL_LINE':      (104, 228, 255, 120),
    'INNER_HIGHLIGHT': (80, 200, 240, 80),
    'HAIRLINE_LIGHT':  (40, 60, 80, 255),
    'HAIRLINE_MID':    (50, 70, 90, 255),
    'HAIRLINE_DARK':   (80, 100, 120, 255),
    'SCAN_LINE':       (104, 228, 255, 10),
    'TEXT_MAIN':       (220, 225, 230, 255),
    'TEXT_MUTED':      (120, 135, 150, 255),
    'GOLD':            (222, 190, 80, 255),
    'GOLD_SOFT':       (222, 190, 80, 40),
    'CYAN':            (104, 228, 255, 255),
    'DIVIDER':         (60, 180, 220, 120),
    'LIST_BG':         (10, 14, 22, 180),
    'LIST_BORDER':     (50, 140, 180, 140),
    'ROW_BG':          (30, 38, 52, 160),
    'ROW_BORDER':      (60, 120, 160, 100),
    'ROW_SELF_BAR':    (222, 190, 80, 255),
    'BTN_BG':          (40, 55, 75, 180),
    'BTN_BORDER':      (70, 150, 190, 160),
    'BTN_LIVE_ACTIVE': (104, 228, 255, 40),
    'BTN_LIVE_BORDER': (104, 228, 255, 255),
    'BTN_LIVE_COLOR':  (180, 235, 255, 255),
    'BTN_DANGER':      (239, 104, 78, 255),
    'BAR_OTHER_A':     (222, 190, 80, 70),
    'BAR_OTHER_B':     (222, 190, 80, 15),
    'BAR_HEAL_A':      (80, 200, 120, 70),
    'BAR_HEAL_B':      (80, 200, 120, 15),
    'BADGE_LIVE':      (104, 228, 255, 255),
    'BADGE_REPORT':    (222, 190, 80, 255),
    # v2.3.x: dark-mode counterparts
    'FOOTER_BG':       (8, 10, 16, 160),
    'FOOTER_CYAN_TINT':(104, 228, 255, 10),
    'FOOTER_SHADOW':   (0, 0, 0, 0),
    'TAB_ACTIVE_FILL': (222, 190, 80, 50),
    'TAB_ACTIVE_BORDER':(222, 190, 80, 240),
    'TAB_INACTIVE_FILL':(25, 35, 50, 140),
    'TAB_INACTIVE_BORDER':(40, 80, 110, 120),
    'BTN_ACTIVE_FILL': (222, 190, 80, 50),
    'BTN_ACTIVE_BORDER':(222, 190, 80, 240),
    'BTN_DISABLED_FILL':(20, 25, 35, 140),
    'BTN_DISABLED_BORDER':(40, 50, 60, 120),
    'BTN_DISABLED_FG': (70, 75, 85, 200),
    'HEADER_BADGE_FILL':(20, 30, 45, 200),
    'HEADER_BADGE_BORDER':(50, 120, 160, 180),
    'LIST_CYAN_TINT':  (104, 228, 255, 10),
    'LIST_SHADOW':     (0, 0, 0, 0),
    'ROW_SHEEN_CYAN':  (104, 228, 255, 12),
    'ROW_LOWER_SHADOW':(0, 0, 0, 0),
    'ROW_SELF_OUTLINE':(222, 190, 80, 240),
    'ROW_SELF_TINT':   (222, 190, 80, 18),
    'DETAIL_CARD_BG':  (20, 28, 40, 180),
    'SKILL_ROW_BG':    (18, 24, 36, 160),
    'STAT_HEAL_GREEN': (110, 200, 80, 255),
    'SKILL_BAR_HEAL':  (80, 200, 120, 80),
    'SKILL_BAR_DAMAGE':(222, 190, 80, 70),
    'VAL_HEAL_GREEN':  (110, 200, 80, 255),
    'SHELL_AMBIENT_SHADOW': (12, 14, 10, 0),
    'SHELL_CONTACT_SHADOW': (18, 20, 14, 0),
    'SHELL_SHEEN_CYAN': (104, 228, 255, 24),
    'SHELL_SHEEN_SHADOW': (0, 0, 0, 40),
    'CORNER_CYAN_ACCENT': (104, 228, 255, 160),
    'CORNER_GOLD_ACCENT': (222, 190, 80, 160),
    'CORNER_GOLD': (222, 190, 80, 255),
}

from sao_theme import register_panel_theme
register_panel_theme('dps', 'light', DPS_THEME_LIGHT)
register_panel_theme('dps', 'dark', DPS_THEME_DARK)
