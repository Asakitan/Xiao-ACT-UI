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


_MANAGED_NAMES = ("AssimpNet.dll", "StirlingLabs.Assimp.Net.dll")
_NATIVE_NAMES = (
    "assimp.dll",
    "assimp-vc143-mt.dll",
    "assimp-vc142-mt.dll",
    "assimp-vc141-mt.dll",
    "libassimp.dll",
)
_CACHE_LIMIT = 128
_MODEL_PARSE_LIMIT = 4 * 1024 * 1024
_ACTION_JSON_PARSE_LIMIT = 4 * 1024 * 1024
_ACTION_JSON_CACHE_PREFIX_LIMIT = 4096
_MESH_PREVIEW_VERTEX_LIMIT = 4096
_MESH_PREVIEW_FACE_LIMIT = 8192
_BACKEND_STATUS_CACHE: Model3DBackendStatus | None = None
_MODEL_METADATA_CACHE: dict[tuple[Any, ...], dict[str, Any]] = {}
_ACTION_METADATA_CACHE: dict[tuple[Any, ...], dict[str, Any]] = {}
_ACTION_JSON_TEXT_CACHE: dict[tuple[str, str], tuple[dict[str, Any], tuple[str, ...]]] = {}
_RETARGET_PLAN_CACHE: dict[tuple[Any, ...], dict[str, Any]] = {}
_UNITY_ANIM_CACHE: dict[tuple[Any, ...], dict[str, Any]] = {}

_UNITY_HAND_MUSCLE_RE = re.compile(
    r"^(LeftHand|RightHand)\.(Thumb|Index|Middle|Ring|Little)\.(?:(\d)\s+Stretched|Spread)$",
    re.IGNORECASE,
)
_UNITY_FINGER_SEGMENTS = {
    "1": "Proximal",
    "2": "Intermediate",
    "3": "Distal",
}
_UNITY_FINGER_CURL_LIMITS = {
    "Thumb": (1.05, 0.95, 0.75),
    "Index": (1.30, 1.15, 0.95),
    "Middle": (1.30, 1.15, 0.95),
    "Ring": (1.35, 1.20, 1.00),
    "Little": (1.35, 1.20, 1.00),
}
_UNITY_FINGER_SPREAD_LIMITS = {
    "Thumb": 0.70,
    "Index": 0.34,
    "Middle": 0.22,
    "Ring": 0.26,
    "Little": 0.34,
}

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
    "hips": ("hips", "hip", "pelvis", "pelvisbone", "waist", "mixamorig:hips", "bip001pelvis", "spine05", "j_bip_c_hips"),
    "spine": ("spine", "spine1", "spine01", "spine_01", "spine04", "spine.001", "j_bip_c_spine", "torso", "body"),
    "chest": ("chest", "upperchest", "spine2", "spine02", "spine_02", "spine03", "spine.002", "spine.003", "j_bip_c_chest", "j_bip_c_upperchest", "breast"),
    "neck": ("neck", "neck1", "neck01", "neck02", "neck03", "j_bip_c_neck"),
    "head": ("head", "headtop", "head_end", "j_bip_c_head"),
    "left_shoulder": ("leftshoulder", "lshoulder", "shoulder_l", "l_clavicle", "leftclavicle", "clavicle_l", "clavicle.l", "shoulder01.l", "j_bip_l_shoulder"),
    "left_arm": ("leftarm", "leftupperarm", "upperarm_l", "lupperarm", "arm_l", "l_arm", "upperarm01.l", "upperarm02.l", "upper_arm.l", "j_bip_l_upperarm"),
    "left_forearm": ("leftforearm", "leftlowerarm", "forearm_l", "lowerarm_l", "lower_arm.l", "lelbow", "l_forearm", "lowerarm01.l", "lowerarm02.l", "j_bip_l_lowerarm"),
    "left_hand": ("lefthand", "hand_l", "lhand", "l_hand", "leftwrist", "wrist_l", "wrist.l", "j_bip_l_hand"),
    "right_shoulder": ("rightshoulder", "rshoulder", "shoulder_r", "r_clavicle", "rightclavicle", "clavicle_r", "clavicle.r", "shoulder01.r", "j_bip_r_shoulder"),
    "right_arm": ("rightarm", "rightupperarm", "upperarm_r", "rupperarm", "arm_r", "r_arm", "upperarm01.r", "upperarm02.r", "upper_arm.r", "j_bip_r_upperarm"),
    "right_forearm": ("rightforearm", "rightlowerarm", "forearm_r", "lowerarm_r", "lower_arm.r", "relbow", "r_forearm", "lowerarm01.r", "lowerarm02.r", "j_bip_r_lowerarm"),
    "right_hand": ("righthand", "hand_r", "rhand", "r_hand", "rightwrist", "wrist_r", "wrist.r", "j_bip_r_hand"),
    "left_leg": ("leftupleg", "leftupperleg", "leftleg", "thigh_l", "upleg_l", "upper_leg.l", "lthigh", "l_leg", "upperleg01.l", "upperleg02.l", "j_bip_l_upperleg"),
    "left_knee": ("leftleg", "leftlowerleg", "calf_l", "leg_l", "lower_leg.l", "lknee", "shin_l", "lowerleg01.l", "lowerleg02.l", "j_bip_l_lowerleg"),
    "left_foot": ("leftfoot", "foot_l", "lfoot", "l_foot", "leftankle", "ankle_l", "foot.l", "j_bip_l_foot"),
    "right_leg": ("rightupleg", "rightupperleg", "rightleg", "thigh_r", "upleg_r", "upper_leg.r", "rthigh", "r_leg", "upperleg01.r", "upperleg02.r", "j_bip_r_upperleg"),
    "right_knee": ("rightleg", "rightlowerleg", "calf_r", "leg_r", "lower_leg.r", "rknee", "shin_r", "lowerleg01.r", "lowerleg02.r", "j_bip_r_lowerleg"),
    "right_foot": ("rightfoot", "foot_r", "rfoot", "r_foot", "rightankle", "ankle_r", "foot.r", "j_bip_r_foot"),
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

_HUMANOID_PARENT_BY_CHILD = {child: parent for parent, child in HUMANOID_SEGMENTS}

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
    import_available: bool
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
    """Inspect the bundled AssimpNet/native-file layout."""

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
        reason = "AssimpNet importer and native assimp files are available."
    return Model3DBackendStatus(
        backend="assimpnet",
        files_present=files_present,
        import_available=files_present,
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


def _hash_action_source(value: Any) -> str:
    if isinstance(value, Mapping):
        return _hash_mapping(value)
    if isinstance(value, str):
        return _hash_text(value)
    if value is None:
        return ""
    return _hash_text(repr(value))


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
        "skins",
        "material_textures",
        "embedded_textures",
        "materials_config",
        "physics",
        "secondary_motion",
        "preview_mesh",
        "rest_positions",
        "node_parents",
        "clip_keyframes",
    ):
        item = out.get(key)
        if isinstance(item, Mapping):
            out[key] = copy.deepcopy(dict(item))
        elif isinstance(item, list):
            out[key] = copy.deepcopy(item)
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
    for key in ("bone_map", "sources", "declared_aliases"):
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


def _extract_node_parents(value: Any) -> dict[str, str]:
    if not isinstance(value, Mapping):
        return {}
    out: dict[str, str] = {}
    for child, parent in value.items():
        child_name = str(child or "").strip()
        parent_name = str(parent or "").strip()
        if child_name and parent_name and child_name != parent_name:
            out[child_name] = parent_name
    return out


def _extract_preview_skin(value: Any) -> list[list[dict[str, float | str]]]:
    if not isinstance(value, (list, tuple)):
        return []
    out: list[list[dict[str, float | str]]] = []
    any_weight = False
    for raw_vertex in value[:_MESH_PREVIEW_VERTEX_LIMIT]:
        if isinstance(raw_vertex, Mapping):
            raw_items: Any = (raw_vertex,)
        elif isinstance(raw_vertex, (list, tuple)):
            raw_items = raw_vertex
        else:
            raw_items = ()
        influences: list[dict[str, float | str]] = []
        for raw_item in list(raw_items)[:8]:
            if not isinstance(raw_item, Mapping):
                continue
            joint = str(
                raw_item.get("joint")
                or raw_item.get("bone")
                or raw_item.get("name")
                or ""
            ).strip()
            if not joint:
                continue
            try:
                weight = float(raw_item.get("weight", 0.0) or 0.0)
            except Exception:
                continue
            if not math.isfinite(weight) or weight <= 0.000001:
                continue
            influences.append({"joint": joint, "weight": float(weight)})
            any_weight = True
        out.append(influences)
    return out if any_weight else []


def _extract_preview_mesh(value: Any) -> dict[str, Any]:
    if not isinstance(value, Mapping):
        return {}
    vertices_raw = value.get("vertices")
    faces_raw = value.get("faces")
    if not isinstance(vertices_raw, (list, tuple)) or not isinstance(faces_raw, (list, tuple)):
        return {}
    vertices: list[list[float]] = []
    for raw in list(vertices_raw)[:_MESH_PREVIEW_VERTEX_LIMIT]:
        point = _point3(raw)
        if point is not None:
            vertices.append([float(point[0]), float(point[1]), float(point[2])])
    if len(vertices) < 3:
        return {}
    faces: list[list[int]] = []
    for raw_face in list(faces_raw)[:_MESH_PREVIEW_FACE_LIMIT]:
        if not isinstance(raw_face, (list, tuple)):
            continue
        face: list[int] = []
        for item in raw_face[:8]:
            try:
                index = int(item)
            except Exception:
                continue
            if 0 <= index < len(vertices):
                face.append(index)
        if len(face) >= 3:
            faces.append(face)
    if not faces:
        return {}
    out = {
        "source": str(value.get("source") or "sidecar"),
        "vertices": vertices,
        "faces": faces,
    }
    raw_face_materials = value.get("face_materials") or value.get("materials")
    if isinstance(raw_face_materials, (list, tuple)):
        face_materials = [
            str(item or "").strip()
            for item in list(raw_face_materials)[:len(faces)]
        ]
        if face_materials:
            while len(face_materials) < len(faces):
                face_materials.append("")
            out["face_materials"] = face_materials
    skin = _extract_preview_skin(value.get("skin"))
    if skin:
        out["skin"] = skin
        out["skin_source"] = "sidecar"
    return out


