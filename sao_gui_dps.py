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
from typing import Any, Dict, List, Optional, Tuple
from decimal import Decimal, ROUND_HALF_UP

import numpy as np
import tkinter as tk
from PIL import Image, ImageDraw, ImageFont, ImageFilter
from gpu_renderer import gaussian_blur_rgba as _gpu_blur
from overlay_scheduler import get_scheduler as _get_scheduler
from overlay_render_worker import AsyncFrameWorker, submit_ulw_commit
from render_capture_sync import wait_until_capture_idle
from config import FONTS_DIR

# v2.3.x: optional GPU presenter. Env-gated via SAO_GPU_DPS
# (defaults to SAO_GPU_OVERLAY). NOTE: GLFW path is click_through, so
# drag-to-move and tab clicks on this overlay are disabled in GPU
# mode — use SAO_GPU_DPS=0 for the legacy interactive ULW path.
try:
    import gpu_overlay_window as _gow  # type: ignore[import-untyped]
except Exception:
    _gow = None  # type: ignore[assignment]


def _gpu_dps_enabled() -> bool:
    if _gow is None or not _gow.glfw_supported():
        return False
    try:
        import os as _os
        flag = _os.environ.get('SAO_GPU_DPS', '0')
        # Default-OFF: GPU path is click-through, but the legacy ULW
        # path supports drag-to-move + right-click context menu, which
        # users expect on this panel. Set SAO_GPU_DPS=1 to opt in.
        return str(flag).strip() not in ('', '0', 'false', 'False')
    except Exception:
        return False

from perf_probe import probe as _probe

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

    # Build premultiplied BGRA buffer via numpy (vectorised).
    rgba = np.asarray(img, dtype=np.uint8)           # shape (h, w, 4) RGBA
    a = rgba[:, :, 3:4].astype(np.uint16)
    rgb = (rgba[:, :, :3].astype(np.uint16) * a + 127) // 255
    bgra = np.empty_like(rgba)
    bgra[:, :, 0] = rgb[:, :, 2]
    bgra[:, :, 1] = rgb[:, :, 1]
    bgra[:, :, 2] = rgb[:, :, 0]
    bgra[:, :, 3] = rgba[:, :, 3]
    raw = bgra.tobytes()
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
        from sao_sound import load_sao_fonts as _load_sao_fonts
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


# ═══════════════════════════════════════════════
#  Formatting (parity with dps.html _fmtNum / _fmtTime)
# ═══════════════════════════════════════════════

def _fmt_num(v: float) -> str:
    v = float(v or 0)
    if v >= 1_000_000:
        digits = 0 if v >= 10_000_000 else 1
        return f"{_to_fixed_half_up(v / 1_000_000, digits)}M"
    if v >= 1_000:
        digits = 0 if v >= 100_000 else 1
        return f"{_to_fixed_half_up(v / 1_000, digits)}K"
    return f"{_round_half_up_int(v):,}"


def _fmt_time(s: float) -> str:
    s = max(0, int(s or 0))
    return f"{s // 60:02d}:{s % 60:02d}"


def _fmt_fp(fp: float) -> str:
    fp = float(fp or 0)
    if fp <= 0:
        return ''
    if fp >= 1_000_000:
        return f"{_to_fixed_half_up(fp / 1_000_000, 2)}M"
    if fp >= 1_000:
        digits = 0 if fp >= 100_000 else 1
        return f"{_to_fixed_half_up(fp / 1_000, digits)}K"
    return f"{_round_half_up_int(fp):,}"


def _to_fixed_half_up(value: float, digits: int) -> str:
    quant = Decimal('1').scaleb(-digits)
    dec = Decimal(str(value)).quantize(quant, rounding=ROUND_HALF_UP)
    return format(dec, f'.{digits}f')


def _round_half_up_int(value: float) -> int:
    return int(Decimal(str(value)).quantize(Decimal('1'), rounding=ROUND_HALF_UP))


# ═══════════════════════════════════════════════
#  Easing
# ═══════════════════════════════════════════════

def _ease_out_cubic(t: float) -> float:
    t = max(0.0, min(1.0, t))
    return 1.0 - (1.0 - t) ** 3


