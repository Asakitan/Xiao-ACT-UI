# -*- coding: utf-8 -*-
# Static and helper checks for the Tk plugin manager panel.

from __future__ import annotations

import _bootstrap  # noqa: F401

from pathlib import Path
import unittest

from gui_modules.sao_gui_plugin_manager import PluginDetachedPanel, _finite_int


class PluginManagerPanelTests(unittest.TestCase):
    def test_plugin_manager_numeric_rendering_uses_finite_helpers(self) -> None:
        source = (Path(__file__).resolve().parents[3] / "gui_modules" / "sao_gui_plugin_manager.py").read_text(encoding="utf-8")

        self.assertNotIn("total = int(status.get('plugin_count', 0) or len(status.get('plugins') or []))", source)
        self.assertNotIn("active = int(status.get('active_count', 0) or 0)", source)
        self.assertIn("total = _finite_int(status.get('plugin_count'), len(plugins), lo=0)", source)
        self.assertIn("active = _finite_int(status.get('active_count'), 0, lo=0)", source)
        self.assertEqual(_finite_int(float("nan"), 3, lo=0), 3)
        self.assertEqual(_finite_int(float("inf"), 4, lo=0), 4)

    def test_card_failure_counts_use_finite_helpers(self) -> None:
        # Replaces the old test_format_meta_filters_non_finite_counts: the
        # `_format_meta` method it exercised (a single "subs=.. fail=../.."
        # text line) was deliberately removed in the 2026-06-14 ACT/webview
        # visual-alignment pass in favor of the current per-card FAIL/EVT
        # badges — the finite-safety guarantee it checked now lives on the
        # `failures`/`event_failures` extraction in `_render_card`.
        source = (Path(__file__).resolve().parents[3] / "gui_modules" / "sao_gui_plugin_manager.py").read_text(encoding="utf-8")

        self.assertIn("failures = _finite_int(plugin.get('failures'), 0, lo=0)", source)
        self.assertIn("event_failures = _finite_int(plugin.get('event_failures'), 0, lo=0)", source)
        self.assertEqual(_finite_int(float("nan"), 0, lo=0), 0)
        self.assertEqual(_finite_int(float("inf"), 0, lo=0), 0)
        self.assertEqual(_finite_int("bad", 0, lo=0), 0)

    def test_detached_panel_dimensions_use_finite_helpers(self) -> None:
        source = (Path(__file__).resolve().parents[3] / "gui_modules" / "sao_gui_plugin_manager.py").read_text(encoding="utf-8")

        self.assertNotIn("self._want_w = int(width or 0)", source)
        self.assertNotIn("int(meta.get('width') or 0)", source)
        self.assertIn("self._want_w = _finite_int(width, 0, lo=0)", source)
        self.assertIn("w = self._want_w or _finite_int(meta.get('width'), 460, lo=1)", source)

        panel = PluginDetachedPanel(object(), object(), "demo", width=float("nan"), height=float("inf"))
        self.assertEqual(panel._want_w, 0)
        self.assertEqual(panel._want_h, 0)
        self.assertEqual(_finite_int("bad", 460, lo=1), 460)


if __name__ == "__main__":
    unittest.main()
