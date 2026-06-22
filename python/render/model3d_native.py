# -*- coding: utf-8 -*-
"""Offscreen native model3d renderer registry.

This module is deliberately windowless.  Native backends may register a
renderer that returns an RGBA image for the current ``model3d`` node, but they
must do their work offscreen and let the existing unified-overlay compositor
own presentation, z-order, and input.
"""
from __future__ import annotations

from collections.abc import Callable, Mapping
from threading import RLock
from typing import Any

try:
    from PIL import Image
except Exception:  # pragma: no cover - PIL is optional at import time
    Image = None  # type: ignore[assignment]


NativeModel3DRenderer = Callable[[Mapping[str, Any], Mapping[str, Any]], Any]

_LOCK = RLock()
_RENDERERS: list[tuple[str, NativeModel3DRenderer]] = []
_LAST_STATUS: dict[str, Any] = {
    "available": False,
    "renderer": "",
    "reason": "no native offscreen model3d renderer registered",
    "errors": [],
}


def _node_size(node: Mapping[str, Any]) -> tuple[int, int]:
    try:
        width = max(1, int(node.get("width") or 320))
    except Exception:
        width = 320
    try:
        height = max(1, int(node.get("height") or 480))
    except Exception:
        height = 480
    return width, height


def _set_status(**updates: Any) -> None:
    with _LOCK:
        next_status = dict(_LAST_STATUS)
        next_status.update(updates)
        if "errors" not in next_status or next_status["errors"] is None:
            next_status["errors"] = []
        _LAST_STATUS.clear()
        _LAST_STATUS.update(next_status)


def register_native_model3d_renderer(name: str, renderer: NativeModel3DRenderer) -> None:
    """Register a windowless renderer.

    Re-registering the same name replaces the previous callable.  The most
    recently registered renderer is tried first so tests or feature-gated
    backends can temporarily override older providers.
    """

    if not callable(renderer):
        raise TypeError("renderer must be callable")
    clean_name = str(name or "").strip() or "native"
    with _LOCK:
        _RENDERERS[:] = [(n, fn) for n, fn in _RENDERERS if n != clean_name]
        _RENDERERS.append((clean_name, renderer))
        _LAST_STATUS.update({
            "available": True,
            "renderer": clean_name,
            "reason": "registered",
            "errors": [],
        })


def unregister_native_model3d_renderer(name: str) -> None:
    clean_name = str(name or "").strip()
    with _LOCK:
        _RENDERERS[:] = [(n, fn) for n, fn in _RENDERERS if n != clean_name]
        if not _RENDERERS:
            _LAST_STATUS.update({
                "available": False,
                "renderer": "",
                "reason": "no native offscreen model3d renderer registered",
                "errors": [],
            })


def clear_native_model3d_renderers() -> None:
    with _LOCK:
        _RENDERERS.clear()
        _LAST_STATUS.update({
            "available": False,
            "renderer": "",
            "reason": "no native offscreen model3d renderer registered",
            "errors": [],
        })


def native_model3d_renderer_status() -> dict[str, Any]:
    with _LOCK:
        return dict(_LAST_STATUS)


def _image_from_result(result: Any, width: int, height: int) -> Any:
    if result is None or Image is None:
        return None
    if isinstance(result, Mapping):
        result = result.get("image")
    if result is None or not hasattr(result, "size"):
        return None
    try:
        image = result.convert("RGBA") if getattr(result, "mode", "") != "RGBA" else result.copy()
        if image.size != (width, height):
            image = image.resize((width, height), Image.LANCZOS)
        return image
    except Exception:
        return None


def try_render_native_model3d_node(
    node: Mapping[str, Any],
    *,
    palette: Mapping[str, Any] | None = None,
) -> Any:
    """Try registered offscreen renderers and return a PIL image or ``None``.

    The context is intentionally plain data so a backend can run in Python, a
    C-extension, or a managed/native bridge without owning any platform window.
    """

    if Image is None:
        _set_status(available=False, reason="PIL unavailable", errors=[])
        return None

    with _LOCK:
        renderers = list(_RENDERERS)
    if not renderers:
        _set_status(
            available=False,
            renderer="",
            reason="no native offscreen model3d renderer registered",
            errors=[],
        )
        return None

    width, height = _node_size(node)
    context = {
        "width": width,
        "height": height,
        "palette": dict(palette or {}),
        "surface": "unioverlay",
        "presentation": "existing_compositor_layer",
        "offscreen": True,
    }
    errors: list[str] = []
    for name, renderer in reversed(renderers):
        try:
            image = _image_from_result(renderer(node, context), width, height)
        except Exception as exc:
            errors.append(f"{name}: {exc}")
            continue
        if image is not None:
            _set_status(available=True, renderer=name, reason="rendered", errors=errors)
            return image
        errors.append(f"{name}: no RGBA image returned")
    _set_status(
        available=bool(renderers),
        renderer=renderers[-1][0] if renderers else "",
        reason="registered renderers did not produce an image",
        errors=errors,
    )
    return None


__all__ = [
    "NativeModel3DRenderer",
    "clear_native_model3d_renderers",
    "native_model3d_renderer_status",
    "register_native_model3d_renderer",
    "try_render_native_model3d_node",
    "unregister_native_model3d_renderer",
]
