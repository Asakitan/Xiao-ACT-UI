# -*- coding: utf-8 -*-
"""Windowless software mesh preview for ``model3d`` nodes."""
from __future__ import annotations

import math
from collections.abc import Mapping
from pathlib import Path
from typing import Any

try:
    from PIL import Image, ImageColor, ImageDraw
except Exception:  # pragma: no cover - optional at import time
    Image = None  # type: ignore[assignment]
    ImageColor = None  # type: ignore[assignment]
    ImageDraw = None  # type: ignore[assignment]

try:
    from render.model3d_backend import evaluate_retarget_pose, get_model_metadata_view
except Exception:  # pragma: no cover
    evaluate_retarget_pose = None  # type: ignore[assignment]
    get_model_metadata_view = None  # type: ignore[assignment]


_GEOMETRY_CACHE_LIMIT = 64
_PREVIEW_GEOMETRY_CACHE: dict[
    tuple[Any, ...],
    tuple[list[tuple[float, float, float]], list[list[int]]],
] = {}
_SKIN_INFLUENCE_CACHE: dict[tuple[Any, ...], list[list[tuple[str, float]]]] = {}
_CHAIN_WEIGHT_CACHE: dict[tuple[Any, ...], list[list[tuple[int, float]]]] = {}
_TEXTURE_COLOR_CACHE: dict[tuple[str, int, int], tuple[int, int, int, int] | None] = {}


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
    for raw in value:
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


def clear_software_model3d_caches() -> None:
    """Clear software-preview geometry caches for tests and reload hooks."""

    _PREVIEW_GEOMETRY_CACHE.clear()
    _SKIN_INFLUENCE_CACHE.clear()
    _CHAIN_WEIGHT_CACHE.clear()
    _TEXTURE_COLOR_CACHE.clear()


def _preview_geometry_cache_key(meta: Mapping[str, Any], preview: Mapping[str, Any]) -> tuple[Any, ...]:
    raw_vertices = preview.get("vertices")
    raw_faces = preview.get("faces")
    return (
        tuple(meta.get("cache_key") or ()),
        str(preview.get("source") or ""),
        len(raw_vertices) if isinstance(raw_vertices, (list, tuple)) else 0,
        len(raw_faces) if isinstance(raw_faces, (list, tuple)) else 0,
    )


def _preview_geometry(
    meta: Mapping[str, Any],
    preview: Mapping[str, Any],
) -> tuple[list[tuple[float, float, float]], list[list[int]]]:
    key = _preview_geometry_cache_key(meta, preview)
    cached = _PREVIEW_GEOMETRY_CACHE.get(key)
    if cached is not None:
        return cached
    raw_vertices = preview.get("vertices") if isinstance(preview, Mapping) else None
    vertices = [point for point in (_point3(item) for item in (raw_vertices or [])) if point is not None]
    faces = _faces(preview.get("faces"), len(vertices)) if vertices else []
    if len(_PREVIEW_GEOMETRY_CACHE) >= _GEOMETRY_CACHE_LIMIT:
        _PREVIEW_GEOMETRY_CACHE.clear()
    cached = (vertices, faces)
    _PREVIEW_GEOMETRY_CACHE[key] = cached
    return cached


def _skin_cache_key(preview: Mapping[str, Any]) -> tuple[Any, ...]:
    skin = preview.get("skin")
    return (
        id(skin),
        len(skin) if isinstance(skin, (list, tuple)) else 0,
    )


def _preview_skin_influences(preview: Mapping[str, Any]) -> list[list[tuple[str, float]]]:
    skin = preview.get("skin")
    if not isinstance(skin, (list, tuple)) or not any(skin):
        return []
    key = _skin_cache_key(preview)
    cached = _SKIN_INFLUENCE_CACHE.get(key)
    if cached is not None:
        return cached
    parsed = [_skin_for_vertex(item) for item in skin]
    if len(_SKIN_INFLUENCE_CACHE) >= _GEOMETRY_CACHE_LIMIT:
        _SKIN_INFLUENCE_CACHE.clear()
        _CHAIN_WEIGHT_CACHE.clear()
    _SKIN_INFLUENCE_CACHE[key] = parsed
    return parsed


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


