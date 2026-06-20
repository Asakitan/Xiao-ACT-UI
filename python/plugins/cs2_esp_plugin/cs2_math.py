# -*- coding: utf-8 -*-
"""CS2 plugin — vector/angle math for aimbot, RCS, projection.

Pure functions, no side effects, no imports beyond stdlib math.
All angles are in degrees; CS2 convention: pitch down = positive,
yaw left = positive (standard Source 2 / Quake convention).
"""

from __future__ import annotations

import math
from typing import Optional, Sequence, Tuple

Vec3 = Tuple[float, float, float]


def angle_to(src: Vec3, dst: Vec3) -> Tuple[float, float]:
    """Calculate (pitch, yaw) from *src* eye position to *dst* target.

    Returns angles in degrees (CS2 convention: pitch positive = down).
    """
    dx = dst[0] - src[0]
    dy = dst[1] - src[1]
    dz = dst[2] - src[2]
    dist_xy = math.hypot(dx, dy)
    if dist_xy < 1e-6:
        return (0.0, 0.0)
    yaw = math.degrees(math.atan2(dy, dx))
    pitch = -math.degrees(math.atan2(dz, dist_xy))
    return (pitch, yaw)


def angle_diff(current: Tuple[float, float],
               target: Tuple[float, float]) -> Tuple[float, float]:
    """Shortest angular delta from *current* to *target* (pitch, yaw).

    Yaw wraps at ±180°; pitch is clamped to ±89°.
    """
    dp = target[0] - current[0]
    dy = target[1] - current[1]
    # Normalize yaw to [-180, 180]
    dy = (dy + 180.0) % 360.0 - 180.0
    # Clamp pitch delta so result stays within ±89
    dp = max(-178.0, min(178.0, dp))
    return (dp, dy)


def angle_fov(current: Tuple[float, float],
              target: Tuple[float, float]) -> float:
    """Angular distance (degrees) between two view angles."""
    dp, dy = angle_diff(current, target)
    return math.hypot(dp, dy)


def smooth_angle(current: Tuple[float, float],
                 target: Tuple[float, float],
                 factor: float) -> Tuple[float, float]:
    """Linearly interpolate from *current* toward *target*.

    ``factor`` in (0, 1]: 1.0 = instant snap, 0.1 = very smooth.
    """
    dp, dy = angle_diff(current, target)
    factor = max(0.01, min(1.0, factor))
    return (current[0] + dp * factor,
            current[1] + dy * factor)


def angle_to_pixels(pitch_delta: float, yaw_delta: float,
                    sensitivity: float,
                    fov_scale: float = 1.0) -> Tuple[int, int]:
    """Convert an angle delta (degrees) to mouse pixel movement.

    CS2 formula: 1 pixel ≈ sensitivity * 0.022 degrees (at default FOV).
    ``fov_scale`` is ``m_flFOVSensitivityAdjust`` (1.0 when unscoped).
    """
    if sensitivity <= 0:
        sensitivity = 1.0
    deg_per_pixel = sensitivity * 0.022 * fov_scale
    if deg_per_pixel < 1e-9:
        return (0, 0)
    dx = int(round(yaw_delta / deg_per_pixel))
    dy = int(round(pitch_delta / deg_per_pixel))
    return (dx, dy)


def distance_3d(a: Vec3, b: Vec3) -> float:
    return math.sqrt((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2)


def normalize_yaw(yaw: float) -> float:
    return (yaw + 180.0) % 360.0 - 180.0


def clamp_pitch(pitch: float) -> float:
    return max(-89.0, min(89.0, pitch))


def vec3_add(a: Vec3, b: Vec3) -> Vec3:
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def vec3_sub(a: Vec3, b: Vec3) -> Vec3:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def vec3_len(v: Vec3) -> float:
    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])


def best_target(eye: Vec3, view_angles: Tuple[float, float],
                targets: Sequence[dict], fov: float,
                max_distance: float = 0) -> Optional[dict]:
    """Pick the target closest to crosshair within *fov* degrees.

    Each target dict needs ``head_pos`` (Vec3).
    Returns the best target dict or None.
    """
    best = None
    best_fov = fov
    for t in targets:
        head = t.get("head_pos")
        if not head:
            continue
        if max_distance > 0 and distance_3d(eye, head) > max_distance:
            continue
        aim = angle_to(eye, head)
        fov_dist = angle_fov(view_angles, aim)
        if fov_dist < best_fov:
            best_fov = fov_dist
            best = t
    return best
