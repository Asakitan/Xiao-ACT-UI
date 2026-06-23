# -*- coding: utf-8 -*-
"""Offscreen native model3d renderer registry.

This module is deliberately windowless.  Native backends may register a
renderer that returns an RGBA image for the current ``model3d`` node, but they
must do their work offscreen and let the existing unified-overlay compositor
own presentation, z-order, and input.
"""
from __future__ import annotations

import importlib
from collections.abc import Callable, Mapping
from dataclasses import asdict, is_dataclass
from threading import RLock
from typing import Any

try:
    from PIL import Image
except Exception:  # pragma: no cover - PIL is optional at import time
    Image = None  # type: ignore[assignment]

try:
    from render import model3d_backend as _backend
except Exception:  # pragma: no cover - backend is optional at import time
    _backend = None  # type: ignore[assignment]


NativeModel3DRenderer = Callable[[Mapping[str, Any], Mapping[str, Any]], Any]

_LOCK = RLock()
_RENDERERS: list[tuple[str, NativeModel3DRenderer]] = []
_BUILTIN_BOOTSTRAPPED = False
_BUILTIN_PROVIDER_MODULES = ("render.model3d_native_moderngl",)
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
    global _BUILTIN_BOOTSTRAPPED
    with _LOCK:
        _RENDERERS.clear()
        _BUILTIN_BOOTSTRAPPED = False
        _LAST_STATUS.update({
            "available": False,
            "renderer": "",
            "reason": "no native offscreen model3d renderer registered",
            "errors": [],
        })


def native_model3d_renderer_status() -> dict[str, Any]:
    with _LOCK:
        return dict(_LAST_STATUS)


def bootstrap_builtin_native_model3d_renderers(*, force: bool = False) -> dict[str, Any]:
    """Lazily register bundled offscreen renderers when present.

    Built-in providers are optional and must obey the same contract as external
    renderers: return an RGBA image and never own a window, z-order, or input.
    Missing provider modules are treated as "no renderer bundled" rather than
    an error so model3d diagnostics/fallbacks stay quiet.
    """

    global _BUILTIN_BOOTSTRAPPED
    with _LOCK:
        if _BUILTIN_BOOTSTRAPPED and not force:
            return {
                "bootstrapped": True,
                "renderer_count": len(_RENDERERS),
                "errors": [],
            }
        _BUILTIN_BOOTSTRAPPED = True

    errors: list[str] = []
    for module_name in _BUILTIN_PROVIDER_MODULES:
        try:
            module = importlib.import_module(module_name)
        except ModuleNotFoundError as exc:
            if getattr(exc, "name", "") != module_name:
                errors.append(f"{module_name}: {exc}")
            continue
        except Exception as exc:
            errors.append(f"{module_name}: {exc}")
            continue
        registrar = getattr(module, "register_builtin_renderers", None)
        if not callable(registrar):
            continue
        try:
            registrar(register_native_model3d_renderer)
        except Exception as exc:
            errors.append(f"{module_name}.register_builtin_renderers: {exc}")

    with _LOCK:
        if errors and not _RENDERERS:
            _LAST_STATUS.update({
                "available": False,
                "renderer": "",
                "reason": "builtin native model3d bootstrap failed",
                "errors": errors,
            })
        return {
            "bootstrapped": True,
            "renderer_count": len(_RENDERERS),
            "errors": list(errors),
        }


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


def _safe_backend_call(name: str, node: Mapping[str, Any], errors: list[str]) -> Any:
    if _backend is None:
        return None
    fn = getattr(_backend, name, None)
    if not callable(fn):
        return None
    try:
        return fn(node)
    except Exception as exc:
        errors.append(f"{name}: {exc}")
        return None


def _backend_status_context(errors: list[str]) -> dict[str, Any]:
    if _backend is None or not callable(getattr(_backend, "get_backend_status", None)):
        return {"available": False, "reason": "model3d backend unavailable"}
    try:
        status = _backend.get_backend_status()
    except Exception as exc:
        errors.append(f"get_backend_status: {exc}")
        return {"available": False, "reason": str(exc)}
    if is_dataclass(status):
        return dict(asdict(status))
    if isinstance(status, Mapping):
        return dict(status)
    return {
        "available": bool(getattr(status, "render_available", False)),
        "backend": str(getattr(status, "backend", "")),
        "files_present": bool(getattr(status, "files_present", False)),
        "render_available": bool(getattr(status, "render_available", False)),
        "reason": str(getattr(status, "reason", "")),
    }


def build_native_model3d_context(
    node: Mapping[str, Any],
    *,
    palette: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """Build the plain-data context consumed by native/offscreen providers."""

    width, height = _node_size(node)
    errors: list[str] = []
    model_meta = _safe_backend_call("get_model_metadata", node, errors)
    action_meta = _safe_backend_call("get_action_metadata", node, errors)
    retarget_plan = _safe_backend_call("get_retarget_plan", node, errors)
    pose = _safe_backend_call("evaluate_retarget_pose", node, errors)
    context: dict[str, Any] = {
        "width": width,
        "height": height,
        "palette": dict(palette or {}),
        "surface": "unioverlay",
        "presentation": "existing_compositor_layer",
        "offscreen": True,
        "windowless": True,
        "model": dict(model_meta) if isinstance(model_meta, Mapping) else {},
        "action": dict(action_meta) if isinstance(action_meta, Mapping) else {},
        "retarget": dict(retarget_plan) if isinstance(retarget_plan, Mapping) else {},
        "pose": dict(pose) if isinstance(pose, Mapping) else {},
        "backend": _backend_status_context(errors),
        "errors": errors,
    }
    model = context["model"]
    action = context["action"]
    retarget = context["retarget"]
    context["signatures"] = {
        "model": tuple(model.get("cache_key") or ()) if isinstance(model, Mapping) else (),
        "action": tuple(action.get("cache_key") or ()) if isinstance(action, Mapping) else (),
        "retarget": (
            tuple(retarget.get("bone_names") or ()) if isinstance(retarget, Mapping) else ()
        ),
    }
    return context


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
        has_explicit_renderers = bool(_RENDERERS)
    if not has_explicit_renderers:
        bootstrap_builtin_native_model3d_renderers()

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

    context = build_native_model3d_context(node, palette=palette)
    width = int(context.get("width") or _node_size(node)[0])
    height = int(context.get("height") or _node_size(node)[1])
    errors: list[str] = []
    context_errors = context.get("errors")
    if isinstance(context_errors, list):
        errors.extend(str(item) for item in context_errors)
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
    "bootstrap_builtin_native_model3d_renderers",
    "build_native_model3d_context",
    "clear_native_model3d_renderers",
    "native_model3d_renderer_status",
    "register_native_model3d_renderer",
    "try_render_native_model3d_node",
    "unregister_native_model3d_renderer",
]
