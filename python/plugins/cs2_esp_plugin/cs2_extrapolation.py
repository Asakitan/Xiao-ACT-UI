# -*- coding: utf-8 -*-
"""CS2 plugin — enemy movement extrapolation (predict future position).

Tracks per-entity position history, estimates velocity + acceleration,
predicts where the target WILL BE after a configurable lookahead time.
Aimbot/triggerbot aim at the predicted position instead of the current.

Prediction model:
  pos(t+dt) = pos + vel·dt + ½·accel·dt²

Confidence scoring:
  - Stable linear motion → high confidence (use full prediction)
  - Erratic/direction change → low confidence (fall back to current pos)
  - Stationary → zero (no prediction needed)

Velocity source priority:
  1. Memory-read m_vecVelocity (if available, most accurate)
  2. Computed from position history (Δpos / Δt, smoothed)
"""

from __future__ import annotations

import math
import time
from collections import deque
from typing import Dict, Optional, Tuple

Vec3 = Tuple[float, float, float]

MAX_HISTORY = 16         # position samples per entity
MIN_SAMPLES = 3          # need at least this many for velocity
MAX_SAMPLE_AGE = 2.0     # seconds — discard older samples
STATIONARY_SPEED = 5.0   # below this = standing still (game units/s)


def _v3_sub(a: Vec3, b: Vec3) -> Vec3:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _v3_add(a: Vec3, b: Vec3) -> Vec3:
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def _v3_scale(v: Vec3, s: float) -> Vec3:
    return (v[0] * s, v[1] * s, v[2] * s)


def _v3_len(v: Vec3) -> float:
    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])


def _v3_dot(a: Vec3, b: Vec3) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


class _Sample:
    __slots__ = ("pos", "t")

    def __init__(self, pos: Vec3, t: float):
        self.pos = pos
        self.t = t


class EntityTracker:
    """Per-entity position history and velocity estimation."""

    def __init__(self) -> None:
        self._history: deque[_Sample] = deque(maxlen=MAX_HISTORY)
        self._vel: Vec3 = (0.0, 0.0, 0.0)
        self._accel: Vec3 = (0.0, 0.0, 0.0)
        self._confidence: float = 0.0
        self._last_dir: Vec3 = (0.0, 0.0, 0.0)

    def update(self, pos: Vec3, mem_velocity: Optional[Vec3] = None,
               now: Optional[float] = None) -> None:
        """Record a new position sample."""
        t = now or time.perf_counter()

        # Prune old samples
        while self._history and (t - self._history[0].t) > MAX_SAMPLE_AGE:
            self._history.popleft()

        self._history.append(_Sample(pos, t))
        self._recompute(mem_velocity)

    def _recompute(self, mem_vel: Optional[Vec3]) -> None:
        n = len(self._history)
        if n < MIN_SAMPLES:
            self._vel = mem_vel or (0.0, 0.0, 0.0)
            self._accel = (0.0, 0.0, 0.0)
            self._confidence = 0.0
            return

        # Velocity: weighted average of recent deltas (newer = heavier)
        vel_sum = [0.0, 0.0, 0.0]
        weight_sum = 0.0
        velocities = []

        for i in range(1, n):
            dt = self._history[i].t - self._history[i - 1].t
            if dt < 0.001:
                continue
            dx = _v3_sub(self._history[i].pos, self._history[i - 1].pos)
            v = _v3_scale(dx, 1.0 / dt)
            w = float(i)  # newer samples get higher weight
            vel_sum[0] += v[0] * w
            vel_sum[1] += v[1] * w
            vel_sum[2] += v[2] * w
            weight_sum += w
            velocities.append(v)

        if weight_sum > 0:
            computed_vel = (vel_sum[0] / weight_sum,
                           vel_sum[1] / weight_sum,
                           vel_sum[2] / weight_sum)
        else:
            computed_vel = (0.0, 0.0, 0.0)

        # Prefer memory velocity if available (more accurate for current frame)
        if mem_vel and _v3_len(mem_vel) > STATIONARY_SPEED:
            # Blend: 70% memory, 30% computed (memory is real-time, computed is smoothed)
            self._vel = (
                mem_vel[0] * 0.7 + computed_vel[0] * 0.3,
                mem_vel[1] * 0.7 + computed_vel[1] * 0.3,
                mem_vel[2] * 0.7 + computed_vel[2] * 0.3,
            )
        else:
            self._vel = computed_vel

        # Acceleration: from last two velocity samples
        if len(velocities) >= 2:
            dt_v = self._history[-1].t - self._history[-2].t
            if dt_v > 0.001:
                dv = _v3_sub(velocities[-1], velocities[-2])
                self._accel = _v3_scale(dv, 1.0 / dt_v)
                # Cap acceleration to prevent wild predictions
                accel_mag = _v3_len(self._accel)
                if accel_mag > 2000.0:
                    self._accel = _v3_scale(self._accel, 2000.0 / accel_mag)
            else:
                self._accel = (0.0, 0.0, 0.0)
        else:
            self._accel = (0.0, 0.0, 0.0)

        # Confidence: how consistent is the movement direction?
        speed = _v3_len(self._vel)
        if speed < STATIONARY_SPEED:
            self._confidence = 0.0
            return

        cur_dir = _v3_scale(self._vel, 1.0 / speed)

        if _v3_len(self._last_dir) > 0.5:
            # Dot product of current vs previous direction: 1.0=same, -1.0=reversed
            dot = _v3_dot(cur_dir, self._last_dir)
            # Confidence: high when direction is consistent
            self._confidence = max(0.0, min(1.0, (dot + 1.0) * 0.5))
            # Decay confidence if acceleration is high relative to velocity
            accel_ratio = _v3_len(self._accel) / max(speed, 1.0)
            if accel_ratio > 0.5:
                self._confidence *= max(0.0, 1.0 - accel_ratio * 0.5)
        else:
            self._confidence = 0.5

        self._last_dir = cur_dir

    def predict(self, dt: float) -> Tuple[Vec3, float]:
        """Predict position at t+dt seconds.

        Returns (predicted_pos, confidence).
        ``confidence`` 0.0–1.0: how much to trust the prediction.
        """
        if not self._history:
            return ((0, 0, 0), 0.0)

        current = self._history[-1].pos
        speed = _v3_len(self._vel)
        if speed < STATIONARY_SPEED or self._confidence < 0.1:
            return (current, 0.0)

        # pos + vel·dt + ½·accel·dt²
        pred = _v3_add(current, _v3_scale(self._vel, dt))
        pred = _v3_add(pred, _v3_scale(self._accel, 0.5 * dt * dt))

        # Sanity cap: prediction shouldn't be more than max_speed * dt away
        max_dist = 450.0 * dt  # ~250 units/s is full sprint in CS2
        delta = _v3_sub(pred, current)
        dist = _v3_len(delta)
        if dist > max_dist:
            pred = _v3_add(current, _v3_scale(delta, max_dist / dist))
            # Reduce confidence for capped predictions
            conf = self._confidence * 0.5
        else:
            conf = self._confidence

        return (pred, conf)

    @property
    def velocity(self) -> Vec3:
        return self._vel

    @property
    def speed(self) -> float:
        return _v3_len(self._vel)

    @property
    def confidence(self) -> float:
        return self._confidence

    @property
    def is_moving(self) -> bool:
        return _v3_len(self._vel) > STATIONARY_SPEED


