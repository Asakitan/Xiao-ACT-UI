"""Menu bar layout: 8-slot vertical column of fisheye circular buttons.

Renders to a single PIL.Image and exposes ``hit_rects`` so the
``HitTester`` can map cursor positions back to button indexes.

"""

from __future__ import annotations

import math
from typing import List, Tuple

from PIL import Image

from sao_menu_hud import MenuCircleButtonRenderer

# Mirror legacy SAOCircleButton constants
SIZE = 54
MAX_SIZE = 70
SLOT = 70                       # vertical slot per button (= MAX_SIZE)
MAX_VISIBLE = 7
WIDTH = MAX_SIZE                # column width

# Animation tuning
HOVER_LERP = 0.28               # per-tick hover_t blend toward target
SIZE_LERP = 0.28                # per-tick fisheye size blend
SIZE_EPS = 0.18

_renderer = MenuCircleButtonRenderer()


def visible_count(state) -> int:
    return min(len(state.menu_items), MAX_VISIBLE)


def column_height(state) -> int:
    return SLOT * max(1, visible_count(state))


def _color_lerp(c1, c2, t):
    # tiny inline lerp_color to avoid pulling sao_theme into ui_gpu
    h1 = c1.lstrip('#')
    h2 = c2.lstrip('#')
    r1, g1, b1 = int(h1[0:2], 16), int(h1[2:4], 16), int(h1[4:6], 16)
    r2, g2, b2 = int(h2[0:2], 16), int(h2[2:4], 16), int(h2[4:6], 16)
    return '#{:02x}{:02x}{:02x}'.format(
        int(r1 + (r2 - r1) * t),
        int(g1 + (g2 - g1) * t),
        int(b1 + (b2 - b1) * t),
    )


# SAO palette (mirrors SAOColors; duplicated to avoid import cycle)
_BORDER = '#bcc4ca'
_BG = '#f7f8f8'
_ICON = '#959aa0'
_ACTIVE_BORDER = '#f3af12'
_ACTIVE_BG = '#f4ebd7'
_ACTIVE_ICON = '#6d5d40'
_HOVER_BG = '#edf7fa'
_HOVER_ICON = '#718995'
_TRANSPARENT = (0, 0, 0, 0)


def advance_animation(state) -> bool:
    """Tick fisheye + hover lerp toward targets. Returns True if still
    animating (caller should re-render next frame)."""
    n = visible_count(state)
    if n == 0:
        return False
    # ensure per-button arrays match
    while len(state.btn_size) < n:
        state.btn_size.append(float(SIZE))
    while len(state.btn_hover_t) < n:
        state.btn_hover_t.append(0.0)
    keep = False
    hover_idx = state.hover_btn_idx
    for i in range(n):
        # fisheye target: hovered button gets +22% with exp falloff to neighbours
        if hover_idx is not None:
            dist = abs(hover_idx - i)
            target = SIZE * (1.0 + 0.22 * math.exp(-0.9 * dist * dist))
        else:
            target = float(SIZE)
        delta = target - state.btn_size[i]
        if abs(delta) > SIZE_EPS:
            state.btn_size[i] += delta * SIZE_LERP
            keep = True
        else:
            state.btn_size[i] = target
        # hover_t lerps to 1 if hovered, else 0
        ht_target = 1.0 if (hover_idx == i) else 0.0
        ht_delta = ht_target - state.btn_hover_t[i]
        if abs(ht_delta) > 0.01:
            state.btn_hover_t[i] += ht_delta * 0.25
            keep = True
        else:
            state.btn_hover_t[i] = ht_target
    return keep


def hit_rects(state, x_off: int, y_off: int) -> List[Tuple[Tuple[int, int, int, int], int]]:
    """Return [((x1,y1,x2,y2), idx), ...] for each visible button slot."""
    n = visible_count(state)
    out = []
    for i in range(n):
        x1 = x_off
        y1 = y_off + i * SLOT
        x2 = x1 + SLOT
        y2 = y1 + SLOT
        out.append(((x1, y1, x2, y2), i))
    return out


def compose(state, bg_hex: str = '#010101') -> Image.Image:
    """Render the menu column as RGBA. Width = MAX_SIZE; height = SLOT*N."""
    n = visible_count(state)
    if n == 0:
        return Image.new('RGBA', (1, 1), _TRANSPARENT)
    img = Image.new('RGBA', (WIDTH, n * SLOT), _TRANSPARENT)
    active_idx = state.active_menu_idx
    for i in range(n):
        item = state.menu_items[i]
        size_f = max(1.0, min(float(MAX_SIZE), float(state.btn_size[i] if i < len(state.btn_size) else SIZE)))
        size = max(1, int(math.ceil(size_f)))
        is_active = (active_idx == i)
        t = state.btn_hover_t[i] if i < len(state.btn_hover_t) else 0.0
        if is_active:
            border, inner, icon_c = _ACTIVE_BORDER, _ACTIVE_BG, _ACTIVE_ICON
        else:
            border = _color_lerp(_BORDER, _ACTIVE_BORDER, t)
            inner = _color_lerp(_BG, _HOVER_BG, t)
            icon_c = _color_lerp(_ICON, _HOVER_ICON, t)
        # MenuCircleButtonRenderer returns RGBA with transparent corners;
        # bg_hex is only used for cache keying (legacy chroma path).
        sprite = _renderer.render(size, item.get('icon', '●') or '●',
                                  border, inner, icon_c, bg_hex)
        ox = int(round((SLOT - size_f) / 2.0))
        oy = int(round(i * SLOT + (SLOT - size_f) / 2.0))
        img.alpha_composite(sprite, (ox, oy))
    return img
