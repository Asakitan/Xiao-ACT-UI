# -*- coding: utf-8 -*-
# Selftest: BossSkillStore persistence/dedup/tag-derivation + engine recording of
# skills, mechanics, and state onsets into the per-scene/per-boss aggregate.
from __future__ import annotations

import os
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from plugins.star_resonance_plugin.engines.boss_skill_store import BossSkillStore, KIND_SKILL, KIND_MECHANIC, KIND_STATE  # noqa: E402


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
        from plugins.star_resonance_plugin.engines.boss_raid_engine import BossRaidEngine
        from plugins.star_resonance_plugin.engines.game_state import GameStateManager
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

    def test_contract_is_scene_aware_and_tagged(self):
        from plugins.star_resonance_plugin.engines.boss_autokey_linkage import build_boss_reactions_state
        eng = self._engine()
        eng.on_mem_boss_action({"boss_base_id": 500, "boss_name": "苍之冠", "boss_uuid": 1,
                                "skill_id": 700, "skill_name": "角斗同步", "cast_edge": "start",
                                "cast_duration_ms": 3000, "overdrive": True})
        eng.on_boss_event({"event_type": 47, "host_uuid": 1})   # SHIELD_BROKEN
        st = build_boss_reactions_state({}, eng, eng._state_mgr)
        self.assertEqual(st["current_scene"]["name"], "幻华领域")
        self.assertEqual(st["selected_scene_key"], "101")
        self.assertIn("101", [s["scene_key"] for s in st["scenes"]])
        self.assertEqual([b["base_id"] for b in st["bosses"]], [500])
        obs = st["observed_skills"]["500"]
        skill = next(o for o in obs if o["kind"] == "skill")
        self.assertIn("enrage", skill["tags"])
        self.assertTrue(any(o["kind"] == "mechanic" and "shield" in o["tags"] for o in obs))

    def test_contract_scene_switch_scopes_bosses(self):
        from plugins.star_resonance_plugin.engines.boss_autokey_linkage import build_boss_reactions_state
        eng = self._engine()
        eng.on_mem_boss_action({"boss_base_id": 500, "skill_id": 700, "cast_edge": "start"})
        eng._state_mgr.update(dungeon_scene_id=202, dungeon_name="另一个场景")
        eng.on_mem_boss_action({"boss_base_id": 600, "skill_id": 800, "cast_edge": "start"})
        st101 = build_boss_reactions_state({}, eng, eng._state_mgr, scene_key="101")
        st202 = build_boss_reactions_state({}, eng, eng._state_mgr, scene_key="202")
        self.assertEqual([b["base_id"] for b in st101["bosses"]], [500])
        self.assertEqual([b["base_id"] for b in st202["bosses"]], [600])

    def test_contract_boss_detail_grouped_and_scoped(self):
        from plugins.star_resonance_plugin.engines.boss_autokey_linkage import build_boss_reactions_state
        eng = self._engine()
        eng.on_mem_boss_action({"boss_base_id": 500, "boss_name": "苍之冠", "boss_uuid": 1,
                                "skill_id": 700, "skill_name": "角斗同步", "cast_edge": "start",
                                "cast_duration_ms": 3000})
        eng.on_boss_event({"event_type": 47, "host_uuid": 1})   # SHIELD_BROKEN mechanic
        st = build_boss_reactions_state({}, eng, eng._state_mgr, boss_base_id=500)
        self.assertEqual(st["selected_boss_base_id"], 500)
        bd = st["boss_detail"]
        self.assertIsNotNone(bd)
        self.assertEqual(bd["base_id"], 500)
        self.assertEqual(bd["name"], "苍之冠")
        self.assertTrue(any(s["id"] == 700 for s in bd["skills"]))
        self.assertTrue(any(m["id"] == 47 for m in bd["mechanics"]))
        self.assertGreaterEqual(bd["summary"]["skill_count"], 1)
        self.assertGreaterEqual(bd["summary"]["mechanic_count"], 1)
        # back-compat: observed_skills still carries the selected boss
        self.assertIn("500", st["observed_skills"])

    def test_contract_light_status_has_no_entities(self):
        # the editor path must not pay the O(N) entity build (crowd-lag fix)
        from plugins.star_resonance_plugin.engines.boss_autokey_linkage import build_boss_reactions_state
        eng = self._engine()
        eng.on_mem_boss_action({"boss_base_id": 500, "skill_id": 700, "cast_edge": "start"})
        light = eng.get_status(include_entities=False)
        self.assertEqual(light["entities"], [])
        self.assertEqual(eng.get_status()["entities"], light["entities"])  # both empty here, but call path differs
        # build_boss_reactions_state must succeed using the light read
        st = build_boss_reactions_state({}, eng, eng._state_mgr)
        self.assertTrue(st["ok"])


