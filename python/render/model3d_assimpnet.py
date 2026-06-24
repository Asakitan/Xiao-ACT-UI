# -*- coding: utf-8 -*-
"""AssimpNet-backed model importer for ``model3d`` metadata.

The bundle is intentionally minimal: one .NET Framework managed assembly, one
win-x64 native Assimp DLL, and license/package metadata.  Import is lazy so
normal overlay startup does not initialize CLR/native Assimp until a model file
needs full-format parsing.
"""
from __future__ import annotations

import math
import os
import hashlib
from pathlib import Path
from threading import RLock
from typing import Any, Mapping


_MESH_PREVIEW_VERTEX_LIMIT = 0
_MESH_PREVIEW_FACE_LIMIT = 0
_ASSIMP_LOCK = RLock()
_ASSIMP: Any = None
_DLL_HANDLES: list[Any] = []
_LOAD_ERROR = ""


def bundle_root() -> Path:
    return Path(__file__).resolve().parents[1] / "vendor" / "model3d" / "assimpnet"


def bundle_paths(root: Path | None = None) -> dict[str, Path]:
    base = Path(root) if root is not None else bundle_root()
    return {
        "root": base,
        "managed": base / "AssimpNet.dll",
        "native_dir": base / "runtimes" / "win-x64" / "native",
        "native": base / "runtimes" / "win-x64" / "native" / "assimp.dll",
        "license": base / "License.txt",
        "nuspec": base / "assimpnet.nuspec",
    }


def available(root: Path | None = None) -> bool:
    paths = bundle_paths(root)
    return paths["managed"].is_file() and paths["native"].is_file()


def load_error() -> str:
    return _LOAD_ERROR


def reset_for_tests() -> None:
    global _ASSIMP, _LOAD_ERROR
    with _ASSIMP_LOCK:
        _ASSIMP = None
        _LOAD_ERROR = ""


def _ensure_assimp(root: Path | None = None) -> Any:
    global _ASSIMP, _LOAD_ERROR
    with _ASSIMP_LOCK:
        if _ASSIMP is not None:
            return _ASSIMP
        paths = bundle_paths(root)
        if not paths["managed"].is_file() or not paths["native"].is_file():
            _LOAD_ERROR = "AssimpNet bundle is incomplete"
            return None
        try:
            if hasattr(os, "add_dll_directory"):
                _DLL_HANDLES.append(os.add_dll_directory(str(paths["native_dir"])))
            os.environ["PATH"] = str(paths["native_dir"]) + os.pathsep + os.environ.get("PATH", "")
            import clr  # type: ignore[import-not-found]

            clr.AddReference(str(paths["managed"]))
            import Assimp  # type: ignore[import-not-found]

            lib = Assimp.Unmanaged.AssimpLibrary.Instance
            lib.Resolver.SetProbingPaths64([str(paths["native_dir"])])
            lib.Resolver.SetOverrideLibraryName64(paths["native"].name)
            lib.LoadLibrary()
            _ASSIMP = Assimp
            _LOAD_ERROR = ""
            return _ASSIMP
        except Exception as exc:
            _LOAD_ERROR = str(exc)
            return None


def _finite_float(value: Any, default: float = 0.0) -> float:
    try:
        out = float(value)
    except Exception:
        return default
    return out if math.isfinite(out) else default


def _vec3(value: Any) -> tuple[float, float, float]:
    return (
        _finite_float(getattr(value, "X", 0.0)),
        _finite_float(getattr(value, "Y", 0.0)),
        _finite_float(getattr(value, "Z", 0.0)),
    )


def _uv2(value: Any) -> tuple[float, float]:
    return (
        _finite_float(getattr(value, "X", 0.0)),
        _finite_float(getattr(value, "Y", 0.0)),
    )