def _extract_sidecar_metadata(value: Any) -> dict[str, Any]:
    if not isinstance(value, Mapping):
        return {}
    src = dict(value)
    skeleton = src.get("skeleton") if isinstance(src.get("skeleton"), Mapping) else {}
    mesh = src.get("mesh") if isinstance(src.get("mesh"), Mapping) else {}
    preview = mesh.get("preview") if isinstance(mesh.get("preview"), Mapping) else {}
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
        aliases = skeleton.get("aliases") or skeleton.get("bone_aliases")
        if isinstance(aliases, Mapping):
            out["aliases"] = copy.deepcopy(dict(aliases))
    rest_positions = (
        _extract_rest_positions(src.get("rest_positions"))
        or _extract_rest_positions((skeleton or {}).get("rest_positions"))
        or _extract_rest_positions((skeleton or {}).get("rest_offsets"))
    )
    if rest_positions:
        out["rest_positions"] = dict(rest_positions)
    node_parents = (
        _extract_node_parents(src.get("node_parents"))
        or _extract_node_parents(src.get("parents"))
        or _extract_node_parents((skeleton or {}).get("node_parents"))
        or _extract_node_parents((skeleton or {}).get("parents"))
    )
    if node_parents:
        out["node_parents"] = dict(node_parents)
    preview_skin = (
        _extract_preview_skin(src.get("preview_skin"))
        or _extract_preview_skin((preview or {}).get("skin"))
        or _extract_preview_skin((mesh or {}).get("skin"))
    )
    if preview_skin:
        out["preview_skin"] = preview_skin
    preview_mesh = _extract_preview_mesh(preview)
    if preview_mesh:
        out["preview_mesh"] = preview_mesh
    for key in ("profile", "up_axis", "unit_scale", "rest_pose"):
        if key in src:
            out[key] = _json_safe_scalar(src.get(key))
        elif skeleton and key in skeleton:
            out[key] = _json_safe_scalar(skeleton.get(key))
    materials = src.get("materials")
    if isinstance(materials, (Mapping, list, tuple)):
        out["materials_config"] = copy.deepcopy(materials)
    native_unity = src.get("native_unity_animations") or src.get("unity_animations")
    if isinstance(native_unity, Mapping):
        out["native_unity_animations"] = copy.deepcopy(dict(native_unity))
    physics = src.get("physics")
    if isinstance(physics, Mapping):
        out["physics"] = copy.deepcopy(dict(physics))
    secondary_motion = (
        src.get("secondary_motion")
        or src.get("secondaryMotion")
        or (physics.get("secondary_motion") if isinstance(physics, Mapping) else None)
        or (physics.get("secondaryMotion") if isinstance(physics, Mapping) else None)
    )
    if isinstance(secondary_motion, Mapping):
        out["secondary_motion"] = copy.deepcopy(dict(secondary_motion))
    return out


def _merge_sidecar_preview_skin(
    file_mesh: Any,
    sidecar_meta: Mapping[str, Any],
) -> dict[str, Any]:
    mesh = copy.deepcopy(file_mesh) if isinstance(file_mesh, Mapping) else {}
    skin = sidecar_meta.get("preview_skin")
    if not isinstance(skin, list) or not skin:
        return mesh
    preview = mesh.get("preview") if isinstance(mesh.get("preview"), Mapping) else {}
    preview = dict(preview or {})
    vertices = preview.get("vertices")
    try:
        vertex_count = len(vertices) if isinstance(vertices, (list, tuple)) else len(skin)
    except Exception:
        vertex_count = len(skin)
    vertex_count = max(0, min(_MESH_PREVIEW_VERTEX_LIMIT, int(vertex_count or 0)))
    if vertex_count <= 0:
        return mesh
    merged_skin = copy.deepcopy(skin[:vertex_count])
    while len(merged_skin) < vertex_count:
        merged_skin.append([])
    preview["skin"] = merged_skin
    preview["skin_source"] = "sidecar"
    mesh["preview"] = preview
    return mesh


def _merge_sidecar_preview(
    file_mesh: Any,
    sidecar_meta: Mapping[str, Any],
) -> dict[str, Any]:
    mesh = _merge_sidecar_preview_skin(file_mesh, sidecar_meta)
    preview_mesh = sidecar_meta.get("preview_mesh")
    if not isinstance(preview_mesh, Mapping):
        return mesh
    preview = copy.deepcopy(dict(preview_mesh))
    skin = sidecar_meta.get("preview_skin")
    if isinstance(skin, list) and skin:
        vertex_count = len(preview.get("vertices") or ())
        merged_skin = copy.deepcopy(skin[:vertex_count])
        while len(merged_skin) < vertex_count:
            merged_skin.append([])
        preview["skin"] = merged_skin
        preview["skin_source"] = "sidecar"
    mesh = copy.deepcopy(mesh)
    mesh["preview"] = preview
    mesh["preview_source"] = "sidecar"
    return mesh


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
_FBX_TICKS_PER_SECOND = 46186158000.0


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


def _fbx_clean_name(value: Any) -> str:
    return str(value or "").split("::", 1)[-1].strip()