class ContractUnitTest(unittest.TestCase):
    # Pure-function coverage for the reactions contract (no game/engine).

    def test_name_helpers_never_throw(self):
        from plugins.star_resonance_plugin.engines.boss_autokey_linkage import (
            _name_resolver, _resolve_skill_name, _resolve_boss_name, _resolve_scene_name)
        nm = _name_resolver()
        self.assertIsInstance(_resolve_skill_name(nm, 700), str)
        self.assertIsInstance(_resolve_boss_name(nm, 500), str)
        self.assertIsInstance(_resolve_scene_name(nm, 101, 0), str)
        self.assertEqual(_resolve_skill_name(None, 700), "")   # no resolver → empty
        self.assertEqual(_resolve_boss_name(None, 0), "")

    def test_build_boss_detail_groups_and_sorts_timeline(self):
        from plugins.star_resonance_plugin.engines.boss_autokey_linkage import _build_boss_detail
        obs = [
            {"id": 2, "skill_id": 2, "kind": "skill", "name": "B", "count": 3,
             "elapsed_s": 30.0, "time_fixed_s": 30.0, "last_cast_duration_ms": 1200},
            {"id": 1, "skill_id": 1, "kind": "skill", "name": "A", "count": 2, "elapsed_s": 10.0},
            {"id": 47, "kind": "mechanic", "name": "破盾", "count": 1, "elapsed_s": 20.0,
             "tags": ["shield"]},
            {"id": 9, "kind": "state", "name": "狂暴", "count": 1, "tags": ["enrage"],
             "hp_line_pct": 0.5},   # no time → excluded from timeline
        ]
        bd = _build_boss_detail(500, "苍之冠", obs)
        self.assertEqual(bd["base_id"], 500)
        self.assertEqual([s["id"] for s in bd["skills"]], [2, 1])
        self.assertEqual([m["id"] for m in bd["mechanics"]], [47, 9])
        self.assertEqual([t["id"] for t in bd["timeline"]], [1, 47, 2])   # by at_s 10,20,30
        self.assertTrue(bd["timeline"][2]["is_fixed"])      # id 2 had time_fixed_s
        self.assertFalse(bd["timeline"][0]["is_fixed"])     # id 1 only elapsed_s
        self.assertEqual(bd["summary"]["skill_count"], 2)
        self.assertEqual(bd["summary"]["mechanic_count"], 2)
        self.assertEqual(bd["summary"]["approx_duration_ms"], 1200)
        self.assertIn(0.5, bd["summary"]["hp_lines"])


if __name__ == "__main__":
    loader = unittest.TestLoader()
    suite = unittest.TestSuite([loader.loadTestsFromTestCase(StoreUnitTest),
                                loader.loadTestsFromTestCase(EngineRecordingTest),
                                loader.loadTestsFromTestCase(ContractUnitTest)])
    res = unittest.TextTestRunner(verbosity=1).run(suite)
    print(f"\n{res.testsRun - len(res.failures) - len(res.errors)} passed, "
          f"{len(res.failures) + len(res.errors)} failed")
    sys.exit(1 if (res.failures or res.errors) else 0)