def _quat4(value: Any) -> tuple[float, float, float, float]:
    x = _finite_float(getattr(value, "X", 0.0))
    y = _finite_float(getattr(value, "Y", 0.0))
    z = _finite_float(getattr(value, "Z", 0.0))
    w = _finite_float(getattr(value, "W", 1.0), 1.0)
    length = math.sqrt(x * x + y * y + z * z + w * w)
    if length <= 0.000001 or not math.isfinite(length):
        return 0.0, 0.0, 0.0, 1.0
    return x / length, y / length, z / length, w / length


def _matrix_tuple(value: Any) -> tuple[float, ...]:
    return (
        _finite_float(getattr(value, "A1", 1.0), 1.0),
        _finite_float(getattr(value, "A2", 0.0)),
        _finite_float(getattr(value, "A3", 0.0)),
        _finite_float(getattr(value, "A4", 0.0)),
        _finite_float(getattr(value, "B1", 0.0)),
        _finite_float(getattr(value, "B2", 1.0), 1.0),
        _finite_float(getattr(value, "B3", 0.0)),
        _finite_float(getattr(value, "B4", 0.0)),
        _finite_float(getattr(value, "C1", 0.0)),
        _finite_float(getattr(value, "C2", 0.0)),
        _finite_float(getattr(value, "C3", 1.0), 1.0),
        _finite_float(getattr(value, "C4", 0.0)),
        _finite_float(getattr(value, "D1", 0.0)),
        _finite_float(getattr(value, "D2", 0.0)),
        _finite_float(getattr(value, "D3", 0.0)),
        _finite_float(getattr(value, "D4", 1.0), 1.0),
    )


def _mat4_mul(a: tuple[float, ...], b: tuple[float, ...]) -> tuple[float, ...]:
    out: list[float] = []
    for row in range(4):
        for col in range(4):
            out.append(sum(a[row * 4 + k] * b[k * 4 + col] for k in range(4)))
    return tuple(out)


def _translation_from_matrix(value: tuple[float, ...]) -> tuple[float, float, float]:
    return value[3], value[7], value[11]


def _transform_point(
    matrix: tuple[float, ...],
    point: tuple[float, float, float],
) -> tuple[float, float, float]:
    x, y, z = point
    return (
        matrix[0] * x + matrix[1] * y + matrix[2] * z + matrix[3],
        matrix[4] * x + matrix[5] * y + matrix[6] * z + matrix[7],
        matrix[8] * x + matrix[9] * y + matrix[10] * z + matrix[11],
    )


def _bbox(points: list[tuple[float, float, float]]) -> dict[str, list[float]]:
    if not points:
        return {}
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    zs = [p[2] for p in points]
    return {
        "min": [min(xs), min(ys), min(zs)],
        "max": [max(xs), max(ys), max(zs)],
    }


def _unique(values: Any) -> tuple[str, ...]:
    seen: set[str] = set()
    out: list[str] = []
    for value in values or ():
        text = str(value or "").strip()
        if text and text not in seen:
            seen.add(text)
            out.append(text)
    return tuple(out)


def _canonical_from_token(value: Any) -> str:
    try:
        from render import model3d_backend as backend

        fn = getattr(backend, "_canonical_from_token", None)
        if callable(fn):
            return str(fn(value) or "")
    except Exception:
        pass
    text = "".join(ch for ch in str(value or "").lower() if ch.isalnum())
    pairs = (
        ("righthand", "right_hand"),
        ("rightforearm", "right_forearm"),
        ("rightarm", "right_arm"),
        ("rightshoulder", "right_shoulder"),
        ("lefthand", "left_hand"),
        ("leftforearm", "left_forearm"),
        ("leftarm", "left_arm"),
        ("leftshoulder", "left_shoulder"),
        ("rightfoot", "right_foot"),
        ("rightleg", "right_knee"),
        ("rightupleg", "right_leg"),
        ("leftfoot", "left_foot"),
        ("leftleg", "left_knee"),
        ("leftupleg", "left_leg"),
        ("head", "head"),
        ("neck", "neck"),
        ("spine2", "chest"),
        ("chest", "chest"),
        ("spine", "spine"),
        ("hips", "hips"),
        ("pelvis", "hips"),
        ("root", "root"),
    )
    for needle, canonical in pairs:
        if needle in text:
            return canonical
    return ""


