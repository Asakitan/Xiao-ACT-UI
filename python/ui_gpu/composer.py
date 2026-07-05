# Composes the full popup frame from layout sub-images.
#
# Produces premultiplied BGRA bytes ready for ``BgraPresenter``.

from __future__ import annotations

import time

from PIL import Image

from . import menu_bar_layout, child_bar_layout, hud_layout
from utils.perf_probe import probe as _probe

try:
    import _sao_cy_pixels as _CY_PIXELS  # type: ignore[import-not-found]
except ImportError:
    _CY_PIXELS = None  # type: ignore[assignment]
import _sao_cy_uihelpers as _CY_UI  # type: ignore[import-not-found]


# Layout offsets within the GPU window.  The window covers the menu_bar
# column + child_bar column + a HUD margin all the way around.
# The margin MUST equal the HUD sprite's own pad (hud_layout.sprite_pad,
# i.e. MenuHudSpriteRenderer.gpu_pad): the sprite is content + pad on
# every side with origin (-pad, -pad), so it is pasted at
# (HUD_PAD - pad, ...). Any HUD_PAD smaller than the sprite pad makes
# that offset negative — the max(0, px) clamp then shifts the whole
# HUD down-right AND PIL crops the sprite at the frame's right/bottom
# edge (glass plate corner cut square, right rail / bottom labels
# missing). A hardcoded 24 broke exactly this way when the sprite pad
# grew to 40 for the glass backdrop.
HUD_PAD = hud_layout.sprite_pad()
GAP_MENU_CHILD = 25                     # px between menu column and child column

MENU_X = HUD_PAD
CHILD_X = HUD_PAD + menu_bar_layout.WIDTH + GAP_MENU_CHILD

_HUD_FAIL_LOG_AT = 0.0  # HUD 层合成失败的 60s 限频日志哨兵


def content_shift(state) -> tuple[int, int]:
    # Small master slide used during popup open/close.
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
    # Return (content_w, content_h) — the inner box that excludes
    # the HUD margin.
    return _CY_UI.popup_content_size(
        len(state.menu_items), len(state.child_rows), menu_bar_layout.MAX_VISIBLE,
        menu_bar_layout.SLOT, child_bar_layout.ROW_STRIDE,
        menu_bar_layout.WIDTH, GAP_MENU_CHILD, child_bar_layout.WIDTH)


def window_size(state) -> tuple:
    iw, ih = content_size(state)
    return iw + HUD_PAD * 2, ih + HUD_PAD * 2


def window_size_reserved(state, reserved_rows: int) -> tuple:
    # Like ``window_size`` but sized for at least ``reserved_rows``
    # child rows. Used at open() time to bake in a fixed window size
    # big enough for the worst-case menu, so switching menus never
    # requires a GPU window resize.
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
        with _probe('ui.popup.compose_hud'):
            hud_img, (ox, oy) = hud_layout.compose(iw, ih, screen_w, screen_h, hud_phase)
        # origin is (-sprite_pad, -sprite_pad) and HUD_PAD == sprite_pad,
        # so the sprite lands at (dx, dy) covering the whole frame.
        px, py = HUD_PAD + ox + dx, HUD_PAD + oy + dy
        with _probe('ui.popup.paste_hud'):
            frame.alpha_composite(hud_img, (max(0, px), max(0, py)))
    except Exception as exc:
        # HUD 层缺席不该无声 — 60s 限频报一次, 其余帧继续静默降级
        global _HUD_FAIL_LOG_AT
        now = time.monotonic()
        if now - _HUD_FAIL_LOG_AT >= 60.0:
            _HUD_FAIL_LOG_AT = now
            print(f'[GPU] popup HUD layer compose failed (degraded frame): {exc}')

    # 2) Menu bar column
    with _probe('ui.popup.compose_menu_bar'):
        menu_img = menu_bar_layout.compose(state)
    if menu_img.size != (1, 1):
        with _probe('ui.popup.paste_menu_bar'):
            frame.alpha_composite(menu_img, menu_origin(state))

    # 3) Child bar column
    with _probe('ui.popup.compose_child_bar'):
        child_img = child_bar_layout.compose(state)
    if child_img.size != (1, 1):
        with _probe('ui.popup.paste_child_bar'):
            frame.alpha_composite(child_img, child_origin(state))

    return frame


def to_premultiplied_bgra(rgba: Image.Image, master_alpha: float = 1.0) -> bytes:
    data = rgba.tobytes()
    if _CY_PIXELS is not None:
        return _CY_PIXELS.premultiply_bgra_bytes_floor(
            data, rgba.height, rgba.width, master_alpha)
    import numpy as np
    arr = np.frombuffer(data, dtype=np.uint8).reshape(rgba.height, rgba.width, 4).copy()
    a = arr[..., 3].astype(np.uint16)
    if master_alpha < 0.999:
        a = np.clip((a * int(master_alpha * 255)) // 255, 0, 255).astype(np.uint16)
    out = np.empty_like(arr)
    out[..., 0] = ((arr[..., 2].astype(np.uint16) * a) // 255).astype(np.uint8)
    out[..., 1] = ((arr[..., 1].astype(np.uint16) * a) // 255).astype(np.uint8)
    out[..., 2] = ((arr[..., 0].astype(np.uint16) * a) // 255).astype(np.uint8)
    out[..., 3] = a.astype(np.uint8)
    return out.tobytes()
