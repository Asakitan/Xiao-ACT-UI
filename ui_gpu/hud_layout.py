"""HUD layout: brackets, rails, scan line, dots, clock stamp.

Wraps ``MenuHudSpriteRenderer`` from ``sao_menu_hud`` so we get a single
RGBA frame plus a sprite origin offset relative to the content rect.
"""

from __future__ import annotations

from typing import Tuple

from PIL import Image

from sao_menu_hud import MenuHudSpriteRenderer

_renderer = MenuHudSpriteRenderer()


def compose(content_w: int, content_h: int,
            screen_w: int, screen_h: int,
            phase: float) -> Tuple[Image.Image, Tuple[int, int]]:
    """Return (rgba_image, (origin_dx, origin_dy)) where the sprite is
    drawn at ``(content_x + origin_dx, content_y + origin_dy)``."""
    img, origin = _renderer.render_pil(content_w, content_h,
                                       screen_w, screen_h, phase)
    return img, origin


def reset() -> None:
    _renderer.reset()