class ExtrapolationEngine:
    """Manages trackers for all entities and applies prediction."""

    def __init__(self) -> None:
        self.enabled: bool = False
        self.lookahead_ms: float = 20.0    # prediction time (ms)
        self.min_confidence: float = 0.3    # below this, use current pos
        self.use_acceleration: bool = True
        self._trackers: Dict[int, EntityTracker] = {}
        self._last_clean: float = 0.0

    def reset(self) -> None:
        self._trackers.clear()

    def update_entity(self, index: int, pos: Vec3,
                      mem_velocity: Optional[Vec3] = None,
                      now: Optional[float] = None) -> None:
        """Feed a new position sample for entity *index*."""
        if index not in self._trackers:
            self._trackers[index] = EntityTracker()
        self._trackers[index].update(pos, mem_velocity, now)

    def predict_entity(self, index: int,
                       current_pos: Vec3) -> Tuple[Vec3, float]:
        """Get predicted position for entity *index*.

        Returns (predicted_pos, confidence).
        Falls back to current_pos with confidence=0 if no data.
        """
        if not self.enabled:
            return (current_pos, 0.0)

        tracker = self._trackers.get(index)
        if tracker is None:
            return (current_pos, 0.0)

        dt = self.lookahead_ms / 1000.0
        pred_pos, conf = tracker.predict(dt)

        if conf < self.min_confidence:
            return (current_pos, conf)

        return (pred_pos, conf)

    def predict_head(self, index: int,
                     current_head: Vec3,
                     current_origin: Vec3) -> Tuple[Vec3, float]:
        """Predict head position by applying origin delta to head.

        Head offset from origin stays constant; we predict origin movement
        and translate head by the same delta.
        """
        if not self.enabled:
            return (current_head, 0.0)

        tracker = self._trackers.get(index)
        if tracker is None:
            return (current_head, 0.0)

        dt = self.lookahead_ms / 1000.0
        pred_origin, conf = tracker.predict(dt)

        if conf < self.min_confidence:
            return (current_head, conf)

        # head = predicted_origin + (head - current_origin)
        head_offset = _v3_sub(current_head, current_origin)
        pred_head = _v3_add(pred_origin, head_offset)
        return (pred_head, conf)

    def get_tracker(self, index: int) -> Optional[EntityTracker]:
        return self._trackers.get(index)

    def cleanup(self, alive_indices: set, now: Optional[float] = None) -> None:
        """Remove trackers for entities no longer alive."""
        t = now or time.perf_counter()
        if t - self._last_clean < 1.0:
            return
        self._last_clean = t
        dead = [k for k in self._trackers if k not in alive_indices]
        for k in dead:
            del self._trackers[k]
