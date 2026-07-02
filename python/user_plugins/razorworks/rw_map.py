# -*- coding: utf-8 -*-
"""RW module — radar overlay on the game's built-in minimap.

Automatically reads radar parameters from memory:
  - Map name → select correct pos_x/pos_y/scale
  - cl_hud_map_rs → compute HUD size
  - cl_map_rs → compute zoom level
No manual tuning needed.

Map metadata from resource/overviews/*.txt (built into game files).
ConVar reading via CCvar interface (+0x40 = convar array, +0x58 = size,
CConVar +0x00 = name, +0x40 = value).
"""

from __future__ import annotations

import math
import struct
from typing import Dict, Optional, Sequence, Tuple

Vec3 = Tuple[float, float, float]

# Map metadata: (pos_x, pos_y, scale) from resource/overviews/*.txt
MAP_META: Dict[str, Tuple[float, float, float]] = {
    "de_mirage":   (-3230.0, 1713.0, 5.00),
    "de_dust2":    (-2476.0, 3239.0, 4.40),
    "de_inferno":  (-2087.0, 3870.0, 4.90),
    "de_nuke":     (-3453.0, 2887.0, 7.00),
    "de_anubis":   (-2796.0, 3328.0, 5.22),
    "de_ancient":  (-2953.0, 2164.0, 5.00),
    "de_vertigo":  (-3168.0, 1762.0, 4.00),
    "de_overpass": (-4831.0, 1781.0, 5.20),
    "de_train":    (-2477.0, 2392.0, 4.70),
}

RADAR_IMG_PX = 1024  # standard radar image = 1024×1024

# Fallback defaults if convar read fails
_FALLBACK_HUD_SCALE = 1.0
_FALLBACK_RADAR_SCALE = 0.4
_FALLBACK_HUD_PX = 256

# Cache for convar values (refreshed periodically)
_convar_cache: Dict[str, float] = {}
_convar_cache_tick: int = 0


def read_convar_float(read_fn, module_base: int, cvar_name: str,
                      ccvar_offset: int = 0) -> Optional[float]:
    """Read a float ConVar value from CCvar interface.

    CCvar layout: +0x40 = ConVar* array ptr, +0x58 = array size (uint16)
    CConVar layout: +0x00 = name ptr, +0x28 = type, +0x40 = values[]
    """
    # This is expensive (iterates all convars), so should be called rarely
    # and cached. If ccvar_offset not provided, skip.
    if not ccvar_offset or not module_base or not read_fn:
        return None

    try:
        arr_ptr_raw = read_fn(module_base + ccvar_offset + 0x40, 8)
        if not arr_ptr_raw or len(arr_ptr_raw) < 8:
            return None
        arr_ptr = struct.unpack_from("<Q", arr_ptr_raw)[0]
        if not arr_ptr:
            return None

        size_raw = read_fn(module_base + ccvar_offset + 0x58, 2)
        if not size_raw or len(size_raw) < 2:
            return None
        arr_size = struct.unpack_from("<H", size_raw)[0]
        if arr_size == 0 or arr_size > 8000:
            return None

        target = cvar_name.encode("utf-8")

        # Read pointer array in batches for speed
        batch_size = min(arr_size, 256)
        for batch_start in range(0, arr_size, batch_size):
            count = min(batch_size, arr_size - batch_start)
            ptrs_raw = read_fn(arr_ptr + batch_start * 8, count * 8)
            if not ptrs_raw:
                continue
            for i in range(count):
                cv_ptr = struct.unpack_from("<Q", ptrs_raw, i * 8)[0]
                if not cv_ptr:
                    continue
                name_ptr_raw = read_fn(cv_ptr, 8)
                if not name_ptr_raw:
                    continue
                name_ptr = struct.unpack_from("<Q", name_ptr_raw)[0]
                if not name_ptr:
                    continue
                name_raw = read_fn(name_ptr, 64)
                if not name_raw:
                    continue
                name = name_raw.split(b"\x00", 1)[0]
                if name == target:
                    val_raw = read_fn(cv_ptr + 0x40, 4)
                    if val_raw and len(val_raw) >= 4:
                        return struct.unpack_from("<f", val_raw)[0]
                    return None
    except Exception:
        pass
    return None


def refresh_radar_convars(read_fn, module_base: int,
                          ccvar_offset: int = 0) -> Dict[str, float]:
    """Read cl_hud_map_rs and cl_map_rs from memory. Call rarely."""
    global _convar_cache, _convar_cache_tick
    _convar_cache_tick += 1
    if _convar_cache and _convar_cache_tick % 500 != 0:
        return _convar_cache  # return cached, refresh every ~4 sec at 125Hz

    for cvar_name, default in [("cl_hud_map_rs", _FALLBACK_HUD_SCALE),
                                ("cl_map_rs", _FALLBACK_RADAR_SCALE)]:
        val = read_convar_float(read_fn, module_base, cvar_name, ccvar_offset)
        _convar_cache[cvar_name] = val if val is not None else default

    return _convar_cache


def build_radar_overlay(
    map_name: str,
    local_origin: Vec3,
    local_yaw: float,
    enemies: Sequence[dict],
    hud_scale: Optional[float] = None,
    map_rs: Optional[float] = None,
    screen_w: int = 1920,
    screen_h: int = 1080,
) -> list:
    """Generate overlay ops to draw enemy dots on the game's radar."""
    meta = MAP_META.get(map_name.lower())
    if not meta:
        return []

    hs = hud_scale if hud_scale is not None else _convar_cache.get("cl_hud_map_rs", _FALLBACK_HUD_SCALE)
    rs = map_rs if map_rs is not None else _convar_cache.get("cl_map_rs", _FALLBACK_RADAR_SCALE)

    # Game radar HUD is always top-left, size scales with resolution + hud_scale
    # Base size at 1080p = ~252px, scales proportionally
    base_px = int(252 * (screen_h / 1080.0))
    actual_size = int(base_px * hs)
    radar_x = 8
    radar_y = 8
    cx = radar_x + actual_size // 2
    cy = radar_y + actual_size // 2

    visible_img_px = RADAR_IMG_PX * max(0.1, rs)
    img_to_screen = actual_size / visible_img_px

    pos_x, pos_y, scale = meta
    lx_img = (local_origin[0] - pos_x) / scale
    ly_img = (pos_y - local_origin[1]) / scale

    yaw_rad = math.radians(-local_yaw + 90)
    cos_y, sin_y = math.cos(yaw_rad), math.sin(yaw_rad)

    ops = []
    half = actual_size // 2

    for e in enemies:
        origin = e.get("origin")
        if not origin or int(e.get("health", 0)) <= 0:
            continue

        ex_img = (origin[0] - pos_x) / scale
        ey_img = (pos_y - origin[1]) / scale
        dx = ex_img - lx_img
        dy = ey_img - ly_img
        rx = dx * cos_y - dy * sin_y
        ry = dx * sin_y + dy * cos_y
        sx = rx * img_to_screen
        sy = ry * img_to_screen

        dist = math.sqrt(sx * sx + sy * sy)
        if dist > half - 3:
            if dist > 0:
                sx = sx / dist * (half - 3)
                sy = sy / dist * (half - 3)
            else:
                continue

        px = int(cx + sx)
        py = int(cy + sy)

        vis = e.get("is_visible", False)
        color = "#FF2233" if vis else "#CC6622"

        ops.append({"op": "oval", "x": px - 3, "y": py - 3,
                     "w": 6, "h": 6,
                     "fill": color, "outline": "#000000AA", "width": 1})

    return ops
