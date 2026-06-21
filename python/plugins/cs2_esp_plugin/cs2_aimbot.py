# -*- coding: utf-8 -*-
"""CS2 plugin — humanised aimbot.

Movement curve: cubic ease-out with optional overshoot.
  ease_out(t) = 1 − (1−t)³         — fast start, decelerating finish
  overshoot(t) = 1 + k·sin(π·t)·(1−t) — overshoots target then returns

Speed is distance-dependent: large deltas produce faster initial
movement; near-target the curve naturally decelerates.

Micro-correction: once within 0.5° of target, switch to tiny
proportional nudges (no curve) simulating deliberate fine-aim.
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

_MICRO_THRESHOLD = 0.5   # degrees — switch to micro-correction below this


def _ease_out_cubic(t: float) -> float:
    t = max(0.0, min(1.0, t))
    return 1.0 - (1.0 - t) ** 3


def _ease_out_overshoot(t: float, overshoot: float = 0.15) -> float:
    """Cubic ease-out with overshoot: goes past 1.0 then returns."""
    t = max(0.0, min(1.0, t))
    base = 1.0 - (1.0 - t) ** 3
    # Overshoot envelope: peaks mid-way, dies at t=1
    kick = overshoot * math.sin(math.pi * t) * (1.0 - t)
    return base + kick


def _jitter(sigma: float) -> Tuple[float, float]:
    if sigma <= 0:
        return (0.0, 0.0)
    return (random.gauss(0, sigma), random.gauss(0, sigma))


class AimbotState:

    def __init__(self) -> None:
        self.enabled: bool = False

        # targeting
        self.fov: float = 5.0
        self.bone: str = "head"
        self.max_distance: float = 0
        self.visible_only: bool = True

        # speed & smoothing
        self.smooth: float = 0.25          # base speed (higher = faster)
        self.speed_scale: float = 1.0      # final pixel multiplier
        self.overshoot: float = 0.0        # 0=off, 0.1–0.3 = subtle overshoot
        self.curve: str = "ease_out"       # ease_out | linear

        # humanisation
        self.jitter: float = 0.06          # hand-shake sigma (degrees)
        self.random_offset: float = 0.0    # intentional miss (degrees)
        self.reaction_ms: int = 0          # delay before aim starts

        # hit-rate
        self.hit_rate: float = 1.0
        self.miss_jitter: float = 0.4

        # sensitivity
        self.sensitivity: float = 1.0
        self.fov_scale: float = 1.0

        # internals
        self._locked_index: int = -1
        self._lock_time: float = 0.0
        self._progress: float = 0.0       # 0→1 per lock cycle
        self._initial_delta: float = 0.0   # angle at lock time
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

        sel_candidates = candidates

        # sticky target
        locked = None
        if self._locked_index >= 0:
            for e in sel_candidates:
                if e.get("index") == self._locked_index:
                    head = e.get("head_pos")
                    if head and angle_fov(angles, angle_to(eye, head)) < self.fov * 2.0:
                        locked = e
                    break

        target = locked or best_target(eye, angles, sel_candidates,
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
        desired = (desired[0] + self._off_p, desired[1] + self._off_y)

        diff = angle_fov(angles, desired)
        if diff < 0.015:
            return None

        # --- micro-correction zone ---
        if diff < _MICRO_THRESHOLD:
            dp, dy = angle_diff(angles, desired)
            micro = 0.15 + 0.1 * random.random()
            dp *= micro
            dy *= micro
            if self.jitter > 0:
                j = _jitter(self.jitter * 0.3)
                dp += j[0]; dy += j[1]
            return angle_to_pixels(dp, dy, self.sensitivity, self.fov_scale)

        # --- main movement curve ---
        # Advance progress: scale by smooth factor; larger initial delta = faster ramp
        step = self.smooth * 0.4 * (1.0 + min(self._initial_delta / self.fov, 2.0))
        self._progress = min(1.0, self._progress + step)

        if self.overshoot > 0:
            t = _ease_out_overshoot(self._progress, self.overshoot)
        else:
            t = _ease_out_cubic(self._progress) if self.curve != "linear" else self._progress

        smoothed = smooth_angle(angles, desired, t)
        dp, dy = angle_diff(angles, smoothed)

        # jitter
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
