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
from typing import Any, Dict, List, Optional

import numpy as np
import tkinter as tk
from PIL import Image, ImageDraw, ImageFont, ImageFilter
from gpu_renderer import gaussian_blur_rgba as _gpu_blur
from overlay_scheduler import get_scheduler as _get_scheduler

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

_BASE_DIR = (
    getattr(sys, '_MEIPASS', os.path.dirname(sys.executable))
    if getattr(sys, 'frozen', False)
    else os.path.dirname(os.path.abspath(__file__))
)
_FONT_DIR = os.path.join(_BASE_DIR, 'assets', 'fonts')

_FONT_CACHE: Dict[tuple, Any] = {}


def _load_font(kind: str, size: int):
    """Load a PIL font (SAOUI for ASCII, ZhuZiAYuanJWD for CJK fallback)."""
    key = (kind, size)
    cached = _FONT_CACHE.get(key)
    if cached is not None:
        return cached
    filename = 'SAOUI.ttf' if kind == 'sao' else 'ZhuZiAYuanJWD.ttf'
    path = os.path.join(_FONT_DIR, filename)
    font = None
    try:
        font = ImageFont.truetype(path, size)
    except Exception:
        for sysname in ('segoeui.ttf', 'msyh.ttc', 'arial.ttf'):
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
        return f"{v / 1_000_000:.{0 if v >= 10_000_000 else 1}f}M"
    if v >= 1_000:
        return f"{v / 1_000:.{0 if v >= 100_000 else 1}f}K"
    return f"{int(round(v)):,}"


def _fmt_time(s: float) -> str:
    s = max(0, int(s or 0))
    return f"{s // 60:02d}:{s % 60:02d}"


