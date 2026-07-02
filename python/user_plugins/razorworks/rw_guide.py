# -*- coding: utf-8 -*-
"""RW module — grenade/guide.

Two modes:
  Guide:  Overlay overlay draws aim direction + text when near a lineup spot.
  Auto:   Automatically set view angles + execute throw sequence.

Data format (per lineup, compatible with Valthrun/rainedot):
  {
    "pos": [x, y, z],       // stand position
    "ang": [pitch, yaw],    // view angles
    "map": "de_mirage",
    "name": "A Site Smoke",
    "grenade": "smoke",      // smoke|flash|he|molotov
    "throw_type": "jump",    // normal|jump|walk|run
    "duck": false,
    "description": "..."
  }

Data sources: built-in nades/*.json + user-imported files.
"""

from __future__ import annotations

import json
import math
import os
from typing import Dict, List, Optional, Sequence, Tuple

Vec3 = Tuple[float, float, float]

PROXIMITY_THRESHOLD = 128.0  # game units — max distance to activate
ANG_TOLERANCE = 3.0          # degrees — "close enough" for auto mode

VK_SPACE = 0x20


def _dist(a: Vec3, b: Vec3) -> float:
    return math.sqrt((a[0]-b[0])**2 + (a[1]-b[1])**2 + (a[2]-b[2])**2)


class GuidePt:
    __slots__ = ("pos", "ang", "name", "grenade", "throw_type",
                 "duck", "description")

    def __init__(self, data: dict):
        p = data.get("pos", [0, 0, 0])
        self.pos: Vec3 = (float(p[0]), float(p[1]), float(p[2]))
        a = data.get("ang", [0, 0])
        self.ang: Tuple[float, float] = (float(a[0]), float(a[1]))
        self.name: str = str(data.get("name", ""))
        self.grenade: str = str(data.get("grenade", "smoke"))
        self.throw_type: str = str(data.get("throw_type", "normal"))
        self.duck: bool = bool(data.get("duck", False))
        self.description: str = str(data.get("description", ""))


class GrenadeSim:
    """Source 2 CS2 grenade ballistic simulation.

    sv_gravity=800, grenade gravity scale ~0.5 → effective 400 u/s².
    Z axis is UP — gravity subtracts from vz.
    Per-grenade velocity: flash fastest, molotov slowest.
    """

    # Base throw velocity per throw mode (left-click full throw)
    _BASE_VEL = {
        'normal': 1.0, 'left': 1.0, 'right': 0.31,
        'leftright': 0.65, 'medium': 0.65,
        'jump': 1.0, 'jumpleft': 1.0, 'jumpright': 0.31,
        'walk': 0.37, 'run': 1.0, 'runjump': 1.0,
        'jumpleftright': 0.65,
    }

    # Max throw speed differs per grenade type (u/s)
    GRENADE_SPEED = {
        'smoke':   680.0,
        'flash':   900.0,
        'he':      750.0,
        'molotov': 500.0,
        'incendiary': 500.0,
        'decoy':   750.0,
    }

    # Molotov/incendiary shatters on impact — no bounce
    GRENADE_ELASTICITY = {
        'smoke': 0.45, 'flash': 0.45, 'he': 0.45,
        'molotov': 0.0, 'incendiary': 0.0, 'decoy': 0.45,
    }

    GRAVITY = 400.0       # sv_gravity(800) × grenade_scale(0.5)
    DEFAULT_ELASTICITY = 0.45
    AIR_DRAG = 0.0002
    STEP = 0.015
    MAX_TIME = 5.0

    def simulate(self, eye, pitch, yaw, throw_type='normal',
                 grenade_type='smoke'):
        base_speed = self.GRENADE_SPEED.get(grenade_type, 750.0)
        scale = self._BASE_VEL.get(throw_type, 1.0)
        speed = base_speed * scale
        elasticity = self.GRENADE_ELASTICITY.get(grenade_type,
                                                  self.DEFAULT_ELASTICITY)

        rad_p = math.radians(pitch)
        rad_y = math.radians(yaw)
        cp, sp = math.cos(rad_p), math.sin(rad_p)
        cy, sy = math.cos(rad_y), math.sin(rad_y)
        vx = speed * cp * cy
        vy = speed * cp * sy
        vz = -speed * sp        # pitch>0=look down → negative vz (downward)
        pos = list(eye)
        points = [tuple(pos)]
        for _ in range(int(self.MAX_TIME / self.STEP)):
            spd = math.sqrt(vx*vx + vy*vy + vz*vz)
            if spd > 0:
                drag = 1.0 - self.AIR_DRAG * spd * self.STEP
                vx *= drag; vy *= drag; vz *= drag
            vz -= self.GRAVITY * self.STEP   # gravity pulls DOWN (-Z)
            pos[0] += vx * self.STEP
            pos[1] += vy * self.STEP
            pos[2] += vz * self.STEP
            points.append(tuple(pos))
            if spd < 1.0:
                break
        return points


