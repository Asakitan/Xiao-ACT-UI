# -*- coding: utf-8 -*-
"""Regression coverage for shared ACT death recap helpers."""

from __future__ import annotations

import json
import unittest

from act_platform import runtime
from act_platform.events import make_event
from act_platform.runtime import ensure_act_event_bus


SELF_UID = 36668136


class FakeOwner:
    pass


def _publish(owner: FakeOwner, topic: str, payload: dict, ts: float) -> None:
    bus = ensure_act_event_bus(owner)
    bus.publish(
        topic,
        event=make_event(
            topic,
            payload,
            source_name="test",
            source_kind="unit",
            observed_at=ts,
        ),
    )


class ActDeathRecapRuntimeTests(unittest.TestCase):
    def _owner_with_death(self) -> FakeOwner:
        owner = FakeOwner()
        _publish(owner, "damage", {"attacker": "Boss", "target": "Kirito", "target_uid": SELF_UID, "damage": 999}, 90.0)
        _publish(owner, "damage", {"attacker": "Boss", "target": "Kirito", "target_uid": SELF_UID, "damage": 1000}, 95.0)
        _publish(owner, "heal", {"actor": "Asuna", "target": "Kirito", "target_uid": SELF_UID, "heal": 250}, 97.0)
        _publish(owner, "shield", {"actor": "Support", "target": "Kirito", "target_uid": SELF_UID, "shield": 100}, 98.0)
        _publish(owner, "damage", {"attacker": "Boss", "target": "Kirito", "target_uid": SELF_UID, "damage": 700}, 99.0)
        _publish(owner, "self_state", {"uid": SELF_UID, "name": "Kirito", "hp": 0, "max_hp": 1000, "is_dead": True}, 100.0)
        _publish(owner, "damage", {"attacker": "Boss", "target": "Kirito", "target_uid": SELF_UID, "damage": 50}, 101.0)
        return owner

    def test_death_recap_summarizes_window_around_latest_death(self) -> None:
        owner = self._owner_with_death()

        status = runtime.act_death_recap_status(owner, window_s=5.0, limit=20)

        self.assertTrue(status["ok"], status)
        self.assertEqual(status["death"]["entity_id"], str(SELF_UID))
        self.assertEqual(status["death"]["name"], "Kirito")
        self.assertEqual(status["summary"]["incoming_damage"], 1750)
        self.assertEqual(status["summary"]["healing"], 250)
        self.assertEqual(status["summary"]["shield"], 100)
        self.assertEqual(status["summary"]["death_events"], 1)
        self.assertFalse(any(row["amount"] == 999 for row in status["rows"]))
        self.assertEqual([row["time_ms"] for row in status["rows"]], sorted(row["time_ms"] for row in status["rows"]))
        self.assertTrue(any(row["is_death"] and row["relative_ms"] == 0 for row in status["rows"]))
        json.dumps(status, ensure_ascii=False)

    def test_death_recap_copy_returns_json_payload(self) -> None:
        owner = self._owner_with_death()

        copied = runtime.act_death_recap_copy(owner, window_s=5.0)
        data = json.loads(copied["text"])

        self.assertTrue(copied["ok"], copied)
        self.assertEqual(data["summary"]["incoming_damage"], 1750)
        self.assertEqual(data["death"]["entity_id"], str(SELF_UID))

    def test_death_recap_empty_bus_is_safe(self) -> None:
        status = runtime.act_death_recap_status(FakeOwner())

        self.assertTrue(status["ok"])
        self.assertIsNone(status["death"])
        self.assertEqual(status["rows"], [])
        self.assertEqual(status["summary"]["event_count"], 0)


if __name__ == "__main__":
    unittest.main()
