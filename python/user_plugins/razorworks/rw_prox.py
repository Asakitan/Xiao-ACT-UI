# -*- coding: utf-8 -*-
"""RW module — Prox-K + Prox-Z.

Only activate when the corresponding weapon is ALREADY held.
Does not auto-switch weapons — player decides when to pull out
the knife or Zeus, the bot handles aiming + attacking.

Prox-K:
  - Active only when holding a knife (item IDs 42, 59, 500-523)
  - Distance ≤ 64u → attack
  - Backstab detection: dot(my_forward, enemy_forward) > 0 → right-click
  - Front stab: left-click

Prox-Z:
  - Active only when holding Zeus (item ID 31)
  - Distance ≤ 183u → left-click (one-shot kill)
  - Only fires once then weapon is gone
"""

from __future__ import annotations

import math
import struct
from typing import Optional, Sequence, Tuple

from .rw_math import Vec3, angle_to, angle_fov, distance_3d

KNIFE_RANGE = 64.0
ZEUS_RANGE = 183.0

# Item definition IDs
KNIFE_IDS = {42, 59} | set(range(500, 524))  # CT/T default + skin knives
ZEUS_ID = 31


def _forward_from_yaw(yaw_deg: float) -> Tuple[float, float]:
    """2D forward vector from yaw angle."""
    r = math.radians(yaw_deg)
    return (math.cos(r), math.sin(r))


def _dot2d(a: Tuple[float, float], b: Tuple[float, float]) -> float:
    return a[0] * b[0] + a[1] * b[1]


class ProxState:
    """Shared logic for Prox-K and Prox-Z."""

    def __init__(self) -> None:
        self.px_k: bool = False
        self.px_z: bool = False

    def tick(self, local: Optional[dict],
             enemies: Sequence[dict],
             weapon_id: int) -> Optional[str]:
        """Return 'left', 'right', or None.

        ``weapon_id`` = m_iItemDefinitionIndex of active weapon.
        """
        if not local:
            return None

        eye = local.get("eye_pos")
        angles = local.get("view_angles")
        if not eye or not angles:
            return None

        # Determine mode from held weapon
        is_knife = self.px_k and weapon_id in KNIFE_IDS
        is_zeus = self.px_z and weapon_id == ZEUS_ID
        if not is_knife and not is_zeus:
            return None

        max_range = KNIFE_RANGE if is_knife else ZEUS_RANGE

        # Find closest visible enemy in range
        best_ent = None
        best_dist = max_range
        for e in enemies:
            if int(e.get("health", 0)) <= 0:
                continue
            if not e.get("is_visible", True):
                continue
            origin = e.get("origin")
            if not origin:
                continue
            d = distance_3d(eye, origin)
            if d < best_dist:
                best_dist = d
                best_ent = e

        if best_ent is None:
            return None

        # Check FOV — enemy should be roughly in front
        target_pos = best_ent.get("head_pos") or best_ent.get("origin")
        if not target_pos:
            return None
        aim = angle_to(eye, target_pos)
        if angle_fov(angles, aim) > 60.0:
            return None

        if is_zeus:
            return "left"

        # Knife: backstab detection
        my_yaw = angles[1]
        my_fwd = _forward_from_yaw(my_yaw)

        enemy_angles = best_ent.get("eye_angles")
        if enemy_angles:
            enemy_fwd = _forward_from_yaw(enemy_angles[1])
            if _dot2d(my_fwd, enemy_fwd) > 0.3:
                return "right"  # backstab — heavy attack

        return "left"  # front stab — light attack


def read_weapon_id(read_fn, pawn: int, weapon_ptr_off: int,
                   item_def_off: int) -> int:
    """Read m_iItemDefinitionIndex from active weapon."""
    if not pawn or not weapon_ptr_off:
        return 0
    wp_raw = read_fn(pawn + weapon_ptr_off, 8)
    if not wp_raw or len(wp_raw) < 8:
        return 0
    weapon_ptr = struct.unpack_from("<Q", wp_raw)[0]
    if not weapon_ptr:
        return 0
    id_raw = read_fn(weapon_ptr + item_def_off, 2)
    if not id_raw or len(id_raw) < 2:
        return 0
    return struct.unpack_from("<H", id_raw)[0]
