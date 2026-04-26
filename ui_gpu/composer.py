"""Composes the full popup frame from layout sub-images.

Produces premultiplied BGRA bytes ready for ``BgraPresenter``.

"""

from __future__ import annotations

import os

import numpy as np
from PIL import Image

from . import menu_bar_layout, child_bar_layout, hud_layout

_CY_PIXELS = None
if os.environ.get('SAO_DISABLE_CYTHON', '').strip().lower() not in (
        '1', 'true', 'yes', 'on'):
    try:
        import _sao_cy_pixels as _CY_PIXELS  # type: ignore[import-not-found]
    except Exception:
        _CY_PIXELS = None


# Layout offsets within the GPU window.  The window covers the menu_bar
# column + child_bar column + a HUD margin all the way around.
HUD_PAD = 24                            # outer breathing room for HUD
GAP_MENU_CHILD = 25                     # px between menu column and child column

MENU_X = HUD_PAD
CHILD_X = HUD_PAD + menu_bar_layout.WIDTH + GAP_MENU_CHILD


def content_shift(state) -> tuple[int, int]:
    """Small master slide used during popup open/close."""
    alpha = max(0.0, min(1.0, float(getattr(state, 'fade_alpha', 1.0))))
    shift_x = -int(round((1.0 - alpha) * 14.0))
    shift_y = int(round((1.0 - alpha) * 10.0))
    return shift_x, shift_y


def menu_origin(state) -> tuple[int, int]:
    dx, dy = content_shift(state)
    return MENU_X + dx, HUD_PAD + dy


def child_origin(state) -> tuple[int, int]:
    dx, dy = content_shift(state)
    return CHILD_X + dx, HUD_PAD + dy


def content_size(state) -> tuple:
    """Return (content_w, content_h) — the inner box that excludes
    the HUD margin."""
    menu_h = menu_bar_layout.column_height(state)
    child_h = child_bar_layout.height_for(state)
    inner_h = max(menu_h, child_h, 1)
    inner_w = menu_bar_layout.WIDTH + GAP_MENU_CHILD + child_bar_layout.WIDTH
    return inner_w, inner_h


def window_size(state) -> tuple:
    iw, ih = content_size(state)
    return iw + HUD_PAD * 2, ih + HUD_PAD * 2


def window_size_reserved(state, reserved_rows: int) -> tuple:
    """Like ``window_size`` but sized for at least ``reserved_rows``
    child rows. Used at open() time to bake in a fixed window size
    big enough for the worst-case menu, so switching menus never
    requires a GPU window resize."""
    iw, ih = content_size(state)
    from sao_child_bar_gpu import ROW_STRIDE as _RS
    reserved_h = max(reserved_rows, 0) * _RS
    ih = max(ih, reserved_h, menu_bar_layout.column_height(state))
    return iw + HUD_PAD * 2, ih + HUD_PAD * 2


def compose_rgba(state, hud_phase: float, screen_w: int, screen_h: int,
                 reserved_rows: int = 0) -> Image.Image:
    iw, ih = content_size(state)
    if reserved_rows > 0:
        from sao_child_bar_gpu import ROW_STRIDE as _RS
        ih = max(ih, reserved_rows * _RS, menu_bar_layout.column_height(state))
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
    if _CY_PIXELS is not None:
        try:
            return _CY_PIXELS.premultiply_bgra_bytes_floor(
                data, rgba.height, rgba.width, master_alpha)
        except Exception:
            pass
    arr = np.frombuffer(data, dtype=np.uint8)
    arr = arr.reshape((rgba.height, rgba.width, 4))   # RGBA
    r = arr[..., 0].astype(np.uint16)
    g = arr[..., 1].astype(np.uint16)
    b = arr[..., 2].astype(np.uint16)
    a = arr[..., 3].astype(np.uint16)
    if master_alpha < 0.999:
        a = (a * max(0, min(255, int(master_alpha * 255)))) // 255
    # Premultiply
    pr = (r * a // 255).astype(np.uint8)
    pg = (g * a // 255).astype(np.uint8)
    pb = (b * a // 255).astype(np.uint8)
    pa = a.astype(np.uint8)
    out = np.empty_like(arr)
    out[..., 0] = pb       # BGRA byte order
    out[..., 1] = pg
    out[..., 2] = pr
    out[..., 3] = pa
    return out.tobytes()