def _prepared_chains(chains: list[Mapping[str, Any]]) -> list[dict[str, Any]]:
    prepared: list[dict[str, Any]] = []
    for chain in chains:
        tokens = _chain_tokens(chain)
        if not tokens:
            continue
        prepared.append({
            "tokens": tokens,
            "axis": _point3(chain.get("axis")) or (1.0, 0.0, 0.0),
            "gravity": _point3(chain.get("gravity")) or (0.0, -1.0, 0.0),
            "amplitude": _float(chain.get("amplitude"), 0.035, 0.0, 1.0),
            "frequency": _float(chain.get("frequency"), 0.8, 0.0, 12.0),
            "phase": _float(chain.get("phase"), 0.0, -1000.0, 1000.0),
            "wave": _float(chain.get("wave"), 0.65, 0.0, 12.0),
            "damping": _float(chain.get("damping"), 0.18, 0.0, 1.0),
            "gravity_strength": _float(chain.get("gravity_strength"), 0.18, 0.0, 2.0),
        })
    return prepared


def _prepared_chain_signature(chains: list[dict[str, Any]]) -> tuple[Any, ...]:
    return tuple(
        (
            item["tokens"],
            item["axis"],
            item["gravity"],
            item["amplitude"],
            item["frequency"],
            item["phase"],
            item["wave"],
            item["damping"],
            item["gravity_strength"],
        )
        for item in chains
    )


def _influence_matches_tokens(influences: list[tuple[str, float]], tokens: tuple[str, ...]) -> float:
    total = 0.0
    for joint, weight in influences:
        text = str(joint or "").lower()
        if any(token == text or token in text for token in tokens):
            total += max(0.0, weight)
    return max(0.0, min(1.0, total))


def _chain_weights_for_skin(
    skin_influences: list[list[tuple[str, float]]],
    chains: list[dict[str, Any]],
    preview: Mapping[str, Any],
) -> list[list[tuple[int, float]]]:
    if not skin_influences or not chains:
        return []
    key = (_skin_cache_key(preview), _prepared_chain_signature(chains))
    cached = _CHAIN_WEIGHT_CACHE.get(key)
    if cached is not None:
        return cached
    weights: list[list[tuple[int, float]]] = []
    for influences in skin_influences:
        vertex_weights: list[tuple[int, float]] = []
        if influences:
            for chain_index, chain in enumerate(chains):
                weight = _influence_matches_tokens(influences, chain["tokens"])
                if weight > 0.000001:
                    vertex_weights.append((chain_index, weight))
        weights.append(vertex_weights)
    if len(_CHAIN_WEIGHT_CACHE) >= _GEOMETRY_CACHE_LIMIT:
        _CHAIN_WEIGHT_CACHE.clear()
    _CHAIN_WEIGHT_CACHE[key] = weights
    return weights


