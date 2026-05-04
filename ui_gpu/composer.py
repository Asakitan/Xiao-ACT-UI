"""Composes the full popup frame from layout sub-images.

Produces premultiplied BGRA bytes ready for ``BgraPresenter``.
"""

from __future__ import annotations

from PIL import Image

from . import menu_bar_layout, child_bar_layout, hud_layout

import _sao_cy_pixels as _CY_PIXELS  # type: ignore[import-not-found]
import _sao_cy_uihelpers as _CY_UI  # type: ignore[import-not-found]


# Layout offsets within the GPU window.  The window covers the menu_bar
# column + child_bar column + a HUD margin all the way around.
HUD_PAD = 24                            # outer breathing room for HUD
GAP_MENU_CHILD = 25                     # px between menu column and child column

MENU_X = HUD_PAD
CHILD_X = HUD_PAD + menu_bar_layout.WIDTH + GAP_MENU_CHILD


def content_shift(state) -> tuple[int, int]:
    """Small master slide used during popup open/close."""
    return _CY_UI.popup_content_shift(getattr(state, 'fade_alpha', 1.0))


def menu_origin(state) -> tuple[int, int]:
    menu, _child = _CY_UI.popup_origins(
        getattr(state, 'fade_alpha', 1.0), HUD_PAD, MENU_X, CHILD_X)
    return menu


def child_origin(state) -> tuple[int, int]:
    _menu, child = _CY_UI.popup_origins(
        getattr(state, 'fade_alpha', 1.0), HUD_PAD, MENU_X, CHILD_X)
    return child


def content_size(state) -> tuple:
    """Return (content_w, content_h) — the inner box that excludes
    the HUD margin."""
    return _CY_UI.popup_content_size(
        len(state.menu_items), len(state.child_rows), menu_bar_layout.MAX_VISIBLE,
        menu_bar_layout.SLOT, child_bar_layout.ROW_STRIDE,
        menu_bar_layout.WIDTH, GAP_MENU_CHILD, child_bar_layout.WIDTH)


def window_size(state) -> tuple:
    iw, ih = content_size(state)
    return iw + HUD_PAD * 2, ih + HUD_PAD * 2


def window_size_reserved(state, reserved_rows: int) -> tuple:
    """Like ``window_size`` but sized for at least ``reserved_rows``
    child rows. Used at open() time to bake in a fixed window size
    big enough for the worst-case menu, so switching menus never
    requires a GPU window resize."""
    return _CY_UI.popup_window_size(
        len(state.menu_items), len(state.child_rows), reserved_rows,
        menu_bar_layout.MAX_VISIBLE, menu_bar_layout.SLOT,
        child_bar_layout.ROW_STRIDE, menu_bar_layout.WIDTH,
        GAP_MENU_CHILD, child_bar_layout.WIDTH, HUD_PAD)


def compose_rgba(state, hud_phase: float, screen_w: int, screen_h: int,
                 reserved_rows: int = 0) -> Image.Image:
    iw, ih = content_size(state)
    if reserved_rows > 0:
        _win_w, win_h = window_size_reserved(state, reserved_rows)
        ih = max(1, win_h - HUD_PAD * 2)
    win_w, win_h = iw + HUD_PAD * 2, ih + HUD_PAD * 2
    frame = Image.new('RGBA', (win_w, win_h), (0, 0, 0, 0))
    dx, dy = content_shift(state)

    # 1) HUD layer (brackets/rails/scan/dots/stamp). Sprite origin is
    #    relative to the *content* top-left, which is (HUD_PAD, HUD_PAD).
    try:
        hud_img, (ox, oy) = hud_layout.compose(iw, ih, screen_w, screen_h, hud_phase)
        # origin is (-PLATE_PAD, -PLATE_PAD) which lines up with HUD_PAD
        # if we paste the sprite at (HUD_PAD + ox, HUD_PAD + oy).
        px, py = HUD_PAD + ox + dx, HUD_PAD + oy + dy
        frame.alpha_composite(hud_img, (max(0, px), max(0, py)))
    except Exception:
        pass

    # 2) Menu bar column
    menu_img = menu_bar_layout.compose(state)
    if menu_img.size != (1, 1):
        frame.alpha_composite(menu_img, menu_origin(state))

    # 3) Child bar column
    child_img = child_bar_layout.compose(state)
    if child_img.size != (1, 1):
        frame.alpha_composite(child_img, child_origin(state))

    return frame


def to_premultiplied_bgra(rgba: Image.Image, master_alpha: float = 1.0) -> bytes:
    data = rgba.tobytes()
    return _CY_PIXELS.premultiply_bgra_bytes_floor(
        data, rgba.height, rgba.width, master_alpha)
