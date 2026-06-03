# -*- coding: utf-8 -*-
"""Regression tests for shared ACT action-log helpers."""

from __future__ import annotations

import json
import os
import tempfile
import unittest

from act_platform import runtime
from act_platform.runtime import ensure_act_event_bus
from engines.dps_history import DpsHistoryStore


class FakeOwner:
    pass


class ActActionLogRuntimeTests(unittest.TestCase):
    def _owner_with_events(self) -> FakeOwner:
        owner = FakeOwner()
        bus = ensure_act_event_bus(owner)
        bus.publish("damage", {"timestamp": 100.0, "attacker": "Kirito", "damage": 1000}, source_name="tcp", source_kind="packet")
        bus.publish("skill", {"timestamp": 101.5, "skill": "Starburst Stream", "target": "Boss"}, source_name="tcp", source_kind="packet")
        bus.publish("boss", {"timestamp": 103.0, "event_type": 101, "message": "Boss phase"}, source_name="trigger", source_kind="runtime")
        return owner

    def test_action_log_status_contains_parity_fields(self) -> None:
        owner = self._owner_with_events()
        status = runtime.act_action_log_status(owner, limit=10)

        self.assertTrue(status["ok"])
        self.assertEqual([row["topic"] for row in status["rows"]], ["boss", "skill", "damage"])
        self.assertEqual(status["columns"][0]["key"], "time_ms")
        self.assertEqual(status["filters"]["query"], "")
        self.assertEqual(status["cursor"]["limit"], 10)
        self.assertIn("encounter_id", status)
        json.dumps(status, ensure_ascii=False)

    def test_action_log_search_and_topic_filter_share_state(self) -> None:
        owner = self._owner_with_events()
        searched = runtime.act_action_log_search(owner, query="Starburst")
        filtered = runtime.act_action_log_filter(owner, topic="skill")
        after = runtime.act_action_log_status(owner)

        self.assertEqual(len(searched["rows"]), 1)
        self.assertEqual(searched["rows"][0]["topic"], "skill")
        self.assertEqual(len(filtered["rows"]), 1)
        self.assertEqual(filtered["filters"]["topic"], "skill")
        self.assertEqual(after["filters"]["topic"], "skill")

    def test_action_log_status_can_clear_filters(self) -> None:
        owner = self._owner_with_events()
        runtime.act_action_log_filter(owner, topic="skill", query="Starburst")

        cleared = runtime.act_action_log_status(owner, topic="", query="")

        self.assertEqual(cleared["filters"]["topic"], "")
        self.assertEqual(cleared["filters"]["query"], "")
        self.assertEqual(len(cleared["rows"]), 3)

    def test_action_log_jump_to_time_marks_nearest_row(self) -> None:
        owner = self._owner_with_events()
        status = runtime.act_action_log_jump_to_time(owner, cursor_ms=101500)

        self.assertEqual(status["cursor"]["time_ms"], 101500)
        self.assertEqual(status["cursor"]["nearest_row_id"], status["rows"][1]["id"])
        self.assertTrue(status["rows"][1]["is_cursor"])

    def test_action_log_copy_returns_json_payload(self) -> None:
        owner = self._owner_with_events()
        copied = runtime.act_action_log_copy(owner, limit=2, query="")
        data = json.loads(copied["text"])

        self.assertTrue(copied["ok"])
        self.assertIn("encounter_id", copied)
        self.assertEqual(len(data["rows"]), 2)
        self.assertIn("columns", data)

    def test_action_log_empty_bus_is_safe(self) -> None:
        status = runtime.act_action_log_status(FakeOwner())

        self.assertTrue(status["ok"])
        self.assertEqual(status["rows"], [])
        self.assertEqual(status["cursor"]["row_count"], 0)

    def test_action_log_history_source_reads_sqlite_actions(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_action_history_") as root:
            store = DpsHistoryStore(
                path=os.path.join(root, "history.json"),
                archive_path=os.path.join(root, "history.jsonl"),
                sqlite_path=os.path.join(root, "history.sqlite3"),
            )
            store.add_report({
                "encounter_id": "enc-history",
                "completed_at": 200.0,
                "completed_local_time": "2026-06-03 12:00:00",
                "report_reason": "unit",
                "encounter_started_at": 100.0,
                "encounter_ended_at": 112.0,
                "elapsed_s": 12.0,
                "total_damage": 321,
                "total_damage_all": 321,
                "total_heal": 0,
                "total_dps": 26,
                "total_hps": 0,
                "entities": [],
                "actions": [{
                    "topic": "damage",
                    "time_ms": 1250,
                    "source_name": "archive",
                    "source_kind": "sqlite",
                    "payload": {
                        "action_type": "damage",
                        "actor_name": "Kirito",
                        "target_name": "Boss",
                        "skill_id": "starburst",
                        "skill_name": "Starburst Stream",
                        "damage": 321,
                    },
                }],
            })
            owner = FakeOwner()
            owner._dps_history_store = store

            status = runtime.act_action_log_status(
                owner,
                source="history",
                encounter_id="enc-history",
                query="Starburst",
                limit=10,
            )

        self.assertTrue(status["ok"])
        self.assertEqual(status["source"], "history")
        self.assertEqual(status["filters"]["encounter_id"], "enc-history")
        self.assertEqual(status["cursor"]["row_count"], 1)
        self.assertEqual(status["rows"][0]["label"], "Starburst Stream")
        self.assertEqual(status["rows"][0]["actor"], "Kirito")
        self.assertEqual(status["rows"][0]["encounter_id"], "enc-history")
        self.assertIn("sqlite", status["storage_status"])


if __name__ == "__main__":
    unittest.main()
