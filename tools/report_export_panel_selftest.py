# -*- coding: utf-8 -*-
"""Regression tests for Entity report/export panel render caches."""

from __future__ import annotations

import unittest

from gui_modules.sao_gui_report_export import ReportExportPanel


class ReportExportPanelCacheTests(unittest.TestCase):
    def test_destroy_resets_preview_and_history_signatures(self) -> None:
        panel = ReportExportPanel.__new__(ReportExportPanel)
        panel._win = None
        panel._rows = None
        panel._history = None
        panel._last_render_sig = "preview-stale"
        panel._last_history_sig = "history-stale"

        panel.destroy()

        self.assertEqual(panel._last_render_sig, "")
        self.assertEqual(panel._last_history_sig, "")

    def test_reset_render_cache_clears_both_report_sections(self) -> None:
        panel = ReportExportPanel.__new__(ReportExportPanel)
        panel._last_render_sig = "preview-stale"
        panel._last_history_sig = "history-stale"

        panel._reset_render_cache()

        self.assertEqual(panel._last_render_sig, "")
        self.assertEqual(panel._last_history_sig, "")


if __name__ == "__main__":
    unittest.main()
