# -*- coding: utf-8 -*-
"""RW module — visibility: spotted state + edge-scan ray approximation.

Edge scanning: for each bone hitbox capsule, generate sample points
at center + 8 compass directions (N/NE/E/SE/S/SW/W/NW) at 80% of
capsule radius. If ANY sample passes the visibility check, that bone
is visible and that specific edge point is returned as the best aim
target — so even a sliver of exposed hitbox is aimable.

Visibility layers (cheapest first):
  1. m_bSpottedByMask — free, from snapshot
  2. m_iIDEntIndex — single-point ray from game (at crosshair center)
  3. Multipoint geometric — edge samples in view frustum
  Combined: any layer passing = visible
"""

from __future__ import annotations

import math
from typing import Dict, List, Optional, Sequence, Tuple

Vec3 = Tuple[float, float, float]

HITBOX_RADII = {
    "head": 3.6, "neck": 3.0, "chest": 5.5, "stomach": 5.0,
    "pelvis": 4.5, "upper_arm": 3.0, "forearm": 2.5, "hand": 2.0,
    "thigh": 3.5, "calf": 3.0, "foot": 2.5,
}

BONE_BODYPART = {
    6: "head", 5: "neck", 4: "chest", 3: "stomach",
    2: "stomach", 1: "pelvis", 0: "pelvis",
    7: "upper_arm", 8: "upper_arm", 9: "forearm", 10: "hand",
    11: "upper_arm", 12: "upper_arm", 13: "forearm", 14: "hand",
    22: "thigh", 23: "calf", 24: "foot",
    25: "thigh", 26: "calf", 27: "foot",
}

# 9-point edge scan: center + 8 compass directions at 80% radius
_EDGE_9 = [
    (0.0, 0.0, 0.0),       # center
    (0.0, 0.0, 0.8),       # N  (top)
    (0.57, 0.0, 0.57),     # NE
    (0.8, 0.0, 0.0),       # E  (right)
    (0.57, 0.0, -0.57),    # SE
    (0.0, 0.0, -0.8),      # S  (bottom)
    (-0.57, 0.0, -0.57),   # SW
    (-0.8, 0.0, 0.0),      # W  (left)
    (-0.57, 0.0, 0.57),    # NW
]

# 5-point: center + cardinal
_EDGE_5 = [
    (0.0, 0.0, 0.0), (0.0, 0.0, 0.8), (0.0, 0.0, -0.8),
    (0.8, 0.0, 0.0), (-0.8, 0.0, 0.0),
]

# 3-point: center + top/right (fast)
_EDGE_3 = [(0.0, 0.0, 0.0), (0.0, 0.0, 0.8), (0.8, 0.0, 0.0)]


def is_visible_by_mask(spotted_mask: int, local_player_index: int) -> bool:
    """Check if local player is in the entity's spotted mask.

    NOTE: spotted_mask may be a uint64 (8 bytes) on some engines.
    Python int handles arbitrary width natively, but the snapshot reader
    should read 8 bytes (uint64) instead of 4 for the mask field to
    support player indices above 31.
    """
    if spotted_mask == 0:
        return False
    if local_player_index <= 0:
        return False
    return bool(spotted_mask & ((1 << (local_player_index - 1)) | (1 << local_player_index)))


def _view_relative_samples(bone_pos: Vec3, eye: Vec3, body_part: str,
                           edge_count: int = 9) -> List[Vec3]:
    """Generate edge samples in the plane perpendicular to view direction.

    Instead of world-axis-aligned offsets, samples are placed on a disc
    facing the viewer so that edge coverage is consistent regardless of
    the camera angle to the target.
    """
    radius = HITBOX_RADII.get(body_part, 3.0)
    dx = bone_pos[0] - eye[0]
    dy = bone_pos[1] - eye[1]
    dz = bone_pos[2] - eye[2]
    dist = math.sqrt(dx*dx + dy*dy + dz*dz)
    if dist < 0.1:
        return [bone_pos]
    # Forward vector (eye → bone)
    fx, fy, fz = dx/dist, dy/dist, dz/dist
    # Right = forward × world_up (0, 0, 1)
    rx = fy * 1.0 - fz * 0.0
    ry = fz * 0.0 - fx * 1.0
    rz = fx * 0.0 - fy * 0.0
    rlen = math.sqrt(rx*rx + ry*ry + rz*rz)
    if rlen < 0.001:
        # Forward is nearly vertical — pick arbitrary right
        rx, ry, rz = 1.0, 0.0, 0.0
    else:
        rx /= rlen; ry /= rlen; rz /= rlen
    # Up = right × forward
    ux = ry * fz - rz * fy
    uy = rz * fx - rx * fz
    uz = rx * fy - ry * fx

    # 2D template: center + 8 compass at 80% radius in (right, up) plane
    _TEMPLATE_2D = [(0, 0), (0, 0.8), (0.57, 0.57), (0.8, 0),
                    (0.57, -0.57), (0, -0.8), (-0.57, -0.57), (-0.8, 0), (-0.57, 0.57)]
    points: List[Vec3] = []
    for rs, us in _TEMPLATE_2D[:edge_count]:
        ox = (rx * rs + ux * us) * radius
        oy = (ry * rs + uy * us) * radius
        oz = (rz * rs + uz * us) * radius
        points.append((bone_pos[0]+ox, bone_pos[1]+oy, bone_pos[2]+oz))
    return points