def _walk_nodes(
    node: Any,
    parent: tuple[float, ...],
    names: list[str],
    rest: dict[str, tuple[float, float, float]],
    parents: dict[str, str] | None = None,
    parent_name: str = "",
) -> None:
    if node is None:
        return
    name = str(getattr(node, "Name", "") or "").strip()
    local = _matrix_tuple(getattr(node, "Transform", None))
    world = _mat4_mul(parent, local)
    current_parent = parent_name
    if name:
        names.append(name)
        rest[name] = _translation_from_matrix(world)
        if parents is not None and parent_name:
            parents[name] = parent_name
        current_parent = name
    try:
        children = list(getattr(node, "Children", []) or [])
    except Exception:
        children = []
    for child in children:
        _walk_nodes(child, world, names, rest, parents, current_parent)


def _mesh_node_world_transforms(scene: Any) -> dict[int, tuple[float, ...]]:
    root = getattr(scene, "RootNode", None)
    if root is None:
        return {}
    identity = (
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0,
    )
    transforms: dict[int, tuple[float, ...]] = {}

    def walk(node: Any, parent: tuple[float, ...]) -> None:
        local = _matrix_tuple(getattr(node, "Transform", None))
        world = _mat4_mul(parent, local)
        try:
            mesh_indices = list(getattr(node, "MeshIndices", []) or [])
        except Exception:
            mesh_indices = []
        for raw_index in mesh_indices:
            try:
                mesh_index = int(raw_index)
            except Exception:
                continue
            transforms.setdefault(mesh_index, world)
        try:
            children = list(getattr(node, "Children", []) or [])
        except Exception:
            children = []
        for child in children:
            walk(child, world)

    walk(root, identity)
    return transforms


def _skin_influence_name(raw_name: str) -> str:
    canonical = _canonical_from_token(raw_name)
    return canonical or raw_name


def _net_bytes(value: Any) -> bytes:
    if value is None:
        return b""
    try:
        return bytes(value)
    except Exception:
        try:
            return bytes(int(value[index]) & 0xFF for index in range(len(value)))
        except Exception:
            return b""


def _embedded_textures(scene: Any) -> dict[str, dict[str, Any]]:
    textures: list[Any] = []
    try:
        textures = list(getattr(scene, "Textures", []) or [])
    except Exception:
        textures = []
    out: dict[str, dict[str, Any]] = {}
    for index, texture in enumerate(textures):
        try:
            compressed = bool(getattr(texture, "HasCompressedData", False))
        except Exception:
            compressed = False
        if not compressed:
            continue
        data = _net_bytes(getattr(texture, "CompressedData", None))
        if not data:
            continue
        fmt = str(getattr(texture, "CompressedFormatHint", "") or "").strip().lower()
        digest = hashlib.sha1(data).hexdigest()
        out[str(index)] = {
            "source": "assimpnet_embedded",
            "kind": "embedded",
            "index": index,
            "format": fmt or "bin",
            "sha1": digest,
            "size": len(data),
            "data": data,
        }
    return out


def _texture_slot_metadata(slot: Any, embedded: Mapping[str, Any]) -> dict[str, Any]:
    file_path = str(getattr(slot, "FilePath", "") or "").strip()
    texture_type = str(getattr(slot, "TextureType", "") or "").split(".")[-1]
    out: dict[str, Any] = {
        "source": "assimpnet_material_slot",
        "type": texture_type,
        "path": file_path,
        "uv_index": int(getattr(slot, "UVIndex", 0) or 0),
        "texture_index": int(getattr(slot, "TextureIndex", 0) or 0),
        "wrap_u": str(getattr(slot, "WrapModeU", "") or ""),
        "wrap_v": str(getattr(slot, "WrapModeV", "") or ""),
    }
    if file_path.startswith("*") and file_path[1:].isdigit():
        embedded_id = file_path[1:]
        out["kind"] = "embedded"
        out["embedded_id"] = embedded_id
        embedded_meta = embedded.get(embedded_id) if isinstance(embedded, Mapping) else None
        if isinstance(embedded_meta, Mapping):
            out["format"] = embedded_meta.get("format")
            out["sha1"] = embedded_meta.get("sha1")
            out["size"] = embedded_meta.get("size")
    elif file_path:
        out["kind"] = "file"
    else:
        out["kind"] = "unknown"
    return out


