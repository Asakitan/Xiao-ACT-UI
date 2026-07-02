# -*- coding: utf-8 -*-
"""RW module — smooth Drift Control System + auto-stop.

Smooth compensation via:
  1. **EMA filter**: exponential moving average on punch delta removes
     per-tick jitter from the raw punch angle readings.
  2. **Sub-pixel accumulator**: fractional pixel debt carries between
     ticks so fine compensation isn't lost to rounding.
  3. **Per-bullet strength ramp**: gradual ramp-up over the first N
     bullets prevents the sudden snap on first-shot compensation.

Formula:
  raw_delta = (punch − old_punch) × RECOIL_SCALE × strength
  ema_delta = alpha × raw_delta + (1−alpha) × prev_ema
  accum += angle_to_float_pixels(−ema_delta)
  mouse_move(int(accum))
  accum −= int(accum)   # keep fractional remainder
"""

from __future__ import annotations

import math
import random
import time
from typing import Optional, Tuple

from .rw_math import angle_to_pixels

RECOIL_SCALE = 2.0


class CompState:
    """Smooth RCS with sub-pixel accumulation and EMA."""

    def __init__(self) -> None:
        self.enabled: bool = False
        self.strength: float = 1.0         # vertical 0.0–1.0
        self.h_strength: float = 1.0       # horizontal 0.0–1.0
        self.sensitivity: float = 1.0
        self.fov_scale: float = 1.0

        # bullet control
        self.start_bullet: int = 1
        self.max_bullets: int = 0

        # smoothing
        self.ema_alpha: float = 0.5        # EMA weight: 0.1=very smooth, 1.0=raw
        self.ramp_bullets: int = 4         # ramp strength over first N bullets

        # accuracy knobs
        self.hit_rate: float = 1.0
        self.jitter: float = 0.0

        # internals
        self._old_punch: Tuple[float, float] = (0.0, 0.0)
        self._ema_p: float = 0.0
        self._ema_y: float = 0.0
        self._accum_x: float = 0.0
        self._accum_y: float = 0.0
        self._active: bool = False
        self._bullet_count: int = 0
        self._last_comp_time: float = 0.0
        self._spray_variance: float = 1.0

    def reset(self) -> None:
        self._old_punch = (0.0, 0.0)
        self._ema_p = 0.0
        self._ema_y = 0.0
        self._accum_x = 0.0
        self._accum_y = 0.0
        self._active = False
        self._bullet_count = 0
        self._last_comp_time = 0.0
        self._spray_variance = 1.0

    def tick(self, local: Optional[dict]) -> Optional[Tuple[int, int]]:
        if not self.enabled or not local:
            self.reset()
            return None

        punch = local.get("aim_punch")
        shots = local.get("shots_fired", 0)
        if not punch or shots is None:
            self.reset()
            return None

        pp, py = float(punch[0]), float(punch[1])

        # Spray ended — reset
        if shots == 0:
            self.reset()
            self._old_punch = (pp, py)
            return None

        # Not enough bullets yet
        if shots < self.start_bullet:
            self._old_punch = (pp, py)
            self._active = False
            return None

        # Max bullet cap
        if self.max_bullets > 0 and shots > self.start_bullet + self.max_bullets:
            self._old_punch = (pp, py)
            return None

        # First eligible tick
        if not self._active:
            self._old_punch = (pp, py)
            self._active = True
            self._bullet_count = 0
            self._ema_p = 0.0
            self._ema_y = 0.0
            self._accum_x = 0.0
            self._accum_y = 0.0
            self._spray_variance = random.uniform(0.85, 1.0)
            return None

        # Hit-rate: randomly skip
        if self.hit_rate < 1.0 and random.random() > self.hit_rate:
            self._old_punch = (pp, py)
            return None

        # Raw delta
        raw_dp = (pp - self._old_punch[0]) * RECOIL_SCALE * self.strength
        raw_dy = (py - self._old_punch[1]) * RECOIL_SCALE * self.h_strength
        self._old_punch = (pp, py)

        # Frame-rate independent EMA (dt-compensated alpha)
        now = time.perf_counter()
        dt = now - self._last_comp_time if self._last_comp_time > 0 else 1.0 / 128
        self._last_comp_time = now
        dt = max(0.001, min(dt, 0.1))

        alpha = max(0.05, min(1.0, self.ema_alpha))
        ref_dt = 1.0 / 128.0
        tau = -ref_dt / math.log(max(0.01, 1.0 - alpha))
        dt_alpha = 1.0 - math.exp(-dt / tau)
        self._ema_p = dt_alpha * raw_dp + (1.0 - dt_alpha) * self._ema_p
        self._ema_y = dt_alpha * raw_dy + (1.0 - dt_alpha) * self._ema_y

        comp_p = self._ema_p
        comp_y = self._ema_y

        # Per-bullet ramp: gradual strength increase
        self._bullet_count += 1
        if self.ramp_bullets > 0 and self._bullet_count <= self.ramp_bullets:
            ramp = self._bullet_count / self.ramp_bullets
            comp_p *= ramp
            comp_y *= ramp

        # Long spray decay: reduce compensation for sustained fire
        if self._bullet_count > 15:
            decay = max(0.4, 1.0 - 0.03 * (self._bullet_count - 15))
            comp_p *= decay
            comp_y *= decay
        comp_p *= self._spray_variance
        comp_y *= self._spray_variance

        if abs(comp_p) < 0.0001 and abs(comp_y) < 0.0001:
            return None

        # Jitter
        if self.jitter > 0:
            comp_p += random.gauss(0, self.jitter)
            comp_y += random.gauss(0, self.jitter)

        # Convert to float pixels (don't round yet)
        deg_per_px = self.sensitivity * 0.022 * (self.fov_scale or 1.0)
        if deg_per_px < 1e-9:
            return None
        float_x = -comp_y / deg_per_px
        float_y = -comp_p / deg_per_px

        # Sub-pixel accumulator
        self._accum_x += float_x
        self._accum_y += float_y

        ix = int(self._accum_x)
        iy = int(self._accum_y)

        if ix == 0 and iy == 0:
            return None

        # Keep fractional remainder
        self._accum_x -= ix
        self._accum_y -= iy

        return (ix, iy)


# --- Auto-stop ---------------------------------------------------------------

class HaltState:

    def __init__(self) -> None:
        self.enabled: bool = False
        self.speed_threshold: float = 10.0
        self.tap_ms: int = 20

    def should_stop(self, local: Optional[dict]) -> bool:
        if not self.enabled or not local:
            return False
        vel = local.get("velocity")
        if not vel:
            return False
        return (vel[0] ** 2 + vel[1] ** 2) ** 0.5 > self.speed_threshold

    def get_counter_keys(self, local: Optional[dict]) -> list:
        if not local:
            return []
        vel = local.get("velocity")
        if not vel:
            return []
        keys = []
        if abs(vel[0]) > self.speed_threshold:
            keys.append(0x44 if vel[0] > 0 else 0x41)
        if abs(vel[1]) > self.speed_threshold:
            keys.append(0x57 if vel[1] > 0 else 0x53)
        return keys
