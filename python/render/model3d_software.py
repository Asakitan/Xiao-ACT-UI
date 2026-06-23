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
    from render.model3d_backend import evaluate_retarget_pose, get_model_metadata
except Exception:  # pragma: no cover
    evaluate_retarget_pose = None  # type: ignore[assignment]
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


def _skin_for_vertex(value: Any) -> list[tuple[str, float]]:
    out: list[tuple[str, float]] = []
    if not isinstance(value, (list, tuple)):
        return out
    for item in value[:8]:
        if not isinstance(item, Mapping):
            continue
        joint = str(item.get("joint") or "").strip()
        if not joint:
            continue
        try:
            weight = float(item.get("weight") or 0.0)
        except Exception:
            continue
        if weight > 0.000001:
            out.append((joint, weight))
    return out


def _pose_points(pose: Mapping[str, Any], key: str) -> dict[str, tuple[float, float, float]]:
    raw = pose.get(key)
    if not isinstance(raw, Mapping):
        return {}
    out: dict[str, tuple[float, float, float]] = {}
    for name, value in raw.items():
        point = _point3(value)
        if point is not None:
            out[str(name)] = point
    return out


def _quat4(value: Any) -> tuple[float, float, float, float] | None:
    if not isinstance(value, (list, tuple)) or len(value) < 4:
        return None
    try:
        quat = (float(value[0]), float(value[1]), float(value[2]), float(value[3]))
    except Exception:
        return None
    length = math.sqrt(sum(item * item for item in quat))
    if length <= 0.000001 or not math.isfinite(length):
        return None
    return quat[0] / length, quat[1] / length, quat[2] / length, quat[3] / length


def _pose_rotations(pose: Mapping[str, Any]) -> dict[str, tuple[float, float, float, float]]:
    raw = pose.get("rotations")
    if not isinstance(raw, Mapping):
        return {}
    out: dict[str, tuple[float, float, float, float]] = {}
    for name, value in raw.items():
        quat = _quat4(value)
        if quat is not None:
            out[str(name)] = quat
    return out


def _vec_sub(
    a: tuple[float, float, float],
    b: tuple[float, float, float],
) -> tuple[float, float, float]:
    return a[0] - b[0], a[1] - b[1], a[2] - b[2]


def _vec_add(
    a: tuple[float, float, float],
    b: tuple[float, float, float],
) -> tuple[float, float, float]:
    return a[0] + b[0], a[1] + b[1], a[2] + b[2]


def _quat_rotate_vec(
    q: tuple[float, float, float, float],
    v: tuple[float, float, float],
) -> tuple[float, float, float]:
    x, y, z, w = q
    vx, vy, vz = v
    tx = 2.0 * (y * vz - z * vy)
    ty = 2.0 * (z * vx - x * vz)
    tz = 2.0 * (x * vy - y * vx)
    return (
        vx + w * tx + (y * tz - z * ty),
        vy + w * ty + (z * tx - x * tz),
        vz + w * tz + (x * ty - y * tx),
    )


def _weighted_add(
    acc: tuple[float, float, float],
    point: tuple[float, float, float],
    weight: float,
) -> tuple[float, float, float]:
    return (
        acc[0] + point[0] * weight,
        acc[1] + point[1] * weight,
        acc[2] + point[2] * weight,
    )


def _weighted_scale(
    point: tuple[float, float, float],
    weight: float,
) -> tuple[float, float, float]:
    return point[0] * weight, point[1] * weight, point[2] * weight


def _mapping(value: Any) -> Mapping[str, Any]:
    return value if isinstance(value, Mapping) else {}


def _float(value: Any, default: float, lo: float, hi: float) -> float:
    try:
        num = float(value)
    except Exception:
        return default
    if not math.isfinite(num):
        return default
    return max(lo, min(hi, num))


def _motion_time(node: Mapping[str, Any]) -> float:
    action = _mapping(node.get("action"))
    try:
        return float(action.get("time", node.get("phase", 0.0)) or 0.0)
    except Exception:
        return 0.0


