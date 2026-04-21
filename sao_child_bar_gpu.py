# -*- coding: utf-8 -*-
"""
sao_child_bar_gpu.py — GPU-presented child bar (popup submenu) for
SAOChildBar in `sao_theme.py`.

Phase 3+++ of the v2.3.0 overlay perf pass. Mirrors the same pattern
as `sao_menu_bar_gpu` and `sao_left_info_gpu`:

  * Tk widgets (the line/arrow Canvases, the row tk.Frames + Labels)
    stay alive for hit-testing (Enter/Leave/click bindings still
    fire). Their backgrounds are forced to chroma-key `#010101` so
    Tk paints nothing visible.
  * One GLFW transparent click-through window covers the full
    child-bar bounding box and presents the row strip via
    `BgraPresenter`.
  * The painter computes its own bbox from the snapshot (line column +
    arrow column + max row width) and walks the GpuOverlayWindow
    geometry on every dispatch so it tracks open/switch animations.

The compose closure uses pure PIL + ImageDraw (no Tk) and runs on the
shared `AsyncFrameWorker` lane. A signature dedup skips redundant
uploads when nothing visible changed.

Public API:

    gpu_child_bar_enabled() -> bool
    ChildBarGpuPainter(root)
        .tick(sx, sy, snap)        — dispatch one paint
        .clear()                   — present an empty frame
        .destroy()
"""

from __future__ import annotations

import os
import threading
import time
from typing import Any, List, Optional, Tuple

import tkinter as tk
from PIL import Image, ImageDraw, ImageFont

try:
    import gpu_overlay_window as _gow  # type: ignore[import-untyped]
except Exception:
    _gow = None  # type: ignore[assignment]

from overlay_render_worker import AsyncFrameWorker
from perf_probe import probe as _probe


# ═══════════════════════════════════════════════
#  Public env gate
# ═══════════════════════════════════════════════

def gpu_child_bar_enabled() -> bool:
    """Mirror SAO_GPU_OVERLAY when SAO_GPU_CHILD_BAR is unset."""
    if _gow is None:
        return False
    if not _gow.glfw_supported():
        return False
    raw = os.environ.get('SAO_GPU_CHILD_BAR')
    if raw is None:
        master = os.environ.get('SAO_GPU_OVERLAY')
        if master is None:
            return True
        return master.strip() in ('1', 'true', 'True', 'yes', 'on')
    return raw.strip() in ('1', 'true', 'True', 'yes', 'on')


# ═══════════════════════════════════════════════
#  Snapshots (worker-safe, no Tk)
# ═══════════════════════════════════════════════

class _RowSnapshot:
    __slots__ = ('icon', 'label', 'hover_t', 'row_w')

    def __init__(self, icon: str, label: str, hover_t: float, row_w: int):
        self.icon = icon
        self.label = label
        self.hover_t = max(0.0, min(1.0, float(hover_t)))
        self.row_w = max(1, int(row_w))


class _ChildBarSnapshot:
    __slots__ = ('line_w', 'line_h', 'arrow_w', 'fade_t', 'rows',
                 'bg_hex', 'colors')

    def __init__(self, line_w: int, line_h: int, arrow_w: int,
                 fade_t: float, rows: List[_RowSnapshot],
                 bg_hex: str, colors: 'BarColors'):
        self.line_w = max(1, int(line_w))
        self.line_h = max(1, int(line_h))
        self.arrow_w = max(1, int(arrow_w))
        self.fade_t = max(0.0, min(1.0, float(fade_t)))
        self.rows = rows
        self.bg_hex = bg_hex
        self.colors = colors


class BarColors:
    """Captures palette + lerp helper as plain values so the worker
    closure has no Tk/sao_theme dependency."""
    __slots__ = ('child_bg', 'child_hover', 'child_text', 'child_hover_fg',
                 'child_icon', 'active_border', 'lerp')

    def __init__(self, child_bg: str, child_hover: str, child_text: str,
                 child_hover_fg: str, child_icon: str, active_border: str,
                 lerp_color):
        self.child_bg = child_bg
        self.child_hover = child_hover
        self.child_text = child_text
        self.child_hover_fg = child_hover_fg
        self.child_icon = child_icon
        self.active_border = active_border
        self.lerp = lerp_color


