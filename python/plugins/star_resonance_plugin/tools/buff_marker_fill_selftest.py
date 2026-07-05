# -*- coding: utf-8 -*-
# Selftest: buff_marker_fill — 段内自动匹配/argmax防串绑/黑名单/CURATED/合并不覆盖。
from __future__ import annotations

import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from plugins.star_resonance_plugin.engines.buff_marker_fill import (                              # noqa: E402
    normalize_name, match_score, auto_match_buffs, apply_fill)


def _profile(pat, mechs):
    return {"target_name_pattern": pat, "mechanics": mechs}


def _mech(mid, name, buff_ids=None):
    return {"id": mid, "name": name,
            "detect": {"skill_ids": [], "buff_ids": list(buff_ids or [])},
            "dodge": {"inline": {"geometry": {"shape": "", "radius": 0.0, "source": ""}}}}


class BuffMarkerFillTest(unittest.TestCase):
    def test_normalize_strips_boss_prefix(self):
        self.assertEqual(normalize_name("石头人-碎地重拳打飞"), "碎地重拳打飞")
        self.assertEqual(normalize_name("玩家-被落石点名"), "落石")
        self.assertEqual(normalize_name("虚蚀龙-分摊"), "分摊")

    def test_match_score_exact_contain(self):
        self.assertEqual(match_score("虚蚀龙-分摊", "虚蚀龙-分摊"), 1.0)
        self.assertEqual(match_score("虚蚀龙-点T", "虚蚀龙-点T致死"), 0.8)
        self.assertLess(match_score("旋转子弹", "回旋球斥力"), 0.5)

    def test_argmax_keeps_phantom_variants_apart(self):
        # 分摊 buff→分摊机制, 幻分摊 buff→幻分摊机制 (含关系不串绑)
        prof = _profile("悖与灾的机骸·始",
                        [_mech("m1", "分摊"), _mech("m2", "幻分摊")])
        res = auto_match_buffs(prof, {829115: "分摊", 829116: "幻分摊"})
        got = {(m, b) for m, b, _n, _s in res["bound"]}
        self.assertIn(("m1", 829115), got)
        self.assertIn(("m2", 829116), got)

    def test_blacklist_and_segment(self):
        prof = _profile("悖与灾的机骸·始", [_mech("m1", "分摊")])
        res = auto_match_buffs(prof, {
            829120: "房间召唤怪物天生buff",     # 黑名单(天生)
            829315: "分摊",                     # 段外(终段)
        })
        self.assertEqual(res["bound"], [])

    def test_apply_merge_no_dupe_no_remove(self):
        prof = _profile("悖与灾的机骸·始", [_mech("m1", "分摊", buff_ids=[111, 829115])])
        out = apply_fill(prof, {829115: "分摊"})
        self.assertEqual(out["filled"], [])     # 已有→不重复写
        self.assertEqual(prof["mechanics"][0]["detect"]["buff_ids"], [111, 829115])

    def test_curated_and_geometry_line(self):
        prof = _profile("幻花的陈骸", [_mech("mech_10290113", "石头人地震波")])
        out = apply_fill(prof, {})
        det = prof["mechanics"][0]["detect"]
        self.assertIn(827220, det["buff_ids"])
        self.assertIn(827270, det["buff_ids"])
        g = prof["mechanics"][0]["dodge"]["inline"]["geometry"]
        self.assertEqual(g["shape"], "line")
        self.assertEqual(g["source"], "reverse")
        self.assertEqual(out["geometry"], ["mech_10290113"])

    def test_geometry_no_overwrite(self):
        prof = _profile("幻花的陈骸", [_mech("mech_10290113", "石头人地震波")])
        prof["mechanics"][0]["dodge"]["inline"]["geometry"] = {
            "shape": "circle", "radius": 7.0, "source": "runtime"}
        apply_fill(prof, {})
        g = prof["mechanics"][0]["dodge"]["inline"]["geometry"]
        self.assertEqual(g["shape"], "circle")  # 运行时已填→不覆盖

    def test_suggest_band_not_written(self):
        # 0.5~0.8 只进 suggest 不写入
        prof = _profile("悖与灾的机骸·始", [_mech("m1", "贯穿电磁脉冲")])
        out = apply_fill(prof, {829104: "电磁脉冲点名A"})
        auto_part = [f for f in out["filled"] if f[2] == "auto"]
        self.assertEqual(auto_part, [])
        self.assertTrue(any(b == 829104 for _m, b, _n, _s in out["suggest"]))


if __name__ == "__main__":
    suite = unittest.TestLoader().loadTestsFromTestCase(BuffMarkerFillTest)
    res = unittest.TextTestRunner(verbosity=1).run(suite)
    print(f"\n{res.testsRun - len(res.failures) - len(res.errors)} passed, "
          f"{len(res.failures) + len(res.errors)} failed")
    sys.exit(1 if (res.failures or res.errors) else 0)
