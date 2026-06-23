# -*- coding: utf-8 -*-
"""Offline-safe model3d backend probe helpers.

This module intentionally does not download or initialize native render
dependencies. It only inspects the local bundle layout so plugin overlay load
can stay safe when model3d assets or AssimpNet files are absent.
"""
from __future__ import annotations

import copy
import hashlib
import json
import math
import re
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping


_MANAGED_NAMES = ("AssimpNet.dll",)
_NATIVE_NAMES = (
    "assimp.dll",
    "assimp-vc143-mt.dll",
    "assimp-vc142-mt.dll",
    "assimp-vc141-mt.dll",
    "libassimp.dll",
)
_CACHE_LIMIT = 128
_MODEL_PARSE_LIMIT = 4 * 1024 * 1024
_MESH_PREVIEW_VERTEX_LIMIT = 2048
_MESH_PREVIEW_FACE_LIMIT = 4096
_BACKEND_STATUS_CACHE: Model3DBackendStatus | None = None
_MODEL_METADATA_CACHE: dict[tuple[Any, ...], dict[str, Any]] = {}
_ACTION_METADATA_CACHE: dict[tuple[Any, ...], dict[str, Any]] = {}
_RETARGET_PLAN_CACHE: dict[tuple[Any, ...], dict[str, Any]] = {}

HUMANOID_BONES = (
    "root",
    "hips",
    "spine",
    "chest",
    "neck",
    "head",
    "left_shoulder",
    "left_arm",
    "left_forearm",
    "left_hand",
    "right_shoulder",
    "right_arm",
    "right_forearm",
    "right_hand",
    "left_leg",
    "left_knee",
    "left_foot",
    "right_leg",
    "right_knee",
    "right_foot",
)

_BONE_ALIASES: dict[str, tuple[str, ...]] = {
    "root": ("root", "armature", "scene", "origin"),
    "hips": ("hips", "hip", "pelvis", "pelvisbone", "waist", "mixamorig:hips", "bip001pelvis"),
    "spine": ("spine", "spine1", "spine01", "spine_01", "torso", "body"),
    "chest": ("chest", "upperchest", "spine2", "spine02", "spine_02", "breast"),
    "neck": ("neck", "neck1", "neck01"),
    "head": ("head", "headtop", "head_end"),
    "left_shoulder": ("leftshoulder", "lshoulder", "shoulder_l", "l_clavicle", "leftclavicle", "clavicle_l"),
    "left_arm": ("leftarm", "leftupperarm", "upperarm_l", "lupperarm", "arm_l", "l_arm"),
    "left_forearm": ("leftforearm", "leftlowerarm", "forearm_l", "lowerarm_l", "lelbow", "l_forearm"),
    "left_hand": ("lefthand", "hand_l", "lhand", "l_hand", "leftwrist", "wrist_l"),
    "right_shoulder": ("rightshoulder", "rshoulder", "shoulder_r", "r_clavicle", "rightclavicle", "clavicle_r"),
    "right_arm": ("rightarm", "rightupperarm", "upperarm_r", "rupperarm", "arm_r", "r_arm"),
    "right_forearm": ("rightforearm", "rightlowerarm", "forearm_r", "lowerarm_r", "relbow", "r_forearm"),
    "right_hand": ("righthand", "hand_r", "rhand", "r_hand", "rightwrist", "wrist_r"),
    "left_leg": ("leftupleg", "leftupperleg", "leftleg", "thigh_l", "upleg_l", "lthigh", "l_leg"),
    "left_knee": ("leftleg", "leftlowerleg", "calf_l", "leg_l", "lknee", "shin_l"),
    "left_foot": ("leftfoot", "foot_l", "lfoot", "l_foot", "leftankle", "ankle_l"),
    "right_leg": ("rightupleg", "rightupperleg", "rightleg", "thigh_r", "upleg_r", "rthigh", "r_leg"),
    "right_knee": ("rightleg", "rightlowerleg", "calf_r", "leg_r", "rknee", "shin_r"),
    "right_foot": ("rightfoot", "foot_r", "rfoot", "r_foot", "rightankle", "ankle_r"),
}

_COARSE_BONE_FALLBACKS: dict[str, tuple[str, ...]] = {
    "left_arm": ("left_arm", "left_upper_arm"),
    "right_arm": ("right_arm", "right_upper_arm"),
    "left_leg": ("left_leg", "left_upper_leg"),
    "right_leg": ("right_leg", "right_upper_leg"),
}

HUMANOID_SEGMENTS = (
    ("root", "hips"),
    ("hips", "spine"),
    ("spine", "chest"),
    ("chest", "neck"),
    ("neck", "head"),
    ("chest", "left_shoulder"),
    ("left_shoulder", "left_arm"),
    ("left_arm", "left_forearm"),
    ("left_forearm", "left_hand"),
    ("chest", "right_shoulder"),
    ("right_shoulder", "right_arm"),
    ("right_arm", "right_forearm"),
    ("right_forearm", "right_hand"),
    ("hips", "left_leg"),
    ("left_leg", "left_knee"),
    ("left_knee", "left_foot"),
    ("hips", "right_leg"),
    ("right_leg", "right_knee"),
    ("right_knee", "right_foot"),
)

_DEFAULT_REST_POSITIONS: dict[str, tuple[float, float, float]] = {
    "root": (0.0, 0.0, 0.0),
    "hips": (0.0, 0.12, 0.0),
    "spine": (0.0, 0.55, 0.0),
    "chest": (0.0, 0.88, 0.0),
    "neck": (0.0, 1.12, 0.0),
    "head": (0.0, 1.35, 0.0),
    "left_shoulder": (-0.18, 0.98, 0.0),
    "left_arm": (-0.40, 0.84, 0.0),
    "left_forearm": (-0.58, 0.60, 0.0),
    "left_hand": (-0.68, 0.38, 0.0),
    "right_shoulder": (0.18, 0.98, 0.0),
    "right_arm": (0.40, 0.84, 0.0),
    "right_forearm": (0.58, 0.60, 0.0),
    "right_hand": (0.68, 0.38, 0.0),
    "left_leg": (-0.15, -0.26, 0.0),
    "left_knee": (-0.17, -0.72, 0.0),
    "left_foot": (-0.18, -1.08, 0.08),
    "right_leg": (0.15, -0.26, 0.0),
    "right_knee": (0.17, -0.72, 0.0),
    "right_foot": (0.18, -1.08, 0.08),
}


@dataclass(frozen=True)
class Model3DBackendStatus:
    """Result of probing the bundled model3d backend files."""

    backend: str
    files_present: bool
    render_available: bool
    reason: str
    managed_files: tuple[str, ...]
    native_files: tuple[str, ...]
    search_roots: tuple[str, ...]


def _python_root() -> Path:
    return Path(__file__).resolve().parents[1]


def _candidate_roots() -> tuple[Path, ...]:
    root = _python_root()
    vendor = root / "vendor" / "model3d"
    assimpnet = vendor / "assimpnet"
    return (
        vendor,
        vendor / "native",
        assimpnet,
        assimpnet / "runtimes" / "win-x64" / "native",
        root / "render" / "model3d",
    )


def _find_named_files(roots: tuple[Path, ...], names: tuple[str, ...]) -> tuple[str, ...]:
    out: list[str] = []
    wanted = {name.lower() for name in names}
    for root in roots:
        if not root.exists() or not root.is_dir():
            continue
        try:
            for child in root.iterdir():
                if child.is_file() and child.name.lower() in wanted:
                    out.append(str(child))
        except Exception:
            continue
    return tuple(sorted(set(out)))


def probe_model3d_backend() -> Model3DBackendStatus:
    """Inspect the bundled AssimpNet/native-file layout.

    ``render_available`` remains false until a real Python/GL bridge is wired.
    Keeping this explicit lets the overlay host draw a useful diagnostic instead
    of pretending a model can render and then failing during plugin load.
    """

    roots = _candidate_roots()
    managed = _find_named_files(roots, _MANAGED_NAMES)
    native = _find_named_files(roots, _NATIVE_NAMES)
    files_present = bool(managed and native)
    if not managed and not native:
        reason = "Bundled AssimpNet and native assimp files were not found."
    elif not managed:
        reason = "Native assimp file found, but AssimpNet.dll is missing."
    elif not native:
        reason = "AssimpNet.dll found, but native assimp DLL is missing."
    else:
        reason = "AssimpNet files are present, but model3d GL rendering is not enabled in this build."
    return Model3DBackendStatus(
        backend="assimpnet",
        files_present=files_present,
        render_available=False,
        reason=reason,
        managed_files=managed,
        native_files=native,
        search_roots=tuple(str(root) for root in roots),
    )


def get_backend_status(*, force: bool = False) -> Model3DBackendStatus:
    """Return a cached backend probe result.

    The compositor may ask this every animation frame.  The local bundle layout
    rarely changes at runtime, so callers should use this cached helper unless
    they are explicitly validating packaging changes.
    """

    global _BACKEND_STATUS_CACHE
    if force or _BACKEND_STATUS_CACHE is None:
        _BACKEND_STATUS_CACHE = probe_model3d_backend()
    return _BACKEND_STATUS_CACHE


def model3d_node_key(plugin_id: Any, node: Mapping[str, Any], order: int = 0) -> str:
    """Return the compositor key for a model3d node.

    Plugin authors should provide ``node["id"]``. The order fallback keeps
    invalid specs visible as diagnostics without crashing or colliding.
    """

    pid = str(plugin_id or "").strip() or "plugin"
    node_id = str(node.get("id") or "").strip()
    if node_id:
        return f"{pid}/{node_id}"
    return f"{pid}/model3d#{max(0, int(order or 0))}"


def model3d_path(node: Mapping[str, Any]) -> str:
    model = node.get("model")
    if isinstance(model, Mapping):
        return str(model.get("path") or "").strip()
    return str(node.get("model_path") or "").strip()


def resolve_model_path(path_text: Any) -> Path:
    """Resolve a model path without assuming the process cwd.

    Root-level script plugins commonly submit paths such as
    ``plugins/script_stickwoman_csharp/assets/foo.fbx`` while the app may be
    started from either the workspace root or ``sao_auto``.  The first existing
    candidate wins; otherwise the cwd-relative path is returned for diagnostics.
    """

    raw = str(path_text or "").strip()
    path = Path(raw)
    if path.is_absolute() or not raw:
        return path
    candidates = [Path.cwd() / path]
    try:
        py_root = _python_root()
        candidates.append(py_root.parent / path)
        candidates.append(py_root.parents[1] / path)
    except Exception:
        pass
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]


