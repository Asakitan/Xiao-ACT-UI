# -*- coding: utf-8 -*-
"""Offline-safe model3d backend probe helpers.

This module intentionally does not download or initialize native render
dependencies. It only inspects the local bundle layout so plugin overlay load
can stay safe when model3d assets or AssimpNet files are absent.
"""
from __future__ import annotations

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


def diagnose_model3d_node(
    plugin_id: Any,
    node: Mapping[str, Any],
    *,
    order: int = 0,
    status: Model3DBackendStatus | None = None,
) -> tuple[str, tuple[str, ...]]:
    """Return ``(key, diagnostic lines)`` for a model3d node."""

    backend = status or probe_model3d_backend()
    key = model3d_node_key(plugin_id, node, order)
    path_text = model3d_path(node)
    lines = [f"model3d {key}"]
    if path_text:
        lines.append(f"model: {Path(path_text).name or path_text}")
        path = resolve_model_path(path_text)
        if not path.exists():
            lines.append("model file is missing")
    else:
        lines.append("model path is empty")
    if not backend.render_available:
        lines.append(backend.reason)
    return key, tuple(lines)


__all__ = [
    "Model3DBackendStatus",
    "diagnose_model3d_node",
    "model3d_node_key",
    "model3d_path",
    "resolve_model_path",
    "probe_model3d_backend",
]