def _apply_secondary_motion(
    vertices: list[tuple[float, float, float]],
    preview: Mapping[str, Any],
    node: Mapping[str, Any],
    meta: Mapping[str, Any] | None,
    skin_influences: list[list[tuple[str, float]]] | None = None,
) -> list[tuple[float, float, float]]:
    chains = _motion_chains(node, meta)
    if not chains:
        return vertices
    if skin_influences is None:
        skin_influences = _preview_skin_influences(preview)
    if not skin_influences:
        return vertices
    prepared = _prepared_chains(chains)
    if not prepared:
        return vertices
    chain_weights = _chain_weights_for_skin(skin_influences, prepared, preview)
    time_value = _motion_time(node)
    moved: list[tuple[float, float, float]] = []
    changed = False
    for index, point in enumerate(vertices):
        vertex_chain_weights = chain_weights[index] if index < len(chain_weights) else ()
        if not vertex_chain_weights:
            moved.append(point)
            continue
        offset = (0.0, 0.0, 0.0)
        for chain_index, weight in vertex_chain_weights:
            chain = prepared[chain_index]
            axis = chain["axis"]
            gravity = chain["gravity"]
            amplitude = chain["amplitude"]
            frequency = chain["frequency"]
            phase = chain["phase"]
            wave = chain["wave"]
            damping = chain["damping"]
            gravity_strength = chain["gravity_strength"]
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
    skin_influences = _preview_skin_influences(preview)
    if not skin_influences:
        return _apply_secondary_motion(vertices, preview, node, meta, skin_influences)
    if not callable(evaluate_retarget_pose):
        return _apply_secondary_motion(vertices, preview, node, meta, skin_influences)
    try:
        pose = evaluate_retarget_pose(node)
    except Exception:
        return _apply_secondary_motion(vertices, preview, node, meta, skin_influences)
    if not isinstance(pose, Mapping) or not bool(pose.get("ok")):
        return _apply_secondary_motion(vertices, preview, node, meta, skin_influences)
    rest = _pose_points(pose, "rest_positions")
    posed = _pose_points(pose, "positions")
    rotations = _pose_rotations(pose)
    if not rest or not posed:
        return vertices

    deformed: list[tuple[float, float, float]] = []
    changed = False
    for index, point in enumerate(vertices):
        influences = skin_influences[index] if index < len(skin_influences) else ()
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
    return _apply_secondary_motion(deformed, preview, node, meta, skin_influences)


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


def _norm_material_token(value: Any) -> str:
    return "".join(ch for ch in str(value or "").lower() if ch.isalnum())


def _face_material_name(preview: Mapping[str, Any], face_index: int) -> str:
    raw = preview.get("face_materials")
    if not isinstance(raw, (list, tuple)) or face_index < 0 or face_index >= len(raw):
        return ""
    return str(raw[face_index] or "").strip()


def _material_texture_entries(config: Mapping[str, Any]) -> list[tuple[str, str]]:
    textures = config.get("textures") if isinstance(config.get("textures"), Mapping) else {}
    entries: list[tuple[str, str]] = []
    base = textures.get("base") if isinstance(textures, Mapping) else None
    if isinstance(base, Mapping):
        for key, value in base.items():
            if isinstance(value, str) and value.strip():
                entries.append((str(key), value.strip()))
    elif isinstance(textures, Mapping):
        for key, value in textures.items():
            if isinstance(value, str) and value.strip():
                entries.append((str(key), value.strip()))
    return entries


def _texture_path_candidates(meta: Mapping[str, Any], texture: str) -> list[Path]:
    text = str(texture or "").strip()
    if not text:
        return []
    raw = Path(text)
    if raw.is_absolute():
        return [raw]
    model_path = Path(str(meta.get("resolved_path") or meta.get("path") or ""))
    bases: list[Path] = []
    if str(model_path):
        bases.append(model_path.parent)
        bases.append(model_path.parent.parent)
    bases.append(Path.cwd())
    out: list[Path] = []
    seen: set[str] = set()
    for base in bases:
        try:
            candidate = (base / raw).resolve()
        except Exception:
            candidate = base / raw
        token = str(candidate)
        if token not in seen:
            seen.add(token)
            out.append(candidate)
    return out