def resolve_action_path(path_text: Any, node: Mapping[str, Any] | None = None) -> Path:
    """Resolve an action config or motion path relative to cwd/model dirs.

    Script plugins commonly keep model and action files side-by-side.  Relative
    action files therefore try the resolved model directory before falling back
    to the app/workspace roots.
    """

    raw = str(path_text or "").strip()
    path = Path(raw)
    if path.is_absolute() or not raw:
        return path
    candidates: list[Path] = []
    if isinstance(node, Mapping):
        model_text = model3d_path(node)
        if model_text:
            try:
                model_path = resolve_model_path(model_text)
                parent = model_path.parent
                if str(parent):
                    candidates.append(parent / path)
            except Exception:
                pass
    candidates.append(Path.cwd() / path)
    try:
        py_root = _python_root()
        candidates.append(py_root.parent / path)
        candidates.append(py_root.parents[1] / path)
    except Exception:
        pass
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]


def _file_signature(path: Path) -> tuple[bool, int, int]:
    try:
        stat = path.stat()
        return True, int(stat.st_size), int(getattr(stat, "st_mtime_ns", int(stat.st_mtime * 1_000_000_000)))
    except Exception:
        return False, 0, 0


def _hash_text(text: str) -> str:
    return hashlib.sha1(text.encode("utf-8", errors="replace")).hexdigest()


def _hash_mapping(value: Mapping[str, Any]) -> str:
    try:
        encoded = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), default=str)
    except Exception:
        encoded = repr(dict(value))
    return _hash_text(encoded)


def _cache_put(cache: dict[tuple[Any, ...], dict[str, Any]], key: tuple[Any, ...], value: dict[str, Any]) -> None:
    if len(cache) >= _CACHE_LIMIT:
        cache.clear()
    cache[key] = value


def _copy_metadata(value: dict[str, Any]) -> dict[str, Any]:
    out = dict(value)
    for key in (
        "data",
        "selected",
        "model",
        "action",
        "sidecar",
        "retarget",
        "mesh",
        "rest_positions",
        "clip_keyframes",
    ):
        item = out.get(key)
        if isinstance(item, Mapping):
            out[key] = copy.deepcopy(dict(item))
    for key in ("bone_names", "clips", "warnings", "unresolved", "nodes", "materials", "metadata_errors"):
        item = out.get(key)
        if isinstance(item, tuple):
            out[key] = list(item)
    errors = out.get("errors")
    if isinstance(errors, tuple):
        out["errors"] = list(errors)
    return out


def _copy_retarget_plan(value: dict[str, Any]) -> dict[str, Any]:
    out = dict(value)
    for key in ("bone_map", "sources"):
        item = out.get(key)
        if isinstance(item, Mapping):
            out[key] = dict(item)
    return out


def _sidecar_candidates(model_path: Path) -> tuple[Path, ...]:
    if not str(model_path):
        return ()
    candidates: list[Path] = []
    try:
        if model_path.suffix:
            candidates.append(model_path.with_suffix(model_path.suffix + ".model3d.json"))
            candidates.append(model_path.with_suffix(model_path.suffix + ".skeleton.json"))
        candidates.append(model_path.with_suffix(".model3d.json"))
        candidates.append(model_path.with_suffix(".skeleton.json"))
        candidates.append(model_path.parent / "model3d.json")
    except Exception:
        return ()
    out: list[Path] = []
    seen: set[str] = set()
    for candidate in candidates:
        key = str(candidate)
        if key and key not in seen:
            seen.add(key)
            out.append(candidate)
    return tuple(out)


def _first_existing_sidecar(model_path: Path) -> tuple[Path, bool, int, int]:
    for candidate in _sidecar_candidates(model_path):
        exists, size, mtime_ns = _file_signature(candidate)
        if exists:
            return candidate, exists, size, mtime_ns
    candidates = _sidecar_candidates(model_path)
    candidate = candidates[0] if candidates else Path("")
    return candidate, False, 0, 0


def _extract_names(value: Any) -> list[str]:
    out: list[str] = []

    def _visit(item: Any) -> None:
        if isinstance(item, str):
            text = item.strip()
            if text:
                out.append(text)
            return
        if isinstance(item, Mapping):
            for key in ("name", "bone", "id"):
                text = str(item.get(key) or "").strip()
                if text:
                    out.append(text)
                    return
            nested = item.get("children")
            if isinstance(nested, (list, tuple)):
                for child in nested:
                    _visit(child)
            return
        if isinstance(item, (list, tuple)):
            for child in item:
                _visit(child)

    _visit(value)
    seen: set[str] = set()
    unique: list[str] = []
    for name in out:
        if name not in seen:
            seen.add(name)
            unique.append(name)
    return unique


def _point3(value: Any) -> tuple[float, float, float] | None:
    if not isinstance(value, (list, tuple)) or len(value) < 2:
        return None
    try:
        x = float(value[0])
        y = float(value[1])
        z = float(value[2]) if len(value) >= 3 else 0.0
        if x != x or y != y or z != z:
            raise ValueError("nan")
        return x, y, z
    except Exception:
        return None


def _quat4(value: Any) -> tuple[float, float, float, float] | None:
    if not isinstance(value, (list, tuple)) or len(value) < 4:
        return None
    try:
        x = float(value[0])
        y = float(value[1])
        z = float(value[2])
        w = float(value[3])
        if not all(math.isfinite(v) for v in (x, y, z, w)):
            raise ValueError("non-finite quaternion")
        return x, y, z, w
    except Exception:
        return None


def _extract_rest_positions(value: Any) -> dict[str, tuple[float, float, float]]:
    if not isinstance(value, Mapping):
        return {}
    out: dict[str, tuple[float, float, float]] = {}
    for key, raw in value.items():
        point = _point3(raw)
        if point is not None:
            out[str(key)] = point
    return out


def _extract_sidecar_metadata(value: Any) -> dict[str, Any]:
    if not isinstance(value, Mapping):
        return {}
    src = dict(value)
    skeleton = src.get("skeleton") if isinstance(src.get("skeleton"), Mapping) else {}
    bones = (
        _extract_names(src.get("bones"))
        or _extract_names(src.get("bone_names"))
        or _extract_names((skeleton or {}).get("bones"))
        or _extract_names((skeleton or {}).get("bone_names"))
        or _extract_names(src.get("nodes"))
    )
    clips = (
        _extract_names(src.get("clips"))
        or _extract_names(src.get("animations"))
        or _extract_names(src.get("actions"))
    )
    out: dict[str, Any] = {}
    if bones:
        out["bone_names"] = tuple(bones)
    if clips:
        out["clips"] = tuple(clips)
    if skeleton:
        bone_map = skeleton.get("bone_map")
        if isinstance(bone_map, Mapping):
            out["bone_map"] = {str(k): str(v) for k, v in bone_map.items()}
    rest_positions = (
        _extract_rest_positions(src.get("rest_positions"))
        or _extract_rest_positions((skeleton or {}).get("rest_positions"))
        or _extract_rest_positions((skeleton or {}).get("rest_offsets"))
    )
    if rest_positions:
        out["rest_positions"] = dict(rest_positions)
    for key in ("profile", "up_axis", "unit_scale", "rest_pose"):
        if key in src:
            out[key] = _json_safe_scalar(src.get(key))
        elif skeleton and key in skeleton:
            out[key] = _json_safe_scalar(skeleton.get(key))
    return out


def _json_safe_scalar(value: Any) -> Any:
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    return str(value)


def _unique_strings(values: Any) -> tuple[str, ...]:
    out: list[str] = []
    seen: set[str] = set()
    if isinstance(values, (list, tuple, set)):
        items = values
    else:
        items = ()
    for item in items:
        text = str(item or "").strip()
        if text and text not in seen:
            seen.add(text)
            out.append(text)
    return tuple(out)


def _merge_unique(*groups: Any) -> tuple[str, ...]:
    out: list[str] = []
    seen: set[str] = set()
    for group in groups:
        for item in _unique_strings(group):
            if item not in seen:
                seen.add(item)
                out.append(item)
    return tuple(out)


def _bbox(points: list[tuple[float, float, float]]) -> dict[str, list[float]]:
    if not points:
        return {}
    mins = [min(point[idx] for point in points) for idx in range(3)]
    maxs = [max(point[idx] for point in points) for idx in range(3)]
    center = [(mins[idx] + maxs[idx]) / 2.0 for idx in range(3)]
    extent = [maxs[idx] - mins[idx] for idx in range(3)]
    return {
        "min": [float(x) for x in mins],
        "max": [float(x) for x in maxs],
        "center": [float(x) for x in center],
        "extent": [float(x) for x in extent],
    }


def _preview_vertices(points: list[tuple[float, float, float]]) -> list[list[float]]:
    out: list[list[float]] = []
    for x, y, z in points:
        if not (math.isfinite(x) and math.isfinite(y) and math.isfinite(z)):
            continue
        out.append([float(x), float(y), float(z)])
        if len(out) >= _MESH_PREVIEW_VERTEX_LIMIT:
            break
    return out


def _bbox_preview(box: Mapping[str, Any]) -> dict[str, Any]:
    mins = box.get("min") if isinstance(box, Mapping) else None
    maxs = box.get("max") if isinstance(box, Mapping) else None
    if not isinstance(mins, list) or not isinstance(maxs, list) or len(mins) < 3 or len(maxs) < 3:
        return {}
    try:
        min_x, min_y, min_z = float(mins[0]), float(mins[1]), float(mins[2])
        max_x, max_y, max_z = float(maxs[0]), float(maxs[1]), float(maxs[2])
        if not all(math.isfinite(v) for v in (min_x, min_y, min_z, max_x, max_y, max_z)):
            return {}
    except Exception:
        return {}
    vertices = [
        [min_x, min_y, min_z], [max_x, min_y, min_z],
        [max_x, max_y, min_z], [min_x, max_y, min_z],
        [min_x, min_y, max_z], [max_x, min_y, max_z],
        [max_x, max_y, max_z], [min_x, max_y, max_z],
    ]
    faces = [
        [0, 1, 2, 3], [4, 5, 6, 7], [0, 1, 5, 4],
        [2, 3, 7, 6], [1, 2, 6, 5], [0, 3, 7, 4],
    ]
    return {"vertices": vertices, "faces": faces, "source": "bbox"}


def _obj_index(token: str, vertex_count: int) -> int | None:
    head = str(token or "").split("/", 1)[0].strip()
    if not head:
        return None
    try:
        raw = int(head)
    except Exception:
        return None
    index = raw - 1 if raw > 0 else vertex_count + raw
    return index if 0 <= index < vertex_count else None


def _float_triplet(values: list[str]) -> tuple[float, float, float] | None:
    if len(values) < 3:
        return None
    try:
        return float(values[0]), float(values[1]), float(values[2])
    except Exception:
        return None


