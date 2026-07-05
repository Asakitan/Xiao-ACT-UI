# -*- coding: utf-8 -*-
# Regression tests for shared ACT timeline/VCR helpers.

from __future__ import annotations

import _bootstrap  # noqa: F401

import json
import unittest

from act_platform.runtime import ensure_act_event_bus
from act_platform import runtime
from act_replay.fixture_io import load_fixture_events


class FakeOwner:
    pass


class ActTimelineRuntimeTests(unittest.TestCase):
    def _owner_with_events(self) -> FakeOwner:
        owner = FakeOwner()
        bus = ensure_act_event_bus(owner)
        bus.publish("damage", {"timestamp": 100.0, "attacker": "Kirito", "damage": 1000}, source_name="test", source_kind="unit")
        bus.publish("skill", {"timestamp": 101.5, "skill": "Starburst Stream"}, source_name="test", source_kind="unit")
        bus.publish("boss", {"timestamp": 103.0, "event_type": 101, "message": "Boss phase"}, source_name="test", source_kind="unit")
        return owner

    def test_timeline_status_contains_parity_fields(self) -> None:
        owner = self._owner_with_events()
        status = runtime.act_timeline_status(owner, limit=10)

        self.assertTrue(status["ok"])
        self.assertEqual([event["topic"] for event in status["events"]], ["boss", "skill", "damage"])
        self.assertEqual(status["cursor_ms"], 0)
        self.assertEqual(status["speed"], 1.0)
        self.assertEqual(status["filters"]["query"], "")
        self.assertIn("encounter_id", status)
        json.dumps(status, ensure_ascii=False)

    def test_timeline_filter_uses_compact_event_text(self) -> None:
        owner = self._owner_with_events()
        status = runtime.act_timeline_status(owner, limit=10, query="Starburst")

        self.assertTrue(status["ok"])
        self.assertEqual(len(status["events"]), 1)
        self.assertEqual(status["events"][0]["topic"], "skill")

    def test_timeline_vcr_controls_update_shared_state(self) -> None:
        owner = self._owner_with_events()
        played = runtime.act_timeline_play(owner, speed=2.0)
        stepped = runtime.act_timeline_step(owner, delta_ms=1500)
        sought = runtime.act_timeline_seek(owner, cursor_ms=500)
        paused = runtime.act_timeline_pause(owner)

        self.assertTrue(played["playing"])
        self.assertEqual(played["speed"], 2.0)
        self.assertEqual(stepped["cursor_ms"], 1500)
        self.assertEqual(sought["cursor_ms"], 500)
        self.assertFalse(paused["playing"])

    def test_missing_or_empty_bus_is_safe(self) -> None:
        status = runtime.act_timeline_status(FakeOwner())

        self.assertTrue(status["ok"])
        self.assertEqual(status["events"], [])
        self.assertEqual(status["cursor_ms"], 0)

    def test_malformed_event_timestamp_does_not_break_status(self) -> None:
        owner = FakeOwner()
        bus = ensure_act_event_bus(owner)
        bus.publish("damage", {"timestamp": "not-a-number", "damage": 1}, source_name="test", source_kind="unit")

        status = runtime.act_timeline_status(owner)

        self.assertTrue(status["ok"])
        self.assertIsInstance(status["events"][0]["time_ms"], int)

    def test_replay_fixture_reconstructs_snapshot_at_cursor(self) -> None:
        self_uid, events = load_fixture_events("demo_events")
        owner = FakeOwner()
        owner._act_timeline_replay_events = events
        owner._act_timeline_replay_self_uid = self_uid

        before_damage = runtime.act_timeline_seek(owner, cursor_ms=5500)
        after_damage = runtime.act_timeline_seek(owner, cursor_ms=6000)

        self.assertTrue(before_damage["ok"])
        self.assertTrue(before_damage["replay"]["enabled"])
        self.assertEqual(before_damage["replay"]["event_count"], 8)
        self.assertEqual(before_damage["replay"]["replayed_event_count"], 6)
        self.assertEqual(before_damage["replay_snapshot"]["live"]["total_damage"], 0)
        self.assertEqual(after_damage["replay"]["replayed_event_count"], 7)
        self.assertEqual(after_damage["replay_snapshot"]["live"]["total_damage"], 123456)
        self.assertTrue(any(event["is_cursor"] for event in after_damage["events"]))


if __name__ == "__main__":
    unittest.main()
