# -*- coding: utf-8 -*-
"""Selftest: BossSkillStore persistence/dedup/tag-derivation + engine recording of
skills, mechanics, and state onsets into the per-scene/per-boss aggregate."""
from __future__ import annotations

import os
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from engines.boss_skill_store import BossSkillStore, KIND_SKILL, KIND_MECHANIC, KIND_STATE  # noqa: E402


class StoreUnitTest(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp(suffix=".json"); os.close(fd); os.remove(self.path)

    def tearDown(self):
        if os.path.exists(self.path):
            os.remove(self.path)

    def test_dedup_and_tag_union(self):
        s = BossSkillStore(path=self.path, autosave_interval=0)
        s.observe(scene_key="1", boss_base_id=5, obs_id=700, name="A", kind=KIND_SKILL, tags=["cast"])
        s.observe(scene_key="1", boss_base_id=5, obs_id=700, name="A", kind=KIND_SKILL, tags=["enrage"])
        obs = s.observations("1", 5)
        self.assertEqual(len(obs), 1)
        self.assertEqual(obs[0]["count"], 2)
        self.assertIn("cast", obs[0]["tags"])
        self.assertIn("enrage", obs[0]["tags"])
        self.assertEqual(obs[0]["skill_id"], 700)

    def test_hp_line_and_time_derived(self):
        s = BossSkillStore(path=self.path, autosave_interval=0)
        for hp, el in ((0.50, 120.0), (0.49, 121.0)):
            s.observe(scene_key="1", boss_base_id=5, obs_id=9, kind=KIND_SKILL, hp_pct=hp, elapsed_s=el)
        o = s.observations("1", 5)[0]
        self.assertIn("hp_line", o["tags"])
        self.assertAlmostEqual(o["hp_line_pct"], 0.495, places=3)
        self.assertIn("time", o["tags"])

    def test_no_hp_line_when_spread_wide(self):
        s = BossSkillStore(path=self.path, autosave_interval=0)
        for hp in (0.90, 0.30):
            s.observe(scene_key="1", boss_base_id=5, obs_id=9, kind=KIND_SKILL, hp_pct=hp)
        self.assertNotIn("hp_line", s.observations("1", 5)[0]["tags"])

    def test_persist_reload(self):
        s = BossSkillStore(path=self.path, autosave_interval=0)
        s.observe(scene_key="7", scene_name="副本X", boss_base_id=5, boss_name="B", obs_id=1, kind=KIND_SKILL)
        s.save(force=True)
        s2 = BossSkillStore(path=self.path)
        self.assertEqual(s2.scenes()[0]["name"], "副本X")
        self.assertEqual(s2.bosses("7")[0]["base_id"], 5)

    def test_scene_scoping(self):
        s = BossSkillStore(path=self.path, autosave_interval=0)
        s.observe(scene_key="1", boss_base_id=10, obs_id=1, kind=KIND_SKILL)
        s.observe(scene_key="2", boss_base_id=20, obs_id=1, kind=KIND_SKILL)
        self.assertEqual([b["base_id"] for b in s.bosses("1")], [10])
        self.assertEqual([b["base_id"] for b in s.bosses("2")], [20])
        self.assertEqual(len(s.bosses()), 2)   # all scenes


class EngineRecordingTest(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp(suffix=".json"); os.close(fd); os.remove(self.path)
        os.environ["SAO_BOSS_SKILL_STORE"] = self.path

    def tearDown(self):
        os.environ.pop("SAO_BOSS_SKILL_STORE", None)
        if os.path.exists(self.path):
            os.remove(self.path)

    def _engine(self, scene_id=101, scene_name="幻华领域"):
        from engines.boss_raid_engine import BossRaidEngine
        from engines.game_state import GameStateManager
        sm = GameStateManager()
        sm.update(dungeon_scene_id=scene_id, dungeon_name=scene_name)
        return BossRaidEngine(sm, settings={})

    def test_skill_recorded_under_scene_with_concurrent_tag(self):
        eng = self._engine()
        eng.on_mem_boss_action({"boss_base_id": 500, "boss_name": "苍之冠", "boss_uuid": 1,
                                "skill_id": 700, "skill_name": "角斗同步", "cast_edge": "start",
                                "cast_duration_ms": 3000, "overdrive": True})
        scenes = eng.get_observed_scenes()
        self.assertTrue(any(s["scene_key"] == "101" and s["name"] == "幻华领域" for s in scenes))
        self.assertEqual([b["base_id"] for b in eng.get_observed_bosses("101")], [500])
        obs = eng.get_observed_boss_skills(500, "101")[500]
        skill = next(o for o in obs if o["id"] == 700)
        self.assertEqual(skill["kind"], KIND_SKILL)
        self.assertIn("enrage", skill["tags"])      # concurrent overdrive tagged onto the cast
        self.assertEqual(skill["last_cast_duration_ms"], 3000)

    def test_mechanic_event_recorded(self):
        eng = self._engine()
        eng.on_mem_boss_action({"boss_base_id": 500, "skill_id": 700, "cast_edge": "start"})
        eng.on_boss_event({"event_type": 47, "host_uuid": 1})   # SHIELD_BROKEN
        obs = eng.get_observed_boss_skills(500, "101")[500]
        mech = next((o for o in obs if o["kind"] == KIND_MECHANIC and o["id"] == 47), None)
        self.assertIsNotNone(mech)
        self.assertIn("shield", mech["tags"])

    def test_enrage_state_onset_recorded(self):
        eng = self._engine()
        # first action establishes boss + non-overdrive baseline
        eng.on_mem_boss_action({"boss_base_id": 500, "skill_id": 700, "cast_edge": "start", "overdrive": False})
        # overdrive rising edge → enrage state onset observation
        eng.on_mem_boss_action({"boss_base_id": 500, "cast_edge": "none", "overdrive": True})
        obs = eng.get_observed_boss_skills(500, "101")[500]
        state = next((o for o in obs if o["kind"] == KIND_STATE), None)
        self.assertIsNotNone(state)
        self.assertIn("enrage", state["tags"])

    def test_persist_across_engine_instances(self):
        eng = self._engine()
        eng.on_mem_boss_action({"boss_base_id": 500, "boss_name": "苍之冠", "skill_id": 700, "cast_edge": "start"})
        eng.stop()   # flushes store to disk
        eng2 = self._engine()
        self.assertEqual([b["base_id"] for b in eng2.get_observed_bosses("101")], [500])


if __name__ == "__main__":
    loader = unittest.TestLoader()
    suite = unittest.TestSuite([loader.loadTestsFromTestCase(StoreUnitTest),
                                loader.loadTestsFromTestCase(EngineRecordingTest)])
    res = unittest.TextTestRunner(verbosity=1).run(suite)
    print(f"\n{res.testsRun - len(res.failures) - len(res.errors)} passed, "
          f"{len(res.failures) + len(res.errors)} failed")
    sys.exit(1 if (res.failures or res.errors) else 0)
