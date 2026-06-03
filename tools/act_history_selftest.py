# -*- coding: utf-8 -*-
"""Regression tests for shared ACT history-browser helpers."""

from __future__ import annotations

import json
import os
import tempfile
import unittest

from act_platform import runtime
from engines.dps_history import (
    DpsHistoryStore,
    DPS_HISTORY_JSONL_SCHEMA_VERSION,
    DPS_HISTORY_SQLITE_SCHEMA_VERSION,
)


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

    def test_history_search_matches_combatant_fields(self) -> None:
        owner = FakeOwner(self._store())
        filtered = runtime.act_history_status(owner, query="B")

        self.assertTrue(filtered["ok"])
        self.assertEqual(len(filtered["encounters"]), 1)
        self.assertEqual(filtered["encounters"][0]["report_reason"], "second")
        self.assertEqual(filtered["encounters"][0]["_history_index"], 0)

    def test_history_store_appends_jsonl_archive(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_history_jsonl_") as root:
            path = os.path.join(root, "history.json")
            archive_path = os.path.join(root, "history.jsonl")
            store = DpsHistoryStore(path=path, archive_path=archive_path, limit=10)
            store.add_report({"report_reason": "first", "elapsed_s": 10, "total_damage": 100, "entities": [{"uid": 1, "name": "A", "damage_total": 100}]})
            store.add_report({"report_reason": "second", "elapsed_s": 20, "total_damage": 200, "entities": [{"uid": 2, "name": "B", "damage_total": 200}]})
            status = store.archive_status()
            archived = store.list_archive_reports(limit=1)
            with open(archive_path, "r", encoding="utf-8") as fp:
                lines = [json.loads(line) for line in fp if line.strip()]

        self.assertTrue(status["available"])
        self.assertTrue(status["exists"])
        self.assertEqual(status["count"], 2)
        self.assertEqual(lines[0]["schema_version"], DPS_HISTORY_JSONL_SCHEMA_VERSION)
        self.assertEqual(lines[0]["report"]["report_reason"], "first")
        self.assertEqual(archived[0]["report_reason"], "second")
        self.assertEqual(archived[0]["_archive_schema_version"], DPS_HISTORY_JSONL_SCHEMA_VERSION)
        self.assertTrue(archived[0]["_archived_at"])

    def test_history_store_appends_sqlite_archive(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_history_sqlite_") as root:
            path = os.path.join(root, "history.json")
            sqlite_path = os.path.join(root, "history.sqlite3")
            store = DpsHistoryStore(path=path, sqlite_path=sqlite_path, limit=10)
            store.add_report({
                "encounter_id": "enc-first",
                "report_reason": "first",
                "elapsed_s": 10,
                "total_damage": 100,
                "entities": [{"uid": 1, "name": "A", "damage_total": 100}],
                "actions": [{
                    "topic": "damage",
                    "observed_at": 10.5,
                    "payload": {
                        "actor_uid": 1,
                        "actor_name": "A",
                        "target_uid": 99,
                        "target_name": "Boss",
                        "skill_id": "slash",
                        "skill_name": "Slash",
                        "damage": 100,
                    },
                    "source_name": "fixture",
                    "source_kind": "replay",
                }],
                "timeline_events": [{"event_type": "phase", "time_ms": 10500, "label": "Phase 1"}],
                "trigger_events": [{"rule_id": "rule-1", "severity": "info", "message": "Trigger", "time_ms": 10600}],
                "source_metadata": {"parser_id": "fixture_parser"},
            })
            store.add_report({
                "encounter_id": "enc-second",
                "report_reason": "second",
                "elapsed_s": 20,
                "total_damage": 200,
                "entities": [{"uid": 2, "name": "B", "damage_total": 200}],
            })
            status = store.sqlite_status()
            archived = store.list_sqlite_reports(limit=2)
            actions = store.list_sqlite_actions(limit=5, encounter_id="enc-first")
            runtime_status = runtime.act_history_status(FakeOwner(store), limit=5)

        self.assertTrue(status["available"])
        self.assertTrue(status["exists"])
        self.assertEqual(status["schema_version"], DPS_HISTORY_SQLITE_SCHEMA_VERSION)
        self.assertEqual(status["encounter_count"], 2)
        self.assertEqual(status["combatant_count"], 2)
        self.assertEqual(status["action_count"], 1)
        self.assertEqual(status["timeline_count"], 1)
        self.assertEqual(status["trigger_count"], 1)
        self.assertEqual(status["source_metadata_count"], 1)
        self.assertEqual(archived[0]["encounter_id"], "enc-second")
        self.assertEqual(archived[0]["entities"][0]["name"], "B")
        self.assertEqual(archived[0]["_sqlite_schema_version"], DPS_HISTORY_SQLITE_SCHEMA_VERSION)
        self.assertEqual(actions[0]["topic"], "damage")
        self.assertEqual(actions[0]["skill_name"], "Slash")
        self.assertEqual(actions[0]["value"], 100.0)
        self.assertEqual(actions[0]["payload"]["payload"]["skill_id"], "slash")
        self.assertEqual(runtime_status["storage_status"]["sqlite"]["encounter_count"], 2)
        self.assertEqual(runtime_status["storage_status"]["sqlite"]["action_count"], 1)

    def test_missing_store_is_reported_without_throwing(self) -> None:
        status = runtime.act_history_status(object())
        loaded = runtime.act_history_load(object(), index=0)
        deleted = runtime.act_history_delete(object(), index=0)

        self.assertFalse(status["ok"])
        self.assertFalse(loaded["ok"])
        self.assertFalse(deleted["ok"])


if __name__ == "__main__":
    unittest.main()