def _parse_obj_metadata(path: Path) -> dict[str, Any]:
    points: list[tuple[float, float, float]] = []
    faces: list[list[int]] = []
    face_count = 0
    nodes: list[str] = []
    materials: list[str] = []
    errors: list[str] = []
    consumed = 0
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fp:
            for raw in fp:
                consumed += len(raw.encode("utf-8", errors="replace"))
                if consumed > _MODEL_PARSE_LIMIT:
                    errors.append("model metadata parse limit reached")
                    break
                line = raw.strip()
                if not line or line.startswith("#"):
                    continue
                if line.startswith("v "):
                    point = _float_triplet(line.split()[1:4])
                    if point is not None:
                        points.append(point)
                elif line.startswith("f "):
                    parts = line.split()[1:]
                    if len(parts) >= 3:
                        face_count += 1
                        if len(faces) < _MESH_PREVIEW_FACE_LIMIT:
                            face = [idx for idx in (_obj_index(part, len(points)) for part in parts) if idx is not None]
                            if len(face) >= 3 and all(idx < _MESH_PREVIEW_VERTEX_LIMIT for idx in face):
                                faces.append(face[:8])
                elif line.startswith(("o ", "g ")):
                    name = line[2:].strip()
                    if name:
                        nodes.append(name)
                elif line.startswith("usemtl "):
                    name = line[7:].strip()
                    if name:
                        materials.append(name)
    except Exception as exc:
        errors.append(str(exc))
    box = _bbox(points)
    return {
        "mesh": {
            "source": "obj",
            "vertex_count": len(points),
            "face_count": face_count,
            "bbox": box,
            "preview": {
                "vertices": _preview_vertices(points),
                "faces": faces,
                "source": "obj",
                "truncated": len(points) > _MESH_PREVIEW_VERTEX_LIMIT or face_count > _MESH_PREVIEW_FACE_LIMIT,
            },
        },
        "nodes": _unique_strings(nodes),
        "materials": _unique_strings(materials),
        "errors": tuple(errors),
    }


_NUMBER_RE = re.compile(r"[-+]?(?:\d+\.\d+|\d+|\.\d+)(?:[eE][-+]?\d+)?")


def _numbers(text: str, *, limit: int = 200000) -> list[float]:
    out: list[float] = []
    for match in _NUMBER_RE.finditer(text):
        try:
            out.append(float(match.group(0)))
        except Exception:
            continue
        if len(out) >= limit:
            break
    return out


def _parse_ascii_fbx_metadata(path: Path) -> dict[str, Any]:
    errors: list[str] = []
    try:
        raw = path.read_bytes()[:_MODEL_PARSE_LIMIT + 1]
    except Exception as exc:
        return {"mesh": {}, "nodes": (), "bone_names": (), "clips": (), "errors": (str(exc),)}
    if len(raw) > _MODEL_PARSE_LIMIT:
        errors.append("model metadata parse limit reached")
        raw = raw[:_MODEL_PARSE_LIMIT]
    text = raw.decode("utf-8", errors="replace")
    if text.startswith("Kaydara FBX Binary"):
        return {
            "mesh": {"source": "fbx_binary", "vertex_count": 0, "face_count": 0, "bbox": {}},
            "nodes": (),
            "bone_names": (),
            "clips": (),
            "errors": ("binary FBX metadata requires native backend",),
        }

    points: list[tuple[float, float, float]] = []
    vertices_match = re.search(r"Vertices:\s*\*\d+\s*{\s*a:\s*([^}]*)}", text, re.S)
    if vertices_match:
        values = _numbers(vertices_match.group(1))
        for idx in range(0, len(values) - 2, 3):
            points.append((values[idx], values[idx + 1], values[idx + 2]))

    face_count = 0
    faces: list[list[int]] = []
    pvi_match = re.search(r"PolygonVertexIndex:\s*\*\d+\s*{\s*a:\s*([^}]*)}", text, re.S)
    if pvi_match:
        current: list[int] = []
        for value in _NUMBER_RE.finditer(pvi_match.group(1)):
            try:
                raw_index = int(float(value.group(0)))
                end_face = raw_index < 0
                index = (-raw_index - 1) if end_face else raw_index
                if 0 <= index < len(points) and (not end_face or not current or current[-1] != index):
                    current.append(index)
                if end_face:
                    face_count += 1
                    if (
                        len(current) >= 3
                        and len(faces) < _MESH_PREVIEW_FACE_LIMIT
                        and all(idx < _MESH_PREVIEW_VERTEX_LIMIT for idx in current)
                    ):
                        faces.append(current[:8])
                    current = []
            except Exception:
                continue

    nodes: list[str] = []
    bones: list[str] = []
    for match in re.finditer(r'Model:\s*[^"\n]*"([^"]+)"\s*,\s*"([^"]+)"', text):
        raw_name = str(match.group(1) or "")
        kind = str(match.group(2) or "")
        name = raw_name.split("::", 1)[-1].strip()
        if not name:
            continue
        nodes.append(name)
        if "limb" in kind.lower() or "bone" in kind.lower():
            bones.append(name)

    clips: list[str] = []
    for match in re.finditer(r'AnimationStack:\s*[^"\n]*"([^"]+)"', text):
        raw_name = str(match.group(1) or "")
        name = raw_name.split("::", 1)[-1].strip()
        if name:
            clips.append(name)
    for match in re.finditer(r'Current:\s*"([^"]+)"', text):
        name = str(match.group(1) or "").strip()
        if name:
            clips.append(name)

    box = _bbox(points)
    return {
        "mesh": {
            "source": "fbx_ascii",
            "vertex_count": len(points),
            "face_count": face_count,
            "bbox": box,
            "preview": {
                "vertices": _preview_vertices(points),
                "faces": faces,
                "source": "fbx_ascii",
                "truncated": len(points) > _MESH_PREVIEW_VERTEX_LIMIT or face_count > _MESH_PREVIEW_FACE_LIMIT,
            },
        },
        "nodes": _unique_strings(nodes),
        "bone_names": _unique_strings(bones),
        "clips": _unique_strings(clips),
        "errors": tuple(errors),
    }


_GLB_MAGIC = b"glTF"
_GLB_JSON_CHUNK = 0x4E4F534A
_GLB_BIN_CHUNK = 0x004E4942
_GLTF_COMPONENTS: dict[int, tuple[str, int]] = {
    5120: ("b", 1),
    5121: ("B", 1),
    5122: ("h", 2),
    5123: ("H", 2),
    5125: ("I", 4),
    5126: ("f", 4),
}
_GLTF_TYPE_COUNTS = {
    "SCALAR": 1,
    "VEC2": 2,
    "VEC3": 3,
    "VEC4": 4,
}


