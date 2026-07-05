# -*- coding: utf-8 -*-
# Numeric guard checks for the Tk plugin UI renderer.

from __future__ import annotations

import _bootstrap  # noqa: F401

from pathlib import Path
import unittest

from gui_modules.sao_plugin_ui_render import _draw_canvas_ops, _finite_float, _finite_int


PAL = {
    "body": "#111111",
    "border": "#222222",
    "sep": "#333333",
    "label": "#444444",
    "value": "#555555",
    "gold": "#666666",
    "accent": "#777777",
    "header_bg": "#888888",
    "header_fg": "#999999",
}


class FakeCanvas:
    def __init__(self) -> None:
        self.calls: list[tuple[str, tuple, dict]] = []

    def delete(self, *args: object) -> None:
        self.calls.append(("delete", args, {}))

    def create_rectangle(self, *args: object, **kwargs: object) -> None:
        self.calls.append(("rect", args, kwargs))

    def create_oval(self, *args: object, **kwargs: object) -> None:
        self.calls.append(("oval", args, kwargs))

    def create_line(self, *args: object, **kwargs: object) -> None:
        self.calls.append(("line", args, kwargs))

    def create_text(self, *args: object, **kwargs: object) -> None:
        self.calls.append(("text", args, kwargs))


class PluginUiNumericTests(unittest.TestCase):
    def test_renderer_numeric_paths_use_finite_helpers(self) -> None:
        source = (Path(__file__).resolve().parents[1] / "gui_modules" / "sao_plugin_ui_render.py").read_text(encoding="utf-8")

        self.assertNotIn("float(ns.get(\"pct\") or 0.0)", source)
        self.assertNotIn("int(ns.get(\"width\") or 1)", source)
        self.assertNotIn("int(op.get(\"x\", 0))", source)
        self.assertIn("pct = _finite_float(ns.get(\"pct\"), 0.0, lo=0.0, hi=1.0)", source)
        self.assertIn("w, h = _finite_int(ns.get(\"width\"), 1, lo=1)", source)
        self.assertEqual(_finite_float(float("nan"), 0.25, lo=0.0, hi=1.0), 0.25)
        self.assertEqual(_finite_float("bad", 0.5, lo=0.0, hi=1.0), 0.5)
        self.assertEqual(_finite_int(float("inf"), 12, lo=1), 12)

    def test_canvas_ops_filter_non_finite_numbers(self) -> None:
        canvas = FakeCanvas()

        _draw_canvas_ops(canvas, {
            "ops": [
                {
                    "op": "rect",
                    "x": "bad",
                    "y": float("nan"),
                    "w": "12",
                    "h": float("inf"),
                    "outline": "accent",
                    "width": "bad",
                },
                {"op": "line", "x1": "bad", "y1": 2, "x2": float("inf"), "y2": 4, "width": "bad"},
                {"op": "text", "x": float("nan"), "y": "bad", "size": float("nan"), "text": "ok"},
            ],
        }, PAL)

        rect = next(call for call in canvas.calls if call[0] == "rect")
        line = next(call for call in canvas.calls if call[0] == "line")
        text = next(call for call in canvas.calls if call[0] == "text")
        self.assertEqual(rect[1], (0, 0, 12, 0))
        self.assertEqual(rect[2]["width"], 1)
        self.assertEqual(line[1], (0, 2, 0, 4))
        self.assertEqual(line[2]["width"], 1)
        self.assertEqual(text[1], (0, 0))
        self.assertEqual(text[2]["font"], ("Segoe UI", 10, "normal"))


if __name__ == "__main__":
    unittest.main()
