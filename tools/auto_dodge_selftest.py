# -*- coding: utf-8 -*-
"""Selftest: auto_dodge_director — camera-relative + world-vector → WASD mapping,
camera reader quaternion math, and the move dispatch (down/up pairing)."""
from __future__ import annotations

import math
import os
import sys
import time
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from engines.auto_dodge_director import (                       # noqa: E402
    AutoDodgeDirector, resolve_dodge_keys, world_vec_to_keys, direction_label,
)
from mem_probe.il2cpp.mem_camera_reader import (                # noqa: E402
    _quat_forward_xz, _quat_right_xz, _quat_pitch_deg,
)


class CamMathTest(unittest.TestCase):
    def test_live_quaternion_yields_known_basis(self):
        # 活体实测的相机朝向四元数 (game 2022.3.59f1)
        q = (0.0093, 0.9870, -0.1482, 0.0618)
        fwd = _quat_forward_xz(q)
        rgt = _quat_right_xz(q)
        self.assertAlmostEqual(fwd[0], 0.125, delta=0.02)
        self.assertAlmostEqual(fwd[1], -0.992, delta=0.02)
        self.assertAlmostEqual(rgt[0], -0.992, delta=0.02)
        # 俯角在真实相机范围
        self.assertTrue(3.0 <= abs(_quat_pitch_deg(q)) <= 80.0)
        yaw = math.degrees(math.atan2(fwd[0], fwd[1]))
        self.assertAlmostEqual(yaw, 172.8, delta=1.0)

    def test_forward_right_orthogonal(self):
        q = (0.0093, 0.9870, -0.1482, 0.0618)
        f = _quat_forward_xz(q)
        r = _quat_right_xz(q)
        dot = f[0] * r[0] + f[1] * r[1]
        self.assertAlmostEqual(dot, 0.0, delta=0.05)


class CameraRelativeTest(unittest.TestCase):
    def test_back_is_S(self):
        keys, lbl = resolve_dodge_keys({"direction": "back"})
        self.assertEqual(keys, ["S"])
        self.assertEqual(lbl, "向后撤")

    def test_back_left_is_S_A(self):
        keys, _ = resolve_dodge_keys({"direction": "back_left"})
        self.assertEqual(set(keys), {"S", "A"})

    def test_forward_right_is_W_D(self):
        keys, _ = resolve_dodge_keys({"direction": "forward_right"})
        self.assertEqual(set(keys), {"W", "D"})

    def test_camera_relative_needs_no_basis(self):
        # 相机相对方向连相机都不读
        keys, _ = resolve_dodge_keys({"direction": "right"}, cam_basis=None)
        self.assertEqual(keys, ["D"])

    def test_empty_direction_noop(self):
        keys, _ = resolve_dodge_keys({"direction": ""})
        self.assertEqual(keys, [])


class WorldVecTest(unittest.TestCase):
    def setUp(self):
        # 用实测相机基: forward=(0.125,-0.992) right=(-0.992,-0.125)
        self.basis = {"forward": (0.125, -0.992), "right": (-0.992, -0.125)}

    def test_world_minus_z_is_forward(self):
        # 世界 -Z ≈ 相机前向 → W
        keys = world_vec_to_keys(0.125, -0.992, self.basis["forward"],
                                 self.basis["right"])
        self.assertIn("W", keys)
        self.assertNotIn("S", keys)

    def test_world_plus_z_is_back(self):
        keys = world_vec_to_keys(-0.125, 0.992, self.basis["forward"],
                                 self.basis["right"])
        self.assertIn("S", keys)

    def test_away_point_moves_opposite(self):
        # 玩家在 (10,_,10), 危险点在 (10,_,20) → 远离 = -Z 方向 = 前向 = W
        keys, lbl = resolve_dodge_keys(
            {"direction": "away_point:10,20"},
            cam_basis=self.basis, player_pos=(10.0, 50.0, 10.0))
        self.assertIn("W", keys)
        self.assertEqual(lbl, "远离指定点")

    def test_away_boss_uses_danger_pos(self):
        keys, lbl = resolve_dodge_keys(
            {"direction": "away_boss"},
            cam_basis=self.basis, player_pos=(0.0, 50.0, 0.0),
            danger_pos=(0.0, 50.0, 10.0))
        self.assertIn("W", keys)   # boss 在 +Z, 远离 = -Z = 前向
        self.assertEqual(lbl, "远离危险源")

    def test_away_nearest_uses_danger_pos(self):
        keys, lbl = resolve_dodge_keys(
            {"direction": "away_nearest"},
            cam_basis=self.basis, player_pos=(0.0, 50.0, 0.0),
            danger_pos=(0.0, 50.0, 10.0))
        self.assertIn("W", keys)
        self.assertEqual(lbl, "远离最近威胁")

    def test_away_boss_degrades_without_danger(self):
        keys, lbl = resolve_dodge_keys(
            {"direction": "away_boss", "fallback_direction": "back"},
            cam_basis=self.basis, player_pos=(0.0, 50.0, 0.0))
        self.assertEqual(keys, ["S"])
        self.assertIn("降级", lbl)

    def test_world_mode_degrades_without_camera(self):
        keys, lbl = resolve_dodge_keys(
            {"direction": "world:1,0", "fallback_direction": "back_left"},
            cam_basis=None)
        self.assertEqual(set(keys), {"S", "A"})
        self.assertIn("降级", lbl)


