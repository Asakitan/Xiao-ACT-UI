# -*- coding: utf-8 -*-
"""CS2 plugin — triggerbot with hitchance (seed) mode.

Modes:
  1. **Classic**: fire when m_iIDEntIndex shows enemy under crosshair.
  2. **Hitchance (seed)**: compute probability that a random bullet in
     the weapon spread cone hits the target's head hitbox.  Only fire
     when hitchance ≥ threshold.  This is the practical "seed triggerbot"
     — CS2 server-side validates spread seeds, so true NoSpread is dead;
     hitchance estimation is the standard replacement.

     hitchance ≈ overlap(spread_cone, hitbox_circle) / spread_area
     Simplified: if crosshair_angle < spread − hitbox_angle → 100%.
                 if crosshair_angle > spread + hitbox_angle →   0%.
                 else → smooth interpolation between the two.
"""

from __future__ import annotations

import math
import random
import time
from typing import Optional, Sequence

from .cs2_math import Vec3, angle_fov, angle_to, distance_3d

HEAD_HITBOX_RADIUS = 3.6   # CS2 head capsule approximate radius (game units)
DEFAULT_SPREAD_DEG = 0.8   # standing AK spray mid-burst (degrees)
MOVE_ACCURACY_THRESH = 88.0  # 34% max speed — above = very inaccurate


def _hitbox_angle(dist: float, radius: float = HEAD_HITBOX_RADIUS) -> float:
    """Angular radius (degrees) of a hitbox at *dist* game units."""
    if dist <= 0:
        return 90.0
    return math.degrees(math.atan2(radius, dist))


def _hitchance(crosshair_deg: float, spread_deg: float,
               hitbox_deg: float) -> float:
    """Estimate hit probability given angular distances.

    Pure geometry: two circles (spread cone section and hitbox section)
    on a sphere approximated as planar at small angles.

    Returns 0.0–1.0.
    """
    if spread_deg <= 0:
        return 1.0 if crosshair_deg <= hitbox_deg else 0.0
    if crosshair_deg <= max(0, spread_deg - hitbox_deg):
        return 1.0
    if crosshair_deg >= spread_deg + hitbox_deg:
        return 0.0
    # Smooth falloff: approximate overlap fraction via cosine blend
    edge_dist = crosshair_deg - (spread_deg - hitbox_deg)
    full_range = 2.0 * hitbox_deg
    if full_range <= 0:
        return 0.0
    t = min(1.0, edge_dist / full_range)
    return 0.5 * (1.0 + math.cos(math.pi * t))