def _fbx_find_matching_brace(text: str, open_index: int) -> int:
    depth = 0
    for index in range(max(0, int(open_index or 0)), len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth <= 0:
                return index
    return len(text)


def _iter_fbx_objects(text: str, kind: str) -> list[tuple[int, str, str, str]]:
    pattern = re.compile(
        rf"\b{re.escape(kind)}:\s*(-?\d+)\s*,\s*\"([^\"]*)\"\s*,\s*\"([^\"]*)\"\s*{{",
        re.S,
    )
    out: list[tuple[int, str, str, str]] = []
    for match in pattern.finditer(text):
        try:
            object_id = int(match.group(1))
        except Exception:
            continue
        body_start = match.end() - 1
        body_end = _fbx_find_matching_brace(text, body_start)
        out.append((object_id, _fbx_clean_name(match.group(2)), str(match.group(3) or ""), text[body_start + 1:body_end]))
    return out


def _fbx_property_numbers(body: str, property_name: str, *, limit: int = 200000) -> list[float]:
    match = re.search(rf"\b{re.escape(property_name)}:\s*\*\d+\s*{{\s*a:\s*([^}}]*)}}", body, re.S)
    if match:
        return _numbers(match.group(1), limit=limit)
    match = re.search(rf"\b{re.escape(property_name)}:\s*([^\n}}]+)", body)
    return _numbers(match.group(1), limit=limit) if match else []


def _fbx_connections(text: str) -> list[tuple[str, int, int, str]]:
    out: list[tuple[str, int, int, str]] = []
    pattern = re.compile(r'C:\s*"([^"]+)"\s*,\s*(-?\d+)\s*,\s*(-?\d+)(?:\s*,\s*"([^"]+)")?')
    for match in pattern.finditer(text):
        try:
            out.append((str(match.group(1) or ""), int(match.group(2)), int(match.group(3)), str(match.group(4) or "")))
        except Exception:
            continue
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
    model_names_by_id: dict[int, str] = {}
    for match in re.finditer(r'Model:\s*[^"\n]*"([^"]+)"\s*,\s*"([^"]+)"', text):
        raw_name = str(match.group(1) or "")
        kind = str(match.group(2) or "")
        name = raw_name.split("::", 1)[-1].strip()
        if not name:
            continue
        nodes.append(name)
        if "limb" in kind.lower() or "bone" in kind.lower():
            bones.append(name)
    for object_id, name, kind, _body in _iter_fbx_objects(text, "Model"):
        if name:
            model_names_by_id[object_id] = name
            if name not in nodes:
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
    clip_keyframes = _fbx_animation_keyframes(text, model_names_by_id)
    clips = _merge_unique(clips, clip_keyframes.keys())

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
        "clip_keyframes": dict(clip_keyframes),
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


def _glb_skin_metadata(data: Mapping[str, Any]) -> list[dict[str, Any]]:
    nodes = data.get("nodes")
    skins = data.get("skins")
    accessors = data.get("accessors")
    if not isinstance(nodes, list) or not isinstance(skins, list):
        return []
    out: list[dict[str, Any]] = []
    for skin_index, skin in enumerate(skins):
        if not isinstance(skin, Mapping):
            continue
        raw_joints = skin.get("joints")
        if not isinstance(raw_joints, list):
            continue
        joints: list[dict[str, Any]] = []
        for raw_joint in raw_joints:
            joint_index = _int_at(raw_joint, -1)
            if not (0 <= joint_index < len(nodes)):
                continue
            name = _node_name(nodes, joint_index)
            joints.append({
                "index": joint_index,
                "name": name,
                "canonical": _canonical_from_token(name),
            })
        inverse_accessor = _json_index(accessors, skin.get("inverseBindMatrices"))
        out.append({
            "index": skin_index,
            "name": str(skin.get("name") or f"skin_{skin_index}"),
            "skeleton": _node_name(nodes, skin.get("skeleton")),
            "joint_count": len(joints),
            "joints": joints,
            "canonical_joints": [item["canonical"] for item in joints if item.get("canonical")],
            "inverse_bind_accessor": _int_at(skin.get("inverseBindMatrices"), -1),
            "inverse_bind_count": _int_at(inverse_accessor.get("count")) if inverse_accessor else 0,
        })
    return out


def _glb_mesh_skin_joints(
    data: Mapping[str, Any],
    skin_meta: list[dict[str, Any]],
) -> dict[int, tuple[dict[str, Any], ...]]:
    nodes = data.get("nodes")
    if not isinstance(nodes, list) or not skin_meta:
        return {}
    skins_by_index = {
        int(skin.get("index")): tuple(copy.deepcopy(skin.get("joints") or ()))
        for skin in skin_meta
        if isinstance(skin, Mapping)
    }
    out: dict[int, tuple[dict[str, Any], ...]] = {}
    for node in nodes:
        if not isinstance(node, Mapping):
            continue
        mesh_index = _int_at(node.get("mesh"), -1)
        skin_index = _int_at(node.get("skin"), -1)
        if mesh_index < 0 or skin_index < 0 or mesh_index in out:
            continue
        joints = skins_by_index.get(skin_index)
        if joints:
            out[mesh_index] = joints
    return out


def _gltf_weight_value(value: Any, accessor: Mapping[str, Any]) -> float:
    try:
        weight = float(value)
    except Exception:
        return 0.0
    if bool(accessor.get("normalized")):
        component = _int_at(accessor.get("componentType"), -1)
        if component == 5121:
            weight = weight / 255.0
        elif component == 5123:
            weight = weight / 65535.0
        elif component == 5120:
            weight = max(0.0, weight / 127.0)
        elif component == 5122:
            weight = max(0.0, weight / 32767.0)
    return max(0.0, weight)


def _glb_vertex_skin(
    attrs: Mapping[str, Any],
    accessors: Any,
    buffer_views: Any,
    binary: bytes,
    *,
    limit: int,
    skin_joints: tuple[dict[str, Any], ...],
    errors: list[str],
) -> list[list[dict[str, Any]]]:
    if not skin_joints:
        return []
    joint_accessor = _json_index(accessors, attrs.get("JOINTS_0"))
    weight_accessor = _json_index(accessors, attrs.get("WEIGHTS_0"))
    if joint_accessor is None or weight_accessor is None:
        return []
    joint_rows, _total_joints, joint_err = _read_accessor_values(
        joint_accessor, buffer_views, binary, limit=limit)
    weight_rows, _total_weights, weight_err = _read_accessor_values(
        weight_accessor, buffer_views, binary, limit=limit)
    if joint_err:
        errors.append(f"skin joints: {joint_err}")
    if weight_err:
        errors.append(f"skin weights: {weight_err}")
    count = min(len(joint_rows), len(weight_rows), max(0, int(limit or 0)))
    out: list[list[dict[str, Any]]] = []
    for row_index in range(count):
        entries: list[dict[str, Any]] = []
        joints = joint_rows[row_index]
        weights = weight_rows[row_index]
        for slot in range(min(4, len(joints), len(weights))):
            joint_slot = int(joints[slot])
            if not (0 <= joint_slot < len(skin_joints)):
                continue
            weight = _gltf_weight_value(weights[slot], weight_accessor)
            if weight <= 0.000001:
                continue
            joint = skin_joints[joint_slot]
            canonical = str(joint.get("canonical") or "").strip()
            source = str(joint.get("name") or "").strip()
            entries.append({
                "joint": canonical or source,
                "source": source,
                "weight": weight,
            })
        total = sum(float(item.get("weight") or 0.0) for item in entries)
        if total > 0.000001:
            for item in entries:
                item["weight"] = float(item.get("weight") or 0.0) / total
        out.append(entries)
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
    skin_meta = _glb_skin_metadata(data)
    mesh_skin_joints = _glb_mesh_skin_joints(data, skin_meta)
    bone_names = [name for name in nodes if _canonical_from_token(name)]
    vertices: list[list[float]] = []
    faces: list[list[int]] = []
    vertex_skin: list[list[dict[str, Any]]] = []
    bbox_points: list[tuple[float, float, float]] = []
    vertex_count = 0
    face_count = 0
    for mesh_index, mesh in enumerate(meshes):
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
            skin = _glb_vertex_skin(
                attrs, accessors, buffer_views, binary,
                limit=len(points),
                skin_joints=mesh_skin_joints.get(mesh_index, ()),
                errors=errors,
            )
            if skin:
                vertex_skin.extend(skin + ([[]] * max(0, len(points) - len(skin))))
            else:
                vertex_skin.extend([[] for _ in points])

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
    if any(vertex_skin):
        preview["skin"] = vertex_skin[:len(vertices)]
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
        "skins": copy.deepcopy(skin_meta),
        "bone_names": _merge_unique(
            bone_names,
            rest_positions.keys(),
            *(skin.get("canonical_joints") for skin in skin_meta),
        ),
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


def _has_model_content(meta: Mapping[str, Any]) -> bool:
    mesh = meta.get("mesh") if isinstance(meta.get("mesh"), Mapping) else {}
    try:
        if int(mesh.get("vertex_count") or 0) > 0 or int(mesh.get("face_count") or 0) > 0:
            return True
    except Exception:
        pass
    return bool(
        meta.get("nodes")
        or meta.get("bone_names")
        or meta.get("clips")
        or meta.get("clip_keyframes")
    )


def _has_imported_mesh_or_motion(meta: Mapping[str, Any]) -> bool:
    mesh = meta.get("mesh") if isinstance(meta.get("mesh"), Mapping) else {}
    try:
        if int(mesh.get("vertex_count") or 0) > 0 or int(mesh.get("face_count") or 0) > 0:
            return True
    except Exception:
        pass
    return bool(meta.get("bone_names") or meta.get("clips") or meta.get("clip_keyframes"))


def _parse_assimpnet_metadata(path: Path) -> dict[str, Any]:
    try:
        from render.model3d_assimpnet import import_model_metadata
    except Exception as exc:
        return {
            "mesh": {"source": "assimpnet", "vertex_count": 0, "face_count": 0, "bbox": {}},
            "nodes": (),
            "materials": (),
            "errors": (str(exc),),
        }
    return import_model_metadata(path)


def _read_model_file_metadata(path: Path, fmt: str, exists: bool) -> dict[str, Any]:
    if not exists or not str(path):
        return {}
    suffix = path.suffix.lower()
    chosen = fmt if fmt and fmt != "auto" else suffix.lstrip(".")
    try:
        if chosen in {"assimp", "assimpnet"}:
            return _parse_assimpnet_metadata(path)
        if chosen == "obj" or suffix == ".obj":
            return _parse_obj_metadata(path)
        if chosen == "fbx" or suffix == ".fbx":
            assimp_meta = _parse_assimpnet_metadata(path)
            if _has_imported_mesh_or_motion(assimp_meta):
                return assimp_meta
            ascii_meta = _parse_ascii_fbx_metadata(path)
            if _has_model_content(ascii_meta):
                return ascii_meta
            return assimp_meta
        if chosen == "gltf" or suffix == ".gltf":
            return _parse_gltf_metadata(path)
        if chosen == "glb" or suffix == ".glb":
            return _parse_glb_metadata(path)
        assimp_meta = _parse_assimpnet_metadata(path)
        if _has_model_content(assimp_meta):
            return assimp_meta
        return assimp_meta
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


def _action_json_text_cache_token(text: str) -> str:
    if len(text) <= _ACTION_JSON_PARSE_LIMIT:
        return _hash_text(text)
    prefix = text[:_ACTION_JSON_CACHE_PREFIX_LIMIT]
    return f"oversize:{len(text)}:{_hash_text(prefix)}"


def _action_json_oversize_message(source: str, size: int) -> str:
    return (
        f"{source} exceeds action json parse limit "
        f"({size} > {_ACTION_JSON_PARSE_LIMIT})"
    )


def _parse_action_json_text(text: str, source: str) -> tuple[dict[str, Any], list[str]]:
    token = _action_json_text_cache_token(text)
    cache_key = (source, token)
    cached = _ACTION_JSON_TEXT_CACHE.get(cache_key)
    if cached is not None:
        cached_data, cached_errors = cached
        return dict(cached_data), list(cached_errors)
    data: dict[str, Any] = {}
    errors: list[str] = []
    if len(text) > _ACTION_JSON_PARSE_LIMIT:
        errors.append(_action_json_oversize_message(source, len(text)))
    else:
        try:
            loaded = json.loads(text)
            if isinstance(loaded, Mapping):
                data.update(dict(loaded))
            else:
                errors.append(f"{source} must decode to an object")
        except Exception as exc:
            errors.append(str(exc))
    if len(_ACTION_JSON_TEXT_CACHE) >= _CACHE_LIMIT:
        _ACTION_JSON_TEXT_CACHE.clear()
    _ACTION_JSON_TEXT_CACHE[cache_key] = (dict(data), tuple(errors))
    return data, errors


def _unity_anim_signature(path: Path) -> tuple[str, bool, int, int]:
    exists, size, mtime_ns = _file_signature(path)
    return str(path), exists, size, mtime_ns


def _unity_yaml_documents(text: str) -> list[Any]:
    lines: list[str] = []
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("%YAML") or stripped.startswith("%TAG"):
            continue
        if stripped.startswith("--- !u!"):
            lines.append("---")
            continue
        lines.append(line)
    try:
        import yaml  # type: ignore[import-not-found]

        return [doc for doc in yaml.safe_load_all("\n".join(lines)) if isinstance(doc, Mapping)]
    except Exception:
        return []


def _curve_keys(raw_curve: Any) -> list[dict[str, float]]:
    if not isinstance(raw_curve, Mapping):
        return []
    curve = raw_curve.get("curve") if isinstance(raw_curve.get("curve"), Mapping) else raw_curve
    raw_keys = curve.get("m_Curve") if isinstance(curve, Mapping) else None
    if not isinstance(raw_keys, (list, tuple)):
        return []
    keys: list[dict[str, float]] = []
    for raw_key in raw_keys:
        if not isinstance(raw_key, Mapping):
            continue
        try:
            time_value = float(raw_key.get("time") or 0.0)
            value = float(raw_key.get("value") or 0.0)
        except Exception:
            continue
        if math.isfinite(time_value) and math.isfinite(value):
            keys.append({"time": time_value, "value": value})
    keys.sort(key=lambda item: item["time"])
    return keys


def _sample_float_keys(keys: list[dict[str, float]], phase: float) -> tuple[float, float]:
    if not keys:
        return 0.0, 0.0
    duration = max(0.0, float(keys[-1].get("time") or 0.0))
    sample_time = float(phase or 0.0)
    if duration > 0.000001:
        sample_time = sample_time % duration
    previous = keys[0]
    for current in keys[1:]:
        current_time = float(current.get("time") or 0.0)
        if sample_time <= current_time + 0.000001:
            prev_time = float(previous.get("time") or 0.0)
            span = max(0.000001, current_time - prev_time)
            amount = max(0.0, min(1.0, (sample_time - prev_time) / span))
            value = float(previous.get("value") or 0.0) * (1.0 - amount) + float(current.get("value") or 0.0) * amount
            return sample_time, value
        previous = current
    return sample_time, float(keys[-1].get("value") or 0.0)


def _read_unity_anim(path: Path) -> dict[str, Any]:
    signature = _unity_anim_signature(path)
    cached = _UNITY_ANIM_CACHE.get(signature)
    if cached is not None:
        return copy.deepcopy(cached)
    if not signature[1]:
        return {"source": "unity_anim_yaml", "resolved_path": str(path), "exists": False, "curves": (), "errors": ("missing",)}
    try:
        text = path.read_text(encoding="utf-8")
    except Exception as exc:
        return {"source": "unity_anim_yaml", "resolved_path": str(path), "exists": False, "curves": (), "errors": (str(exc),)}
    clip_doc: Mapping[str, Any] = {}
    for doc in _unity_yaml_documents(text):
        if isinstance(doc.get("AnimationClip"), Mapping):
            clip_doc = doc["AnimationClip"]
            break
    if not clip_doc:
        return {"source": "unity_anim_yaml", "resolved_path": str(path), "exists": True, "curves": (), "errors": ("AnimationClip missing",)}
    curves: list[dict[str, Any]] = []
    raw_curves = clip_doc.get("m_FloatCurves")
    if isinstance(raw_curves, (list, tuple)):
        for raw_curve in raw_curves:
            if not isinstance(raw_curve, Mapping):
                continue
            attribute = str(raw_curve.get("attribute") or "").strip()
            curve_path = str(raw_curve.get("path") or "").strip()
            if not attribute:
                continue
            keys = _curve_keys(raw_curve)
            if not keys:
                continue
            curves.append({
                "attribute": attribute,
                "path": curve_path,
                "class_id": int(raw_curve.get("classID") or 0),
                "keys": keys,
            })
    duration = max((float(item["keys"][-1]["time"]) for item in curves if item.get("keys")), default=0.0)
    parsed = {
        "source": "unity_anim_yaml",
        "resolved_path": str(path),
        "exists": True,
        "name": str(clip_doc.get("m_Name") or path.stem),
        "duration": duration,
        "curve_count": len(curves),
        "curves": tuple(curves),
        "errors": (),
    }
    if len(_UNITY_ANIM_CACHE) >= _CACHE_LIMIT:
        _UNITY_ANIM_CACHE.clear()
    _UNITY_ANIM_CACHE[signature] = copy.deepcopy(parsed)
    return parsed


def _model_asset_root(model_meta: Mapping[str, Any]) -> Path:
    sidecar = model_meta.get("sidecar") if isinstance(model_meta.get("sidecar"), Mapping) else {}
    sidecar_path = Path(str(sidecar.get("path") or ""))
    if str(sidecar_path):
        parent = sidecar_path.parent
        return parent.parent if parent.name.lower() == "fbx" else parent
    resolved = Path(str(model_meta.get("resolved_path") or ""))
    return resolved.parent.parent if str(resolved) and resolved.parent.name.lower() == "fbx" else resolved.parent


def _resolve_model_asset(model_meta: Mapping[str, Any], reference: str) -> Path:
    raw = Path(str(reference or "").strip())
    if raw.is_absolute():
        return raw
    return _model_asset_root(model_meta) / raw


def _unity_curve_name(curve: Mapping[str, Any]) -> str:
    attribute = str(curve.get("attribute") or "").strip()
    path = str(curve.get("path") or "").strip()
    return f"{path}/{attribute}" if path else attribute


def _unity_finger_bone(side: str, finger: str, segment: str) -> str:
    suffix = "_L" if side.lower().startswith("left") else "_R"
    segment_name = _UNITY_FINGER_SEGMENTS.get(str(segment or "1"), "Proximal")
    return f"{finger}{segment_name}{suffix}"


def _is_unity_finger_bone(name: str) -> bool:
    text = str(name or "")
    if not text.endswith(("_L", "_R")):
        return False
    return any(
        text.startswith(f"{finger}{segment}")
        for finger in ("Thumb", "Index", "Middle", "Ring", "Little")
        for segment in ("Proximal", "Intermediate", "Distal")
    )


def _clamped_muscle_value(value: Any) -> float:
    try:
        out = float(value)
    except Exception:
        return 0.0
    if not math.isfinite(out):
        return 0.0
    return max(-1.0, min(1.0, out))


def _unity_humanoid_muscle_rotations(
    sampled: Mapping[str, Any],
) -> dict[str, tuple[float, float, float, float]]:
    rotations: dict[str, tuple[float, float, float, float]] = {}
    if not isinstance(sampled, Mapping):
        return rotations
    for raw_name, raw_value in sampled.items():
        name = str(raw_name or "").rsplit("/", 1)[-1].strip()
        match = _UNITY_HAND_MUSCLE_RE.match(name)
        if not match:
            continue
        side, finger, segment, = match.group(1), match.group(2), match.group(3)
        value = _clamped_muscle_value(raw_value)
        side_sign = -1.0 if side.lower().startswith("left") else 1.0
        if segment:
            bone = _unity_finger_bone(side, finger, segment)
            limits = _UNITY_FINGER_CURL_LIMITS.get(finger, (1.20, 1.05, 0.90))
            limit = limits[max(0, min(2, int(segment) - 1))]
            curl = ((1.0 - value) * 0.5) * limit
            quat = _axis_angle_quat((0.0, 0.0, side_sign), curl)
        else:
            bone = _unity_finger_bone(side, finger, "1")
            spread_limit = _UNITY_FINGER_SPREAD_LIMITS.get(finger, 0.28)
            quat = _axis_angle_quat((0.0, side_sign, 0.0), value * spread_limit)
        rotations[bone] = _quat_mul(rotations.get(bone, (0.0, 0.0, 0.0, 1.0)), quat)
    return rotations


def _load_native_unity_action(
    model_meta: Mapping[str, Any],
    selected: Mapping[str, Any],
    phase: float,
) -> dict[str, Any]:
    raw_refs = selected.get("native_unity")
    if not isinstance(raw_refs, (list, tuple)):
        return {}
    refs = [str(item).strip() for item in raw_refs if str(item or "").strip()]
    clips: list[dict[str, Any]] = []
    sampled: dict[str, float] = {}
    blendshapes: dict[str, float] = {}
    errors: list[str] = []
    for ref in refs[:8]:
        path = _resolve_model_asset(model_meta, ref)
        parsed = _read_unity_anim(path)
        if parsed.get("errors"):
            errors.extend(f"{ref}: {item}" for item in parsed.get("errors") or ())
        curves = parsed.get("curves") if isinstance(parsed.get("curves"), (list, tuple)) else ()
        curve_summaries: list[dict[str, Any]] = []
        sample_time = 0.0
        for curve in curves:
            if not isinstance(curve, Mapping):
                continue
            keys = curve.get("keys") if isinstance(curve.get("keys"), list) else list(curve.get("keys") or ())
            sample_time, value = _sample_float_keys(keys, phase)
            name = _unity_curve_name(curve)
            sampled[name] = value
            if "blendshape." in name.lower():
                blendshapes[name] = value
            curve_summaries.append({
                "attribute": str(curve.get("attribute") or ""),
                "path": str(curve.get("path") or ""),
                "class_id": int(curve.get("class_id") or 0),
                "value": value,
            })
        clips.append({
            "source": "unity_anim_yaml",
            "path": ref,
            "resolved_path": str(path),
            "name": str(parsed.get("name") or Path(ref).stem),
            "duration": float(parsed.get("duration") or 0.0),
            "sample_time": sample_time,
            "curve_count": int(parsed.get("curve_count") or len(curve_summaries)),
            "curves": curve_summaries,
        })
    out: dict[str, Any] = {
        "native_unity": tuple(refs),
        "unity_clips": clips,
        "unity_float_curves": sampled,
        "unity_blendshapes": blendshapes,
    }
    muscle_rotations = _unity_humanoid_muscle_rotations(sampled)
    if muscle_rotations:
        out["unity_muscle_rotations"] = muscle_rotations
    if errors:
        out["unity_errors"] = tuple(errors)
    return out


def _has_keyframes(value: Any) -> bool:
    if not isinstance(value, Mapping):
        return False
    frames = value.get("keyframes")
    if not isinstance(frames, (list, tuple)):
        frames = value.get("frames")
    return isinstance(frames, (list, tuple)) and bool(frames)


def _has_explicit_motion(value: Any) -> bool:
    if not isinstance(value, Mapping):
        return False
    if _has_keyframes(value):
        return True
    for key in (
        "pose_offsets",
        "bone_offsets",
        "offsets",
        "pose_rotations",
        "bone_rotations",
        "rotations",
    ):
        item = value.get(key)
        if isinstance(item, Mapping) and item:
            return True
    return False


def _unity_motion_sample_info(selected: Mapping[str, Any]) -> dict[str, Any]:
    clips = selected.get("unity_clips")
    curves = selected.get("unity_float_curves")
    blendshapes = selected.get("unity_blendshapes")
    muscle_rotations = selected.get("unity_muscle_rotations")
    clip_count = len(clips) if isinstance(clips, (list, tuple)) else 0
    curve_count = len(curves) if isinstance(curves, Mapping) else 0
    blendshape_count = len(blendshapes) if isinstance(blendshapes, Mapping) else 0
    muscle_rotation_count = len(muscle_rotations) if isinstance(muscle_rotations, Mapping) else 0
    if clip_count <= 0 and curve_count <= 0 and blendshape_count <= 0 and muscle_rotation_count <= 0:
        return {}
    return {
        "motion_source": "unity_anim",
        "unity_clip_count": clip_count,
        "unity_curve_count": curve_count,
        "unity_blendshape_count": blendshape_count,
        "unity_muscle_rotation_count": muscle_rotation_count,
        "unity_clips": tuple(
            str(item.get("name") or item.get("path") or "")
            for item in list(clips or [])[:8]
            if isinstance(item, Mapping)
        ) if isinstance(clips, (list, tuple)) else (),
    }


def _clip_match_tokens(value: Any) -> set[str]:
    text = str(value or "").strip()
    if not text:
        return set()
    candidates = {text}
    for part in re.split(r"[|:/\\]+", text):
        part = part.strip()
        if part:
            candidates.add(part)
    out: set[str] = set()
    for candidate in candidates:
        normalized = _normalize_action_name(candidate)
        compact = "".join(ch for ch in normalized if ch.isalnum())
        if normalized:
            out.add(normalized)
        if compact:
            out.add(compact)
    return out


def _select_clip_keyframes(model_meta: Mapping[str, Any], action_name: str, clip_name: Any) -> tuple[str, dict[str, Any]]:
    clips = model_meta.get("clip_keyframes")
    if not isinstance(clips, Mapping):
        return "", {}
    requested = [str(action_name or "")]
    clip_text = str(clip_name or "").strip()
    if clip_text:
        requested.insert(0, clip_text)
    normalized: set[str] = set()
    for name in requested:
        normalized.update(_clip_match_tokens(name))
    for raw_name, raw_clip in clips.items():
        clip_key = str(raw_name or "").strip()
        if not clip_key or not (_clip_match_tokens(clip_key) & normalized):
            continue
        if isinstance(raw_clip, Mapping):
            selected = copy.deepcopy(dict(raw_clip))
            selected.setdefault("clip", clip_key)
            selected.setdefault("name", clip_key)
            return clip_key, selected
    if len(clips) == 1:
        raw_name, raw_clip = next(iter(clips.items()))
        clip_key = str(raw_name or "").strip()
        if clip_key and isinstance(raw_clip, Mapping):
            selected = copy.deepcopy(dict(raw_clip))
            selected.setdefault("clip", clip_key)
            selected.setdefault("name", clip_key)
            return clip_key, selected
    return "", {}


def _external_motion_metadata(path: Path, exists: bool) -> dict[str, Any]:
    if not exists or not str(path):
        return {}
    sidecar_path, sidecar_signature = _model_sidecar_signature(path, exists)
    sidecar_meta = _read_model_sidecar(sidecar_path, bool(sidecar_signature[1]))
    file_meta = _read_model_file_metadata(path, "auto", exists)
    clips = _merge_unique(sidecar_meta.get("clips"), file_meta.get("clips"))
    metadata = {
        "path": str(path),
        "resolved_path": str(path),
        "sidecar": dict(sidecar_meta),
        "clips": clips,
        "clip_keyframes": copy.deepcopy(file_meta.get("clip_keyframes") or {}),
        "native_unity_animations": copy.deepcopy(sidecar_meta.get("native_unity_animations") or {}),
        "errors": _merge_unique(sidecar_meta.get("errors"), file_meta.get("errors")),
    }
    return metadata


def _load_external_motion_action(
    action_path: Path,
    model_meta: Mapping[str, Any],
    selected: Mapping[str, Any],
    action_name: str,
    phase: float,
) -> tuple[str, dict[str, Any]]:
    suffix = action_path.suffix.lower()
    if suffix == ".anim":
        merged = dict(selected)
        refs = [str(action_path)]
        existing = merged.get("native_unity")
        if isinstance(existing, (list, tuple)):
            refs.extend(str(item).strip() for item in existing if str(item or "").strip())
        deduped: list[str] = []
        seen: set[str] = set()
        for ref in refs:
            if ref and ref not in seen:
                seen.add(ref)
                deduped.append(ref)
        merged["native_unity"] = deduped
        unity_action = _load_native_unity_action(model_meta, merged, phase)
        if unity_action:
            merged.update(unity_action)
            merged.setdefault("clip", str(action_path.stem))
            merged.setdefault("name", str(action_path.stem))
            return str(action_path.stem), merged
        return "", {}
    motion_meta = _external_motion_metadata(action_path, action_path.is_file())
    clip_key, motion_selected = _select_clip_keyframes(motion_meta, action_name, selected.get("clip"))
    if motion_selected:
        merged = _merge_action_config(selected, motion_selected)
        merged.setdefault("source_file", str(action_path))
        return clip_key, merged
    return "", {}


def _selected_action_motion_file(selected: Mapping[str, Any]) -> str:
    for key in ("file", "motion_file", "animation_file"):
        text = str(selected.get(key) or "").strip()
        if text:
            return text
    return ""


def clear_model3d_metadata_caches() -> None:
    """Clear model3d probe/metadata caches for focused tests."""

    global _BACKEND_STATUS_CACHE
    _BACKEND_STATUS_CACHE = None
    _MODEL_METADATA_CACHE.clear()
    _ACTION_METADATA_CACHE.clear()
    _ACTION_JSON_TEXT_CACHE.clear()
    _RETARGET_PLAN_CACHE.clear()
    _UNITY_ANIM_CACHE.clear()


def _get_model_metadata_cached(node: Mapping[str, Any], *, copy_result: bool) -> dict[str, Any]:
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
        return _copy_metadata(cached) if copy_result else cached
    sidecar_meta = _read_model_sidecar(sidecar_path, bool(sidecar_signature[1]))
    file_meta = _read_model_file_metadata(resolved, fmt, exists)
    bone_names = _merge_unique(sidecar_meta.get("bone_names"), file_meta.get("bone_names"))
    clips = _merge_unique(sidecar_meta.get("clips"), file_meta.get("clips"))
    errors = _merge_unique(sidecar_meta.get("errors"), file_meta.get("errors"))
    clip_keyframes = copy.deepcopy(file_meta.get("clip_keyframes") or {})
    file_rest = dict(file_meta.get("rest_positions") or {}) if isinstance(file_meta.get("rest_positions"), Mapping) else {}
    sidecar_rest = dict(sidecar_meta.get("rest_positions") or {}) if isinstance(sidecar_meta.get("rest_positions"), Mapping) else {}
    rest_positions = dict(sidecar_rest)
    rest_positions.update(file_rest)
    file_parents = dict(file_meta.get("node_parents") or {}) if isinstance(file_meta.get("node_parents"), Mapping) else {}
    sidecar_parents = dict(sidecar_meta.get("node_parents") or {}) if isinstance(sidecar_meta.get("node_parents"), Mapping) else {}
    node_parents = dict(sidecar_parents)
    node_parents.update(file_parents)
    rest_source = ""
    if file_rest:
        rest_source = str(file_meta.get("rest_positions_source") or "model")
    elif sidecar_rest:
        rest_source = "sidecar"
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
        "mesh": _merge_sidecar_preview(file_meta.get("mesh") or {}, sidecar_meta),
        "skins": copy.deepcopy(file_meta.get("skins") or []),
        "nodes": tuple(file_meta.get("nodes") or ()),
        "materials": tuple(file_meta.get("materials") or ()),
        "material_textures": copy.deepcopy(file_meta.get("material_textures") or {}),
        "embedded_textures": copy.deepcopy(file_meta.get("embedded_textures") or {}),
        "materials_config": copy.deepcopy(sidecar_meta.get("materials_config") or {}),
        "native_unity_animations": copy.deepcopy(sidecar_meta.get("native_unity_animations") or {}),
        "physics": copy.deepcopy(sidecar_meta.get("physics") or {}),
        "secondary_motion": copy.deepcopy(sidecar_meta.get("secondary_motion") or {}),
        "bone_names": bone_names,
        "clips": clips,
        "clip_keyframes": dict(clip_keyframes) if isinstance(clip_keyframes, Mapping) else {},
        "rest_positions": rest_positions,
        "node_parents": node_parents,
        "rest_positions_source": rest_source,
        "metadata_errors": errors,
        "cache_key": cache_key,
    }
    _cache_put(_MODEL_METADATA_CACHE, cache_key, metadata)
    return _copy_metadata(metadata) if copy_result else metadata


