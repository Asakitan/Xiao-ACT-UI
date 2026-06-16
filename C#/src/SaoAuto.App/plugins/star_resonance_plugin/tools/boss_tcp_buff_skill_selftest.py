# -*- coding: utf-8 -*-
"""Selftest: pure-TCP boss-skill detection (diff monster.buff_list for new base_ids)
+ the memory-priority gate (memory is authoritative in hybrid, suppressing TCP)."""
from __future__ import annotations

import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from plugins.star_resonance_plugin.engines.boss_raid_engine import BossRaidEngine          # noqa: E402
from plugins.star_resonance_plugin.engines.game_state import GameStateManager              # noqa: E402


def _monster(uuid, buffs, **kw):
    md = {
        "uuid": uuid, "hp": 1000, "max_hp": 1000, "template_id": 90001,
        "name": "测试Boss", "breaking_stage": -1, "in_overdrive": False,
        "stunned": 0, "extinction_pct": 0.0,
        "buff_list": [{"buff_id": b, "duration": d} for b, d in buffs],
    }
    md.update(kw)
    return md


class TcpBuffSkillTest(unittest.TestCase):
    def setUp(self):
        import tempfile
        fd, self._store_path = tempfile.mkstemp(suffix=".json")
        os.close(fd)
        os.remove(self._store_path)
        os.environ["SAO_BOSS_SKILL_STORE"] = self._store_path

    def tearDown(self):
        os.environ.pop("SAO_BOSS_SKILL_STORE", None)
        if os.path.exists(self._store_path):
            os.remove(self._store_path)

    def _engine(self):
        self.fired = []
        eng = BossRaidEngine(GameStateManager(), settings={},
                             on_boss_action=lambda a: self.fired.append(a))
        return eng

    def test_new_buff_fires_skill_id_is_base_id(self):
        eng = self._engine()
        # first sight seeds baseline (persistent buffs) -> no fire
        eng.on_monster_update(_monster(1000, [(827170, -1), (501711, -1)]))
        self.assertEqual(self.fired, [], "persistent baseline must not fire")
        # a NEW buff appears -> one forward, skill_id == buff base_id, source tcp
        eng.on_monster_update(_monster(1000, [(827170, -1), (501711, -1), (827173, 3000)]))
        self.assertEqual(len(self.fired), 1)
        a = self.fired[0]
        self.assertEqual(a["skill_id"], 827173)
        self.assertEqual(a["cast_edge"], "start")
        self.assertEqual(a["cast_duration_ms"], 3000)
        self.assertEqual(a["source"], "tcp")
        self.assertEqual(a["boss_base_id"], 90001)

    def test_same_buffs_do_not_refire(self):
        eng = self._engine()
        eng.on_monster_update(_monster(1000, [(827170, -1)]))
        eng.on_monster_update(_monster(1000, [(827170, -1), (827173, 3000)]))
        self.fired.clear()
        eng.on_monster_update(_monster(1000, [(827170, -1), (827173, 3000)]))
        self.assertEqual(self.fired, [], "unchanged buff_list must not refire")

    def test_longest_duration_new_buff_is_primary(self):
        eng = self._engine()
        eng.on_monster_update(_monster(1000, [(1, -1)]))
        eng.on_monster_update(_monster(1000, [(1, -1), (700, 1000), (701, 5000)]))
        self.assertEqual(len(self.fired), 1)
        self.assertEqual(self.fired[0]["skill_id"], 701)   # 5000ms > 1000ms

    def test_enrage_timer_buff_is_not_reported_as_skill(self):
        eng = self._engine()
        eng.on_monster_update(_monster(1000, [(1, -1)]))
        eng.on_monster_update(_monster(1000, [(1, -1), (501712, 600000)]))
        self.assertEqual(self.fired, [], "hard-enrage timer buff is not a boss skill")

    def test_memory_priority_gate_suppresses_tcp(self):
        eng = self._engine()
        eng.on_monster_update(_monster(1000, [(827170, -1)]))   # seed baseline
        # a memory action arrives -> memory feed is live (hybrid)
        eng.on_mem_boss_action({"boss_base_id": 90001, "skill_id": 5,
                                "cast_edge": "none", "boss_uuid": 1000})
        self.fired.clear()
        # now a new buff via TCP must be SUPPRESSED (memory is authoritative)
        eng.on_monster_update(_monster(1000, [(827170, -1), (827173, 3000)]))
        self.assertEqual(self.fired, [], "TCP must be suppressed while memory feed is live")

    def test_observed_skill_recorded_from_tcp(self):
        eng = self._engine()
        eng.on_monster_update(_monster(1000, [(827170, -1)]))
        eng.on_monster_update(_monster(1000, [(827170, -1), (827173, 3000)]))
        obs = eng.get_observed_boss_skills(90001)
        ids = [r["skill_id"] for r in obs.get(90001, [])]
        self.assertIn(827173, ids, "TCP-detected skill must land in observed skills")

    def test_malformed_monster_numbers_keep_valid_boss_update(self):
        eng = self._engine()

        eng.on_monster_update(_monster(
            1000,
            [],
            hp="bad",
            max_hp=2000,
            shield_pct="bad",
            breaking_stage="bad",
            extinction_pct="nan",
        ))

        self.assertEqual(eng._boss_uuid, 1000)
        self.assertEqual(eng._boss_hp, 0)
        self.assertEqual(eng._boss_max_hp, 2000)
        self.assertEqual(eng._boss_breaking_stage, 0)


if __name__ == "__main__":
    suite = unittest.TestLoader().loadTestsFromTestCase(TcpBuffSkillTest)
    res = unittest.TextTestRunner(verbosity=1).run(suite)
    print(f"\n{res.testsRun - len(res.failures) - len(res.errors)} passed, "
          f"{len(res.failures) + len(res.errors)} failed")
    sys.exit(1 if (res.failures or res.errors) else 0)