# Layout constants — must match _create_item / _rebuild
ROW_H = 44
ROW_STRIDE = 47
ROW_X_INDICATOR_W = 2
ROW_X_PAD_L = 8
ROW_X_ICON_GAP = 5
ROW_X_PAD_R = 8
LINE_COL_W = 10
LINE_COL_PAD_R = 3
ARROW_COL_W = 12
ARROW_COL_PAD_R = 2
LINE_TOP_PAD = 5
ARROW_TOP_PAD = 5
ICON_FONT_SIZE = 12
LABEL_FONT_SIZE = 10
LIST_X = LINE_COL_W + LINE_COL_PAD_R + ARROW_COL_W + ARROW_COL_PAD_R  # 27


_font_tls = threading.local()
_FONT_SAO_PATH: Optional[str] = None
_FONT_CJK_PATH: Optional[str] = None


def _resolve_sao_path() -> str:
    global _FONT_SAO_PATH
    if _FONT_SAO_PATH is not None:
        return _FONT_SAO_PATH
    try:
        from sao_sound import get_sao_font as _gs
        path = _gs()
        if path:
            _FONT_SAO_PATH = path
            return path
    except Exception:
        pass
    _FONT_SAO_PATH = 'segoeui.ttf'
    return _FONT_SAO_PATH


def _resolve_cjk_path() -> str:
    global _FONT_CJK_PATH
    if _FONT_CJK_PATH is not None:
        return _FONT_CJK_PATH
    try:
        from sao_sound import get_cjk_font as _gc
        path = _gc()
        if path:
            _FONT_CJK_PATH = path
            return path
    except Exception:
        pass
    _FONT_CJK_PATH = 'msyh.ttc'
    return _FONT_CJK_PATH


def _icon_font_cached(size: int) -> ImageFont.FreeTypeFont:
    cache = getattr(_font_tls, 'icon', None)
    if cache is None:
        cache = {}
        _font_tls.icon = cache
    f = cache.get(size)
    if f is None:
        try:
            f = ImageFont.truetype(_resolve_sao_path(), size=size)
        except Exception:
            try:
                f = ImageFont.truetype('segoeui.ttf', size=size)
            except Exception:
                f = ImageFont.load_default()
        cache[size] = f
    return f


def _label_font_cached(size: int) -> ImageFont.FreeTypeFont:
    cache = getattr(_font_tls, 'label', None)
    if cache is None:
        cache = {}
        _font_tls.label = cache
    f = cache.get(size)
    if f is None:
        try:
            f = ImageFont.truetype(_resolve_cjk_path(), size=size)
        except Exception:
            try:
                f = ImageFont.truetype('arial.ttf', size=size)
            except Exception:
                f = ImageFont.load_default()
        cache[size] = f
    return f


# ═══════════════════════════════════════════════
#  Compose helper (worker-safe)
# ═══════════════════════════════════════════════

