# -*- coding: utf-8 -*-
"""Windowless ModernGL renderer for ``model3d`` preview meshes.

This provider renders into an offscreen framebuffer and returns a PIL RGBA
image.  It never owns a platform window, z-order, or input; presentation still
belongs to the unified overlay compositor.
"""
from __future__ import annotations

import io
import os
import threading
from collections.abc import Mapping
from pathlib import Path
from typing import Any

try:
    import numpy as np
except Exception:  # pragma: no cover - optional at import time
    np = None  # type: ignore[assignment]

try:
    from PIL import Image
except Exception:  # pragma: no cover - optional at import time
    Image = None  # type: ignore[assignment]

try:
    from render import model3d_software as _software
except Exception:  # pragma: no cover - optional at import time
    _software = None  # type: ignore[assignment]


_DISABLED = os.environ.get("SAO_MODEL3D_NATIVE_MODERNGL_DISABLE", "") == "1"
_RENDERER_NAME = "moderngl-offscreen"
_GLOBAL_LOCK = threading.Lock()
_RENDER_LOCK = threading.Lock()
_TLS = threading.local()
_GLOBAL_FAILED = False
_MODERNGL: Any = None
_IMPORT_ERROR = ""
_TOPOLOGY_CACHE_LIMIT = 64
_TOPOLOGY_CACHE: dict[
    tuple[Any, ...],
    tuple[list[tuple[Any, Any]], list[tuple[Any, Any]]],
] = {}
_SKIN_CACHE_LIMIT = 32
_MAX_GPU_BONES = 96
_ALPHA_STATS_CACHE_LIMIT = 64
_ALPHA_STATS_CACHE: dict[tuple[Any, ...], dict[str, float]] = {}


_VS_MESH = """
#version 330
in vec2 in_pos;
void main() {
    gl_Position = vec4(in_pos, 0.0, 1.0);
}
"""

_FS_MESH = """
#version 330
uniform vec4 u_color;
out vec4 f_color;
void main() {
    f_color = u_color;
}
"""

_VS_TEXTURE = """
#version 330
in vec3 in_pos;
in vec2 in_uv;
out vec2 v_uv;
void main() {
    v_uv = in_uv;
    gl_Position = vec4(in_pos, 1.0);
}
"""

_SKIN_ROTATE_GLSL = """\
vec3 rotate_node(vec3 point) {
    float cy = cos(u_rotation.y);
    float sy = sin(u_rotation.y);
    float x = point.x * cy + point.z * sy;
    float z = -point.x * sy + point.z * cy;
    float cx = cos(u_rotation.x);
    float sx = sin(u_rotation.x);
    float y = point.y * cx - z * sx;
    z = point.y * sx + z * cx;
    float cz = cos(u_rotation.z);
    float sz = sin(u_rotation.z);
    float rx = x * cz - y * sz;
    float ry = x * sz + y * cz;
    return vec3(rx, ry, z);
}"""

_SKIN_MATRIX_GLSL = """\
mat4 skin_matrix() {
    mat4 skin = mat4(0.0);
    float total = 0.0;
    for (int i = 0; i < 4; i++) {
        int index = int(in_joints0[i]);
        float weight = in_weights0[i];
        if (weight > 0.00001 && index >= 0 && index < %(max_bones)d) {
            skin += u_bones[index] * weight;
            total += weight;
        }
    }
    for (int i = 0; i < 4; i++) {
        int index = int(in_joints1[i]);
        float weight = in_weights1[i];
        if (weight > 0.00001 && index >= 0 && index < %(max_bones)d) {
            skin += u_bones[index] * weight;
            total += weight;
        }
    }
    if (total <= 0.00001) {
        return mat4(1.0);
    }
    return skin;
}""" % {"max_bones": _MAX_GPU_BONES}

_VS_SKIN_TEXTURE = """
#version 330
const int MAX_BONES = %(max_bones)d;
in vec3 in_pos;
in vec2 in_uv;
in vec4 in_joints0;
in vec4 in_weights0;
in vec4 in_joints1;
in vec4 in_weights1;
uniform mat4 u_bones[MAX_BONES];
uniform vec2 u_viewport;
uniform vec3 u_center;
uniform vec3 u_rotation;
uniform float u_scale;
uniform float u_depth_scale;
out vec2 v_uv;

%(rotate_func)s

%(skin_matrix_func)s

void main() {
    vec4 world = skin_matrix() * vec4(in_pos, 1.0);
    vec3 point = rotate_node(world.xyz);
    float ndc_x = (point.x - u_center.x) * (2.0 * u_scale / max(1.0, u_viewport.x));
    float ndc_y = (point.y - u_center.y) * (2.0 * u_scale / max(1.0, u_viewport.y)) - 0.04;
    float ndc_z = (point.z - u_center.z) * u_depth_scale;
    v_uv = in_uv;
    gl_Position = vec4(ndc_x, ndc_y, ndc_z, 1.0);
}
""" % {"max_bones": _MAX_GPU_BONES, "rotate_func": _SKIN_ROTATE_GLSL, "skin_matrix_func": _SKIN_MATRIX_GLSL}

# -- MToon toon shading vertex shader (skin path) --
_VS_SKIN_TOON = """
#version 330
const int MAX_BONES = %(max_bones)d;
in vec3 in_pos;
in vec2 in_uv;
in vec4 in_joints0;
in vec4 in_weights0;
in vec4 in_joints1;
in vec4 in_weights1;
uniform mat4 u_bones[MAX_BONES];
uniform vec2 u_viewport;
uniform vec3 u_center;
uniform vec3 u_rotation;
uniform float u_scale;
uniform float u_depth_scale;
out vec2 v_uv;
out vec3 v_world_normal;
out vec3 v_world_pos;

%(rotate_func)s

%(skin_matrix_func)s

void main() {
    mat4 sm = skin_matrix();
    vec4 world = sm * vec4(in_pos, 1.0);
    // Approximate world normal from skin matrix (handles uniform scale)
    vec3 raw_normal = normalize(mat3(sm) * vec3(0.0, 0.0, 1.0));
    v_world_normal = normalize(mat3(1.0) * raw_normal);
    v_world_pos = world.xyz;

    vec3 point = rotate_node(world.xyz);
    float ndc_x = (point.x - u_center.x) * (2.0 * u_scale / max(1.0, u_viewport.x));
    float ndc_y = (point.y - u_center.y) * (2.0 * u_scale / max(1.0, u_viewport.y)) - 0.04;
    float ndc_z = (point.z - u_center.z) * u_depth_scale;
    v_uv = in_uv;
    gl_Position = vec4(ndc_x, ndc_y, ndc_z, 1.0);
}
""" % {"max_bones": _MAX_GPU_BONES, "rotate_func": _SKIN_ROTATE_GLSL, "skin_matrix_func": _SKIN_MATRIX_GLSL}

# -- MToon toon shading vertex shader (non-skin textured path) --
_VS_TEXTURE_TOON = """
#version 330
in vec3 in_pos;
in vec2 in_uv;
out vec2 v_uv;
out vec3 v_world_normal;
out vec3 v_world_pos;
void main() {
    v_uv = in_uv;
    // Non-skin path: positions are already in NDC-ish space, approximate normal
    v_world_normal = vec3(0.0, 0.0, 1.0);
    v_world_pos = in_pos;
    gl_Position = vec4(in_pos, 1.0);
}
"""

# -- MToon toon fragment shader --
_FS_TOON = """
#version 330
uniform sampler2D u_texture;
uniform vec4 u_tint;
uniform vec3 u_shade_color;
uniform float u_shade_toony;
uniform float u_shade_shift;
uniform float u_rim_power;
uniform vec3 u_rim_color;
uniform vec3 u_ambient;
uniform vec3 u_light_dir;
uniform vec3 u_camera_pos;
in vec2 v_uv;
in vec3 v_world_normal;
in vec3 v_world_pos;
out vec4 f_color;
void main() {
    vec4 texel = texture(u_texture, v_uv);
    if (texel.a * u_tint.a < 0.03) {
        discard;
    }
    vec3 baseColor = texel.rgb * u_tint.rgb;
    vec3 N = normalize(v_world_normal);
    vec3 L = normalize(u_light_dir);
    vec3 V = normalize(u_camera_pos - v_world_pos);

    // Half-Lambert + toony threshold for 2-step anime shadow
    float NdotL = dot(N, L) * 0.5 + 0.5;
    float shade = smoothstep(u_shade_shift, mix(1.0, u_shade_shift, u_shade_toony), NdotL);
    vec3 diffuse = mix(baseColor * u_shade_color, baseColor, shade);

    // Hard specular highlight
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), 256.0);
    spec = smoothstep(0.005, 0.01, spec);

    // Fresnel rim light (only on lit surfaces)
    float rim = pow(1.0 - max(dot(V, N), 0.0), u_rim_power);
    rim *= smoothstep(0.0, 0.01, NdotL);

    vec3 color = diffuse * (u_ambient + vec3(1.0)) + vec3(1.0) * spec * 0.3 + u_rim_color * rim * 0.5;
    f_color = vec4(color, texel.a * u_tint.a);
}
"""