def _int_at(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        return default


def _json_index(items: Any, index: Any) -> Mapping[str, Any] | None:
    if not isinstance(items, list):
        return None
    idx = _int_at(index, -1)
    if 0 <= idx < len(items) and isinstance(items[idx], Mapping):
        return items[idx]
    return None


def _read_glb(path: Path) -> tuple[dict[str, Any] | None, bytes, list[str]]:
    errors: list[str] = []
    try:
        size = path.stat().st_size
        if size > _MODEL_PARSE_LIMIT:
            errors.append("model metadata parse limit reached")
            return None, b"", errors
        raw = path.read_bytes()
    except Exception as exc:
        return None, b"", [str(exc)]
    if len(raw) < 20 or raw[:4] != _GLB_MAGIC:
        return None, b"", ["invalid glb header"]
    try:
        version, declared_len = struct.unpack_from("<II", raw, 4)
    except Exception as exc:
        return None, b"", [str(exc)]
    if version != 2:
        errors.append(f"unsupported glb version {version}")
        return None, b"", errors
    if declared_len > len(raw):
        errors.append("truncated glb file")
        return None, b"", errors

    pos = 12
    json_data: dict[str, Any] | None = None
    bin_chunk = b""
    while pos + 8 <= declared_len:
        try:
            chunk_len, chunk_type = struct.unpack_from("<II", raw, pos)
        except Exception as exc:
            errors.append(str(exc))
            break
        pos += 8
        end = pos + chunk_len
        if end > declared_len:
            errors.append("truncated glb chunk")
            break
        chunk = raw[pos:end]
        pos = end
        if chunk_type == _GLB_JSON_CHUNK:
            try:
                loaded = json.loads(chunk.decode("utf-8").rstrip(" \t\r\n\0"))
                if isinstance(loaded, Mapping):
                    json_data = dict(loaded)
                else:
                    errors.append("glb JSON chunk root is not an object")
            except Exception as exc:
                errors.append(str(exc))
        elif chunk_type == _GLB_BIN_CHUNK and not bin_chunk:
            bin_chunk = bytes(chunk)
    if json_data is None:
        errors.append("glb JSON chunk missing")
    return json_data, bin_chunk, errors


def _accessor_byte_layout(
    accessor: Mapping[str, Any],
    buffer_views: Any,
) -> tuple[int, int, int, int, str] | None:
    view = _json_index(buffer_views, accessor.get("bufferView"))
    if view is None:
        return None
    component_type = _int_at(accessor.get("componentType"), -1)
    comp = _GLTF_COMPONENTS.get(component_type)
    if comp is None:
        return None
    fmt, comp_size = comp
    type_name = str(accessor.get("type") or "SCALAR").upper()
    comp_count = _GLTF_TYPE_COUNTS.get(type_name)
    if comp_count is None:
        return None
    base = _int_at(view.get("byteOffset")) + _int_at(accessor.get("byteOffset"))
    stride = _int_at(view.get("byteStride"), comp_size * comp_count)
    if stride < comp_size * comp_count:
        return None
    return base, stride, comp_count, comp_size, fmt


def _read_accessor_values(
    accessor: Mapping[str, Any],
    buffer_views: Any,
    binary: bytes,
    *,
    limit: int,
) -> tuple[list[tuple[float, ...]], int, str]:
    layout = _accessor_byte_layout(accessor, buffer_views)
    if layout is None:
        return [], _int_at(accessor.get("count")), "unsupported accessor layout"
    base, stride, comp_count, comp_size, fmt = layout
    count = max(0, _int_at(accessor.get("count")))
    read_count = min(count, max(0, limit))
    row_fmt = "<" + (fmt * comp_count)
    row_size = comp_size * comp_count
    rows: list[tuple[float, ...]] = []
    for idx in range(read_count):
        offset = base + idx * stride
        if offset < 0 or offset + row_size > len(binary):
            return rows, count, "accessor exceeds binary chunk"
        try:
            raw = struct.unpack_from(row_fmt, binary, offset)
        except Exception as exc:
            return rows, count, str(exc)
        rows.append(tuple(float(v) for v in raw))
    return rows, count, ""


def _accessor_minmax_points(accessor: Mapping[str, Any]) -> list[tuple[float, float, float]]:
    mins = accessor.get("min")
    maxs = accessor.get("max")
    if not isinstance(mins, list) or not isinstance(maxs, list) or len(mins) < 3 or len(maxs) < 3:
        return []
    try:
        return [
            (float(mins[0]), float(mins[1]), float(mins[2])),
            (float(maxs[0]), float(maxs[1]), float(maxs[2])),
        ]
    except Exception:
        return []


def _node_name(nodes: Any, index: Any) -> str:
    node = _json_index(nodes, index)
    if node is None:
        return ""
    text = str(node.get("name") or "").strip()
    if text:
        return text
    try:
        return f"node_{int(index)}"
    except Exception:
        return ""


def _node_translation(nodes: Any, index: int) -> tuple[float, float, float]:
    node = _json_index(nodes, index)
    if node is None:
        return (0.0, 0.0, 0.0)
    point = _point3(node.get("translation"))
    return point or (0.0, 0.0, 0.0)


def _mat4_identity() -> tuple[float, ...]:
    return (
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0,
    )


def _mat4_from_json(value: Any) -> tuple[float, ...] | None:
    if not isinstance(value, (list, tuple)) or len(value) < 16:
        return None
    try:
        matrix = tuple(float(value[index]) for index in range(16))
    except Exception:
        return None
    if not all(math.isfinite(item) for item in matrix):
        return None
    return matrix


def _mat4_mul(a: tuple[float, ...], b: tuple[float, ...]) -> tuple[float, ...]:
    out: list[float] = []
    for col in range(4):
        for row in range(4):
            value = 0.0
            for k in range(4):
                value += a[k * 4 + row] * b[col * 4 + k]
            out.append(value)
    return tuple(out)


def _mat4_transform_point(
    m: tuple[float, ...],
    point: tuple[float, float, float],
) -> tuple[float, float, float]:
    x, y, z = point
    return (
        m[0] * x + m[4] * y + m[8] * z + m[12],
        m[1] * x + m[5] * y + m[9] * z + m[13],
        m[2] * x + m[6] * y + m[10] * z + m[14],
    )


def _mat4_from_translation(value: tuple[float, float, float]) -> tuple[float, ...]:
    x, y, z = value
    return (
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        x, y, z, 1.0,
    )


def _mat4_from_scale(value: tuple[float, float, float]) -> tuple[float, ...]:
    x, y, z = value
    return (
        x, 0.0, 0.0, 0.0,
        0.0, y, 0.0, 0.0,
        0.0, 0.0, z, 0.0,
        0.0, 0.0, 0.0, 1.0,
    )


def _mat4_from_quat(value: tuple[float, float, float, float]) -> tuple[float, ...]:
    x, y, z, w = _quat_normalize(value)
    xx = x * x
    yy = y * y
    zz = z * z
    xy = x * y
    xz = x * z
    yz = y * z
    wx = w * x
    wy = w * y
    wz = w * z
    return (
        1.0 - 2.0 * (yy + zz), 2.0 * (xy + wz), 2.0 * (xz - wy), 0.0,
        2.0 * (xy - wz), 1.0 - 2.0 * (xx + zz), 2.0 * (yz + wx), 0.0,
        2.0 * (xz + wy), 2.0 * (yz - wx), 1.0 - 2.0 * (xx + yy), 0.0,
        0.0, 0.0, 0.0, 1.0,
    )


def _node_local_matrix(nodes: Any, index: int) -> tuple[float, ...]:
    node = _json_index(nodes, index)
    if node is None:
        return _mat4_identity()
    matrix = _mat4_from_json(node.get("matrix"))
    if matrix is not None:
        return matrix
    translation = _node_translation(nodes, index)
    rotation = _quat4(node.get("rotation")) or (0.0, 0.0, 0.0, 1.0)
    scale = _point3(node.get("scale")) or (1.0, 1.0, 1.0)
    return _mat4_mul(
        _mat4_mul(_mat4_from_translation(translation), _mat4_from_quat(rotation)),
        _mat4_from_scale(scale),
    )


def _glb_node_rest_positions(data: Mapping[str, Any]) -> dict[str, tuple[float, float, float]]:
    nodes = data.get("nodes")
    if not isinstance(nodes, list):
        return {}
    parent: dict[int, int] = {}
    for index, node in enumerate(nodes):
        if not isinstance(node, Mapping):
            continue
        children = node.get("children")
        if not isinstance(children, list):
            continue
        for raw_child in children:
            child = _int_at(raw_child, -1)
            if 0 <= child < len(nodes) and child not in parent:
                parent[child] = index

    joint_indices: set[int] = set()
    skins = data.get("skins")
    if isinstance(skins, list):
        for skin in skins:
            if not isinstance(skin, Mapping):
                continue
            joints = skin.get("joints")
            if isinstance(joints, list):
                for raw_joint in joints:
                    joint = _int_at(raw_joint, -1)
                    if 0 <= joint < len(nodes):
                        joint_indices.add(joint)
    if not joint_indices:
        joint_indices = {
            index for index in range(len(nodes))
            if _canonical_from_token(_node_name(nodes, index))
        }

    world_cache: dict[int, tuple[float, ...]] = {}

    def world(index: int, seen: set[int] | None = None) -> tuple[float, ...]:
        if index in world_cache:
            return world_cache[index]
        if seen is None:
            seen = set()
        if index in seen:
            return _node_local_matrix(nodes, index)
        seen.add(index)
        local = _node_local_matrix(nodes, index)
        parent_index = parent.get(index)
        if parent_index is None:
            result = local
        else:
            result = _mat4_mul(world(parent_index, seen), local)
        world_cache[index] = result
        return result

    out: dict[str, tuple[float, float, float]] = {}
    for index in sorted(joint_indices):
        name = _node_name(nodes, index)
        if not name or not _canonical_from_token(name):
            continue
        out[name] = _mat4_transform_point(world(index), (0.0, 0.0, 0.0))
    return out


def _glb_animation_keyframes(
    data: Mapping[str, Any],
    buffer_views: Any,
    binary: bytes,
    errors: list[str],
) -> dict[str, dict[str, Any]]:
    animations = data.get("animations")
    accessors = data.get("accessors")
    nodes = data.get("nodes")
    if not isinstance(animations, list) or not isinstance(accessors, list):
        return {}
    clips: dict[str, dict[str, Any]] = {}
    for anim_index, animation in enumerate(animations):
        if not isinstance(animation, Mapping):
            continue
        name = str(animation.get("name") or f"animation_{anim_index}").strip() or f"animation_{anim_index}"
        samplers = animation.get("samplers")
        channels = animation.get("channels")
        if not isinstance(samplers, list) or not isinstance(channels, list):
            continue
        frames_by_time: dict[float, dict[str, dict[str, tuple[float, ...]]]] = {}
        sample_count = 0
        for channel in channels:
            if not isinstance(channel, Mapping):
                continue
            target = channel.get("target") if isinstance(channel.get("target"), Mapping) else {}
            target_path = str((target or {}).get("path") or "").lower()
            if target_path not in {"translation", "rotation"}:
                continue
            canonical = _canonical_from_token(_node_name(nodes, (target or {}).get("node")))
            if not canonical:
                continue
            sampler = _json_index(samplers, channel.get("sampler"))
            if sampler is None:
                continue
            time_accessor = _json_index(accessors, sampler.get("input"))
            value_accessor = _json_index(accessors, sampler.get("output"))
            if time_accessor is None or value_accessor is None:
                continue
            times, total_times, time_err = _read_accessor_values(
                time_accessor, buffer_views, binary, limit=_MESH_PREVIEW_FACE_LIMIT)
            values, total_values, value_err = _read_accessor_values(
                value_accessor, buffer_views, binary, limit=_MESH_PREVIEW_FACE_LIMIT)
            if time_err:
                errors.append(f"{name}: {time_err}")
            if value_err:
                errors.append(f"{name}: {value_err}")
            total = min(len(times), len(values))
            if total <= 0:
                continue
            sample_count += min(total_times, total_values)
            for idx in range(total):
                row = values[idx]
                try:
                    t = float(times[idx][0])
                except Exception:
                    t = float(idx)
                if not math.isfinite(t):
                    continue
                if target_path == "translation":
                    if len(row) < 3 or not all(math.isfinite(v) for v in row[:3]):
                        continue
                    frame = frames_by_time.setdefault(t, {"offsets": {}, "rotations": {}})
                    frame["offsets"][canonical] = (float(row[0]), float(row[1]), float(row[2]))
                elif target_path == "rotation":
                    quat = _quat4(row)
                    if quat is not None:
                        frame = frames_by_time.setdefault(t, {"offsets": {}, "rotations": {}})
                        frame["rotations"][canonical] = quat
        if frames_by_time:
            frames: list[dict[str, Any]] = []
            for t, groups in sorted(frames_by_time.items()):
                frame: dict[str, Any] = {"time": t}
                offsets = groups.get("offsets") if isinstance(groups.get("offsets"), Mapping) else {}
                rotations = groups.get("rotations") if isinstance(groups.get("rotations"), Mapping) else {}
                if offsets:
                    frame["bone_offsets"] = {bone: list(offset) for bone, offset in sorted(offsets.items())}
                if rotations:
                    frame["bone_rotations"] = {bone: list(rotation) for bone, rotation in sorted(rotations.items())}
                frames.append(frame)
            clips[name] = {
                "source": "glb",
                "duration": max(frames_by_time),
                "loop": True,
                "keyframes": frames,
                "sample_count": sample_count,
            }
    return clips


def _parse_glb_metadata(path: Path) -> dict[str, Any]:
    data, binary, errors = _read_glb(path)
    empty = {"mesh": {"source": "glb", "vertex_count": 0, "face_count": 0, "bbox": {}},
             "nodes": (), "materials": (), "errors": tuple(errors)}
    if not isinstance(data, Mapping):
        return empty
    accessors = data.get("accessors")
    buffer_views = data.get("bufferViews")
    meshes = data.get("meshes")
    nodes = _extract_names(data.get("nodes"))
    materials = _extract_names(data.get("materials"))
    if not isinstance(accessors, list) or not isinstance(buffer_views, list) or not isinstance(meshes, list):
        errors.append("glb mesh metadata is incomplete")
        return {"mesh": {"source": "glb", "vertex_count": 0, "face_count": 0, "bbox": {}},
                "nodes": _unique_strings(nodes), "materials": _unique_strings(materials),
                "errors": tuple(errors)}

    clip_keyframes = _glb_animation_keyframes(data, buffer_views, binary, errors)
    rest_positions = _glb_node_rest_positions(data)
    bone_names = [name for name in nodes if _canonical_from_token(name)]
    vertices: list[list[float]] = []
    faces: list[list[int]] = []
    bbox_points: list[tuple[float, float, float]] = []
    vertex_count = 0
    face_count = 0
    for mesh in meshes:
        if not isinstance(mesh, Mapping):
            continue
        primitives = mesh.get("primitives")
        if not isinstance(primitives, list):
            continue
        for primitive in primitives:
            if not isinstance(primitive, Mapping):
                continue
            attrs = primitive.get("attributes")
            if not isinstance(attrs, Mapping) or "POSITION" not in attrs:
                continue
            pos_accessor = _json_index(accessors, attrs.get("POSITION"))
            if pos_accessor is None:
                continue
            remaining_vertices = max(0, _MESH_PREVIEW_VERTEX_LIMIT - len(vertices))
            rows, total_positions, err = _read_accessor_values(
                pos_accessor, buffer_views, binary, limit=remaining_vertices)
            if err:
                errors.append(err)
            vertex_count += total_positions
            bbox_points.extend(_accessor_minmax_points(pos_accessor))
            points: list[tuple[float, float, float]] = []
            for row in rows:
                if len(row) >= 3 and all(math.isfinite(v) for v in row[:3]):
                    point = (float(row[0]), float(row[1]), float(row[2]))
                    points.append(point)
                    bbox_points.append(point)
            base_index = len(vertices)
            vertices.extend([list(point) for point in points])

            mode = _int_at(primitive.get("mode"), 4)
            if mode != 4:
                errors.append(f"unsupported glb primitive mode {mode}")
                continue
            index_accessor = _json_index(accessors, primitive.get("indices")) if "indices" in primitive else None
            if index_accessor is not None:
                remaining_indices = max(0, (_MESH_PREVIEW_FACE_LIMIT - len(faces)) * 3)
                idx_rows, total_indices, idx_err = _read_accessor_values(
                    index_accessor, buffer_views, binary, limit=remaining_indices)
                if idx_err:
                    errors.append(idx_err)
                face_count += total_indices // 3
                flat_indices = [int(row[0]) for row in idx_rows if row]
            else:
                total_indices = total_positions
                face_count += total_indices // 3
                flat_indices = list(range(min(len(points), (_MESH_PREVIEW_FACE_LIMIT - len(faces)) * 3)))
            for idx in range(0, len(flat_indices) - 2, 3):
                tri = flat_indices[idx:idx + 3]
                if all(0 <= item < len(points) for item in tri) and len(faces) < _MESH_PREVIEW_FACE_LIMIT:
                    faces.append([base_index + item for item in tri])
    box = _bbox(bbox_points)
    preview = {
        "vertices": vertices,
        "faces": faces,
        "source": "glb",
        "truncated": vertex_count > _MESH_PREVIEW_VERTEX_LIMIT or face_count > _MESH_PREVIEW_FACE_LIMIT,
    }
    if not vertices or not faces:
        preview = _bbox_preview(box)
        if preview:
            preview["source"] = "glb_bbox"
    return {
        "mesh": {
            "source": "glb",
            "mesh_count": len(meshes),
            "vertex_count": vertex_count,
            "face_count": face_count,
            "bbox": box,
            "preview": preview,
        },
        "nodes": _unique_strings(nodes),
        "materials": _unique_strings(materials),
        "bone_names": _merge_unique(bone_names, rest_positions.keys()),
        "clips": _merge_unique(_extract_names(data.get("animations")), clip_keyframes.keys()),
        "clip_keyframes": copy.deepcopy(clip_keyframes),
        "rest_positions": dict(rest_positions),
        "rest_positions_source": "glb_nodes" if rest_positions else "",
        "errors": tuple(_unique_strings(errors)),
    }


def _parse_gltf_metadata(path: Path) -> dict[str, Any]:
    errors: list[str] = []
    try:
        if path.stat().st_size > _MODEL_PARSE_LIMIT:
            errors.append("model metadata parse limit reached")
            return {"mesh": {"source": "gltf", "vertex_count": 0, "face_count": 0, "bbox": {}},
                    "nodes": (), "materials": (), "errors": tuple(errors)}
        with open(path, "r", encoding="utf-8") as fp:
            data = json.load(fp)
    except Exception as exc:
        return {"mesh": {"source": "gltf", "vertex_count": 0, "face_count": 0, "bbox": {}},
                "nodes": (), "materials": (), "errors": (str(exc),)}
    if not isinstance(data, Mapping):
        return {"mesh": {"source": "gltf", "vertex_count": 0, "face_count": 0, "bbox": {}},
                "nodes": (), "materials": (), "errors": ("gltf root is not an object",)}
    nodes = _extract_names(data.get("nodes"))
    materials = _extract_names(data.get("materials"))
    bbox_points: list[tuple[float, float, float]] = []
    accessors = data.get("accessors")
    if isinstance(accessors, list):
        for accessor in accessors:
            if not isinstance(accessor, Mapping):
                continue
            mins = accessor.get("min")
            maxs = accessor.get("max")
            if isinstance(mins, list) and isinstance(maxs, list) and len(mins) >= 3 and len(maxs) >= 3:
                try:
                    bbox_points.append((float(mins[0]), float(mins[1]), float(mins[2])))
                    bbox_points.append((float(maxs[0]), float(maxs[1]), float(maxs[2])))
                except Exception:
                    pass
    mesh_count = len(data.get("meshes") or []) if isinstance(data.get("meshes"), list) else 0
    box = _bbox(bbox_points)
    return {
        "mesh": {
            "source": "gltf",
            "mesh_count": mesh_count,
            "vertex_count": 0,
            "face_count": 0,
            "bbox": box,
            "preview": _bbox_preview(box),
        },
        "nodes": _unique_strings(nodes),
        "materials": _unique_strings(materials),
        "errors": tuple(errors),
    }


def _read_model_file_metadata(path: Path, fmt: str, exists: bool) -> dict[str, Any]:
    if not exists or not str(path):
        return {}
    suffix = path.suffix.lower()
    chosen = fmt if fmt and fmt != "auto" else suffix.lstrip(".")
    try:
        if chosen == "obj" or suffix == ".obj":
            return _parse_obj_metadata(path)
        if chosen == "fbx" or suffix == ".fbx":
            return _parse_ascii_fbx_metadata(path)
        if chosen == "gltf" or suffix == ".gltf":
            return _parse_gltf_metadata(path)
        if chosen == "glb" or suffix == ".glb":
            return _parse_glb_metadata(path)
    except Exception as exc:
        return {"mesh": {}, "nodes": (), "materials": (), "errors": (str(exc),)}
    return {}


def _model_sidecar_signature(path: Path, exists: bool) -> tuple[Path, tuple[str, bool, int, int]]:
    if not exists:
        return Path(""), ("", False, 0, 0)
    sidecar, sidecar_exists, size, mtime_ns = _first_existing_sidecar(path)
    signature = (str(sidecar) if sidecar else "", sidecar_exists, size, mtime_ns)
    return sidecar, signature


def _read_model_sidecar(sidecar: Path, exists: bool) -> dict[str, Any]:
    if not exists or not str(sidecar):
        return {}
    try:
        with open(sidecar, "r", encoding="utf-8") as fp:
            raw = json.load(fp)
        meta = _extract_sidecar_metadata(raw)
        meta["path"] = str(sidecar)
        return meta
    except Exception as exc:
        return {"path": str(sidecar), "errors": (str(exc),)}


def _is_json_action_file(path: Path, action_file: str) -> bool:
    suffix = path.suffix.lower()
    if suffix == ".json":
        return True
    return bool(action_file and not suffix)


def _has_keyframes(value: Any) -> bool:
    if not isinstance(value, Mapping):
        return False
    frames = value.get("keyframes")
    if not isinstance(frames, (list, tuple)):
        frames = value.get("frames")
    return isinstance(frames, (list, tuple)) and bool(frames)


def _select_clip_keyframes(model_meta: Mapping[str, Any], action_name: str, clip_name: Any) -> tuple[str, dict[str, Any]]:
    clips = model_meta.get("clip_keyframes")
    if not isinstance(clips, Mapping):
        return "", {}
    requested = [str(action_name or "")]
    clip_text = str(clip_name or "").strip()
    if clip_text:
        requested.insert(0, clip_text)
    normalized = {_normalize_action_name(name) for name in requested if str(name or "").strip()}
    for raw_name, raw_clip in clips.items():
        clip_key = str(raw_name or "").strip()
        if not clip_key or _normalize_action_name(clip_key) not in normalized:
            continue
        if isinstance(raw_clip, Mapping):
            selected = copy.deepcopy(dict(raw_clip))
            selected.setdefault("clip", clip_key)
            selected.setdefault("name", clip_key)
            return clip_key, selected
    return "", {}


def clear_model3d_metadata_caches() -> None:
    """Clear model3d probe/metadata caches for focused tests."""

    global _BACKEND_STATUS_CACHE
    _BACKEND_STATUS_CACHE = None
    _MODEL_METADATA_CACHE.clear()
    _ACTION_METADATA_CACHE.clear()
    _RETARGET_PLAN_CACHE.clear()


def get_model_metadata(node: Mapping[str, Any]) -> dict[str, Any]:
    """Return cached offline metadata for a model3d node's model file."""

    model = node.get("model") if isinstance(node.get("model"), Mapping) else {}
    path_text = model3d_path(node)
    fmt = str((model or {}).get("format") or node.get("format") or "auto").strip().lower() or "auto"
    reload_key = str((model or {}).get("reload_key") or node.get("reload_key") or "")
    resolved = resolve_model_path(path_text)
    exists, size, mtime_ns = _file_signature(resolved) if path_text else (False, 0, 0)
    sidecar_path, sidecar_signature = _model_sidecar_signature(resolved, exists)
    cache_key = (str(resolved), fmt, reload_key, exists, size, mtime_ns, sidecar_signature)
    cached = _MODEL_METADATA_CACHE.get(cache_key)
    if cached is not None:
        return _copy_metadata(cached)
    sidecar_meta = _read_model_sidecar(sidecar_path, bool(sidecar_signature[1]))
    file_meta = _read_model_file_metadata(resolved, fmt, exists)
    bone_names = _merge_unique(sidecar_meta.get("bone_names"), file_meta.get("bone_names"))
    clips = _merge_unique(sidecar_meta.get("clips"), file_meta.get("clips"))
    errors = _merge_unique(sidecar_meta.get("errors"), file_meta.get("errors"))
    clip_keyframes = copy.deepcopy(file_meta.get("clip_keyframes") or {})
    file_rest = dict(file_meta.get("rest_positions") or {}) if isinstance(file_meta.get("rest_positions"), Mapping) else {}
    sidecar_rest = dict(sidecar_meta.get("rest_positions") or {}) if isinstance(sidecar_meta.get("rest_positions"), Mapping) else {}
    rest_positions = dict(file_rest)
    rest_positions.update(sidecar_rest)
    rest_source = ""
    if sidecar_rest:
        rest_source = "sidecar"
    elif file_rest:
        rest_source = str(file_meta.get("rest_positions_source") or "model")
    metadata = {
        "path": path_text,
        "resolved_path": str(resolved),
        "format": fmt,
        "reload_key": reload_key,
        "exists": exists,
        "size": size,
        "mtime_ns": mtime_ns,
        "extension": resolved.suffix.lower(),
        "sidecar": dict(sidecar_meta),
        "mesh": dict(file_meta.get("mesh") or {}),
        "nodes": tuple(file_meta.get("nodes") or ()),
        "materials": tuple(file_meta.get("materials") or ()),
        "bone_names": bone_names,
        "clips": clips,
        "clip_keyframes": dict(clip_keyframes) if isinstance(clip_keyframes, Mapping) else {},
        "rest_positions": rest_positions,
        "rest_positions_source": rest_source,
        "metadata_errors": errors,
        "cache_key": cache_key,
    }
    _cache_put(_MODEL_METADATA_CACHE, cache_key, metadata)
    return _copy_metadata(metadata)


def get_action_metadata(node: Mapping[str, Any]) -> dict[str, Any]:
    """Return cached action data and the selected action config for a node."""

    action = node.get("action") if isinstance(node.get("action"), Mapping) else {}
    action = dict(action or {})
    name = str(action.get("name") or "idle")
    inline = dict(action.get("json") or {}) if isinstance(action.get("json"), Mapping) else {}
    json_text = ""
    if isinstance(action.get("json_text"), str):
        json_text = str(action.get("json_text") or "")
    elif isinstance(action.get("json"), str):
        json_text = str(action.get("json") or "")
    action_file = str(action.get("file") or "").strip()
    action_path = resolve_action_path(action_file, node) if action_file else Path("")
    file_exists, file_size, file_mtime_ns = _file_signature(action_path) if action_file else (False, 0, 0)
    model_meta = get_model_metadata(node)
    model_cache_key = tuple(model_meta.get("cache_key") or ())
    cache_key = (
        name,
        _hash_mapping(inline) if inline else "",
        _hash_text(json_text) if json_text else "",
        str(action_path) if action_file else "",
        file_exists,
        file_size,
        file_mtime_ns,
        model_cache_key,
    )
    cached = _ACTION_METADATA_CACHE.get(cache_key)
    if cached is not None:
        return _copy_metadata(cached)

    data: dict[str, Any] = dict(inline)
    errors: list[str] = []
    if json_text:
        try:
            loaded = json.loads(json_text)
            if isinstance(loaded, Mapping):
                data.update(dict(loaded))
            else:
                errors.append("action json_text must decode to an object")
        except Exception as exc:
            errors.append(str(exc))
    if action_file:
        if file_exists:
            if _is_json_action_file(action_path, action_file):
                try:
                    with open(action_path, "r", encoding="utf-8") as fp:
                        loaded = json.load(fp)
                    if isinstance(loaded, Mapping):
                        data.update(dict(loaded))
                    else:
                        errors.append("action file must decode to an object")
                except Exception as exc:
                    errors.append(str(exc))
        else:
            errors.append(f"action file is missing: {action_file}")

    selected = dict(data.get(name) or {}) if isinstance(data.get(name), Mapping) else {}
    model_clip_name = ""
    if not _has_keyframes(selected):
        model_clip_name, model_selected = _select_clip_keyframes(model_meta, name, action.get("clip"))
        if model_selected:
            merged = dict(selected)
            merged.update(model_selected)
            selected = merged
            data.setdefault(name, dict(selected))
    file_kind = "json" if action_file and _is_json_action_file(action_path, action_file) else "motion" if action_file else ""
    metadata = {
        "name": name,
        "data": dict(data),
        "selected": dict(selected or {}),
        "model_clip": model_clip_name,
        "file": action_file,
        "resolved_file": str(action_path) if action_file else "",
        "file_kind": file_kind,
        "motion_file": action_file if file_kind == "motion" else "",
        "resolved_motion_file": str(action_path) if file_kind == "motion" else "",
        "motion_exists": bool(file_exists) if file_kind == "motion" else False,
        "motion_format": action_path.suffix.lower().lstrip(".") if file_kind == "motion" else "",
        "errors": tuple(errors),
        "cache_key": cache_key,
    }
    if errors:
        metadata["selected"]["_load_error"] = "; ".join(errors)
    _cache_put(_ACTION_METADATA_CACHE, cache_key, metadata)
    return _copy_metadata(metadata)


def _normalize_bone_token(value: Any) -> str:
    text = str(value or "").strip().lower()
    if not text:
        return ""
    for prefix in (
        "mixamorig:",
        "mixamorig",
        "cc_base_",
        "ccbase_",
        "bip001 ",
        "bip001_",
        "bip001",
        "bip ",
        "bip_",
        "bip",
        "armature|",
        "armature:",
        "def-",
        "jnt_",
    ):
        if text.startswith(prefix):
            text = text[len(prefix):]
    out = []
    for ch in text:
        if ch.isalnum():
            out.append(ch)
    return "".join(out)


def _bone_index(names: list[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    for name in names:
        norm = _normalize_bone_token(name)
        if norm and norm not in out:
            out[norm] = name
    return out


def _explicit_bone_requests(node: Mapping[str, Any]) -> dict[str, str]:
    requests: dict[str, str] = {}
    skeleton = node.get("skeleton") if isinstance(node.get("skeleton"), Mapping) else {}
    retarget = node.get("retarget") if isinstance(node.get("retarget"), Mapping) else {}
    for source in (skeleton, retarget):
        if not isinstance(source, Mapping):
            continue
        bone_map = source.get("bone_map")
        if isinstance(bone_map, Mapping):
            for key, value in bone_map.items():
                requests[str(key)] = str(value)
        for key, value in source.items():
            if key in {"bone_map", "aliases", "profile"}:
                continue
            if isinstance(value, (str, int, float)):
                requests[str(key)] = str(value)
    for coarse, aliases in _COARSE_BONE_FALLBACKS.items():
        if coarse in requests:
            for alias in aliases:
                requests.setdefault(alias, requests[coarse])
    return requests


def _sidecar_bone_map(model_meta: Mapping[str, Any]) -> dict[str, str]:
    sidecar = model_meta.get("sidecar") if isinstance(model_meta.get("sidecar"), Mapping) else {}
    bone_map = sidecar.get("bone_map") if isinstance(sidecar.get("bone_map"), Mapping) else {}
    return {str(k): str(v) for k, v in bone_map.items()}


def _resolve_bone(
    canonical: str,
    request: str,
    names: list[str],
    index: Mapping[str, str],
) -> tuple[str, str]:
    raw = str(request or "").strip()
    if raw and raw.lower() != "auto":
        norm = _normalize_bone_token(raw)
        return str(index.get(norm) or raw), "explicit"
    for alias in (canonical, *_BONE_ALIASES.get(canonical, ())):
        norm = _normalize_bone_token(alias)
        if norm in index:
            return str(index[norm]), "alias"
    if names:
        return "", "unresolved"
    return "", "pending_backend"


def get_retarget_plan(node: Mapping[str, Any]) -> dict[str, Any]:
    """Build an offline humanoid retarget plan for a model3d node.

    The native renderer will eventually consume this plan directly.  Until then
    it gives script plugins deterministic diagnostics and lets users provide a
    sidecar skeleton map for arbitrary FBX/model files without loading Assimp.
    """

    retarget = node.get("retarget") if isinstance(node.get("retarget"), Mapping) else {}
    model_meta = get_model_metadata(node)
    skeleton = node.get("skeleton") if isinstance(node.get("skeleton"), Mapping) else {}
    cache_key = (
        tuple(model_meta.get("cache_key") or ()),
        _hash_mapping(retarget) if retarget else "",
        _hash_mapping(skeleton) if skeleton else "",
    )
    cached = _RETARGET_PLAN_CACHE.get(cache_key)
    if cached is not None:
        return _copy_retarget_plan(cached)

    sidecar_map = _sidecar_bone_map(model_meta)
    requests = dict(sidecar_map)
    requests.update(_explicit_bone_requests(node))
    bone_names = list(model_meta.get("bone_names") or [])
    index = _bone_index(bone_names)

    resolved: dict[str, str] = {}
    sources: dict[str, str] = {}
    unresolved: list[str] = []
    pending_backend: list[str] = []
    for canonical in HUMANOID_BONES:
        mapped, source = _resolve_bone(canonical, requests.get(canonical, "auto"), bone_names, index)
        if mapped:
            resolved[canonical] = mapped
            sources[canonical] = source
        elif source == "pending_backend":
            pending_backend.append(canonical)
        else:
            unresolved.append(canonical)

    required = ("hips", "spine", "head")
    warnings: list[str] = []
    missing_required = [name for name in required if name not in resolved]
    if missing_required and bone_names:
        warnings.append("missing required humanoid bones: " + ", ".join(missing_required))
    if pending_backend and not bone_names:
        warnings.append("no offline bone list; native backend will resolve bones at load time")
    if unresolved:
        warnings.append("unresolved optional bones: " + ", ".join(unresolved[:6]))

    stretch_limit = _float_from_mapping(retarget, "stretch_limit", 0.08, lo=0.0, hi=0.5)
    twist_limit = _float_from_mapping(retarget, "twist_limit", 0.35, lo=0.0, hi=1.5)
    total = len(HUMANOID_BONES)
    coverage = len(resolved) / float(total) if total else 0.0
    plan = {
        "mode": str(retarget.get("mode") or "humanoid_auto"),
        "profile": str(retarget.get("profile") or retarget.get("mode") or "humanoid_auto"),
        "rest_pose": str(retarget.get("rest_pose") or "auto"),
        "preserve_proportions": bool(retarget.get("preserve_proportions", True)),
        "adaptive_ik": bool(retarget.get("adaptive_ik", True)),
        "prefer_model_clips": bool(retarget.get("prefer_model_clips", True)),
        "stretch_limit": stretch_limit,
        "twist_limit": twist_limit,
        "bone_map": resolved,
        "sources": sources,
        "unresolved": tuple(unresolved),
        "pending_backend": tuple(pending_backend),
        "coverage": coverage,
        "resolved_count": len(resolved),
        "total_count": total,
        "bone_names": tuple(bone_names),
        "clips": tuple(model_meta.get("clips") or ()),
        "warnings": tuple(warnings),
    }
    _cache_put(_RETARGET_PLAN_CACHE, cache_key, plan)
    return _copy_retarget_plan(plan)


def _float_from_mapping(src: Mapping[str, Any], key: str, default: float,
                        *, lo: float, hi: float) -> float:
    try:
        value = float(src.get(key, default))
        if value != value:
            raise ValueError("nan")
    except Exception:
        value = default
    return max(lo, min(hi, value))


def _vec_add(a: tuple[float, float, float], b: tuple[float, float, float]) -> tuple[float, float, float]:
    return a[0] + b[0], a[1] + b[1], a[2] + b[2]


def _vec_sub(a: tuple[float, float, float], b: tuple[float, float, float]) -> tuple[float, float, float]:
    return a[0] - b[0], a[1] - b[1], a[2] - b[2]


def _vec_scale(a: tuple[float, float, float], scale: float) -> tuple[float, float, float]:
    return a[0] * scale, a[1] * scale, a[2] * scale


def _vec_len(a: tuple[float, float, float]) -> float:
    return math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])


def _vec_cross(
    a: tuple[float, float, float],
    b: tuple[float, float, float],
) -> tuple[float, float, float]:
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def _quat_normalize(q: tuple[float, float, float, float]) -> tuple[float, float, float, float]:
    length = math.sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3])
    if length <= 0.000001:
        return 0.0, 0.0, 0.0, 1.0
    return q[0] / length, q[1] / length, q[2] / length, q[3] / length