def _material_texture_metadata(scene: Any, material_names: tuple[str, ...]) -> tuple[dict[str, Any], dict[str, Any]]:
    embedded = _embedded_textures(scene)
    materials: list[Any] = []
    try:
        materials = list(getattr(scene, "Materials", []) or [])
    except Exception:
        materials = []
    material_textures: dict[str, Any] = {}
    for index, material in enumerate(materials):
        name = (
            material_names[index]
            if 0 <= index < len(material_names)
            else str(getattr(material, "Name", "") or f"material_{index}")
        )
        slots: list[Any] = []
        try:
            slots = list(material.GetAllMaterialTextures() or [])
        except Exception:
            try:
                slot = getattr(material, "TextureDiffuse", None)
                slots = [slot] if slot is not None and bool(getattr(material, "HasTextureDiffuse", False)) else []
            except Exception:
                slots = []
        parsed = [_texture_slot_metadata(slot, embedded) for slot in slots if slot is not None]
        parsed = [item for item in parsed if item.get("kind") in {"embedded", "file"}]
        if not parsed:
            continue
        diffuse = next((item for item in parsed if str(item.get("type") or "").lower().endswith("diffuse")), parsed[0])
        material_textures[str(name)] = {
            "source": "assimpnet",
            "base_color": diffuse,
            "textures": parsed,
        }
    return material_textures, embedded


def _spread_indices(count: int, target: int) -> list[int]:
    target = max(0, min(int(target), int(count)))
    if target <= 0 or count <= 0:
        return []
    if target == 1:
        return [0]
    seen: set[int] = set()
    out: list[int] = []
    span = max(1, count - 1)
    for slot in range(target):
        index = min(count - 1, int(round((slot * span) / float(target - 1))))
        if index not in seen:
            seen.add(index)
            out.append(index)
    return out


