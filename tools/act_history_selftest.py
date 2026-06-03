# -*- coding: utf-8 -*-
"""Regression tests for shared ACT history-browser helpers."""

from __future__ import annotations

import json
import os
import tempfile
import unittest

from act_platform import runtime
from engines.dps_history import DpsHistoryStore


class FakeOwner:
    def __init__(self, store):
        self._dps_history_store = store
        self.shown = None

    def _show_dps_last_report(self, report=None):
        self.shown = report
        return bool(report)


class ActHistoryRuntimeTests(unittest.TestCase):
    def _store(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        path = os.path.join(tmp.name, "history.json")
        store = DpsHistoryStore(path=path, limit=10)
        store.add_report({"report_reason": "first", "elapsed_s": 10, "total_damage": 100, "entities": [{"uid": 1, "name": "A", "damage_total": 100}]})
        store.add_report({"report_reason": "second", "elapsed_s": 20, "total_damage": 200, "entities": [{"uid": 2, "name": "B", "damage_total": 200}]})
        return store

    def test_history_status_lists_newest_first_with_parity_fields(self) -> None:
        owner = FakeOwner(self._store())
        status = runtime.act_history_status(owner, limit=5)

        self.assertTrue(status["ok"])
        self.assertEqual(status["encounters"][0]["report_reason"], "second")
        self.assertEqual(status["encounters"][1]["report_reason"], "first")
        self.assertEqual(status["cursor"]["limit"], 5)
        self.assertEqual(status["storage_status"]["count"], 2)
        self.assertIn("filters", status)
        json.dumps(status, ensure_ascii=False)

    def test_history_load_can_push_selected_report_to_owner(self) -> None:
        owner = FakeOwner(self._store())
        loaded = runtime.act_history_load(owner, index=1, show=True)

        self.assertTrue(loaded["ok"])
        self.assertEqual(loaded["report"]["report_reason"], "first")
        self.assertEqual(owner.shown["report_reason"], "first")

    def test_history_delete_index_and_clear_are_safe(self) -> None:
        owner = FakeOwner(self._store())
        deleted = runtime.act_history_delete(owner, index=0)
        after_delete = runtime.act_history_status(owner)
        cleared = runtime.act_history_delete(owner, clear=True)
        after_clear = runtime.act_history_status(owner)

        self.assertTrue(deleted["ok"])
        self.assertEqual(deleted["deleted"]["report_reason"], "second")
        self.assertEqual([item["report_reason"] for item in after_delete["encounters"]], ["first"])
        self.assertTrue(cleared["ok"])
        self.assertEqual(after_clear["encounters"], [])

    def test_filtered_history_keeps_original_delete_index(self) -> None:
        owner = FakeOwner(self._store())
        filtered = runtime.act_history_status(owner, query="first")

        self.assertEqual(filtered["encounters"][0]["report_reason"], "first")
        self.assertEqual(filtered["encounters"][0]["_history_index"], 1)
        deleted = runtime.act_history_delete(owner, index=filtered["encounters"][0]["_history_index"])
        remaining = runtime.act_history_status(owner)

        self.assertTrue(deleted["ok"])
        self.assertEqual(deleted["deleted"]["report_reason"], "first")
        self.assertEqual([item["report_reason"] for item in remaining["encounters"]], ["second"])

    def test_missing_store_is_reported_without_throwing(self) -> None:
        status = runtime.act_history_status(object())
        loaded = runtime.act_history_load(object(), index=0)
        deleted = runtime.act_history_delete(object(), index=0)

        self.assertFalse(status["ok"])
        self.assertFalse(loaded["ok"])
        self.assertFalse(deleted["ok"])


if __name__ == "__main__":
    unittest.main()
