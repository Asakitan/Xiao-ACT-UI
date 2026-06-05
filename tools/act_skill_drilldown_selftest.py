# -*- coding: utf-8 -*-
"""Regression tests for shared ACT skill drilldown helpers."""

from __future__ import annotations

import json
import unittest

from act_platform import runtime
from act_platform.runtime import ensure_act_event_bus


class FakeTracker:
    def __init__(self) -> None:
        self.detail = {
            "uid": 1001,
            "name": "Kirito",
            "skills": [
                {
                    "skill_id": 11,
                    "source_skill_id": 110048200100,
                    "base_skill_id": 1004820,
                    "skill_level_id": 11,
                    "skill_name": "Slash",
                    "total": 1800,
                    "hits": 3,
                    "casts": 2,
                    "crit_rate": 0.333,
                    "timeline_refs": [{"time_ms": 1000, "event_id": "evt-1"}],
                },
                {
                    "skill_id": 12,
                    "skill_name": "Potion",
                    "heal_total": 120,
                    "heal_hits": 1,
                    "casts": 1,
                },
            ],
        }

    def get_entity_detail(self, uid: int):
        return self.detail if int(uid) == 1001 else None


class FakeOwner:
    def __init__(self) -> None:
        self._dps_tracker = FakeTracker()


class ActSkillDrilldownRuntimeTests(unittest.TestCase):
    def _owner_with_events(self) -> FakeOwner:
        owner = FakeOwner()
        bus = ensure_act_event_bus(owner)
        bus.publish("skill", {"timestamp": 101.0, "attacker": "Kirito", "skill_id": 11, "skill_name": "Slash", "damage": 900}, source_name="tcp", source_kind="packet")
        bus.publish("damage", {"timestamp": 102.0, "attacker": "Kirito", "skill_id": 11, "skill_name": "Slash", "damage": 900}, source_name="tcp", source_kind="packet")
        bus.publish("damage", {"timestamp": 102.5, "attacker": "Kirito", "skill_id": 110048200100, "base_skill_id": 1004820, "skill_name": "Slash", "damage": 1}, source_name="tcp", source_kind="packet")
        bus.publish("heal", {"timestamp": 103.0, "attacker": "Kirito", "skill_id": 12, "skill_name": "Potion", "heal": 120}, source_name="tcp", source_kind="packet")
        return owner

    def test_skill_status_contains_parity_fields(self) -> None:
        status = runtime.act_skill_drilldown_status(self._owner_with_events(), combatant_id=1001, skill_id=11)

        self.assertTrue(status["ok"])
        self.assertEqual(status["combatant_id"], "1001")
        self.assertEqual(status["skill_id"], "11")
        self.assertEqual(status["summary"]["name"], "Slash")
        self.assertEqual(status["summary"]["base_skill_id"], "1004820")
        self.assertIn("110048200100", status["summary"]["candidate_skill_ids"])
        self.assertEqual(status["casts"], 2)
        self.assertEqual(status["hits"], 3)
        self.assertAlmostEqual(status["crit_rate"], 0.333)
        self.assertGreaterEqual(len(status["timeline_refs"]), 1)
        self.assertIn("encounter_id", status)
        json.dumps(status, ensure_ascii=False)

    def test_skill_filter_uses_timeline_refs(self) -> None:
        status = runtime.act_skill_drilldown_filter(self._owner_with_events(), combatant_id=1001, skill_id=11, query="damage")

        self.assertTrue(status["timeline_refs"])
        self.assertTrue(all("damage" in json.dumps(ref, ensure_ascii=False).lower() for ref in status["timeline_refs"]))

    def test_skill_copy_returns_json_payload(self) -> None:
        copied = runtime.act_skill_drilldown_copy(self._owner_with_events(), combatant_id=1001, skill_id=11)
        data = json.loads(copied["text"])

        self.assertTrue(copied["ok"])
        self.assertEqual(data["skill_id"], "11")
        self.assertIn("timeline_refs", data)

    def test_skill_status_matches_composite_source_key(self) -> None:
        status = runtime.act_skill_drilldown_status(self._owner_with_events(), combatant_id=1001, skill_id=110048200100)

        self.assertTrue(status["ok"])
        self.assertEqual(status["summary"]["name"], "Slash")
        self.assertGreaterEqual(len(status["timeline_refs"]), 1)

    def test_skill_status_matches_semantic_base_id(self) -> None:
        status = runtime.act_skill_drilldown_status(self._owner_with_events(), combatant_id=1001, skill_id=1004820)

        self.assertTrue(status["ok"])
        self.assertEqual(status["summary"]["name"], "Slash")

    def test_skill_back_clears_selection(self) -> None:
        owner = self._owner_with_events()
        runtime.act_skill_drilldown_status(owner, combatant_id=1001, skill_id=11)
        status = runtime.act_skill_drilldown_back(owner)

        self.assertEqual(status["combatant_id"], "")
        self.assertEqual(status["skill_id"], "")
        self.assertEqual(status["summary"], {})

    def test_missing_skill_is_safe(self) -> None:
        status = runtime.act_skill_drilldown_status(self._owner_with_events(), combatant_id=1001, skill_id=999)

        self.assertFalse(status["ok"])
        self.assertEqual(status["summary"], {})
        self.assertTrue(status["errors"])


if __name__ == "__main__":
    unittest.main()