def _fmt_fp(fp: float) -> str:
    fp = float(fp or 0)
    if fp <= 0:
        return ''
    if fp >= 1_000_000:
        return f"{fp / 1_000_000:.2f}M"
    if fp >= 1_000:
        return f"{fp / 1_000:.{0 if fp >= 100_000 else 1}f}K"
    return f"{int(fp):,}"


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
    DEFAULT_HEIGHT = 460
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

    # Colors (parity with web/dps.html CSS vars, alpha 0-255)
    PANEL_BG_A = (203, 205, 194, 255)   # opaque shell bg to avoid hollow panel body
    PANEL_BG_B = (216, 218, 206, 255)   # opaque shell bg to avoid hollow panel body
    PANEL_EDGE = (160, 162, 150, 158)   # --panel-edge 0.62
    PANEL_LINE = (255, 255, 255, 132)   # --panel-line 0.52
    INNER_HIGHLIGHT = (255, 255, 255, 40)   # ::after border 0.16
    SCAN_LINE = (255, 255, 255, 14)         # ::before 0.055
    TEXT_MAIN = (83, 84, 73, 255)
    TEXT_MUTED = (139, 139, 130, 255)
    GOLD = (222, 166, 32, 255)
    GOLD_SOFT = (222, 166, 32, 56)
    CYAN = (104, 228, 255, 140)         # --cyan 0.55
    DIVIDER = (160, 162, 150, 128)      # header/footer borders 0.5
    LIST_BG = (255, 255, 255, 46)       # list-frame bg 0.18
    LIST_BORDER = (160, 162, 150, 107)  # list-frame border 0.42
    ROW_BG = (255, 255, 255, 122)       # entity-row bg 0.48
    ROW_BORDER = (160, 162, 150, 82)    # entity-row border 0.32
    ROW_SELF_BAR = (222, 166, 32, 220)  # border-left on .self
    BTN_BG = (255, 255, 255, 112)       # .sao-btn bg 0.44
    BTN_BORDER = (160, 162, 150, 163)   # .sao-btn border 0.64
    BTN_LIVE_ACTIVE = (104, 228, 255, 31)    # secondary.active 0.12
    BTN_LIVE_BORDER = (104, 228, 255, 184)   # secondary.active border 0.72
    BTN_LIVE_COLOR = (68, 144, 162, 255)     # secondary.active color
    BTN_DANGER = (239, 104, 78, 255)         # --danger
    BAR_OTHER_A = (222, 166, 32, 51)    # entity-bar gradient 0.20→0.03
    BAR_OTHER_B = (222, 166, 32, 8)
    BAR_HEAL_A = (154, 211, 52, 61)
    BAR_HEAL_B = (154, 211, 52, 8)
    BADGE_LIVE = (82, 140, 48, 255)     # mode-badge.live
    BADGE_REPORT = (222, 166, 32, 255)  # mode-badge.report
    RANK_COLORS = {
        0: (222, 166, 32),              # --gold
        1: (130, 132, 140),             # .rank.r2
        2: (177, 132, 74),              # .rank.r3
    }

    # Animation timings (seconds)
    BAR_TWEEN = 0.35
    NUM_TWEEN = 0.45
    ROW_TWEEN = 0.30
    FADE_IN = 0.28
    FADE_OUT = 0.26

    TICK_MS = 16          # ~60 FPS while animating
    IDLE_TICK_MS = 60     # idle refresh still drives clock / fade-out

    def __init__(self, root: tk.Tk, settings: Any = None):
        self.root = root
        self.settings = settings
        self._win: Optional[tk.Toplevel] = None
        self._hwnd: int = 0
        self._visible = False
        self._last_snapshot: Optional[dict] = None
        self._self_uid = 0

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

        self._tick_after_id: Optional[str] = None
        self._last_rendered_size: tuple = (0, 0)
        self._registered: bool = False

        # Static shell layer cache (shadow + shell + corners).
        # Only depends on (w, h); reused as long as panel size is stable.
        self._shell_cache: Optional[Image.Image] = None
        self._shell_cache_size: tuple = (0, 0)

    # ──────────────────────────────────────────
    #  Lifecycle
    # ──────────────────────────────────────────

    def show(self) -> None:
        if self._win is not None:
            return
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

        # Dragging
        self._win.bind('<Button-1>', self._on_drag_start)
        self._win.bind('<B1-Motion>', self._on_drag_move)
        self._win.bind('<ButtonRelease-1>', self._on_drag_end)

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
        if self._win is not None:
            try:
                self._win.destroy()
            except Exception:
                pass
        self._win = None
        self._hwnd = 0
        self._visible = False

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

    # ──────────────────────────────────────────
    #  Snapshot ingestion
    # ──────────────────────────────────────────

    def update(self, snapshot: dict) -> None:
        if snapshot is None:
            return
        if not self._visible or not self._hwnd:
            self.show()
        self._last_snapshot = snapshot

        self._target_total_damage = float(snapshot.get('total_damage') or 0)
        self._target_total_dps = float(snapshot.get('total_dps') or 0)
        self._target_total_heal = float(snapshot.get('total_heal') or 0)
        self._target_total_hps = float(snapshot.get('total_hps') or 0)
        self._target_elapsed = float(snapshot.get('elapsed_s') or 0)
        self._encounter_active = bool(snapshot.get('encounter_active'))

        # dps_tracker emits 'entities'; tolerate legacy 'party' too.
        entities = snapshot.get('entities')
        if entities is None:
            entities = snapshot.get('party') or []
        entities = list(entities or [])

        seen_uids: List[int] = []
        for idx, ent in enumerate(entities[: self.MAX_ROWS]):
            try:
                uid = int(ent.get('uid') or 0)
            except Exception:
                uid = 0
            if not uid:
                continue
            row = self._rows.get(uid)
            if row is None:
                row = _RowState(uid)
                # New rows slide in from just below their target slot.
                row.disp_y = float(idx + 0.6)
                row.target_y = float(idx)
                self._rows[uid] = row
            else:
                row.target_y = float(idx)
            row.update_targets(ent)
            if self._self_uid and uid == self._self_uid:
                row.is_self = True
            seen_uids.append(uid)

        # Drop rows that fell off the snapshot.
        for stale_uid in [u for u in self._rows if u not in seen_uids]:
            self._rows.pop(stale_uid, None)

        self._row_order = seen_uids

        # HitFX
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
            if abs(row.disp_dps - row.dps) > 0.5 or \
               abs(row.disp_damage - row.damage) > 0.5 or \
               abs(row.disp_hps - row.hps) > 0.5 or \
               abs(row.disp_heal - row.heal) > 0.5:
                return True
        if abs(self._disp_total_damage - self._total_damage) > 0.5:
            return True
        if abs(self._disp_total_dps - self._total_dps) > 0.5:
            return True
        return False

    def _tick(self, now: Optional[float] = None) -> None:
        if not self._visible or self._win is None:
            return
        if now is None:
            now = time.time()
        self._advance_animations(now)
        try:
            self._render(now)
        except Exception as e:
            print(f'[DPS-OV] render error: {e}')
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
        self._disp_total_damage = self._decay_toward(
            self._disp_total_damage, self._target_total_damage, self.NUM_TWEEN)
        self._disp_total_dps = self._decay_toward(
            self._disp_total_dps, self._target_total_dps, self.NUM_TWEEN)
        self._disp_total_heal = self._decay_toward(
            self._disp_total_heal, self._target_total_heal, self.NUM_TWEEN)
        self._disp_total_hps = self._decay_toward(
            self._disp_total_hps, self._target_total_hps, self.NUM_TWEEN)
        # Elapsed ticks in real time — snap rather than tween.
        self._disp_elapsed = self._target_elapsed
        return prev != (self._disp_total_damage, self._disp_total_dps,
                        self._disp_total_heal, self._disp_total_hps,
                        self._disp_elapsed)

    def _step_row(self, row: _RowState, now: float) -> bool:
        before = (row.disp_damage, row.disp_dps, row.disp_heal, row.disp_hps,
                  row.disp_bar_pct, row.disp_y)
        row.disp_damage = self._decay_toward(
            row.disp_damage, row.target_damage, self.NUM_TWEEN)
        row.disp_dps = self._decay_toward(
            row.disp_dps, row.target_dps, self.NUM_TWEEN)
        row.disp_heal = self._decay_toward(
            row.disp_heal, row.target_heal, self.NUM_TWEEN)
        row.disp_hps = self._decay_toward(
            row.disp_hps, row.target_hps, self.NUM_TWEEN)
        row.disp_bar_pct = self._decay_toward(
            row.disp_bar_pct, row.target_bar_pct, self.BAR_TWEEN)
        row.disp_y = self._decay_toward(
            row.disp_y, row.target_y, self.ROW_TWEEN)
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

    def _compute_size(self) -> tuple:
        rows = min(max(0, len(self._rows)), self.MAX_ROWS)
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
        shadow = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(shadow).rounded_rectangle(
            (self.BODY_PAD + 2, self.BODY_PAD + 4,
             w - self.BODY_PAD, h - self.BODY_PAD),
            radius=8, fill=(31, 34, 16, 72),
        )
        shadow = _gpu_blur(shadow, 18)
        layer.alpha_composite(shadow)

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

        if self._fade_alpha < 0.999:
            a = np.asarray(img, dtype=np.uint8).copy()
            mul = int(max(0, min(255, self._fade_alpha * 255)))
            a[:, :, 3] = (a[:, :, 3].astype(np.uint16) * mul // 255
                          ).astype(np.uint8)
            img = Image.fromarray(a, 'RGBA')
        return img

    # --------  Shell (gradient + scanlines + borders)  --------

    def _draw_shell(self, img: Image.Image,
                    sx: int, sy: int, sw: int, sh: int) -> None:
        # Vertical gradient A→B
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

        # ::before repeating horizontal scanlines (every 4px, rgba .055)
        scan = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
        sd = ImageDraw.Draw(scan)
        for y in range(0, sh, 4):
            sd.line((0, y, sw, y), fill=self.SCAN_LINE)
        scan_masked = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
        scan_masked.paste(scan, (0, 0), mask)
        img.alpha_composite(scan_masked, (sx, sy))

        draw = ImageDraw.Draw(img, 'RGBA')
        # Outer border
        draw.rounded_rectangle(
            (sx, sy, sx + sw - 1, sy + sh - 1),
            radius=6, outline=self.PANEL_EDGE, width=1,
        )
        # Inner highlight (::after rgba 0.16 inside)
        draw.rounded_rectangle(
            (sx + 1, sy + 1, sx + sw - 2, sy + sh - 2),
            radius=6, outline=self.INNER_HIGHLIGHT, width=1,
        )
        # Top-edge highlight (box-shadow inset 0 1px 0 rgba(255,255,255,0.42))
        draw.line(
            (sx + 1, sy + 2, sx + sw - 2, sy + 2),
            fill=(255, 255, 255, 107), width=1,
        )

    def _draw_corners(self, draw: ImageDraw.ImageDraw,
                      sx: int, sy: int, sw: int, sh: int) -> None:
        cs = self.CORNER_SIZE
        # top-left: cyan
        cyan = self.CYAN
        draw.line((sx + 2, sy + 2, sx + 2 + cs, sy + 2),
                  fill=cyan, width=2)
        draw.line((sx + 2, sy + 2, sx + 2, sy + 2 + cs),
                  fill=cyan, width=2)
        # bottom-right: gold-ish
        gold_c = (212, 156, 23, 184)
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

        # Mode-badge "LIVE" (green) or "REPORT" (gold)
        badge_label = 'LIVE' if self._encounter_active else 'IDLE'
        badge_color = self.BADGE_LIVE if self._encounter_active \
            else self.TEXT_MUTED
        bx = x_left + title_w + 10
        by = y
        font_badge = _load_font('sao', 10)
        bw_text = self._tracked_text_width(draw, badge_label, font_badge, 1.1)
        bw = bw_text + 16
        bh_ = 20
        self._draw_clip_rect(draw, bx, by, bw, bh_,
                             fill=(255, 255, 255, 107),
                             outline=(160, 162, 150, 163))
        self._draw_tracked(draw, (bx + 8, by + 4), badge_label,
                           font_badge, badge_color, 1.1)
        y += self.TITLE_H

        # Summary
        if self._encounter_active:
            summary = (
                f'COMBAT ACTIVE · '
                f'{_fmt_num(self._disp_total_damage)} DMG · '
                f'{_fmt_num(self._disp_total_dps)}/s'
            )
        elif self._rows:
            summary = 'WAITING FOR COMBAT DATA'
        else:
            summary = 'AWAITING ENGAGEMENT'
        font_sum = _load_font('sao', 10)
        self._draw_tracked(draw, (x_left, y + 2),
                           summary, font_sum, self.TEXT_MUTED, 0.85)
        y += self.SUMMARY_H + 8

        # Button row: LIVE | LAST REPORT | RESET (right-aligned, wrap-below)
        buttons = [
            ('LIVE', True, 'live'),
            ('LAST REPORT', False, 'normal'),
            ('RESET', False, 'danger'),
        ]
        btn_font = _load_font('sao', 10)
        # Measure and lay out right-aligned
        gap = self.BTN_GAP
        sizes = []
        for text, _, _ in buttons:
            tw = _text_width(draw, text, btn_font)
            sizes.append(max(76, tw + 20))
        total_w = sum(sizes) + gap * (len(sizes) - 1)
        start_x = x_right - total_w
        bx = start_x
        by = y
        for (text, active, kind), bw2 in zip(buttons, sizes):
            self._draw_button(draw, bx, by, bw2, self.BTN_H,
                              text, active, kind, btn_font)
            bx += bw2 + gap

        # Bottom border of header
        draw.line((sx, sy + hh, sx + sw - 1, sy + hh),
                  fill=self.DIVIDER, width=1)

    def _draw_button(self, draw: ImageDraw.ImageDraw, bx: int, by: int,
                     bw: int, bh: int, text: str, active: bool,
                     kind: str, font) -> None:
        if kind == 'live' and active:
            fill = self.BTN_LIVE_ACTIVE
            border = self.BTN_LIVE_BORDER
            fg = self.BTN_LIVE_COLOR
        elif active:
            fill = (222, 166, 32, 46)
            border = (222, 166, 32, 184)
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
        tabs = [('DAMAGE', True), ('HEALING', False)]
        gap = 8
        tab_w = (x1 - x0 - gap) // 2
        tf = _load_font('sao', 10)
        for i, (label, active) in enumerate(tabs):
            tx = x0 + i * (tab_w + gap)
            if active:
                fill = (222, 166, 32, 33)       # 0.13
                border = (222, 166, 32, 168)     # 0.66
                fg = self.GOLD
            else:
                fill = (255, 255, 255, 66)      # 0.26
                border = (160, 162, 150, 133)   # 0.52
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
        # Frame background
        self._fill_rounded_rect(
            img, (lx, ly, lx + lw - 1, ly + lh - 1), radius=4, fill=self.LIST_BG
        )
        draw.rounded_rectangle(
            (lx, ly, lx + lw - 1, ly + lh - 1),
            radius=4, outline=self.LIST_BORDER, width=1,
        )
        if not self._rows:
            msg = 'WAITING FOR COMBAT DATA'
            font = _load_font('sao', 11)
            self._draw_tracked_centered(draw, msg, font, self.TEXT_MUTED,
                                        lx + lw // 2, ly + lh // 2 - 6, 2)
            return

        # Clip region is list-frame; render rows in animated Y order
        rows = sorted(self._rows.values(), key=lambda r: r.disp_y)
        margin = self.ROW_MARGIN
        for row in rows:
            if row.disp_y < -1 or row.disp_y > self.MAX_ROWS + 1:
                continue
            rank_idx = int(round(row.disp_y))
            ry = ly + margin + int(round(row.disp_y * self.ROW_H))
            rx = lx + margin
            rw = lw - 2 * margin
            rh = self.ROW_H - margin
            if ry + rh > ly + lh - 2:
                continue
            self._draw_row(draw, img, row, rx, ry, rw, rh, rank_idx)

    def _draw_row(self, draw: ImageDraw.ImageDraw, img: Image.Image,
                  row: _RowState, x: int, y: int, w: int, h: int,
                  rank_idx: int) -> None:
        # Row background with clip-path:polygon(10px 0,100% 0,100% 100%,0 100%,0 10px)
        self._draw_clip_rect(draw, x, y, w, h,
                             fill=self.ROW_BG, outline=self.ROW_BORDER,
                             bevel=10)
        # Self highlight: left gold border
        if row.is_self:
            draw.line((x + 1, y + 2, x + 1, y + h - 2),
                      fill=self.ROW_SELF_BAR, width=3)

        # Animated bar fill (within clip)
        bar_pct = max(0.0, min(1.0, row.disp_bar_pct))
        bar_w = int(max(0, (w - 4) * bar_pct))
        if bar_w > 0:
            bar_img = self._make_bar(bar_w, h - 4, row)
            img.alpha_composite(bar_img, (x + 2, y + 2))

        # Hit-fx pulse outline
        if row.fx_tier:
            dur, tint, _ = _HIT_FX_TIERS[row.fx_tier]
            age = time.time() - row.fx_start
            t = max(0.0, min(1.0, age / max(0.01, dur)))
            intensity = (1.0 - t) ** 2
            outline_a = int(220 * intensity)
            if outline_a > 8:
                self._draw_clip_rect(
                    draw, x, y, w, h,
                    outline=(tint[0], tint[1], tint[2], outline_a),
                    bevel=10,
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
        name_font = _pick_font(row.name, 12)
        name_color = self.GOLD if row.is_self else self.TEXT_MAIN
        max_name_w = int(w * 0.55) - 40
        name = self._truncate(row.name or 'Unknown', name_font,
                              max_name_w, draw)

        # Hit-FX text color + shadow (web: .entity-row.impact-hit etc.)
        _fx_shadow = None
        if row.fx_tier:
            _dur, _tint, _ = _HIT_FX_TIERS[row.fx_tier]
            _age = time.time() - row.fx_start
            _t = max(0.0, min(1.0, _age / max(0.01, _dur)))
            _int = max(0.0, (1.0 - _t) ** 2)
            if row.fx_tier == 'impact':
                name_color = (57, 126, 146, 255)
                _fx_shadow = (104, 228, 255, int(66 * _int))
            elif row.fx_tier == 'mega':
                name_color = (196, 135, 16, 255)
                _fx_shadow = (255, 220, 112, int(87 * _int))
            elif row.fx_tier == 'starburst':
                name_color = (110, 118, 182, 255)
                _fx_shadow = (88, 166, 255, int(77 * _int))

        self._draw_tracked(draw, (name_x, y + 7), name,
                           name_font, name_color, 0.7,
                           shadow_color=_fx_shadow, shadow_blur=5 if _fx_shadow else 0)

        sub_parts = []
        if row.profession:
            sub_parts.append(row.profession)
        fp = _fmt_fp(row.fight_point)
        if fp:
            sub_parts.append(fp)
        sub_text = ' · '.join(sub_parts).upper() if sub_parts else ''
        if sub_text:
            sub_font = _pick_font(sub_text, 9)
            sub = self._truncate(sub_text, sub_font, max_name_w, draw)
            self._draw_tracked(draw, (name_x, y + 22),
                               sub, sub_font, self.TEXT_MUTED, 0.75)

        # Right side: damage total (13px) + dps/pct (9px muted)
        font_val = _load_font('sao', 13)
        font_sub = _load_font('sao', 9)
        val_main = _fmt_num(row.disp_damage)
        val_color = name_color if _fx_shadow else self.TEXT_MAIN
        vw = self._tracked_text_width(draw, val_main, font_val, 0.7)
        self._draw_tracked(draw, (x + w - 10 - vw, y + 6), val_main,
                           font_val, val_color, 0.7,
                           shadow_color=_fx_shadow, shadow_blur=5 if _fx_shadow else 0)
        pct = int(round(row.damage_pct * 100))
        val_sub = f'{_fmt_num(row.disp_dps)}/s · {pct}%'
        sw_ = self._tracked_text_width(draw, val_sub, font_sub, 0.75)
        self._draw_tracked(draw, (x + w - 10 - sw_, y + 22), val_sub,
                           font_sub, self.TEXT_MUTED, 0.75)

    # --------  Footer  --------

    def _draw_footer(self, draw: ImageDraw.ImageDraw,
                     sx: int, fy: int, sw: int, fh: int) -> None:
        # Top divider
        draw.line((sx, fy, sx + sw - 1, fy), fill=self.DIVIDER, width=1)
        # Footer bg (rgba 0.30)
        self._fill_rect(
            draw._image,
            (sx + 1, fy + 1, sx + sw - 2, fy + fh - 2),
            fill=(255, 255, 255, 76),
        )
        font = _load_font('sao', 10)
        x_left = sx + self.HEADER_PAD_X
        x_right = sx + sw - self.HEADER_PAD_X
        left = f'ELAPSED {_fmt_time(self._disp_elapsed)}'
        self._draw_tracked(draw, (x_left, fy + (fh - 12) // 2),
                           left, font, self.TEXT_MUTED, 0.85)
        total_val = _fmt_num(self._disp_total_damage)
        heal_val = _fmt_num(self._disp_total_heal)
        right = f'TOTAL {total_val} · HEAL {heal_val}'
        rw = self._tracked_text_width(draw, right, font, 0.85)
        self._draw_tracked(draw, (x_right - rw, fy + (fh - 12) // 2),
                           right, font, self.TEXT_MUTED, 0.85)

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

    def _make_bar(self, bw: int, bh: int, row: _RowState) -> Image.Image:
        if bw <= 0 or bh <= 0:
            return Image.new('RGBA', (1, 1), (0, 0, 0, 0))
        if row.is_self:
            ca, cb = (255, 210, 120, 92), (222, 166, 32, 8)
        elif row.heal_total > row.damage_total and row.heal_total > 0:
            ca, cb = self.BAR_HEAL_A, self.BAR_HEAL_B
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
        return out

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
                self.settings.set('dps_ov_x', int(self._x))
                self.settings.set('dps_ov_y', int(self._y))
                save = getattr(self.settings, 'save', None)
                if callable(save):
                    save()
            except Exception:
                pass