class TriggerbotState:
    """Triggerbot with classic + hitchance (seed) modes."""

    def __init__(self) -> None:
        self.enabled: bool = False

        # --- mode ---
        self.seed_mode: bool = False

        # --- timing ---
        self.delay_min_ms: int = 50
        self.delay_max_ms: int = 150
        self.cooldown_ms: int = 60         # anti-double-tap

        # --- hitchance params ---
        self.hitchance_min: float = 0.65   # minimum hitchance to fire
        self.spread_deg: float = DEFAULT_SPREAD_DEG
        self.hitbox_radius: float = HEAD_HITBOX_RADIUS

        # --- accuracy control ---
        self.hit_rate: float = 1.0
        self.visible_only: bool = True
        self.movement_gate: bool = True    # don't fire when moving > threshold

        # --- burst control ---
        self.burst_max: int = 0
        self.burst_cooldown_ms: int = 500

        # --- internal ---
        self._pending_fire: bool = False
        self._fire_at: float = 0.0
        self._last_target: int = -1
        self._last_fire_time: float = 0.0
        self._burst_count: int = 0
        self._burst_cd_until: float = 0.0

    def reset(self) -> None:
        self._pending_fire = False
        self._fire_at = 0.0
        self._last_target = -1

    def tick(self, local: Optional[dict],
             enemies: dict,
             all_enemies: Optional[Sequence[dict]] = None) -> Optional[str]:
        if not self.enabled or not local:
            self.reset()
            return None

        # Movement gate: don't fire when inaccurate
        if self.movement_gate:
            vel = local.get("velocity", (0, 0, 0))
            if vel:
                import math
                speed_xy = math.hypot(vel[0], vel[1])
                if speed_xy > MOVE_ACCURACY_THRESH:
                    return None

        # Use runtime accuracy penalty if available (dynamic spread)
        runtime_spread = local.get("accuracy_penalty")
        if runtime_spread is not None and runtime_spread > 0:
            self.spread_deg = runtime_spread * 57.2958  # radians → degrees

        now = time.perf_counter()

        # Anti-double-tap cooldown
        if now - self._last_fire_time < self.cooldown_ms / 1000.0:
            return None
        # Burst cooldown
        if self.burst_max > 0 and now < self._burst_cd_until:
            return None

        should_fire = False
        target_id = -1

        if self.seed_mode:
            should_fire, target_id = self._hitchance_check(
                local, all_enemies or list(enemies.values()))
        else:
            should_fire, target_id = self._classic_check(local, enemies)

        if not should_fire:
            if target_id <= 0:
                self.reset()
            return None

        # Hit-rate limiter
        if self.hit_rate < 1.0 and random.random() > self.hit_rate:
            return None

        # New target → start delay
        if target_id != self._last_target:
            self._last_target = target_id
            delay = random.randint(self.delay_min_ms, self.delay_max_ms)
            self._fire_at = now + delay / 1000.0
            self._pending_fire = True
            return None

        if self._pending_fire and now >= self._fire_at:
            # Re-verify before firing
            if self.seed_mode:
                ok, _ = self._hitchance_check(
                    local, all_enemies or list(enemies.values()))
            else:
                ok, _ = self._classic_check(local, enemies)
            if not ok:
                self.reset()
                return None

            self._pending_fire = False
            self._last_fire_time = now
            self._burst_count += 1
            if self.burst_max > 0 and self._burst_count >= self.burst_max:
                self._burst_count = 0
                self._burst_cd_until = now + self.burst_cooldown_ms / 1000.0
            return "fire"

        return None

    def _classic_check(self, local: dict, enemies: dict) -> tuple:
        cross_id = local.get("crosshair_entity", -1)
        if not cross_id or cross_id <= 0:
            return (False, -1)
        enemy = enemies.get(cross_id)
        if not enemy or int(enemy.get("health", 0)) <= 0:
            return (False, cross_id)
        if self.visible_only and not enemy.get("is_visible", True):
            return (False, cross_id)
        return (True, cross_id)

    def _hitchance_check(self, local: dict,
                         enemies: Sequence[dict]) -> tuple:
        """Hitchance mode: fire when hit probability ≥ threshold.

        head_pos on each enemy is already resolved by timeshift
        (backtrack/extrap/interp) before this is called.
        """
        eye = local.get("eye_pos")
        angles = local.get("view_angles")
        if not eye or not angles:
            return (False, -1)

        best_id = -1
        best_hc = 0.0

        for e in enemies:
            if int(e.get("health", 0)) <= 0:
                continue
            if self.visible_only and not e.get("is_visible", True):
                continue
            head = e.get("head_pos")
            if not head:
                continue

            dist = distance_3d(eye, head)
            aim_angle = angle_to(eye, head)
            crosshair_deg = angle_fov(angles, aim_angle)
            hb_deg = _hitbox_angle(dist, self.hitbox_radius)
            hc = _hitchance(crosshair_deg, self.spread_deg, hb_deg)

            if hc > best_hc:
                best_hc = hc
                best_id = idx

        if best_id < 0 or best_hc < self.hitchance_min:
            return (False, best_id)

        return (True, best_id)

    @property
    def stats(self) -> dict:
        return {
            "pending": self._pending_fire,
            "target": self._last_target,
            "burst": self._burst_count,
            "mode": "hitchance" if self.seed_mode else "classic",
        }
