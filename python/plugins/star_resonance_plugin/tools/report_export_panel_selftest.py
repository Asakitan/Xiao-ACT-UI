# -*- coding: utf-8 -*-
"""Regression tests for Entity report/export panel render caches."""

from __future__ import annotations

import _bootstrap  # noqa: F401

import time
import unittest
from unittest import mock

from plugins.star_resonance_plugin.panels.sao_gui_report_export import ReportExportPanel


class FakeVar:
    def __init__(self, value: str = "") -> None:
        self.value = value

    def get(self) -> str:
        return self.value

    def set(self, value: object) -> None:
        self.value = str(value)


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

    def test_refresh_cache_reuses_only_same_format(self) -> None:
        panel = ReportExportPanel.__new__(ReportExportPanel)
        panel.owner = object()
        panel._format_var = FakeVar("json")
        panel._last_status = {"ok": True, "preview": {"format": "cached"}, "history": []}
        panel._last_refresh_at = time.time()
        panel._last_request_key = ("json",)
        rendered: list[dict] = []
        panel._render_status = lambda status: rendered.append(dict(status))

        with mock.patch("plugins.star_resonance_plugin.panels.sao_gui_report_export.act_report_status") as status_fn:
            cached = panel.refresh()

        self.assertEqual(cached["preview"]["format"], "cached")
        self.assertEqual(rendered[-1]["preview"]["format"], "cached")
        status_fn.assert_not_called()

        panel._format_var.set("csv")
        status_payload = {"ok": True, "preview": {"format": "fresh"}, "history": []}
        with mock.patch("plugins.star_resonance_plugin.panels.sao_gui_report_export.act_report_status", return_value=status_payload) as status_fn:
            refreshed = panel.refresh()

        self.assertEqual(refreshed["preview"]["format"], "fresh")
        self.assertEqual(panel._last_request_key, ("csv",))
        status_fn.assert_called_once_with(panel.owner, limit=20, fmt="csv")

    def test_render_status_normalizes_malformed_preview_damage(self) -> None:
        panel = ReportExportPanel.__new__(ReportExportPanel)
        panel.owner = object()
        panel._format_var = FakeVar("json")
        panel._summary_var = FakeVar()
        panel._status_var = FakeVar()
        panel._rows = None
        panel._history = None
        panel._last_render_sig = ""
        panel._last_history_sig = ""

        panel._render_status({"ok": True, "preview": {"total_damage": "oops"}, "history": [], "errors": []})

        self.assertEqual(panel._summary_var.get(), "JSON · 0 DMG")

    def test_history_actions_normalize_malformed_index(self) -> None:
        panel = ReportExportPanel.__new__(ReportExportPanel)
        panel.owner = object()
        panel._status_var = FakeVar()
        panel._last_refresh_at = 1.0
        panel._last_request_key = ("json",)
        panel._last_history_sig = "history"
        panel.refresh = lambda: {"ok": True}  # type: ignore[method-assign]

        with mock.patch("plugins.star_resonance_plugin.panels.sao_gui_report_export.act_history_load", return_value={"ok": True}) as load_fn:
            panel.load_history("bad")  # type: ignore[arg-type]
        with mock.patch("plugins.star_resonance_plugin.panels.sao_gui_report_export.act_history_delete", return_value={"ok": True}) as delete_fn:
            panel.delete_history("bad")  # type: ignore[arg-type]

        load_fn.assert_called_once_with(panel.owner, index=0, show=True)
        delete_fn.assert_called_once_with(panel.owner, index=0)


if __name__ == "__main__":
    unittest.main()
