# -*- coding: utf-8 -*-
"""Offline-safe model3d backend probe helpers.

This module intentionally does not download or initialize native render
dependencies. It only inspects the local bundle layout so plugin overlay load
can stay safe when model3d assets or AssimpNet files are absent.
"""
from __future__ import annotations

import hashlib
import json
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
_BACKEND_STATUS_CACHE: Model3DBackendStatus | None = None
_MODEL_METADATA_CACHE: dict[tuple[Any, ...], dict[str, Any]] = {}
_ACTION_METADATA_CACHE: dict[tuple[Any, ...], dict[str, Any]] = {}


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
    """Resolve an action JSON path relative to cwd, model dir, and workspace.

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
    for key in ("data", "selected", "model", "action"):
        item = out.get(key)
        if isinstance(item, Mapping):
            out[key] = dict(item)
    errors = out.get("errors")
    if isinstance(errors, tuple):
        out["errors"] = list(errors)
    return out


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
    cache_key = (str(resolved), fmt, reload_key, exists, size, mtime_ns)
    cached = _MODEL_METADATA_CACHE.get(cache_key)
    if cached is not None:
        return _copy_metadata(cached)
    metadata = {
        "path": path_text,
        "resolved_path": str(resolved),
        "format": fmt,
        "reload_key": reload_key,
        "exists": exists,
        "size": size,
        "mtime_ns": mtime_ns,
        "extension": resolved.suffix.lower(),
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
    metadata = {
        "name": name,
        "data": dict(data),
        "selected": dict(selected or {}),
        "file": action_file,
        "resolved_file": str(action_path) if action_file else "",
        "errors": tuple(errors),
        "cache_key": cache_key,
    }
    if errors:
        metadata["selected"]["_load_error"] = "; ".join(errors)
    _cache_put(_ACTION_METADATA_CACHE, cache_key, metadata)
    return _copy_metadata(metadata)


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
    return key, tuple(lines)


__all__ = [
    "Model3DBackendStatus",
    "clear_model3d_metadata_caches",
    "diagnose_model3d_node",
    "get_action_metadata",
    "get_backend_status",
    "get_model_metadata",
    "model3d_node_key",
    "model3d_path",
    "resolve_action_path",
    "resolve_model_path",
    "probe_model3d_backend",
]
