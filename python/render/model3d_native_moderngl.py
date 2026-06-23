# -*- coding: utf-8 -*-
"""Windowless ModernGL renderer for ``model3d`` preview meshes.

This provider renders into an offscreen framebuffer and returns a PIL RGBA
image.  It never owns a platform window, z-order, or input; presentation still
belongs to the unified overlay compositor.
"""
from __future__ import annotations

import os
import threading
from collections.abc import Mapping
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
        except Exception:
            _GLOBAL_FAILED = True
            _TLS.failed = True
            return None
    state = {
        "ctx": ctx,
        "program": program,
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


def _ndc(point: tuple[float, float, float], width: int, height: int) -> tuple[float, float]:
    return (
        (float(point[0]) / max(1.0, float(width))) * 2.0 - 1.0,
        1.0 - (float(point[1]) / max(1.0, float(height))) * 2.0,
    )


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
    vertices = _deform_vertices(vertices, preview, node, model)
    projected = _project_vertices(vertices, width, height, node)
    if len(projected) != len(vertices):
        return None
    accent = _accent_color(context)
    triangle_batches, line_batches = _mesh_arrays_by_color(projected, faces, width, height, preview, accent, model)
    if not triangle_batches:
        return None

    with _RENDER_LOCK, _get_wgl_serialize_lock():
        state = _ensure_state()
        if state is None:
            return None
        ctx = state["ctx"]
        program = state["program"]
        moderngl = state["moderngl"]
        tex = fbo = None
        try:
            tex = ctx.texture((width, height), 4, dtype="f1")
            fbo = ctx.framebuffer(color_attachments=[tex])
            fbo.use()
            fbo.clear(0.0, 0.0, 0.0, 0.0)
            enable = getattr(ctx, "enable", None)
            if callable(enable):
                try:
                    enable(getattr(moderngl, "BLEND", 0))
                except Exception:
                    pass
            try:
                ctx.blend_func = (
                    getattr(moderngl, "SRC_ALPHA", 770),
                    getattr(moderngl, "ONE_MINUS_SRC_ALPHA", 771),
                )
            except Exception:
                pass
            for triangles, color in triangle_batches:
                _draw_array(ctx, program, getattr(moderngl, "TRIANGLES", 4), triangles, color)
            try:
                ctx.line_width = 1.6
            except Exception:
                pass
            for lines, color in line_batches:
                _draw_array(ctx, program, getattr(moderngl, "LINES", 1), lines, color)
            return _read_image(fbo, width, height)
        except Exception:
            return None
        finally:
            for resource in (fbo, tex):
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
            for key in ("program", "ctx"):
                resource = state.get(key)
                if resource is not None and _release_resource(resource):
                    released.append(key)
        _TLS.__dict__.clear()
        _TOPOLOGY_CACHE.clear()
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
    }


def _reset_for_tests() -> None:
    reset_builtin_renderer_resources(clear_import_cache=True, reset_failures=True)


__all__ = [
    "register_builtin_renderers",
    "render_moderngl_model3d",
    "reset_builtin_renderer_resources",
]
