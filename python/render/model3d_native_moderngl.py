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
    for raw in value[:8192]:
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
    raw_vertices = preview.get("vertices") if isinstance(preview, Mapping) else ()
    vertices = [point for point in (_point3(item) for item in (raw_vertices or ())) if point is not None]
    faces = _faces(preview.get("faces"), len(vertices)) if isinstance(preview, Mapping) else []
    return vertices, faces, preview


def _deform_vertices(
    vertices: list[tuple[float, float, float]],
    preview: Mapping[str, Any],
    node: Mapping[str, Any],
) -> list[tuple[float, float, float]]:
    fn = getattr(_software, "_deform_vertices", None) if _software is not None else None
    if not callable(fn):
        return vertices
    try:
        return fn(vertices, preview, node)
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
        projected, _scale = fn(vertices, width, height, node)
        return list(projected)
    except Exception:
        return []


def _ndc(point: tuple[float, float, float], width: int, height: int) -> tuple[float, float]:
    return (
        (float(point[0]) / max(1.0, float(width))) * 2.0 - 1.0,
        1.0 - (float(point[1]) / max(1.0, float(height))) * 2.0,
    )


def _mesh_arrays(
    projected: list[tuple[float, float, float]],
    faces: list[list[int]],
    width: int,
    height: int,
) -> tuple[Any, Any]:
    if np is None:
        return None, None
    triangles: list[tuple[float, float]] = []
    lines: list[tuple[float, float]] = []
    ordered = sorted(faces, key=lambda face: sum(projected[idx][2] for idx in face) / len(face))
    for face in ordered:
        pts = [_ndc(projected[index], width, height) for index in face]
        for index in range(1, len(pts) - 1):
            triangles.extend((pts[0], pts[index], pts[index + 1]))
        for index, point in enumerate(pts):
            lines.extend((point, pts[(index + 1) % len(pts)]))
    tri_arr = np.asarray(triangles, dtype="f4") if triangles else None
    line_arr = np.asarray(lines, dtype="f4") if lines else None
    return tri_arr, line_arr


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
    vertices = _deform_vertices(vertices, preview, node)
    projected = _project_vertices(vertices, width, height, node)
    if len(projected) != len(vertices):
        return None
    triangles, lines = _mesh_arrays(projected, faces, width, height)
    if triangles is None or getattr(triangles, "size", 0) <= 0:
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
            _draw_array(ctx, program, getattr(moderngl, "TRIANGLES", 4), triangles, (0.42, 0.86, 0.94, 0.26))
            try:
                ctx.line_width = 1.6
            except Exception:
                pass
            _draw_array(ctx, program, getattr(moderngl, "LINES", 1), lines, (0.48, 0.88, 1.0, 0.92))
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


def _reset_for_tests() -> None:
    global _GLOBAL_FAILED, _MODERNGL, _IMPORT_ERROR
    _GLOBAL_FAILED = False
    _MODERNGL = None
    _IMPORT_ERROR = ""
    _TLS.__dict__.clear()


__all__ = [
    "register_builtin_renderers",
    "render_moderngl_model3d",
]