def get_model_metadata(node: Mapping[str, Any]) -> dict[str, Any]:
    """Return a caller-owned copy of offline metadata for a model3d model file."""

    return _get_model_metadata_cached(node, copy_result=True)


def get_model_metadata_view(node: Mapping[str, Any]) -> dict[str, Any]:
    """Return cached metadata for internal read-only hot paths.

    The returned object is shared with the metadata cache.  Rendering and
    retargeting code must treat it as immutable; public/plugin-facing callers
    should continue to use :func:`get_model_metadata`.
    """

    return _get_model_metadata_cached(node, copy_result=False)


def _get_action_metadata_cached(node: Mapping[str, Any], *, copy_result: bool) -> dict[str, Any]:
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
    json_error = str(action.get("json_error") or "").strip()
    action_file = str(action.get("file") or "").strip()
    action_path = resolve_action_path(action_file, node) if action_file else Path("")
    file_exists, file_size, file_mtime_ns = _file_signature(action_path) if action_file else (False, 0, 0)
    model_meta = get_model_metadata_view(node)
    model_cache_key = tuple(model_meta.get("cache_key") or ())
    try:
        action_time = float(action.get("time", node.get("phase", 0.0)) or 0.0)
    except Exception:
        action_time = 0.0
    procedural_source_hash = _hash_action_source(
        node.get("procedural_action")
        if node.get("procedural_action") is not None
        else action.get("procedural_action")
        if isinstance(action, Mapping) and action.get("procedural_action") is not None
        else action.get("procedural_action_json")
        if isinstance(action, Mapping)
        else None
    )

    data: dict[str, Any] = dict(inline)
    errors: list[str] = []
    if json_error:
        errors.append(json_error)
    if json_text:
        loaded_data, loaded_errors = _parse_action_json_text(json_text, "action json_text")
        data.update(loaded_data)
        errors.extend(loaded_errors)
    if action_file:
        if file_exists:
            if _is_json_action_file(action_path, action_file):
                if file_size > _ACTION_JSON_PARSE_LIMIT:
                    errors.append(_action_json_oversize_message("action file", int(file_size)))
                else:
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
    top_level_is_json = bool(action_file and _is_json_action_file(action_path, action_file))
    selected_motion_file = _selected_action_motion_file(selected) if (not action_file or top_level_is_json) else ""
    motion_file = (
        action_file
        if action_file and not top_level_is_json
        else selected_motion_file
    )
    motion_path = resolve_action_path(motion_file, node) if motion_file else Path("")
    motion_exists, motion_size, motion_mtime_ns = _file_signature(motion_path) if motion_file else (False, 0, 0)
    file_kind = "motion" if motion_file else "json" if top_level_is_json else ""
    cache_key = (
        name,
        _hash_mapping(inline) if inline else "",
        _action_json_text_cache_token(json_text) if json_text else "",
        str(action_path) if action_file else "",
        file_exists,
        file_size,
        file_mtime_ns,
        str(motion_path) if motion_file else "",
        motion_exists,
        motion_size,
        motion_mtime_ns,
        json_error,
        procedural_source_hash,
        model_cache_key,
        round(action_time, 4),
    )
    cached = _ACTION_METADATA_CACHE.get(cache_key)
    if cached is not None:
        return _copy_metadata(cached) if copy_result else cached
    external_motion_clip = ""
    procedural_fallback = False
    if file_kind == "motion" and motion_exists:
        external_motion_clip, external_selected = _load_external_motion_action(
            motion_path,
            model_meta,
            selected,
            name,
            action_time,
        )
        if external_selected:
            selected = external_selected
            data.setdefault(name, dict(selected))
    model_clip_name = ""
    if not _has_explicit_motion(selected):
        model_clip_name, model_selected = _select_clip_keyframes(model_meta, name, action.get("clip"))
        if model_selected:
            merged = dict(selected)
            merged.update(model_selected)
            selected = merged
            data.setdefault(name, dict(selected))
    if not _has_explicit_motion(selected):
        _procedural_root, procedural_selected = _selected_relative_action(node, name)
        if procedural_selected:
            selected = _merge_action_config(procedural_selected, selected)
            procedural_fallback = True
    elif not any(key in selected for key in ("native_unity", "unity_anim", "unity_animation")):
        _procedural_root, procedural_selected = _selected_relative_action(node, name)
        if procedural_selected:
            for key in ("native_unity", "unity_anim", "unity_animation"):
                if key in procedural_selected:
                    selected[key] = copy.deepcopy(procedural_selected[key])
    unity_action = _load_native_unity_action(model_meta, selected, action_time)
    if unity_action:
        selected.update(unity_action)
        data.setdefault(name, dict(selected))
    metadata = {
        "name": name,
        "data": dict(data),
        "selected": dict(selected or {}),
        "model_clip": model_clip_name,
        "external_motion_clip": external_motion_clip,
        "file": action_file,
        "resolved_file": str(action_path) if action_file else "",
        "file_kind": file_kind,
        "motion_file": motion_file if file_kind == "motion" else "",
        "resolved_motion_file": str(motion_path) if file_kind == "motion" else "",
        "motion_exists": bool(motion_exists) if file_kind == "motion" else False,
        "motion_format": motion_path.suffix.lower().lstrip(".") if file_kind == "motion" else "",
        "procedural_fallback": procedural_fallback,
        "errors": tuple(errors),
        "cache_key": cache_key,
    }
    if errors:
        metadata["selected"]["_load_error"] = "; ".join(errors)
    _cache_put(_ACTION_METADATA_CACHE, cache_key, metadata)
    return _copy_metadata(metadata) if copy_result else metadata


