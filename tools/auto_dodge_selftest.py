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


if __name__ == "__main__":
    suite = unittest.TestSuite()
    for tc in (CamMathTest, CameraRelativeTest, WorldVecTest, DispatchTest):
        suite.addTests(unittest.TestLoader().loadTestsFromTestCase(tc))
    res = unittest.TextTestRunner(verbosity=1).run(suite)
    print(f"\n{res.testsRun - len(res.failures) - len(res.errors)} passed, "
          f"{len(res.failures) + len(res.errors)} failed")
    sys.exit(1 if (res.failures or res.errors) else 0)
