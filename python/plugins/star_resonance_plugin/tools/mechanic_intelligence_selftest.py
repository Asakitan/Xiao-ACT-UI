# -*- coding: utf-8 -*-
# Selftest: mechanic_intelligence — 招名分类 + 机制壳子 + 范围填入 + 几何出圈判定。
from __future__ import annotations

import os
import sys
import time
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from plugins.star_resonance_plugin.engines.mechanic_intelligence import (                         # noqa: E402
    MechanicClassifier, build_mechanic_shell, fill_geometry)
from plugins.star_resonance_plugin.engines.boss_raid_engine import normalize_mechanic             # noqa: E402
from plugins.star_resonance_plugin.engines.auto_dodge_director import AutoDodgeDirector           # noqa: E402
from plugins.star_resonance_plugin.mem.il2cpp.mem_dodge_context import DodgeContext         # noqa: E402


class ClassifyTest(unittest.TestCase):
    def setUp(self):
        self.c = MechanicClassifier()

    def test_ring_needs_geometry_away(self):
        a = self.c.classify("炎光环形aoe")
        self.assertEqual(a.category, "ring")
        self.assertEqual(a.action, "away_nearest")
        self.assertTrue(a.needs_geometry)
        self.assertEqual(a.suggested_shape, "ring")

    def test_stack_goto_teammate_no_geometry(self):
        a = self.c.classify("炎光领地分摊")
        self.assertEqual(a.action, "goto_teammate")
        self.assertFalse(a.needs_geometry)       # 集合靠队友位, 不需范围

    def test_charge_breath_away_boss(self):
        self.assertEqual(self.c.classify("炎光角斗-开冲").action, "away_boss")
        self.assertEqual(self.c.classify("光龙吐息").action, "away_boss")

    def test_raidwide_summon_alert_only(self):
        self.assertEqual(self.c.classify("史诗狂怒").action, "")     # 全屏只提示
        self.assertEqual(self.c.classify("召唤小怪").action, "")
        self.assertFalse(self.c.classify("史诗狂怒").needs_geometry)

    def test_non_mechanic_skipped(self):
        for nm in ("炎光普攻01", "原地转向090", "向前瞬移", "初始待机"):
            self.assertFalse(self.c.classify(nm).is_mechanic, nm)


class ShellTest(unittest.TestCase):
    def test_shell_has_geometry_placeholder(self):
        m = build_mechanic_shell(10280006, "炎光环形aoe")
        self.assertIsNotNone(m)
        geom = m["dodge"]["inline"]["geometry"]
        self.assertEqual(geom["shape"], "ring")   # 建议形状占位
        self.assertEqual(geom["radius"], 0.0)     # 待填
        self.assertEqual(geom["source"], "")      # 未填标记
        self.assertTrue(m["_needs_geometry"])
        # detect 绑真实 skill_id
        self.assertEqual(m["detect"]["skill_ids"], [10280006])

    def test_shell_non_mechanic_none(self):
        self.assertIsNone(build_mechanic_shell(10280001, "炎光普攻01"))

    def test_fill_geometry(self):
        m = build_mechanic_shell(10280006, "炎光环形aoe")
        ok = fill_geometry(m, shape="ring", radius=10.0, inner=3.0, center="boss")
        self.assertTrue(ok)
        geom = m["dodge"]["inline"]["geometry"]
        self.assertEqual(geom["radius"], 10.0)
        self.assertEqual(geom["source"], "reverse")
        self.assertNotIn("_needs_geometry", m)    # 填后清标记

    def test_shell_normalizes_clean(self):
        m = build_mechanic_shell(10280006, "炎光环形aoe")
        nm = normalize_mechanic(m) if False else m
        # geometry 经引擎 normalize 往返 (壳子可被引擎接受)
        from plugins.star_resonance_plugin.engines.boss_raid_engine import normalize_dodge
        d = normalize_dodge(m["dodge"])
        self.assertIn("geometry", d["inline"])


class GeometryDodgeTest(unittest.TestCase):
    def test_geometry_clear_check_unfilled_returns_none(self):
        # 占位(source='')→ make_geometry_clear_check 返回 None (用招式生命周期兜底)
        ctx = DodgeContext.__new__(DodgeContext)   # 不连真进程
        ctx.get_player_pos = lambda: (0.0, 0.0, 0.0)
        geom = {"shape": "ring", "radius": 0.0, "source": ""}
        self.assertIsNone(ctx.make_geometry_clear_check(geom, lambda: (0, 0, 10)))

    def test_geometry_clear_check_circle_filled(self):
        # 填了 circle r=5, 中心(0,0,0): 玩家在(0,0,3)内圈→未清; (0,0,6)出圈→清
        ctx = DodgeContext.__new__(DodgeContext)
        state = {"p": (0.0, 0.0, 3.0)}
        ctx.get_player_pos = lambda: state["p"]
        geom = {"shape": "circle", "radius": 5.0, "center": "boss", "source": "reverse"}
        chk = ctx.make_geometry_clear_check(geom, lambda: (0.0, 0.0, 0.0))
        self.assertIsNotNone(chk)
        self.assertFalse(chk())                   # 距3 < r5 → 在范围内
        state["p"] = (0.0, 0.0, 6.0)
        self.assertTrue(chk())                    # 距6 ≥ r5 → 出圈清

    def test_geometry_ring_inner_safe(self):
        # 环形 inner=2 outer=8: 中心(距1)安全, 环中(距5)危险, 环外(距9)安全
        ctx = DodgeContext.__new__(DodgeContext)
        state = {"p": (0.0, 0.0, 5.0)}
        ctx.get_player_pos = lambda: state["p"]
        geom = {"shape": "ring", "radius": 8.0, "inner": 2.0, "source": "reverse"}
        chk = ctx.make_geometry_clear_check(geom, lambda: (0.0, 0.0, 0.0))
        self.assertFalse(chk())                   # 距5 在环内 → 危险
        state["p"] = (0.0, 0.0, 1.0)
        self.assertTrue(chk())                    # 距1 ≤ inner → 内圈安全
        state["p"] = (0.0, 0.0, 9.0)
        self.assertTrue(chk())                    # 距9 ≥ outer → 环外安全


if __name__ == "__main__":
    suite = unittest.TestSuite()
    for tc in (ClassifyTest, ShellTest, GeometryDodgeTest):
        suite.addTests(unittest.TestLoader().loadTestsFromTestCase(tc))
    res = unittest.TextTestRunner(verbosity=1).run(suite)
    print(f"\n{res.testsRun - len(res.failures) - len(res.errors)} passed, "
          f"{len(res.failures) + len(res.errors)} failed")
    sys.exit(1 if (res.failures or res.errors) else 0)