def get_action_metadata(node: Mapping[str, Any]) -> dict[str, Any]:
    """Return a caller-owned copy of action data and selected action config."""

    return _get_action_metadata_cached(node, copy_result=True)


def get_action_metadata_view(node: Mapping[str, Any]) -> dict[str, Any]:
    """Return cached action metadata for internal read-only hot paths."""

    return _get_action_metadata_cached(node, copy_result=False)


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
        "org-",
        "mch-",
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


def _canonical_alias_key(value: Any) -> str:
    norm = _normalize_bone_token(value)
    if not norm:
        return ""
    for canonical in HUMANOID_BONES:
        if norm == _normalize_bone_token(canonical):
            return canonical
        for alias in _BONE_ALIASES.get(canonical, ()):
            if norm == _normalize_bone_token(alias):
                return canonical
    return ""


def _iter_alias_items(value: Any) -> tuple[str, ...]:
    if isinstance(value, str):
        text = value.strip()
        return (text,) if text else ()
    if isinstance(value, Mapping):
        out: list[str] = []
        for key in ("name", "bone", "id", "alias"):
            text = str(value.get(key) or "").strip()
            if text:
                out.append(text)
        values = value.get("aliases") or value.get("names")
        if isinstance(values, (list, tuple, set)):
            out.extend(str(item or "").strip() for item in values)
        return tuple(item for item in out if item)
    if isinstance(value, (list, tuple, set)):
        out: list[str] = []
        for item in value:
            out.extend(_iter_alias_items(item))
        return tuple(out)
    return ()


