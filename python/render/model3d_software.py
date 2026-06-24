# -*- coding: utf-8 -*-
"""Windowless software mesh preview for ``model3d`` nodes."""
from __future__ import annotations

import math
from collections.abc import Mapping
from pathlib import Path
from typing import Any

try:
    import numpy as _np
except Exception:  # pragma: no cover - optional at import time
    _np = None  # type: ignore[assignment]

try:
    from PIL import Image, ImageColor, ImageDraw
except Exception:  # pragma: no cover - optional at import time
    Image = None  # type: ignore[assignment]
    ImageColor = None  # type: ignore[assignment]
    ImageDraw = None  # type: ignore[assignment]

try:
    from render import model3d_backend as _model3d_backend
    from render.model3d_backend import get_model_metadata_view
except Exception:  # pragma: no cover
    _model3d_backend = None  # type: ignore[assignment]
    get_model_metadata_view = None  # type: ignore[assignment]


_GEOMETRY_CACHE_LIMIT = 64
_PREVIEW_GEOMETRY_CACHE: dict[
    tuple[Any, ...],
    tuple[list[tuple[float, float, float]], list[list[int]]],
] = {}
_SKIN_INFLUENCE_CACHE: dict[tuple[Any, ...], list[list[tuple[str, float]]]] = {}
_SKIN_BATCH_CACHE: dict[tuple[Any, ...], dict[str, Any]] = {}
_CHAIN_WEIGHT_CACHE: dict[tuple[Any, ...], list[list[tuple[int, float]]]] = {}
_TEXTURE_COLOR_CACHE: dict[tuple[str, int, int], tuple[int, int, int, int] | None] = {}
_CANONICAL_TOKEN_CACHE: dict[str, str] = {}
_POSE_KEY_CANDIDATE_CACHE: dict[str, tuple[str, ...]] = {}
_SPRING_WORLD_CACHE: dict[tuple[Any, ...], dict[str, Any]] = {}


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


def _vec3_or(value: Any, default: tuple[float, float, float]) -> tuple[float, float, float]:
    point = _point3(value)
    return point if point is not None else default


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
    _SKIN_BATCH_CACHE.clear()
    _CHAIN_WEIGHT_CACHE.clear()
    _TEXTURE_COLOR_CACHE.clear()
    _CANONICAL_TOKEN_CACHE.clear()
    _POSE_KEY_CANDIDATE_CACHE.clear()
    _SPRING_WORLD_CACHE.clear()


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
    for item in value:
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


def _quat_identity() -> tuple[float, float, float, float]:
    return 0.0, 0.0, 0.0, 1.0


def _quat_normalize(
    value: tuple[float, float, float, float],
) -> tuple[float, float, float, float]:
    length = math.sqrt(sum(item * item for item in value))
    if length <= 0.000001 or not math.isfinite(length):
        return _quat_identity()
    return value[0] / length, value[1] / length, value[2] / length, value[3] / length


def _quat_mul(
    a: tuple[float, float, float, float],
    b: tuple[float, float, float, float],
) -> tuple[float, float, float, float]:
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return _quat_normalize((
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    ))


def _canonical_from_token(value: Any) -> str:
    raw = str(value or "").strip()
    cached = _CANONICAL_TOKEN_CACHE.get(raw)
    if cached is not None:
        return cached
    fn = getattr(_model3d_backend, "_canonical_from_token", None)
    if callable(fn):
        try:
            out = str(fn(raw) or "")
            if len(_CANONICAL_TOKEN_CACHE) >= 512:
                _CANONICAL_TOKEN_CACHE.clear()
            _CANONICAL_TOKEN_CACHE[raw] = out
            return out
        except Exception:
            return ""
    return ""


def _pose_key_candidates(name: str) -> tuple[str, ...]:
    raw = str(name or "").strip()
    cached = _POSE_KEY_CANDIDATE_CACHE.get(raw)
    if cached is not None:
        return cached
    canonical = _canonical_from_token(raw)
    out: list[str] = []
    for item in (raw, canonical):
        if item and item not in out:
            out.append(item)
    result = tuple(out)
    if len(_POSE_KEY_CANDIDATE_CACHE) >= 512:
        _POSE_KEY_CANDIDATE_CACHE.clear()
    _POSE_KEY_CANDIDATE_CACHE[raw] = result
    return result