def _quat_rotate_vec(
    q: tuple[float, float, float, float],
    v: tuple[float, float, float],
) -> tuple[float, float, float]:
    x, y, z, w = _quat_normalize(q)
    u = (x, y, z)
    uv = _vec_cross(u, v)
    uuv = _vec_cross(u, uv)
    return _vec_add(v, _vec_add(_vec_scale(uv, 2.0 * w), _vec_scale(uuv, 2.0)))


def _lerp_quat(
    a: tuple[float, float, float, float],
    b: tuple[float, float, float, float],
    amount: float,
) -> tuple[float, float, float, float]:
    t = max(0.0, min(1.0, float(amount or 0.0)))
    dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]
    if dot < 0.0:
        b = (-b[0], -b[1], -b[2], -b[3])
    return _quat_normalize((
        a[0] + (b[0] - a[0]) * t,
        a[1] + (b[1] - a[1]) * t,
        a[2] + (b[2] - a[2]) * t,
        a[3] + (b[3] - a[3]) * t,
    ))


def _normalize_action_name(value: Any) -> str:
    return str(value or "").strip().lower().replace(" ", "_")


def _canonical_from_token(token: Any) -> str:
    raw = str(token or "").strip()
    if raw in HUMANOID_BONES:
        return raw
    norm = _normalize_bone_token(raw)
    for canonical in HUMANOID_BONES:
        if norm == _normalize_bone_token(canonical):
            return canonical
        for alias in _BONE_ALIASES.get(canonical, ()):
            if norm == _normalize_bone_token(alias):
                return canonical
    return raw if raw in HUMANOID_BONES else ""


