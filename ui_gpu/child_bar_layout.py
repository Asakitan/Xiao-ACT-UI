"""Child bar layout: rows + connecting line + arrow indicator.

Reuses the existing PIL composer ``_compose_child_bar`` from
``sao_child_bar_gpu`` so we don't duplicate ~200 LOC of font fallback +
row composition.
"""

from __future__ import annotations

from typing import List, Tuple

from PIL import Image

from sao_child_bar_gpu import (
    _compose_child_bar, _ChildBarSnapshot, _RowSnapshot, BarColors,
    ROW_STRIDE, ROW_H, LIST_X,
)
import _sao_cy_uihelpers as _CY_UI  # type: ignore[import-not-found]

# Column dimensions
TARGET_ROW_W = 240   # full slide-in width
ARROW_COL_TOTAL = 12 + 2  # ARROW_COL_W + ARROW_COL_PAD_R
LINE_COL_TOTAL = 10 + 3   # LINE_COL_W + LINE_COL_PAD_R

WIDTH = LIST_X + TARGET_ROW_W   # 27 + 240 = 267


# Palette (mirrors SAOColors child-* values)
_COLORS = BarColors(
    child_bg='#f8f8f8',
    child_hover='#f4eee1',
    child_text='#646364',
    child_hover_fg='#625846',
    child_icon='#8f959b',
    active_border='#f3af12',
    lerp_color=None,  # filled below
)


def _lerp_color(c1: str, c2: str, t: float) -> str:
    h1 = c1.lstrip('#')[:6]
    h2 = c2.lstrip('#')[:6]
    if len(h1) != 6 or len(h2) != 6:
        return c1
    r1, g1, b1 = int(h1[0:2], 16), int(h1[2:4], 16), int(h1[4:6], 16)
    r2, g2, b2 = int(h2[0:2], 16), int(h2[2:4], 16), int(h2[4:6], 16)
    return '#{:02x}{:02x}{:02x}'.format(
        int(r1 + (r2 - r1) * t),
        int(g1 + (g2 - g1) * t),
        int(b1 + (b2 - b1) * t),
    )


_COLORS.lerp = _lerp_color


def height_for(state) -> int:
    return _CY_UI.popup_child_height(len(state.child_rows), ROW_STRIDE)


def advance_animation(state, now: float) -> bool:
    """Tick row hover lerp + slide-in width. Returns True if still
    animating."""
    return bool(_CY_UI.popup_advance_child_animation(
        state.row_hover_t, state.row_anim_w, state.hover_row_idx,
        len(state.child_rows), now, state.row_anim_t0,
        float(TARGET_ROW_W), 0.32, 0.05, 0.25))


def hit_rects(state, x_off: int, y_off: int) -> List[Tuple[Tuple[int, int, int, int], int]]:
    return _CY_UI.popup_child_hit_rects(
        len(state.child_rows), state.row_anim_w, x_off, y_off,
        LIST_X, ROW_STRIDE, ROW_H, TARGET_ROW_W)


def compose(state) -> Image.Image:
    n = len(state.child_rows)
    if n == 0:
        return Image.new('RGBA', (1, 1), (0, 0, 0, 0))
    h = height_for(state)
    line_h = max(1, n * ROW_STRIDE - 3)
    rows = []
    for i, item in enumerate(state.child_rows):
        rw = state.row_anim_w[i] if i < len(state.row_anim_w) else TARGET_ROW_W
        ht = state.row_hover_t[i] if i < len(state.row_hover_t) else 0.0
        rows.append(_RowSnapshot(
            item.get('icon', '') or '',
            item.get('label', '') or '',
            ht, rw,
        ))
    snap = _ChildBarSnapshot(
        line_w=10, line_h=line_h, arrow_w=12,
        fade_t=max(0.0, min(1.0, float(getattr(state, 'child_fade_t', 0.0)))), rows=rows,
        bg_hex='#010101', colors=_COLORS,
    )
    return _compose_child_bar(snap, WIDTH, h)