def _vec_cross(
    a: tuple[float, float, float],
    b: tuple[float, float, float],
) -> tuple[float, float, float]:
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def _axis_angle_quat(
    axis: tuple[float, float, float],
    angle: float,
) -> tuple[float, float, float, float]:
    length = _vec_len(axis)
    if length <= 0.000001 or abs(angle) <= 0.000001:
        return _quat_identity()
    half = float(angle) * 0.5
    scale = math.sin(half) / length
    return _quat_normalize((axis[0] * scale, axis[1] * scale, axis[2] * scale, math.cos(half)))


def _quat_from_vectors(
    src: tuple[float, float, float],
    dst: tuple[float, float, float],
) -> tuple[float, float, float, float]:
    a = _vec_normalize(src, (1.0, 0.0, 0.0))
    b = _vec_normalize(dst, (1.0, 0.0, 0.0))
    dot = max(-1.0, min(1.0, _vec_dot(a, b)))
    if dot >= 0.999999:
        return _quat_identity()
    if dot <= -0.999999:
        axis = _vec_cross((1.0, 0.0, 0.0), a)
        if _vec_len(axis) <= 0.000001:
            axis = _vec_cross((0.0, 1.0, 0.0), a)
        return _axis_angle_quat(axis, math.pi)
    axis = _vec_cross(a, b)
    return _quat_normalize((axis[0], axis[1], axis[2], 1.0 + dot))


def _humanoid_segments() -> tuple[tuple[str, str], ...]:
    value = getattr(_model3d_backend, "HUMANOID_SEGMENTS", ())
    if isinstance(value, tuple):
        return value
    return tuple(value) if isinstance(value, list) else ()


def _humanoid_parent_by_child() -> dict[str, str]:
    out: dict[str, str] = {}
    for parent, child in _humanoid_segments():
        out[str(child)] = str(parent)
    return out


def _derived_pose_rotations(
    rest: Mapping[str, tuple[float, float, float]],
    posed: Mapping[str, tuple[float, float, float]],
    base: Mapping[str, tuple[float, float, float, float]],
) -> dict[str, tuple[float, float, float, float]]:
    out = dict(base)
    derived: dict[str, tuple[float, float, float, float]] = {}
    for parent, child in _humanoid_segments():
        if parent in derived or parent in base:
            continue
        before_parent = rest.get(parent)
        before_child = rest.get(child)
        after_parent = posed.get(parent)
        after_child = posed.get(child)
        if before_parent is None or before_child is None or after_parent is None or after_child is None:
            continue
        rest_vec = _vec_sub(before_child, before_parent)
        posed_vec = _vec_sub(after_child, after_parent)
        if _vec_len(rest_vec) <= 0.000001 or _vec_len(posed_vec) <= 0.000001:
            continue
        quat = _quat_from_vectors(rest_vec, posed_vec)
        if _transform_has_motion((before_parent, after_parent, quat)):
            derived[parent] = quat
            out[parent] = quat
    parents = _humanoid_parent_by_child()
    for bone, parent in parents.items():
        if bone not in out and parent in out:
            out[bone] = out[parent]
    return out


def _transform_has_motion(
    transform: tuple[
        tuple[float, float, float],
        tuple[float, float, float],
        tuple[float, float, float, float] | None,
    ] | None,
) -> bool:
    if transform is None:
        return False
    before, after, quat = transform
    if any(abs(after[axis] - before[axis]) > 0.000001 for axis in range(3)):
        return True
    if quat is not None:
        return any(abs(quat[axis]) > 0.000001 for axis in range(3)) or abs(quat[3] - 1.0) > 0.000001
    return False


def _pose_transform_for_key(
    key: str,
    rest: Mapping[str, tuple[float, float, float]],
    posed: Mapping[str, tuple[float, float, float]],
    rotations: Mapping[str, tuple[float, float, float, float]],
) -> tuple[
    tuple[float, float, float],
    tuple[float, float, float],
    tuple[float, float, float, float] | None,
] | None:
    before = rest.get(key)
    if before is None:
        return None
    return before, posed.get(key, before), rotations.get(key)


def _direct_joint_transform(
    joint: str,
    rest: Mapping[str, tuple[float, float, float]],
    posed: Mapping[str, tuple[float, float, float]],
    rotations: Mapping[str, tuple[float, float, float, float]],
) -> tuple[
    tuple[float, float, float],
    tuple[float, float, float],
    tuple[float, float, float, float] | None,
] | None:
    fallback = None
    for key in _pose_key_candidates(joint):
        transform = _pose_transform_for_key(key, rest, posed, rotations)
        if transform is None:
            continue
        if _transform_has_motion(transform):
            return transform
        if fallback is None:
            fallback = transform
    return fallback


def _node_parent_map(meta: Mapping[str, Any] | None) -> Mapping[str, Any]:
    if not isinstance(meta, Mapping):
        return {}
    parents = meta.get("node_parents")
    return parents if isinstance(parents, Mapping) else {}


