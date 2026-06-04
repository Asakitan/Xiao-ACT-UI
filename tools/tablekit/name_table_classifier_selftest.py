# -*- coding: utf-8 -*-
"""Selftest for semantic runtime name table classification."""
from __future__ import annotations

import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from tools.tablekit import name_table_classifier as classifier  # noqa: E402


class NameTableClassifierTests(unittest.TestCase):
    def test_field_marker_skill_type_and_name_are_split_from_skill(self) -> None:
        self.assertEqual(classifier.classify_skill_id(1101), "field_marker")
        tables = classifier.load_classified_tables()
        self.assertIn("1101", tables["field_marker"])
        self.assertNotIn("1101", tables["skill"])

    def test_regular_player_skill_stays_skill(self) -> None:
        self.assertEqual(classifier.classify_skill_id(1201), "skill")
        self.assertIn("1201", classifier.load_classified_tables()["skill"])

    def test_boss_monster_skill_is_boss_skill(self) -> None:
        boss_skill_ids = sorted(classifier.boss_skill_ids())
        self.assertTrue(boss_skill_ids)
        sample = boss_skill_ids[0]
        kind = classifier.classify_skill_id(sample)
        if kind == "field_marker":
            sample = next(sid for sid in boss_skill_ids if classifier.classify_skill_id(sid) == "boss_skill")
        self.assertEqual(classifier.classify_skill_id(sample), "boss_skill")
        self.assertIn(str(sample), classifier.load_classified_tables()["boss_skill"])

    def test_visible_buff_is_player_buff_and_legacy_buff_aggregate(self) -> None:
        self.assertEqual(classifier.classify_buff_id(3701), "player_buff")
        tables = classifier.load_classified_tables()
        self.assertIn("3701", tables["player_buff"])
        self.assertIn("3701", tables["buff"])

    def test_hidden_internal_buff_is_factor_buff_and_legacy_buff_aggregate(self) -> None:
        self.assertEqual(classifier.classify_buff_id(21411), "factor_buff")
        tables = classifier.load_classified_tables()
        self.assertIn("21411", tables["factor_buff"])
        self.assertIn("21411", tables["buff"])

    def test_event_buff_is_split_from_legacy_buff(self) -> None:
        self.assertEqual(classifier.classify_buff_id(55310), "event")
        tables = classifier.load_classified_tables()
        self.assertIn("55310", tables["event"])
        self.assertNotIn("55310", tables["buff"])

    def test_boss_mechanics_are_mechanic_and_status_tables(self) -> None:
        tables = classifier.load_classified_tables()
        self.assertIn("47", tables["boss_mechanic"])
        self.assertIn("47", tables["boss_status"])
        self.assertEqual(tables["boss_mechanic"]["47"]["text"], "护盾破裂")


if __name__ == "__main__":
    unittest.main(verbosity=2)
