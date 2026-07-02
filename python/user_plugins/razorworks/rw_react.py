# -*- coding: utf-8 -*-
"""RW module — reactor with hitchance + multi-bone priority.

Multi-bone priority: check primary bones first (e.g., head), if no
hitchance passes, try secondary bones (e.g., chest). Different hitbox
radii per body part.

Left-click conflict prevention: if player is already holding left
mouse button (manual fire), reactor does NOT inject additional
clicks. This prevents double-fire and tracker-reactor fighting.
"""

from __future__ import annotations

import ctypes
import math
import random
import time
from typing import Dict, List, Optional, Sequence

from .rw_mesh import TRACKER_BONES
from .rw_math import Vec3, angle_fov, angle_to, distance_3d
from .rw_sight import BONE_BODYPART, HITBOX_RADII

DEFAULT_SPREAD_DEG = 0.8
MOVE_ACCURACY_THRESH = 88.0
VK_LBUTTON = 0x01

_user32 = None


def _is_lbutton_held() -> bool:
    """Check if left mouse button is physically held down."""
    global _user32
    if _user32 is None:
        try:
            _user32 = ctypes.WinDLL("user32", use_last_error=True)
        except Exception:
            return False
    try:
        return bool(_user32.GetAsyncKeyState(VK_LBUTTON) & 0x8000)
    except Exception:
        return False


def _hitbox_angle(dist: float, radius: float) -> float:
    if dist <= 0:
        return 90.0
    return math.degrees(math.atan2(radius, dist))


def _hitchance(crosshair_deg: float, spread_deg: float,
               hitbox_deg: float) -> float:
    if spread_deg <= 0:
        return 1.0 if crosshair_deg <= hitbox_deg else 0.0
    if crosshair_deg <= max(0, spread_deg - hitbox_deg):
        return 1.0
    if crosshair_deg >= spread_deg + hitbox_deg:
        return 0.0
    edge_dist = crosshair_deg - (spread_deg - hitbox_deg)
    full_range = 2.0 * hitbox_deg
    if full_range <= 0:
        return 0.0
    t = min(1.0, edge_dist / full_range)
    return 0.5 * (1.0 + math.cos(math.pi * t))


def _bone_radius(bone_name: str) -> float:
    """Get hitbox radius for a bone name."""
    mapping = {
        "head": "head", "neck": "neck", "chest": "chest",
        "stomach": "stomach", "pelvis": "pelvis",
        "l_upperarm": "upper_arm", "r_upperarm": "upper_arm",
        "l_thigh": "thigh", "r_thigh": "thigh",
    }
    part = mapping.get(bone_name, "chest")
    return HITBOX_RADII.get(part, 3.6)