class DispatchTest(unittest.TestCase):
    def test_move_presses_and_releases_all_keys(self):
        events = []
        d = AutoDodgeDirector(lambda k, down: events.append((k, down)),
                              gate=lambda: True)
        r = d.dodge({"direction": "back_left", "move_ms": 120})
        self.assertTrue(r["fired"])
        self.assertEqual(set(r["keys"]), {"S", "A"})
        time.sleep(0.4)
        downs = [k for k, dn in events if dn]
        ups = [k for k, dn in events if not dn]
        self.assertEqual(set(downs), {"S", "A"})
        self.assertEqual(set(ups), {"S", "A"})

    def test_gate_blocks_dispatch(self):
        events = []
        d = AutoDodgeDirector(lambda k, down: events.append((k, down)),
                              gate=lambda: False)
        d.dodge({"direction": "back", "move_ms": 120})
        time.sleep(0.2)
        self.assertEqual(events, [])

    def test_noop_direction_does_not_fire(self):
        events = []
        d = AutoDodgeDirector(lambda k, down: events.append((k, down)))
        r = d.dodge({"direction": ""})
        self.assertFalse(r["fired"])
        self.assertEqual(events, [])

    def test_closed_loop_stops_when_clear(self):
        # 玩家固定在原点, 危险源固定在 (0,_,3); safe_dist=2 → 已出圈, 闭环秒退不按键
        events = []
        d = AutoDodgeDirector(
            lambda k, down: events.append((k, down)),
            get_cam_basis=lambda: {"forward": (0.0, -1.0), "right": (-1.0, 0.0)},
            get_player_pos=lambda: (0.0, 0.0, 0.0), gate=lambda: True)
        r = d.dodge({"direction": "away_nearest", "exit_margin_m": 2.0, "move_ms": 1000},
                    danger_pos=(0.0, 0.0, 3.0),
                    get_danger_pos=lambda: (0.0, 0.0, 3.0))
        self.assertTrue(r["fired"])
        time.sleep(0.3)
        # dist=3 >= safe 2 → 一进循环就 break, 不应留下按住的键
        downs = [k for k, dn in events if dn]
        ups = [k for k, dn in events if not dn]
        self.assertEqual(set(downs), set(ups))   # 按下的都松开了

    def test_closed_loop_moves_then_releases_when_out(self):
        # 玩家逐步远离危险源, 到 safe_dist 后停; 用可变 danger 距离模拟出圈
        events = []
        state = {"d": 1.0}
        d = AutoDodgeDirector(
            lambda k, down: events.append((k, down)),
            get_cam_basis=lambda: {"forward": (0.0, -1.0), "right": (-1.0, 0.0)},
            get_player_pos=lambda: (0.0, 0.0, 0.0), gate=lambda: True)

        def danger():
            state["d"] += 1.5      # 每次读, 危险源"变远"模拟玩家撤离
            return (0.0, 0.0, state["d"])
        r = d.dodge({"direction": "away_boss", "exit_margin_m": 5.0, "move_ms": 2000},
                    danger_pos=(0.0, 0.0, 1.0), get_danger_pos=danger)
        self.assertTrue(r["fired"])
        self.assertIn("精准出圈", r["label"])
        time.sleep(0.6)
        downs = [k for k, dn in events if dn]
        ups = [k for k, dn in events if not dn]
        self.assertTrue(downs)                 # 出圈前确实按了键移动
        self.assertEqual(set(downs), set(ups)) # 出圈后全松开

    def test_is_clear_stops_loop_precisely(self):
        # is_clear() 优先于距离: 玩家离开 AOE 圈(is_clear→True)立即停, 不靠 exit_margin
        events = []
        state = {"in_zone": True, "ticks": 0}

        def is_clear():
            state["ticks"] += 1
            if state["ticks"] >= 2:    # 第2 tick 起"已出圈"
                state["in_zone"] = False
            return not state["in_zone"]
        d = AutoDodgeDirector(
            lambda k, down: events.append((k, down)),
            get_cam_basis=lambda: {"forward": (0.0, -1.0), "right": (-1.0, 0.0)},
            get_player_pos=lambda: (0.0, 0.0, 0.0), gate=lambda: True)
        # exit_margin 设很大(50m)永远到不了, 只能靠 is_clear 停 → 证明区域判定生效
        r = d.dodge({"direction": "away_nearest", "exit_margin_m": 50.0, "move_ms": 3000},
                    danger_pos=(0.0, 0.0, 1.0), get_danger_pos=lambda: (0.0, 0.0, 1.0),
                    is_clear=is_clear)
        self.assertTrue(r["fired"])
        self.assertIn("区域判定", r["label"])
        time.sleep(0.5)
        ups = [k for k, dn in events if not dn]
        self.assertTrue(ups)               # is_clear 触发后松键停了
        self.assertEqual(d._held, [])

    def test_walk_to_moves_toward_and_stops_at_arrive(self):
        # 玩家逐步靠近目标; 到 arrive_m 内停, 与躲避的"远离"相反(朝目标按键)
        events = []
        state = {"p": [0.0, 0.0, 0.0]}
        d = AutoDodgeDirector(
            lambda k, down: events.append((k, down)),
            get_cam_basis=lambda: {"forward": (0.0, -1.0), "right": (-1.0, 0.0)},
            get_player_pos=lambda: tuple(state["p"]), gate=lambda: True)

        def target():
            state["p"][2] += 1.0       # 每读一次玩家"靠近"目标(模拟位移)
            return (0.0, 0.0, 5.0)
        r = d.walk_to(target, arrive_m=1.5, max_ms=2000)
        self.assertTrue(r["fired"])
        time.sleep(0.5)
        downs = [k for k, dn in events if dn]
        ups = [k for k, dn in events if not dn]
        # 目标在 +Z, 相机前向=(0,-1)指向-Z → 朝目标(+Z)=相机后方 → 按 S
        self.assertIn("S", downs)
        self.assertEqual(set(downs), set(ups)) # 到位后全松开

    def test_walk_to_stops_when_unreadable_no_blind_walk(self):
        # ★安全: 读不到目标位 → 立即停, 不盲按键 (与躲避可降级纯按键不同)
        events = []
        d = AutoDodgeDirector(
            lambda k, down: events.append((k, down)),
            get_cam_basis=lambda: {"forward": (0.0, -1.0), "right": (-1.0, 0.0)},
            get_player_pos=lambda: (0.0, 0.0, 0.0), gate=lambda: True)
        d.walk_to(lambda: None, arrive_m=2.0, max_ms=2000)   # 目标恒 None
        time.sleep(0.3)
        self.assertEqual(events, [])           # 一个键都没按
        self.assertEqual(d._held, [])

    def test_walk_sequence_advances_by_is_arrived(self):
        # 序列走: is_arrived 推进/收尾, arrive_m 不提前打断
        events = []
        prog = {"idx": 0}
        d = AutoDodgeDirector(
            lambda k, down: events.append((k, down)),
            get_cam_basis=lambda: {"forward": (0.0, -1.0), "right": (-1.0, 0.0)},
            get_player_pos=lambda: (0.0, 0.0, 0.0), gate=lambda: True)
        targets = [(0.0, 0.0, 10.0), (10.0, 0.0, 0.0)]

        def cur():
            return targets[prog["idx"]] if prog["idx"] < len(targets) else None

        def arrived():
            prog["idx"] += 1               # 每 tick "进一个圈"
            return prog["idx"] >= len(targets)
        r = d.walk_to(cur, arrive_m=1.5, max_ms=2000, is_arrived=arrived,
                      label="按序走完编号圈")
        self.assertTrue(r["fired"])
        time.sleep(0.4)
        self.assertGreaterEqual(prog["idx"], len(targets))   # 走完了序列
        self.assertEqual(d._held, [])

    def test_walk_to_release_all_stops(self):
        events = []
        d = AutoDodgeDirector(
            lambda k, down: events.append((k, down)),
            get_cam_basis=lambda: {"forward": (0.0, -1.0), "right": (-1.0, 0.0)},
            get_player_pos=lambda: (0.0, 0.0, 0.0), gate=lambda: True)
        d.walk_to(lambda: (0.0, 0.0, 99.0), arrive_m=1.0, max_ms=3000)
        time.sleep(0.15)
        d.release_all()                       # F12
        time.sleep(0.2)
        self.assertEqual(d._held, [])         # 急停松键

    def test_epoch_single_flight_cancels_old_loop(self):
        # release_all (F12) 递增 epoch, 在途循环下一 tick 退出且松键
        events = []
        d = AutoDodgeDirector(
            lambda k, down: events.append((k, down)),
            get_cam_basis=lambda: {"forward": (0.0, -1.0), "right": (-1.0, 0.0)},
            get_player_pos=lambda: (0.0, 0.0, 0.0), gate=lambda: True)
        d.dodge({"direction": "away_nearest", "exit_margin_m": 50.0, "move_ms": 3000},
                danger_pos=(0.0, 0.0, 1.0), get_danger_pos=lambda: (0.0, 0.0, 1.0))
        time.sleep(0.15)
        d.release_all()             # F12
        time.sleep(0.2)
        ups = [k for k, dn in events if not dn]
        self.assertTrue(ups)        # release_all 松开了在途按键
        self.assertEqual(d._held, [])


if __name__ == "__main__":
    suite = unittest.TestSuite()
    for tc in (CamMathTest, CameraRelativeTest, WorldVecTest, DispatchTest):
        suite.addTests(unittest.TestLoader().loadTestsFromTestCase(tc))
    res = unittest.TextTestRunner(verbosity=1).run(suite)
    print(f"\n{res.testsRun - len(res.failures) - len(res.errors)} passed, "
          f"{len(res.failures) + len(res.errors)} failed")
    sys.exit(1 if (res.failures or res.errors) else 0)