def edge_sample_points(bone_pos: Vec3, body_part: str,
                       edge_count: int = 9) -> List[Vec3]:
    """Generate sample positions around a bone center for edge scanning."""
    radius = HITBOX_RADII.get(body_part, 3.0)
    if edge_count >= 9:
        offsets = _EDGE_9
    elif edge_count >= 5:
        offsets = _EDGE_5
    elif edge_count >= 3:
        offsets = _EDGE_3
    else:
        offsets = _EDGE_9[:1]
    return [
        (bone_pos[0] + dx * radius,
         bone_pos[1] + dy * radius,
         bone_pos[2] + dz * radius)
        for dx, dy, dz in offsets
    ]


def find_best_visible_point(
    bone_pos: Vec3,
    body_part: str,
    eye: Vec3,
    view_angles: Tuple[float, float],
    fov: float = 90.0,
    edge_count: int = 9,
) -> Optional[Vec3]:
    """Find the edge sample point closest to crosshair center.

    Returns the best aimable point, or None if no sample is in FOV.
    Even a sliver of exposed hitbox (one edge point in view) is enough.
    """
    from .rw_math import angle_to, angle_fov
    half_fov = fov / 2.0
    # Use view-relative sampling when eye is available for consistent
    # coverage regardless of camera angle; fall back to world-axis-aligned
    if eye:
        samples = _view_relative_samples(bone_pos, eye, body_part, edge_count)
    else:
        samples = edge_sample_points(bone_pos, body_part, edge_count)
    best_point = None
    best_fov_dist = half_fov

    for sp in samples:
        aim = angle_to(eye, sp)
        fd = angle_fov(view_angles, aim)
        if fd < best_fov_dist:
            best_fov_dist = fd
            best_point = sp

    return best_point


def scan_entity_visible_points(
    bones: Dict[int, Vec3],
    eye: Vec3,
    view_angles: Tuple[float, float],
    fov: float = 90.0,
    target_bones: Sequence[int] = (6, 5, 4, 3, 0),
    edge_count: int = 9,
) -> Dict[int, Vec3]:
    """For each bone, find the best visible edge point.

    Returns {bone_index: best_aim_point} for bones that have at least
    one visible edge point. Empty if nothing is visible.
    """
    from .rw_math import angle_to, angle_fov
    half_fov = fov / 2.0
    visible_points: Dict[int, Vec3] = {}

    for bidx in target_bones:
        bpos = bones.get(bidx)
        if not bpos:
            continue
        part = BONE_BODYPART.get(bidx, "chest")
        bp = find_best_visible_point(bpos, part, eye, view_angles, fov, edge_count)
        if bp is not None:
            visible_points[bidx] = bp

    return visible_points


def combined_visibility(
    entity: dict,
    local_index: int,
    eye: Optional[Vec3] = None,
    view_angles: Optional[Tuple[float, float]] = None,
    edge_count: int = 9,
    fov: float = 90.0,
) -> Tuple[bool, Dict[int, Vec3]]:
    """Combined check: spotted OR any bone edge point visible.

    Returns (is_visible, {bone_idx: best_aim_point}).
    Even one visible edge point = entity is visible and aimable at that point.
    """
    mask = entity.get("spotted_mask", 0)
    spotted = is_visible_by_mask(mask, local_index) if mask else False

    visible_points: Dict[int, Vec3] = {}
    bones = entity.get("bones")

    if bones and eye and view_angles:
        visible_points = scan_entity_visible_points(
            bones, eye, view_angles, fov=fov,
            target_bones=(6, 5, 4, 3, 0, 22, 25),
            edge_count=edge_count,
        )

    is_vis = spotted or bool(visible_points)
    return (is_vis, visible_points)
