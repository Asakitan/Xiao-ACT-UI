# -*- coding: utf-8 -*-
"""Selftest: geometry_auto_fill — skill_id 精确填 + 名字相似度领域填(纯逻辑)。"""
from __future__ import annotations

import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from engines.geometry_auto_fill import (                            # noqa: E402
    name_similarity, fill_by_skill_id, auto_fill_from_fields)


def _profile(mechs):
    return {"target_name_pattern": "虚蚀龙", "mechanics": mechs}


def _mech(sid, name, shape=""):
    return {"id": "m%d" % sid, "name": name,
            "detect": {"skill_ids": [sid]},
            "dodge": {"inline": {"direction": "away_nearest",
                                 "geometry": {"shape": shape, "radius": 0.0,
                                              "source": ""}}}}


class GeometryAutoFillTest(unittest.TestCase):
    def test_fill_by_skill_id_exact(self):
        prof = _profile([_mech(10240117, "虚蚀龙-虚雾喷洒", "circle"),
                         _mech(10240114, "虚蚀龙-头部吐息", "cone")])
        n = fill_by_skill_id(prof, 10240117, shape="circle", radius=8.0)
        self.assertEqual(n, 1)
        g = prof["mechanics"][0]["dodge"]["inline"]["geometry"]
        self.assertEqual(g["radius"], 8.0)
        self.assertEqual(g["source"], "runtime")
        # 另一个机制不受影响
        self.assertEqual(prof["mechanics"][1]["dodge"]["inline"]["geometry"]["radius"], 0.0)

    def test_fill_by_skill_id_no_match(self):
        prof = _profile([_mech(10240117, "虚蚀龙-虚雾喷洒")])
        self.assertEqual(fill_by_skill_id(prof, 99999, shape="circle", radius=5), 0)

    def test_name_similarity(self):
        self.assertGreater(name_similarity("虚蚀龙-虚雾喷洒", "虚蚀龙-虚雾喷洒"), 0.99)
        self.assertGreater(name_similarity("虚蚀龙-传染虚雾", "虚蚀龙-虚雾喷洒"), 0.3)
        self.assertLess(name_similarity("炎光环形aoe", "冰霜风暴"), 0.2)

    def test_auto_fill_from_fields_high_score(self):
        # field "虚蚀龙-虚雾领域" 高相似 → 填进"虚蚀龙-虚雾喷洒"机制
        prof = _profile([_mech(10240117, "虚蚀龙-虚雾喷洒")])
        fields = [{"name": "虚蚀龙-虚雾领域", "shape": "circle", "radius": 6.0,
                   "inner": 0.0, "boss": "虚蚀龙"}]
        filled = auto_fill_from_fields(prof, fields, min_score=0.4)
        self.assertEqual(len(filled), 1)
        self.assertEqual(prof["mechanics"][0]["dodge"]["inline"]["geometry"]["radius"], 6.0)
        self.assertEqual(prof["mechanics"][0]["dodge"]["inline"]["geometry"]["source"], "reverse")

    def test_auto_fill_skips_low_score(self):
        # field 名字差太远 → 不填(避免错填)
        prof = _profile([_mech(10240117, "虚蚀龙-虚雾喷洒")])
        fields = [{"name": "冰领域", "shape": "circle", "radius": 8.0, "boss": ""}]
        filled = auto_fill_from_fields(prof, fields, min_score=0.5)
        self.assertEqual(filled, [])
        self.assertEqual(prof["mechanics"][0]["dodge"]["inline"]["geometry"]["radius"], 0.0)

    def test_auto_fill_skips_cross_boss(self):
        # boss 标识不一致 → 不填
        prof = _profile([_mech(10240117, "虚蚀龙-虚雾喷洒")])
        fields = [{"name": "炎光-虚雾喷洒领域", "shape": "circle", "radius": 9.0,
                   "boss": "炎光"}]
        self.assertEqual(auto_fill_from_fields(prof, fields, min_score=0.4), [])

    def test_auto_fill_no_overwrite_runtime(self):
        # 已填(运行时)的不被领域填覆盖
        prof = _profile([_mech(10240117, "虚蚀龙-虚雾喷洒")])
        fill_by_skill_id(prof, 10240117, shape="circle", radius=7.0)
        fields = [{"name": "虚蚀龙-虚雾领域", "shape": "circle", "radius": 6.0, "boss": "虚蚀龙"}]
        auto_fill_from_fields(prof, fields, min_score=0.4)
        self.assertEqual(prof["mechanics"][0]["dodge"]["inline"]["geometry"]["radius"], 7.0)  # 保留运行时


if __name__ == "__main__":
    suite = unittest.TestLoader().loadTestsFromTestCase(GeometryAutoFillTest)
    res = unittest.TextTestRunner(verbosity=1).run(suite)
    print(f"\n{res.testsRun - len(res.failures) - len(res.errors)} passed, "
          f"{len(res.failures) + len(res.errors)} failed")
    sys.exit(1 if (res.failures or res.errors) else 0)
