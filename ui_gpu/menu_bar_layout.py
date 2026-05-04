"""Menu bar layout: 8-slot vertical column of fisheye circular buttons.

Renders to a single PIL.Image and exposes ``hit_rects`` so the
``HitTester`` can map cursor positions back to button indexes.
"""

from __future__ import annotations

from typing import List, Tuple

from PIL import Image

from sao_menu_hud import MenuCircleButtonRenderer
import _sao_cy_uihelpers as _CY_UI  # type: ignore[import-not-found]

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
    return _CY_UI.popup_visible_count(len(state.menu_items), MAX_VISIBLE)


def column_height(state) -> int:
    return _CY_UI.popup_column_height(len(state.menu_items), MAX_VISIBLE, SLOT)


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
    return bool(_CY_UI.popup_advance_menu_animation(
        state.btn_size, state.btn_hover_t, state.hover_btn_idx,
        len(state.menu_items), MAX_VISIBLE, float(SIZE), SIZE_EPS,
        SIZE_LERP, 0.25))


def hit_rects(state, x_off: int, y_off: int) -> List[Tuple[Tuple[int, int, int, int], int]]:
    """Return [((x1,y1,x2,y2), idx), ...] for each visible button slot."""
    return _CY_UI.popup_menu_hit_rects(len(state.menu_items), MAX_VISIBLE, SLOT, x_off, y_off)


def compose(state, bg_hex: str = '#010101') -> Image.Image:
    """Render the menu column as RGBA. Width = MAX_SIZE; height = SLOT*N."""
    n = visible_count(state)
    if n == 0:
        return Image.new('RGBA', (1, 1), _TRANSPARENT)
    img = Image.new('RGBA', (WIDTH, n * SLOT), _TRANSPARENT)
    active_idx = state.active_menu_idx
    for i in range(n):
        item = state.menu_items[i]
        size_f, size, ox, oy = _CY_UI.popup_menu_button_frame(
            state.btn_size[i] if i < len(state.btn_size) else SIZE,
            SLOT, MAX_SIZE, i)
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
        img.alpha_composite(sprite, (ox, oy))
    return img