def _inherited_joint_transform(
    joint: str,
    rest: Mapping[str, tuple[float, float, float]],
    posed: Mapping[str, tuple[float, float, float]],
    rotations: Mapping[str, tuple[float, float, float, float]],
    meta: Mapping[str, Any] | None,
) -> tuple[
    tuple[float, float, float],
    tuple[float, float, float],
    tuple[float, float, float, float] | None,
] | None:
    parents = _node_parent_map(meta)
    if not parents:
        return None
    original = str(joint or "").strip()
    current = original
    original_before = rest.get(original)
    seen: set[str] = set()
    for _ in range(64):
        if not current or current in seen:
            break
        seen.add(current)
        parent = str(parents.get(current) or "").strip()
        if not parent:
            break
        transform = _direct_joint_transform(parent, rest, posed, rotations)
        if _transform_has_motion(transform) and original_before is not None:
            after = _apply_pose_transform(original_before, transform)
            quat = transform[2] if transform is not None else None
            return original_before, after, quat
        current = parent
    return None


def _joint_pose_transform(
    joint: str,
    rest: Mapping[str, tuple[float, float, float]],
    posed: Mapping[str, tuple[float, float, float]],
    rotations: Mapping[str, tuple[float, float, float, float]],
    meta: Mapping[str, Any] | None,
) -> tuple[
    tuple[float, float, float],
    tuple[float, float, float],
    tuple[float, float, float, float] | None,
] | None:
    direct = _direct_joint_transform(joint, rest, posed, rotations)
    if _transform_has_motion(direct):
        return direct
    inherited = _inherited_joint_transform(joint, rest, posed, rotations, meta)
    if inherited is not None:
        return inherited
    return direct


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


def _vec_dot(
    a: tuple[float, float, float],
    b: tuple[float, float, float],
) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _vec_len(a: tuple[float, float, float]) -> float:
    return math.sqrt(max(0.0, _vec_dot(a, a)))


def _vec_normalize(
    value: tuple[float, float, float],
    fallback: tuple[float, float, float] = (0.0, 1.0, 0.0),
) -> tuple[float, float, float]:
    length = _vec_len(value)
    if length <= 0.000001:
        return fallback
    return value[0] / length, value[1] / length, value[2] / length


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


def _physics_float(value: Any, default: float) -> float:
    try:
        num = float(value)
    except Exception:
        return default
    return num if math.isfinite(num) else default


def _physics_positive_float(value: Any, default: float) -> float:
    num = _physics_float(value, default)
    return num if num > 0.0 else default


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
        for item in raw:
            if isinstance(item, Mapping):
                chains.append(item)
    return chains


def _chain_joint_names(chain: Mapping[str, Any]) -> tuple[str, ...]:
    raw = (
        chain.get("joints")
        or chain.get("bones")
        or chain.get("joint")
        or chain.get("bone")
    )
    if isinstance(raw, str):
        items = [raw]
    elif isinstance(raw, (list, tuple)):
        items = list(raw)
    else:
        items = []
    out: list[str] = []
    seen: set[str] = set()
    for item in items:
        text = str(item or "").strip()
        if text and text not in seen:
            seen.add(text)
            out.append(text)
    return tuple(out)


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
        joints = _chain_joint_names(chain)
        tokens = _chain_tokens(chain)
        if not tokens and not joints:
            continue
        prepared.append({
            "name": str(chain.get("name") or ""),
            "joints": joints,
            "tokens": tokens,
            "axis": _vec_normalize(_point3(chain.get("axis")) or (1.0, 0.0, 0.0), (1.0, 0.0, 0.0)),
            "gravity": _point3(chain.get("gravity")) or (0.0, -1.0, 0.0),
            "amplitude": _physics_float(chain.get("amplitude"), 0.035),
            "frequency": _physics_float(chain.get("frequency"), 0.8),
            "phase": _physics_float(chain.get("phase"), 0.0),
            "wave": _physics_float(chain.get("wave"), 0.65),
            "damping": _physics_float(chain.get("damping"), 0.18),
            "gravity_strength": _physics_float(chain.get("gravity_strength"), 0.18),
            "stiffness": _physics_positive_float(chain.get("stiffness"), 18.0),
            "mass": _physics_positive_float(chain.get("mass"), 0.035),
            "radius": _physics_positive_float(chain.get("radius") or chain.get("collision_radius"), 0.018),
            "friction": _physics_float(chain.get("friction"), 0.62),
            "restitution": _physics_float(chain.get("restitution"), 0.02),
            "pin_root": chain.get("pin_root") is not False,
        })
    return prepared


