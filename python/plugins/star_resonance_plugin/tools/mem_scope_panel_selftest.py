# -*- coding: utf-8 -*-
"""Regression tests for Entity Mem Scope render signatures."""

from __future__ import annotations

import _bootstrap  # noqa: F401

import unittest
from unittest import mock

from plugins.star_resonance_plugin.panels.sao_gui_mem_scope import MemScopePanel


def _panel() -> MemScopePanel:
    panel = MemScopePanel.__new__(MemScopePanel)
    panel._job_id = "job-1"
    return panel


def _status(**overrides):
    data = {
        "status": {
            "active": True,
            "armed": True,
            "process": "star.exe",
            "module_base": "0x1000",
            "provider_mode": "hybrid",
        },
        "catalog": {
            "categories": [{"id": "entities", "name": "Entities", "available": True, "hint": "ready"}]
        },
        "self": {"ok": True, "uid": 1, "hp": 100, "max_hp": 200},
        "entities": {
            "entities": [
                {
                    "kind": "boss",
                    "name": "Boss",
                    "cur_hp": 1000,
                    "max_hp": 2000,
                    "hp_pct": 50,
                    "base_id": 1,
                    "obj": "0x1",
                }
            ]
        },
        "damage": {"totals": {"1": 100}},
        "search": {
            "state": "done",
            "count": 1,
            "progress": 1.0,
            "results": [{"addr": "0x1", "as": {"u32": 100}}],
        },
    }
    data.update(overrides)
    return data


class MemScopePanelSignatureTests(unittest.TestCase):
    def test_signature_tracks_entity_value_changes(self) -> None:
        panel = _panel()
        base = _status()
        changed = _status(entities={
            "entities": [
                {
                    "kind": "boss",
                    "name": "Boss",
                    "cur_hp": 900,
                    "max_hp": 2000,
                    "hp_pct": 45,
                    "base_id": 1,
                    "obj": "0x1",
                }
            ]
        })

        self.assertNotEqual(panel._signature(base), panel._signature(changed))

    def test_signature_tracks_damage_value_changes(self) -> None:
        panel = _panel()
        base = _status()
        changed = _status(damage={"totals": {"1": 999}})

        self.assertNotEqual(panel._signature(base), panel._signature(changed))

    def test_signature_tracks_search_result_changes(self) -> None:
        panel = _panel()
        base = _status()
        changed = _status(search={
            "state": "done",
            "count": 1,
            "progress": 1.0,
            "results": [{"addr": "0x2", "as": {"u32": 999}}],
        })

        self.assertNotEqual(panel._signature(base), panel._signature(changed))

    def test_copy_addr_reports_clipboard_failure(self) -> None:
        class _Var:
            def __init__(self) -> None:
                self.value = ""

            def set(self, value) -> None:
                self.value = value

        class _BadClipboardRoot:
            def clipboard_clear(self):
                raise RuntimeError("no clipboard")

            def clipboard_append(self, _text):
                raise RuntimeError("no clipboard")

        panel = _panel()
        panel._status_var = _Var()
        panel.root = _BadClipboardRoot()

        panel._copy_addr("0x1234")

        self.assertTrue(panel._status_var.value.startswith("复制失败"))

    def test_destroy_resets_render_signature(self) -> None:
        panel = _panel()
        panel._win = None
        panel._rows = None
        panel._poll_after = None
        panel._last_sig = "stale"

        panel.destroy()

        self.assertEqual(panel._last_sig, "")

    def test_render_search_tolerates_bad_count_and_progress(self) -> None:
        class _Widget:
            def pack(self, *args, **kwargs):
                return None

        panel = _panel()
        panel._rows = _Widget()
        panel._job_id = "job-1"

        with (
            mock.patch("plugins.star_resonance_plugin.panels.sao_gui_mem_scope.tk.Frame", return_value=_Widget()),
            mock.patch("plugins.star_resonance_plugin.panels.sao_gui_mem_scope.tk.Label", return_value=_Widget()),
            mock.patch("plugins.star_resonance_plugin.panels.sao_gui_mem_scope.section_card", return_value=_Widget()),
            mock.patch("plugins.star_resonance_plugin.panels.sao_gui_mem_scope.empty_state", return_value=_Widget()),
            mock.patch("plugins.star_resonance_plugin.panels.sao_gui_mem_scope.action_button", return_value=_Widget()),
        ):
            panel._render_search({
                "search": {
                    "state": "running",
                    "count": "bad",
                    "progress": "bad",
                    "results": [],
                }
            })


if __name__ == "__main__":
    unittest.main()