_FS_TEXTURE = """
#version 330
uniform sampler2D u_texture;
uniform vec4 u_tint;
in vec2 v_uv;
out vec4 f_color;
void main() {
    vec4 texel = texture(u_texture, v_uv);
    if (texel.a * u_tint.a < 0.03) {
        discard;
    }
    f_color = vec4(texel.rgb * u_tint.rgb, texel.a * u_tint.a);
}
"""


def _get_wgl_serialize_lock() -> Any:
    try:
        from render.gpu_overlay_window import get_wgl_serialize_lock
        return get_wgl_serialize_lock()
    except Exception:
        return threading.RLock()


def _import_moderngl() -> Any:
    global _MODERNGL, _IMPORT_ERROR
    if _MODERNGL is not None:
        return _MODERNGL
    try:
        import moderngl  # type: ignore[import-not-found]
    except Exception as exc:
        _IMPORT_ERROR = str(exc)
        return None
    _MODERNGL = moderngl
    return _MODERNGL


def _create_context(moderngl: Any) -> Any:
    if os.name == "nt":
        return moderngl.create_standalone_context(require=330)
    return moderngl.create_standalone_context(require=330, backend="egl")


def _release_resource(resource: Any) -> bool:
    released = False
    for name in ("release", "close"):
        fn = getattr(resource, name, None)
        if not callable(fn):
            continue
        try:
            fn()
            released = True
            break
        except Exception:
            pass
    return released


def _ensure_state() -> Any:
    global _GLOBAL_FAILED
    if _DISABLED or np is None or Image is None:
        return None
    state = getattr(_TLS, "state", None)
    if state is not None:
        return state
    if getattr(_TLS, "failed", False) or _GLOBAL_FAILED:
        return None
    with _GLOBAL_LOCK:
        if _GLOBAL_FAILED:
            return None
        moderngl = _import_moderngl()
        if moderngl is None:
            _GLOBAL_FAILED = True
            _TLS.failed = True
            return None
        try:
            ctx = _create_context(moderngl)
            program = ctx.program(vertex_shader=_VS_MESH, fragment_shader=_FS_MESH)
            texture_program = ctx.program(vertex_shader=_VS_TEXTURE, fragment_shader=_FS_TEXTURE)
            skin_texture_program = ctx.program(vertex_shader=_VS_SKIN_TEXTURE, fragment_shader=_FS_TEXTURE)
            # MToon toon shading programs
            skin_toon_program = ctx.program(vertex_shader=_VS_SKIN_TOON, fragment_shader=_FS_TOON)
            texture_toon_program = ctx.program(vertex_shader=_VS_TEXTURE_TOON, fragment_shader=_FS_TOON)
        except Exception:
            _GLOBAL_FAILED = True
            _TLS.failed = True
            return None
    state = {
        "ctx": ctx,
        "program": program,
        "texture_program": texture_program,
        "skin_texture_program": skin_texture_program,
        "skin_toon_program": skin_toon_program,
        "texture_toon_program": texture_toon_program,
        "textures": {},
        "skin_geometry": {},
        "skin_geometry_toon": {},
        "moderngl": moderngl,
    }
    _TLS.state = state
    return state


def _point3(value: Any) -> tuple[float, float, float] | None:
    if not isinstance(value, (list, tuple)) or len(value) < 3:
        return None
    try:
        return float(value[0]), float(value[1]), float(value[2])
    except Exception:
        return None


def _vec_add(
    a: tuple[float, float, float],
    b: tuple[float, float, float],
) -> tuple[float, float, float]:
    return a[0] + b[0], a[1] + b[1], a[2] + b[2]


def _faces(value: Any, vertex_count: int) -> list[list[int]]:
    out: list[list[int]] = []
    if not isinstance(value, (list, tuple)):
        return out
    for raw in value:
        if not isinstance(raw, (list, tuple)):
            continue
        face: list[int] = []
        for item in raw[:16]:
            try:
                index = int(item)
            except Exception:
                continue
            if 0 <= index < vertex_count:
                face.append(index)
        if len(face) >= 3:
            out.append(face)
    return out


def _uv2(value: Any) -> tuple[float, float] | None:
    if not isinstance(value, (list, tuple)) or len(value) < 2:
        return None
    try:
        return float(value[0]), float(value[1])
    except Exception:
        return None


def _preview_uvs(preview: Mapping[str, Any], vertex_count: int) -> list[tuple[float, float]]:
    raw = preview.get("uvs")
    if not isinstance(raw, (list, tuple)) or len(raw) < vertex_count:
        return []
    out: list[tuple[float, float]] = []
    for item in raw[:vertex_count]:
        uv = _uv2(item)
        if uv is None:
            return []
        out.append(uv)
    return out


def _preview_from_context(context: Mapping[str, Any]) -> tuple[list[tuple[float, float, float]], list[list[int]], Mapping[str, Any]]:
    model = context.get("model") if isinstance(context.get("model"), Mapping) else {}
    mesh = model.get("mesh") if isinstance(model.get("mesh"), Mapping) else {}
    preview = mesh.get("preview") if isinstance(mesh.get("preview"), Mapping) else {}
    cached_geometry = getattr(_software, "_preview_geometry", None) if _software is not None else None
    if callable(cached_geometry):
        try:
            vertices, faces = cached_geometry(model, preview)
            return vertices, faces, preview
        except Exception:
            pass
    raw_vertices = preview.get("vertices") if isinstance(preview, Mapping) else ()
    vertices = [point for point in (_point3(item) for item in (raw_vertices or ())) if point is not None]
    faces = _faces(preview.get("faces"), len(vertices)) if isinstance(preview, Mapping) else []
    return vertices, faces, preview


def _deform_vertices(
    vertices: list[tuple[float, float, float]],
    preview: Mapping[str, Any],
    node: Mapping[str, Any],
    meta: Mapping[str, Any] | None = None,
) -> list[tuple[float, float, float]]:
    fn = getattr(_software, "_deform_vertices", None) if _software is not None else None
    if not callable(fn):
        return vertices
    try:
        return fn(vertices, preview, node, meta)
    except RuntimeError as exc:
        if "pybullet physics provider" in str(exc).lower():
            raise
        return vertices
    except Exception:
        return vertices


def _project_vertices(
    vertices: list[tuple[float, float, float]],
    width: int,
    height: int,
    node: Mapping[str, Any],
) -> list[tuple[float, float, float]]:
    fn = getattr(_software, "_project", None) if _software is not None else None
    if not callable(fn):
        return []
    try:
        result = fn(vertices, width, height, node)
        projected = result[0] if isinstance(result, tuple) and result else result
        return list(projected)
    except Exception:
        return []


def _accent_color(context: Mapping[str, Any]) -> tuple[int, int, int, int]:
    palette = context.get("palette") if isinstance(context.get("palette"), Mapping) else {}
    color_fn = getattr(_software, "_color", None) if _software is not None else None
    if callable(color_fn):
        try:
            return color_fn(palette.get("accent") or "#7dd3fc", (125, 211, 252, 255))
        except Exception:
            pass
    return 125, 211, 252, 255


def _face_base_color(
    face: list[int],
    preview: Mapping[str, Any],
    accent: tuple[int, int, int, int],
    face_index: int = -1,
    meta: Mapping[str, Any] | None = None,
) -> tuple[int, int, int, int]:
    fn = getattr(_software, "_face_base_color", None) if _software is not None else None
    if callable(fn):
        try:
            return fn(face, preview, accent, face_index, meta)
        except Exception:
            pass
    return accent