def _declared_bone_aliases(
    node: Mapping[str, Any],
    model_meta: Mapping[str, Any],
) -> dict[str, tuple[str, ...]]:
    aliases: dict[str, list[str]] = {}
    skeleton = node.get("skeleton") if isinstance(node.get("skeleton"), Mapping) else {}
    retarget = node.get("retarget") if isinstance(node.get("retarget"), Mapping) else {}
    sidecar = model_meta.get("sidecar") if isinstance(model_meta.get("sidecar"), Mapping) else {}
    for source in (sidecar, skeleton, retarget):
        raw = None
        if isinstance(source, Mapping):
            raw = source.get("aliases") or source.get("bone_aliases")
        if not isinstance(raw, Mapping):
            continue
        for key, value in raw.items():
            canonical = _canonical_alias_key(key)
            if not canonical:
                continue
            bucket = aliases.setdefault(canonical, [])
            for alias in _iter_alias_items(value):
                if alias and alias not in bucket:
                    bucket.append(alias)
    return {key: tuple(value) for key, value in aliases.items()}


def _sidecar_bone_map(model_meta: Mapping[str, Any]) -> dict[str, str]:
    sidecar = model_meta.get("sidecar") if isinstance(model_meta.get("sidecar"), Mapping) else {}
    bone_map = sidecar.get("bone_map") if isinstance(sidecar.get("bone_map"), Mapping) else {}
    return {str(k): str(v) for k, v in bone_map.items()}


def _resolve_bone(
    canonical: str,
    request: str,
    names: list[str],
    index: Mapping[str, str],
    declared_aliases: Mapping[str, tuple[str, ...]] | None = None,
) -> tuple[str, str]:
    raw = str(request or "").strip()
    if raw and raw.lower() != "auto":
        norm = _normalize_bone_token(raw)
        return str(index.get(norm) or raw), "explicit"
    for alias in (
        canonical,
        *_BONE_ALIASES.get(canonical, ()),
        *((declared_aliases or {}).get(canonical, ())),
    ):
        norm = _normalize_bone_token(alias)
        if norm in index:
            source = "declared_alias" if alias in (declared_aliases or {}).get(canonical, ()) else "alias"
            return str(index[norm]), source
    if names:
        return "", "unresolved"
    return "", "pending_backend"


def _get_retarget_plan_cached(node: Mapping[str, Any], *, copy_result: bool) -> dict[str, Any]:
    """Build an offline humanoid retarget plan for a model3d node.

    The native renderer will eventually consume this plan directly.  Until then
    it gives script plugins deterministic diagnostics and lets users provide a
    sidecar skeleton map for arbitrary FBX/model files without loading Assimp.
    """

    retarget = node.get("retarget") if isinstance(node.get("retarget"), Mapping) else {}
    model_meta = get_model_metadata_view(node)
    skeleton = node.get("skeleton") if isinstance(node.get("skeleton"), Mapping) else {}
    cache_key = (
        tuple(model_meta.get("cache_key") or ()),
        _hash_mapping(retarget) if retarget else "",
        _hash_mapping(skeleton) if skeleton else "",
    )
    cached = _RETARGET_PLAN_CACHE.get(cache_key)
    if cached is not None:
        return _copy_retarget_plan(cached) if copy_result else cached

    sidecar_map = _sidecar_bone_map(model_meta)
    requests = dict(sidecar_map)
    requests.update(_explicit_bone_requests(node))
    bone_names = list(model_meta.get("bone_names") or [])
    index = _bone_index(bone_names)
    declared_aliases = _declared_bone_aliases(node, model_meta)

    resolved: dict[str, str] = {}
    sources: dict[str, str] = {}
    unresolved: list[str] = []
    pending_backend: list[str] = []
    for canonical in HUMANOID_BONES:
        mapped, source = _resolve_bone(
            canonical,
            requests.get(canonical, "auto"),
            bone_names,
            index,
            declared_aliases,
        )
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
    motion_scale_min = _float_from_mapping(retarget, "motion_scale_min", 0.25, lo=0.05, hi=1.0)
    motion_scale_max = _float_from_mapping(retarget, "motion_scale_max", 4.0, lo=1.0, hi=8.0)
    if motion_scale_min > motion_scale_max:
        motion_scale_min, motion_scale_max = motion_scale_max, motion_scale_min
    total = len(HUMANOID_BONES)
    coverage = len(resolved) / float(total) if total else 0.0
    plan = {
        "mode": str(retarget.get("mode") or "humanoid_auto"),
        "profile": str(retarget.get("profile") or retarget.get("mode") or "humanoid_auto"),
        "rest_pose": str(retarget.get("rest_pose") or "auto"),
        "preserve_proportions": _bool_from_mapping(retarget, "preserve_proportions", True),
        "adaptive_ik": _bool_from_mapping(retarget, "adaptive_ik", True),
        "adaptive_motion_scale": _bool_from_mapping(retarget, "adaptive_motion_scale", True),
        "prefer_model_clips": _bool_from_mapping(retarget, "prefer_model_clips", True),
        "stretch_limit": stretch_limit,
        "twist_limit": twist_limit,
        "motion_scale_min": motion_scale_min,
        "motion_scale_max": motion_scale_max,
        "bone_map": resolved,
        "sources": sources,
        "declared_aliases": declared_aliases,
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
    return _copy_retarget_plan(plan) if copy_result else plan


def get_retarget_plan(node: Mapping[str, Any]) -> dict[str, Any]:
    """Return a caller-owned humanoid retarget plan for a model3d node."""

    return _get_retarget_plan_cached(node, copy_result=True)


def get_retarget_plan_view(node: Mapping[str, Any]) -> dict[str, Any]:
    """Return cached retarget plan for internal read-only hot paths."""

    return _get_retarget_plan_cached(node, copy_result=False)


def _float_from_mapping(src: Mapping[str, Any], key: str, default: float,
                        *, lo: float, hi: float) -> float:
    try:
        value = float(src.get(key, default))
        if value != value:
            raise ValueError("nan")
    except Exception:
        value = default
    return max(lo, min(hi, value))


def _quat_normalize(q: tuple[float, float, float, float]) -> tuple[float, float, float, float]:
    length = math.sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3])
    if length <= 0.000001:
        return 0.0, 0.0, 0.0, 1.0
    return q[0] / length, q[1] / length, q[2] / length, q[3] / length


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


def _axis_angle_quat(
    axis: tuple[float, float, float],
    angle: float,
) -> tuple[float, float, float, float]:
    length = math.sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2])
    if length <= 0.000001 or abs(angle) <= 0.000001:
        return 0.0, 0.0, 0.0, 1.0
    half = float(angle) * 0.5
    scale = math.sin(half) / length
    return _quat_normalize((axis[0] * scale, axis[1] * scale, axis[2] * scale, math.cos(half)))


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


def _fbx_axis_from_property(value: Any) -> str:
    text = str(value or "").strip().lower()
    if "|" in text:
        text = text.rsplit("|", 1)[-1]
    if text.endswith("x"):
        return "x"
    if text.endswith("y"):
        return "y"
    if text.endswith("z"):
        return "z"
    return ""


def _fbx_curve_node_kind(name: str, property_name: str) -> str:
    text = f"{name} {property_name}".lower()
    if "translation" in text or text.rstrip().endswith(" t") or "::t" in text or "|t" in text:
        return "translation"
    if "rotation" in text or text.rstrip().endswith(" r") or "::r" in text or "|r" in text:
        return "rotation"
    return ""


def _fbx_euler_degrees_to_quat(x_deg: float, y_deg: float, z_deg: float) -> tuple[float, float, float, float]:
    # FBX exports commonly store local Euler channels in degrees.  XYZ order is
    # a conservative offline preview default; native providers can later apply
    # the exact model rotation order.
    hx = math.radians(float(x_deg or 0.0)) * 0.5
    hy = math.radians(float(y_deg or 0.0)) * 0.5
    hz = math.radians(float(z_deg or 0.0)) * 0.5
    sx, cx = math.sin(hx), math.cos(hx)
    sy, cy = math.sin(hy), math.cos(hy)
    sz, cz = math.sin(hz), math.cos(hz)
    return _quat_normalize((
        sx * cy * cz + cx * sy * sz,
        cx * sy * cz - sx * cy * sz,
        cx * cy * sz + sx * sy * cz,
        cx * cy * cz - sx * sy * sz,
    ))


def _fbx_curve_samples(body: str) -> list[tuple[float, float]]:
    times = _fbx_property_numbers(body, "KeyTime", limit=_MESH_PREVIEW_FACE_LIMIT)
    values = _fbx_property_numbers(body, "KeyValueFloat", limit=_MESH_PREVIEW_FACE_LIMIT)
    total = min(len(times), len(values))
    if total <= 0:
        return []
    out: list[tuple[float, float]] = []
    for index in range(total):
        raw_time = float(times[index])
        time_value = raw_time / _FBX_TICKS_PER_SECOND if abs(raw_time) > 1000000.0 else raw_time
        if not math.isfinite(time_value):
            time_value = float(index)
        value = float(values[index])
        if math.isfinite(value):
            out.append((time_value, value))
    return out


def _fbx_value_at(samples: list[tuple[float, float]], time_value: float) -> float:
    if not samples:
        return 0.0
    best = samples[0][1]
    for sample_time, value in samples:
        if sample_time > time_value + 0.000001:
            break
        best = value
    return best


def _fbx_stack_for_node(
    node_id: int,
    object_parents: Mapping[int, tuple[int, ...]],
    stack_names: Mapping[int, str],
) -> str:
    seen: set[int] = set()
    pending = [node_id]
    while pending:
        current = pending.pop(0)
        if current in seen:
            continue
        seen.add(current)
        if current in stack_names:
            return str(stack_names[current])
        pending.extend(object_parents.get(current, ()))
    return next(iter(stack_names.values()), "Take 001")


