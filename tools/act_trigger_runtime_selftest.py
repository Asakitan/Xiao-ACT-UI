# -*- coding: utf-8 -*-
"""Regression tests for shared ACT trigger/timer management helpers."""

from __future__ import annotations

import unittest

from act_platform import runtime


class FakeSettings:
    def __init__(self):
        self.data = {
            "act_trigger_rules": [
                {
                    "id": "damage_gate",
                    "type": "damage_total",
                    "threshold": 100,
                    "message": "Damage gate",
                    "enabled": True,
                },
                {
                    "id": "elapsed_gate",
                    "type": "elapsed_s",
                    "threshold": 5,
                    "message": "Elapsed gate",
                    "enabled": False,
                },
            ]
        }
        self.saved = 0

    def get(self, key, default=None):
        return self.data.get(key, default)

    def set(self, key, value):
        self.data[key] = value

    def save(self):
        self.saved += 1


class FakeOwner:
    def __init__(self):
        self._cfg_settings_ref = FakeSettings()
        self._act_trigger_engine = None


class ActTriggerRuntimeTests(unittest.TestCase):
    def test_status_enable_disable_reload_and_test_event(self) -> None:
        owner = FakeOwner()

        status = runtime.act_trigger_status(owner)
        self.assertTrue(status["ok"])
        self.assertTrue(status["enabled"])
        self.assertEqual([rule["id"] for rule in status["triggers"]], ["damage_gate", "elapsed_gate"])
        self.assertEqual([rule["id"] for rule in status["timers"]], ["elapsed_gate"])
        self.assertEqual(status["errors"], [])

        disabled = runtime.act_trigger_disable(owner, "damage_gate")
        self.assertTrue(disabled["ok"])
        self.assertFalse(disabled["triggers"][0]["enabled"])
        self.assertGreater(owner._cfg_settings_ref.saved, 0)

        enabled = runtime.act_trigger_enable(owner, "damage_gate")
        self.assertTrue(enabled["ok"])
        self.assertTrue(enabled["triggers"][0]["enabled"])

        tested = runtime.act_trigger_test(owner, "damage_gate")
        self.assertTrue(tested["ok"])
        self.assertEqual(tested["events"][0]["rule_id"], "damage_gate")
        self.assertEqual(tested["events"][0]["message"], "Damage gate")

        reloaded = runtime.act_trigger_reload(owner)
        self.assertTrue(reloaded["ok"])
        self.assertGreater(reloaded["last_reload_ms"], 0)

    def test_missing_rule_is_reported_without_throwing(self) -> None:
        owner = FakeOwner()
        result = runtime.act_trigger_disable(owner, "missing")
        self.assertFalse(result["ok"])
        self.assertIn("not found", result["message"])

    def test_test_event_supports_supported_trigger_types(self) -> None:
        owner = FakeOwner()
        owner._cfg_settings_ref.data["act_trigger_rules"] = [
            {"id": "heal", "type": "heal_total", "threshold": 1, "message": "heal"},
            {"id": "hp", "type": "boss_hp_pct_below", "threshold": 35, "message": "hp"},
            {"id": "skill", "type": "skill_kind", "match": "server_end", "message": "skill"},
            {"id": "boss_evt", "type": "boss_event_type", "event_type": 101, "message": "boss event"},
            {"id": "start", "type": "encounter_start", "message": "start"},
            {
                "id": "field",
                "type": "field_match",
                "field": "context.last_skill_kind",
                "operator": "eq",
                "match": "server_end",
                "message": "field",
            },
            {"id": "timer", "type": "timer_preset", "duration_s": 3, "message": "timer"},
        ]

        for rule_id in ("heal", "hp", "skill", "boss_evt", "start", "field", "timer"):
            with self.subTest(rule_id=rule_id):
                result = runtime.act_trigger_test(owner, rule_id)
                self.assertTrue(result["ok"], result)
                self.assertEqual(result["events"][0]["rule_id"], rule_id)

    def test_trigger_preset_export_import_merge_and_replace(self) -> None:
        owner = FakeOwner()

        exported = runtime.act_trigger_export_presets(owner)
        self.assertTrue(exported["ok"])
        self.assertEqual(exported["count"], 2)
        self.assertEqual(exported["kind"], "act_trigger_presets")

        imported = runtime.act_trigger_import_presets(owner, {
            "rules": [
                {
                    "id": "field",
                    "type": "field_match",
                    "field": "totals.damage",
                    "operator": "gte",
                    "threshold": 10,
                    "message": "field",
                },
                {"id": "timer", "type": "timer_preset", "duration_s": 8, "message": "timer"},
            ]
        })
        self.assertTrue(imported["ok"], imported)
        self.assertEqual(imported["imported_count"], 2)
        self.assertEqual(imported["timer_count"], 2)
        self.assertIn("timer", [rule["id"] for rule in imported["timers"]])

        replaced = runtime.act_trigger_import_presets(owner, [
            {"id": "only_timer", "type": "timer_preset", "duration_s": 2}
        ], replace=True)
        self.assertTrue(replaced["ok"], replaced)
        self.assertEqual(replaced["rule_count"], 1)
        self.assertEqual(replaced["timers"][0]["id"], "only_timer")


if __name__ == "__main__":
    unittest.main()
