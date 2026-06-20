# -*- coding: utf-8 -*-
"""CS2 ESP plugin — world→screen projection and overlay spec builder.

Full ESP rendering: skeleton, bounding box, head dot, HP bar,
distance text, snap lines, visibility coloring, FOV circle.
"""

from __future__ import annotations

import math
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

from .cs2_bones import SKELETON_LINES, head_position

DEFAULT_VIEWPORT = (1920, 1080)

COLOR_ENEMY = "#FF3B3B"
COLOR_ENEMY_VISIBLE = "#FF3B3B"
COLOR_ENEMY_HIDDEN = "#FFAA00"
COLOR_HEALTH = "#FFD23B"
COLOR_BONE = "#00FF88"
COLOR_BONE_HIDDEN = "#888800"
COLOR_HEAD = "#FF4444"
COLOR_SNAPLINE = "#FFFFFF33"
COLOR_DISTANCE = "#AADDFF"
COLOR_FOV = "#FFFFFF22"


def project_point(world: Sequence[float], matrix: Sequence[float],
                  viewport: Sequence[int]) -> Optional[Tuple[float, float]]:
    if not world or len(world) < 3 or not matrix or len(matrix) < 16:
        return None
    wx, wy, wz = float(world[0]), float(world[1]), float(world[2])
    m = [float(v) for v in matrix]
    sx = m[0] * wx + m[1] * wy + m[2] * wz + m[3]
    sy = m[4] * wx + m[5] * wy + m[6] * wz + m[7]
    sw = m[12] * wx + m[13] * wy + m[14] * wz + m[15]
    if sw <= 0.0:
        return None
    inv = 1.0 / sw
    vw, vh = int(viewport[0]), int(viewport[1])
    if vw <= 0 or vh <= 0:
        return None
    screen_x = (sx * inv * 0.5 + 0.5) * vw
    screen_y = (1.0 - (sy * inv * 0.5 + 0.5)) * vh
    return float(screen_x), float(screen_y)


def filter_enemies(entities: Iterable[dict], local_team: int) -> List[dict]:
    try:
        local_team = int(local_team)
    except (TypeError, ValueError):
        local_team = 0
    out = []
    for ent in entities or []:
        if not isinstance(ent, dict):
            continue
        if int(ent.get("health") or 0) <= 0:
            continue
        if local_team > 0 and int(ent.get("team") or 0) == local_team:
            continue
        out.append(ent)
    return out


def _clamp(v: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, v))


def _hp_color(hp: int) -> str:
    if hp > 75:
        return "#44FF44"
    if hp > 30:
        return "#FFAA00"
    return "#FF3333"