def _pose_offsets_from_mapping(value: Any) -> dict[str, tuple[float, float, float]]:
    if not isinstance(value, Mapping):
        return {}
    out: dict[str, tuple[float, float, float]] = {}
    for key, raw in value.items():
        canonical = _canonical_from_token(key)
        point = _point3(raw)
        if canonical and point is not None:
            out[canonical] = point
    return out


def _pose_rotations_from_mapping(value: Any) -> dict[str, tuple[float, float, float, float]]:
    if not isinstance(value, Mapping):
        return {}
    out: dict[str, tuple[float, float, float, float]] = {}
    for key, raw in value.items():
        canonical = _canonical_from_token(key)
        quat = _quat4(raw)
        if canonical and quat is not None:
            out[canonical] = _quat_normalize(quat)
    return out


def _merge_offsets(*groups: Mapping[str, tuple[float, float, float]]) -> dict[str, tuple[float, float, float]]:
    out: dict[str, tuple[float, float, float]] = {}
    for group in groups:
        for key, value in group.items():
            old = out.get(key, (0.0, 0.0, 0.0))
            out[key] = _vec_add(old, value)
    return out


def _bool_from_mapping(src: Mapping[str, Any], key: str, default: bool) -> bool:
    try:
        value = src.get(key, default)
    except Exception:
        return bool(default)
    if isinstance(value, str):
        text = value.strip().lower()
        if text in {"0", "false", "no", "off"}:
            return False
        if text in {"1", "true", "yes", "on"}:
            return True
    return bool(value)