def _prepared_chain_signature(chains: list[dict[str, Any]]) -> tuple[Any, ...]:
    return tuple(
        (
            item["name"],
            item["joints"],
            item["tokens"],
            item["axis"],
            item["gravity"],
            item["amplitude"],
            item["frequency"],
            item["phase"],
            item["wave"],
            item["damping"],
            item["gravity_strength"],
            item["stiffness"],
            item["mass"],
            item["radius"],
            item["friction"],
            item["restitution"],
            item["pin_root"],
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


def _apply_pose_transform(
    point: tuple[float, float, float],
    transform: tuple[
        tuple[float, float, float],
        tuple[float, float, float],
        tuple[float, float, float, float] | None,
    ] | None,
) -> tuple[float, float, float]:
    if transform is None:
        return point
    before, after, quat = transform
    candidate = point
    if quat is not None:
        candidate = _vec_add(before, _quat_rotate_vec(quat, _vec_sub(candidate, before)))
    return _vec_add(candidate, _vec_sub(after, before))


def _current_joint_position(
    joint: str,
    rest: Mapping[str, tuple[float, float, float]],
    posed: Mapping[str, tuple[float, float, float]],
    rotations: Mapping[str, tuple[float, float, float, float]],
    meta: Mapping[str, Any] | None,
) -> tuple[float, float, float] | None:
    source = None
    for key in _pose_key_candidates(joint):
        if key in rest:
            source = rest[key]
            break
    if source is None:
        return None
    return _apply_pose_transform(source, _joint_pose_transform(joint, rest, posed, rotations, meta))


def _physics_world_key(
    node: Mapping[str, Any],
    meta: Mapping[str, Any] | None,
    chains: list[dict[str, Any]],
) -> tuple[Any, ...]:
    model_key = tuple(meta.get("cache_key") or ()) if isinstance(meta, Mapping) else ()
    return (
        model_key,
        str(node.get("id") or node.get("name") or ""),
        _prepared_chain_signature(chains),
    )


def _physics_root_motion(
    node: Mapping[str, Any],
    state: Mapping[str, Any],
) -> tuple[tuple[float, float, float], tuple[float, float, float]]:
    try:
        screen_x = float(node.get("x") or 0.0)
    except Exception:
        screen_x = 0.0
    try:
        screen_y = float(node.get("y") or 0.0)
    except Exception:
        screen_y = 0.0
    try:
        screen_z = float(node.get("z") or 0.0)
    except Exception:
        screen_z = 0.0
    try:
        extent = max(1.0, float(node.get("width") or 0.0), float(node.get("height") or 0.0))
    except Exception:
        extent = 512.0
    screen_to_world = 2.2 / extent
    origin = state.get("root_origin")
    if not isinstance(origin, tuple) or len(origin) < 3:
        origin = (screen_x, screen_y, screen_z)
        state["root_origin"] = origin
    last = state.get("root_last")
    if not isinstance(last, tuple) or len(last) < 3:
        last = origin
    offset = (
        (screen_x - float(origin[0])) * screen_to_world,
        -(screen_y - float(origin[1])) * screen_to_world,
        (screen_z - float(origin[2])) * screen_to_world,
    )
    velocity = (
        (screen_x - float(last[0])) * screen_to_world * 60.0,
        -(screen_y - float(last[1])) * screen_to_world * 60.0,
        (screen_z - float(last[2])) * screen_to_world * 60.0,
    )
    state["root_last"] = (screen_x, screen_y, screen_z)
    return offset, velocity


def _spring_world(
    node: Mapping[str, Any],
    meta: Mapping[str, Any] | None,
    chains: list[dict[str, Any]],
) -> dict[str, Any]:
    key = _physics_world_key(node, meta, chains)
    state = _SPRING_WORLD_CACHE.get(key)
    if state is not None:
        return state
    state = {
        "chains": {},
        "last_time": None,
        "root_origin": None,
        "root_last": None,
    }
    if len(_SPRING_WORLD_CACHE) >= _GEOMETRY_CACHE_LIMIT:
        _SPRING_WORLD_CACHE.clear()
    _SPRING_WORLD_CACHE[key] = state
    return state


def _joint_distance(
    a: tuple[float, float, float] | None,
    b: tuple[float, float, float] | None,
    default: float,
) -> float:
    if a is None or b is None:
        return default
    length = _vec_len(_vec_sub(a, b))
    return length if length > 0.000001 else default


def _physics_colliders(
    rest: Mapping[str, tuple[float, float, float]],
    posed: Mapping[str, tuple[float, float, float]],
    rotations: Mapping[str, tuple[float, float, float, float]],
    meta: Mapping[str, Any] | None,
    root_offset: tuple[float, float, float] = (0.0, 0.0, 0.0),
) -> list[tuple[str, tuple[float, float, float], float]]:
    positions = {
        key: _current_joint_position(key, rest, posed, rotations, meta)
        for key in (
            "head", "neck", "chest", "hips",
            "left_shoulder", "right_shoulder",
            "left_hand", "right_hand",
        )
    }
    shoulder_width = _joint_distance(positions.get("left_shoulder"), positions.get("right_shoulder"), 0.20)
    torso_height = _joint_distance(positions.get("chest"), positions.get("hips"), 0.24)
    head_radius = _joint_distance(positions.get("neck"), positions.get("head"), shoulder_width * 0.42) * 0.72
    chest_radius = max(shoulder_width * 0.46, torso_height * 0.34)
    hips_radius = max(shoulder_width * 0.34, torso_height * 0.28)
    hand_radius = max(shoulder_width * 0.10, 0.025)
    out: list[tuple[str, tuple[float, float, float], float]] = []
    for name, radius in (
        ("head", head_radius),
        ("chest", chest_radius),
        ("hips", hips_radius),
        ("left_hand", hand_radius),
        ("right_hand", hand_radius),
    ):
        point = positions.get(name)
        if point is not None and radius > 0.000001:
            out.append((name, _vec_add(point, root_offset), radius))
    return out


def _chain_target_positions(
    chain: Mapping[str, Any],
    rest: Mapping[str, tuple[float, float, float]],
    posed: Mapping[str, tuple[float, float, float]],
    rotations: Mapping[str, tuple[float, float, float, float]],
    meta: Mapping[str, Any] | None,
    root_offset: tuple[float, float, float] = (0.0, 0.0, 0.0),
) -> tuple[list[str], list[tuple[float, float, float]]]:
    joints: list[str] = []
    targets: list[tuple[float, float, float]] = []
    for joint in chain.get("joints") or ():
        point = _current_joint_position(str(joint), rest, posed, rotations, meta)
        if point is None:
            continue
        joints.append(str(joint))
        targets.append(_vec_add(point, root_offset))
    return joints, targets


def _chain_gravity_sag_offsets(
    chain: Mapping[str, Any],
    joints: list[str],
    targets: list[tuple[float, float, float]],
) -> dict[str, tuple[float, float, float]]:
    if len(joints) < 2 or len(joints) != len(targets):
        return {}
    gravity = _vec3_or(chain.get("gravity"), (0.0, -1.0, 0.0))
    gravity_dir = _vec_normalize(gravity, (0.0, -1.0, 0.0))
    try:
        strength = float(chain.get("gravity_strength") or 0.0)
    except Exception:
        strength = 0.0
    if strength <= 0.000001:
        return {}
    # Spring-bone runtimes keep the root pinned and solve child joints toward
    # a gravity-biased chain shape before adding dynamic inertia.  Mapping the
    # authored 0..1-style gravity strength onto a direction blend gives visible
    # cloth/hair sag without requiring high per-frame Bullet forces.
    blend = max(0.0, min(0.92, strength * 1.8))
    clean_targets = [_vec3_or(target, (0.0, 0.0, 0.0)) for target in targets]
    sagged: list[tuple[float, float, float]] = [clean_targets[0]]
    offsets: dict[str, tuple[float, float, float]] = {joints[0]: (0.0, 0.0, 0.0)}
    for index in range(1, len(clean_targets)):
        previous_target = clean_targets[index - 1]
        target = clean_targets[index]
        segment = _vec_sub(target, previous_target)
        length = _vec_len(segment)
        if length <= 0.000001:
            sagged.append(target)
            offsets[joints[index]] = (0.0, 0.0, 0.0)
            continue
        rest_dir = _vec_normalize(segment, (1.0, 0.0, 0.0))
        direction = _vec_normalize(
            _vec_add(
                _weighted_scale(rest_dir, 1.0 - blend),
                _weighted_scale(gravity_dir, blend),
            ),
            gravity_dir,
        )
        point = _vec_add(sagged[len(sagged) - 1], _weighted_scale(direction, length))
        sagged.append(point)
        offsets[joints[index]] = _vec_sub(point, target)
    return offsets


def _chain_gravity_targets(
    chain: Mapping[str, Any],
    targets: list[tuple[float, float, float]],
) -> list[tuple[float, float, float]]:
    if len(targets) < 2:
        return list(targets)
    gravity_dir = _vec_normalize(_vec3_or(chain.get("gravity"), (0.0, -1.0, 0.0)), (0.0, -1.0, 0.0))
    try:
        strength = float(chain.get("gravity_strength") or 0.0)
    except Exception:
        strength = 0.0
    blend = max(0.0, min(0.72, strength * 1.26))
    if blend <= 0.000001:
        return [_vec3_or(target, (0.0, 0.0, 0.0)) for target in targets]
    clean = [_vec3_or(target, (0.0, 0.0, 0.0)) for target in targets]
    out = [clean[0]]
    for index in range(1, len(clean)):
        segment = _vec_sub(clean[index], clean[index - 1])
        length = _vec_len(segment)
        if length <= 0.000001:
            out.append(clean[index])
            continue
        rest_dir = _vec_normalize(segment, (1.0, 0.0, 0.0))
        desired_dir = _vec_normalize(
            _vec_add(_weighted_scale(rest_dir, 1.0 - blend), _weighted_scale(gravity_dir, blend)),
            rest_dir,
        )
        out.append(_vec_add(out[len(out) - 1], _weighted_scale(desired_dir, length)))
    return out


def _spring_chain_signature(
    joints: list[str],
    targets: list[tuple[float, float, float]],
) -> tuple[Any, ...]:
    lengths = []
    for index in range(len(targets) - 1):
        lengths.append(round(_vec_len(_vec_sub(targets[index + 1], targets[index])), 5))
    return tuple(joints), tuple(lengths)


def _spring_coeff(chain: Mapping[str, Any]) -> float:
    try:
        stiffness = float(chain.get("stiffness") or 0.0)
    except Exception:
        stiffness = 0.0
    if stiffness <= 1.0:
        return max(0.12, min(0.95, stiffness))
    return max(0.12, min(0.95, stiffness * 0.04))


def _spring_drag(chain: Mapping[str, Any]) -> float:
    try:
        damping = float(chain.get("damping") or 0.0)
    except Exception:
        damping = 0.0
    return max(0.0, min(0.96, damping))


def _spring_drive(
    chain: Mapping[str, Any],
    target: tuple[float, float, float],
    index: int,
    time_value: float,
) -> tuple[float, float, float]:
    axis = _vec_normalize(_vec3_or(chain.get("axis"), (1.0, 0.0, 0.0)), (1.0, 0.0, 0.0))
    amplitude = float(chain.get("amplitude") or 0.0)
    if abs(amplitude) <= 0.000001:
        return 0.0, 0.0, 0.0
    frequency = float(chain.get("frequency") or 0.8)
    phase = float(chain.get("phase") or 0.0)
    wave = float(chain.get("wave") or 0.0)
    sample = (
        time_value * frequency * math.tau
        + phase
        + (target[0] * 0.73 + target[1] * 1.19 + target[2] * 0.41 + index * 0.37) * wave
    )
    return _weighted_scale(axis, math.sin(sample) * amplitude)


def _constrain_to_parent(
    point: tuple[float, float, float],
    parent: tuple[float, float, float],
    rest_target: tuple[float, float, float],
    length: float,
) -> tuple[float, float, float]:
    if length <= 0.000001:
        return point
    direction = _vec_sub(point, parent)
    current_len = _vec_len(direction)
    if current_len <= 0.000001:
        direction = _vec_normalize(_vec_sub(rest_target, parent), (1.0, 0.0, 0.0))
    else:
        direction = _weighted_scale(direction, 1.0 / current_len)
    return _vec_add(parent, _weighted_scale(direction, length))


def _resolve_sphere_collisions(
    point: tuple[float, float, float],
    radius: float,
    colliders: list[tuple[str, tuple[float, float, float], float]],
) -> tuple[float, float, float]:
    out = point
    point_radius = max(0.0, float(radius or 0.0))
    for _name, center, collider_radius in colliders:
        min_dist = max(0.0, float(collider_radius or 0.0)) + point_radius
        delta = _vec_sub(out, center)
        dist = _vec_len(delta)
        if dist >= min_dist or min_dist <= 0.000001:
            continue
        direction = _vec_normalize(delta, (0.0, 1.0, 0.0))
        out = _vec_add(center, _weighted_scale(direction, min_dist))
    return out


def _spring_secondary_offsets(
    node: Mapping[str, Any],
    meta: Mapping[str, Any] | None,
    chains: list[dict[str, Any]],
    rest: Mapping[str, tuple[float, float, float]],
    posed: Mapping[str, tuple[float, float, float]],
    rotations: Mapping[str, tuple[float, float, float, float]],
) -> dict[str, tuple[float, float, float]]:
    state = _spring_world(node, meta, chains)
    _root_offset, root_velocity = _physics_root_motion(node, state)
    chain_targets = []
    for chain_index, chain in enumerate(chains):
        joints, targets = _chain_target_positions(chain, rest, posed, rotations, meta, (0.0, 0.0, 0.0))
        if joints and len(joints) == len(targets):
            chain_targets.append((chain_index, chain, joints, targets))
    if not chain_targets:
        return {}
    colliders = _physics_colliders(rest, posed, rotations, meta, (0.0, 0.0, 0.0))
    time_value = _motion_time(node)
    fixed_dt = 1.0 / 60.0
    offsets: dict[str, tuple[float, float, float]] = {}
    chain_state = state.setdefault("chains", {})
    for chain_index, chain, joints, targets in chain_targets:
        if len(targets) < 2:
            continue
        signature = _spring_chain_signature(joints, targets)
        entry = chain_state.get(chain_index)
        if not isinstance(entry, dict) or entry.get("signature") != signature:
            entry = {
                "signature": signature,
                "points": list(targets),
                "previous": list(targets),
            }
            chain_state[chain_index] = entry
        points = [tuple(item) for item in list(entry.get("points") or targets)]
        previous = [tuple(item) for item in list(entry.get("previous") or targets)]
        if len(points) != len(targets) or len(previous) != len(targets):
            points = list(targets)
            previous = list(targets)
        desired = _chain_gravity_targets(chain, targets)
        lengths = [
            _vec_len(_vec_sub(targets[index + 1], targets[index]))
            for index in range(len(targets) - 1)
        ]
        pin_root = bool(chain.get("pin_root")) and len(points) > 1
        if pin_root:
            points[0] = targets[0]
            previous[0] = targets[0]
        drag = _spring_drag(chain)
        stiffness = _spring_coeff(chain)
        inertia_strength = float(chain.get("inertia_strength") or 10.0)
        inertia = _weighted_scale(root_velocity, -fixed_dt * 0.018 * inertia_strength)
        next_points = list(points)
        for index in range(1 if pin_root else 0, len(points)):
            current = points[index]
            prev = previous[index]
            velocity = _weighted_scale(_vec_sub(current, prev), 1.0 - drag)
            spring = _weighted_scale(_vec_sub(desired[index], current), stiffness)
            drive = _spring_drive(chain, targets[index], index, time_value)
            candidate = _vec_add(current, _vec_add(velocity, _vec_add(spring, _vec_add(inertia, drive))))
            parent = next_points[index - 1] if index > 0 else targets[index]
            if index > 0:
                candidate = _constrain_to_parent(candidate, parent, targets[index], lengths[index - 1])
            candidate = _resolve_sphere_collisions(candidate, float(chain.get("radius") or 0.018), colliders)
            if index > 0:
                candidate = _constrain_to_parent(candidate, parent, targets[index], lengths[index - 1])
            next_points[index] = candidate
        for _ in range(2):
            if pin_root:
                next_points[0] = targets[0]
            for index in range(1, len(next_points)):
                next_points[index] = _constrain_to_parent(
                    next_points[index],
                    next_points[index - 1],
                    targets[index],
                    lengths[index - 1],
                )
        entry["previous"] = points
        entry["points"] = next_points
        for joint, point, target in zip(joints, next_points, targets):
            offsets[joint] = _vec_sub(point, target)
    state["last_time"] = time_value
    return offsets


def _physics_offset_for_joint(
    joint: str,
    offsets: Mapping[str, tuple[float, float, float]],
) -> tuple[float, float, float] | None:
    for key in _pose_key_candidates(joint):
        value = offsets.get(key)
        if value is not None:
            return value
    joint_l = str(joint or "").lower()
    for key, value in offsets.items():
        if str(key).lower() == joint_l:
            return value
    return None


def _unique_skin_joints(skin_influences: list[list[tuple[str, float]]]) -> tuple[str, ...]:
    seen: set[str] = set()
    out: list[str] = []
    for influences in skin_influences:
        for joint, _weight in influences:
            if joint and joint not in seen:
                seen.add(joint)
                out.append(joint)
    return tuple(out)


def _apply_secondary_motion(
    vertices: list[tuple[float, float, float]],
    preview: Mapping[str, Any],
    node: Mapping[str, Any],
    meta: Mapping[str, Any] | None,
    skin_influences: list[list[tuple[str, float]]] | None = None,
    pose_context: tuple[
        Mapping[str, tuple[float, float, float]],
        Mapping[str, tuple[float, float, float]],
        Mapping[str, tuple[float, float, float, float]],
    ] | None = None,
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
    if pose_context is None:
        return vertices
    rest, posed, rotations = pose_context
    physics_offsets = _spring_secondary_offsets(node, meta, prepared, rest, posed, rotations)
    if not physics_offsets:
        return vertices
    physics_offset_cache = {
        joint: _physics_offset_for_joint(joint, physics_offsets)
        for joint in _unique_skin_joints(skin_influences)
    }
    moved: list[tuple[float, float, float]] = []
    changed = False
    for index, point in enumerate(vertices):
        influences = skin_influences[index] if index < len(skin_influences) else ()
        if not influences:
            moved.append(point)
            continue
        offset = (0.0, 0.0, 0.0)
        for joint, weight in influences:
            joint_offset = physics_offset_cache.get(joint)
            if joint_offset is not None:
                offset = _vec_add(offset, _weighted_scale(joint_offset, weight))
        if any(abs(value) > 0.000001 for value in offset):
            changed = True
            moved.append(_vec_add(point, offset))
        else:
            moved.append(point)
    return moved if changed else vertices


def _secondary_joint_offsets(
    preview: Mapping[str, Any],
    node: Mapping[str, Any],
    meta: Mapping[str, Any] | None,
    skin_influences: list[list[tuple[str, float]]] | None,
    pose_context: tuple[
        Mapping[str, tuple[float, float, float]],
        Mapping[str, tuple[float, float, float]],
        Mapping[str, tuple[float, float, float, float]],
    ] | None,
) -> dict[str, tuple[float, float, float]]:
    chains = _motion_chains(node, meta)
    if not chains:
        return {}
    if skin_influences is None:
        skin_influences = _preview_skin_influences(preview)
    if not skin_influences or pose_context is None:
        return {}
    prepared = _prepared_chains(chains)
    if not prepared:
        return {}
    rest, posed, rotations = pose_context
    return _spring_secondary_offsets(node, meta, prepared, rest, posed, rotations)


def _pose_state_from_pose(
    preview: Mapping[str, Any],
    node: Mapping[str, Any],
    pose: Mapping[str, Any],
    meta: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    skin_influences = _preview_skin_influences(preview)
    if not skin_influences or not isinstance(pose, Mapping) or not bool(pose.get("ok")):
        return {}
    rest = _pose_points(pose, "rest_positions")
    posed = _pose_points(pose, "positions")
    rotations = _pose_rotations(pose)
    if not rest or not posed:
        return {}
    rotations = _derived_pose_rotations(rest, posed, rotations)
    physics_offsets = _secondary_joint_offsets(preview, node, meta, skin_influences, (rest, posed, rotations))
    unique_joints = _unique_skin_joints(skin_influences)
    joint_transform_cache = {
        joint: _joint_pose_transform(joint, rest, posed, rotations, meta)
        for joint in unique_joints
    }
    return {
        "skin_influences": skin_influences,
        "rest": rest,
        "posed": posed,
        "rotations": rotations,
        "joint_transforms": joint_transform_cache,
        "physics_offsets": physics_offsets,
        "unique_joints": unique_joints,
    }


def _resolve_pose_state(
    preview: Mapping[str, Any],
    node: Mapping[str, Any],
    meta: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    return {}


def _deform_vertices(
    vertices: list[tuple[float, float, float]],
    preview: Mapping[str, Any],
    node: Mapping[str, Any],
    meta: Mapping[str, Any] | None = None,
) -> list[tuple[float, float, float]]:
    skin_influences = _preview_skin_influences(preview)
    if not skin_influences:
        return _apply_secondary_motion(vertices, preview, node, meta, skin_influences)
    state = _resolve_pose_state(preview, node, meta)
    if not state:
        return _apply_secondary_motion(vertices, preview, node, meta, skin_influences)
    rest = state["rest"]
    posed = state["posed"]
    rotations = state["rotations"]
    joint_transform_cache = state["joint_transforms"]

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
            transform = joint_transform_cache.get(joint)
            if transform is None:
                continue
            before, after, quat = transform
            candidate = point
            if quat is not None:
                candidate = _vec_add(before, _quat_rotate_vec(quat, _vec_sub(candidate, before)))
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
    return _apply_secondary_motion(deformed, preview, node, meta, skin_influences, (rest, posed, rotations))


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
    scale = min(width * 0.84 / span_x, height * 0.86 / span_y)
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
    path = _material_texture_path(meta, material_name)
    return _average_texture_color(path) if path is not None else None


def _material_texture_path(
    meta: Mapping[str, Any] | None,
    material_name: str,
) -> Path | None:
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
        if candidate.is_file():
            return candidate
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
