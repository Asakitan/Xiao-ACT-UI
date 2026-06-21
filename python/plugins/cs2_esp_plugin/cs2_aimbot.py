# -*- coding: utf-8 -*-
"""CS2 plugin — precision aimbot.

Precision techniques:
  - RCS integrated: desired_angle = angle_to(eye, head) - punch × 2.0
    (bullets go where aimed, not where crosshair visually is)
  - Head bone Z−1 correction (bone center sits above hitbox center)
  - Movement gate: refuse to fire when velocity > 88 u/s (34% max speed)
  - Cubic ease-out curve + overshoot for natural mouse movement
  - Micro-correction zone (<0.5°) with proportional nudges
"""

from __future__ import annotations

import math
import random
import time
from typing import Optional, Sequence, Tuple

from .cs2_math import (
    Vec3, angle_diff, angle_fov, angle_to, angle_to_pixels,
    best_target, distance_3d, smooth_angle,
)

_MICRO_THRESHOLD = 0.5
HEAD_Z_OFFSET = -1.0          # bone 6 center is above capsule hitbox center
MOVE_ACCURACY_THRESH = 88.0   # 34% of rifle max speed — above = inaccurate
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


def _correct_head(head: Vec3) -> Vec3:
    """Apply Z−1 correction to head bone position."""
    return (head[0], head[1], head[2] + HEAD_Z_OFFSET)


class AimbotState:

    def __init__(self) -> None:
        self.enabled: bool = False

        # targeting
        self.fov: float = 5.0
        self.bone: str = "head"
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
        self.rcs_integrated: bool = True   # subtract punch from aim angle
        self.movement_gate: bool = True    # refuse when moving too fast

        # sensitivity
        self.sensitivity: float = 1.0
        self.fov_scale: float = 1.0

        # internals
        self._locked_index: int = -1
        self._lock_time: float = 0.0
        self._progress: float = 0.0
        self._initial_delta: float = 0.0
        self._off_p: float = 0.0
        self._off_y: float = 0.0

    def reset(self) -> None:
        self._locked_index = -1
        self._lock_time = 0.0
        self._progress = 0.0
        self._initial_delta = 0.0
        self._off_p = 0.0
        self._off_y = 0.0

    def tick(self, local: Optional[dict],
             enemies: Sequence[dict]) -> Optional[Tuple[int, int]]:
        """head_pos on each enemy is already resolved by timeshift."""
        if not self.enabled or not local:
            self.reset()
            return None

        eye = local.get("eye_pos")
        angles = local.get("view_angles")
        if not eye or not angles:
            return None

        # Movement gate: don't aim when inaccurate
        if self.movement_gate:
            vel = local.get("velocity", (0, 0, 0))
            if vel:
                speed_xy = math.hypot(vel[0], vel[1])
                if speed_xy > MOVE_ACCURACY_THRESH:
                    return None

        candidates = list(enemies)
        if self.visible_only:
            candidates = [e for e in candidates if e.get("is_visible", True)]
        if not candidates:
            self.reset()
            return None

        now = time.perf_counter()

        # hit-rate limiter
        if self.hit_rate < 1.0 and random.random() > self.hit_rate:
            if self.miss_jitter > 0:
                jp, jy = _jitter(self.miss_jitter)
                return angle_to_pixels(jp, jy, self.sensitivity, self.fov_scale)
            return None

        # Apply head Z correction to all candidates
        for e in candidates:
            head = e.get("head_pos")
            if head:
                e["head_pos"] = _correct_head(head)

        # sticky target
        locked = None
        if self._locked_index >= 0:
            for e in candidates:
                if e.get("index") == self._locked_index:
                    head = e.get("head_pos")
                    if head and angle_fov(angles, angle_to(eye, head)) < self.fov * 2.0:
                        locked = e
                    break

        target = locked or best_target(eye, angles, candidates,
                                       self.fov, self.max_distance)
        if target is None:
            self.reset()
            return None

        # new target
        if target.get("index") != self._locked_index:
            self._locked_index = target.get("index", -1)
            self._lock_time = now
            self._progress = 0.0
            head = target.get("head_pos")
            self._initial_delta = angle_fov(angles, angle_to(eye, head)) if head else self.fov
            if self.random_offset > 0:
                self._off_p = random.gauss(0, self.random_offset)
                self._off_y = random.gauss(0, self.random_offset)
            else:
                self._off_p = self._off_y = 0.0

        # reaction delay
        if self.reaction_ms > 0:
            if (now - self._lock_time) * 1000 < self.reaction_ms:
                return None

        head = target["head_pos"]
        desired = angle_to(eye, head)

        # --- RCS integrated: subtract punch from desired angle ---
        # Bullet direction = viewAngles + aimPunch × 2.0
        # So to hit target: viewAngles = desired - aimPunch × 2.0
        if self.rcs_integrated:
            punch = local.get("aim_punch")
            if punch:
                desired = (desired[0] - punch[0] * RECOIL_SCALE,
                           desired[1] - punch[1] * RECOIL_SCALE)

        # Apply intentional offset
        desired = (desired[0] + self._off_p, desired[1] + self._off_y)

        diff = angle_fov(angles, desired)
        if diff < 0.015:
            return None

        # micro-correction zone
        if diff < _MICRO_THRESHOLD:
            dp, dy = angle_diff(angles, desired)
            micro = 0.15 + 0.1 * random.random()
            dp *= micro
            dy *= micro
            if self.jitter > 0:
                j = _jitter(self.jitter * 0.3)
                dp += j[0]; dy += j[1]
            return angle_to_pixels(dp, dy, self.sensitivity, self.fov_scale)

        # main movement curve
        step = self.smooth * 0.4 * (1.0 + min(self._initial_delta / self.fov, 2.0))
        self._progress = min(1.0, self._progress + step)

        if self.overshoot > 0:
            t = _ease_out_overshoot(self._progress, self.overshoot)
        else:
            t = _ease_out_cubic(self._progress) if self.curve != "linear" else self._progress

        smoothed = smooth_angle(angles, desired, t)
        dp, dy = angle_diff(angles, smoothed)

        if self.jitter > 0:
            j = _jitter(self.jitter)
            dp += j[0]; dy += j[1]

        px, py = angle_to_pixels(dp, dy, self.sensitivity, self.fov_scale)
        if self.speed_scale != 1.0:
            px = int(round(px * self.speed_scale))
            py = int(round(py * self.speed_scale))
        return (px, py)

    @property
    def has_target(self) -> bool:
        return self._locked_index >= 0
