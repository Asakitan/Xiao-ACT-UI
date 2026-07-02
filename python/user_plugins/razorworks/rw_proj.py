# -*- coding: utf-8 -*-
"""RW module — polished overlay rendering.

Visual style:
  - Corner box (only 4 corners, not full rectangle — cleaner)
  - Skeleton with outline (2-pass: dark shadow + colored line)
  - Head ring (hollow circle with glow)
  - HP bar: left side, gradient green→yellow→red, dark background
  - HP number: above head, drop-shadow for readability
  - Distance: below feet, muted color
  - Snap line: gradient transparency from bottom to target
  - FOV circle: dashed appearance via short segments
  - Visibility coloring: bright when visible, dim when hidden
"""

from __future__ import annotations

import math
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

from .rw_mesh import SKELETON_LINES, head_position

DEFAULT_VIEWPORT = (1920, 1080)

# ── Color palette ──
C_VIS = "#FF4455"         # visible enemy — vivid red
C_HID = "#CC8833"         # hidden enemy — dim orange
C_BONE_VIS = "#22FFAA"    # skeleton visible — bright mint
C_BONE_HID = "#668844"    # skeleton hidden — dim olive
C_BONE_SHADOW = "#00000088"  # skeleton outline shadow
C_HEAD_RING = "#FF3344"   # head ring
C_HEAD_GLOW = "#FF334466" # head glow (translucent)
C_HP_BG = "#1A1A1A"       # HP bar background
C_HP_HIGH = "#44FF44"     # HP > 75
C_HP_MID = "#FFCC00"      # HP 30-75
C_HP_LOW = "#FF3333"      # HP < 30
C_HP_TEXT = "#FFFFFF"      # HP number
C_HP_SHADOW = "#00000099"  # HP text shadow
C_DIST = "#88BBDD"        # distance text
C_SNAP = "#FFFFFF20"      # snap line (very transparent)
C_FOV = "#FFFFFF18"        # FOV circle
C_ARMOR = "#5588FF"       # armor indicator


def project_point(world: Sequence[float], matrix: Sequence[float],
                  viewport: Sequence[int]) -> Optional[Tuple[float, float]]:
    if not world or len(world) < 3 or not matrix or len(matrix) < 16:
        return None
    wx, wy, wz = float(world[0]), float(world[1]), float(world[2])
    m = matrix
    sx = m[0] * wx + m[1] * wy + m[2] * wz + m[3]
    sy = m[4] * wx + m[5] * wy + m[6] * wz + m[7]
    sw = m[12] * wx + m[13] * wy + m[14] * wz + m[15]
    if sw <= 0.0:
        return None
    inv = 1.0 / sw
    vw, vh = viewport[0], viewport[1]
    return ((sx * inv * 0.5 + 0.5) * vw,
            (1.0 - (sy * inv * 0.5 + 0.5)) * vh)


def filter_enemies(entities: Iterable[dict], local_team: int) -> List[dict]:
    try:
        local_team = int(local_team)
    except (TypeError, ValueError):
        local_team = 0
    return [e for e in (entities or [])
            if isinstance(e, dict) and int(e.get("health") or 0) > 0
            and (local_team <= 0 or int(e.get("team") or 0) != local_team)]


def _cl(v: float, lo: float, hi: float) -> int:
    return int(max(lo, min(hi, v)))


def _hp_color(hp: int) -> str:
    if hp > 75: return C_HP_HIGH
    if hp > 30: return C_HP_MID
    return C_HP_LOW