def _frame_time(frame: Mapping[str, Any], fallback: float) -> float:
    for key in ("time", "t", "phase", "frame"):
        try:
            value = frame.get(key)
            if value is not None:
                result = float(value)
                if result == result:
                    return result
        except Exception:
            continue
    return float(fallback)


def _frame_offsets(frame: Mapping[str, Any]) -> dict[str, tuple[float, float, float]]:
    direct = {
        key: value for key, value in frame.items()
        if key not in {
            "time", "t", "phase", "frame",
            "offsets", "pose_offsets", "bone_offsets", "bones",
            "rotations", "pose_rotations", "bone_rotations",
        }
    }
    return _merge_offsets(
        _pose_offsets_from_mapping(frame.get("pose_offsets")),
        _pose_offsets_from_mapping(frame.get("bone_offsets")),
        _pose_offsets_from_mapping(frame.get("offsets")),
        _pose_offsets_from_mapping(frame.get("bones")),
        _pose_offsets_from_mapping(direct),
    )


def _frame_rotations(frame: Mapping[str, Any]) -> dict[str, tuple[float, float, float, float]]:
    out: dict[str, tuple[float, float, float, float]] = {}
    for group in (
        _pose_rotations_from_mapping(frame.get("pose_rotations")),
        _pose_rotations_from_mapping(frame.get("bone_rotations")),
        _pose_rotations_from_mapping(frame.get("rotations")),
    ):
        out.update(group)
    return out


def _lerp_offsets(
    a: Mapping[str, tuple[float, float, float]],
    b: Mapping[str, tuple[float, float, float]],
    amount: float,
) -> dict[str, tuple[float, float, float]]:
    t = max(0.0, min(1.0, float(amount or 0.0)))
    out: dict[str, tuple[float, float, float]] = {}
    for key in set(a) | set(b):
        av = a.get(key, (0.0, 0.0, 0.0))
        bv = b.get(key, (0.0, 0.0, 0.0))
        out[key] = (
            av[0] + (bv[0] - av[0]) * t,
            av[1] + (bv[1] - av[1]) * t,
            av[2] + (bv[2] - av[2]) * t,
        )
    return out


def _lerp_rotations(
    a: Mapping[str, tuple[float, float, float, float]],
    b: Mapping[str, tuple[float, float, float, float]],
    amount: float,
) -> dict[str, tuple[float, float, float, float]]:
    t = max(0.0, min(1.0, float(amount or 0.0)))
    out: dict[str, tuple[float, float, float, float]] = {}
    for key in set(a) | set(b):
        av = a.get(key)
        bv = b.get(key)
        if av is None and bv is not None:
            av = bv
        if bv is None and av is not None:
            bv = av
        if av is None or bv is None:
            continue
        out[key] = _lerp_quat(av, bv, t)
    return out


def _json_rotations(
    rotations: Mapping[str, tuple[float, float, float, float]],
) -> dict[str, list[float]]:
    return {key: [float(x) for x in value] for key, value in sorted(rotations.items())}


def _sample_keyframe_offsets(
    selected: Mapping[str, Any],
    phase: float,
) -> tuple[dict[str, tuple[float, float, float]], dict[str, Any]]:
    raw_frames = selected.get("keyframes")
    if not isinstance(raw_frames, (list, tuple)):
        raw_frames = selected.get("frames")
    if not isinstance(raw_frames, (list, tuple)):
        return {}, {}

    frames: list[tuple[
        float,
        dict[str, tuple[float, float, float]],
        dict[str, tuple[float, float, float, float]],
    ]] = []
    for index, raw in enumerate(raw_frames):
        if not isinstance(raw, Mapping):
            continue
        offsets = _frame_offsets(raw)
        rotations = _frame_rotations(raw)
        if offsets or rotations:
            frames.append((_frame_time(raw, float(index)), offsets, rotations))
    frames.sort(key=lambda item: item[0])
    if not frames:
        return {}, {}

    speed = _float_from_mapping(selected, "speed", 1.0, lo=0.05, hi=5.0)
    t = float(phase or 0.0) * speed
    try:
        duration = float(selected.get("duration") or 0.0)
    except Exception:
        duration = 0.0
    if duration <= 0.0:
        duration = max(frames[-1][0], 0.0)
    loop = _bool_from_mapping(selected, "loop", True)
    if loop and duration > 0.0:
        t = t % duration
    elif frames:
        t = max(frames[0][0], min(frames[-1][0], t))

    if len(frames) == 1:
        return dict(frames[0][1]), {
            "motion_source": "keyframes",
            "sample_time": t,
            "keyframe_count": 1,
            "duration": duration,
            "loop": loop,
            "rotations": _json_rotations(frames[0][2]),
            "rotation_count": len(frames[0][2]),
        }

    prev = frames[0]
    nxt = frames[-1]
    for index in range(1, len(frames)):
        if t <= frames[index][0]:
            prev = frames[index - 1]
            nxt = frames[index]
            break

    span = max(0.000001, nxt[0] - prev[0])
    amount = (t - prev[0]) / span
    interpolation = str(selected.get("interpolation") or "linear").strip().lower()
    if interpolation in {"step", "hold", "nearest"}:
        sampled = dict(prev[1] if amount < 1.0 else nxt[1])
        rotations = dict(prev[2] if amount < 1.0 else nxt[2])
    else:
        sampled = _lerp_offsets(prev[1], nxt[1], amount)
        rotations = _lerp_rotations(prev[2], nxt[2], amount)
    return sampled, {
        "motion_source": "keyframes",
        "sample_time": t,
        "keyframe_count": len(frames),
        "duration": duration,
        "loop": loop,
        "frame_a": prev[0],
        "frame_b": nxt[0],
        "blend": max(0.0, min(1.0, amount)),
        "rotations": _json_rotations(rotations),
        "rotation_count": len(rotations),
    }


def _humanoid_descendants(root: str) -> tuple[str, ...]:
    children: dict[str, list[str]] = {}
    for parent, child in HUMANOID_SEGMENTS:
        children.setdefault(parent, []).append(child)
    out: list[str] = []
    stack = list(children.get(root, ()))
    seen: set[str] = set()
    while stack:
        bone = stack.pop(0)
        if bone in seen:
            continue
        seen.add(bone)
        out.append(bone)
        stack.extend(children.get(bone, ()))
    return tuple(out)


def _rotation_offsets_from_samples(
    rest: Mapping[str, tuple[float, float, float]],
    rotations: Mapping[str, Any],
) -> dict[str, tuple[float, float, float]]:
    out: dict[str, tuple[float, float, float]] = {}
    for bone, raw_quat in rotations.items():
        canonical = _canonical_from_token(bone)
        quat = _quat4(raw_quat)
        if not canonical or quat is None or canonical not in rest:
            continue
        anchor = rest[canonical]
        for child in _humanoid_descendants(canonical):
            if child not in rest:
                continue
            rest_vec = _vec_sub(rest[child], anchor)
            if _vec_len(rest_vec) <= 0.000001:
                continue
            rotated = _vec_add(anchor, _quat_rotate_vec(quat, rest_vec))
            delta = _vec_sub(rotated, rest[child])
            out[child] = _vec_add(out.get(child, (0.0, 0.0, 0.0)), delta)
    return out