def _fbx_animation_keyframes(text: str, model_names_by_id: Mapping[int, str]) -> dict[str, dict[str, Any]]:
    curves = {
        object_id: _fbx_curve_samples(body)
        for object_id, _name, _kind, body in _iter_fbx_objects(text, "AnimationCurve")
    }
    curve_nodes = {
        object_id: name
        for object_id, name, _kind, _body in _iter_fbx_objects(text, "AnimationCurveNode")
    }
    if not curves or not curve_nodes:
        return {}

    stack_names = {
        object_id: (name or f"animation_{index}")
        for index, (object_id, name, _kind, _body) in enumerate(_iter_fbx_objects(text, "AnimationStack"))
    }
    if not stack_names:
        stack_names = {0: "Take 001"}

    object_parents: dict[int, list[int]] = {}
    curves_by_node: dict[int, dict[str, int]] = {}
    model_by_node: dict[int, tuple[int, str]] = {}
    for conn_type, src, dst, prop in _fbx_connections(text):
        if conn_type == "OO":
            object_parents.setdefault(src, []).append(dst)
            if src in curves and dst in curve_nodes:
                axis = _fbx_axis_from_property(prop)
                if axis:
                    curves_by_node.setdefault(dst, {})[axis] = src
            elif src in curve_nodes and dst in model_names_by_id:
                model_by_node[src] = (dst, prop)
        elif conn_type == "OP":
            if src in curves and dst in curve_nodes:
                axis = _fbx_axis_from_property(prop)
                if axis:
                    curves_by_node.setdefault(dst, {})[axis] = src
            elif src in curve_nodes and dst in model_names_by_id:
                model_by_node[src] = (dst, prop)

    clips_by_name: dict[str, dict[float, dict[str, dict[str, tuple[float, ...]]]]] = {}
    sample_counts: dict[str, int] = {}
    for node_id, axes in curves_by_node.items():
        model_entry = model_by_node.get(node_id)
        if model_entry is None:
            continue
        model_id, model_prop = model_entry
        canonical = _canonical_from_token(model_names_by_id.get(model_id, ""))
        if not canonical:
            continue
        kind = _fbx_curve_node_kind(curve_nodes.get(node_id, ""), model_prop)
        if kind not in {"translation", "rotation"}:
            continue
        clip_name = _fbx_stack_for_node(node_id, object_parents, stack_names)
        frames_by_time = clips_by_name.setdefault(clip_name, {})
        axis_samples = {axis: curves.get(curve_id, []) for axis, curve_id in axes.items()}
        times = sorted({time_value for samples in axis_samples.values() for time_value, _value in samples})
        if not times:
            continue
        base = {
            axis: (_fbx_value_at(samples, times[0]) if samples else 0.0)
            for axis, samples in axis_samples.items()
        }
        for time_value in times:
            x = _fbx_value_at(axis_samples.get("x", []), time_value) - float(base.get("x", 0.0))
            y = _fbx_value_at(axis_samples.get("y", []), time_value) - float(base.get("y", 0.0))
            z = _fbx_value_at(axis_samples.get("z", []), time_value) - float(base.get("z", 0.0))
            frame = frames_by_time.setdefault(time_value, {"offsets": {}, "rotations": {}})
            if kind == "translation":
                frame["offsets"][canonical] = (x, y, z)
            else:
                frame["rotations"][canonical] = _fbx_euler_degrees_to_quat(x, y, z)
        sample_counts[clip_name] = sample_counts.get(clip_name, 0) + sum(len(samples) for samples in axis_samples.values())

    out: dict[str, dict[str, Any]] = {}
    for clip_name, frames_by_time in clips_by_name.items():
        frames: list[dict[str, Any]] = []
        for time_value, groups in sorted(frames_by_time.items()):
            frame: dict[str, Any] = {"time": float(time_value)}
            offsets = groups.get("offsets") if isinstance(groups.get("offsets"), Mapping) else {}
            rotations = groups.get("rotations") if isinstance(groups.get("rotations"), Mapping) else {}
            if offsets:
                frame["bone_offsets"] = {bone: [float(x) for x in value] for bone, value in sorted(offsets.items())}
            if rotations:
                frame["bone_rotations"] = {bone: [float(x) for x in value] for bone, value in sorted(rotations.items())}
            if len(frame) > 1:
                frames.append(frame)
        if frames:
            out[str(clip_name)] = {
                "source": "fbx_ascii",
                "duration": max(float(frame["time"]) for frame in frames),
                "loop": True,
                "keyframes": frames,
                "sample_count": int(sample_counts.get(clip_name, 0)),
            }
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



def _default_procedural_action_root() -> dict[str, Any]:
    return {
        "schema": "sao.humanoid.procedural.v1",
        "enabled": True,
        "mode": "relative_ik_moe",
        "default_action": "moe_idle",
        "common": {
            "body": {"breathing": 0.014, "sway": 0.006, "cadence": 0.85},
            "head": {"look_at": [0.0, 1.42, 1.08], "weight": 0.12},
        },
        "actions": {
            "tomurai_idle": {
                "clip": "Idle",
                "body": {"breathing": 0.020, "sway": 0.010, "cadence": 0.72, "bounce": 0.006},
                "head": {"look_at": [0.0, 1.44, 1.10], "weight": 0.18},
                "effectors": {
                    "left_arm": {"offset": [0.16, -0.34, 0.03], "weight": 0.95},
                    "left_forearm": {"offset": [0.34, -0.68, 0.06], "weight": 1.00},
                    "left_hand": {"offset": [0.50, -0.88, 0.08], "weight": 1.00},
                    "right_arm": {"offset": [-0.16, -0.34, 0.03], "weight": 0.95},
                    "right_forearm": {"offset": [-0.34, -0.68, 0.06], "weight": 1.00},
                    "right_hand": {"offset": [-0.50, -0.88, 0.08], "weight": 1.00},
                },
            },
            "moe_idle": {
                "clip": "Idle",
                "body": {
                    "breathing": 0.022,
                    "sway": 0.016,
                    "cadence": 0.82,
                    "bounce": 0.010,
                    "lean": [0.010, 0.0, 0.012],
                },
                "head": {"look_at": [0.06, 1.45, 1.08], "weight": 0.24},
                "effectors": {
                    "left_arm": {"offset": [0.18, -0.38, 0.04], "weight": 0.95},
                    "left_forearm": {"offset": [0.38, -0.76, 0.08], "weight": 1.00},
                    "left_hand": {"offset": [0.56, -0.98, 0.10], "weight": 1.00, "wave": [0.0, 0.010, 0.0], "frequency": 1.2},
                    "right_arm": {"offset": [-0.18, -0.38, 0.04], "weight": 0.95},
                    "right_forearm": {"offset": [-0.38, -0.76, 0.08], "weight": 1.00},
                    "right_hand": {"offset": [-0.56, -0.98, 0.10], "weight": 1.00, "wave": [0.0, 0.012, 0.0], "frequency": 1.3, "phase": 0.4},
                },
            "bone_rotations": {
                "left_arm": [0.0, 0.0, -0.62, 0.785],
                "left_forearm": [0.0, 0.0, -0.40, 0.916],
                "right_arm": [0.0, 0.0, 0.62, 0.785],
                "right_forearm": [0.0, 0.0, 0.40, 0.916],
            },
            },
            "tap_react": {
                "effectors": {
                    "left_arm": {"offset": [0.06, -0.04, 0.02], "weight": 0.85},
                    "left_forearm": {"offset": [0.18, 0.06, 0.06], "weight": 1.00},
                    "left_hand": {"offset": [0.24, 0.14, 0.10], "weight": 1.00},
                    "right_arm": {"offset": [-0.06, -0.04, 0.02], "weight": 0.85},
                    "right_forearm": {"offset": [-0.18, 0.06, 0.06], "weight": 1.00},
                    "right_hand": {"offset": [-0.24, 0.14, 0.10], "weight": 1.00},
                },
                "bone_rotations": {
                    "left_arm": [0.0, 0.0, -0.26, 0.965],
                    "left_forearm": [0.0, 0.0, -0.18, 0.985],
                    "right_arm": [0.0, 0.0, 0.26, 0.965],
                    "right_forearm": [0.0, 0.0, 0.18, 0.985],
                },
            },
            "press_react": {
                "effectors": {
                    "left_hand": {"offset": [0.18, -0.12, 0.08], "weight": 1.0},
                    "right_hand": {"offset": [-0.18, -0.12, 0.08], "weight": 1.0},
                },
            },
            "drag_react": {
                "body": {"lean": [0.04, 0.0, 0.035], "sway": 0.022, "breathing": 0.016},
                "effectors": {
                    "left_arm": {"offset": [0.10, 0.04, 0.03], "weight": 0.90},
                    "left_forearm": {"offset": [0.26, 0.18, 0.08], "weight": 1.00},
                    "left_hand": {"offset": [0.34, 0.26, 0.12], "weight": 1.00},
                    "right_arm": {"offset": [-0.02, -0.10, 0.01], "weight": 0.75},
                    "right_forearm": {"offset": [-0.08, -0.20, 0.03], "weight": 0.80},
                    "right_hand": {"offset": [-0.10, -0.24, 0.04], "weight": 0.80},
                },
                "bone_rotations": {
                    "left_arm": [0.0, 0.0, -0.18, 0.982],
                    "left_forearm": [0.0, 0.0, -0.10, 0.995],
                    "right_arm": [0.0, 0.0, 0.52, 0.854],
                    "right_forearm": [0.0, 0.0, 0.30, 0.954],
                },
            },
            "drop_react": {
                "body": {"bounce": 0.026, "breathing": 0.020, "sway": 0.010},
                "effectors": {
                    "left_arm": {"offset": [0.04, -0.10, 0.02], "weight": 0.80},
                    "left_forearm": {"offset": [0.12, 0.00, 0.05], "weight": 0.95},
                    "left_hand": {"offset": [0.18, 0.10, 0.08], "weight": 1.00},
                    "right_arm": {"offset": [-0.04, -0.10, 0.02], "weight": 0.80},
                    "right_forearm": {"offset": [-0.12, 0.00, 0.05], "weight": 0.95},
                    "right_hand": {"offset": [-0.18, 0.10, 0.08], "weight": 1.00},
                },
                "bone_rotations": {
                    "left_arm": [0.0, 0.0, -0.30, 0.954],
                    "left_forearm": [0.0, 0.0, -0.16, 0.987],
                    "right_arm": [0.0, 0.0, 0.30, 0.954],
                    "right_forearm": [0.0, 0.0, 0.16, 0.987],
                },
            },
            "double_peace": {
                "head": {"look_at": [0.04, 1.48, 1.12], "weight": 0.22},
                "effectors": {
                    "left_arm": {"offset": [0.08, 0.05, 0.04], "weight": 0.85},
                    "left_forearm": {"offset": [0.22, 0.30, 0.08], "weight": 1.00},
                    "left_hand": {"offset": [0.32, 0.48, 0.12], "weight": 1.00},
                    "right_arm": {"offset": [-0.08, 0.05, 0.04], "weight": 0.85},
                    "right_forearm": {"offset": [-0.22, 0.30, 0.08], "weight": 1.00},
                    "right_hand": {"offset": [-0.32, 0.48, 0.12], "weight": 1.00},
                },
                "bone_rotations": {
                    "left_arm": [0.0, 0.0, 0.58, 0.815],
                    "left_forearm": [0.0, 0.0, 0.44, 0.898],
                    "left_hand": [0.0, 0.0, 0.16, 0.987],
                    "right_arm": [0.0, 0.0, -0.58, 0.815],
                    "right_forearm": [0.0, 0.0, -0.44, 0.898],
                    "right_hand": [0.0, 0.0, -0.16, 0.987],
                },
            },
            "shy_wave": {
                "body": {"lean": [0.018, 0.0, 0.012], "breathing": 0.020, "sway": 0.014},
                "head": {"look_at": [-0.04, 1.43, 1.04], "weight": 0.28},
                "effectors": {
                    "left_arm": {"offset": [0.12, -0.22, 0.03], "weight": 0.80},
                    "left_forearm": {"offset": [0.28, -0.44, 0.06], "weight": 0.85},
                    "left_hand": {"offset": [0.38, -0.60, 0.08], "weight": 0.90},
                    "right_arm": {"offset": [-0.08, 0.04, 0.04], "weight": 0.90},
                    "right_forearm": {"offset": [-0.26, 0.30, 0.08], "weight": 1.00, "wave": [0.02, 0.04, 0.0], "frequency": 4.0},
                    "right_hand": {"offset": [-0.38, 0.48, 0.12], "weight": 1.00, "wave": [0.04, 0.08, 0.0], "frequency": 4.0},
                },
                "bone_rotations": {
                    "left_arm": [0.0, 0.0, -0.36, 0.933],
                    "left_forearm": [0.0, 0.0, -0.22, 0.975],
                    "right_arm": [0.0, 0.0, -0.48, 0.877],
                    "right_forearm": [0.0, 0.0, -0.36, 0.933],
                    "right_hand": [0.0, 0.0, -0.18, 0.984],
                },
            },
            "wave": {
                "use_effectors": True,
                "body": {"breathing": 0.018, "sway": 0.010, "cadence": 0.86},
                "head": {"look_at": [-0.08, 1.40, 1.04], "weight": 0.24},
                "effectors": {
                    "right_arm": {"offset": [-0.04, 0.13, 0.02], "weight": 0.70},
                    "right_forearm": {"offset": [-0.13, 0.28, 0.04], "weight": 0.92, "wave": [0.025, 0.035, 0.0], "frequency": 3.2},
                    "right_hand": {"offset": [-0.22, 0.44, 0.04], "weight": 1.00, "wave": [0.045, 0.065, 0.0], "frequency": 3.2},
                },
            },
            "guard": {
                "body": {"crouch": 0.04, "breathing": 0.018},
                "effectors": {
                    "left_arm": {"offset": [0.06, -0.02, 0.04], "weight": 0.85},
                    "left_forearm": {"offset": [0.18, 0.12, 0.08], "weight": 1.00},
                    "left_hand": {"offset": [0.26, 0.22, 0.12], "weight": 1.00},
                    "right_arm": {"offset": [-0.06, -0.02, 0.04], "weight": 0.85},
                    "right_forearm": {"offset": [-0.18, 0.12, 0.08], "weight": 1.00},
                    "right_hand": {"offset": [-0.26, 0.22, 0.12], "weight": 1.00},
                },
                "bone_rotations": {
                    "left_arm": [0.0, 0.0, 0.32, 0.947],
                    "left_forearm": [0.0, 0.0, -0.58, 0.815],
                    "right_arm": [0.0, 0.0, -0.32, 0.947],
                    "right_forearm": [0.0, 0.0, 0.58, 0.815],
                },
            },
        },
    }


