# -*- coding: utf-8 -*-
"""Regression tests for SAO panel theme sync and restyling helpers."""

from __future__ import annotations

import _bootstrap  # noqa: F401

import sys
import types
import unittest

from gui_modules import sao_panel_ui as ui


class FakeWidget:
    def __init__(self, cls: str, children=None, exists: bool = True, **options):
        self._cls = cls
        self._children = list(children or [])
        self._exists = exists
        self.options = dict(options)
        self.configured = []

    def winfo_class(self):
        return self._cls

    def winfo_exists(self):
        return self._exists

    def winfo_children(self):
        return list(self._children)

    def cget(self, key):
        if key not in self.options:
            raise KeyError(key)
        return self.options[key]

    def configure(self, **kwargs):
        self.configured.append(kwargs)
        self.options.update(kwargs)


class BrokenChildrenWidget(FakeWidget):
    def winfo_children(self):
        raise RuntimeError("destroyed")


class PanelUiThemeTests(unittest.TestCase):
    def setUp(self) -> None:
        self._theme = ui._SAO_PANEL_THEME

    def tearDown(self) -> None:
        ui._set_sao_panel_theme(self._theme)
        sys.modules.pop("sao_gui.panel_ui_test", None)
        sys.modules.pop("not_sao_panel_ui_test", None)

    def test_sync_imported_constants_reaches_top_level_sao_modules(self) -> None:
        fake = types.ModuleType("sao_gui.panel_ui_test")
        fake._SAO_PANEL_BG = "stale"
        ignored = types.ModuleType("not_sao_panel_ui_test")
        ignored._SAO_PANEL_BG = "stale"
        sys.modules[fake.__name__] = fake
        sys.modules[ignored.__name__] = ignored

        ui._set_sao_panel_theme("dark")

        self.assertEqual(fake._SAO_PANEL_BG, ui._SAO_PANEL_BG)
        self.assertEqual(ignored._SAO_PANEL_BG, "stale")

    def test_style_descendants_handles_destroyed_children_and_checkbuttons(self) -> None:
        broken = BrokenChildrenWidget("Frame", bg=ui._SAO_PANEL_BODY_BG)
        check = FakeWidget(
            "Checkbutton",
            bg=ui._SAO_PANEL_HEADER_BG,
            fg=ui._SAO_PANEL_LABEL_FG,
        )
        radio = FakeWidget(
            "Radiobutton",
            bg=ui._SAO_PANEL_HEADER_BG,
            fg=ui._SAO_PANEL_LABEL_FG,
        )
        destroyed = FakeWidget("Button", exists=False)
        root = FakeWidget("Frame", [broken, check, radio, destroyed])

        ui._style_panel_descendants(root)

        self.assertEqual(check.options["bg"], ui._SAO_PANEL_BODY_BG)
        self.assertEqual(check.options["selectcolor"], ui._SAO_PANEL_HEADER_BG)
        self.assertEqual(check.options["activeforeground"], ui._SAO_PANEL_GOLD)
        self.assertEqual(radio.options["bg"], ui._SAO_PANEL_BODY_BG)
        self.assertEqual(destroyed.configured, [])


if __name__ == "__main__":
    unittest.main()
