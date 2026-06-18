# -*- coding: utf-8 -*-
"""CS2 ESP plugin — world→screen projection and overlay spec builder.

Pure-Python and side-effect free so it can be unit-tested with fake
snapshots. The projection uses the standard Source 2 4x4 row-major
view matrix layout (16 floats read straight from ``dwViewMatrix``).
"""

from __future__ import annotations

from typing import Any, Iterable, List, Optional, Sequence, Tuple

# Conservative viewport defaults; the plugin panel can override width/height.
DEFAULT_VIEWPORT = (1920, 1080)


def project_point(world: Sequence[float], matrix: Sequence[float],
                  viewport: Sequence[int]) -> Optional[Tuple[float, float]]:
    """Project a 3D world point to screen (x, y) or None if behind camera.

    ``matrix`` is 16 floats, row-major as read from ``dwViewMatrix``.
    """
    if not world or len(world) < 3 or not matrix or len(matrix) < 16:
        return None
    wx, wy, wz = float(world[0]), float(world[1]), float(world[2])
    m = [float(v) for v in matrix]
    # Row-major: m[r*4 + c]
    sx = m[0] * wx + m[1] * wy + m[2] * wz + m[3]
    sy = m[4] * wx + m[5] * wy + m[6] * wz + m[7]
    sz = m[8] * wx + m[9] * wy + m[10] * wz + m[11]
    sw = m[12] * wx + m[13] * wy + m[14] * wz + m[15]
    if sw == 0.0:
        return None
    # Behind/above camera cutoff: Source 2 uses w<=0 for "behind".
    if sw <= 0.0:
        return None
    inv = 1.0 / sw
    ndc_x = sx * inv
    ndc_y = sy * inv
    vw, vh = int(viewport[0]), int(viewport[1])
    if vw <= 0 or vh <= 0:
        return None
    screen_x = (ndc_x * 0.5 + 0.5) * vw
    screen_y = (1.0 - (ndc_y * 0.5 + 0.5)) * vh
    return float(screen_x), float(screen_y)


def filter_enemies(entities: Iterable[dict], local_team: int) -> List[dict]:
    """Keep only alive entities whose team differs from the local team."""
    out = []
    for ent in entities or []:
        if not isinstance(ent, dict):
            continue
        if int(ent.get("health") or 0) <= 0:
            continue
        if int(ent.get("team") or 0) == int(local_team):
            continue
        out.append(ent)
    return out


def build_overlay_spec(entities: Sequence[dict], matrix: Sequence[float],
                       viewport: Sequence[int], *,
                       show_health: bool = True,
                       max_entities: int = 32) -> dict:
    """Build a declarative overlay spec (canvas + rect/text ops).

    Returns a ui_spec-shaped dict the plugin passes to ``ctx.set_overlay``.
    Coordinates are clamped to the viewport and the entity count is capped
    to avoid runaway draw lists on a bad read.
    """
    vw, vh = int(viewport[0]), int(viewport[1])
    if vw <= 0 or vh <= 0:
        vw, vh = DEFAULT_VIEWPORT
    ops: list = []
    count = 0
    for ent in entities[:max(max_entities, 0)]:
        origin = ent.get("origin")
        if not origin or len(origin) < 3:
            continue
        screen = project_point(origin, matrix, (vw, vh))
        if screen is None:
            continue
        sx, sy = screen
        # Clamp to viewport so a misestimated matrix can't draw offscreen
        # garbage that confuses the renderer.
        sx = max(0.0, min(float(vw), sx))
        sy = max(0.0, min(float(vh), sy))
        # Simple 2D box marker; a full skeleton/HP bar can layer on later.
        box_w, box_h = 40, 60
        x0 = sx - box_w / 2.0
        y0 = sy - box_h / 2.0
        ops.append({
            "op": "rect", "x": int(x0), "y": int(y0),
            "w": box_w, "h": box_h,
            "fill": "", "outline": "#FF3B3B", "width": 2,
        })
        if show_health:
            label = f"HP {int(ent.get('health') or 0)}"
            ops.append({
                "op": "text", "x": int(x0), "y": int(y0) - 14,
                "text": label, "fill": "#FFD23B",
                "size": 10, "anchor": "nw", "bold": True,
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

