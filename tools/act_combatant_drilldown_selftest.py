# -*- coding: utf-8 -*-
"""Regression tests for shared ACT combatant drilldown helpers."""

from __future__ import annotations

import json
import unittest

from act_platform import runtime


class FakeTracker:
    def __init__(self) -> None:
        self.detail = {
            "uid": 1001,
            "name": "Kirito",
            "profession": "Sword",
            "damage": 2500,
            "damage_total": 2500,
            "heal": 120,
            "heal_total": 120,
            "dps": 1250,
            "hps": 60,
            "crit_rate": 0.25,
            "damage_pct": 0.625,
            "skills": [
                {"skill_id": 11, "skill_name": "Slash", "total": 1800, "hits": 3, "crit_rate": 0.333},
                {"skill_id": 12, "skill_name": "Potion", "heal_total": 120, "heal_hits": 1},
            ],
        }

    def get_entity_detail(self, uid: int):
        return self.detail if int(uid) == 1001 else None


class FakeOwner:
    def __init__(self) -> None:
        self._dps_tracker = FakeTracker()
        self._act_combatant_drilldown_state = {}


class ActCombatantDrilldownRuntimeTests(unittest.TestCase):
    def test_combatant_status_contains_parity_fields(self) -> None:
        status = runtime.act_combatant_drilldown_status(FakeOwner(), combatant_id=1001)

        self.assertTrue(status["ok"])
        self.assertEqual(status["combatant_id"], "1001")
        self.assertEqual(status["summary"]["name"], "Kirito")
        self.assertEqual(len(status["skills"]), 2)
        self.assertIn("incoming", status)
        self.assertIn("outgoing", status)
        self.assertIn("encounter_id", status)
        json.dumps(status, ensure_ascii=False)

    def test_combatant_status_sorts_skills_by_activity(self) -> None:
        status = runtime.act_combatant_drilldown_status(FakeOwner(), combatant_id=1001)
        skills = status["skills"]

        self.assertEqual(skills[0]["name"], "Slash")
        self.assertEqual(skills[0]["amount"], 1800)
        self.assertEqual(skills[1]["kind"], "heal")
        self.assertEqual(skills[1]["amount"], 120)

    def test_combatant_filter_and_focus_state(self) -> None:
        owner = FakeOwner()
        filtered = runtime.act_combatant_drilldown_filter(owner, combatant_id=1001, query="slash")
        focused = runtime.act_combatant_drilldown_focus_target(owner, combatant_id=1001, target_id="boss-7")
        after = runtime.act_combatant_drilldown_status(owner)

        self.assertEqual([skill["name"] for skill in filtered["skills"]], ["Slash"])
        self.assertEqual(focused["filters"]["focus_target"], "boss-7")
        self.assertEqual(after["combatant_id"], "1001")
        self.assertEqual(after["filters"]["query"], "slash")

    def test_combatant_back_clears_selection(self) -> None:
        owner = FakeOwner()
        runtime.act_combatant_drilldown_status(owner, combatant_id=1001)
        status = runtime.act_combatant_drilldown_back(owner)

        self.assertEqual(status["combatant_id"], "")
        self.assertEqual(status["summary"], {})
        self.assertEqual(status["skills"], [])

    def test_missing_combatant_is_safe(self) -> None:
        status = runtime.act_combatant_drilldown_status(FakeOwner(), combatant_id=9999)

        self.assertFalse(status["ok"])
        self.assertEqual(status["summary"], {})
        self.assertTrue(status["errors"])


if __name__ == "__main__":
    unittest.main()