def _balanced_preview_face_indices(full_faces: list[tuple[list[int], str]]) -> list[int]:
    face_limit = len(full_faces) if _MESH_PREVIEW_FACE_LIMIT <= 0 else _MESH_PREVIEW_FACE_LIMIT
    if not full_faces or face_limit <= 0:
        return []
    material_order: list[str] = []
    grouped: dict[str, list[int]] = {}
    for face_index, (_face, material_name) in enumerate(full_faces):
        key = str(material_name or "").strip()
        if key not in grouped:
            grouped[key] = []
            material_order.append(key)
        grouped[key].append(face_index)
    per_material = max(1, face_limit // max(1, len(material_order)))
    material_candidates: list[list[int]] = []
    for material_name in material_order:
        members = grouped[material_name]
        local_indices = _spread_indices(len(members), min(len(members), per_material))
        material_candidates.append([members[index] for index in local_indices])

    out: list[int] = []
    seen: set[int] = set()
    max_group = max((len(items) for items in material_candidates), default=0)
    for slot in range(max_group):
        for candidates in material_candidates:
            if slot >= len(candidates):
                continue
            face_index = candidates[slot]
            if face_index in seen:
                continue
            seen.add(face_index)
            out.append(face_index)
            if len(out) >= face_limit:
                return out

    for face_index in range(len(full_faces)):
        if len(out) >= face_limit:
            break
        if face_index in seen:
            continue
        seen.add(face_index)
        out.append(face_index)
    return out


def _append_preview_vertex(
    global_index: int,
    full_vertices: list[tuple[float, float, float]],
    full_uvs: list[tuple[float, float] | None],
    full_skin: list[list[dict[str, Any]]],
    preview_vertices: list[list[float]],
    preview_uvs: list[list[float] | None],
    preview_skin: list[list[dict[str, Any]]],
    global_to_preview: dict[int, int],
) -> int:
    preview_index = global_to_preview.get(global_index)
    if preview_index is not None:
        return preview_index
    point = full_vertices[global_index]
    preview_index = len(preview_vertices)
    global_to_preview[global_index] = preview_index
    preview_vertices.append([point[0], point[1], point[2]])
    uv = full_uvs[global_index] if global_index < len(full_uvs) else None
    preview_uvs.append([uv[0], uv[1]] if uv is not None else None)
    preview_skin.append([dict(item) for item in full_skin[global_index]])
    return preview_index


def _build_mesh_preview(
    full_vertices: list[tuple[float, float, float]],
    full_uvs: list[tuple[float, float] | None],
    full_skin: list[list[dict[str, Any]]],
    full_faces: list[tuple[list[int], str]],
) -> tuple[list[list[float]], list[list[float] | None], list[list[int]], list[str], list[list[dict[str, Any]]]]:
    preview_vertices: list[list[float]] = []
    preview_uvs: list[list[float] | None] = []
    preview_faces: list[list[int]] = []
    preview_face_materials: list[str] = []
    preview_skin: list[list[dict[str, Any]]] = []
    global_to_preview: dict[int, int] = {}
    vertex_limit = len(full_vertices) if _MESH_PREVIEW_VERTEX_LIMIT <= 0 else _MESH_PREVIEW_VERTEX_LIMIT
    face_limit = len(full_faces) if _MESH_PREVIEW_FACE_LIMIT <= 0 else _MESH_PREVIEW_FACE_LIMIT
    if not full_vertices or vertex_limit <= 0:
        return preview_vertices, preview_uvs, preview_faces, preview_face_materials, preview_skin

    for face_index in _balanced_preview_face_indices(full_faces):
        face, material_name = full_faces[face_index]
        needed = [index for index in face if index not in global_to_preview]
        if len(preview_vertices) + len(needed) > vertex_limit:
            continue
        mapped = [
            _append_preview_vertex(
                index,
                full_vertices,
                full_uvs,
                full_skin,
                preview_vertices,
                preview_uvs,
                preview_skin,
                global_to_preview,
            )
            for index in face
        ]
        if len(mapped) >= 3:
            preview_faces.append(mapped)
            preview_face_materials.append(material_name)
        if len(preview_faces) >= face_limit:
            break

    if not preview_vertices:
        for global_index, point in enumerate(full_vertices[:vertex_limit]):
            global_to_preview[global_index] = len(preview_vertices)
            preview_vertices.append([point[0], point[1], point[2]])
            uv = full_uvs[global_index] if global_index < len(full_uvs) else None
            preview_uvs.append([uv[0], uv[1]] if uv is not None else None)
            preview_skin.append([dict(item) for item in full_skin[global_index]])
    return preview_vertices, preview_uvs, preview_faces, preview_face_materials, preview_skin


def _extract_meshes(
    scene: Any,
    material_names: tuple[str, ...] = (),
) -> tuple[dict[str, Any], list[dict[str, Any]], tuple[str, ...]]:
    points_for_bbox: list[tuple[float, float, float]] = []
    full_vertices: list[tuple[float, float, float]] = []
    full_uvs: list[tuple[float, float] | None] = []
    full_skin: list[list[dict[str, Any]]] = []
    full_faces: list[tuple[list[int], str]] = []
    skins: list[dict[str, Any]] = []
    bone_names: list[str] = []
    vertex_total = 0
    face_total = 0
    mesh_count = int(getattr(scene, "MeshCount", 0) or 0)
    try:
        meshes = list(getattr(scene, "Meshes", []) or [])
    except Exception:
        meshes = []
    mesh_world_transforms = _mesh_node_world_transforms(scene)
    for mesh_index, mesh in enumerate(meshes):
        try:
            material_index = int(getattr(mesh, "MaterialIndex", -1))
        except Exception:
            material_index = -1
        material_name = (
            material_names[material_index]
            if 0 <= material_index < len(material_names)
            else (f"material_{material_index}" if material_index >= 0 else "")
        )
        try:
            vertices = list(getattr(mesh, "Vertices", []) or [])
        except Exception:
            vertices = []
        mesh_uvs: list[Any] = []
        try:
            has_uvs = bool(mesh.HasTextureCoords(0)) if callable(getattr(mesh, "HasTextureCoords", None)) else False
        except Exception:
            has_uvs = False
        if has_uvs:
            try:
                channels = getattr(mesh, "TextureCoordinateChannels", []) or []
                mesh_uvs = list(channels[0] or [])
            except Exception:
                mesh_uvs = []
        base_index = len(full_vertices)
        mesh_transform = mesh_world_transforms.get(mesh_index)
        for local_index, vertex in enumerate(vertices):
            point = _vec3(vertex)
            if mesh_transform is not None:
                point = _transform_point(mesh_transform, point)
            points_for_bbox.append(point)
            full_vertices.append(point)
            full_uvs.append(_uv2(mesh_uvs[local_index]) if local_index < len(mesh_uvs) else None)
            full_skin.append([])
            vertex_total += 1
        try:
            faces = list(getattr(mesh, "Faces", []) or [])
        except Exception:
            faces = []
        for face in faces:
            try:
                indices = [int(index) for index in list(getattr(face, "Indices", []) or [])]
            except Exception:
                indices = []
            if len(indices) >= 3:
                face_total += 1
            if len(indices) < 3:
                continue
            bounded = indices[:8]
            mapped = [base_index + index for index in bounded if 0 <= index < len(vertices)]
            if len(mapped) == len(bounded) and len(mapped) >= 3:
                full_faces.append((mapped, material_name))
        mesh_bones: list[str] = []
        mesh_offset_matrices: dict[str, list[float]] = {}
        mesh_canonical_offset_matrices: dict[str, list[float]] = {}
        try:
            bones = list(getattr(mesh, "Bones", []) or [])
        except Exception:
            bones = []
        for bone in bones:
            raw_name = str(getattr(bone, "Name", "") or "").strip()
            if raw_name:
                bone_names.append(raw_name)
                mesh_bones.append(raw_name)
            joint = _skin_influence_name(raw_name)
            offset_matrix = _matrix_tuple(getattr(bone, "OffsetMatrix", None))
            if raw_name:
                mesh_offset_matrices[raw_name] = [float(value) for value in offset_matrix]
            if joint:
                mesh_canonical_offset_matrices[joint] = [float(value) for value in offset_matrix]
            try:
                weights = list(getattr(bone, "VertexWeights", []) or [])
            except Exception:
                weights = []
            for weight in weights:
                try:
                    local_index = int(getattr(weight, "VertexID", -1))
                except Exception:
                    local_index = -1
                if not 0 <= local_index < len(vertices):
                    continue
                global_index = base_index + local_index
                amount = _finite_float(getattr(weight, "Weight", 0.0))
                if amount > 0.000001 and joint:
                    full_skin[global_index].append({
                        "joint": joint,
                        "source": raw_name,
                        "weight": amount,
                    })
        if mesh_bones:
            skin_entry = {
                "mesh": str(getattr(mesh, "Name", "") or f"mesh_{mesh_index}"),
                "joint_count": len(_unique(mesh_bones)),
                "joints": _unique(mesh_bones),
                "canonical_joints": _unique(_canonical_from_token(name) for name in mesh_bones),
            }
            if mesh_offset_matrices:
                skin_entry["offset_matrices"] = dict(mesh_offset_matrices)
            if mesh_canonical_offset_matrices:
                skin_entry["canonical_offset_matrices"] = dict(mesh_canonical_offset_matrices)
            skins.append(skin_entry)
    preview_vertices, preview_uvs, preview_faces, preview_face_materials, preview_skin = _build_mesh_preview(
        full_vertices,
        full_uvs,
        full_skin,
        full_faces,
    )
    for entries in preview_skin:
        total = sum(_finite_float(item.get("weight")) for item in entries)
        if total > 0.000001:
            for item in entries:
                item["weight"] = _finite_float(item.get("weight")) / total
    preview: dict[str, Any] = {
        "source": "assimpnet",
        "vertices": preview_vertices,
        "faces": preview_faces,
        "truncated": len(preview_vertices) < vertex_total or len(preview_faces) < face_total,
    }
    if preview_face_materials:
        preview["face_materials"] = preview_face_materials
    if any(item is not None for item in preview_uvs):
        preview["uvs"] = [item if item is not None else [0.0, 0.0] for item in preview_uvs]
    if any(preview_skin):
        preview["skin"] = preview_skin
        preview["skin_source"] = "assimpnet"
    mesh_meta = {
        "source": "assimpnet",
        "mesh_count": mesh_count,
        "vertex_count": vertex_total,
        "face_count": face_total,
        "bbox": _bbox(points_for_bbox),
        "preview": preview,
    }
    return mesh_meta, skins, _unique(bone_names)


def _value_at(samples: list[tuple[float, Any]], time_value: float) -> Any:
    if not samples:
        return None
    best = samples[0][1]
    for sample_time, value in samples:
        if sample_time > time_value + 0.000001:
            break
        best = value
    return best


def _clip_time(raw_time: Any, ticks_per_second: float) -> float:
    time_value = _finite_float(raw_time)
    return time_value / ticks_per_second if ticks_per_second > 0.000001 else time_value


def _extract_animations(scene: Any) -> tuple[tuple[str, ...], dict[str, dict[str, Any]]]:
    try:
        animations = list(getattr(scene, "Animations", []) or [])
    except Exception:
        animations = []
    clip_names: list[str] = []
    clip_keyframes: dict[str, dict[str, Any]] = {}
    for anim_index, animation in enumerate(animations):
        name = str(getattr(animation, "Name", "") or f"animation_{anim_index}").strip() or f"animation_{anim_index}"
        clip_names.append(name)
        ticks_per_second = _finite_float(getattr(animation, "TicksPerSecond", 0.0))
        if ticks_per_second <= 0.000001:
            ticks_per_second = 25.0
        frames_by_time: dict[float, dict[str, dict[str, tuple[float, ...]]]] = {}
        sample_count = 0
        try:
            channels = list(getattr(animation, "NodeAnimationChannels", []) or [])
        except Exception:
            channels = []
        for channel in channels:
            canonical = _canonical_from_token(getattr(channel, "NodeName", ""))
            if not canonical:
                continue
            try:
                position_keys = list(getattr(channel, "PositionKeys", []) or [])
            except Exception:
                position_keys = []
            try:
                rotation_keys = list(getattr(channel, "RotationKeys", []) or [])
            except Exception:
                rotation_keys = []
            positions = [
                (_clip_time(getattr(key, "Time", 0.0), ticks_per_second), _vec3(getattr(key, "Value", None)))
                for key in position_keys
            ]
            rotations = [
                (_clip_time(getattr(key, "Time", 0.0), ticks_per_second), _quat4(getattr(key, "Value", None)))
                for key in rotation_keys
            ]
            sample_count += len(positions) + len(rotations)
            times = sorted({time for time, _value in positions} | {time for time, _value in rotations})
            for time_value in times:
                frame = frames_by_time.setdefault(time_value, {"offsets": {}, "rotations": {}})
                position = _value_at(positions, time_value)
                if position is not None:
                    frame["offsets"][canonical] = position
                rotation = _value_at(rotations, time_value)
                if rotation is not None:
                    frame["rotations"][canonical] = rotation
        if frames_by_time:
            frames: list[dict[str, Any]] = []
            for time_value, groups in sorted(frames_by_time.items()):
                frame: dict[str, Any] = {"time": float(time_value)}
                offsets = groups.get("offsets") if isinstance(groups.get("offsets"), Mapping) else {}
                rotations = groups.get("rotations") if isinstance(groups.get("rotations"), Mapping) else {}
                if offsets:
                    frame["bone_offsets"] = {
                        bone: [float(x) for x in value]
                        for bone, value in sorted(offsets.items())
                    }
                if rotations:
                    frame["bone_rotations"] = {
                        bone: [float(x) for x in value]
                        for bone, value in sorted(rotations.items())
                    }
                if len(frame) > 1:
                    frames.append(frame)
            if frames:
                duration = _finite_float(getattr(animation, "DurationInTicks", 0.0)) / ticks_per_second
                if duration <= 0.000001:
                    duration = max(float(frame["time"]) for frame in frames)
                clip_keyframes[name] = {
                    "source": "assimpnet",
                    "duration": duration,
                    "loop": True,
                    "keyframes": frames,
                    "sample_count": sample_count,
                }
    return _unique(clip_names), clip_keyframes


def import_model_metadata(path: str | Path, *, root: Path | None = None) -> dict[str, Any]:
    assimp = _ensure_assimp(root)
    if assimp is None:
        return {
            "mesh": {"source": "assimpnet", "mesh_count": 0, "vertex_count": 0, "face_count": 0, "bbox": {}},
            "nodes": (),
            "materials": (),
            "errors": (load_error() or "AssimpNet importer unavailable",),
        }
    context = None
    try:
        context = assimp.AssimpContext()
        steps = (
            assimp.PostProcessSteps.JoinIdenticalVertices
            | assimp.PostProcessSteps.Triangulate
            | assimp.PostProcessSteps.GenerateSmoothNormals
        )
        scene = context.ImportFile(str(path), steps)
        nodes: list[str] = []
        rest_positions: dict[str, tuple[float, float, float]] = {}
        node_parents: dict[str, str] = {}
        _walk_nodes(
            getattr(scene, "RootNode", None),
            (1.0, 0.0, 0.0, 0.0,
             0.0, 1.0, 0.0, 0.0,
             0.0, 0.0, 1.0, 0.0,
             0.0, 0.0, 0.0, 1.0),
            nodes,
            rest_positions,
            node_parents,
        )
        try:
            materials = _unique(getattr(material, "Name", "") for material in list(scene.Materials or []))
        except Exception:
            materials = ()
        material_textures, embedded_textures = _material_texture_metadata(scene, materials)
        mesh, skins, bone_names = _extract_meshes(scene, materials)
        clip_names, clip_keyframes = _extract_animations(scene)
        return {
            "mesh": mesh,
            "skins": skins,
            "nodes": _unique(nodes),
            "materials": materials,
            "material_textures": material_textures,
            "embedded_textures": embedded_textures,
            "bone_names": bone_names,
            "clips": clip_names,
            "clip_keyframes": clip_keyframes,
            "rest_positions": dict(rest_positions),
            "node_parents": dict(node_parents),
            "rest_positions_source": "assimpnet" if rest_positions else "",
            "errors": (),
        }
    except Exception as exc:
        return {
            "mesh": {"source": "assimpnet", "mesh_count": 0, "vertex_count": 0, "face_count": 0, "bbox": {}},
            "nodes": (),
            "materials": (),
            "errors": (str(exc),),
        }
    finally:
        if context is not None:
            try:
                context.Dispose()
            except Exception:
                pass


__all__ = [
    "available",
    "bundle_paths",
    "bundle_root",
    "import_model_metadata",
    "load_error",
    "reset_for_tests",
]