def build_overlay_spec(entities: Sequence[dict], matrix: Sequence[float],
                       viewport: Sequence[int], *,
                       local_origin: Optional[Tuple[float, float, float]] = None,
                       show_health: bool = True,
                       show_skeleton: bool = True,
                       show_distance: bool = True,
                       show_snapline: bool = False,
                       show_head: bool = True,
                       show_box: bool = True,
                       show_hp_bar: bool = True,
                       show_fov_circle: bool = False,
                       fov_radius: float = 5.0,
                       max_entities: int = 32) -> dict:
    vw, vh = int(viewport[0]), int(viewport[1])
    if vw <= 0 or vh <= 0:
        vw, vh = DEFAULT_VIEWPORT
    ops: list = []
    count = 0
    cx, cy = vw / 2.0, vh / 2.0

    if show_fov_circle and fov_radius > 0:
        fov_px = int(fov_radius / 90.0 * vh)
        ops.append({
            "op": "oval",
            "x": int(cx - fov_px), "y": int(cy - fov_px),
            "w": fov_px * 2, "h": fov_px * 2,
            "fill": "", "outline": COLOR_FOV, "width": 1,
        })

    for ent in entities[:max(max_entities, 0)]:
        origin = ent.get("origin")
        if not origin or len(origin) < 3:
            continue
        screen = project_point(origin, matrix, (vw, vh))
        if screen is None:
            continue
        sx = _clamp(screen[0], 0, vw)
        sy = _clamp(screen[1], 0, vh)

        visible = ent.get("is_visible", True)
        hp = int(ent.get("health") or 0)
        bones = ent.get("bones")
        head = ent.get("head_pos")

        primary_color = COLOR_ENEMY_VISIBLE if visible else COLOR_ENEMY_HIDDEN
        bone_color = COLOR_BONE if visible else COLOR_BONE_HIDDEN

        # --- Skeleton ---
        if show_skeleton and bones:
            for a_idx, b_idx in SKELETON_LINES:
                pa = bones.get(a_idx)
                pb = bones.get(b_idx)
                if not pa or not pb:
                    continue
                sa = project_point(pa, matrix, (vw, vh))
                sb = project_point(pb, matrix, (vw, vh))
                if sa and sb:
                    ops.append({
                        "op": "line",
                        "x1": int(_clamp(sa[0], 0, vw)),
                        "y1": int(_clamp(sa[1], 0, vh)),
                        "x2": int(_clamp(sb[0], 0, vw)),
                        "y2": int(_clamp(sb[1], 0, vh)),
                        "fill": bone_color, "width": 1,
                    })

        # --- Head dot ---
        if show_head and head:
            hs = project_point(head, matrix, (vw, vh))
            if hs:
                hx, hy = _clamp(hs[0], 0, vw), _clamp(hs[1], 0, vh)
                r = 4
                ops.append({
                    "op": "oval",
                    "x": int(hx - r), "y": int(hy - r),
                    "w": r * 2, "h": r * 2,
                    "fill": COLOR_HEAD, "outline": "", "width": 0,
                })

        # --- Bounding box (height-estimated from head to feet) ---
        if show_box:
            if head:
                hs = project_point(head, matrix, (vw, vh))
                if hs:
                    head_y = _clamp(hs[1], 0, vh)
                    box_h = max(abs(sy - head_y) * 1.1, 20)
                    box_w = box_h * 0.5
                    bx = sx - box_w / 2
                    by = min(head_y, sy) - box_h * 0.05
                    ops.append({
                        "op": "rect",
                        "x": int(bx), "y": int(by),
                        "w": int(box_w), "h": int(box_h),
                        "fill": "", "outline": primary_color, "width": 2,
                    })
            else:
                box_w, box_h = 40, 60
                ops.append({
                    "op": "rect",
                    "x": int(sx - box_w / 2), "y": int(sy - box_h / 2),
                    "w": box_w, "h": box_h,
                    "fill": "", "outline": primary_color, "width": 2,
                })

        # --- HP bar (left side of box) ---
        if show_hp_bar and hp > 0:
            bar_h = 50
            bar_w = 4
            bar_x = int(sx - 28)
            bar_y = int(sy - bar_h / 2)
            filled = int(bar_h * hp / 100)
            ops.append({
                "op": "rect",
                "x": bar_x, "y": bar_y,
                "w": bar_w, "h": bar_h,
                "fill": "", "outline": "#333333", "width": 1,
            })
            ops.append({
                "op": "rect",
                "x": bar_x, "y": bar_y + (bar_h - filled),
                "w": bar_w, "h": filled,
                "fill": _hp_color(hp), "outline": "", "width": 0,
            })

        # --- HP text ---
        if show_health and hp > 0:
            ops.append({
                "op": "text",
                "x": int(sx), "y": int(sy - 36),
                "text": f"{hp}",
                "fill": COLOR_HEALTH, "size": 10, "anchor": "n", "bold": True,
            })

        # --- Distance ---
        if show_distance and local_origin and origin:
            dx = origin[0] - local_origin[0]
            dy = origin[1] - local_origin[1]
            dz = origin[2] - local_origin[2]
            dist = math.sqrt(dx * dx + dy * dy + dz * dz)
            dist_m = dist * 0.01905  # Source units to meters
            ops.append({
                "op": "text",
                "x": int(sx), "y": int(sy + 8),
                "text": f"{dist_m:.0f}m",
                "fill": COLOR_DISTANCE, "size": 9, "anchor": "n",
            })

        # --- Snap line (bottom center → entity) ---
        if show_snapline:
            ops.append({
                "op": "line",
                "x1": int(cx), "y1": vh,
                "x2": int(sx), "y2": int(sy),
                "fill": COLOR_SNAPLINE, "width": 1,
            })

        count += 1

    return {
        "type": "canvas",
        "width": vw,
        "height": vh,
        "bg": "transparent",
        "ops": ops,
        "_entity_count": count,
    }