def _corner_box(ops: list, x: int, y: int, w: int, h: int,
                color: str, thick: int = 2) -> None:
    """Draw 4 corner brackets instead of full rectangle."""
    cl = max(int(min(w, h) * 0.2), 6)  # corner length
    # Top-left
    ops.append({"op": "line", "x1": x, "y1": y, "x2": x + cl, "y2": y,
                "fill": color, "width": thick})
    ops.append({"op": "line", "x1": x, "y1": y, "x2": x, "y2": y + cl,
                "fill": color, "width": thick})
    # Top-right
    ops.append({"op": "line", "x1": x+w, "y1": y, "x2": x+w-cl, "y2": y,
                "fill": color, "width": thick})
    ops.append({"op": "line", "x1": x+w, "y1": y, "x2": x+w, "y2": y+cl,
                "fill": color, "width": thick})
    # Bottom-left
    ops.append({"op": "line", "x1": x, "y1": y+h, "x2": x+cl, "y2": y+h,
                "fill": color, "width": thick})
    ops.append({"op": "line", "x1": x, "y1": y+h, "x2": x, "y2": y+h-cl,
                "fill": color, "width": thick})
    # Bottom-right
    ops.append({"op": "line", "x1": x+w, "y1": y+h, "x2": x+w-cl, "y2": y+h,
                "fill": color, "width": thick})
    ops.append({"op": "line", "x1": x+w, "y1": y+h, "x2": x+w, "y2": y+h-cl,
                "fill": color, "width": thick})


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
    cx, cy = vw // 2, vh // 2
    vp = (vw, vh)
    _pp = project_point
    m = list(matrix) if matrix else []

    # ── FOV circle (dashed via short arc segments) ──
    if show_fov_circle and fov_radius > 0:
        fov_px = int(fov_radius / 90.0 * vh)
        if fov_px > 2:
            segs = 32
            for i in range(0, segs, 2):
                a0 = 2.0 * math.pi * i / segs
                a1 = 2.0 * math.pi * (i + 1) / segs
                ops.append({"op": "line",
                    "x1": int(cx + fov_px * math.cos(a0)),
                    "y1": int(cy + fov_px * math.sin(a0)),
                    "x2": int(cx + fov_px * math.cos(a1)),
                    "y2": int(cy + fov_px * math.sin(a1)),
                    "fill": C_FOV, "width": 1})

    for ent in entities[:max(max_entities, 0)]:
        origin = ent.get("origin")
        if not origin or len(origin) < 3:
            continue
        screen = _pp(origin, m, vp)
        if screen is None:
            continue
        sx, sy = _cl(screen[0], 0, vw), _cl(screen[1], 0, vh)

        vis = ent.get("is_visible", True)
        hp = int(ent.get("health") or 0)
        bones = ent.get("bones")
        head = ent.get("head_pos")

        box_color = C_VIS if vis else C_HID
        bone_col = C_BONE_VIS if vis else C_BONE_HID

        # Pre-project
        hs = _pp(head, m, vp) if head else None
        bone_sc = {}
        if show_skeleton and bones:
            for idx in bones:
                bone_sc[idx] = _pp(bones[idx], m, vp)

        # ── Snap line ──
        if show_snapline:
            ops.append({"op": "line", "x1": cx, "y1": vh,
                        "x2": sx, "y2": sy, "fill": C_SNAP, "width": 1})

        # ── Skeleton (2-pass: shadow + color) ──
        if show_skeleton and bone_sc:
            for a_idx, b_idx in SKELETON_LINES:
                sa, sb = bone_sc.get(a_idx), bone_sc.get(b_idx)
                if sa and sb:
                    x1, y1 = _cl(sa[0], 0, vw), _cl(sa[1], 0, vh)
                    x2, y2 = _cl(sb[0], 0, vw), _cl(sb[1], 0, vh)
                    ops.append({"op": "line", "x1": x1, "y1": y1,
                                "x2": x2, "y2": y2,
                                "fill": C_BONE_SHADOW, "width": 3})
                    ops.append({"op": "line", "x1": x1, "y1": y1,
                                "x2": x2, "y2": y2,
                                "fill": bone_col, "width": 1})

        # ── Corner box ──
        if show_box:
            if hs:
                head_y = _cl(hs[1], 0, vh)
                bh = max(int(abs(sy - head_y) * 1.15), 24)
                bw = int(bh * 0.45)
                bx = sx - bw // 2
                by = min(head_y, sy) - int(bh * 0.04)
            else:
                bw, bh = 36, 56
                bx, by = sx - bw // 2, sy - bh // 2
            _corner_box(ops, bx, by, bw, bh, box_color, 2)

        # ── HP bar (left edge, with background) ──
        if show_hp_bar and hp > 0:
            if hs:
                bar_h = max(int(abs(sy - _cl(hs[1], 0, vh)) * 1.1), 24)
                bar_x = (sx - bw // 2 - 6) if show_box else (sx - 24)
                bar_y = by if show_box else (sy - bar_h // 2)
            else:
                bar_h, bar_x, bar_y = 50, sx - 24, sy - 25
            bar_w = 3
            filled = max(1, int(bar_h * hp / 100))
            # Background
            ops.append({"op": "rect", "x": bar_x, "y": bar_y,
                        "w": bar_w, "h": bar_h,
                        "fill": C_HP_BG, "outline": "", "width": 0})
            # Fill
            ops.append({"op": "rect", "x": bar_x, "y": bar_y + bar_h - filled,
                        "w": bar_w, "h": filled,
                        "fill": _hp_color(hp), "outline": "", "width": 0})

        # ── Head ring (hollow circle with glow) ──
        if show_head and hs:
            hx, hy = _cl(hs[0], 0, vw), _cl(hs[1], 0, vh)
            # Glow ring (larger, translucent)
            ops.append({"op": "oval", "x": hx - 6, "y": hy - 6,
                        "w": 12, "h": 12,
                        "fill": C_HEAD_GLOW, "outline": "", "width": 0})
            # Inner ring (crisp)
            ops.append({"op": "oval", "x": hx - 3, "y": hy - 3,
                        "w": 6, "h": 6,
                        "fill": "", "outline": C_HEAD_RING, "width": 1})

        # ── HP text (with shadow) ──
        if show_health and hp > 0 and hs:
            tx = _cl(hs[0], 0, vw)
            ty = _cl(hs[1], 0, vh) - 12
            # Shadow
            ops.append({"op": "text", "x": tx + 1, "y": ty + 1,
                        "text": str(hp), "fill": C_HP_SHADOW,
                        "size": 10, "anchor": "n", "bold": True})
            # Text
            ops.append({"op": "text", "x": tx, "y": ty,
                        "text": str(hp), "fill": C_HP_TEXT,
                        "size": 10, "anchor": "n", "bold": True})

        # ── Distance ──
        if show_distance and local_origin and origin:
            d = math.sqrt(sum((a - b) ** 2 for a, b in zip(origin, local_origin)))
            dm = d * 0.01905
            ops.append({"op": "text", "x": sx, "y": sy + 6,
                        "text": f"{dm:.0f}m", "fill": C_DIST,
                        "size": 9, "anchor": "n"})

        count += 1

    return {"type": "canvas", "width": vw, "height": vh,
            "bg": "transparent", "ops": ops, "_entity_count": count}
