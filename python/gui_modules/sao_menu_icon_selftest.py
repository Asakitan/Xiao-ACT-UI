# -*- coding: utf-8 -*-
"""Pixel-level smoke tests for procedural SAO menu plugin icons."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path


_PY_ROOT = Path(__file__).resolve().parents[1]
if str(_PY_ROOT) not in sys.path:
    sys.path.insert(0, str(_PY_ROOT))

try:
    from PIL import Image, ImageDraw
except Exception:  # pragma: no cover - PIL is required by the menu renderer
    Image = None  # type: ignore[assignment]
    ImageDraw = None  # type: ignore[assignment]

from gui_modules.sao_menu_hud import MenuCircleButtonRenderer


@unittest.skipIf(Image is None or ImageDraw is None, "PIL is unavailable")
class MenuCirclePluginIconTests(unittest.TestCase):
    def test_plugin_builtin_icons_are_visible_and_centered(self) -> None:
        renderer = MenuCircleButtonRenderer()
        for icon in ("◷", "♀", "◥", "▣"):
            for size in (20, 54, 70):
                with self.subTest(icon=icon, size=size):
                    scale = 4 if size <= 20 else 3
                    canvas = size * scale
                    image = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
                    draw = ImageDraw.Draw(image)
                    drawn = renderer._draw_builtin_icon(
                        draw,
                        icon,
                        canvas,
                        (255, 255, 255, 255),
                        scale,
                    )

                    self.assertTrue(drawn)
                    alpha = image.getchannel("A")
                    bbox = alpha.getbbox()
                    self.assertIsNotNone(bbox)
                    if bbox is None:
                        continue
                    left, top, right, bottom = bbox
                    visible = sum(1 for value in alpha.tobytes() if value > 8)
                    self.assertGreater(visible, int(canvas * canvas * 0.010))
                    self.assertLess(visible, int(canvas * canvas * 0.180))
                    center_x = (left + right) / 2.0
                    center_y = (top + bottom) / 2.0
                    self.assertLess(abs(center_x - canvas / 2.0), canvas * 0.10)
                    self.assertLess(abs(center_y - canvas / 2.0), canvas * 0.12)
                    self.assertGreater(right - left, canvas * 0.34)
                    self.assertGreater(bottom - top, canvas * 0.34)

    def test_rendered_plugin_buttons_keep_stable_size(self) -> None:
        renderer = MenuCircleButtonRenderer()
        for icon in ("◷", "♀", "◥", "▣"):
            for size in (20, 54, 70):
                with self.subTest(icon=icon, size=size):
                    image = renderer.render(
                        size,
                        icon,
                        "#7dd3fc",
                        "#07111f",
                        "#f8fafc",
                        "#010101",
                    )
                    self.assertEqual(image.size, (size, size))
                    self.assertIsNotNone(image.getchannel("A").getbbox())


if __name__ == "__main__":
    unittest.main(verbosity=2)