def _procedural_action_offsets(action_name: str, phase: float, selected: Mapping[str, Any]) -> dict[str, tuple[float, float, float]]:
    name = _normalize_action_name(action_name)
    speed = _float_from_mapping(selected, "speed", 1.0, lo=0.05, hi=5.0)
    t = float(phase or 0.0) * speed
    arm = _float_from_mapping(selected, "armSwing", 0.16, lo=0.0, hi=1.2)
    leg = _float_from_mapping(selected, "legSwing", 0.14, lo=0.0, hi=1.2)
    bounce = _float_from_mapping(selected, "bounce", 0.03, lo=0.0, hi=0.4)
    wave = math.sin(t * 3.0)
    step = math.sin(t * 4.0)
    offsets: dict[str, tuple[float, float, float]] = {}

    if "jump" in name:
        lift = abs(math.sin(t * 2.1)) * max(0.16, bounce)
        for bone in ("hips", "spine", "chest", "neck", "head", "left_shoulder", "right_shoulder"):
            offsets[bone] = (0.0, lift, 0.0)
        offsets["left_foot"] = (-0.03, lift * 0.25, 0.02)
        offsets["right_foot"] = (0.03, lift * 0.25, 0.02)
    elif "walk" in name or "run" in name:
        offsets["left_arm"] = (0.0, -arm * step * 0.18, 0.0)
        offsets["left_forearm"] = (0.0, -arm * step * 0.26, 0.0)
        offsets["left_hand"] = (0.0, -arm * step * 0.34, 0.0)
        offsets["right_arm"] = (0.0, arm * step * 0.18, 0.0)
        offsets["right_forearm"] = (0.0, arm * step * 0.26, 0.0)
        offsets["right_hand"] = (0.0, arm * step * 0.34, 0.0)
        offsets["left_knee"] = (leg * step * 0.10, abs(step) * 0.06, 0.0)
        offsets["left_foot"] = (leg * step * 0.18, abs(step) * 0.08, 0.03)
        offsets["right_knee"] = (-leg * step * 0.10, abs(step) * 0.04, 0.0)
        offsets["right_foot"] = (-leg * step * 0.18, abs(step) * 0.05, -0.03)
        offsets["hips"] = (0.0, abs(step) * bounce, 0.0)
    elif "wave" in name:
        lift = _float_from_mapping(selected, "rightArmLift", 0.52, lo=0.0, hi=1.5)
        offsets["right_arm"] = (-0.05, lift * 0.16, 0.0)
        offsets["right_forearm"] = (-0.18, lift * 0.36 + wave * 0.05, 0.0)
        offsets["right_hand"] = (-0.28, lift * 0.58 + wave * 0.10, 0.0)
        offsets["left_hand"] = (0.03, -0.04, 0.0)
    else:
        idle = math.sin(t * 1.6)
        offsets["hips"] = (0.0, idle * bounce * 0.5, 0.0)
        offsets["chest"] = (idle * 0.015, idle * bounce, 0.0)
        offsets["head"] = (idle * 0.02, idle * bounce * 1.2, 0.0)
        offsets["right_hand"] = (0.0, idle * 0.025, 0.0)
        offsets["left_hand"] = (0.0, -idle * 0.020, 0.0)
    return offsets


def _canonical_rest_positions(plan: Mapping[str, Any], model_meta: Mapping[str, Any]) -> tuple[dict[str, tuple[float, float, float]], str]:
    rest_positions = model_meta.get("rest_positions")
    rest_source = str(model_meta.get("rest_positions_source") or "").strip()
    if not isinstance(rest_positions, Mapping):
        sidecar = model_meta.get("sidecar") if isinstance(model_meta.get("sidecar"), Mapping) else {}
        rest_positions = sidecar.get("rest_positions") if isinstance(sidecar.get("rest_positions"), Mapping) else {}
        if isinstance(rest_positions, Mapping) and rest_positions:
            rest_source = "sidecar"
    bone_map = plan.get("bone_map") if isinstance(plan.get("bone_map"), Mapping) else {}
    out: dict[str, tuple[float, float, float]] = {}
    source = "default"
    for canonical in HUMANOID_BONES:
        actual = str(bone_map.get(canonical) or "")
        point = None
        if isinstance(rest_positions, Mapping):
            point = _point3(rest_positions.get(actual)) or _point3(rest_positions.get(canonical))
        if point is not None:
            out[canonical] = point
            source = rest_source or "model"
        elif canonical in _DEFAULT_REST_POSITIONS:
            out[canonical] = _DEFAULT_REST_POSITIONS[canonical]
    return out, source


def evaluate_retarget_pose(node: Mapping[str, Any]) -> dict[str, Any]:
    """Evaluate a safe humanoid pose for a model3d node.

    This is an offline-safe retargeting stage: it uses sidecar/model metadata,
    action JSON, and the declared stretch limit to produce bone positions that
    preserve the target rest proportions.  Native importers can later feed real
    rest positions and clip samples into the same contract.
    """

    model_meta = get_model_metadata(node)
    if not bool(model_meta.get("exists")):
        return {
            "ok": False,
            "reason": "model missing",
            "positions": {},
            "segments": (),
        }
    plan = get_retarget_plan(node)
    action_meta = get_action_metadata(node)
    action_name = str(action_meta.get("name") or "idle")
    selected = action_meta.get("selected") if isinstance(action_meta.get("selected"), Mapping) else {}
    action = node.get("action") if isinstance(node.get("action"), Mapping) else {}
    try:
        phase = float(action.get("time", node.get("phase", 0.0)) or 0.0)
    except Exception:
        phase = 0.0

    rest, rest_source = _canonical_rest_positions(plan, model_meta)
    if not rest:
        return {
            "ok": False,
            "reason": "no rest pose",
            "positions": {},
            "segments": (),
        }
    explicit_offsets = _merge_offsets(
        _pose_offsets_from_mapping(selected.get("pose_offsets")),
        _pose_offsets_from_mapping(selected.get("bone_offsets")),
        _pose_offsets_from_mapping(selected.get("offsets")),
    )
    keyframe_offsets, keyframe_info = _sample_keyframe_offsets(selected, phase)
    keyframe_rotations = (
        keyframe_info.get("rotations") if isinstance(keyframe_info.get("rotations"), Mapping) else {}
    )
    rotation_offsets = _rotation_offsets_from_samples(rest, keyframe_rotations)
    has_keyframe_motion = keyframe_info.get("motion_source") == "keyframes"
    use_procedural = (
        not has_keyframe_motion
        or _bool_from_mapping(selected, "procedural", False)
    )
    offsets = _merge_offsets(
        _procedural_action_offsets(action_name, phase, selected) if use_procedural else {},
        rotation_offsets,
        keyframe_offsets,
        explicit_offsets,
    )
    stretch_limit = float(plan.get("stretch_limit") or 0.0)
    preserve = bool(plan.get("preserve_proportions", True))

    pose: dict[str, tuple[float, float, float]] = {
        bone: _vec_add(point, offsets.get(bone, (0.0, 0.0, 0.0)))
        for bone, point in rest.items()
    }
    clamped: list[str] = []
    segment_infos: list[dict[str, Any]] = []
    max_stretch = 0.0
    for parent, child in HUMANOID_SEGMENTS:
        if parent not in rest or child not in rest or parent not in pose:
            continue
        rest_vec = _vec_sub(rest[child], rest[parent])
        rest_len = _vec_len(rest_vec)
        if rest_len <= 0.000001:
            continue
        target = pose.get(child, rest[child])
        vec = _vec_sub(target, pose[parent])
        length = _vec_len(vec)
        was_clamped = False
        if preserve:
            lo = rest_len * max(0.0, 1.0 - stretch_limit)
            hi = rest_len * (1.0 + stretch_limit)
            clipped = max(lo, min(hi, length))
            if abs(clipped - length) > 0.000001:
                direction = _vec_scale(vec, 1.0 / length) if length > 0.000001 else _vec_scale(rest_vec, 1.0 / rest_len)
                target = _vec_add(pose[parent], _vec_scale(direction, clipped))
                pose[child] = target
                length = clipped
                was_clamped = True
                clamped.append(f"{parent}->{child}")
        stretch = abs((length / rest_len) - 1.0)
        max_stretch = max(max_stretch, stretch)
        segment_infos.append({
            "parent": parent,
            "child": child,
            "rest_length": rest_len,
            "length": length,
            "stretch": stretch,
            "clamped": was_clamped,
        })

    return {
        "ok": True,
        "mode": plan.get("mode"),
        "profile": plan.get("profile"),
        "action": action_name,
        "phase": phase,
        "coverage": plan.get("coverage"),
        "stretch_limit": stretch_limit,
        "preserve_proportions": preserve,
        "rest_source": rest_source,
        "motion_source": keyframe_info.get("motion_source") or ("procedural" if use_procedural else "offsets"),
        "motion_sample": dict(keyframe_info),
        "rest_positions": {key: [float(x) for x in value] for key, value in rest.items()},
        "positions": {key: [float(x) for x in value] for key, value in pose.items()},
        "segments": tuple(segment_infos),
        "clamped_segments": tuple(clamped),
        "clamped_count": len(clamped),
        "max_stretch": max_stretch,
        "warnings": tuple(plan.get("warnings") or ()),
    }


def diagnose_model3d_node(
    plugin_id: Any,
    node: Mapping[str, Any],
    *,
    order: int = 0,
    status: Model3DBackendStatus | None = None,
) -> tuple[str, tuple[str, ...]]:
    """Return ``(key, diagnostic lines)`` for a model3d node."""

    backend = status or get_backend_status()
    key = model3d_node_key(plugin_id, node, order)
    model_meta = get_model_metadata(node)
    path_text = str(model_meta.get("path") or "")
    lines = [f"model3d {key}"]
    if path_text:
        lines.append(f"model: {Path(path_text).name or path_text}")
        if not bool(model_meta.get("exists")):
            lines.append("model file is missing")
    else:
        lines.append("model path is empty")
    if not backend.render_available:
        lines.append(backend.reason)
    retarget = node.get("retarget") if isinstance(node.get("retarget"), Mapping) else {}
    if retarget:
        plan = get_retarget_plan(node)
        if plan.get("mode"):
            lines.append(
                f"retarget {plan.get('mode')}: "
                f"{int(plan.get('resolved_count') or 0)}/{int(plan.get('total_count') or 0)} bones"
            )
        for warning in list(plan.get("warnings") or ())[:2]:
            lines.append(str(warning))
    return key, tuple(lines)


__all__ = [
    "Model3DBackendStatus",
    "clear_model3d_metadata_caches",
    "diagnose_model3d_node",
    "evaluate_retarget_pose",
    "get_action_metadata",
    "get_backend_status",
    "get_model_metadata",
    "get_retarget_plan",
    "HUMANOID_BONES",
    "HUMANOID_SEGMENTS",
    "model3d_node_key",
    "model3d_path",
    "resolve_action_path",
    "resolve_model_path",
    "probe_model3d_backend",
]