def _outline_color(base: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    fn = getattr(_software, "_outline_color", None) if _software is not None else None
    if callable(fn):
        try:
            return fn(base)
        except Exception:
            pass
    return (
        max(0, int(base[0] * 0.58)),
        max(0, int(base[1] * 0.64)),
        max(0, int(base[2] * 0.72)),
        238,
    )


def _topology_cache_key(
    faces: list[list[int]],
    preview: Mapping[str, Any],
    accent: tuple[int, int, int, int],
    meta: Mapping[str, Any] | None = None,
) -> tuple[Any, ...]:
    skin = preview.get("skin")
    materials = preview.get("face_materials")
    material_config = meta.get("materials_config") if isinstance(meta, Mapping) else None
    return (
        id(preview.get("faces")),
        len(faces),
        id(skin),
        len(skin) if isinstance(skin, (list, tuple)) else 0,
        id(materials),
        len(materials) if isinstance(materials, (list, tuple)) else 0,
        id(material_config),
        accent,
    )


def _topology_index_batches(
    faces: list[list[int]],
    preview: Mapping[str, Any],
    accent: tuple[int, int, int, int],
    meta: Mapping[str, Any] | None = None,
) -> tuple[list[tuple[Any, Any]], list[tuple[Any, Any]]]:
    if np is None:
        return [], []
    key = _topology_cache_key(faces, preview, accent, meta)
    cached = _TOPOLOGY_CACHE.get(key)
    if cached is not None:
        return cached
    tri_groups: dict[tuple[int, int, int, int], list[int]] = {}
    line_groups: dict[tuple[int, int, int, int], list[int]] = {}
    for face_index, face in enumerate(faces):
        if len(face) < 3:
            continue
        base = _face_base_color(face, preview, accent, face_index, meta)
        outline = _outline_color(base)
        tri_group = tri_groups.setdefault(base, [])
        line_group = line_groups.setdefault(outline, [])
        first = face[0]
        for index in range(1, len(face) - 1):
            tri_group.extend((first, face[index], face[index + 1]))
        for index, point in enumerate(face):
            line_group.extend((point, face[(index + 1) % len(face)]))
    triangles = [
        (np.asarray(indices, dtype=np.int32), _gl_color(color, alpha=0.76))
        for color, indices in tri_groups.items()
        if indices
    ]
    lines = [
        (np.asarray(indices, dtype=np.int32), _gl_color(color, alpha=0.96))
        for color, indices in line_groups.items()
        if indices
    ]
    if len(_TOPOLOGY_CACHE) >= _TOPOLOGY_CACHE_LIMIT:
        _TOPOLOGY_CACHE.clear()
    cached = (triangles, lines)
    _TOPOLOGY_CACHE[key] = cached
    return cached


def _projected_ndc_array(projected: list[tuple[float, float, float]], width: int, height: int) -> Any:
    if np is None:
        return None
    arr = np.asarray(projected, dtype=np.float32)
    if arr.ndim != 2 or arr.shape[0] <= 0 or arr.shape[1] < 2:
        return None
    out = np.empty((arr.shape[0], 2), dtype=np.float32)
    out[:, 0] = (arr[:, 0] / max(1.0, float(width))) * 2.0 - 1.0
    out[:, 1] = 1.0 - (arr[:, 1] / max(1.0, float(height))) * 2.0
    return out


def _gl_color(color: tuple[int, int, int, int], alpha: float | None = None) -> tuple[float, float, float, float]:
    a = color[3] / 255.0 if alpha is None else alpha
    return (
        max(0.0, min(1.0, color[0] / 255.0)),
        max(0.0, min(1.0, color[1] / 255.0)),
        max(0.0, min(1.0, color[2] / 255.0)),
        max(0.0, min(1.0, a)),
    )


def _material_texture_path(meta: Mapping[str, Any] | None, material_name: str) -> Path | None:
    fn = getattr(_software, "_material_texture_path", None) if _software is not None else None
    if not callable(fn):
        return None
    try:
        return fn(meta, material_name)
    except Exception:
        return None


def _material_tokens_match(left: str, right: str) -> bool:
    if not left or not right:
        return False
    norm = getattr(_software, "_norm_material_token", None) if _software is not None else None
    if callable(norm):
        try:
            left_token = str(norm(left) or "")
            right_token = str(norm(right) or "")
        except Exception:
            left_token = "".join(ch for ch in str(left).lower() if ch.isalnum())
            right_token = "".join(ch for ch in str(right).lower() if ch.isalnum())
    else:
        left_token = "".join(ch for ch in str(left).lower() if ch.isalnum())
        right_token = "".join(ch for ch in str(right).lower() if ch.isalnum())
    return bool(
        left_token
        and right_token
        and (left_token == right_token or left_token in right_token or right_token in left_token)
    )


def _embedded_texture_ref(meta: Mapping[str, Any], slot: Mapping[str, Any]) -> dict[str, Any] | None:
    embedded_id = str(slot.get("embedded_id") or "").strip()
    if not embedded_id:
        return None
    embedded = meta.get("embedded_textures")
    if not isinstance(embedded, Mapping):
        return None
    item = embedded.get(embedded_id)
    if not isinstance(item, Mapping):
        return None
    data = item.get("data")
    if not isinstance(data, (bytes, bytearray)) or not data:
        return None
    return {
        "kind": "embedded",
        "embedded_id": embedded_id,
        "sha1": str(item.get("sha1") or slot.get("sha1") or ""),
        "size": int(item.get("size") or len(data)),
        "format": str(item.get("format") or slot.get("format") or ""),
        "data": bytes(data),
    }


def _file_texture_ref(meta: Mapping[str, Any], slot: Mapping[str, Any]) -> dict[str, Any] | None:
    texture_path = str(slot.get("path") or "").strip()
    if not texture_path:
        return None
    candidates_fn = getattr(_software, "_texture_path_candidates", None) if _software is not None else None
    candidates: list[Path] = []
    if callable(candidates_fn):
        try:
            candidates = list(candidates_fn(meta, texture_path) or [])
        except Exception:
            candidates = []
    if not candidates:
        raw = Path(texture_path)
        if raw.is_absolute():
            candidates.append(raw)
        else:
            model_path = Path(str(meta.get("resolved_path") or meta.get("path") or ""))
            if str(model_path):
                candidates.append(model_path.parent / raw)
                candidates.append(model_path.parent.parent / raw)
            candidates.append(Path.cwd() / raw)
    for candidate in candidates:
        try:
            if candidate.is_file():
                return {"kind": "file", "path": candidate}
        except Exception:
            continue
    return None


def _material_texture_ref(meta: Mapping[str, Any] | None, material_name: str) -> Any:
    if not isinstance(meta, Mapping) or not material_name:
        return None
    material_textures = meta.get("material_textures")
    entry: Any = None
    if isinstance(material_textures, Mapping):
        entry = material_textures.get(material_name)
        if not isinstance(entry, Mapping):
            for name, candidate in material_textures.items():
                if isinstance(candidate, Mapping) and _material_tokens_match(str(name), material_name):
                    entry = candidate
                    break
    if isinstance(entry, Mapping):
        slot = entry.get("base_color")
        slots = [slot] if isinstance(slot, Mapping) else []
        raw_slots = entry.get("textures")
        if isinstance(raw_slots, (list, tuple)):
            slots.extend(item for item in raw_slots if isinstance(item, Mapping))
        seen: set[tuple[str, str]] = set()
        for item in slots:
            key = (str(item.get("kind") or ""), str(item.get("embedded_id") or item.get("path") or ""))
            if key in seen:
                continue
            seen.add(key)
            if item.get("kind") == "embedded":
                ref = _embedded_texture_ref(meta, item)
            elif item.get("kind") == "file":
                ref = _file_texture_ref(meta, item)
            else:
                ref = None
            if ref is not None:
                return ref
    path = _material_texture_path(meta, material_name)
    return {"kind": "file", "path": path} if path is not None else None


def _ndc(point: tuple[float, float, float], width: int, height: int) -> tuple[float, float]:
    return (
        (float(point[0]) / max(1.0, float(width))) * 2.0 - 1.0,
        1.0 - (float(point[1]) / max(1.0, float(height))) * 2.0,
    )


def _projected_depth_array(projected: list[tuple[float, float, float]]) -> Any:
    if np is None or not projected:
        return None
    depth = np.asarray([float(point[2]) for point in projected], dtype=np.float32)
    if getattr(depth, "size", 0) <= 0:
        return None
    depth_min = float(np.min(depth))
    depth_max = float(np.max(depth))
    span = depth_max - depth_min
    if span <= 0.000001:
        return np.zeros((int(depth.size),), dtype=np.float32)
    return np.asarray(0.9 - ((depth - depth_min) / span) * 1.8, dtype=np.float32)


def _mesh_textured_arrays(
    projected: list[tuple[float, float, float]],
    faces: list[list[int]],
    width: int,
    height: int,
    preview: Mapping[str, Any],
    meta: Mapping[str, Any] | None = None,
) -> list[tuple[Any, Any, tuple[float, float, float, float]]]:
    if np is None:
        return []
    uvs = _preview_uvs(preview, len(projected))
    if not uvs:
        return []
    ndc = _projected_ndc_array(projected, width, height)
    if ndc is None:
        return []
    depth = _projected_depth_array(projected)
    if depth is None or len(depth) != len(projected):
        return []
    uv_array = np.asarray(uvs, dtype=np.float32)
    if uv_array.ndim != 2 or uv_array.shape[0] != len(projected) or uv_array.shape[1] < 2:
        return []
    materials = preview.get("face_materials")
    if not isinstance(materials, (list, tuple)):
        return []
    groups: dict[tuple[Any, ...], tuple[Any, list[int]]] = {}
    texture_cache: dict[str, Any] = {}
    for face_index, face in enumerate(faces):
        material_name = str(materials[face_index] or "").strip() if face_index < len(materials) else ""
        if not material_name:
            continue
        texture_ref = texture_cache.get(material_name)
        if material_name not in texture_cache:
            texture_ref = _material_texture_ref(meta, material_name)
            texture_cache[material_name] = texture_ref
        if texture_ref is None:
            continue
        group_key = _texture_cache_key(texture_ref)
        _ref, indices = groups.setdefault(group_key, (texture_ref, []))
        first = face[0]
        for index in range(1, len(face) - 1):
            indices.extend((first, face[index], face[index + 1]))
    batches: list[tuple[Any, Any, tuple[float, float, float, float]]] = []
    for texture_ref, raw_indices in groups.values():
        if not raw_indices:
            continue
        indices = np.asarray(raw_indices, dtype=np.int32)
        if getattr(indices, "size", 0) <= 0:
            continue
        array = np.empty((int(indices.size), 5), dtype=np.float32)
        array[:, 0:2] = ndc[indices]
        array[:, 2] = depth[indices]
        array[:, 3:5] = uv_array[indices, 0:2]
        batches.append((texture_ref, np.ascontiguousarray(array, dtype="f4"), (1.0, 1.0, 1.0, 1.0)))
    return batches


def _skin_geometry_cache_key(preview: Mapping[str, Any], meta: Mapping[str, Any] | None) -> tuple[Any, ...]:
    skin = preview.get("skin")
    vertices = preview.get("vertices")
    faces = preview.get("faces")
    return (
        tuple(meta.get("cache_key") or ()) if isinstance(meta, Mapping) else (),
        id(vertices),
        len(vertices) if isinstance(vertices, (list, tuple)) else 0,
        id(faces),
        len(faces) if isinstance(faces, (list, tuple)) else 0,
        id(skin),
        len(skin) if isinstance(skin, (list, tuple)) else 0,
    )


def _skin_static_geometry(preview: Mapping[str, Any], meta: Mapping[str, Any] | None) -> dict[str, Any] | None:
    if np is None:
        return None
    raw_vertices = preview.get("vertices")
    raw_skin = preview.get("skin")
    raw_faces = preview.get("faces")
    materials = preview.get("face_materials")
    uvs = _preview_uvs(preview, len(raw_vertices) if isinstance(raw_vertices, (list, tuple)) else 0)
    if not isinstance(raw_vertices, (list, tuple)) or not isinstance(raw_skin, (list, tuple)) or not uvs:
        return None
    vertices: list[tuple[float, float, float]] = []
    for item in raw_vertices:
        point = _point3(item)
        if point is None:
            return None
        vertices.append(point)
    if len(vertices) != len(uvs):
        return None
    fn = getattr(_software, "_preview_skin_influences", None) if _software is not None else None
    if not callable(fn):
        return None
    skin_influences = list(fn(preview) or [])
    if len(skin_influences) != len(vertices):
        return None
    joint_names: list[str] = []
    joint_index: dict[str, int] = {}
    for influences in skin_influences:
        for joint, _weight in influences:
            if joint not in joint_index:
                if len(joint_names) >= _MAX_GPU_BONES:
                    return None
                joint_index[joint] = len(joint_names)
                joint_names.append(joint)
    positions = np.asarray(vertices, dtype=np.float32)
    uv_array = np.asarray(uvs, dtype=np.float32)
    joints0 = np.zeros((len(vertices), 4), dtype=np.float32)
    weights0 = np.zeros((len(vertices), 4), dtype=np.float32)
    joints1 = np.zeros((len(vertices), 4), dtype=np.float32)
    weights1 = np.zeros((len(vertices), 4), dtype=np.float32)
    for row, influences in enumerate(skin_influences):
        for slot, (joint, weight) in enumerate(influences[:8]):
            target_joints = joints0 if slot < 4 else joints1
            target_weights = weights0 if slot < 4 else weights1
            col = slot if slot < 4 else slot - 4
            target_joints[row, col] = float(joint_index.get(joint, 0))
            target_weights[row, col] = float(weight)
    face_list = _faces(raw_faces, len(vertices))
    if not face_list or not isinstance(materials, (list, tuple)):
        return None
    batches: list[tuple[str, np.ndarray]] = []
    groups: dict[str, list[int]] = {}
    for face_index, face in enumerate(face_list):
        material_name = str(materials[face_index] or "").strip() if face_index < len(materials) else ""
        if not material_name:
            continue
        bucket = groups.setdefault(material_name, [])
        first = face[0]
        for index in range(1, len(face) - 1):
            bucket.extend((first, face[index], face[index + 1]))
    for material_name, indices in groups.items():
        if indices:
            batches.append((material_name, np.asarray(indices, dtype=np.int32)))
    if not batches:
        return None
    return {
        "positions": positions,
        "uvs": uv_array,
        "joints0": joints0,
        "weights0": weights0,
        "joints1": joints1,
        "weights1": weights1,
        "joint_names": tuple(joint_names),
        "batches": batches,
    }


def _rotation_tuple(node: Mapping[str, Any]) -> tuple[float, float, float]:
    fn = getattr(_software, "_rotation", None) if _software is not None else None
    if callable(fn):
        try:
            return fn(node)
        except Exception:
            pass
    return 0.0, 0.0, 0.0


def _gpu_projection_params(
    positions: Any,
    width: int,
    height: int,
    node: Mapping[str, Any],
) -> dict[str, Any] | None:
    if np is None or positions is None or getattr(positions, "size", 0) <= 0:
        return None
    arr = np.asarray(positions, dtype=np.float32)
    if arr.ndim != 2 or arr.shape[1] < 3:
        return None
    rx, ry, rz = _rotation_tuple(node)
    x = arr[:, 0]
    y = arr[:, 1]
    z = arr[:, 2]
    cy = np.cos(ry); sy = np.sin(ry)
    x1 = x * cy + z * sy
    z1 = -x * sy + z * cy
    cx = np.cos(rx); sx = np.sin(rx)
    y2 = y * cx - z1 * sx
    z2 = y * sx + z1 * cx
    cz = np.cos(rz); sz = np.sin(rz)
    x3 = x1 * cz - y2 * sz
    y3 = x1 * sz + y2 * cz
    min_x = float(np.min(x3)); max_x = float(np.max(x3))
    min_y = float(np.min(y3)); max_y = float(np.max(y3))
    min_z = float(np.min(z2)); max_z = float(np.max(z2))
    span_x = max(0.0001, max_x - min_x)
    span_y = max(0.0001, max_y - min_y)
    span_z = max(0.0001, max_z - min_z)
    scale = min(width * 0.84 / span_x, height * 0.86 / span_y)
    return {
        "rotation": (float(rx), float(ry), float(rz)),
        "center": ((min_x + max_x) * 0.5, (min_y + max_y) * 0.5, (min_z + max_z) * 0.5),
        "scale": float(scale),
        "depth_scale": float(min(0.9, 0.9 / span_z)),
    }


def _resolve_pose_state(preview: Mapping[str, Any], node: Mapping[str, Any], meta: Mapping[str, Any] | None) -> dict[str, Any]:
    fn = getattr(_software, "_resolve_pose_state", None) if _software is not None else None
    if not callable(fn):
        return {}
    try:
        state = fn(preview, node, meta)
        return dict(state) if isinstance(state, Mapping) else {}
    except Exception:
        return {}


def _pose_state_from_context(preview: Mapping[str, Any], node: Mapping[str, Any], context: Mapping[str, Any], meta: Mapping[str, Any] | None) -> dict[str, Any]:
    pose = context.get("pose") if isinstance(context.get("pose"), Mapping) else {}
    fn = getattr(_software, "_pose_state_from_pose", None) if _software is not None else None
    if callable(fn):
        try:
            state = fn(preview, node, pose, meta)
            if isinstance(state, Mapping) and state:
                return dict(state)
        except Exception:
            pass
    return _resolve_pose_state(preview, node, meta)


def _joint_matrix(before: tuple[float, float, float], after: tuple[float, float, float], quat: tuple[float, float, float, float] | None) -> Any:
    if np is None:
        return None
    if quat is None:
        quat = (0.0, 0.0, 0.0, 1.0)
    x, y, z, w = quat
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    rot = np.asarray([
        [1.0 - 2.0 * (yy + zz), 2.0 * (xy - wz), 2.0 * (xz + wy)],
        [2.0 * (xy + wz), 1.0 - 2.0 * (xx + zz), 2.0 * (yz - wx)],
        [2.0 * (xz - wy), 2.0 * (yz + wx), 1.0 - 2.0 * (xx + yy)],
    ], dtype=np.float32)
    before_v = np.asarray(before, dtype=np.float32)
    after_v = np.asarray(after, dtype=np.float32)
    translation = after_v - rot.dot(before_v)
    out = np.eye(4, dtype=np.float32)
    out[:3, :3] = rot
    out[:3, 3] = translation
    return out


def _quat_has_rotation(quat: tuple[float, float, float, float] | None) -> bool:
    if quat is None:
        return False
    return (
        abs(float(quat[0])) > 0.000001
        or abs(float(quat[1])) > 0.000001
        or abs(float(quat[2])) > 0.000001
        or abs(float(quat[3]) - 1.0) > 0.000001
    )


def _bone_palette(joint_names: tuple[str, ...], pose_state: Mapping[str, Any]) -> Any:
    if np is None or not joint_names:
        return None
    rest = pose_state.get("rest") if isinstance(pose_state.get("rest"), Mapping) else {}
    transforms = pose_state.get("joint_transforms") if isinstance(pose_state.get("joint_transforms"), Mapping) else {}
    physics_offsets = pose_state.get("physics_offsets") if isinstance(pose_state.get("physics_offsets"), Mapping) else {}
    palette = np.repeat(np.eye(4, dtype=np.float32)[None, :, :], _MAX_GPU_BONES, axis=0)
    for index, joint in enumerate(joint_names):
        transform = transforms.get(joint)
        before = rest.get(joint)
        after = before
        quat = None
        if isinstance(transform, tuple) and len(transform) == 3:
            before = transform[0]
            after = transform[1]
            quat = transform[2]
        if before is None:
            continue
        if _quat_has_rotation(quat):
            after = before
        extra = physics_offsets.get(joint)
        if isinstance(extra, tuple):
            after = _vec_add(after, extra) if after is not None else _vec_add(before, extra)
        matrix = _joint_matrix(before, after if after is not None else before, quat)
        if matrix is not None:
            palette[index] = matrix
    return palette


def _skin_gpu_resources(state: Mapping[str, Any], ctx: Any, program: Any, preview: Mapping[str, Any], meta: Mapping[str, Any] | None) -> dict[str, Any] | None:
    cache = state.get("skin_geometry")
    if not isinstance(cache, dict):
        return None
    key = _skin_geometry_cache_key(preview, meta)
    cached = cache.get(key)
    if cached is not None:
        return cached
    geometry = _skin_static_geometry(preview, meta)
    if geometry is None:
        return None
    packed = np.concatenate((
        geometry["positions"],
        geometry["uvs"],
        geometry["joints0"],
        geometry["weights0"],
        geometry["joints1"],
        geometry["weights1"],
    ), axis=1).astype(np.float32, copy=False)
    vbo = ctx.buffer(np.ascontiguousarray(packed, dtype="f4").tobytes())
    batches: list[tuple[str, Any, Any]] = []
    for material_name, indices in geometry["batches"]:
        ibo = ctx.buffer(np.ascontiguousarray(indices, dtype=np.int32).tobytes())
        vao = ctx.vertex_array(
            program,
            [(vbo, "3f 2f 4f 4f 4f 4f", "in_pos", "in_uv", "in_joints0", "in_weights0", "in_joints1", "in_weights1")],
            index_buffer=ibo,
        )
        batches.append((material_name, vao, ibo))
    resource = {
        "vbo": vbo,
        "joint_names": geometry["joint_names"],
        "positions": geometry["positions"],
        "batches": batches,
    }
    if len(cache) >= _SKIN_CACHE_LIMIT:
        cache.clear()
    cache[key] = resource
    return resource

def _mesh_arrays_by_color(
    projected: list[tuple[float, float, float]],
    faces: list[list[int]],
    width: int,
    height: int,
    preview: Mapping[str, Any],
    accent: tuple[int, int, int, int],
    meta: Mapping[str, Any] | None = None,
) -> tuple[list[tuple[Any, tuple[float, float, float, float]]], list[tuple[Any, tuple[float, float, float, float]]]]:
    if np is None:
        return [], []
    ndc = _projected_ndc_array(projected, width, height)
    if ndc is not None:
        tri_index_batches, line_index_batches = _topology_index_batches(faces, preview, accent, meta)
        triangles = [
            (np.ascontiguousarray(ndc[indices].reshape((-1, 2)), dtype="f4"), color)
            for indices, color in tri_index_batches
            if getattr(indices, "size", 0) > 0
        ]
        lines = [
            (np.ascontiguousarray(ndc[indices].reshape((-1, 2)), dtype="f4"), color)
            for indices, color in line_index_batches
            if getattr(indices, "size", 0) > 0
        ]
        return triangles, lines
    tri_groups: dict[tuple[int, int, int, int], list[tuple[float, float]]] = {}
    line_groups: dict[tuple[int, int, int, int], list[tuple[float, float]]] = {}
    ordered = sorted(enumerate(faces), key=lambda item: sum(projected[idx][2] for idx in item[1]) / len(item[1]))
    for face_index, face in ordered:
        pts = [_ndc(projected[index], width, height) for index in face]
        base = _face_base_color(face, preview, accent, face_index, meta)
        outline = _outline_color(base)
        tri_group = tri_groups.setdefault(base, [])
        line_group = line_groups.setdefault(outline, [])
        for index in range(1, len(pts) - 1):
            tri_group.extend((pts[0], pts[index], pts[index + 1]))
        for index, point in enumerate(pts):
            line_group.extend((point, pts[(index + 1) % len(pts)]))
    triangles = [
        (np.asarray(points, dtype="f4"), _gl_color(color, alpha=0.76))
        for color, points in tri_groups.items()
        if points
    ]
    lines = [
        (np.asarray(points, dtype="f4"), _gl_color(color, alpha=0.96))
        for color, points in line_groups.items()
        if points
    ]
    return triangles, lines


def _set_uniform(program: Any, name: str, value: tuple[float, float, float, float]) -> None:
    try:
        program[name].value = value
    except Exception:
        try:
            program[name] = value
        except Exception:
            pass


def _draw_array(ctx: Any, program: Any, mode: Any, array: Any, color: tuple[float, float, float, float]) -> None:
    if array is None or getattr(array, "size", 0) <= 0:
        return
    vbo = None
    vao = None
    try:
        vbo = ctx.buffer(array.tobytes())
        vao = ctx.simple_vertex_array(program, vbo, "in_pos")
        _set_uniform(program, "u_color", color)
        vao.render(mode)
    finally:
        for resource in (vao, vbo):
            release = getattr(resource, "release", None)
            if callable(release):
                try:
                    release()
                except Exception:
                    pass


def _texture_cache_key(ref: Any) -> tuple[Any, ...]:
    if isinstance(ref, Mapping):
        kind = str(ref.get("kind") or "")
        if kind == "embedded":
            return (
                "embedded",
                str(ref.get("sha1") or ""),
                int(ref.get("size") or 0),
                str(ref.get("embedded_id") or ""),
            )
        path = ref.get("path")
    else:
        path = ref
    try:
        path_obj = Path(path)
    except Exception:
        path_obj = Path(str(path or ""))
    try:
        stat = path_obj.stat()
        return "file", str(path_obj), int(stat.st_size), int(stat.st_mtime_ns)
    except Exception:
        return "file", str(path_obj), 0, 0


def _texture_resource(state: Mapping[str, Any], ctx: Any, moderngl: Any, ref: Any) -> Any:
    textures = state.get("textures")
    if not isinstance(textures, dict):
        return None
    key = _texture_cache_key(ref)
    cached = textures.get(key)
    if cached is not None:
        return cached
    if Image is None:
        return None
    try:
        if isinstance(ref, Mapping) and ref.get("kind") == "embedded":
            data = ref.get("data")
            if not isinstance(data, (bytes, bytearray)) or not data:
                return None
            image_source = io.BytesIO(bytes(data))
        else:
            raw_path = ref.get("path") if isinstance(ref, Mapping) else ref
            image_source = Path(raw_path)
        with Image.open(image_source) as image:
            rgba = image.convert("RGBA")
            transpose = getattr(Image, "Transpose", None)
            flip = getattr(transpose, "FLIP_TOP_BOTTOM", None) if transpose is not None else None
            if flip is None:
                flip = getattr(Image, "FLIP_TOP_BOTTOM", None)
            if flip is not None:
                rgba = rgba.transpose(flip)
            data = rgba.tobytes()
            texture = ctx.texture(rgba.size, 4, data)
    except Exception:
        return None
    try:
        texture.filter = (
            getattr(moderngl, "LINEAR", 9729),
            getattr(moderngl, "LINEAR", 9729),
        )
    except Exception:
        pass
    for attr in ("repeat_x", "repeat_y"):
        try:
            setattr(texture, attr, True)
        except Exception:
            pass
    textures[key] = texture
    return texture


def _texture_alpha_stats(ref: Any) -> dict[str, float]:
    key = _texture_cache_key(ref)
    cached = _ALPHA_STATS_CACHE.get(key)
    if cached is not None:
        return cached
    stats = {"transparent": 0.0, "coverage": 1.0, "mean": 1.0}
    if Image is None:
        return stats
    try:
        if isinstance(ref, Mapping) and ref.get("kind") == "embedded":
            data = ref.get("data")
            if not isinstance(data, (bytes, bytearray)) or not data:
                return stats
            image_source = io.BytesIO(bytes(data))
        else:
            raw_path = ref.get("path") if isinstance(ref, Mapping) else ref
            image_source = Path(raw_path)
        with Image.open(image_source) as image:
            alpha = image.convert("RGBA").getchannel("A")
            histogram = alpha.histogram()
    except Exception:
        return stats
    total = float(sum(histogram) or 1)
    below_opaque = float(sum(histogram[:250]))
    visible = float(sum(histogram[8:]))
    mean = sum(index * count for index, count in enumerate(histogram)) / (255.0 * total)
    stats = {
        "transparent": below_opaque / total,
        "coverage": visible / total,
        "mean": mean,
    }
    if len(_ALPHA_STATS_CACHE) >= _ALPHA_STATS_CACHE_LIMIT:
        _ALPHA_STATS_CACHE.clear()
    _ALPHA_STATS_CACHE[key] = stats
    return stats


def _material_is_transparent(model: Mapping[str, Any], material_name: str) -> bool:
    texture_ref = _material_texture_ref(model, str(material_name))
    if texture_ref is None:
        return False
    stats = _texture_alpha_stats(texture_ref)
    return float(stats.get("transparent") or 0.0) > 0.001


def _draw_textured_array(
    ctx: Any,
    program: Any,
    moderngl: Any,
    array: Any,
    texture: Any,
    tint: tuple[float, float, float, float],
) -> None:
    if array is None or getattr(array, "size", 0) <= 0 or texture is None:
        return
    vbo = None
    vao = None
    try:
        use = getattr(texture, "use", None)
        if callable(use):
            use(location=0)
        try:
            program["u_texture"].value = 0
        except Exception:
            try:
                program["u_texture"] = 0
            except Exception:
                pass
        _set_uniform(program, "u_tint", tint)
        vbo = ctx.buffer(array.tobytes())
        vertex_array = getattr(ctx, "vertex_array", None)
        if callable(vertex_array):
            vao = vertex_array(program, [(vbo, "3f 2f", "in_pos", "in_uv")])
        else:
            vao = ctx.simple_vertex_array(program, vbo, "in_pos", "in_uv")
        vao.render(getattr(moderngl, "TRIANGLES", 4))
    finally:
        for resource in (vao, vbo):
            release = getattr(resource, "release", None)
            if callable(release):
                try:
                    release()
                except Exception:
                    pass


def _set_scalar_uniform(program: Any, name: str, value: float) -> None:
    try:
        program[name].value = float(value)
    except Exception:
        pass


def _set_vec2_uniform(program: Any, name: str, value: tuple[float, float]) -> None:
    try:
        program[name].value = (float(value[0]), float(value[1]))
    except Exception:
        pass


def _set_vec3_uniform(program: Any, name: str, value: tuple[float, float, float]) -> None:
    try:
        program[name].value = (float(value[0]), float(value[1]), float(value[2]))
    except Exception:
        pass


def _set_bone_palette(program: Any, palette: Any) -> None:
    if palette is None:
        return
    program["u_bones"].write(np.ascontiguousarray(np.transpose(palette, (0, 2, 1)), dtype="f4").tobytes())


def _is_toon_shading(node: Mapping[str, Any], context: Mapping[str, Any]) -> bool:
    """Check whether toon/MToon shading is requested for this render."""
    # Check node materials section for toon/mtoon style
    materials = node.get("materials") if isinstance(node.get("materials"), Mapping) else {}
    style = str(materials.get("style") or "").lower().strip()
    if style in ("toon", "mtoon"):
        return True
    # Check context-level materials
    ctx_materials = context.get("materials") if isinstance(context.get("materials"), Mapping) else {}
    ctx_style = str(ctx_materials.get("style") or "").lower().strip()
    if ctx_style in ("toon", "mtoon"):
        return True
    # Default: enable toon shading for all model3d rendering
    return True


def _set_toon_uniforms(program: Any, node: Mapping[str, Any]) -> None:
    """Set MToon-style toon shading uniforms on a program."""
    import math
    # Defaults inspired by VRM MToon shader
    _set_vec3_uniform(program, "u_shade_color", (0.7, 0.65, 0.75))
    _set_scalar_uniform(program, "u_shade_toony", 0.9)
    _set_scalar_uniform(program, "u_shade_shift", -0.05)
    _set_scalar_uniform(program, "u_rim_power", 5.0)
    _set_vec3_uniform(program, "u_rim_color", (1.0, 1.0, 1.0))
    _set_vec3_uniform(program, "u_ambient", (0.12, 0.12, 0.14))
    # Normalized light direction: top-right key light
    inv_len = 1.0 / math.sqrt(0.3 * 0.3 + 1.0 * 1.0 + 0.5 * 0.5)
    _set_vec3_uniform(program, "u_light_dir", (0.3 * inv_len, 1.0 * inv_len, 0.5 * inv_len))
    # Camera position: approximate from node or default front-facing
    _set_vec3_uniform(program, "u_camera_pos", (0.0, 0.0, 5.0))
    # Allow node-level overrides
    materials = node.get("materials") if isinstance(node.get("materials"), Mapping) else {}
    toon = materials.get("toon") if isinstance(materials.get("toon"), Mapping) else {}
    if toon:
        shade_color = _point3(toon.get("shade_color"))
        if shade_color is not None:
            _set_vec3_uniform(program, "u_shade_color", shade_color)
        if "shade_toony" in toon:
            try:
                _set_scalar_uniform(program, "u_shade_toony", float(toon["shade_toony"]))
            except Exception:
                pass
        if "shade_shift" in toon:
            try:
                _set_scalar_uniform(program, "u_shade_shift", float(toon["shade_shift"]))
            except Exception:
                pass
        if "rim_power" in toon:
            try:
                _set_scalar_uniform(program, "u_rim_power", float(toon["rim_power"]))
            except Exception:
                pass
        rim_color = _point3(toon.get("rim_color"))
        if rim_color is not None:
            _set_vec3_uniform(program, "u_rim_color", rim_color)
        ambient = _point3(toon.get("ambient"))
        if ambient is not None:
            _set_vec3_uniform(program, "u_ambient", ambient)


def _skin_gpu_resources_toon(state: Mapping[str, Any], ctx: Any, program: Any, preview: Mapping[str, Any], meta: Mapping[str, Any] | None) -> dict[str, Any] | None:
    """Build skin GPU resources for the toon shader (different vertex layout with normals)."""
    cache = state.get("skin_geometry_toon")
    if not isinstance(cache, dict):
        return None
    key = _skin_geometry_cache_key(preview, meta)
    cached = cache.get(key)
    if cached is not None:
        return cached
    geometry = _skin_static_geometry(preview, meta)
    if geometry is None:
        return None
    packed = np.concatenate((
        geometry["positions"],
        geometry["uvs"],
        geometry["joints0"],
        geometry["weights0"],
        geometry["joints1"],
        geometry["weights1"],
    ), axis=1).astype(np.float32, copy=False)
    vbo = ctx.buffer(np.ascontiguousarray(packed, dtype="f4").tobytes())
    batches: list[tuple[str, Any, Any]] = []
    for material_name, indices in geometry["batches"]:
        ibo = ctx.buffer(np.ascontiguousarray(indices, dtype=np.int32).tobytes())
        vao = ctx.vertex_array(
            program,
            [(vbo, "3f 2f 4f 4f 4f 4f", "in_pos", "in_uv", "in_joints0", "in_weights0", "in_joints1", "in_weights1")],
            index_buffer=ibo,
        )
        batches.append((material_name, vao, ibo))
    resource = {
        "vbo": vbo,
        "joint_names": geometry["joint_names"],
        "positions": geometry["positions"],
        "batches": batches,
    }
    if len(cache) >= _SKIN_CACHE_LIMIT:
        cache.clear()
    cache[key] = resource
    return resource


def _draw_skin_batches(
    state: Mapping[str, Any],
    ctx: Any,
    program: Any,
    moderngl: Any,
    model: Mapping[str, Any],
    resources: Mapping[str, Any],
) -> bool:
    batches = resources.get("batches")
    if not isinstance(batches, list) or not batches:
        return False
    drawn = False
    opaque_batches: list[Any] = []
    transparent_batches: list[Any] = []
    for batch in batches:
        material_name = str(batch[0]) if batch else ""
        if _material_is_transparent(model, material_name):
            transparent_batches.append(batch)
        else:
            opaque_batches.append(batch)

    def draw_batch(batch: Any) -> bool:
        material_name, vao, _ibo = batch
        texture_ref = _material_texture_ref(model, str(material_name))
        if texture_ref is None:
            return False
        texture = _texture_resource(state, ctx, moderngl, texture_ref)
        if texture is None:
            return False
        try:
            use = getattr(texture, "use", None)
            if callable(use):
                use(location=0)
        except Exception:
            pass
        try:
            program["u_texture"].value = 0
        except Exception:
            pass
        _set_uniform(program, "u_tint", (1.0, 1.0, 1.0, 1.0))
        try:
            vao.render(getattr(moderngl, "TRIANGLES", 4))
            return True
        except Exception:
            return False

    for batch in opaque_batches:
        drawn = draw_batch(batch) or drawn
    if transparent_batches:
        previous_depth_mask = None
        try:
            previous_depth_mask = getattr(ctx, "depth_mask")
            ctx.depth_mask = False
        except Exception:
            previous_depth_mask = None
        try:
            for batch in transparent_batches:
                drawn = draw_batch(batch) or drawn
        finally:
            try:
                ctx.depth_mask = True if previous_depth_mask is None else previous_depth_mask
            except Exception:
                pass
    return drawn


def _read_image(fbo: Any, width: int, height: int) -> Any:
    if Image is None:
        return None
    data = fbo.read(components=4, alignment=1)
    image = Image.frombytes("RGBA", (width, height), data)
    transpose = getattr(Image, "Transpose", None)
    flip = getattr(transpose, "FLIP_TOP_BOTTOM", None) if transpose is not None else None
    if flip is None:
        flip = getattr(Image, "FLIP_TOP_BOTTOM", None)
    return image.transpose(flip) if flip is not None else image


def render_moderngl_model3d(node: Mapping[str, Any], context: Mapping[str, Any]) -> Any:
    """Render a preview mesh using a standalone ModernGL context."""

    if _DISABLED or np is None or Image is None:
        return None
    try:
        width = max(1, int(context.get("width") or node.get("width") or 320))
        height = max(1, int(context.get("height") or node.get("height") or 480))
    except Exception:
        width, height = 320, 480
    vertices, faces, preview = _preview_from_context(context)
    if len(vertices) < 3 or not faces:
        return None
    model = context.get("model") if isinstance(context.get("model"), Mapping) else {}
    pose_state = _pose_state_from_context(preview, node, context, model)
    accent = _accent_color(context)
    projected: list[tuple[float, float, float]] = []
    textured_batches: list[tuple[Any, Any, tuple[float, float, float, float]]] = []
    triangle_batches: list[tuple[Any, tuple[float, float, float, float]]] = []
    line_batches: list[tuple[Any, tuple[float, float, float, float]]] = []

    with _RENDER_LOCK, _get_wgl_serialize_lock():
        state = _ensure_state()
        if state is None:
            return None
        ctx = state["ctx"]
        program = state["program"]
        texture_program = state.get("texture_program")
        skin_texture_program = state.get("skin_texture_program")
        moderngl = state["moderngl"]
        skin_resources = _skin_gpu_resources(state, ctx, skin_texture_program, preview, model) if skin_texture_program is not None else None
        gpu_projection = (
            _gpu_projection_params(skin_resources.get("positions"), width, height, node)
            if isinstance(skin_resources, Mapping)
            else None
        )
        bone_palette = (
            _bone_palette(skin_resources.get("joint_names"), pose_state)
            if isinstance(skin_resources, Mapping) and pose_state
            else None
        )
        use_gpu_skin = (
            isinstance(skin_resources, Mapping)
            and gpu_projection is not None
            and bone_palette is not None
            and len(skin_resources.get("joint_names") or ()) <= _MAX_GPU_BONES
            and bool(skin_resources.get("batches"))
        )
        if not use_gpu_skin:
            vertices = _deform_vertices(vertices, preview, node, model)
            projected = _project_vertices(vertices, width, height, node)
            if len(projected) != len(vertices):
                return None
            textured_batches = _mesh_textured_arrays(projected, faces, width, height, preview, model)
            if not textured_batches:
                triangle_batches, line_batches = _mesh_arrays_by_color(projected, faces, width, height, preview, accent, model)
            if not textured_batches and not triangle_batches:
                return None
        # Determine toon shading mode
        toon = _is_toon_shading(node, context)
        skin_toon_program = state.get("skin_toon_program") if toon else None
        texture_toon_program = state.get("texture_toon_program") if toon else None

        # Build toon-specific skin resources when toon + gpu skin
        skin_resources_toon = None
        if toon and use_gpu_skin and skin_toon_program is not None:
            skin_resources_toon = _skin_gpu_resources_toon(state, ctx, skin_toon_program, preview, model)

        # MSAA 4x framebuffer setup with fallback to non-MSAA
        fbo_ms = fbo_resolve = color_ms = depth_ms = tex_resolve = depth_resolve = None
        use_msaa = False
        try:
            color_ms = ctx.renderbuffer((width, height), 4, samples=4)
            depth_ms = ctx.depth_renderbuffer((width, height), samples=4)
            fbo_ms = ctx.framebuffer(color_attachments=[color_ms], depth_attachment=depth_ms)
            tex_resolve = ctx.texture((width, height), 4, dtype="f1")
            try:
                depth_resolve = ctx.depth_renderbuffer((width, height))
                fbo_resolve = ctx.framebuffer(color_attachments=[tex_resolve], depth_attachment=depth_resolve)
            except Exception:
                depth_resolve = None
                fbo_resolve = ctx.framebuffer(color_attachments=[tex_resolve])
            use_msaa = True
        except Exception:
            # MSAA not supported, fall back to non-MSAA
            for res in (fbo_ms, fbo_resolve, color_ms, depth_ms, tex_resolve, depth_resolve):
                release = getattr(res, "release", None)
                if callable(release):
                    try:
                        release()
                    except Exception:
                        pass
            fbo_ms = fbo_resolve = color_ms = depth_ms = tex_resolve = depth_resolve = None

        # Non-MSAA fallback path
        tex = depth = fbo = None
        if not use_msaa:
            try:
                tex = ctx.texture((width, height), 4, dtype="f1")
                try:
                    depth = ctx.depth_renderbuffer((width, height))
                    fbo = ctx.framebuffer(color_attachments=[tex], depth_attachment=depth)
                except Exception:
                    depth = None
                    fbo = ctx.framebuffer(color_attachments=[tex])
            except Exception:
                return None

        render_fbo = fbo_ms if use_msaa else fbo
        has_depth = (depth_ms is not None) if use_msaa else (depth is not None)
        try:
            render_fbo.use()
            render_fbo.clear(0.0, 0.0, 0.0, 0.0)
            enable = getattr(ctx, "enable", None)
            if callable(enable):
                try:
                    enable(getattr(moderngl, "BLEND", 0))
                except Exception:
                    pass
                if (textured_batches or use_gpu_skin) and has_depth:
                    try:
                        enable(getattr(moderngl, "DEPTH_TEST", 0))
                    except Exception:
                        pass
            try:
                ctx.blend_func = (
                    getattr(moderngl, "SRC_ALPHA", 770),
                    getattr(moderngl, "ONE_MINUS_SRC_ALPHA", 771),
                )
            except Exception:
                pass
            if use_gpu_skin and isinstance(skin_resources, Mapping):
                # Choose toon or standard program for skin path
                if toon and skin_toon_program is not None and isinstance(skin_resources_toon, Mapping):
                    active_program = skin_toon_program
                    active_resources = skin_resources_toon
                    _set_vec2_uniform(active_program, "u_viewport", (float(width), float(height)))
                    _set_vec3_uniform(active_program, "u_center", gpu_projection["center"])
                    _set_vec3_uniform(active_program, "u_rotation", gpu_projection["rotation"])
                    _set_scalar_uniform(active_program, "u_scale", gpu_projection["scale"])
                    _set_scalar_uniform(active_program, "u_depth_scale", gpu_projection["depth_scale"])
                    _set_bone_palette(active_program, bone_palette)
                    _set_toon_uniforms(active_program, node)
                    if not _draw_skin_batches(state, ctx, active_program, moderngl, model, active_resources):
                        return None
                elif skin_texture_program is not None:
                    _set_vec2_uniform(skin_texture_program, "u_viewport", (float(width), float(height)))
                    _set_vec3_uniform(skin_texture_program, "u_center", gpu_projection["center"])
                    _set_vec3_uniform(skin_texture_program, "u_rotation", gpu_projection["rotation"])
                    _set_scalar_uniform(skin_texture_program, "u_scale", gpu_projection["scale"])
                    _set_scalar_uniform(skin_texture_program, "u_depth_scale", gpu_projection["depth_scale"])
                    _set_bone_palette(skin_texture_program, bone_palette)
                    if not _draw_skin_batches(state, ctx, skin_texture_program, moderngl, model, skin_resources):
                        return None
                else:
                    return None
            else:
                if textured_batches:
                    # Choose toon or standard program for textured non-skin path
                    if toon and texture_toon_program is not None:
                        active_tex_program = texture_toon_program
                        _set_toon_uniforms(active_tex_program, node)
                    else:
                        active_tex_program = texture_program
                    if active_tex_program is not None:
                        for texture_ref, array, tint in textured_batches:
                            texture = _texture_resource(state, ctx, moderngl, texture_ref)
                            _draw_textured_array(ctx, active_tex_program, moderngl, array, texture, tint)
                for triangles, color in triangle_batches:
                    _draw_array(ctx, program, getattr(moderngl, "TRIANGLES", 4), triangles, color)
                if not textured_batches:
                    try:
                        ctx.line_width = 1.6
                    except Exception:
                        pass
                    for lines, color in line_batches:
                        _draw_array(ctx, program, getattr(moderngl, "LINES", 1), lines, color)

            # MSAA resolve: blit multisample FBO to single-sample resolve FBO
            if use_msaa and fbo_resolve is not None:
                ctx.copy_framebuffer(fbo_resolve, fbo_ms)
                return _read_image(fbo_resolve, width, height)
            return _read_image(render_fbo, width, height)
        except Exception:
            return None
        finally:
            for resource in (fbo_ms, fbo_resolve, color_ms, depth_ms, tex_resolve, depth_resolve, fbo, depth, tex):
                release = getattr(resource, "release", None)
                if callable(release):
                    try:
                        release()
                    except Exception:
                        pass


def register_builtin_renderers(register: Any) -> tuple[str, ...]:
    """Register the bundled ModernGL renderer when optional deps are present."""

    if _DISABLED or np is None or Image is None or not callable(register):
        return ()
    register(_RENDERER_NAME, render_moderngl_model3d)
    return (_RENDERER_NAME,)


def reset_builtin_renderer_resources(
    *,
    clear_import_cache: bool = False,
    reset_failures: bool = False,
) -> dict[str, Any]:
    """Release persistent ModernGL resources owned by the current thread.

    Per-frame textures, framebuffers, vertex buffers, and VAOs are already
    released in the render path. This hook handles longer-lived context/program
    state once no model3d layers are visible, so plugin enable/disable cycles do
    not pin GPU resources indefinitely.
    """

    global _GLOBAL_FAILED, _MODERNGL, _IMPORT_ERROR
    released: list[str] = []
    with _RENDER_LOCK, _get_wgl_serialize_lock():
        state = getattr(_TLS, "state", None)
        if isinstance(state, Mapping):
            textures = state.get("textures")
            if isinstance(textures, Mapping):
                for texture in list(textures.values()):
                    if texture is not None and _release_resource(texture):
                        released.append("texture")
            for cache_name in ("skin_geometry", "skin_geometry_toon"):
                geometry_cache = state.get(cache_name)
                if isinstance(geometry_cache, Mapping):
                    for item in list(geometry_cache.values()):
                        if not isinstance(item, Mapping):
                            continue
                        for resource in (item.get("vbo"),):
                            if resource is not None and _release_resource(resource):
                                released.append("skin_vbo")
                        batches = item.get("batches")
                        if isinstance(batches, list):
                            for _material_name, vao, ibo in batches:
                                if vao is not None and _release_resource(vao):
                                    released.append("skin_vao")
                                if ibo is not None and _release_resource(ibo):
                                    released.append("skin_ibo")
            for key in ("skin_toon_program", "texture_toon_program", "skin_texture_program", "texture_program", "program", "ctx"):
                resource = state.get(key)
                if resource is not None and _release_resource(resource):
                    released.append(key)
        _TLS.__dict__.clear()
        _TOPOLOGY_CACHE.clear()
        _ALPHA_STATS_CACHE.clear()
        if reset_failures:
            _GLOBAL_FAILED = False
            _IMPORT_ERROR = ""
        if clear_import_cache:
            _MODERNGL = None
    return {
        "ok": True,
        "renderer": _RENDERER_NAME,
        "released": tuple(released),
        "topology_cache_size": len(_TOPOLOGY_CACHE),
        "alpha_stats_cache_size": len(_ALPHA_STATS_CACHE),
    }


def _reset_for_tests() -> None:
    reset_builtin_renderer_resources(clear_import_cache=True, reset_failures=True)


__all__ = [
    "register_builtin_renderers",
    "render_moderngl_model3d",
    "reset_builtin_renderer_resources",
]
