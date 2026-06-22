# -*- coding: utf-8 -*-
"""Offline-safe model3d backend probe helpers.

This module intentionally does not download or initialize native render
dependencies. It only inspects the local bundle layout so plugin overlay load
can stay safe when model3d assets or AssimpNet files are absent.
"""
from __future__ import annotations

import hashlib
import json
import re
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
_BACKEND_STATUS_CACHE: Model3DBackendStatus | None = None
_MODEL_METADATA_CACHE: dict[tuple[Any, ...], dict[str, Any]] = {}
_ACTION_METADATA_CACHE: dict[tuple[Any, ...], dict[str, Any]] = {}

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
    for key in ("data", "selected", "model", "action", "sidecar", "retarget", "mesh"):
        item = out.get(key)
        if isinstance(item, Mapping):
            out[key] = dict(item)
    for key in ("bone_names", "clips", "warnings", "unresolved", "nodes", "materials", "metadata_errors"):
        item = out.get(key)
        if isinstance(item, tuple):
            out[key] = list(item)
    errors = out.get("errors")
    if isinstance(errors, tuple):
        out["errors"] = list(errors)
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


def _float_triplet(values: list[str]) -> tuple[float, float, float] | None:
    if len(values) < 3:
        return None
    try:
        return float(values[0]), float(values[1]), float(values[2])
    except Exception:
        return None


def _parse_obj_metadata(path: Path) -> dict[str, Any]:
    points: list[tuple[float, float, float]] = []
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
                    if len(line.split()) >= 4:
                        face_count += 1
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
    return {
        "mesh": {
            "source": "obj",
            "vertex_count": len(points),
            "face_count": face_count,
            "bbox": _bbox(points),
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
    pvi_match = re.search(r"PolygonVertexIndex:\s*\*\d+\s*{\s*a:\s*([^}]*)}", text, re.S)
    if pvi_match:
        for value in _NUMBER_RE.finditer(pvi_match.group(1)):
            try:
                if int(float(value.group(0))) < 0:
                    face_count += 1
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

    return {
        "mesh": {
            "source": "fbx_ascii",
            "vertex_count": len(points),
            "face_count": face_count,
            "bbox": _bbox(points),
        },
        "nodes": _unique_strings(nodes),
        "bone_names": _unique_strings(bones),
        "clips": _unique_strings(clips),
        "errors": tuple(errors),
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
    return {
        "mesh": {
            "source": "gltf",
            "mesh_count": mesh_count,
            "vertex_count": 0,
            "face_count": 0,
            "bbox": _bbox(bbox_points),
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
            return {
                "mesh": {"source": "glb", "vertex_count": 0, "face_count": 0, "bbox": {}},
                "nodes": (),
                "materials": (),
                "errors": ("glb metadata requires native backend",),
            }
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


def clear_model3d_metadata_caches() -> None:
    """Clear model3d probe/metadata caches for focused tests."""

    global _BACKEND_STATUS_CACHE
    _BACKEND_STATUS_CACHE = None
    _MODEL_METADATA_CACHE.clear()
    _ACTION_METADATA_CACHE.clear()


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
    cache_key = (
        name,
        _hash_mapping(inline) if inline else "",
        _hash_text(json_text) if json_text else "",
        str(action_path) if action_file else "",
        file_exists,
        file_size,
        file_mtime_ns,
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

    selected = data.get(name) if isinstance(data.get(name), Mapping) else {}
    file_kind = "json" if action_file and _is_json_action_file(action_path, action_file) else "motion" if action_file else ""
    metadata = {
        "name": name,
        "data": dict(data),
        "selected": dict(selected or {}),
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
    return {
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


def _float_from_mapping(src: Mapping[str, Any], key: str, default: float,
                        *, lo: float, hi: float) -> float:
    try:
        value = float(src.get(key, default))
        if value != value:
            raise ValueError("nan")
    except Exception:
        value = default
    return max(lo, min(hi, value))


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
    "get_action_metadata",
    "get_backend_status",
    "get_model_metadata",
    "get_retarget_plan",
    "HUMANOID_BONES",
    "model3d_node_key",
    "model3d_path",
    "resolve_action_path",
    "resolve_model_path",
    "probe_model3d_backend",
]
