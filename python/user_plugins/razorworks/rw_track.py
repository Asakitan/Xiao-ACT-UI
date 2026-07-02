# -*- coding: utf-8 -*-
"""RW module — precision tracker with target selection strategies.

Target selection strategies:
  crosshair — closest to crosshair center (smallest angular distance)
  distance  — closest to local player (smallest 3D distance)
  health    — lowest HP enemy

Multi-bone priority:
  primary_bones = ["head", "neck"]     — try these first
  secondary_bones = ["chest", "pelvis"] — fallback if no primary in FOV
  For each enemy, the first available bone in priority order is used.
"""

from __future__ import annotations

import math
import random
import time
from typing import List, Optional, Sequence, Tuple

from .rw_mesh import TRACKER_BONES
from .rw_math import (
    Vec3, angle_diff, angle_fov, angle_to, angle_to_pixels,
    distance_3d, smooth_angle,
)
from .rw_sight import BONE_BODYPART, find_best_visible_point

_MICRO_THRESHOLD = 0.5
HEAD_Z_OFFSET = -1.0
MOVE_ACCURACY_THRESH = 88.0
RECOIL_SCALE = 2.0


def _ease_out_cubic(t: float) -> float:
    t = max(0.0, min(1.0, t))
    return 1.0 - (1.0 - t) ** 3


def _ease_out_overshoot(t: float, k: float = 0.15) -> float:
    t = max(0.0, min(1.0, t))
    return 1.0 - (1.0 - t) ** 3 + k * math.sin(math.pi * t) * (1.0 - t)


def _jitter(sigma: float) -> Tuple[float, float]:
    if sigma <= 0:
        return (0.0, 0.0)
    return (random.gauss(0, sigma), random.gauss(0, sigma))


def _resolve_bone_pos(entity: dict, bone_names: List[str],
                      eye: Optional[Vec3] = None,
                      view_angles: Optional[tuple] = None,
                      fov: float = 90.0,
                      edge_count: int = 9) -> Optional[Vec3]:
    """Get the best VISIBLE edge point from priority bone list.

    For each bone, scans edge sample points and returns the one closest
    to the crosshair. Even a tiny sliver of exposed hitbox is aimable.
    Falls back to bone center if edge scan is unavailable.
    """
    bones = entity.get("bones")
    if not bones:
        return entity.get("head_pos")

    # If we have eye/angles, do edge scanning for precise edge aiming
    if eye and view_angles:
        # Also check pre-computed visible_points from snapshot
        vis_pts = entity.get("visible_points")
        if vis_pts:
            for name in bone_names:
                idx = TRACKER_BONES.get(name.lower())
                if idx is not None and idx in vis_pts:
                    pos = vis_pts[idx]
                    if name == "head":
                        return (pos[0], pos[1], pos[2] + HEAD_Z_OFFSET)
                    return pos

        # Live edge scan per bone
        for name in bone_names:
            idx = TRACKER_BONES.get(name.lower())
            if idx is None or idx not in bones:
                continue
            part = BONE_BODYPART.get(idx, "chest")
            best = find_best_visible_point(
                bones[idx], part, eye, view_angles, fov, edge_count)
            if best:
                if name == "head":
                    return (best[0], best[1], best[2] + HEAD_Z_OFFSET)
                return best

    # Fallback: bone center
    for name in bone_names:
        idx = TRACKER_BONES.get(name.lower())
        if idx is not None and idx in bones:
            pos = bones[idx]
            if name == "head":
                return (pos[0], pos[1], pos[2] + HEAD_Z_OFFSET)
            return pos
    return entity.get("head_pos")


def _select_target(eye: Vec3, view_angles: Tuple[float, float],
                   candidates: Sequence[dict], strategy: str,
                   fov: float, max_distance: float,
                   bone_names: List[str],
                   edge_count: int = 9) -> Optional[Tuple[dict, Vec3]]:
    """Select best target using chosen strategy. Returns (entity, aim_pos)."""
    best = None
    best_score = float("inf")
    best_pos = None

    for e in candidates:
        pos = _resolve_bone_pos(e, bone_names, eye, view_angles, fov, edge_count)
        if not pos:
            continue
        dist = distance_3d(eye, pos)
        if max_distance > 0 and dist > max_distance:
            continue
        aim = angle_to(eye, pos)
        fov_dist = angle_fov(view_angles, aim)
        if fov_dist > fov:
            continue

        if strategy == "distance":
            score = dist
        elif strategy == "health":
            score = int(e.get("health", 100))
        else:  # crosshair
            score = fov_dist

        if score < best_score:
            best_score = score
            best = e
            best_pos = pos

    if best is None:
        return None
    return (best, best_pos)