def _compose_child_bar(snap: _ChildBarSnapshot, total_w: int, total_h: int) -> Image.Image:
    img = Image.new('RGBA', (total_w, total_h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    colors = snap.colors
    bg = snap.bg_hex
    fade_t = snap.fade_t

    # Line column
    line_h = snap.line_h
    line_w = max(1, snap.line_w)
    if line_w > 1 and line_h > 5:
        glow_color = colors.lerp('#d4d0d0', bg, fade_t)
        main_color = colors.lerp('#9c9999', bg, fade_t)
        dot_color = colors.lerp('#b0b0b0', bg, fade_t)
        cx = LINE_COL_W // 2
        ly = LINE_TOP_PAD
        draw.line((cx, ly + 5, cx, ly + line_h - 5), fill=glow_color, width=4)
        draw.line((cx, ly + 5, cx, ly + line_h - 5), fill=main_color, width=2)
        draw.ellipse((cx - 2, ly + 3, cx + 2, ly + 7), fill=dot_color)
        draw.ellipse((cx - 2, ly + line_h - 7, cx + 2, ly + line_h - 3),
                     fill=dot_color)

    # Arrow column
    arrow_x = LINE_COL_W + LINE_COL_PAD_R
    arrow_w = max(1, snap.arrow_w)
    if arrow_w > 1 and line_h > 5:
        ay = ARROW_TOP_PAD
        cx = 6
        cy = ay + line_h // 2
        for gr in range(6, 0, -2):
            ga = int(15 * (1 - gr / 6))
            base_hex = '#%02x%02x%02x' % (
                int(ga * 3.5) & 0xFF,
                int(ga * 2.2) & 0xFF,
                int(ga * 0.3) & 0xFF,
            )
            faded = colors.lerp(base_hex, bg, fade_t)
            draw.ellipse((arrow_x + cx - gr, cy - gr,
                          arrow_x + cx + gr, cy + gr),
                         fill=faded)
        core_fill = colors.lerp('#c9b896', bg, fade_t)
        core_outline = colors.lerp('#d4c8a8', bg, fade_t)
        draw.ellipse((arrow_x + 4, cy - 2, arrow_x + 9, cy + 3),
                     fill=core_fill, outline=core_outline)

    # Rows
    icon_font = _icon_font_cached(ICON_FONT_SIZE)
    label_font = _label_font_cached(LABEL_FONT_SIZE)

    for idx, row in enumerate(snap.rows):
        row_y = idx * ROW_STRIDE
        ht = row.hover_t
        rw = row.row_w
        if rw <= 1:
            continue
        bg_now = colors.lerp(colors.child_bg, colors.child_hover, ht)
        fg_now = colors.lerp(colors.child_text, colors.child_hover_fg, ht)
        icon_now = colors.lerp(colors.child_icon, colors.child_hover_fg, ht)
        ind_now = colors.lerp(colors.child_bg, colors.active_border, ht)
        bg_blend = colors.lerp(bg_now, bg, fade_t)
        fg_blend = colors.lerp(fg_now, bg, fade_t)
        icon_blend = colors.lerp(icon_now, bg, fade_t)
        ind_blend = colors.lerp(ind_now, bg, fade_t)

        draw.rectangle((LIST_X, row_y, LIST_X + rw, row_y + ROW_H),
                       fill=bg_blend)
        draw.rectangle((LIST_X, row_y, LIST_X + ROW_X_INDICATOR_W, row_y + ROW_H),
                       fill=ind_blend)

        ic_x = LIST_X + ROW_X_INDICATOR_W + ROW_X_PAD_L
        ic_y = row_y + (ROW_H - ICON_FONT_SIZE) // 2 - 2
        if row.icon:
            try:
                draw.text((ic_x, ic_y), row.icon, fill=icon_blend, font=icon_font)
            except Exception:
                pass
        try:
            ic_w = int(draw.textlength(row.icon or '', font=icon_font))
        except Exception:
            ic_w = ICON_FONT_SIZE
        lbl_x = ic_x + ic_w + ROW_X_ICON_GAP
        lbl_y = row_y + (ROW_H - LABEL_FONT_SIZE) // 2 - 2
        max_lbl_w = max(0, LIST_X + rw - lbl_x - ROW_X_PAD_R - 6)
        if row.label and max_lbl_w > 4:
            try:
                draw.text((lbl_x, lbl_y), row.label, fill=fg_blend, font=label_font)
            except Exception:
                pass
        if ht > 0.05:
            arr_x = LIST_X + rw - ROW_X_PAD_R - 6
            arr_y = row_y + (ROW_H - 14) // 2 - 1
            arr_color = colors.lerp(colors.child_bg, colors.child_hover_fg, ht)
            arr_blend = colors.lerp(arr_color, bg, fade_t)
            try:
                draw.text((arr_x, arr_y), '\u203a', fill=arr_blend, font=label_font)
            except Exception:
                pass

    return img


# ═══════════════════════════════════════════════
#  Painter
# ═══════════════════════════════════════════════

class ChildBarGpuPainter:
    """Owns one GpuOverlayWindow + AsyncFrameWorker for the child bar."""

    def __init__(self, root: tk.Misc):
        self._root = root
        self._render_worker = AsyncFrameWorker(prefer_isolation=True)
        self._gpu_window: Optional[Any] = None
        self._presenter: Optional[Any] = None
        self._destroyed = False
        self._last_sig: Optional[tuple] = None
        self._last_geom: Optional[Tuple[int, int, int, int]] = None
        self._lock = threading.Lock()

    def _ensure_window(self, w: int, h: int, x: int, y: int) -> bool:
        if self._gpu_window is not None:
            return True
        if _gow is None or not _gow.glfw_supported():
            return False
        try:
            pump = _gow.get_glfw_pump(self._root)
            self._presenter = _gow.BgraPresenter()
            self._gpu_window = _gow.GpuOverlayWindow(
                pump,
                w=max(1, int(w)), h=max(1, int(h)),
                x=int(x), y=int(y),
                render_fn=self._presenter.render,
                click_through=True,
                title='sao_child_bar_gpu',
            )
            self._gpu_window.show()
            return True
        except Exception:
            self._presenter = None
            self._gpu_window = None
            return False

    def destroy(self) -> None:
        if self._destroyed:
            return
        self._destroyed = True
        try:
            self._render_worker.stop()
        except Exception:
            pass
        if self._presenter is not None:
            try:
                self._presenter.release()
            except Exception:
                pass
            self._presenter = None
        if self._gpu_window is not None:
            try:
                self._gpu_window.destroy()
            except Exception:
                pass
            self._gpu_window = None

    def clear(self) -> None:
        if self._destroyed:
            return
        self._last_sig = None
        if self._presenter is not None:
            try:
                self._presenter.clear()
            except Exception:
                pass
        if self._gpu_window is not None:
            try:
                self._gpu_window.request_redraw()
            except Exception:
                pass

    @_probe.decorate('ui.menu.child_bar_gpu_tick')
    def tick(self, screen_x: int, screen_y: int,
             snap: _ChildBarSnapshot) -> None:
        if self._destroyed:
            return
        if not snap.rows:
            self.clear()
            return

        max_row_w = max(r.row_w for r in snap.rows)
        out_w = max(1, LIST_X + max_row_w)
        rows_h = len(snap.rows) * ROW_STRIDE - 3
        line_h_pad = LINE_TOP_PAD + snap.line_h
        out_h = max(rows_h, line_h_pad)
        if out_w < 4 or out_h < 4:
            return
        if not self._ensure_window(out_w, out_h, screen_x, screen_y):
            return

        # 1) Drain previous result and present.
        fb = self._render_worker.take_result(allow_during_capture=True)
        if fb is not None and self._gpu_window is not None and self._presenter is not None:
            try:
                geom = (fb.x, fb.y, fb.width, fb.height)
                if geom != self._last_geom:
                    self._gpu_window.set_geometry(*geom)
                    self._last_geom = geom
                self._presenter.set_frame(fb.bgra_bytes, fb.width, fb.height)
                self._gpu_window.request_redraw()
            except Exception:
                pass

        # 2) Build dedup signature.
        rows_sig = tuple(
            (r.icon, r.label, int(r.hover_t * 16), r.row_w)
            for r in snap.rows
        )
        sig = (
            out_w, out_h,
            snap.line_w, snap.line_h, snap.arrow_w,
            int(snap.fade_t * 16),
            snap.bg_hex,
            rows_sig,
        )
        with self._lock:
            if sig == self._last_sig:
                return
            self._last_sig = sig

        # 3) Submit compose. Capture state by value.
        s = snap
        out_w_local = out_w
        out_h_local = out_h

        def compose(_now: float) -> Image.Image:
            return _compose_child_bar(s, out_w_local, out_h_local)

        try:
            self._render_worker.submit(
                compose, time.perf_counter(),
                0, int(screen_x), int(screen_y),
            )
        except Exception:
            pass