def _motion_sources(node: Mapping[str, Any], meta: Mapping[str, Any] | None) -> list[Mapping[str, Any]]:
    out: list[Mapping[str, Any]] = []
    for source in (_mapping(node.get("secondary_motion")), _mapping(node.get("physics")).get("secondary_motion")):
        if isinstance(source, Mapping):
            out.append(source)
    if isinstance(meta, Mapping):
        for source in (
            _mapping(meta.get("secondary_motion")),
            _mapping(meta.get("physics")).get("secondary_motion"),
        ):
            if isinstance(source, Mapping):
                out.append(source)
    return out


def _motion_chains(node: Mapping[str, Any], meta: Mapping[str, Any] | None) -> list[Mapping[str, Any]]:
    chains: list[Mapping[str, Any]] = []
    for source in _motion_sources(node, meta):
        if source.get("enabled") is False:
            continue
        raw = source.get("chains") or source.get("spring_bones") or source.get("springs")
        if isinstance(raw, Mapping):
            raw = raw.values()
        if not isinstance(raw, (list, tuple)):
            continue
        for item in raw[:64]:
            if isinstance(item, Mapping):
                chains.append(item)
    return chains


def _chain_tokens(chain: Mapping[str, Any]) -> tuple[str, ...]:
    raw = (
        chain.get("joints")
        or chain.get("bones")
        or chain.get("tokens")
        or chain.get("match")
        or chain.get("joint")
        or chain.get("bone")
    )
    if isinstance(raw, str):
        items = [raw]
    elif isinstance(raw, (list, tuple)):
        items = list(raw)
    else:
        items = []
    return tuple(str(item).strip().lower() for item in items if str(item).strip())


def _chain_weight(influences: list[tuple[str, float]], chain: Mapping[str, Any]) -> float:
    tokens = _chain_tokens(chain)
    if not tokens:
        return 0.0
    total = 0.0
    for joint, weight in influences:
        text = str(joint or "").lower()
        if any(token == text or token in text for token in tokens):
            total += max(0.0, weight)
    return max(0.0, min(1.0, total))


def _apply_secondary_motion(
    vertices: list[tuple[float, float, float]],
    preview: Mapping[str, Any],
    node: Mapping[str, Any],
    meta: Mapping[str, Any] | None,
) -> list[tuple[float, float, float]]:
    chains = _motion_chains(node, meta)
    if not chains:
        return vertices
    skin = preview.get("skin")
    if not isinstance(skin, (list, tuple)) or not any(skin):
        return vertices
    time_value = _motion_time(node)
    moved: list[tuple[float, float, float]] = []
    changed = False
    for index, point in enumerate(vertices):
        influences = _skin_for_vertex(skin[index] if index < len(skin) else ())
        if not influences:
            moved.append(point)
            continue
        offset = (0.0, 0.0, 0.0)
        for chain in chains:
            weight = _chain_weight(influences, chain)
            if weight <= 0.000001:
                continue
            axis = _point3(chain.get("axis")) or (1.0, 0.0, 0.0)
            gravity = _point3(chain.get("gravity")) or (0.0, -1.0, 0.0)
            amplitude = _float(chain.get("amplitude"), 0.035, 0.0, 1.0)
            frequency = _float(chain.get("frequency"), 0.8, 0.0, 12.0)
            phase = _float(chain.get("phase"), 0.0, -1000.0, 1000.0)
            wave = _float(chain.get("wave"), 0.65, 0.0, 12.0)
            damping = _float(chain.get("damping"), 0.18, 0.0, 1.0)
            gravity_strength = _float(chain.get("gravity_strength"), 0.18, 0.0, 2.0)
            sample = (
                time_value * frequency * math.tau
                + phase
                + (point[0] * 0.73 + point[1] * 1.19 + point[2] * 0.41) * wave
            )
            sway = math.sin(sample) * amplitude * (1.0 - damping)
            fall = (0.5 + 0.5 * math.sin(sample - math.pi * 0.5)) * amplitude * gravity_strength
            offset = _vec_add(offset, _weighted_scale(axis, sway * weight))
            offset = _vec_add(offset, _weighted_scale(gravity, fall * weight))
        if any(abs(value) > 0.000001 for value in offset):
            changed = True
            moved.append(_vec_add(point, offset))
        else:
            moved.append(point)
    return moved if changed else vertices


