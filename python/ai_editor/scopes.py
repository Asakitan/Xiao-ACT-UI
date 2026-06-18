"""Three-level scope system aligned with VSCode Copilot.

Scopes (all merged, later scopes override earlier by ID):
  1. System    ~/.sao/                     global, cross-project
  2. Workspace <BASE_DIR>/.sao/            per-project
  3. Plugin    plugins/<id>/.sao/          per-plugin workspace

Modes (VSCode-aligned):
  - chat   conversation only, no file/terminal tools
  - edit   can read/edit with confirmation on mutating ops
  - agent  full autonomous, mutating tools auto-allowed

Permissions per tool:
  - allowed   execute without asking
  - confirm   ask user before executing
  - disabled  tool hidden from LLM
"""

from __future__ import annotations

import os
import time as _time
from typing import Any, Dict, List, Optional

# Module-level cache for resolve_scopes (avoids repeated filesystem walks)
_scope_cache_result: Optional[List[Dict[str, Any]]] = None
_scope_cache_time: float = 0.0
_SCOPE_CACHE_TTL: float = 30.0


def _base_dir() -> str:
    try:
        from config import BASE_DIR
        return BASE_DIR
    except Exception:
        return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _home_sao() -> str:
    return os.path.join(os.path.expanduser("~"), ".sao")


# ── Scope resolution ──

def resolve_scopes() -> List[Dict[str, Any]]:
    """Return all scope directories in merge order (system -> workspace -> plugins).

    Results are cached for 30 seconds to avoid repeated filesystem walks.
    """
    global _scope_cache_result, _scope_cache_time
    now = _time.monotonic()
    if _scope_cache_result is not None and (now - _scope_cache_time) < _SCOPE_CACHE_TTL:
        return _scope_cache_result

    scopes: List[Dict[str, Any]] = []

    scopes.append({
        "scope": "system", "path": _home_sao(), "label": "System",
    })

    base = _base_dir()
    scopes.append({
        "scope": "workspace", "path": os.path.join(base, ".sao"),
        "label": "Workspace",
    })

    for parent in ("plugins", "user_plugins"):
        pdir = os.path.join(base, parent)
        if not os.path.isdir(pdir):
            continue
        for name in sorted(os.listdir(pdir)):
            full = os.path.join(pdir, name)
            if os.path.isdir(full):
                scopes.append({
                    "scope": "plugin", "path": os.path.join(full, ".sao"),
                    "label": f"Plugin: {name}", "plugin_id": name,
                })

    _scope_cache_result = scopes
    _scope_cache_time = now
    return scopes


def scope_subdirs(subdir: str) -> List[Dict[str, Any]]:
    """Resolve a subdir (e.g. 'agents', 'instructions') across all scopes.

    Returns list of ``{scope, path, label, ...}`` where *path* is the
    full directory (may not exist yet).
    """
    result = []
    for s in resolve_scopes():
        entry = dict(s)
        entry["path"] = os.path.join(s["path"], subdir)
        result.append(entry)
    return result


# ── Modes ──

MODES = ("agent", "ask", "plan")

_MODE_ALIASES: Dict[str, str] = {"chat": "ask", "edit": "plan"}

MUTATING_TOOLS = frozenset({
    "editFile", "runTerminal", "editor_setContent",
    "manageTodoList",
})

READ_TOOLS = frozenset({
    "readFile", "listFiles", "searchFiles",
    "editor_getContent", "editor_getSelection",
    "webFetch",
})

ALWAYS_TOOLS = frozenset({
    "askQuestion", "taskComplete", "getConfirmation", "engine",
})

MODE_PERMISSIONS: Dict[str, Dict[str, str]] = {
    "agent": {
        **{t: "allowed" for t in READ_TOOLS},
        **{t: "allowed" for t in MUTATING_TOOLS},
        **{t: "allowed" for t in ALWAYS_TOOLS},
    },
    "ask": {
        **{t: "disabled" for t in MUTATING_TOOLS},
        **{t: "allowed" for t in READ_TOOLS},
        **{t: "allowed" for t in ALWAYS_TOOLS},
    },
    "plan": {
        **{t: "allowed" for t in READ_TOOLS},
        **{t: "confirm" for t in MUTATING_TOOLS},
        **{t: "allowed" for t in ALWAYS_TOOLS},
    },
}


def normalize_mode(mode: str) -> str:
    """Map legacy mode names to current ones."""
    return _MODE_ALIASES.get(mode, mode) if mode not in MODES else mode


def effective_permissions(mode: str,
                          overrides: Optional[Dict[str, str]] = None,
                          ) -> Dict[str, str]:
    """Merge mode defaults with user overrides."""
    mode = normalize_mode(mode)
    base = dict(MODE_PERMISSIONS.get(mode, MODE_PERMISSIONS["agent"]))
    if overrides:
        base.update(overrides)
    return base


def tool_permission(mode: str, tool_name: str,
                    overrides: Optional[Dict[str, str]] = None,
                    read_only: Optional[bool] = None,
                    category: str = "") -> str:
    mode = normalize_mode(mode)
    perms = effective_permissions(mode, overrides)
    explicit = perms.get(tool_name)
    if explicit in {"allowed", "confirm", "disabled"}:
        return explicit

    selected_mode = mode if mode in MODES else "agent"
    if selected_mode == "ask":
        return "disabled"
    if selected_mode == "agent":
        return "allowed"

    normalized_category = (category or "").strip().lower()
    if normalized_category.startswith("ext:") or normalized_category.startswith("mcp:"):
        return "confirm"

    if read_only is True:
        return "allowed"
    return "confirm"
