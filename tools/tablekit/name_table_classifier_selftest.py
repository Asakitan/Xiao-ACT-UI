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

    def test_placeholder_description_does_not_create_field_markers(self) -> None:
        self.assertEqual(classifier.classify_skill_id(1005201), "monster_skill")
        self.assertEqual(classifier.classify_skill_id(1006401), "monster_skill")
        tables = classifier.load_classified_tables()
        self.assertIn("1005201", tables["monster_skill"])
        self.assertIn("1006401", tables["monster_skill"])
        self.assertNotIn("1005201", tables["skill"])
        self.assertNotIn("1005201", tables["field_marker"])

    def test_profession_skill_is_split_from_generic_skill(self) -> None:
        self.assertEqual(classifier.classify_skill_id(1201), "profession_skill")
        tables = classifier.load_classified_tables()
        self.assertIn("1201", tables["profession_skill"])
        self.assertNotIn("1201", tables["skill"])

    def test_player_monster_and_environment_skills_are_split_from_generic_skill(self) -> None:
        self.assertEqual(classifier.classify_skill_id(1004820), "monster_skill")
        self.assertEqual(classifier.classify_skill_id(1006507), "environment_skill")
        tables = classifier.load_classified_tables()
        self.assertIn("1004820", tables["monster_skill"])
        self.assertIn("1006507", tables["environment_skill"])
        self.assertNotIn("1004820", tables["skill"])
        self.assertNotIn("1006507", tables["skill"])

    def test_obvious_legacy_fallback_names_leave_generic_skill(self) -> None:
        self.assertEqual(classifier.classify_skill_id(1), "player_skill")
        self.assertEqual(classifier.classify_skill_id(10), "player_skill")
        self.assertEqual(classifier.classify_skill_id(100), "monster_skill")
        self.assertEqual(classifier.classify_skill_id(100431), "monster_skill")
        self.assertEqual(classifier.classify_skill_id(1006512), "monster_skill")
        self.assertEqual(classifier.classify_skill_id(1010403), "monster_skill")
        self.assertEqual(classifier.classify_skill_id(10290117), "monster_skill")
        self.assertEqual(classifier.classify_skill_id(1005303), "monster_skill")
        self.assertEqual(classifier.classify_skill_id(1006410), "environment_skill")
        self.assertEqual(classifier.classify_skill_id(7020340), "environment_skill")
        self.assertEqual(classifier.classify_skill_id(1001), "factor_buff")
        tables = classifier.load_classified_tables()
        self.assertIn("1", tables["player_skill"])
        self.assertIn("10", tables["player_skill"])
        self.assertIn("100", tables["monster_skill"])
        self.assertIn("100431", tables["monster_skill"])
        self.assertIn("1006512", tables["monster_skill"])
        self.assertIn("1010403", tables["monster_skill"])
        self.assertIn("10290117", tables["monster_skill"])
        self.assertIn("1005303", tables["monster_skill"])
        self.assertIn("1006410", tables["environment_skill"])
        self.assertIn("7020340", tables["environment_skill"])
        self.assertIn("1001", tables["factor_buff"])
        for legacy_id in ("1", "10", "100", "100431", "1006512", "1010403", "10290117", "1005303", "1006410", "7020340", "1001"):
            self.assertNotIn(legacy_id, tables["skill"])

    def test_refined_boss_skill_excludes_scripted_and_virtual_samples(self) -> None:
        tables = classifier.load_classified_tables()
        self.assertTrue(tables["boss_skill"])
        self.assertEqual(classifier.classify_skill_id(100324), "scripted_skill")
        self.assertEqual(classifier.classify_skill_id(100325), "scripted_skill")
        self.assertEqual(classifier.classify_skill_id(100432), "virtual_skill")
        self.assertNotIn("100324", tables["boss_skill"])
        self.assertNotIn("100325", tables["boss_skill"])
        self.assertNotIn("100432", tables["boss_skill"])

    def test_ultimate_and_roguelike_tables_are_split_from_skill(self) -> None:
        self.assertEqual(classifier.classify_skill_id(1713), "ultimate_skill")
        self.assertEqual(classifier.classify_skill_id(3901), "ultimate_skill")
        self.assertEqual(classifier.classify_skill_id(998141), "roguelike_affix")
        self.assertEqual(classifier.classify_skill_id(3004020), "roguelike_affix")
        tables = classifier.load_classified_tables()
        self.assertIn("1713", tables["ultimate_skill"])
        self.assertIn("998141", tables["roguelike_affix"])
        self.assertIn("3004020", tables["roguelike_affix"])
        self.assertNotIn("1713", tables["skill"])
        self.assertNotIn("998141", tables["skill"])

    def test_scripted_skill_examples_are_split_from_skill(self) -> None:
        for sid in (100324, 100325, 100591, 1006508):
            with self.subTest(skill_id=sid):
                self.assertEqual(classifier.classify_skill_id(sid), "scripted_skill")
                self.assertIn(str(sid), classifier.load_classified_tables()["scripted_skill"])
                self.assertNotIn(str(sid), classifier.load_classified_tables()["skill"])

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

    def test_profession_skill_buff_is_split_and_aggregated(self) -> None:
        self.assertEqual(classifier.classify_buff_id(55302), "profession_skill_buff")
        tables = classifier.load_classified_tables()
        self.assertIn("55302", tables["profession_skill_buff"])
        self.assertIn("55302", tables["buff"])

    def test_event_buff_is_split_from_legacy_buff(self) -> None:
        self.assertEqual(classifier.classify_buff_id(55310), "event")
        tables = classifier.load_classified_tables()
        self.assertIn("55310", tables["event"])
        self.assertNotIn("55310", tables["buff"])

    def test_boss_mechanics_use_single_mechanic_table(self) -> None:
        tables = classifier.load_classified_tables()
        self.assertIn("47", tables["boss_mechanic"])
        self.assertNotIn("boss_status", tables)
        self.assertEqual(tables["boss_mechanic"]["47"]["text"], "护盾破裂")


if __name__ == "__main__":
    unittest.main(verbosity=2)