class GuideHelper:
    """Grenade lineup helper with guide + auto modes."""

    def __init__(self) -> None:
        self.enabled: bool = False
        self.auto_mode: bool = False
        self.proximity: float = PROXIMITY_THRESHOLD
        self._lineups: Dict[str, List[GuidePt]] = {}  # map → lineups
        self._active: Optional[GuidePt] = None
        self._auto_phase: int = 0  # 0=idle, 1=aiming, 2=throwing

    def load_builtin(self, nades_dir: str) -> int:
        """Load all .json files from nades/ directory. Returns count."""
        total = 0
        if not os.path.isdir(nades_dir):
            return 0
        for fn in os.listdir(nades_dir):
            if not fn.endswith(".json"):
                continue
            path = os.path.join(nades_dir, fn)
            total += self._load_file(path)
        return total

    def load_import(self, path: str) -> int:
        """Import a user-provided JSON lineup file."""
        return self._load_file(path)

    def _load_file(self, path: str) -> int:
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
        except Exception:
            return 0
        items = data if isinstance(data, list) else data.get("lineups", [])
        count = 0
        for item in items:
            if not isinstance(item, dict):
                continue
            map_name = str(item.get("map", "")).lower()
            if not map_name:
                continue
            lineup = GuidePt(item)
            if map_name not in self._lineups:
                self._lineups[map_name] = []
            self._lineups[map_name].append(lineup)
            count += 1
        return count

    def find_nearest(self, map_name: str, pos: Vec3) -> Optional[GuidePt]:
        """Find the closest lineup to *pos* on *map_name*."""
        lineups = self._lineups.get(map_name.lower(), [])
        best = None
        best_dist = self.proximity
        for lu in lineups:
            d = _dist(pos, lu.pos)
            if d < best_dist:
                best_dist = d
                best = lu
        self._active = best
        return best

    def guide_overlay_ops(self, lineup: GuidePt,
                          player_pos: Vec3,
                          player_angles: Tuple[float, float],
                          screen_w: int, screen_h: int) -> list:
        """Generate Overlay overlay ops for guide mode.

        Draws: target crosshair direction + distance + name + throw type.
        """
        ops = []
        cx, cy = screen_w // 2, screen_h // 2

        # Direction indicator: where to look
        target_yaw = lineup.ang[1]
        current_yaw = player_angles[1]
        yaw_diff = ((target_yaw - current_yaw + 180) % 360) - 180
        pitch_diff = lineup.ang[0] - player_angles[0]

        # Map angle diff to screen offset (rough: 1° ≈ 10px at 1080p)
        px_per_deg = screen_h / 90.0
        dx = int(yaw_diff * px_per_deg * 0.5)
        dy = int(pitch_diff * px_per_deg * 0.5)

        # Crosshair target
        tx, ty = cx + dx, cy + dy
        tx = max(20, min(screen_w - 20, tx))
        ty = max(20, min(screen_h - 20, ty))
        r = 8
        ops.append({"op": "oval", "x": tx-r, "y": ty-r, "w": r*2, "h": r*2,
                     "fill": "", "outline": "#00FF88", "width": 2})
        ops.append({"op": "line", "x1": tx-r-4, "y1": ty, "x2": tx+r+4, "y2": ty,
                     "fill": "#00FF88", "width": 1})
        ops.append({"op": "line", "x1": tx, "y1": ty-r-4, "x2": tx, "y2": ty+r+4,
                     "fill": "#00FF88", "width": 1})

        # Info text
        dist = _dist(player_pos, lineup.pos)
        grenade_icons = {"smoke": "🚬", "flash": "⚡", "he": "💥", "molotov": "🔥"}
        icon = grenade_icons.get(lineup.grenade, "●")
        throw_label = {
            "jump": "跳投", "walk": "走投", "run": "跑投", "normal": "站投",
            "left": "左键投", "right": "右键投", "leftright": "左右键投",
            "medium": "中力投", "jumpleft": "跳左投", "jumpright": "跳右投",
            "runjump": "跑跳投", "jumpleftright": "跳左右投",
        }.get(lineup.throw_type, lineup.throw_type)

        ops.append({"op": "text", "x": cx, "y": 40,
                     "text": f"{lineup.name}  [{throw_label}]",
                     "fill": "#00FF88", "size": 14, "anchor": "n", "bold": True})
        ops.append({"op": "text", "x": cx, "y": 58,
                     "text": f"{dist:.0f}u  {lineup.description}",
                     "fill": "#88CCAA", "size": 10, "anchor": "n"})

        # Angle readout
        ang_off = math.sqrt(yaw_diff**2 + pitch_diff**2)
        color = "#44FF44" if ang_off < 2.0 else ("#FFCC00" if ang_off < 5.0 else "#FF4444")
        ops.append({"op": "text", "x": cx, "y": 74,
                     "text": f"角度偏差 {ang_off:.1f}°",
                     "fill": color, "size": 10, "anchor": "n"})

        return ops

    def auto_tick(self, lineup: GuidePt,
                  player_angles: Tuple[float, float],
                  player_velocity=None) -> Optional[dict]:
        """Auto mode: 5-phase state machine.

        Phase 0: aim — smooth mouse toward target angles.
        Phase 1: pre-throw prep (duck if needed).
        Phase 2: execute throw (buttons based on throw_type).
        Phase 3: release all inputs.

        Returns action dict or None.
        """
        if not self.auto_mode or not lineup:
            self._auto_phase = 0
            return None

        yaw_diff = ((lineup.ang[1] - player_angles[1] + 180) % 360) - 180
        pitch_diff = lineup.ang[0] - player_angles[0]
        ang_off = math.sqrt(yaw_diff**2 + pitch_diff**2)

        if self._auto_phase == 0:
            # Phase 0: aim
            if ang_off > ANG_TOLERANCE:
                from .rw_math import angle_to_pixels
                smooth = min(0.4, 1.0 / max(1.0, ang_off))
                dx, dy = angle_to_pixels(pitch_diff * smooth, yaw_diff * smooth, 1.0, 1.0)
                return {"move": (dx, dy)}
            self._auto_phase = 1
            return None

        if self._auto_phase == 1:
            # Phase 1: pre-throw prep (duck if needed)
            actions = {}
            if lineup.duck:
                actions["duck"] = True
            self._auto_phase = 2
            return actions if actions else None

        if self._auto_phase == 2:
            # Phase 2: execute throw
            tt = lineup.throw_type
            actions = {"throw": tt}
            if "jump" in tt:
                actions["jump"] = True
            if tt in ("left", "jumpleft"):
                actions["buttons"] = ["attack"]
            elif tt in ("right", "jumpright"):
                actions["buttons"] = ["attack2"]
            elif tt in ("leftright", "jumpleftright", "medium"):
                actions["buttons"] = ["attack", "attack2"]
            else:
                actions["buttons"] = ["attack"]
            self._auto_phase = 3
            return actions

        if self._auto_phase == 3:
            # Phase 3: release
            self._auto_phase = 0
            return {"release_all": True}

        self._auto_phase = 0
        return None

    def trajectory(self, lineup: GuidePt, eye: Vec3) -> list:
        """Get simulated trajectory points for rendering."""
        sim = GrenadeSim()
        return sim.simulate(eye, lineup.ang[0], lineup.ang[1],
                            lineup.throw_type, lineup.grenade)

    @property
    def active_lineup(self) -> Optional[GuidePt]:
        return self._active

    @property
    def lineup_count(self) -> int:
        return sum(len(v) for v in self._lineups.values())

    @property
    def map_count(self) -> int:
        return len(self._lineups)

    def maps(self) -> List[str]:
        return sorted(self._lineups.keys())
