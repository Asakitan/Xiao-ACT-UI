# -*- coding: utf-8 -*-
"""Regression coverage for ACT Selective Parsing policy helpers."""

from __future__ import annotations

import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from act_platform.runtime import (
    act_selective_parsing_clear,
    act_selective_parsing_status,
    act_selective_parsing_update,
    should_record_owner_combat_event,
)
from act_platform.selective_parsing import normalize_policy, should_record_event


class _Settings(dict):
    def set(self, key, value):
        self[key] = value

    def save(self):
        self["_saved"] = True


class _Tracker:
    _self_uid = 1001


class _Owner:
    def __init__(self):
        self.settings = _Settings()
        self._dps_tracker = _Tracker()


class ActSelectiveParsingTests(unittest.TestCase):
    def test_default_policy_records_everything(self) -> None:
        policy = normalize_policy({})
        decision = should_record_event({"attacker_uid": 2002, "damage": 100}, policy)

        self.assertFalse(policy["enabled"])
        self.assertTrue(decision.record)
        self.assertEqual(decision.reason, "record_all_default")

    def test_include_id_policy_filters_unselected_damage(self) -> None:
        policy = normalize_policy({"enabled": True, "mode": "include", "include_ids": [1001]})

        selected = should_record_event({"attacker_uid": 1001, "damage": 100}, policy)
        rejected = should_record_event({"attacker_uid": 2002, "damage": 100}, policy)

        self.assertTrue(selected.record)
        self.assertEqual(selected.reason, "included_id")
        self.assertFalse(rejected.record)
        self.assertEqual(rejected.reason, "not_selected")

    def test_owner_runtime_status_update_and_clear(self) -> None:
        owner = _Owner()

        initial = act_selective_parsing_status(owner)
        self.assertTrue(initial["ok"])
        self.assertFalse(initial["enabled"])

        updated = act_selective_parsing_update(owner, {"enabled": True, "mode": "self"})
        self.assertTrue(updated["enabled"])
        self.assertEqual(updated["mode"], "self")
        self.assertTrue(owner.settings.get("_saved"))

        accepted = should_record_owner_combat_event(owner, {"attacker_uid": 1001, "damage": 50})
        rejected = should_record_owner_combat_event(owner, {"attacker_uid": 2002, "damage": 50})
        self.assertTrue(accepted["record"])
        self.assertFalse(rejected["record"])
        self.assertEqual(act_selective_parsing_status(owner)["last_decision"]["reason"], "not_selected")

        cleared = act_selective_parsing_clear(owner)
        self.assertFalse(cleared["enabled"])
        self.assertTrue(should_record_owner_combat_event(owner, {"attacker_uid": 2002, "damage": 50})["record"])


if __name__ == "__main__":
    unittest.main()