def _lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * max(0.0, min(1.0, t))


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

    @_probe.decorate('ui.dps.update_targets')
    def update_targets(self, data: dict) -> None:
        self.name = str(data.get('name') or self.name)
        self.profession = str(data.get('profession') or '')
        try:
            self.fight_point = int(data.get('fight_point') or 0)
        except Exception:
            self.fight_point = 0
        self.is_self = bool(data.get('is_self'))
        self.damage_total = int(data.get('damage_total') or 0)
        self.heal_total = int(data.get('heal_total') or 0)
        self.damage_pct = float(data.get('damage_pct') or 0)
        self.target_damage = float(data.get('damage_total') or 0)
        self.target_dps = float(data.get('dps') or 0)
        self.target_heal = float(data.get('heal_total') or 0)
        self.target_hps = float(data.get('hps') or 0)
        self.target_bar_pct = float(data.get('bar_pct') or 0)


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
        │   ├─ dps-header   (eyebrow · title+badge · summary · 3 buttons)
        │   ├─ dps-tabs     (Damage | Healing)
        │   ├─ list-frame   (entity rows)
        │   └─ dps-footer   (ELAPSED … | TOTAL …)
    """

    # Outer panel (ULW window) size
    WIDTH = 340
    DEFAULT_HEIGHT = 420
    MAX_HEIGHT = 700

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
    BTN_H = 26
    BTN_GAP = 6
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
    PANEL_BG_A = (20, 24, 32, 245)     # deep dark blue-black shell bg
    PANEL_BG_B = (16, 18, 26, 250)     # slightly darker bottom
    PANEL_EDGE = (60, 180, 220, 200)   # cyan-tinted border
    PANEL_LINE = (104, 228, 255, 120)  # cyan highlight line
    INNER_HIGHLIGHT = (80, 200, 240, 80)  # subtle inner glow
    HAIRLINE_LIGHT = (40, 60, 80, 255)
    HAIRLINE_MID = (50, 70, 90, 255)
    HAIRLINE_DARK = (80, 100, 120, 255)
    SCAN_LINE = (104, 228, 255, 10)    # faint cyan scanlines
    TEXT_MAIN = (220, 225, 230, 255)   # bright text on dark bg
    TEXT_MUTED = (120, 135, 150, 255)  # muted blue-gray text
    GOLD = (222, 190, 80, 255)         # warm gold for emphasis
    GOLD_SOFT = (222, 190, 80, 40)
    CYAN = (104, 228, 255, 255)        # structural cyan lines
    DIVIDER = (60, 180, 220, 120)      # cyan-tinted divider
    LIST_BG = (10, 14, 22, 180)        # dark list background
    LIST_BORDER = (50, 140, 180, 140)  # cyan-tinted list border
    ROW_BG = (30, 38, 52, 160)         # darker row background
    ROW_BORDER = (60, 120, 160, 100)   # subtle row border
    ROW_SELF_BAR = (222, 190, 80, 255) # gold self-indicator
    BTN_BG = (40, 55, 75, 180)         # dark button bg
    BTN_BORDER = (70, 150, 190, 160)   # cyan button border
    BTN_LIVE_ACTIVE = (104, 228, 255, 40)     # active live button tint
    BTN_LIVE_BORDER = (104, 228, 255, 255)    # bright cyan border
    BTN_LIVE_COLOR = (180, 235, 255, 255)     # bright text for active
    BTN_DANGER = (239, 104, 78, 255)
    BAR_OTHER_A = (222, 190, 80, 70)   # gold damage bar gradient
    BAR_OTHER_B = (222, 190, 80, 15)
    BAR_HEAL_A = (80, 200, 120, 70)
    BAR_HEAL_B = (80, 200, 120, 15)
    BADGE_LIVE = (104, 228, 255, 255)  # cyan badge
    BADGE_REPORT = (222, 190, 80, 255) # gold badge
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
                 alert=None):
        self.root = root
        self.settings = settings
        self._request_live_snapshot = request_live_snapshot
        self._show_last_report_cb = show_last_report
        self._reset_dps_cb = reset_dps
        self._has_last_report_cb = has_last_report
        self._request_entity_detail_cb = request_entity_detail
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
        self._self_uid = 0
        self._view_mode = 'live'
        self._current_tab = 'damage'
        self._report_available = False
        # Detail view state (parity with web/dps.html _openDetail/_closeDetail)
        self._detail_visible = False
        self._detail_uid = 0
        self._live_detail_cache: Dict[int, dict] = {}
        # Hit-test regions captured during the most recent compose pass.
        self._row_click_regions: List[Tuple[int, Tuple[int, int, int, int]]] = []
        self._detail_back_rect: Optional[Tuple[int, int, int, int]] = None

        sw = _user32.GetSystemMetrics(0)
        sh = _user32.GetSystemMetrics(1)
        # Mirror the webview DPS window geometry (sao_webview.py): dimensions
        # are derived from screen size so the Tk ULW overlay lines up pixel-
        # for-pixel with the webview panel.
        dyn_w = max(320, int(min(sw, 1920) * 0.19))
        dyn_h = max(self.DEFAULT_HEIGHT, int(min(sh, 1080) * 0.48))
        self.WIDTH = dyn_w
        self.DEFAULT_HEIGHT = max(self.DEFAULT_HEIGHT, dyn_h)
        self._x = max(0, sw - self.WIDTH - max(16, int(sw * 0.012)))
        self._y = max(0, int(sh * 0.18))
        if settings is not None:
            try:
                self._x = int(settings.get('dps_ov_x', self._x))
                self._y = int(settings.get('dps_ov_y', self._y))
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

        self._drag_ox = 0
        self._drag_oy = 0
        self._drag_start_root = (0, 0)
        self._drag_moved = False

        self._tick_after_id: Optional[str] = None
        self._last_rendered_size: tuple = (0, 0)
        self._registered: bool = False

        # Scroll offset for entity list (mouse wheel support)
        self._scroll_offset: int = 0
        self._scroll_offset_report: int = 0

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

    # ──────────────────────────────────────────
    #  Lifecycle
    # ──────────────────────────────────────────

    def show(self) -> None:
        if self._win is not None:
            return
        # v2.3.x: GPU presenter path (env-gated).
        if _gpu_dps_enabled():
            try:
                pump = _gow.get_glfw_pump(self.root)
                presenter = _gow.BgraPresenter()
                # Initial 1×1 placeholder; tick will resize to compose dims.
                gpu_win = _gow.GpuOverlayWindow(
                    pump,
                    w=1, h=1,
                    x=int(self._x), y=int(self._y),
                    render_fn=presenter.render,
                    click_through=True,
                    title='sao_dps_gpu',
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
        self._win = tk.Toplevel(self.root)
        self._win.overrideredirect(True)
        self._win.attributes('-topmost', True)
        self._win.geometry(f'1x1+{self._x}+{self._y}')
        self._win.update_idletasks()

        try:
            self._hwnd = _user32.GetParent(self._win.winfo_id()) or \
                self._win.winfo_id()
        except Exception:
            self._hwnd = self._win.winfo_id()

        ex = _user32.GetWindowLongW(ctypes.c_void_p(self._hwnd), GWL_EXSTYLE)
        _user32.SetWindowLongW(
            ctypes.c_void_p(self._hwnd),
            GWL_EXSTYLE,
            ex | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        )
        try:
            _user32.SetWindowDisplayAffinity(ctypes.c_void_p(self._hwnd), 0x00000011)
        except Exception:
            pass
        # Disable DWM non-client rendering (incl. system drop shadow) so the
        # shadow does not linger while the ULW bitmap fades to transparent.
        try:
            _ncr_disabled = ctypes.c_int(1)   # DWMNCRP_DISABLED
            ctypes.windll.dwmapi.DwmSetWindowAttribute(
                ctypes.c_void_p(self._hwnd), 2,
                ctypes.byref(_ncr_disabled), ctypes.sizeof(_ncr_disabled))
        except Exception:
            pass

        # Dragging
        self._win.bind('<Button-1>', self._on_drag_start)
        self._win.bind('<B1-Motion>', self._on_drag_move)
        self._win.bind('<ButtonRelease-1>', self._on_drag_end)
        self._win.bind('<MouseWheel>', self._on_mouse_wheel)

        self._visible = True
        self._faded_out = False
        self._hide_after_fade = False
        self._fade_from = 0.0
        self._fade_alpha = 0.0
        self._fade_target = 1.0
        self._fade_start = time.time()
        self._fade_duration = self.FADE_IN
        self._schedule_tick(immediate=True)

    def hide(self) -> None:
        self._hide_after_fade = False
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
        self._visible = False
        self._last_compose_sig = None

    # Idle fade target: fully fade the panel out, then destroy the ULW window.
    FADE_IDLE_ALPHA = 0.0

    def fade_out(self) -> None:
        if self._faded_out:
            return
        self._faded_out = True
        self._hide_after_fade = True
        self._fade_from = self._fade_alpha
        self._fade_target = self.FADE_IDLE_ALPHA
        self._fade_start = time.time()
        self._fade_duration = self.FADE_OUT
        self._schedule_tick(immediate=True)

    def fade_in(self) -> None:
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

    def is_report_mode(self) -> bool:
        return self._view_mode == 'report'

    def show_live(self, snapshot: Optional[dict] = None) -> None:
        self._sync_report_available()
        self._view_mode = 'live'
        self._detail_visible = False
        self._detail_uid = 0
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
        if not self._visible or not self._hwnd:
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
        # In live mode, request a fresh skill breakdown from the controller.
        if self._view_mode == 'live' and callable(self._request_entity_detail_cb):
            try:
                detail = self._request_entity_detail_cb(uid)
                if isinstance(detail, dict):
                    self._live_detail_cache[uid] = detail
            except Exception:
                pass
        self._schedule_tick(immediate=True)

    def close_detail(self) -> None:
        if not self._detail_visible:
            return
        self._detail_visible = False
        self._detail_uid = 0
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
            or int(snapshot.get('total_damage') or 0) > 0
            or int(snapshot.get('total_heal') or 0) > 0
        )
        if (not self._visible or not self._hwnd) and not has_content and not force_show:
            self._last_snapshot = snapshot
            self._target_total_damage = float(snapshot.get('total_damage') or 0)
            self._target_total_dps = float(snapshot.get('total_dps') or 0)
            self._target_total_heal = float(snapshot.get('total_heal') or 0)
            self._target_total_hps = float(snapshot.get('total_hps') or 0)
            self._target_elapsed = float(snapshot.get('elapsed_s') or 0)
            self._encounter_active = bool(snapshot.get('encounter_active'))
            return
        if (not self._visible or not self._hwnd) and force_show:
            self.show()
        elif not self._visible or not self._hwnd:
            self.show()
        self._last_snapshot = snapshot

        self._target_total_damage = float(snapshot.get('total_damage') or 0)
        self._target_total_dps = float(snapshot.get('total_dps') or 0)
        self._target_total_heal = float(snapshot.get('total_heal') or 0)
        self._target_total_hps = float(snapshot.get('total_hps') or 0)
        self._target_elapsed = float(snapshot.get('elapsed_s') or 0)
        self._encounter_active = bool(snapshot.get('encounter_active'))

        seen_uids: List[int] = []
        # Apply scroll offset to the entity list
        scroll = max(0, min(self._scroll_offset, max(0, len(entities) - self.MAX_ROWS)))
        self._scroll_offset = scroll
        for idx, ent in enumerate(entities[scroll: scroll + self.MAX_ROWS]):
            try:
                uid = int(ent.get('uid') or 0)
            except Exception:
                uid = 0
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
        if self._hide_after_fade:
            return True
        if abs(self._fade_alpha - self._fade_target) > 1e-3:
            return True
        if self._panel_fx_tier:
            return True
        for row in self._rows.values():
            if abs(row.disp_dps - row.target_dps) > 0.5 or \
               abs(row.disp_damage - row.target_damage) > 0.5 or \
               abs(row.disp_hps - row.target_hps) > 0.5 or \
               abs(row.disp_heal - row.target_heal) > 0.5 or \
               abs(row.disp_bar_pct - row.target_bar_pct) > 1e-3 or \
               abs(row.disp_y - row.target_y) > 5e-4 or \
               bool(row.fx_tier):
                return True
        if abs(self._disp_total_damage - self._target_total_damage) > 0.5:
            return True
        if abs(self._disp_total_dps - self._target_total_dps) > 0.5:
            return True
        if abs(self._disp_total_heal - self._target_total_heal) > 0.5:
            return True
        if abs(self._disp_total_hps - self._target_total_hps) > 0.5:
            return True
        return False

    def _compose_signature(self, now: float) -> Optional[tuple]:
        """v2.3.x: coarse fingerprint of frame inputs. Returns None
        when an animation is in flight (forces every-tick submit so
        tweens look smooth). Otherwise returns a tuple covering all
        observable state — if it equals the previous signature, the
        compose+ULW pass can be skipped entirely."""
        if self._is_animating():
            return None
        try:
            row_sig = tuple(
                (uid,
                 int(row.target_damage),
                 int(row.target_dps),
                 int(row.target_heal),
                 int(row.target_hps),
                 round(float(row.target_bar_pct), 4),
                 int(row.target_y * 1000))
                for uid, row in self._rows.items()
            )
            return (
                int(self._disp_elapsed),  # 1 Hz tick
                int(self._target_total_damage),
                int(self._target_total_dps),
                int(self._target_total_heal),
                int(self._target_total_hps),
                self._view_mode,
                self._current_tab,
                bool(self._detail_visible),
                int(self._detail_uid),
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
        if self._hwnd or self._gpu_managed:
            # v2.2.23: don't let vision capture starve our commits.
            fb = self._render_worker.take_result(allow_during_capture=True)
            if fb is not None:
                sz = (fb.width, fb.height)
                if self._gpu_managed and self._gpu_presenter is not None \
                        and self._gpu_window is not None:
                    try:
                        if sz != self._last_rendered_size \
                                or (fb.x, fb.y) != (self._x, self._y):
                            self._gpu_window.set_geometry(
                                self._x, self._y, fb.width, fb.height)
                            self._last_rendered_size = sz
                        self._gpu_presenter.set_frame(
                            fb.bgra_bytes, fb.width, fb.height)
                        self._gpu_window.request_redraw()
                    except Exception as e:
                        print(f'[DPS-OV] gpu present error: {e}')
                elif self._hwnd:
                    # Resize Tk window if panel dimensions changed.
                    if self._win is not None and self._win is not self \
                            and sz != self._last_rendered_size:
                        try:
                            self._win.geometry(
                                f'{fb.width}x{fb.height}+{self._x}+{self._y}')
                        except Exception:
                            pass
                        self._last_rendered_size = sz
                    try:
                        submit_ulw_commit(self._hwnd, fb, allow_during_capture=True)
                    except Exception as e:
                        print(f'[DPS-OV] ulw error: {e}')

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

        return animating

    def _decay_toward(self, cur: float, tgt: float, tween: float) -> float:
        """Exponential approach — covers ~95% of remaining distance in
        `tween` seconds at TICK_MS frame rate. Cheap ease-out stand-in."""
        if abs(cur - tgt) < 0.5:
            return tgt
        k = 1.0 - pow(0.05, self.TICK_MS / 1000.0 / max(0.05, tween))
        return cur + (tgt - cur) * k

    def _step_toward_totals(self) -> bool:
        prev = (self._disp_total_damage, self._disp_total_dps,
                self._disp_total_heal, self._disp_total_hps,
                self._disp_elapsed)
        # Smoothly tween totals so the header counters slide instead of
        # snapping every snapshot. Elapsed is a clock and stays linear.
        self._disp_total_damage = self._decay_toward(
            self._disp_total_damage, self._target_total_damage, self.NUM_TWEEN)
        self._disp_total_dps = self._decay_toward(
            self._disp_total_dps, self._target_total_dps, self.NUM_TWEEN)
        self._disp_total_heal = self._decay_toward(
            self._disp_total_heal, self._target_total_heal, self.NUM_TWEEN)
        self._disp_total_hps = self._decay_toward(
            self._disp_total_hps, self._target_total_hps, self.NUM_TWEEN)
        self._disp_elapsed = self._target_elapsed
        return prev != (self._disp_total_damage, self._disp_total_dps,
                        self._disp_total_heal, self._disp_total_hps,
                        self._disp_elapsed)

    def _step_row(self, row: _RowState, now: float) -> bool:
        before = (row.disp_damage, row.disp_dps, row.disp_heal, row.disp_hps,
                  row.disp_bar_pct, row.disp_y)
        # Tween numeric values, bar fill, and slot Y so reorders/value
        # changes visibly slide instead of snapping each frame.
        row.disp_damage = self._decay_toward(
            row.disp_damage, row.target_damage, self.NUM_TWEEN)
        row.disp_dps = self._decay_toward(
            row.disp_dps, row.target_dps, self.NUM_TWEEN)
        row.disp_heal = self._decay_toward(
            row.disp_heal, row.target_heal, self.NUM_TWEEN)
        row.disp_hps = self._decay_toward(
            row.disp_hps, row.target_hps, self.NUM_TWEEN)
        # Bar percent uses a tighter epsilon than the 0.5-absolute one
        # baked into _decay_toward; clamp manually.
        if abs(row.disp_bar_pct - row.target_bar_pct) < 5e-4:
            row.disp_bar_pct = row.target_bar_pct
        else:
            k = 1.0 - pow(0.05, self.TICK_MS / 1000.0
                          / max(0.05, self.BAR_TWEEN))
            row.disp_bar_pct = (row.disp_bar_pct
                                + (row.target_bar_pct - row.disp_bar_pct) * k)
        if abs(row.disp_y - row.target_y) < 5e-4:
            row.disp_y = row.target_y
        else:
            k = 1.0 - pow(0.05, self.TICK_MS / 1000.0
                          / max(0.05, self.ROW_TWEEN))
            row.disp_y = row.disp_y + (row.target_y - row.disp_y) * k
        if row.fx_tier:
            dur = _HIT_FX_TIERS.get(row.fx_tier, (0,))[0]
            if now - row.fx_start > dur:
                row.fx_tier = ''
        after = (row.disp_damage, row.disp_dps, row.disp_heal, row.disp_hps,
                 row.disp_bar_pct, row.disp_y)
        return bool(row.fx_tier) or (before != after)

    # ──────────────────────────────────────────
    #  Rendering — pixel-for-pixel port of web/dps.html
    # ──────────────────────────────────────────

    def _header_height(self) -> int:
        """Total .dps-header height = pad-top + eyebrow + 4 + title + 6 +
        summary + pad-bot + button row (below title cluster, wrapped)."""
        # Top cluster (eyebrow/title/summary) + button row + gap between
        return (self.HEADER_PAD_TOP
                + self.EYEBROW_H + 4
                + self.TITLE_H + 6
                + self.SUMMARY_H
                + 8
                + self.BTN_H
                + self.HEADER_PAD_BOT)

    def _tabs_height(self) -> int:
        return self.TAB_PAD_TOP + self.TAB_H + self.TAB_PAD_BOT

    def _current_row_count(self) -> int:
        if self._view_mode == 'report':
            return min(len(self._report_entities()), self.MAX_ROWS)
        return min(len(self._rows), self.MAX_ROWS)

    def _clamped_scroll(self) -> int:
        """Return the scroll offset clamped to current data bounds."""
        if self._view_mode == 'report':
            total = len(self._report_entities())
            return max(0, min(self._scroll_offset_report, max(0, total - self.MAX_ROWS)))
        return max(0, min(self._scroll_offset, max(0, len(self._rows) - self.MAX_ROWS)))

    def _build_view_rows(self) -> List[dict]:
        is_heal = self._current_tab == 'heal'
        rows: List[dict] = []

        if self._view_mode == 'report':
            entities = self._report_entities()
            entities.sort(
                key=lambda ent: float(ent.get('heal_total' if is_heal else 'damage_total') or 0),
                reverse=True,
            )
            scroll = max(0, min(self._scroll_offset_report, max(0, len(entities) - self.MAX_ROWS)))
            self._scroll_offset_report = scroll
            total = float((self._last_report or {}).get(
                'total_heal' if is_heal else 'total_damage'
            ) or 0)
            max_amount = max(
                [float(ent.get('heal_total' if is_heal else 'damage_total') or 0)
                 for ent in entities],
                default=0.0,
            )
            for ent in entities[scroll: scroll + self.MAX_ROWS]:
                uid = int(ent.get('uid') or 0)
                amount = float(ent.get('heal_total' if is_heal else 'damage_total') or 0)
                rows.append({
                    'uid': uid,
                    'name': str(ent.get('name') or f'Player_{uid or 0}'),
                    'profession': str(ent.get('profession') or ''),
                    'fight_point': int(ent.get('fight_point') or 0),
                    'is_self': bool(ent.get('is_self')) or (self._self_uid and uid == self._self_uid),
                    'amount': amount,
                    'rate': float(ent.get('hps' if is_heal else 'dps') or 0),
                    'pct': (amount / total) if total > 0 else 0.0,
                    'bar_pct': (amount / max_amount) if max_amount > 0 else 0.0,
                    'is_heal': is_heal,
                    'fx_tier': '',
                    'fx_start': 0.0,
                    'fallback_sub': 'LAST REPORT ENTRY',
                })
            return rows

        live_rows = list(self._rows.values())
        if is_heal:
            live_rows.sort(key=lambda row: (row.disp_heal, row.uid), reverse=True)
        else:
            live_rows.sort(key=lambda row: (row.disp_y, row.uid))
        total = self._disp_total_heal if is_heal else self._disp_total_damage
        max_amount = max(
            [row.disp_heal if is_heal else row.disp_damage for row in live_rows],
            default=0.0,
        )
        for row in live_rows[: self.MAX_ROWS]:
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
                'is_heal': is_heal,
                'fx_tier': row.fx_tier,
                'fx_start': row.fx_start,
                'fallback_sub': 'LIVE COMBAT ENTRY',
            })
        return rows

    def _compute_size(self) -> tuple:
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
        if not self._hwnd:
            return
        img = self.compose_frame(now)
        w, h = img.size

        if self._win is not None and (w, h) != self._last_rendered_size:
            try:
                self._win.geometry(f'{w}x{h}+{self._x}+{self._y}')
            except Exception:
                pass
            self._last_rendered_size = (w, h)

        try:
            _ulw_update(self._hwnd, img, self._x, self._y)
        except Exception as e:
            print(f'[DPS-OV] ulw error: {e}')

    def _build_shell_layer(self, w: int, h: int) -> Image.Image:
        """Compose the static shell (shadow + body + corners) once per size."""
        layer = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        ambient = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(ambient).rounded_rectangle(
            (self.BODY_PAD + 4, self.BODY_PAD + 8,
             w - self.BODY_PAD + 1, h - self.BODY_PAD + 4),
            radius=10, fill=(22, 24, 18, 62),
        )
        ambient = _gpu_blur(ambient, 8)
        layer.alpha_composite(ambient)

        contact = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(contact).rounded_rectangle(
            (self.BODY_PAD + 2, self.BODY_PAD + 4,
             w - self.BODY_PAD, h - self.BODY_PAD + 1),
            radius=8, fill=(31, 34, 16, 88),
        )
        contact = _gpu_blur(contact, 3)
        layer.alpha_composite(contact)

        sx, sy = self.BODY_PAD, self.BODY_PAD
        sw, sh = w - 2 * self.BODY_PAD, h - 2 * self.BODY_PAD
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
        if self._detail_visible:
            content_y = sy + hh
            footer_y = sy + sh - self.FOOTER_H
            list_x = sx + self.CONTENT_PAD_X
            list_w = sw - 2 * self.CONTENT_PAD_X
            list_h = footer_y - content_y - self.CONTENT_PAD_BOT
            self._draw_detail_view(draw, img, list_x, content_y, list_w, list_h)
        else:
            tabs_y = sy + hh
            self._draw_tabs(draw, sx, tabs_y, sw)
            content_y = tabs_y + self._tabs_height()
            footer_y = sy + sh - self.FOOTER_H
            list_x = sx + self.CONTENT_PAD_X
            list_w = sw - 2 * self.CONTENT_PAD_X
            list_h = footer_y - content_y - self.CONTENT_PAD_BOT
            self._draw_list_frame(draw, img, list_x, content_y, list_w, list_h)
        self._draw_footer(draw, sx, footer_y, sw, self.FOOTER_H)

        if self._panel_fx_tier:
            self._overlay_panel_flash(img, sx, sy, sw, sh, now)

        final_alpha = max(0.0, min(1.0, self.PANEL_OPACITY * self._fade_alpha))
        if final_alpha < 0.999:
            a = np.asarray(img, dtype=np.uint8).copy()
            mul = int(max(0, min(255, final_alpha * 255)))
            a[:, :, 3] = (a[:, :, 3].astype(np.uint16) * mul // 255
                          ).astype(np.uint8)
            img = Image.fromarray(a, 'RGBA')
        return img

    # --------  Shell (gradient + scanlines + borders)  --------

    def _draw_shell(self, img: Image.Image,
                    sx: int, sy: int, sw: int, sh: int) -> None:
        # Vertical gradient A→B (dark blue-black)
        grad = np.zeros((sh, 1, 4), dtype=np.uint8)
        ys = np.linspace(0, 1, sh)
        for i in range(4):
            grad[:, 0, i] = (self.PANEL_BG_A[i]
                             + (self.PANEL_BG_B[i] - self.PANEL_BG_A[i]) * ys)
        grad_img = Image.fromarray(grad, 'RGBA').resize((sw, sh))

        mask = Image.new('L', (sw, sh), 0)
        ImageDraw.Draw(mask).rounded_rectangle(
            (0, 0, sw - 1, sh - 1), radius=6, fill=255
        )
        img.paste(grad_img, (sx, sy), mask)

        # Top-cyan sheen (glow effect at the top edge)
        sheen = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
        sd_sheen = ImageDraw.Draw(sheen, 'RGBA')
        sd_sheen.rounded_rectangle(
            (1, 1, sw - 2, max(16, int(sh * 0.18))),
            radius=6, fill=(104, 228, 255, 18),
        )
        sd_sheen.rounded_rectangle(
            (2, max(12, int(sh * 0.42)), sw - 3, sh - 3),
            radius=6, fill=(0, 0, 0, 25),
        )
        sheen = _gpu_blur(sheen, 4)
        sheen_masked = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
        sheen_masked.paste(sheen, (0, 0), mask)
        img.alpha_composite(sheen_masked, (sx, sy))

        # Subtle horizontal scanlines (every 3px, faint cyan)
        scan = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
        sd = ImageDraw.Draw(scan)
        for y in range(0, sh, 3):
            sd.line((0, y, sw, y), fill=self.SCAN_LINE)
        scan_masked = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
        scan_masked.paste(scan, (0, 0), mask)
        img.alpha_composite(scan_masked, (sx, sy))

        draw = ImageDraw.Draw(img, 'RGBA')
        # Outer border (cyan-tinted)
        draw.rounded_rectangle(
            (sx, sy, sx + sw - 1, sy + sh - 1),
            radius=6, outline=self.PANEL_EDGE, width=1,
        )
        # Inner glow border
        draw.rounded_rectangle(
            (sx + 1, sy + 1, sx + sw - 2, sy + sh - 2),
            radius=6, outline=self.INNER_HIGHLIGHT, width=1,
        )
        # Top-edge cyan highlight
        draw.line(
            (sx + 1, sy + 2, sx + sw - 2, sy + 2),
            fill=self.PANEL_LINE, width=1,
        )

    def _draw_corners(self, draw: ImageDraw.ImageDraw,
                      sx: int, sy: int, sw: int, sh: int) -> None:
        cs = self.CORNER_SIZE
        # top-left: bright cyan
        cyan = self.CYAN
        draw.line((sx + 2, sy + 2, sx + 2 + cs, sy + 2),
                  fill=cyan, width=2)
        draw.line((sx + 2, sy + 2, sx + 2, sy + 2 + cs),
                  fill=cyan, width=2)
        # top-right: faint cyan accent
        draw.line((sx + sw - 2 - cs, sy + 2, sx + sw - 2, sy + 2),
                  fill=(104, 228, 255, 120), width=1)
        # bottom-left: faint gold accent
        draw.line((sx + 2, sy + sh - 2, sx + 2 + cs, sy + sh - 2),
                  fill=(222, 190, 80, 120), width=1)
        # bottom-right: gold
        gold_c = (222, 190, 80, 255)
        bx = sx + sw - 2
        by = sy + sh - 2
        draw.line((bx - cs, by, bx, by), fill=gold_c, width=2)
        draw.line((bx, by - cs, bx, by), fill=gold_c, width=2)

    # --------  Header  --------

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
                             fill=(30, 45, 65, 180),
                             outline=(60, 140, 180, 160))
        self._draw_tracked(draw, (bx + 8, by + 4), badge_label,
                           font_badge, badge_color, 1.1)
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
        font_sum = _load_font('sao', 10)
        self._draw_tracked(draw, (x_left, y + 2),
                           summary, font_sum, self.TEXT_MUTED, 0.85)
        y += self.SUMMARY_H + 8

        # Button row: LIVE | LAST REPORT | RESET (right-aligned, wrap-below)
        buttons = [
            ('LIVE', self._view_mode == 'live', 'live', True),
            ('LAST REPORT', self._view_mode == 'report', 'normal',
             self._report_available or self._has_report_data()),
            ('RESET', False, 'danger', True),
        ]
        btn_font = _load_font('sao', 10)
        # Measure and lay out right-aligned
        gap = self.BTN_GAP
        sizes = []
        for text, _, _, _ in buttons:
            tw = _text_width(draw, text, btn_font)
            sizes.append(max(76, tw + 20))
        total_w = sum(sizes) + gap * (len(sizes) - 1)
        start_x = x_right - total_w
        bx = start_x
        by = y
        for (text, active, kind, enabled), bw2 in zip(buttons, sizes):
            self._draw_button(draw, bx, by, bw2, self.BTN_H,
                              text, active, kind, btn_font, enabled)
            bx += bw2 + gap

        # Bottom border of header
        draw.line((sx, sy + hh, sx + sw - 1, sy + hh),
                  fill=self.DIVIDER, width=1)

    def _draw_button(self, draw: ImageDraw.ImageDraw, bx: int, by: int,
                     bw: int, bh: int, text: str, active: bool,
                     kind: str, font, enabled: bool = True) -> None:
        if not enabled:
            fill = (30, 35, 45, 120)
            border = (50, 60, 70, 100)
            fg = (80, 85, 95, 180)
        elif kind == 'live' and active:
            fill = self.BTN_LIVE_ACTIVE
            border = self.BTN_LIVE_BORDER
            fg = self.BTN_LIVE_COLOR
        elif active:
            fill = (222, 190, 80, 40)
            border = (222, 190, 80, 220)
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
                fill = (222, 190, 80, 35)
                border = (222, 190, 80, 220)
                fg = self.GOLD
            else:
                fill = (40, 55, 75, 120)
                border = (50, 100, 130, 100)
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
            radius=4, fill=(104, 228, 255, 6),
        )
        self._fill_rounded_rect(
            img, (lx + 3, ly + max(14, lh // 2), lx + lw - 4, ly + lh - 4),
            radius=4, fill=(0, 0, 0, 15),
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

        margin = self.ROW_MARGIN
        for rank_idx, row_data in enumerate(view_rows):
            ry = ly + margin + rank_idx * self.ROW_H
            rx = lx + margin
            rw = lw - 2 * margin
            rh = self.ROW_H - margin
            if ry + rh > ly + lh - 2:
                continue
            self._draw_row(draw, img, row_data, rx, ry, rw, rh, rank_idx)
            uid = int(row_data.get('uid') or 0)
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
        top_sheen = [
            (x + bevel + 1, y + 1),
            (x + w - 2, y + 1),
            (x + w - 3, y + min(h // 3 + 1, h - 4)),
            (x + 2, y + min(h // 3 + 1, h - 4)),
            (x + 1, y + bevel),
        ]
        self._fill_polygon(img, top_sheen, (104, 228, 255, 8))
        self._fill_polygon(
            img,
            [
                (x + bevel, y + max(8, h // 2)),
                (x + w - 2, y + max(8, h // 2)),
                (x + w - 2, y + h - 2),
                (x, y + h - 2),
                (x, y + bevel),
            ],
            (0, 0, 0, 20),
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
                outline=(222, 190, 80, 220),
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
                (222, 190, 80, 12),
            )

        # Animated bar fill (within clip)
        bar_pct = max(0.0, min(1.0, float(row['bar_pct'] or 0.0)))
        bar_w = int(max(0, (w - 4) * bar_pct))
        if bar_w > 0:
            bar_img = self._make_bar(bar_w, h - 4, row['is_self'], row['is_heal'])
            img.alpha_composite(bar_img, (x + 2, y + 2))

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
        rank = max(0, min(rank_idx, self.MAX_ROWS - 1))
        rank_color = self.RANK_COLORS.get(rank, self.TEXT_MUTED[:3])
        rf = _load_font('sao', 11)
        rank_txt = str(rank + 1)
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
        val_main = _fmt_num(row['amount'])
        val_color = name_color if _fx_shadow else self.TEXT_MAIN
        vw = self._tracked_text_width(draw, val_main, font_val, 0.7)
        self._draw_tracked(draw, (x + w - 10 - vw, y + 6), val_main,
                           font_val, val_color, 0.7,
                           shadow_color=_fx_shadow, shadow_blur=5 if _fx_shadow else 0)
        pct = int(round(float(row['pct'] or 0.0) * 100))
        val_sub = f'{_fmt_num(row["rate"])}/s · {pct}%'
        sw_ = self._tracked_text_width(draw, val_sub, font_sub, 0.75)
        self._draw_tracked(draw, (x + w - 10 - sw_, y + 22), val_sub,
                           font_sub, self.TEXT_MUTED, 0.75)

    # --------  Footer  --------

    def _draw_footer(self, draw: ImageDraw.ImageDraw,
                     sx: int, fy: int, sw: int, fh: int) -> None:
        # Top divider (cyan-tinted)
        draw.line((sx, fy, sx + sw - 1, fy), fill=self.DIVIDER, width=1)
        # Footer bg (dark tint)
        self._fill_rect(
            draw._image,
            (sx + 1, fy + 1, sx + sw - 2, fy + fh - 2),
            fill=(15, 20, 30, 140),
        )
        self._fill_rect(
            draw._image,
            (sx + 2, fy + 2, sx + sw - 3, fy + min(fh // 2, 16)),
            fill=(104, 228, 255, 6),
        )
        self._fill_rect(
            draw._image,
            (sx + 2, fy + max(14, fh // 2), sx + sw - 3, fy + fh - 3),
            fill=(0, 0, 0, 18),
        )
        font = _load_font('sao', 10)
        x_left = sx + self.HEADER_PAD_X
        x_right = sx + sw - self.HEADER_PAD_X
        if self._view_mode == 'report' and self._has_report_data():
            completed = str((self._last_report or {}).get('completed_local_time') or _fmt_time((self._last_report or {}).get('elapsed_s') or 0))
            left = f'COMPLETED {completed}'
            total_val = _fmt_num(float((self._last_report or {}).get('total_damage') or 0))
            heal_val = _fmt_num(float((self._last_report or {}).get('total_heal') or 0))
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
        uid = int(self._detail_uid or 0)
        if uid <= 0:
            return None
        if self._view_mode == 'report':
            for ent in self._report_entities():
                if int(ent.get('uid') or 0) == uid:
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

        back_w = 56
        back_rect = (head_x, head_y, head_x + back_w, head_y + head_h - 4)
        self._draw_clip_rect(draw, back_rect[0], back_rect[1],
                             back_rect[2] - back_rect[0],
                             back_rect[3] - back_rect[1],
                             fill=self.BTN_BG, outline=self.BTN_BORDER,
                             bevel=8)
        font_back = _load_font('sao', 10)
        self._draw_tracked_centered(draw, 'BACK', font_back, self.TEXT_MAIN,
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
            fp = int(entity.get('fight_point') or 0)
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

        # Stats grid: 2 cols × 4 rows of small cards
        grid_x = lx + 10
        grid_w = lw - 20
        col_gap = 6
        col_w = (grid_w - col_gap) // 2
        card_h = 30
        card_gap = 4
        rows = 4
        grid_h = rows * card_h + (rows - 1) * card_gap
        stats = [
            ('DAMAGE', _fmt_num(entity.get('damage_total') or 0), self.GOLD),
            ('DPS', _fmt_num(entity.get('dps') or 0), self.TEXT_MAIN),
            ('HEALING', _fmt_num(entity.get('heal_total') or 0), (92, 150, 44, 255)),
            ('HPS', _fmt_num(entity.get('hps') or 0), self.TEXT_MAIN),
            ('CRIT', f"{int(round(float(entity.get('crit_rate') or 0) * 100))}%", self.TEXT_MAIN),
            ('MAX HIT', _fmt_num(entity.get('max_hit') or 0), self.GOLD),
            ('HITS', str(int(entity.get('damage_hits') or 0)), self.TEXT_MAIN),
            ('TIME', _fmt_time(float(entity.get('elapsed_s') or 0)), self.TEXT_MAIN),
        ]
        lbl_font = _load_font('sao', 8)
        val_font = _load_font('sao', 13)
        for i, (label, value, color) in enumerate(stats):
            r = i // 2
            c = i % 2
            cx = grid_x + c * (col_w + col_gap)
            cy = body_y + r * (card_h + card_gap)
            self._fill_rounded_rect(
                img, (cx, cy, cx + col_w - 1, cy + card_h - 1),
                radius=3, fill=(35, 45, 60, 160),
            )
            draw.rounded_rectangle(
                (cx, cy, cx + col_w - 1, cy + card_h - 1),
                radius=3, outline=self.ROW_BORDER, width=1,
            )
            self._draw_tracked(draw, (cx + 6, cy + 3), label,
                               lbl_font, self.TEXT_MUTED, 1.1)
            vw = self._tracked_text_width(draw, value, val_font, 0.7)
            self._draw_tracked(draw, (cx + col_w - 6 - vw, cy + 13),
                               value, val_font, color, 0.7)

        # Skill rows below the stats grid
        sk_y = body_y + grid_h + 6
        sk_h = ly + lh - sk_y - 4
        if sk_h <= 12:
            return
        skills = entity.get('skills') or []
        if not skills:
            font = _load_font('sao', 10)
            msg = ('NO SKILL DATA IN LAST REPORT' if self._view_mode == 'report'
                   else 'WAITING FOR LIVE SKILL DETAIL')
            self._draw_tracked_centered(draw, msg, font, self.TEXT_MUTED,
                                        lx + lw // 2, sk_y + sk_h // 2 - 6, 1.5)
            return

        # Sort by max(damage_total, heal_total) like the webview
        skills_sorted = sorted(
            list(skills),
            key=lambda s: max(float(s.get('total') or 0),
                              float(s.get('heal_total') or 0)),
            reverse=True,
        )
        max_val = max(
            (max(float(s.get('total') or 0), float(s.get('heal_total') or 0))
             for s in skills_sorted),
            default=0.0,
        ) or 1.0
        sk_row_h = 22
        max_rows = max(1, sk_h // (sk_row_h + 2))
        sk_font_extra = _load_font('sao', 8)
        sk_font_val = _load_font('sao', 11)
        for i, sk in enumerate(skills_sorted[:max_rows]):
            ry = sk_y + i * (sk_row_h + 2)
            if ry + sk_row_h > ly + lh - 4:
                break
            dmg = float(sk.get('total') or 0)
            heal = float(sk.get('heal_total') or 0)
            amount = max(dmg, heal)
            is_heal = heal > dmg
            hits = int(sk.get('heal_hits' if is_heal else 'hits') or 0)
            crit = (float(sk.get('crit_rate') or 0)
                    if not is_heal and int(sk.get('hits') or 0) > 0 else 0)
            bar_pct = amount / max_val if max_val > 0 else 0
            # Background bar
            self._fill_rounded_rect(
                img, (lx + 6, ry, lx + lw - 7, ry + sk_row_h - 1),
                radius=2, fill=(30, 40, 55, 140),
            )
            bar_w = int((lw - 14) * bar_pct)
            if bar_w > 0:
                bar_color = ((154, 211, 52, 70) if is_heal
                             else (222, 190, 80, 60))
                self._fill_rounded_rect(
                    img, (lx + 6, ry, lx + 6 + bar_w, ry + sk_row_h - 1),
                    radius=2, fill=bar_color,
                )
            # Pick font based on the actual skill name text so CJK skill
            # names (Chinese skill names like 星辉剑制, 岚刃 etc.) use
            # ZhuZiAYuanJWD instead of SAOUI.ttf which has no CJK glyphs
            # and renders them as boxes.
            sk_name_raw = str(sk.get('skill_name') or sk.get('skill_id') or 'Unknown')
            sk_font_name = _pick_font(sk_name_raw, 10)
            sk_name = self._truncate(
                sk_name_raw,
                sk_font_name, int((lw - 14) * 0.55), draw,
            )
            self._draw_tracked(draw, (lx + 12, ry + 2), sk_name,
                               sk_font_name, self.TEXT_MAIN, 0.6)
            extras = [('HEAL' if is_heal else 'DMG'), f'×{hits}']
            if not is_heal and crit > 0:
                extras.append(f'CRIT {int(round(crit * 100))}%')
            extra_text = ' · '.join(extras)
            self._draw_tracked(draw, (lx + 12, ry + sk_row_h - 11),
                               extra_text, sk_font_extra,
                               self.TEXT_MUTED, 0.6)
            val_text = _fmt_num(amount)
            vw = self._tracked_text_width(draw, val_text, sk_font_val, 0.6)
            val_color = ((92, 150, 44, 255) if is_heal else self.GOLD)
            self._draw_tracked(draw, (lx + lw - 12 - vw, ry + 4),
                               val_text, sk_font_val, val_color, 0.6)

    # --------  Helpers  --------

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
        overlay = Image.new('RGBA', img.size, (0, 0, 0, 0))
        ImageDraw.Draw(overlay, 'RGBA').rectangle(box, fill=fill)
        img.alpha_composite(overlay)

    @staticmethod
    def _fill_rounded_rect(img: Image.Image, box, radius: int, fill) -> None:
        overlay = Image.new('RGBA', img.size, (0, 0, 0, 0))
        ImageDraw.Draw(overlay, 'RGBA').rounded_rectangle(
            box, radius=radius, fill=fill
        )
        img.alpha_composite(overlay)

    @staticmethod
    def _fill_polygon(img: Image.Image, poly, fill) -> None:
        overlay = Image.new('RGBA', img.size, (0, 0, 0, 0))
        ImageDraw.Draw(overlay, 'RGBA').polygon(poly, fill=fill)
        img.alpha_composite(overlay)

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
            try:
                cw = draw.textlength(ch, font=font)
            except Exception:
                cw = font.size / 2
            acc += cw + spacing

    @staticmethod
    def _draw_text_shadow(draw, text, font, color, blur, x, y, spacing):
        from PIL import Image as _Img, ImageFilter as _IF, ImageDraw as _ID
        acc = 0.0
        widths = []
        for ch in text:
            try:
                cw = draw.textlength(ch, font=font)
            except Exception:
                cw = font.size / 2
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
        total = 0.0
        widths = []
        for ch in text:
            try:
                cw = draw.textlength(ch, font=font)
            except Exception:
                cw = font.size / 2
            widths.append(cw)
            total += cw + spacing
        total -= spacing
        x = cx - total / 2
        acc = 0.0
        for ch, cw in zip(text, widths):
            draw.text((int(round(x + acc)), cy), ch, fill=fill, font=font)
            acc += cw + spacing

    def _tracked_text_width(self, draw, text: str, font,
                            spacing: float = 1) -> int:
        """Return the total pixel width of tracked text."""
        total = 0.0
        for ch in text:
            try:
                cw = draw.textlength(ch, font=font)
            except Exception:
                cw = font.size / 2
            total += cw + spacing
        if text:
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

    def _control_regions(self) -> dict:
        w, h = self._last_rendered_size
        if not w or not h:
            w, h = self._compute_size()
        sx, sy = self.BODY_PAD, self.BODY_PAD
        sw = w - 2 * self.BODY_PAD
        hh = self._header_height()
        dummy = ImageDraw.Draw(Image.new('RGBA', (1, 1), (0, 0, 0, 0)))

        btn_font = _load_font('sao', 10)
        buttons = [('live', 'LIVE'), ('report', 'LAST REPORT'), ('reset', 'RESET')]
        sizes = [max(76, _text_width(dummy, label, btn_font) + 20)
                 for _, label in buttons]
        total_w = sum(sizes) + self.BTN_GAP * (len(sizes) - 1)
        start_x = sx + sw - self.HEADER_PAD_X - total_w
        start_y = (sy + self.HEADER_PAD_TOP + self.EYEBROW_H + 4
                   + self.TITLE_H + 6 + self.SUMMARY_H + 8)

        button_regions = {}
        cur_x = start_x
        for (name, _label), bw in zip(buttons, sizes):
            button_regions[name] = (cur_x, start_y, cur_x + bw, start_y + self.BTN_H)
            cur_x += bw + self.BTN_GAP

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

    def _activate_reset(self) -> None:
        snapshot = None
        if callable(self._reset_dps_cb):
            try:
                snapshot = self._reset_dps_cb()
            except Exception:
                snapshot = None
        self.show_live(snapshot or _empty_snapshot())

    def _handle_click(self, x: int, y: int) -> None:
        # Detail-view back button has highest priority
        if (self._detail_visible and self._detail_back_rect
                and self._point_in_rect(x, y, self._detail_back_rect)):
            self.close_detail()
            return
        regions = self._control_regions()
        for name, rect in regions['buttons'].items():
            if self._point_in_rect(x, y, rect):
                if name == 'live':
                    self._activate_live()
                elif name == 'report':
                    if self._report_available or self._has_report_data():
                        self._activate_report()
                    else:
                        self._notify_no_report()
                elif name == 'reset':
                    self._activate_reset()
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

    def _on_drag_start(self, ev) -> None:
        try:
            self._drag_ox = ev.x_root - self._x
            self._drag_oy = ev.y_root - self._y
            self._drag_start_root = (int(ev.x_root), int(ev.y_root))
            self._drag_moved = False
        except Exception:
            self._drag_ox = 0
            self._drag_oy = 0
            self._drag_start_root = (0, 0)
            self._drag_moved = False

    def _on_drag_move(self, ev) -> None:
        try:
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
        """Handle mouse wheel to scroll the entity list."""
        # ev.delta is positive = scroll up, negative = scroll down on Windows
        delta = getattr(ev, 'delta', 0)
        if delta > 0:
            self._scroll(-1)
        elif delta < 0:
            self._scroll(1)

    def _scroll(self, direction: int) -> None:
        """Scroll the entity list by one row in the given direction (+1 down, -1 up)."""
        is_report = self._view_mode == 'report'
        if is_report:
            total = len(self._report_entities())
            offset_attr = '_scroll_offset_report'
        else:
            total = len(self._rows)
            offset_attr = '_scroll_offset'
        max_offset = max(0, total - self.MAX_ROWS)
        old = getattr(self, offset_attr)
        new = max(0, min(max_offset, old + direction))
        if new != old:
            setattr(self, offset_attr, new)
            # Reset row positions so they animate smoothly
            self._schedule_tick(immediate=True)
