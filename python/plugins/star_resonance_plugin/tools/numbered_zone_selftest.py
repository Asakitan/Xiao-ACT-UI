# -*- coding: utf-8 -*-
# Selftest: numbered_zone_tracker — 编号圈出现顺序编号 + 成员命中归属(纯逻辑, 合成快照)。
from __future__ import annotations

import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from plugins.star_resonance_plugin.engines.numbered_zone_tracker import NumberedZoneTracker     # noqa: E402


def _z(uuid, members, group_id=10, base_id=1100, zone_type=1):
    return {"zone_uuid": uuid, "members": set(members), "group_id": group_id,
            "base_id": base_id, "zone_type": zone_type}


class NumberedZoneTest(unittest.TestCase):
    def test_persistent_region_not_numbered(self):
        t = NumberedZoneTracker()
        new = t.observe([_z(900, [1], base_id=1002, zone_type=-1)], now=0.0)
        self.assertEqual(new, [])                 # 1002/负type 持久区不编号
        self.assertEqual(t.active_sequence(), [])

    def test_sequence_numbering_by_appearance(self):
        t = NumberedZoneTracker()
        t.observe([_z(101, [1])], now=1.0)        # 圈1
        t.observe([_z(101, [1]), _z(102, [2])], now=2.0)   # 圈2 出现
        t.observe([_z(101, [1]), _z(102, [2]), _z(103, [])], now=3.0)  # 圈3
        seq = t.active_sequence()
        self.assertEqual([z.seq_in_group for z in seq], [1, 2, 3])
        self.assertEqual([z.zone_uuid for z in seq], [101, 102, 103])

    def test_group_isolation(self):
        t = NumberedZoneTracker()
        t.observe([_z(201, [1], group_id=10)], now=1.0)
        t.observe([_z(201, [1], group_id=10), _z(301, [2], group_id=20)], now=2.0)
        groups = t.active_groups()
        self.assertEqual(set(groups.keys()), {10, 20})
        self.assertEqual(groups[10][0].seq_in_group, 1)
        self.assertEqual(groups[20][0].seq_in_group, 1)   # 各组独立从1编号

    def test_damage_attributed_by_membership(self):
        t = NumberedZoneTracker()
        t.observe([_z(101, [11, 12]), _z(102, [13])], now=1.0)
        tz = t.observe_damage(skill_id=10330007, target_uuid=13, pos=None, now=1.1)
        self.assertIsNotNone(tz)
        self.assertEqual(tz.zone_uuid, 102)       # 13 只在圈102 → 精确归属
        self.assertEqual(tz.hits[0][0], 10330007)

    def test_damage_picks_latest_when_multiple(self):
        t = NumberedZoneTracker()
        # 玩家7 同时在圈1和圈2 → 归到后出现的(编号大)
        t.observe([_z(101, [7])], now=1.0)
        t.observe([_z(101, [7]), _z(102, [7])], now=2.0)
        tz = t.observe_damage(skill_id=1, target_uuid=7, pos=None, now=2.1)
        self.assertEqual(tz.zone_uuid, 102)

    def test_damage_falls_back_to_nearest_pos(self):
        t = NumberedZoneTracker()
        t.observe([_z(101, [11]), _z(102, [12])], now=1.0)
        # 给圈位置(经早先伤害补充)
        t.observe_damage(1, 11, (0.0, 0.0, 0.0), now=1.1)
        t.observe_damage(1, 12, (50.0, 0.0, 50.0), now=1.2)
        # 目标99 不在任何成员表 → 按 pos 最近(靠近圈101)
        tz = t.observe_damage(2, 99, (1.0, 0.0, 1.0), now=1.3)
        self.assertEqual(tz.zone_uuid, 101)

    def test_zone_disappears_marks_dead(self):
        t = NumberedZoneTracker()
        t.observe([_z(101, [1])], now=1.0)
        t.observe([], now=2.0)                    # 圈炸了消失
        self.assertEqual(t.active_sequence(), [])
        s = t.summary()
        self.assertEqual(len(s), 1)
        self.assertFalse(s[0]["alive"])           # 保留做统计

    def test_members_ever_accumulates(self):
        t = NumberedZoneTracker()
        t.observe([_z(101, [1, 2])], now=1.0)
        t.observe([_z(101, [3])], now=2.0)        # 成员变了
        s = t.summary()[0]
        self.assertEqual(s["members_ever"], [1, 2, 3])
        self.assertEqual(s["members_now"], [3])

    def test_prune_removes_old_dead(self):
        t = NumberedZoneTracker()
        t.observe([_z(101, [1])], now=1.0)
        t.observe([], now=2.0)                    # 死于 t=2
        t.prune(now=40.0, keep_s=30.0)            # 38s 后清掉
        self.assertEqual(t.summary(), [])

    def test_concurrent_observe_summary_no_crash(self):
        # 审查#3: 采样线程 observe/prune 与主线程 summary/active_sequence 并发不崩
        import threading
        t = NumberedZoneTracker()
        errors = []

        def writer():
            try:
                for i in range(400):
                    t.observe([_z(100 + (i % 7), [i, i + 1], group_id=i % 3)], now=float(i))
                    t.prune(now=float(i), keep_s=2.0)
            except Exception as e:
                errors.append(('writer', repr(e)))

        def reader():
            try:
                for i in range(400):
                    t.summary()
                    t.active_sequence()
                    t.observe_damage(1, i, (float(i), 0.0, 0.0), now=float(i))
            except Exception as e:
                errors.append(('reader', repr(e)))
        ths = [threading.Thread(target=writer) for _ in range(2)] + \
              [threading.Thread(target=reader) for _ in range(2)]
        for th in ths:
            th.start()
        for th in ths:
            th.join()
        self.assertEqual(errors, [])               # 无 "dict changed size during iteration"


if __name__ == "__main__":
    suite = unittest.TestLoader().loadTestsFromTestCase(NumberedZoneTest)
    res = unittest.TextTestRunner(verbosity=1).run(suite)
    print(f"\n{res.testsRun - len(res.failures) - len(res.errors)} passed, "
          f"{len(res.failures) + len(res.errors)} failed")
    sys.exit(1 if (res.failures or res.errors) else 0)