def _selected_relative_action(
    node: Mapping[str, Any],
    action_name: str,
) -> tuple[dict[str, Any], dict[str, Any]]:
    user_root = _procedural_action_root(node)
    if user_root and "enabled" in user_root and not _bool_from_mapping(user_root, "enabled", True):
        return dict(user_root), {}
    root = _merge_action_config(_default_procedural_action_root(), user_root) if user_root else _default_procedural_action_root()
    actions = root.get("actions") if isinstance(root.get("actions"), Mapping) else {}
    if not isinstance(actions, Mapping):
        actions = {}
    candidates: list[str] = []
    name = str(action_name or "").strip()
    if name:
        candidates.extend([name, _normalize_action_name(name)])
    default_action = str(root.get("default_action") or "").strip()
    if user_root and default_action:
        candidates.extend([default_action, _normalize_action_name(default_action)])
    if user_root:
        candidates.append("idle")
    selected: dict[str, Any] = {}
    for candidate in candidates:
        if not candidate:
            continue
        raw = actions.get(candidate)
        if raw is None:
            for key, value in actions.items():
                if _normalize_action_name(key) == _normalize_action_name(candidate):
                    raw = value
                    break
        if isinstance(raw, Mapping):
            selected = copy.deepcopy(dict(raw))
            selected.setdefault("name", candidate)
            break
    if not selected:
        if not user_root:
            return {}, {}
        selected = {}
    common = root.get("common") if isinstance(root.get("common"), Mapping) else {}
    merged = _merge_action_config(common, selected)
    mode = str(root.get("mode") or "").strip()
    if mode and "mode" not in merged:
        merged["mode"] = mode
    if user_root and "use_effectors" not in merged and "use_effector_ik" not in merged:
        profile = str(root.get("character_profile") or "").lower()
        if "tomurai" not in profile and "moe" not in mode.lower():
            merged["use_effectors"] = True
    return dict(root), merged


def _procedural_action_root(node: Mapping[str, Any]) -> dict[str, Any]:
    raw = node.get("procedural_action")
    if isinstance(raw, Mapping):
        return copy.deepcopy(dict(raw))
    if isinstance(raw, str):
        text = raw.strip()
        if not text:
            return {}
        try:
            parsed = json.loads(text)
        except Exception:
            return {}
        return copy.deepcopy(dict(parsed)) if isinstance(parsed, Mapping) else {}
    action = node.get("action") if isinstance(node.get("action"), Mapping) else {}
    for key in ("procedural_action", "procedural_action_json"):
        raw_nested = action.get(key) if isinstance(action, Mapping) else None
        if isinstance(raw_nested, Mapping):
            return copy.deepcopy(dict(raw_nested))
        if isinstance(raw_nested, str) and raw_nested.strip():
            try:
                parsed = json.loads(raw_nested)
            except Exception:
                continue
            if isinstance(parsed, Mapping):
                return copy.deepcopy(dict(parsed))
    return {}


def _merge_action_config(common: Mapping[str, Any], selected: Mapping[str, Any]) -> dict[str, Any]:
    merged = copy.deepcopy(dict(common or {}))
    for key, value in dict(selected or {}).items():
        existing = merged.get(key)
        if isinstance(existing, Mapping) and isinstance(value, Mapping):
            merged[key] = _merge_action_config(existing, value)
        else:
            merged[key] = copy.deepcopy(value)
    return merged



def _canonical_rest_positions(
    plan: Mapping[str, Any],
    model_meta: Mapping[str, Any],
) -> tuple[dict[str, tuple[float, float, float]], str, set[str]]:
    rest_positions = model_meta.get("rest_positions")
    rest_source = str(model_meta.get("rest_positions_source") or "").strip()
    if not isinstance(rest_positions, Mapping):
        sidecar = model_meta.get("sidecar") if isinstance(model_meta.get("sidecar"), Mapping) else {}
        rest_positions = sidecar.get("rest_positions") if isinstance(sidecar.get("rest_positions"), Mapping) else {}
        if isinstance(rest_positions, Mapping) and rest_positions:
            rest_source = "sidecar"
    bone_map = plan.get("bone_map") if isinstance(plan.get("bone_map"), Mapping) else {}
    out: dict[str, tuple[float, float, float]] = {}
    model_rest_bones: set[str] = set()
    source = "default"
    for canonical in HUMANOID_BONES:
        actual = str(bone_map.get(canonical) or "")
        point = None
        if isinstance(rest_positions, Mapping):
            point = _point3(rest_positions.get(actual)) or _point3(rest_positions.get(canonical))
        if point is not None:
            out[canonical] = point
            model_rest_bones.add(canonical)
            source = rest_source or "model"
        elif canonical in _DEFAULT_REST_POSITIONS:
            out[canonical] = _DEFAULT_REST_POSITIONS[canonical]
    if isinstance(rest_positions, Mapping):
        for raw_name, raw_point in rest_positions.items():
            name = str(raw_name or "").strip()
            point = _point3(raw_point)
            if name and point is not None and name not in out:
                out[name] = point
                source = rest_source or source
    return out, source, model_rest_bones



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
    model_meta = get_model_metadata_view(node)
    path_text = str(model_meta.get("path") or "")
    lines = [f"model3d {key}"]
    if path_text:
        lines.append(f"model: {Path(path_text).name or path_text}")
        if not bool(model_meta.get("exists")):
            lines.append("model file is missing")
    else:
        lines.append("model path is empty")
    if not bool(getattr(backend, "import_available", backend.files_present)):
        lines.append(backend.reason)
    retarget = node.get("retarget") if isinstance(node.get("retarget"), Mapping) else {}
    if retarget:
        plan = get_retarget_plan_view(node)
        if plan.get("mode"):
            lines.append(
                f"retarget {plan.get('mode')}: "
                f"{int(plan.get('resolved_count') or 0)}/{int(plan.get('total_count') or 0)} bones"
            )
        for warning in list(plan.get("warnings") or ())[:2]:
            lines.append(str(warning))
    return key, tuple(lines)


def get_model_data(node: Mapping[str, Any]) -> dict[str, Any]:
    """Return raw skeleton, mesh, and animation data for plugin-side computation.

    The C# plugin calls this once when the model changes to initialize its
    local skeletal animation engine.  All subsequent per-frame computation
    happens in C#; the platform only receives pre-computed bone transforms.
    """
    model_meta = get_model_metadata_view(node)
    if not bool(model_meta.get("exists")):
        return {"ok": False, "reason": "model missing"}

    plan = get_retarget_plan_view(node)
    rest, rest_source, model_rest_bones = _canonical_rest_positions(plan, model_meta)

    skeleton = {
        "bones": list(HUMANOID_BONES),
        "parents": dict(_HUMANOID_PARENT_BY_CHILD),
        "rest_positions": {
            bone: list(pos) for bone, pos in rest.items()
        },
        "model_rest_bones": sorted(model_rest_bones),
        "rest_source": rest_source,
        "bone_map": dict(plan.get("bone_map") or {}),
    }

    # Extract animation clips if available
    clips = {}
    action_meta = get_action_metadata_view(node)
    if isinstance(action_meta, Mapping):
        selected = action_meta.get("selected")
        if isinstance(selected, Mapping):
            clips["current"] = dict(selected)

    # Extract secondary motion chains from sidecar
    sidecar = model_meta.get("sidecar") if isinstance(model_meta.get("sidecar"), Mapping) else {}
    physics = sidecar.get("physics") if isinstance(sidecar.get("physics"), Mapping) else {}
    secondary_motion = physics.get("secondary_motion") if isinstance(physics.get("secondary_motion"), Mapping) else {}
    chains = secondary_motion.get("chains") if isinstance(secondary_motion.get("chains"), (list, tuple)) else []

    return {
        "ok": True,
        "skeleton": skeleton,
        "clips": clips,
        "secondary_motion_chains": list(chains),
        "model_path": str(model_meta.get("path") or ""),
        "format": str(model_meta.get("format") or ""),
    }


__all__ = [
    "Model3DBackendStatus",
    "clear_model3d_metadata_caches",
    "diagnose_model3d_node",
    "get_action_metadata",
    "get_action_metadata_view",
    "get_backend_status",
    "get_model_data",
    "get_model_metadata",
    "get_model_metadata_view",
    "get_retarget_plan",
    "get_retarget_plan_view",
    "HUMANOID_BONES",
    "HUMANOID_SEGMENTS",
    "model3d_node_key",
    "model3d_path",
    "resolve_action_path",
    "resolve_model_path",
    "probe_model3d_backend",
]