class TrackState:

    def __init__(self) -> None:
        self.enabled: bool = False

        # targeting
        self.fov: float = 5.0
        self.strategy: str = "crosshair"       # crosshair | distance | health
        self.primary_bones: List[str] = ["head"]
        self.secondary_bones: List[str] = ["neck", "chest"]
        self.max_distance: float = 0
        self.visible_only: bool = True

        # speed & smoothing
        self.smooth: float = 0.25
        self.speed_scale: float = 1.0
        self.overshoot: float = 0.0
        self.curve: str = "ease_out"

        # humanisation
        self.jitter: float = 0.06
        self.random_offset: float = 0.0
        self.reaction_ms: int = 0

        # hit-rate
        self.hit_rate: float = 1.0
        self.miss_jitter: float = 0.4

        # precision
        self.rcs_integrated: bool = True
        self.movement_gate: bool = True

        # sensitivity
        self.sensitivity: float = 1.0
        self.fov_scale: float = 1.0

        # internals
        self._locked_index: int = -1
        self._lock_time: float = 0.0
        self._last_tick_time: float = 0.0
        self.smooth_lambda: float = 6.0
        self._initial_delta: float = 0.0
        self._off_p: float = 0.0
        self._off_y: float = 0.0
        self._reaction_delay: float = 0.0
        self._ar_p: float = 0.0
        self._ar_y: float = 0.0
        self._bez_p1: Tuple[float, float] = (0.0, 0.0)
        self._bez_p2: Tuple[float, float] = (0.0, 0.0)

    def reset(self) -> None:
        self._locked_index = -1
        self._lock_time = 0.0
        self._last_tick_time = 0.0

    def tick(self, local: Optional[dict],
             enemies: Sequence[dict]) -> Optional[Tuple[int, int]]:
        if not self.enabled or not local:
            self.reset()
            return None

        eye = local.get("eye_pos")
        angles = local.get("view_angles")
        if not eye or not angles:
            return None

        if self.movement_gate:
            vel = local.get("velocity", (0, 0, 0))
            if vel and math.hypot(vel[0], vel[1]) > MOVE_ACCURACY_THRESH:
                return None

        candidates = list(enemies)
        if self.visible_only:
            candidates = [e for e in candidates if e.get("is_visible", True)]
        if not candidates:
            self.reset()
            return None

        now = time.perf_counter()

        if self.hit_rate < 1.0 and random.random() > self.hit_rate:
            if self.miss_jitter > 0:
                jp, jy = _jitter(self.miss_jitter)
                return angle_to_pixels(jp, jy, self.sensitivity, self.fov_scale)
            return None

        # --- Target selection with bone priority ---
        # Sticky target: keep if still valid
        locked_result = None
        if self._locked_index >= 0:
            for e in candidates:
                if e.get("index") == self._locked_index:
                    pos = _resolve_bone_pos(e, self.primary_bones + self.secondary_bones,
                                            eye, angles, self.fov)
                    if pos and angle_fov(angles, angle_to(eye, pos)) < self.fov * 2.0:
                        locked_result = (e, pos)
                    break

        if locked_result is None:
            # Try primary bones first
            result = _select_target(eye, angles, candidates, self.strategy,
                                    self.fov, self.max_distance, self.primary_bones)
            # Fallback to secondary bones
            if result is None and self.secondary_bones:
                result = _select_target(eye, angles, candidates, self.strategy,
                                        self.fov, self.max_distance, self.secondary_bones)
        else:
            result = locked_result

        if result is None:
            self.reset()
            return None

        target, aim_pos = result

        # New target
        if target.get("index") != self._locked_index:
            self._locked_index = target.get("index", -1)
            self._lock_time = now
            self._initial_delta = angle_fov(angles, angle_to(eye, aim_pos))
            if self.random_offset > 0:
                self._off_p = random.gauss(0, self.random_offset)
                self._off_y = random.gauss(0, self.random_offset)
            else:
                self._off_p = self._off_y = 0.0
            # P0-8: log-normal reaction time
            if self.reaction_ms > 0:
                sigma_n = max(0.01, self.reaction_ms * 0.2) / max(1, self.reaction_ms)
                mu_ln = math.log(max(1, self.reaction_ms)) - 0.5 * sigma_n * sigma_n
                self._reaction_delay = random.lognormvariate(mu_ln, sigma_n)
                self._reaction_delay = max(80.0, min(500.0, self._reaction_delay))
            else:
                self._reaction_delay = 0.0
            # P2-1: bezier random path control points
            dp_full, dy_full = angle_diff(angles, angle_to(eye, aim_pos))
            mag = math.hypot(dp_full, dy_full)
            if mag > 0.1:
                perp_p = -dy_full / mag
                perp_y = dp_full / mag
                self._bez_p1 = (angles[0] + dp_full * 0.3 + perp_p * random.gauss(0, 0.15 * mag),
                                angles[1] + dy_full * 0.3 + perp_y * random.gauss(0, 0.15 * mag))
                self._bez_p2 = (angles[0] + dp_full * 0.7 + perp_p * random.gauss(0, 0.08 * mag),
                                angles[1] + dy_full * 0.7 + perp_y * random.gauss(0, 0.08 * mag))
            else:
                self._bez_p1 = (0.0, 0.0)
                self._bez_p2 = (0.0, 0.0)

        if self._reaction_delay > 0 and (now - self._lock_time) * 1000 < self._reaction_delay:
            return None

        desired = angle_to(eye, aim_pos)

        # RCS integrated
        if self.rcs_integrated:
            punch = local.get("aim_punch")
            if punch:
                desired = (desired[0] - punch[0] * RECOIL_SCALE,
                           desired[1] - punch[1] * RECOIL_SCALE)

        desired = (desired[0] + self._off_p, desired[1] + self._off_y)
        diff = angle_fov(angles, desired)
        if diff < 0.015:
            return None

        # Micro-correction
        if diff < _MICRO_THRESHOLD:
            dp, dy = angle_diff(angles, desired)
            micro = 0.15 + 0.1 * random.random()
            dp *= micro; dy *= micro
            if self.jitter > 0:
                sigma_scaled = self.jitter * 0.3 * (0.3 + 0.7 * min(1.0, diff / max(0.1, self.fov)))
                self._ar_p = 0.6 * self._ar_p + 0.4 * random.gauss(0, sigma_scaled * 1.3)
                self._ar_y = 0.6 * self._ar_y + 0.4 * random.gauss(0, sigma_scaled)
                tremor = 0.08 * math.sin(2 * math.pi * 3.5 * now)
                dp += self._ar_p + tremor
                dy += self._ar_y
            return angle_to_pixels(dp, dy, self.sensitivity, self.fov_scale)

        # Main curve — frame-rate independent exponential decay
        dt = now - self._last_tick_time if self._last_tick_time > 0 else 1 / 128
        self._last_tick_time = now
        dt = max(0.001, min(dt, 0.1))
        factor = 1.0 - math.exp(-self.smooth_lambda * dt)
        if self.overshoot > 0:
            factor *= (1.0 + self.overshoot * math.sin(math.pi * factor) * (1.0 - factor))
        smoothed = smooth_angle(angles, desired, factor)

        # P2-1: bezier random path for large corrections
        if self._initial_delta > 1.0 and factor < 0.95:
            t = factor
            p0, p3 = angles, desired
            p1, p2 = self._bez_p1, self._bez_p2
            bz_p = (1 - t) ** 3 * p0[0] + 3 * (1 - t) ** 2 * t * p1[0] + 3 * (1 - t) * t ** 2 * p2[0] + t ** 3 * p3[0]
            bz_y = (1 - t) ** 3 * p0[1] + 3 * (1 - t) ** 2 * t * p1[1] + 3 * (1 - t) * t ** 2 * p2[1] + t ** 3 * p3[1]
            smoothed = (bz_p, bz_y)

        dp, dy = angle_diff(angles, smoothed)
        if self.jitter > 0:
            sigma_scaled = self.jitter * (0.3 + 0.7 * min(1.0, diff / max(0.1, self.fov)))
            self._ar_p = 0.6 * self._ar_p + 0.4 * random.gauss(0, sigma_scaled * 1.3)
            self._ar_y = 0.6 * self._ar_y + 0.4 * random.gauss(0, sigma_scaled)
            tremor = 0.08 * math.sin(2 * math.pi * 3.5 * now)
            dp += self._ar_p + tremor
            dy += self._ar_y
        px, py = angle_to_pixels(dp, dy, self.sensitivity, self.fov_scale)
        if self.speed_scale != 1.0:
            px = int(round(px * self.speed_scale))
            py = int(round(py * self.speed_scale))
        return (px, py)

    @property
    def has_target(self) -> bool:
        return self._locked_index >= 0
