# -*- coding: utf-8 -*-
"""Regression tests for Entity plugin renderer compatibility paths."""

from __future__ import annotations

import _bootstrap  # noqa: F401

import unittest

from gui_modules import sao_panel_ui as panel_ui
from gui_modules.sao_gui_plugin_manager import PluginDetachedPanel
from gui_modules.sao_plugin_ui_render import _RNode, _pal, SpecRenderer


class FakeVar:
    def __init__(self, value: str = "") -> None:
        self.value = value

    def get(self) -> str:
        return self.value

    def set(self, value: str) -> None:
        self.value = value


class FakeEntry:
    def __init__(self) -> None:
        self.configs: list[dict] = []
        self.pack_configs: list[dict] = []
        self._focused = False

    def config(self, **kwargs) -> None:
        self.configs.append(kwargs)

    def pack_configure(self, **kwargs) -> None:
        self.pack_configs.append(kwargs)

    def focus_get(self):
        return self if self._focused else None

    def register(self, callback):
        return callback

    def winfo_exists(self) -> bool:
        return True


class PluginRendererCompatTests(unittest.TestCase):
    def test_input_update_propagates_placeholder_type_and_width(self) -> None:
        renderer = SpecRenderer.__new__(SpecRenderer)
        entry = FakeEntry()
        var = FakeVar("old hint")
        rn = _RNode(
            "input",
            entry,
            spec={"type": "input", "id": "field", "value": "", "placeholder": "old hint"},
        )
        rn.parts = {
            "var": var,
            "id": "field",
            "placeholder": "old hint",
            "itype": "text",
            "is_placeholder": True,
            "last_seed": "",
        }

        SpecRenderer._update_input(
            renderer,
            rn,
            {
                "type": "input",
                "id": "field",
                "value": "",
                "placeholder": "new hint",
                "input_type": "text",
                "width": 0,
            },
            _pal(),
        )

        self.assertEqual(var.get(), "new hint")
        self.assertTrue(rn.parts["is_placeholder"])

        SpecRenderer._update_input(
            renderer,
            rn,
            {
                "type": "input",
                "id": "field",
                "value": "",
                "placeholder": "secret",
                "input_type": "password",
                "width": 96,
            },
            _pal(),
        )

        self.assertEqual(var.get(), "")
        self.assertFalse(rn.parts["is_placeholder"])
        self.assertEqual(rn.parts["itype"], "password")
        self.assertIn({"show": "•"}, entry.configs)
        self.assertIn({"width": 12}, entry.configs)
        self.assertEqual(entry.pack_configs[-1], {"pady": 2, "fill": "none"})

        SpecRenderer._update_input(
            renderer,
            rn,
            {
                "type": "input",
                "id": "field",
                "value": "",
                "placeholder": "loose",
                "input_type": "text",
                "width": "auto",
            },
            _pal(),
        )

        self.assertEqual(var.get(), "loose")
        self.assertEqual(entry.pack_configs[-1], {"fill": "x", "pady": 2})

    def test_detached_panel_signature_tracks_theme(self) -> None:
        panel = PluginDetachedPanel.__new__(PluginDetachedPanel)
        original = panel_ui._SAO_PANEL_THEME
        try:
            panel_ui._set_sao_panel_theme("light")
            light_sig = panel._render_signature({"version": 1, "nodes": []})
            panel_ui._set_sao_panel_theme("dark")
            dark_sig = panel._render_signature({"version": 1, "nodes": []})
        finally:
            panel_ui._set_sao_panel_theme(original)

        self.assertNotEqual(light_sig, dark_sig)


if __name__ == "__main__":
    unittest.main()