def _deform_vertices(
    vertices: list[tuple[float, float, float]],
    preview: Mapping[str, Any],
    node: Mapping[str, Any],
    meta: Mapping[str, Any] | None = None,
) -> list[tuple[float, float, float]]:
    skin = preview.get("skin")
    if not isinstance(skin, (list, tuple)) or not any(skin):
        return _apply_secondary_motion(vertices, preview, node, meta)
    if not callable(evaluate_retarget_pose):
        return _apply_secondary_motion(vertices, preview, node, meta)
    try:
        pose = evaluate_retarget_pose(node)
    except Exception:
        return _apply_secondary_motion(vertices, preview, node, meta)
    if not isinstance(pose, Mapping) or not bool(pose.get("ok")):
        return _apply_secondary_motion(vertices, preview, node, meta)
    rest = _pose_points(pose, "rest_positions")
    posed = _pose_points(pose, "positions")
    rotations = _pose_rotations(pose)
    if not rest or not posed:
        return vertices

    deformed: list[tuple[float, float, float]] = []
    changed = False
    for index, point in enumerate(vertices):
        influences = _skin_for_vertex(skin[index] if index < len(skin) else ())
        if not influences:
            deformed.append(point)
            continue
        mixed = (0.0, 0.0, 0.0)
        total = 0.0
        for joint, weight in influences:
            before = rest.get(joint)
            after = posed.get(joint)
            if before is None:
                continue
            candidate = point
            quat = rotations.get(joint)
            if quat is not None:
                candidate = _vec_add(before, _quat_rotate_vec(quat, _vec_sub(candidate, before)))
            if after is not None:
                candidate = _vec_add(candidate, _vec_sub(after, before))
            mixed = _weighted_add(mixed, candidate, weight)
            total += weight
        if total <= 0.000001:
            deformed.append(point)
            continue
        if total > 1.000001:
            mixed = _weighted_scale(mixed, 1.0 / total)
        elif total < 0.999999:
            mixed = _weighted_add(mixed, point, 1.0 - total)
        moved = mixed
        if any(abs(moved[axis] - point[axis]) > 0.000001 for axis in range(3)):
            changed = True
        deformed.append(moved)
    deformed = deformed if changed else vertices
    return _apply_secondary_motion(deformed, preview, node, meta)


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
             node: Mapping[str, Any]) -> tuple[list[tuple[float, float, float]], float, list[tuple[float, float, float]]]:
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
    return projected, scale, transformed


def _face_normal(face: list[int], points: list[tuple[float, float, float]]) -> tuple[float, float, float]:
    if len(face) < 3:
        return 0.0, 0.0, 1.0
    a = points[face[0]]
    b = points[face[1]]
    c = points[face[2]]
    ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
    vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
    nx = uy * vz - uz * vy
    ny = uz * vx - ux * vz
    nz = ux * vy - uy * vx
    length = math.sqrt(nx * nx + ny * ny + nz * nz)
    if length <= 0.000001 or not math.isfinite(length):
        return 0.0, 0.0, 1.0
    return nx / length, ny / length, nz / length


def _lit_color(
    base: tuple[int, int, int, int],
    normal: tuple[float, float, float],
    depth: float,
    min_depth: float,
    max_depth: float,
    *,
    alpha: int,
) -> tuple[int, int, int, int]:
    lx, ly, lz = -0.35, -0.55, 0.76
    light_len = math.sqrt(lx * lx + ly * ly + lz * lz)
    lx, ly, lz = lx / light_len, ly / light_len, lz / light_len
    diffuse = abs(normal[0] * lx + normal[1] * ly + normal[2] * lz)
    span = max(0.0001, max_depth - min_depth)
    depth_t = (depth - min_depth) / span
    shade = 0.52 + 0.42 * diffuse + 0.14 * depth_t
    return (
        max(0, min(255, int(base[0] * shade))),
        max(0, min(255, int(base[1] * shade))),
        max(0, min(255, int(base[2] * shade))),
        alpha,
    )