class ReactState:

    def __init__(self) -> None:
        self.enabled: bool = False

        # mode
        self.seed_mode: bool = False

        # timing
        self.delay_min_ms: int = 50
        self.delay_max_ms: int = 150
        self.cooldown_ms: int = 60

        # hitchance
        self.hitchance_min: float = 0.65
        self.spread_deg: float = DEFAULT_SPREAD_DEG

        # multi-bone priority
        self.primary_bones: List[str] = ["head"]
        self.secondary_bones: List[str] = ["neck", "chest"]

        # accuracy
        self.hit_rate: float = 1.0
        self.visible_only: bool = True
        self.movement_gate: bool = True

        # burst
        self.burst_max: int = 0
        self.burst_cooldown_ms: int = 500

        # ex-gaussian reaction time parameters
        self.rt_mu: float = 160.0
        self.rt_sigma: float = 25.0
        self.rt_tau: float = 50.0

        # kill cooldown + dwell
        self._kill_cooldown_until: float = 0.0
        self._dwell_start: float = 0.0
        self._dwell_target: int = -1
        self.dwell_min_ms: int = 40

        # session fatigue
        self._session_start: float = 0.0

        # internals
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

        # Left-click conflict: if player is manually firing, don't inject
        if _is_lbutton_held():
            return None

        if self.movement_gate:
            vel = local.get("velocity", (0, 0, 0))
            if vel and math.hypot(vel[0], vel[1]) > MOVE_ACCURACY_THRESH:
                return None

        runtime_spread = local.get("accuracy_penalty")
        if runtime_spread is not None and runtime_spread > 0:
            self.spread_deg = runtime_spread * 57.2958

        now = time.perf_counter()

        # Session fatigue
        if self._session_start == 0:
            self._session_start = now
        elapsed_min = (now - self._session_start) / 60.0
        fatigue = 1.0 + 0.003 * min(elapsed_min, 45.0)

        # Kill cooldown
        if now < self._kill_cooldown_until:
            return None

        if now - self._last_fire_time < self.cooldown_ms / 1000.0:
            return None
        if self.burst_max > 0 and now < self._burst_cd_until:
            return None

        should_fire = False
        target_id = -1

        if self.seed_mode:
            should_fire, target_id = self._hitchance_check(
                local, all_enemies or list(enemies.values()))
        else:
            should_fire, target_id = self._classic_check(local, enemies)

        # Kill cooldown: if previous target died, pause before engaging next
        if self._last_target > 0 and all_enemies:
            for e in (all_enemies if all_enemies else []):
                if e.get("index") == self._last_target and int(e.get("health", 0)) <= 0:
                    cd = random.lognormvariate(math.log(250), 0.4)
                    self._kill_cooldown_until = now + max(0.1, min(0.6, cd / 1000))
                    self._last_target = -1
                    return None

        if not should_fire:
            if target_id <= 0:
                self.reset()
            return None

        # Dwell: require crosshair on target for dwell_min_ms before allowing fire
        if should_fire:
            if target_id == getattr(self, '_dwell_target', -1):
                if self._dwell_start > 0 and (now - self._dwell_start) * 1000 < self.dwell_min_ms:
                    return None
            else:
                self._dwell_target = target_id
                self._dwell_start = now
                return None

        if self.hit_rate < 1.0 and random.random() > self.hit_rate:
            return None

        if target_id != self._last_target:
            self._last_target = target_id
            # Ex-Gaussian reaction time distribution
            g = random.gauss(self.rt_mu, self.rt_sigma)
            e = random.expovariate(1.0 / max(1.0, self.rt_tau))
            delay = max(40.0, min(600.0, g + e))
            delay = delay * fatigue
            self._fire_at = now + delay / 1000.0
            self._pending_fire = True
            return None

        if self._pending_fire and now >= self._fire_at:
            # Re-verify + re-check left button
            if _is_lbutton_held():
                return None
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
        """Check hitchance using multi-bone priority."""
        eye = local.get("eye_pos")
        angles = local.get("view_angles")
        if not eye or not angles:
            return (False, -1)

        # Try primary bones, then secondary
        for bone_list in (self.primary_bones, self.secondary_bones):
            best_id, best_hc = self._check_bone_list(
                eye, angles, enemies, bone_list)
            if best_id >= 0 and best_hc >= self.hitchance_min:
                return (True, best_id)

        return (False, -1)

    def _check_bone_list(self, eye: Vec3, angles, enemies, bone_names):
        """Check hitchance using cached visible_points from edge scan.

        Reuses the edge-scan results computed once per tick in plugin._tick,
        avoiding redundant 9-point × N-bone × M-enemy trig recalculation.
        Falls back to bone center if visible_points not available.
        """
        best_id = -1
        best_hc = 0.0

        for e in enemies:
            if int(e.get("health", 0)) <= 0:
                continue
            if self.visible_only and not e.get("is_visible", True):
                continue

            bones = e.get("bones", {})
            vis_pts = e.get("visible_points", {})

            for bname in bone_names:
                bidx = TRACKER_BONES.get(bname.lower())
                if bidx is None:
                    continue

                # Prefer cached edge point (already the best visible point)
                pos = vis_pts.get(bidx) or bones.get(bidx)
                if not pos:
                    continue

                part = BONE_BODYPART.get(bidx, "chest")
                radius = HITBOX_RADII.get(part, 3.6)
                dist = distance_3d(eye, pos)
                aim = angle_to(eye, pos)
                cross_deg = angle_fov(angles, aim)
                hb_deg = _hitbox_angle(dist, radius)
                hc = _hitchance(cross_deg, self.spread_deg, hb_deg)

                if hc > best_hc:
                    best_hc = hc
                    best_id = e.get("index", -1)
                if best_hc >= self.hitchance_min:
                    return best_id, best_hc  # early exit

        return best_id, best_hc

    @property
    def stats(self) -> dict:
        return {
            "pending": self._pending_fire,
            "target": self._last_target,
            "burst": self._burst_count,
            "mode": "hitchance" if self.seed_mode else "classic",
        }