def _average_texture_color(path: Path) -> tuple[int, int, int, int] | None:
    if Image is None:
        return None
    try:
        stat = path.stat()
    except Exception:
        return None
    key = (str(path), int(stat.st_size), int(stat.st_mtime_ns))
    if key in _TEXTURE_COLOR_CACHE:
        return _TEXTURE_COLOR_CACHE[key]
    try:
        with Image.open(path) as image:
            rgba = image.convert("RGBA")
            rgba.thumbnail((64, 64))
            raw_pixels = (
                rgba.get_flattened_data()
                if hasattr(rgba, "get_flattened_data")
                else rgba.getdata()
            )
            pixels = list(raw_pixels)
    except Exception:
        _TEXTURE_COLOR_CACHE[key] = None
        return None
    total_r = total_g = total_b = total_a = count = 0
    for r, g, b, a in pixels:
        if int(a) <= 8:
            continue
        total_r += int(r) * int(a)
        total_g += int(g) * int(a)
        total_b += int(b) * int(a)
        total_a += int(a)
        count += 1
    if count <= 0 or total_a <= 0:
        color = None
    else:
        color = (
            max(0, min(255, int(total_r / total_a))),
            max(0, min(255, int(total_g / total_a))),
            max(0, min(255, int(total_b / total_a))),
            255,
        )
    _TEXTURE_COLOR_CACHE[key] = color
    return color


def _material_texture_color(
    meta: Mapping[str, Any] | None,
    material_name: str,
) -> tuple[int, int, int, int] | None:
    if not isinstance(meta, Mapping) or not material_name:
        return None
    config = meta.get("materials_config")
    if not isinstance(config, Mapping):
        return None
    token = _norm_material_token(material_name)
    if not token:
        return None
    entries = _material_texture_entries(config)
    best: str = ""
    for name, texture in entries:
        name_token = _norm_material_token(name)
        if name_token and (name_token == token or name_token in token or token in name_token):
            best = texture
            break
    if not best:
        all_files = config.get("all_files")
        if isinstance(all_files, (list, tuple)):
            for item in all_files:
                text = str(item or "")
                text_token = _norm_material_token(Path(text).stem)
                if token and token in text_token and "normal" not in text.lower() and "mask" not in text.lower():
                    best = text
                    break
    if not best:
        return None
    for candidate in _texture_path_candidates(meta, best):
        color = _average_texture_color(candidate)
        if color is not None:
            return color
    return None


def _face_base_color(
    face: list[int],
    preview: Mapping[str, Any],
    accent: tuple[int, int, int, int],
    face_index: int = -1,
    meta: Mapping[str, Any] | None = None,
) -> tuple[int, int, int, int]:
    material_name = _face_material_name(preview, face_index)
    material_color = _material_texture_color(meta, material_name)
    if material_color is not None:
        return material_color
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

    if Image is None or ImageDraw is None or not callable(get_model_metadata_view):
        return None
    try:
        width = max(1, int(node.get("width") or 320))
        height = max(1, int(node.get("height") or 480))
    except Exception:
        width, height = 320, 480
    try:
        meta = get_model_metadata_view(node)
    except Exception:
        return None
    if not bool(meta.get("exists")):
        return None
    mesh = meta.get("mesh") if isinstance(meta.get("mesh"), Mapping) else {}
    preview = mesh.get("preview") if isinstance(mesh.get("preview"), Mapping) else {}
    vertices, faces = _preview_geometry(meta, preview)
    if len(vertices) < 3:
        return None
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

    ordered = sorted(enumerate(faces), key=lambda item: sum(projected[idx][2] for idx in item[1]) / len(item[1]))
    stroke = max(1, min(5, int(scale * 0.02)))
    depths = [p[2] for p in projected]
    min_depth, max_depth = min(depths), max(depths)
    for face_index, face in ordered:
        pts = [(projected[idx][0], projected[idx][1]) for idx in face]
        depth = sum(projected[idx][2] for idx in face) / len(face)
        normal = _face_normal(face, transformed)
        base = _face_base_color(face, preview, accent, face_index, meta)
        fill = _lit_color(base, normal, depth, min_depth, max_depth, alpha=168)
        draw.polygon(pts, fill=fill)
        draw.line(pts + [pts[0]], fill=_outline_color(base), width=stroke)
    return image


__all__ = [
    "clear_software_model3d_caches",
    "render_software_model3d_preview",
]