def _joint_color(joint: str, accent: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    text = str(joint or "").lower()
    if "eye_green" in text or "emerald_eye" in text or "iris_green" in text:
        return 56, 226, 190, 255
    if "hair_white" in text or "white_hair" in text or "silver_hair" in text:
        return 236, 231, 218, 255
    if "rabbit_ear_inner" in text or "ear_inner" in text:
        return 224, 183, 176, 255
    if "rabbit_ear" in text or "bunny_ear" in text:
        return 154, 130, 108, 255
    if "bow" in text or "ribbon" in text or "choker" in text or "strap" in text:
        return 38, 29, 48, 255
    if "outfit_olive" in text or "dress_olive" in text or "skirt_olive" in text:
        return 126, 119, 91, 255
    if "outfit_dark" in text or "bodice" in text or "stocking" in text or "tights" in text:
        return 55, 47, 62, 255
    if "sleeve" in text or "shawl" in text or "wrap" in text:
        return 226, 219, 207, 255
    if "skin" in text or "body_skin" in text:
        return 246, 213, 194, 255
    if "head" in text or "neck" in text or "hand" in text:
        return 246, 213, 194, 255
    if "foot" in text:
        return 40, 54, 76, 255
    if "leg" in text or "knee" in text:
        return 92, 176, 204, 255
    if "hips" in text or "spine" in text or "chest" in text or "shoulder" in text:
        return 82, 192, 198, 255
    if "arm" in text or "forearm" in text:
        return 94, 191, 204, 255
    return accent


def _face_base_color(
    face: list[int],
    preview: Mapping[str, Any],
    accent: tuple[int, int, int, int],
) -> tuple[int, int, int, int]:
    skin = preview.get("skin")
    if not isinstance(skin, (list, tuple)) or not skin:
        return accent
    totals: dict[str, float] = {}
    for index in face:
        if index >= len(skin):
            continue
        for joint, weight in _skin_for_vertex(skin[index]):
            totals[joint] = totals.get(joint, 0.0) + weight
    if not totals:
        return accent
    joint = max(totals.items(), key=lambda item: item[1])[0]
    return _joint_color(joint, accent)


def _outline_color(base: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    return (
        max(0, int(base[0] * 0.58)),
        max(0, int(base[1] * 0.64)),
        max(0, int(base[2] * 0.72)),
        238,
    )


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

    vertices = _deform_vertices(vertices, preview, node, meta)
    projected, scale, transformed = _project(vertices, width, height, node)
    pal = dict(pal or {})
    accent = _color(pal.get("accent") or "#7dd3fc", (125, 211, 252, 255))
    shadow = (0, 0, 0, 72)
    image = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image, "RGBA")

    ys = [p[1] for p in projected]
    draw.ellipse((width * 0.25, max(ys) + 8, width * 0.75, max(ys) + 34), fill=shadow)

    ordered = sorted(faces, key=lambda face: sum(projected[idx][2] for idx in face) / len(face))
    stroke = max(1, min(5, int(scale * 0.02)))
    depths = [p[2] for p in projected]
    min_depth, max_depth = min(depths), max(depths)
    for face in ordered:
        pts = [(projected[idx][0], projected[idx][1]) for idx in face]
        depth = sum(projected[idx][2] for idx in face) / len(face)
        normal = _face_normal(face, transformed)
        base = _face_base_color(face, preview, accent)
        fill = _lit_color(base, normal, depth, min_depth, max_depth, alpha=168)
        draw.polygon(pts, fill=fill)
        draw.line(pts + [pts[0]], fill=_outline_color(base), width=stroke)
    return image


__all__ = ["render_software_model3d_preview"]
