# -*- coding: utf-8 -*-
"""Windowless software mesh preview for ``model3d`` nodes."""
from __future__ import annotations

import math
from collections.abc import Mapping
from typing import Any

try:
    from PIL import Image, ImageColor, ImageDraw
except Exception:  # pragma: no cover - optional at import time
    Image = None  # type: ignore[assignment]
    ImageColor = None  # type: ignore[assignment]
    ImageDraw = None  # type: ignore[assignment]

try:
    from render.model3d_backend import get_model_metadata
except Exception:  # pragma: no cover
    get_model_metadata = None  # type: ignore[assignment]


def _color(value: Any, default: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    if ImageColor is None:
        return default
    try:
        rgb = ImageColor.getrgb(str(value))
        return int(rgb[0]), int(rgb[1]), int(rgb[2]), 255
    except Exception:
        return default


def _point3(value: Any) -> tuple[float, float, float] | None:
    if not isinstance(value, (list, tuple)) or len(value) < 3:
        return None
    try:
        return float(value[0]), float(value[1]), float(value[2])
    except Exception:
        return None


def _faces(value: Any, vertex_count: int) -> list[list[int]]:
    out: list[list[int]] = []
    if not isinstance(value, (list, tuple)):
        return out
    for raw in value[:4096]:
        if not isinstance(raw, (list, tuple)):
            continue
        face: list[int] = []
        for item in raw[:8]:
            try:
                index = int(item)
            except Exception:
                continue
            if 0 <= index < vertex_count:
                face.append(index)
        if len(face) >= 3:
            out.append(face)
    return out


def _rotation(node: Mapping[str, Any]) -> tuple[float, float, float]:
    transform = node.get("transform") if isinstance(node.get("transform"), Mapping) else {}
    raw = transform.get("rotation") if isinstance(transform, Mapping) else None
    if isinstance(raw, (list, tuple)) and len(raw) >= 3:
        try:
            return math.radians(float(raw[0])), math.radians(float(raw[1])), math.radians(float(raw[2]))
        except Exception:
            pass
    return math.radians(-12.0), math.radians(180.0), 0.0


def _transform(point: tuple[float, float, float], rotation: tuple[float, float, float]) -> tuple[float, float, float]:
    x, y, z = point
    rx, ry, rz = rotation
    cy, sy = math.cos(ry), math.sin(ry)
    x, z = x * cy + z * sy, -x * sy + z * cy
    cx, sx = math.cos(rx), math.sin(rx)
    y, z = y * cx - z * sx, y * sx + z * cx
    cz, sz = math.cos(rz), math.sin(rz)
    x, y = x * cz - y * sz, x * sz + y * cz
    return x, y, z


def _project(points: list[tuple[float, float, float]], width: int, height: int,
             node: Mapping[str, Any]) -> tuple[list[tuple[float, float, float]], float]:
    rotation = _rotation(node)
    transformed = [_transform(point, rotation) for point in points]
    xs = [p[0] for p in transformed]
    ys = [p[1] for p in transformed]
    min_x, max_x = min(xs), max(xs)
    min_y, max_y = min(ys), max(ys)
    span_x = max(0.0001, max_x - min_x)
    span_y = max(0.0001, max_y - min_y)
    scale = min(width * 0.72 / span_x, height * 0.74 / span_y)
    cx = (min_x + max_x) * 0.5
    cy = (min_y + max_y) * 0.5
    projected = [
        (
            width * 0.50 + (x - cx) * scale,
            height * 0.52 - (y - cy) * scale,
            z,
        )
        for x, y, z in transformed
    ]
    return projected, scale


def render_software_model3d_preview(node: Mapping[str, Any], pal: Mapping[str, Any] | None = None) -> Any:
    """Render a bounded mesh/bbox preview into an RGBA image.

    This is a CPU fallback for formats where lightweight metadata extraction
    has enough vertices/faces to draw a useful preview.  It never creates a
    window and never owns input/z-order.
    """

    if Image is None or ImageDraw is None or not callable(get_model_metadata):
        return None
    try:
        width = max(1, int(node.get("width") or 320))
        height = max(1, int(node.get("height") or 480))
    except Exception:
        width, height = 320, 480
    try:
        meta = get_model_metadata(node)
    except Exception:
        return None
    if not bool(meta.get("exists")):
        return None
    mesh = meta.get("mesh") if isinstance(meta.get("mesh"), Mapping) else {}
    preview = mesh.get("preview") if isinstance(mesh.get("preview"), Mapping) else {}
    raw_vertices = preview.get("vertices") if isinstance(preview, Mapping) else None
    vertices = [point for point in (_point3(item) for item in (raw_vertices or [])) if point is not None]
    if len(vertices) < 3:
        return None
    faces = _faces(preview.get("faces"), len(vertices))
    if not faces:
        return None
    if len(vertices) < 6 or len(faces) < 4:
        return None

    projected, scale = _project(vertices, width, height, node)
    pal = dict(pal or {})
    accent = _color(pal.get("accent") or "#7dd3fc", (125, 211, 252, 255))
    line = (accent[0], accent[1], accent[2], 220)
    fill = (accent[0], accent[1], accent[2], 42)
    shadow = (0, 0, 0, 72)
    image = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image, "RGBA")

    ys = [p[1] for p in projected]
    draw.ellipse((width * 0.25, max(ys) + 8, width * 0.75, max(ys) + 34), fill=shadow)

    ordered = sorted(faces, key=lambda face: sum(projected[idx][2] for idx in face) / len(face))
    stroke = max(1, min(5, int(scale * 0.02)))
    for face in ordered:
        pts = [(projected[idx][0], projected[idx][1]) for idx in face]
        draw.polygon(pts, fill=fill)
        draw.line(pts + [pts[0]], fill=line, width=stroke)
    return image


__all__ = ["render_software_model3d_preview"]
