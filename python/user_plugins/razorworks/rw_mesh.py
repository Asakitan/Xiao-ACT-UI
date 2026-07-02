# -*- coding: utf-8 -*-
"""RW module — bone array reader and skeleton builder.

Source 2 bone data access chain (external, read-only):
  pawn → m_pGameSceneNode (CSkeletonInstance)
       → m_modelState (CModelState)
       → boneArray pointer (m_modelState + BONE_ARRAY_OFFSET)
       → array of BoneData [N]

Each BoneData entry (32 bytes, tightly packed):
    float[3] position    (12 bytes)  — world-space bone position
    float    scale        (4 bytes)
    float[4] quaternion  (16 bytes)  — rotation (unused for ESP)

Head bone index for Source 2 player models is typically 6.
"""

from __future__ import annotations

import struct
from typing import Dict, List, Optional, Sequence, Tuple

Vec3 = Tuple[float, float, float]

BONE_DATA_SIZE = 32   # bytes per bone
MAX_BONES = 128       # sanity cap

BONE_NAMES = {
    0: "pelvis",
    1: "spine_0",
    2: "spine_1",
    3: "spine_2",
    4: "spine_3",      # upper chest
    5: "neck",
    6: "head",
    7: "l_clavicle",
    8: "l_upperarm",
    9: "l_forearm",
    10: "l_hand",
    11: "r_clavicle",
    12: "r_upperarm",
    13: "r_forearm",
    14: "r_hand",
    22: "l_thigh",
    23: "l_calf",
    24: "l_foot",
    25: "r_thigh",
    26: "r_calf",
    27: "r_foot",
}

SKELETON_LINES = [
    (6, 5),    # head → neck
    (5, 4),    # neck → upper chest
    (4, 3),    # upper chest → spine_2
    (3, 2),    # spine_2 → spine_1
    (2, 1),    # spine_1 → spine_0
    (1, 0),    # spine_0 → pelvis
    (5, 7),    # neck → l_clavicle
    (7, 8),    # l_clavicle → l_upperarm
    (8, 9),    # l_upperarm → l_forearm
    (9, 10),   # l_forearm → l_hand
    (5, 11),   # neck → r_clavicle
    (11, 12),  # r_clavicle → r_upperarm
    (12, 13),  # r_upperarm → r_forearm
    (13, 14),  # r_forearm → r_hand
    (0, 22),   # pelvis → l_thigh
    (22, 23),  # l_thigh → l_calf
    (23, 24),  # l_calf → l_foot
    (0, 25),   # pelvis → r_thigh
    (25, 26),  # r_thigh → r_calf
    (26, 27),  # r_calf → r_foot
]

TRACKER_BONES = {
    "head": 6,
    "neck": 5,
    "chest": 4,
    "stomach": 3,
    "pelvis": 0,
    "l_upperarm": 8,
    "r_upperarm": 12,
    "l_thigh": 22,
    "r_thigh": 25,
}

BODYPART_PRIORITY = ["head", "neck", "chest", "stomach", "pelvis"]


def target_bone_index(bone_name: str) -> int:
    """Resolve a bone name to its index. Case-insensitive."""
    return TRACKER_BONES.get(bone_name.lower(), 6)


def parse_bone_positions(raw: bytes, count: int) -> Dict[int, Vec3]:
    """Parse bone array bytes into {bone_index: (x, y, z)}.

    ``raw`` = count * 32 bytes read from the bone array pointer.
    Only extracts position (first 3 floats per entry).
    """
    count = min(count, MAX_BONES)
    needed = count * BONE_DATA_SIZE
    if not raw or len(raw) < needed:
        count = len(raw) // BONE_DATA_SIZE if raw else 0
    positions: Dict[int, Vec3] = {}
    for i in range(count):
        off = i * BONE_DATA_SIZE
        try:
            x, y, z = struct.unpack_from("<3f", raw, off)
        except struct.error:
            break
        if x == 0.0 and y == 0.0 and z == 0.0:
            continue
        positions[i] = (x, y, z)
    return positions


def read_bones(read_fn, pawn: int, scene_node_off: int,
               model_state_off: int, bone_array_off: int,
               bone_count: int = 28) -> Optional[Dict[int, Vec3]]:
    """Full bone read chain: pawn → scene_node → model_state → bones.

    ``read_fn(addr, size) -> Optional[bytes]`` is the reader callback.
    Returns bone positions dict or None on any failure.
    """
    if not pawn:
        return None
    sn_raw = read_fn(pawn + scene_node_off, 8)
    if not sn_raw or len(sn_raw) < 8:
        return None
    scene_node = struct.unpack_from("<Q", sn_raw)[0]
    if not scene_node:
        return None

    ba_raw = read_fn(scene_node + model_state_off + bone_array_off, 8)
    if not ba_raw or len(ba_raw) < 8:
        return None
    bone_array_ptr = struct.unpack_from("<Q", ba_raw)[0]
    if not bone_array_ptr:
        return None

    bone_count = min(bone_count, MAX_BONES)
    data = read_fn(bone_array_ptr, bone_count * BONE_DATA_SIZE)
    if not data:
        return None

    return parse_bone_positions(data, bone_count)


def get_bone(bones: Optional[Dict[int, Vec3]], index: int) -> Optional[Vec3]:
    """Get a single bone position by index."""
    if not bones:
        return None
    return bones.get(index)


def head_position(bones: Optional[Dict[int, Vec3]]) -> Optional[Vec3]:
    """Get head bone (index 6) world position."""
    return get_bone(bones, 6)


def skeleton_lines_3d(bones: Dict[int, Vec3]) -> List[Tuple[Vec3, Vec3]]:
    """Return list of (start, end) 3D line segments for skeleton rendering."""
    lines = []
    for a, b in SKELETON_LINES:
        pa = bones.get(a)
        pb = bones.get(b)
        if pa and pb:
            lines.append((pa, pb))
    return lines
