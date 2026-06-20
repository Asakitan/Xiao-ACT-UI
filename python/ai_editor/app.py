"""SAO AI Editor — standalone pywebview GUI application.

Launch:
    python -m ai_editor.app            # standalone
    python -m ai_editor.app --attach   # attached to running SAO instance

From SAO menu, ``_toggle_ai_editor_panel`` calls ``launch()`` which opens
the pywebview window in a background thread if not already running.
"""

from __future__ import annotations

import json
import inspect
import math
import os
import re
import sys
import threading
import time
from typing import Any, Callable, Dict, List, Optional

# Ensure package root on path
_HERE = os.path.dirname(__file__)
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from ai_editor.llm_engine import LLMEngine, ProviderConfig, StreamDelta
from ai_editor.tool_registry import ToolRegistry, normalize_tool_parameters
from ai_editor.engine_tools import register_engine_tools
from ai_editor.chat_state import ChatController, Conversation, ChatMessage
from ai_editor.chat_providers import (
    build_provider_runtime_config,
    describe_provider_status,
    provider_runtime_cli_path,
)


# ---------------------------------------------------------------------------
# Settings helpers
# ---------------------------------------------------------------------------

_PROVIDER_CONFIG_KEYS = {
    "provider", "api_key", "base_url", "model", "temperature", "max_tokens",
    "system_prompt", "transport", "top_p", "frequency_penalty", "presence_penalty", "stop",
    "max_input_tokens", "max_output_tokens", "timeout", "extra_headers", "extra_body",
}

_TRANSIENT_CONFIG_KEYS = {"_provider_keys", "context_window"}
_AI_EDITOR_MIN_SIZE = (600, 400)
_AI_EDITOR_WINDOW_TITLE = "SAO AI Editor"

_MODE_VALUES = {"agent", "ask", "plan", "chat", "edit"}
_APPROVAL_VALUES = {"default", "bypass", "autopilot"}
_ENGINE_TRANSPORT_VALUES = {"chat_completions", "responses"}
_CODEX_TRANSPORT_VALUES = {"chat_completions", "responses", "cli"}
_DANGEROUS_ENGINE_ACTIONS = {"settings_set", "eval", "exec"}
_STALE_ANTHROPIC_DEFAULT_MODELS = {"claude-sonnet-4-20250514"}
_WORKSPACE_TREE_IGNORED_DIRS = {
    ".git", ".hg", ".svn", ".idea", ".vscode",
    "__pycache__", ".pytest_cache", ".mypy_cache", ".ruff_cache",
    ".tox", ".nox", ".venv", "venv", "env", "node_modules",
    "build", "dist", "publish", "out", "tmp", "temp",
}
_WORKSPACE_FILE_PREVIEW_BYTES = 1024 * 1024
_EDITOR_LANGUAGE_BY_EXT = {
    ".py": "python", ".pyi": "python", ".pyw": "python",
    ".js": "javascript", ".mjs": "javascript", ".cjs": "javascript",
    ".ts": "typescript", ".tsx": "typescript",
    ".json": "json", ".html": "html", ".htm": "html",
    ".css": "css", ".scss": "css", ".sass": "css",
    ".md": "markdown", ".markdown": "markdown",
    ".yml": "yaml", ".yaml": "yaml",
    ".xml": "xml", ".sql": "sql",
    ".sh": "shell", ".ps1": "shell", ".bat": "shell", ".cmd": "shell",
    ".lua": "lua", ".c": "c", ".cc": "cpp", ".cpp": "cpp",
    ".cxx": "cpp", ".h": "cpp", ".hpp": "cpp", ".cs": "csharp",
    ".java": "java", ".go": "go", ".rs": "rust", ".toml": "toml",
}
_AI_EDITOR_LAYOUT_DEFAULTS: Dict[str, Any] = {
    "sidebarVisible": True,
    "editorVisible": False,
    "chatVisible": True,
    "panelHeight": "",
    "sidebarWidth": "",
    "rightSidebarWidth": "",
    "activeSidebarPanel": "chat",
    "activeBottomTab": "terminal",
    "minimapVisible": False,
}

_AI_EDITOR_SECTION_DEFAULTS: Dict[str, Dict[str, Any]] = {
    "claude_code": {
        "cli_path": "",
        "cli_args": [],
        "prefer_cli": False,
        "allow_dangerously_skip_permissions": False,
        "model": "",
    },
    "codex": {
        "cli_path": "",
        "cli_args": [],
        "transport": "chat_completions",
        "model": "codex-mini-latest",
    },
    "mcp": {
        "access": "prompt",
        "autostart": False,
        "discovery_enabled": True,
        "collision_behavior": "first",
        "server_sampling": False,
    },
    "terminal": {
        "profile": "PowerShell 7 (No Profile)",
        "shell_path": "",
        "shell_args": [],
        "timeout": 30,
        "output_limit": 8000,
        "auto_approve": {},
    },
    "extensions": {
        "confirm_install": True,
        "allowed_publishers": [],
        "blocked_publishers": [],
        "enabled_contributions": ["chatParticipants", "languageModelTools", "commands", "views"],
    },
    "customization": {
        "instructions_locations": [".sao/instructions.md", ".sao/instructions"],
        "agent_locations": [".sao/agents"],
        "workflow_locations": [".sao/workflows"],
        "skill_locations": [".agents/skills/.local", ".claude/skills/.local"],
        "use_agent_md": True,
        "use_claude_md": False,
    },
}


def _as_dict(value: Any) -> Dict[str, Any]:
    return dict(value) if isinstance(value, dict) else {}


def _as_list(value: Any) -> List[Any]:
    return list(value) if isinstance(value, list) else []


def _as_float(value: Any, default: float) -> float:
    try:
        number = float(value)
        return number if math.isfinite(number) else default
    except (TypeError, ValueError):
        return default


def _as_int(value: Any, default: int) -> int:
    try:
        return int(value)
    except (TypeError, ValueError, OverflowError):
        return default


def _as_bool(value: Any, default: bool = False) -> bool:
    if value is None:
        return default
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return bool(value)
    if isinstance(value, str):
        normalized = value.strip().lower()
        if normalized in {"1", "true", "yes", "on", "enabled"}:
            return True
        if normalized in {"0", "false", "no", "off", "disabled"}:
            return False
    return default


def _normalize_stop(value: Any) -> List[str]:
    if isinstance(value, str):
        return [part.strip() for part in value.split(",") if part.strip()]
    if isinstance(value, list):
        return [str(part) for part in value if str(part)]
    return []


def _normalize_cli_args(value: Any) -> List[str]:
    if isinstance(value, str):
        return [part.strip() for part in value.splitlines() if part.strip()]
    if isinstance(value, list):
        return [str(part).strip() for part in value if str(part).strip()]
    return []


def _normalize_transport(value: Any, default: str = "chat_completions",
                         allowed: Optional[set[str]] = None) -> str:
    allowed_values = allowed or _ENGINE_TRANSPORT_VALUES
    raw = str(value or default).strip().lower().replace("-", "_")
    if raw in {"openai_responses", "response"}:
        raw = "responses"
    return raw if raw in allowed_values else default


def _normalize_cli_provider_section(section: str,
                                    value: Dict[str, Any]) -> Dict[str, Any]:
    cfg = dict(value)
    cfg["cli_path"] = str(cfg.get("cli_path") or "").strip()
    cfg["cli_args"] = _normalize_cli_args(cfg.get("cli_args", cfg.get("args", [])))
    cfg["model"] = str(cfg.get("model") or "").strip()
    if section == "claude_code":
        cfg["prefer_cli"] = _as_bool(cfg.get("prefer_cli"), False)
        cfg["allow_dangerously_skip_permissions"] = _as_bool(
            cfg.get("allow_dangerously_skip_permissions"), False)
    if section == "codex":
        cfg["transport"] = _normalize_transport(
            cfg.get("transport"), "chat_completions", _CODEX_TRANSPORT_VALUES)
    return cfg


def _normalize_approval(value: Any, default: str = "default") -> str:
    raw = str(value or default).strip().lower()
    return raw if raw in _APPROVAL_VALUES else default


def _normalize_layout_state(value: Any) -> Dict[str, Any]:
    state = dict(_AI_EDITOR_LAYOUT_DEFAULTS)
    if isinstance(value, dict):
        state.update(value)
    state["sidebarVisible"] = _as_bool(state.get("sidebarVisible"), True)
    state["editorVisible"] = _as_bool(state.get("editorVisible"), False)
    state["chatVisible"] = _as_bool(state.get("chatVisible"), True)
    state["panelHeight"] = str(state.get("panelHeight") or "")
    state["sidebarWidth"] = str(state.get("sidebarWidth") or "")
    state["rightSidebarWidth"] = str(state.get("rightSidebarWidth") or "")
    state["activeSidebarPanel"] = str(
        state.get("activeSidebarPanel") or "chat")
    active_bottom_tab = str(state.get("activeBottomTab") or "terminal")
    state["activeBottomTab"] = active_bottom_tab if active_bottom_tab in {
        "terminal", "output", "problems"
    } else "terminal"
    state["minimapVisible"] = _as_bool(state.get("minimapVisible"), False)
    return state


def _normalize_ai_editor_config(raw: Any) -> Dict[str, Any]:
    """Return a backward-compatible AI Editor config dict.

    The user settings file can contain older or partially written values. Keep
    unknown keys for forward compatibility, but normalize values consumed by the
    runtime and UI so saving endpoint settings does not corrupt sibling fields.
    """
    cfg = dict(raw) if isinstance(raw, dict) else {}
    if ("provider_keys" not in cfg or not isinstance(cfg.get("provider_keys"), dict)):
        legacy_provider_keys = cfg.get("_provider_keys")
        if isinstance(legacy_provider_keys, dict):
            cfg["provider_keys"] = dict(legacy_provider_keys)
    cfg["provider"] = str(cfg.get("provider") or "openai")
    cfg["api_key"] = str(cfg.get("api_key") or "")
    cfg["base_url"] = str(cfg.get("base_url") or "")
    cfg["model"] = str(cfg.get("model") or "")
    cfg["temperature"] = _as_float(cfg.get("temperature"), 0.7)
    cfg["max_tokens"] = _as_int(cfg.get("max_tokens"), 4096)
    cfg["system_prompt"] = str(cfg.get("system_prompt") or "")
    cfg["transport"] = _normalize_transport(cfg.get("transport"))
    cfg["top_p"] = _as_float(cfg.get("top_p"), 1.0)
    cfg["frequency_penalty"] = _as_float(cfg.get("frequency_penalty"), 0.0)
    cfg["presence_penalty"] = _as_float(cfg.get("presence_penalty"), 0.0)
    cfg["stop"] = _normalize_stop(cfg.get("stop"))
    cfg["max_input_tokens"] = _as_int(cfg.get("max_input_tokens"), 0)
    cfg["max_output_tokens"] = _as_int(cfg.get("max_output_tokens"), 0)
    cfg["timeout"] = _as_int(cfg.get("timeout"), 180)
    cfg["extra_headers"] = _as_dict(cfg.get("extra_headers"))
    cfg["extra_body"] = _as_dict(cfg.get("extra_body"))
    cfg["provider_keys"] = _as_dict(cfg.get("provider_keys"))
    cfg["custom_models"] = _as_dict(cfg.get("custom_models"))
    cfg["permissions"] = _as_dict(cfg.get("permissions"))
    cfg["approval"] = _normalize_approval(cfg.get("approval"), "default")
    cfg["active_chat_provider"] = str(
        cfg.get("active_chat_provider") or "chat").strip() or "chat"
    if "layout" in cfg:
        cfg["layout"] = _normalize_layout_state(cfg.get("layout"))
    if "mode" in cfg:
        mode = str(cfg.get("mode") or "edit").strip().lower()
        cfg["mode"] = mode if mode in _MODE_VALUES else "agent"
    if "theme" in cfg:
        cfg["theme"] = str(cfg.get("theme") or "")
    for section, defaults in _AI_EDITOR_SECTION_DEFAULTS.items():
        if section in cfg:
            current = _as_dict(cfg.get(section))
            merged = dict(defaults)
            merged.update(current)
            if section in {"claude_code", "codex"}:
                merged = _normalize_cli_provider_section(section, merged)
            cfg[section] = merged
    return cfg


def _merge_ai_editor_config(existing: Any, incoming: Any) -> Dict[str, Any]:
    merged = dict(existing) if isinstance(existing, dict) else {}
    if isinstance(incoming, dict):
        for key, value in incoming.items():
            if key in _TRANSIENT_CONFIG_KEYS:
                if key == "_provider_keys" and "provider_keys" not in incoming:
                    merged["provider_keys"] = _as_dict(value)
                continue
            merged[key] = value
    return _normalize_ai_editor_config(merged)


_STANDALONE_SETTINGS = None


def _resolve_settings(gui_ref: Any = None):
    settings = getattr(gui_ref, 'settings', None) if gui_ref else None
    if settings:
        return settings
    settings = getattr(gui_ref, '_cfg_settings_ref', None) if gui_ref else None
    if settings:
        return settings
    global _STANDALONE_SETTINGS
    if _STANDALONE_SETTINGS is None:
        try:
            from config import SettingsManager
            _STANDALONE_SETTINGS = SettingsManager()
        except Exception:
            _STANDALONE_SETTINGS = False
    return None if _STANDALONE_SETTINGS is False else _STANDALONE_SETTINGS


def load_provider_config(gui_ref: Any = None) -> ProviderConfig:
    """Shared helper: build a ProviderConfig from settings.

    Used by both ``AIEditorAPI`` (pywebview) and ``AIEditorBridge``
    (C# webview) to avoid duplicated config-parsing logic.
    """
    settings = _resolve_settings(gui_ref)
    if not settings:
        return ProviderConfig()
    raw = _normalize_ai_editor_config(settings.get("ai_editor", {}) or {})
    custom_models = raw.get("custom_models", {})
    if custom_models:
        from ai_editor.llm_engine import set_custom_models
        set_custom_models(custom_models)
    return ProviderConfig(
        provider=raw["provider"],
        api_key=raw["api_key"],
        base_url=raw["base_url"],
        model=raw["model"],
        temperature=raw["temperature"],
        max_tokens=raw["max_tokens"],
        system_prompt=raw["system_prompt"],
        transport=raw["transport"],
        top_p=raw["top_p"],
        frequency_penalty=raw["frequency_penalty"],
        presence_penalty=raw["presence_penalty"],
        stop=raw["stop"],
        max_input_tokens=raw["max_input_tokens"],
        max_output_tokens=raw["max_output_tokens"],
        timeout=raw["timeout"],
        extra_headers=raw["extra_headers"],
        extra_body=raw["extra_body"],
    )


# ---------------------------------------------------------------------------
# UIBridge implementation — delegates vscode API UI calls to the webview
# ---------------------------------------------------------------------------

class _AIEditorUIBridge:
    """Concrete UIBridge that delegates to AIEditorAPI._emit / _eval_js.

    Created with a *forward reference* to the API so it can be constructed
    during ``AIEditorAPI.__init__`` before ``_emit`` / ``_eval_js`` are usable
    (they become usable once ``set_window`` is called).
    """

    def __init__(self, api: "AIEditorAPI") -> None:
        self._api = api

    # -- Output channel --
    def show_output(self, channel_name: str, content: str) -> None:
        self._api._emit("show_output", {"name": channel_name, "content": content})

    def clear_output(self, channel_name: str) -> None:
        self._api._emit("clear_output", {"name": channel_name})

    def dispose_output(self, channel_name: str) -> None:
        self._api._emit("dispose_output", {"name": channel_name})

    # -- Terminal --
    def show_terminal(self, name: str) -> None:
        self._api._eval_js(
            "document.querySelectorAll('.ptab').forEach(p=>p.classList.toggle('active',p.dataset.ptab==='terminal'));"
            "var tp=document.getElementById('terminal-panel');if(tp){tp.style.display='flex';tp.classList.add('active')}"
        )

    def hide_terminal(self, name: str) -> None:
        self._api._eval_js(
            "var tp=document.getElementById('terminal-panel');if(tp){tp.style.display='none';tp.classList.remove('active')}"
        )

    def run_terminal_command(self, name: str, text: str) -> Optional[str]:
        result = self._api.execute_tool(
            "runTerminal", json.dumps({"command": text}), True)
        return result

    # -- Messages / toasts --
    def show_message(self, level: str, message: str) -> None:
        self._api._emit("show_message", {"level": level, "message": message})

    def confirm_tool_invocation(self, tool_name: str,
                                confirmation: Dict[str, Any],
                                input_data: Any) -> bool:
        if not getattr(self._api, "_window", None):
            return False
        title = str(
            confirmation.get("title")
            or confirmation.get("message")
            or f"Run tool {tool_name}?"
        )
        message = str(
            confirmation.get("message")
            or confirmation.get("detail")
            or json.dumps(input_data, ensure_ascii=False, default=str)
        )
        script = (
            "(function(){"
            f"return window.confirm({json.dumps(title + chr(10) + chr(10) + message)});"
            "})()"
        )
        return bool(self._api._eval_js(script))

    # -- Progress --
    def show_progress(self, message: Optional[str],
                      increment: Optional[float]) -> None:
        self._api._emit("show_progress", {"message": message, "increment": increment})

    # -- Status bar --
    def show_status_bar_item(self, item_id: str, text: str,
                             tooltip: str, command: str) -> None:
        self._api._emit("show_status_bar_item", {
            "id": item_id, "text": text, "tooltip": tooltip, "command": command})

    def hide_status_bar_item(self, item_id: str) -> None:
        self._api._emit("hide_status_bar_item", {"id": item_id})

    def dispose_status_bar_item(self, item_id: str) -> None:
        self._api._emit("dispose_status_bar_item", {"id": item_id})

    # -- Webview panels --
    def render_webview_panel(self, view_id: str, html: str) -> None:
        """Push HTML content for a webview panel to the frontend."""
        self._api._emit("render_webview_panel", {"view_id": view_id, "html": html})

    def dispose_webview_panel(self, view_id: str) -> None:
        """Dispose a webview panel by emitting a dispose event to the frontend."""
        self._api._emit("dispose_webview_panel", {"view_id": view_id})

    def post_webview_message(self, view_id: str, message: Any) -> None:
        """Relay a message from the extension to the webview iframe."""
        self._api._emit("webview_message", {
            "view_id": view_id,
            "message": message,
            "raw_message": message,
            "rawMessage": message,
            "data": message,
        })

    def receive_webview_message(self, view_id: str, message: Any) -> None:
        """Relay a message from the webview back to the extension."""
        self._api.webview_post_message(view_id, message)

    # -- Pickers / dialogs --
    def show_quick_pick(self, items: List[Any],
                        options: Dict[str, Any]) -> Any:
        if not items or not getattr(self._api, "_window", None):
            return None
        formatted: List[Dict[str, str]] = []
        for index, item in enumerate(items):
            if isinstance(item, dict):
                label = str(item.get("label") or item.get("name") or item.get("value") or item)
                description = str(item.get("description") or item.get("detail") or "")
            else:
                label = str(item)
                description = ""
            formatted.append({
                "index": str(index),
                "label": label,
                "description": description,
            })
        lines = []
        for entry in formatted:
            line = f"{entry['index']}: {entry['label']}"
            if entry["description"]:
                line += f" — {entry['description']}"
            lines.append(line)
        title = str(options.get("title") or options.get("placeHolder") or "Select an item")
        prompt = title + "\n\n" + "\n".join(lines)
        script = (
            "(function(){"
            f"const answer = window.prompt({json.dumps(prompt)}, '');"
            "if (answer === null) { return null; }"
            "return String(answer);"
            "})()"
        )
        answer = self._api._eval_js(script)
        if answer is None:
            return None
        raw = str(answer).strip()
        if not raw:
            return None
        lookup: Dict[str, Any] = {}
        for index, item in enumerate(items):
            lookup[str(index)] = item
            lookup[formatted[index]["label"]] = item
        if options.get("canPickMany"):
            picks: List[Any] = []
            seen: set[str] = set()
            for token in raw.split(","):
                key = token.strip()
                if not key or key in seen:
                    continue
                item = lookup.get(key)
                if item is None:
                    continue
                seen.add(key)
                picks.append(item)
            return picks or None
        return lookup.get(raw)

    def show_input_box(self, options: Dict[str, Any]) -> Optional[str]:
        if not getattr(self._api, "_window", None):
            return None
        prompt = str(options.get("prompt") or options.get("placeHolder") or "Input")
        value = str(options.get("value") or "")
        script = (
            "(function(){"
            f"const answer = window.prompt({json.dumps(prompt)}, {json.dumps(value)});"
            "if (answer === null) { return null; }"
            "return String(answer);"
            "})()"
        )
        result = self._api._eval_js(script)
        return None if result is None else str(result)


# ---------------------------------------------------------------------------
# JS API exposed to the webview window
# ---------------------------------------------------------------------------

class AIEditorAPI:
    """Python backend exposed to JavaScript via ``window.pywebview.api``."""

    def __init__(self, gui_ref: Any = None) -> None:
        self._gui_ref = gui_ref
        self._engine: Optional[LLMEngine] = None
        self._registry: Optional[ToolRegistry] = None
        self._controller: Optional[ChatController] = None
        self._window = None  # set after window creation
        self._ready = threading.Event()
        self._delta_buf: List[str] = []
        self._thinking_buf: List[str] = []
        self._delta_last_flush: float = 0.0
        self._mode: str = "agent"
        self._perm_overrides: Dict[str, str] = {}
        self._mcp = None
        self._webview_states: Dict[str, Any] = {}
        self._maximized = False
        self._window_geometry: Optional[Dict[str, int]] = None
        self._window_resize_supports_fix_point: Optional[bool] = None

    def set_window(self, window: Any, initial_geometry: Optional[Dict[str, int]] = None) -> None:
        self._window = window
        self._window_resize_supports_fix_point = None
        if isinstance(initial_geometry, dict):
            self._window_geometry = {
                "x": _as_int(initial_geometry.get("x"), 0),
                "y": _as_int(initial_geometry.get("y"), 0),
                "width": max(_AI_EDITOR_MIN_SIZE[0], _as_int(initial_geometry.get("width"), _AI_EDITOR_MIN_SIZE[0])),
                "height": max(_AI_EDITOR_MIN_SIZE[1], _as_int(initial_geometry.get("height"), _AI_EDITOR_MIN_SIZE[1])),
            }
        else:
            self._capture_window_geometry(prefer_live=True)
        self._ready.set()

    def _capture_window_geometry(self, prefer_live: bool = False) -> Dict[str, int]:
        min_w, min_h = _AI_EDITOR_MIN_SIZE
        cached = self._window_geometry if isinstance(self._window_geometry, dict) else {}

        def _coerce(raw: Any, fallback: int) -> int:
            try:
                return int(raw)
            except (TypeError, ValueError, OverflowError):
                return int(fallback)

        def _read(name: str, fallback: int) -> int:
            if not prefer_live and name in cached:
                return _coerce(cached.get(name), fallback)
            raw = getattr(self._window, name, None) if self._window else None
            if raw is None and name in cached:
                raw = cached.get(name)
            return _coerce(raw, fallback)

        geom = {
            "x": _read("x", 0),
            "y": _read("y", 0),
            "width": max(min_w, _read("width", min_w)),
            "height": max(min_h, _read("height", min_h)),
        }
        self._window_geometry = geom
        return dict(geom)

    def _window_resize_supports_fix_point_arg(self) -> bool:
        if self._window_resize_supports_fix_point is not None:
            return bool(self._window_resize_supports_fix_point)
        resize = getattr(self._window, "resize", None)
        supports = False
        if callable(resize):
            try:
                supports = "fix_point" in inspect.signature(resize).parameters
            except (TypeError, ValueError):
                supports = False
        self._window_resize_supports_fix_point = supports
        return supports

    def _window_resize_fix_point(self, edge: str) -> Any:
        if not self._window_resize_supports_fix_point_arg():
            return None
        try:
            from webview.window import FixPoint
        except Exception:
            return None
        vertical_anchor = FixPoint.SOUTH if "n" in edge else FixPoint.NORTH
        horizontal_anchor = FixPoint.EAST if "w" in edge else FixPoint.WEST
        return vertical_anchor | horizontal_anchor

    def _apply_window_geometry(self, edge: str, x: int, y: int, width: int, height: int) -> None:
        move = getattr(self._window, "move", None)
        resize = getattr(self._window, "resize", None)
        if not callable(resize):
            raise AttributeError("Window resize API unavailable")

        old = self._capture_window_geometry()
        used_fix_point = False
        fix_point = self._window_resize_fix_point(edge)
        if fix_point is not None:
            try:
                resize(width, height, fix_point)
                used_fix_point = True
            except TypeError:
                self._window_resize_supports_fix_point = False

        if not used_fix_point and (x != old["x"] or y != old["y"]):
            if not callable(move):
                raise AttributeError("Window move API unavailable")
            move(x, y)
        if not used_fix_point:
            resize(width, height)
        self._window_geometry = {"x": x, "y": y, "width": width, "height": height}

    # ── Init ──

    def _ensure_engine(self) -> None:
        if self._controller is not None:
            return
        config = self._load_config_obj()
        self._engine = LLMEngine(config)
        self._registry = ToolRegistry()
        register_engine_tools(self._registry, self._gui_ref or _DummyGui(), api_ref=self)
        self._install_engine_policy()

        # Initialize MCP servers
        self._mcp = None
        try:
            from ai_editor.mcp_client import McpManager, load_mcp_configs
            settings = _resolve_settings(self._gui_ref)
            getter = (lambda k, d=None: settings.get(k, d)) if settings else None
            configs = load_mcp_configs(getter)
            if configs:
                self._mcp = McpManager()
                for cfg in configs:
                    ok = self._mcp.add_server(cfg)
                    if ok:
                        client = self._mcp._clients.get(cfg.id)
                        n = len(client.tools) if client else 0
                        print(f"[MCP] Connected: {cfg.id} ({n} tools)")
        except Exception as exc:
            print(f"[MCP] Init failed: {exc}")

        sp = config.system_prompt or self._default_system_prompt()
        conv = Conversation(system_prompt=sp)
        self._controller = ChatController(self._engine, self._registry, conv)

        self._controller.on_stream_delta = self._on_stream_delta
        self._controller.on_thinking_delta = self._on_thinking_delta
        self._controller.on_stream_end = self._on_stream_end
        self._controller.on_tool_start = self._on_tool_start
        self._controller.on_tool_end = self._on_tool_end
        self._controller.on_tool_confirm = self._on_tool_confirm
        self._controller.on_tool_progress = self._on_tool_progress
        self._controller.on_token_warning = self._on_token_warning
        self._controller.on_error = self._on_error
        self._controller.on_idle = self._on_idle

        self._configure_controller_tooling(self._controller)

        # @-mention variable resolver
        self._controller.resolve_variable = self._resolve_variable

        self._pending_confirm: Dict[str, threading.Event] = {}
        self._confirm_results: Dict[str, bool] = {}
        self._confirmation_timeout = 30.0

        # Load mode from settings
        ai_cfg = _normalize_ai_editor_config(
            self._settings_getter("ai_editor", {}) or {})
        if isinstance(ai_cfg, dict):
            from ai_editor.scopes import normalize_mode
            self._mode = normalize_mode(ai_cfg.get("mode", "agent"))
            self._perm_overrides = ai_cfg.get("permissions", {})
        self._apply_mode_permissions()

        # Chat provider registry + per-provider controllers
        from ai_editor.chat_providers import get_provider_registry
        self._provider_registry = get_provider_registry()
        self._provider_controllers: Dict[str, ChatController] = {}
        saved_provider = str(ai_cfg.get("active_chat_provider") or "chat") if isinstance(ai_cfg, dict) else "chat"
        self._active_provider = saved_provider if self._provider_registry.get(saved_provider) else "chat"

        # Extension host + VSCode API namespace
        # IMPORTANT: _vscode_ns must be fully initialised (including
        # set_ui_bridge) BEFORE the init_extensions thread proceeds,
        # because extension activation and _register_ext_tools both
        # access _vscode_ns.  A threading.Event gates the background
        # thread until the main thread signals readiness.
        from ai_editor.extension_host import get_extension_host
        self._ext_host = get_extension_host()
        self._node_ext_host = None  # NodeExtensionHost, created lazily in _init_extension_host
        self._extensions_inited = False
        self._vscode_ns_ready = threading.Event()

        from ai_editor.vscode_api import VscodeNamespace
        self._vscode_ns = VscodeNamespace(
            self._ext_host, self._engine, self._settings_getter)
        self._vscode_ns.set_ui_bridge(_AIEditorUIBridge(self))
        self._vscode_ns.set_webview_view_change_callback(
            self._on_webview_view_changed)
        self._ext_host.set_command_fallback_resolver(
            self._resolve_extension_command_fallback)
        self._vscode_ns_ready.set()

        threading.Thread(target=self.init_extensions, daemon=True).start()

        # Claude proxy (lazy — started on first CC provider use)
        self._claude_proxy = None

        # Agent & Workflow registries
        from ai_editor.agents import get_agent_registry
        from ai_editor.workflows import get_workflow_registry, WorkflowEngine
        self._agent_registry = get_agent_registry()
        self._wf_registry = get_workflow_registry()
        self._wf_engine = WorkflowEngine(self._engine, self._agent_registry)
        self._active_agent_id: Optional[str] = None

        gui = self._gui_ref or _DummyGui()
        actions = getattr(gui, '_ai_engine_actions', None)
        if not isinstance(actions, dict):
            actions = {}
            gui._ai_engine_actions = actions
        actions["list_agents"] = lambda **kw: self._eng_list_agents()
        actions["invoke_agent"] = lambda **kw: self._eng_invoke_agent(kw)
        actions["list_workflows"] = lambda **kw: self._eng_list_workflows()
        actions["run_workflow"] = lambda **kw: self._eng_run_workflow(kw)

    def _settings_getter(self, key: str, default=None):
        settings = _resolve_settings(self._gui_ref)
        if settings:
            return settings.get(key, default)
        return default

    def _default_system_prompt(self, agent_mode: bool = False,
                               plan_mode: bool = False) -> str:
        from ai_editor.prompts import get_system_prompt
        return get_system_prompt(agent_mode=agent_mode,
                                 plan_mode=plan_mode,
                                 settings_getter=self._settings_getter)

    # ── Config ──

    def _load_config_obj(self) -> ProviderConfig:
        return load_provider_config(self._gui_ref)

    # ── JS-callable methods (window.pywebview.api.*) ──

    def load_config(self) -> Dict:
        cfg = self._load_config_obj()
        # Load theme from ACT panel_themes or ai_editor config
        theme = "dark"
        settings = _resolve_settings(self._gui_ref)
        ai_cfg = {}
        if settings:
            ai_cfg = _normalize_ai_editor_config(settings.get("ai_editor", {}) or {})
            theme = ai_cfg.get("theme", "")
            if not theme:
                themes = settings.get("panel_themes", {}) or {}
                theme = themes.get("act", "dark")
        pkeys = ai_cfg.get("provider_keys", {}) if isinstance(ai_cfg, dict) else {}
        model_name = cfg.model or cfg.effective_model
        ctx = cfg.effective_context
        language = ai_cfg.get("language", "") if isinstance(ai_cfg, dict) else ""
        result = {
            "provider": cfg.provider, "api_key": cfg.api_key,
            "base_url": cfg.base_url, "model": model_name,
            "temperature": cfg.temperature, "max_tokens": cfg.max_tokens,
            "transport": cfg.transport,
            "top_p": cfg.top_p,
            "frequency_penalty": cfg.frequency_penalty,
            "presence_penalty": cfg.presence_penalty,
            "stop": cfg.stop,
            "max_input_tokens": cfg.max_input_tokens,
            "max_output_tokens": cfg.max_output_tokens,
            "timeout": cfg.timeout,
            "extra_headers": cfg.extra_headers,
            "extra_body": cfg.extra_body,
            "system_prompt": cfg.system_prompt,
            "theme": theme,
            "language": language,
            "_provider_keys": pkeys,
            "provider_keys": pkeys,
            "custom_models": ai_cfg.get("custom_models", {}),
            "mode": ai_cfg.get("mode", self._mode),
            "approval": ai_cfg.get("approval", "default"),
            "active_chat_provider": ai_cfg.get(
                "active_chat_provider", getattr(self, "_active_provider", "chat")),
            "permissions": ai_cfg.get("permissions", self._perm_overrides),
            "context_window": ctx,
        }
        for section in _AI_EDITOR_SECTION_DEFAULTS:
            if isinstance(ai_cfg, dict) and section in ai_cfg:
                result[section] = ai_cfg[section]
        if isinstance(ai_cfg, dict) and "layout" in ai_cfg:
            result["layout"] = ai_cfg["layout"]
        return result

    def save_config(self, data: Dict) -> Dict:
        settings = _resolve_settings(self._gui_ref)
        merged = _merge_ai_editor_config({}, data)
        if settings:
            merged = _merge_ai_editor_config(settings.get("ai_editor", {}) or {}, data)
        persist_error = self._persist_ai_editor_config(merged)
        if self._engine:
            self._apply_config_to_engine(merged)
        self._invalidate_provider_controllers(data)
        if isinstance(merged.get("mode"), str):
            from ai_editor.scopes import normalize_mode
            self._mode = normalize_mode(merged.get("mode")) or self._mode
        if isinstance(merged.get("permissions"), dict):
            self._perm_overrides = dict(merged.get("permissions") or {})
        self._apply_mode_permissions()
        if persist_error:
            return {"error": persist_error, "applied": True}
        return {"ok": True}

    def _apply_config_to_engine(self, config: Dict[str, Any]) -> None:
        if not self._engine:
            return
        for k, v in config.items():
            if k in _PROVIDER_CONFIG_KEYS and hasattr(self._engine.config, k):
                setattr(self._engine.config, k, v)

    def _persist_ai_editor_config(self, merged: Dict[str, Any]) -> Optional[str]:
        settings = _resolve_settings(self._gui_ref)
        if not settings:
            return "Settings not available"
        settings.set("ai_editor", merged)
        try:
            settings.save()
        except Exception as exc:
            return str(exc)
        # Push updated settings to Node extension host
        self._notify_node_settings_changed()
        return None

    def _notify_node_settings_changed(self) -> None:
        """Re-sync the full settings dict to the Node extension host.

        Called after any Python-side settings mutation so that
        ``workspace.getConfiguration()`` in Node stays current.
        """
        host = getattr(self, "_node_ext_host", None)
        if host is not None and host.is_running:
            try:
                self._sync_settings_to_node_host()
            except Exception:
                pass

    def _save_config_patch(self, data: Dict[str, Any]) -> Dict[str, Any]:
        settings = _resolve_settings(self._gui_ref)
        current = settings.get("ai_editor", {}) if settings else self.load_config()
        merged = _merge_ai_editor_config(current or {}, data)
        persist_error = self._persist_ai_editor_config(merged)
        self._apply_config_to_engine(merged)
        self._invalidate_provider_controllers(data)
        if isinstance(merged.get("mode"), str):
            from ai_editor.scopes import normalize_mode
            self._mode = normalize_mode(merged.get("mode")) or self._mode
        if isinstance(merged.get("permissions"), dict):
            self._perm_overrides = dict(merged.get("permissions") or {})
        self._apply_mode_permissions()
        if persist_error:
            raise RuntimeError(persist_error)
        return merged

    @staticmethod
    def _default_model_for_provider(provider: str) -> str:
        return ProviderConfig(provider=provider).effective_model

    def _provider_options(self, ai_cfg: Dict[str, Any]) -> List[Dict[str, Any]]:
        from ai_editor.llm_engine import Provider
        current_provider = self._engine.config.provider if self._engine else ai_cfg.get("provider", "openai")
        configured_provider = ai_cfg.get("provider", "")
        active_key_present = bool(ai_cfg.get("api_key"))
        provider_keys = _as_dict(ai_cfg.get("provider_keys"))
        providers = []
        for p in Provider:
            cfg = ProviderConfig(provider=p.value)
            providers.append({
                "id": p.value,
                "name": p.value.replace("_", " ").title(),
                "current": p.value == current_provider,
                "configured": bool(provider_keys.get(p.value)) or (configured_provider == p.value and active_key_present),
                "default_model": cfg.effective_model,
                "base_url": cfg.effective_base_url,
            })
        return providers

    def _model_options(self, current_model: str, custom_models: Dict[str, Any]) -> List[Dict[str, Any]]:
        from ai_editor.llm_engine import get_model_capabilities, get_model_context, list_all_models, Provider
        names: Dict[str, Dict[str, Any]] = {}
        for name, meta in list_all_models().items():
            names[str(name)] = _as_dict(meta)
        for provider in Provider:
            default_model = self._default_model_for_provider(provider.value)
            if default_model:
                names.setdefault(default_model, {})
        if current_model:
            names.setdefault(current_model, {})
        items = []
        for name in sorted(names):
            ctx = get_model_context(name)
            items.append({
                "id": name,
                "name": name,
                "current": name == current_model,
                "custom": name in custom_models,
                "max_input": ctx["max_input"],
                "max_output": ctx["max_output"],
                "capabilities": get_model_capabilities(name),
            })
        return items

    def _current_context_window(self) -> Dict[str, Any]:
        self._ensure_engine()
        cfg = self._engine.config
        model = cfg.effective_model
        ctx = cfg.effective_context
        used = 0
        messages = 0
        if self._controller and self._controller.conversation:
            try:
                api_messages = self._controller.conversation.to_api_messages()
                used = self._engine.count_message_tokens(api_messages)
                messages = len(api_messages)
            except Exception:
                used = 0
                messages = 0
        max_input = max(1, int(ctx.get("max_input", 0) or 1))
        return {
            "model": model,
            "max_input": ctx.get("max_input", 0),
            "max_output": ctx.get("max_output", 0),
            "compact_at": int(max_input * 0.9),
            "used": used,
            "messages": messages,
            "percent": round((used / max_input) * 100, 2) if used else 0,
        }

    def get_chat_controls(self) -> Dict:
        """Return the active chat toolbar selector state in one safe payload."""
        self._ensure_engine()
        ai_cfg = _normalize_ai_editor_config(
            self._settings_getter("ai_editor", {}) or {})
        cfg = self._engine.config
        model = cfg.effective_model
        active_agent = None
        if self._active_agent_id:
            agent = self._agent_registry.get(self._active_agent_id)
            if agent:
                active_agent = agent.to_dict()
        agents = self.list_agents().get("agents", [])
        workflows = self.list_workflows().get("workflows", [])
        chat_providers = self.list_chat_providers().get("providers", [])
        context_window = self._current_context_window()
        custom_models = _as_dict(ai_cfg.get("custom_models"))
        return {
            "provider": cfg.provider,
            "model": model,
            "mode": self._mode,
            "approval": ai_cfg.get("approval", "default"),
            "active_agent_id": self._active_agent_id or "",
            "active_agent": active_agent,
            "active_chat_provider": self._active_provider,
            "providers": self._provider_options(ai_cfg),
            "chat_providers": chat_providers,
            "models": self._model_options(model, custom_models),
            "custom_models": custom_models,
            "agents": agents,
            "workflows": workflows,
            "context_window": context_window,
            "context_window_info": context_window,
            "status": {
                "provider": cfg.provider,
                "model": model,
                "mode": self._mode,
                "active_agent_id": self._active_agent_id or "",
                "active_chat_provider": self._active_provider,
                "tokens": context_window["used"],
                "context_percent": context_window["percent"],
            },
        }

    def set_active_provider(self, provider: str, model: str = "") -> Dict:
        """Set the main chat LLM provider without opening settings."""
        self._ensure_engine()
        from ai_editor.llm_engine import Provider
        provider_id = str(provider or "").strip().lower()
        valid = {p.value for p in Provider}
        if provider_id not in valid:
            return {"error": f"Invalid provider: {provider}. Valid: {sorted(valid)}"}
        selected_model = str(model or "").strip() or self._default_model_for_provider(provider_id)
        patch: Dict[str, Any] = {"provider": provider_id, "model": selected_model}
        try:
            self._save_config_patch(patch)
        except RuntimeError as exc:
            return {"error": str(exc), "applied": True,
                    "provider": provider_id, "model": selected_model,
                    "controls": self.get_chat_controls()}
        return {"ok": True, "provider": provider_id, "model": selected_model,
                "controls": self.get_chat_controls()}

    def set_active_model(self, model: str) -> Dict:
        """Set the main chat model without opening settings."""
        self._ensure_engine()
        selected_model = str(model or "").strip()
        if not selected_model:
            return {"error": "Model is required"}
        try:
            self._save_config_patch({"model": selected_model})
        except RuntimeError as exc:
            return {"error": str(exc), "applied": True,
                    "model": selected_model,
                    "controls": self.get_chat_controls()}
        return {"ok": True, "model": selected_model,
                "controls": self.get_chat_controls()}

    def set_active_mode(self, mode: str) -> Dict:
        """Alias for toolbar callers that use active-control naming."""
        result = self.set_mode(mode)
        if result.get("ok"):
            result["controls"] = self.get_chat_controls()
        return result

    def set_provider_model(self, provider: str = "", model: str = "") -> Dict:
        """Compatibility setter for toolbar code that sends provider+model."""
        if provider:
            return self.set_active_provider(provider, model)
        return self.set_active_model(model)

    def set_chat_provider(self, provider_id: str) -> Dict:
        """Set the active right-sidebar chat provider tab."""
        result = self.switch_provider(provider_id)
        if result.get("ok"):
            result["controls"] = self.get_chat_controls()
        return result

    def webview_post_message(self, view_id: str, message: Any) -> Dict:
        """Relay a message FROM the webview HTML TO the extension.

        Routes through the VS Code namespace bridge so runtime webview views and
        panels receive the same onDidReceiveMessage event shape.
        Token validation: if the message carries a ``_token`` field, it must
        match the nonce stored in ``_vscode_ns._webview_tokens`` for the
        given *view_id*.  Messages without a ``_token`` are allowed through
        (backward compat), but an incorrect token is rejected.
        """
        vscode_ns = getattr(self, "_vscode_ns", None)
        if not vscode_ns:
            return {"error": "vscode namespace not initialized"}
        # Validate webview token when present
        if isinstance(message, dict):
            token = message.get("_token")
            if token is not None:
                expected = vscode_ns._webview_tokens.get(view_id)
                if expected is None or not vscode_ns.verify_webview_token(view_id, str(token)):
                    return {"error": "Invalid webview token", "view_id": view_id}
        delivered = vscode_ns.deliver_webview_message(view_id, message)
        if not delivered:
            return {"error": f"No webview receiver found for: {view_id}"}
        return {"ok": True, "view_id": view_id}

    def get_provider_webview(self, provider_id: str) -> Dict:
        """Return the real provider webview surface when the runtime exposes one."""
        self._ensure_engine()
        provider_id = str(provider_id or "").strip()
        if not provider_id:
            return {"html": "", "view_id": "", "available": False, "state": None}
        self._sync_dynamic_webview_providers()
        provider = self._provider_registry.get(provider_id) if hasattr(self, "_provider_registry") else None
        fallback_id = self._provider_fallback_view_id(provider_id)
        runtime_info = self._provider_runtime_webview_info(provider_id)
        activate_view_id = str(
            runtime_info.get("manifest_view_id")
            or runtime_info.get("declared_view_id")
            or runtime_info.get("runtime_view_id")
            or ""
        )
        if activate_view_id:
            self._activate_provider_view(activate_view_id)
            self._sync_dynamic_webview_providers()
            runtime_info = self._provider_runtime_webview_info(provider_id)
        vscode_ns = getattr(self, "_vscode_ns", None)
        runtime_view_id = str(runtime_info.get("runtime_view_id") or "")
        if vscode_ns and runtime_view_id:
            runtime_html = vscode_ns.get_webview_html(runtime_view_id)
            if runtime_html:
                return {"html": runtime_html, "view_id": runtime_view_id,
                        "available": True, "source": "runtime",
                        "state": self._provider_webview_state(runtime_view_id, fallback_id)}

        manifest_view = runtime_info.get("manifest_view") or {}
        manifest_view_id = str(runtime_info.get("manifest_view_id") or "")
        if manifest_view_id:
            reason = manifest_view.get("_runtimeSupport", {}).get(
                "message",
                "扩展声明了视图，但当前 runtime 尚未注册可读取的 HTML。",
            )
            return {
                "html": "",
                "view_id": manifest_view_id,
                "available": False,
                "source": "manifest",
                "reason": reason,
                "state": self._provider_webview_state(manifest_view_id, fallback_id),
            }

        if provider_id == "copilot":
            reason = "Copilot 走 VS Code 原生 ChatWidget/chat participant，当前没有 WebviewView HTML。"
            if provider is not None:
                reason = str(getattr(provider, "metadata", {}).get("native_chat_reason") or reason)
            return {
                "html": "",
                "view_id": fallback_id,
                "available": False,
                "source": "native-chat",
                "reason": reason,
                "state": self._provider_webview_state("", fallback_id),
            }

        return {
            "html": "",
            "view_id": fallback_id,
            "available": False,
            "source": "missing-runtime",
            "reason": "未找到扩展 runtime 注册的 WebviewView HTML。",
            "state": self._provider_webview_state("", fallback_id),
        }

    def webview_set_state(self, view_id: str, state: Any) -> Dict:
        normalized_view_id = str(view_id or "").strip()
        if not normalized_view_id:
            return {"error": "view_id is required"}
        self._webview_states[normalized_view_id] = state
        return {"ok": True, "view_id": normalized_view_id}

    def set_chat_controls(self, data: Optional[Dict[str, Any]] = None) -> Dict:
        """Apply one or more chat toolbar selector updates in a single call."""
        self._ensure_engine()
        payload = data if isinstance(data, dict) else {}

        provider = str(payload.get("provider") or "").strip()
        model_value = payload.get("model")
        model = str(model_value or "").strip() if model_value is not None else ""
        if provider:
            result = self.set_active_provider(provider, model)
            if result.get("error"):
                return result
        elif model:
            result = self.set_active_model(model)
            if result.get("error"):
                return result

        if "mode" in payload:
            result = self.set_mode(str(payload.get("mode") or ""))
            if result.get("error"):
                return result

        agent_key_present = "agent_id" in payload or "active_agent_id" in payload
        if agent_key_present:
            agent_id = str(payload.get("agent_id", payload.get("active_agent_id", "")) or "").strip()
            result = self._set_active_agent(agent_id) if agent_id else self.clear_active_agent()
            if result.get("error"):
                return result

        chat_provider = str(
            payload.get("chat_provider") or payload.get("active_chat_provider") or ""
        ).strip()
        if chat_provider:
            result = self.switch_provider(chat_provider)
            if result.get("error"):
                return result

        if "approval" in payload:
            approval = _normalize_approval(payload.get("approval"), "default")
            try:
                self._save_config_patch({"approval": approval})
            except RuntimeError as exc:
                return {
                    "error": str(exc),
                    "applied": True,
                    "approval": approval,
                    "controls": self.get_chat_controls(),
                }

        return {"ok": True, "controls": self.get_chat_controls()}

    def _invalidate_provider_controllers(self, data: Dict) -> None:
        if not hasattr(self, "_provider_controllers"):
            return
        provider_keys = set(_PROVIDER_CONFIG_KEYS) | {"provider_keys", "claude_code", "codex"}
        if not any(k in data for k in provider_keys):
            return
        for provider_id in ("copilot", "claude-code", "codex"):
            ctrl = self._provider_controllers.pop(provider_id, None)
            if ctrl:
                try:
                    ctrl.cancel()
                except Exception:
                    pass

    def send_message(self, text: str, config: Optional[Dict] = None, agent_mode: bool = False) -> Dict:
        if not text or not text.strip():
            return {"error": "Empty message"}
        self._ensure_engine()
        if config:
            if config.get("provider"):
                self._engine.config.provider = config["provider"]
            if config.get("model"):
                self._engine.config.model = config["model"]
        # @agent-id prefix → activate agent for this message
        stripped = text.strip()
        if stripped.startswith("@") and " " in stripped:
            mention, rest = stripped.split(" ", 1)
            agent_id = mention[1:]
            if self._agent_registry and self._agent_registry.get(agent_id):
                self._set_active_agent(agent_id)
                stripped = rest.strip()
        self._sync_extension_tools()
        effective_agent = agent_mode or self._mode == "agent"
        is_plan = self._mode == "plan" and not effective_agent
        if effective_agent and self._controller.conversation:
            self._controller.conversation.system_prompt = self._default_system_prompt(agent_mode=True)
        elif is_plan and self._controller.conversation:
            self._controller.conversation.system_prompt = self._default_system_prompt(plan_mode=True)
        self._apply_mode_permissions()
        if self._mode == "agent":
            for t in self._registry.list_tools():
                self._controller._session_auto_approve[t.name] = True
        self._controller.send(stripped, agent_mode=effective_agent)
        return {"ok": True}

    def implement_plan(self) -> Dict:
        """Switch from Plan to Agent mode and execute the last plan."""
        self._ensure_engine()
        old_mode = self._mode
        self._mode = "agent"
        self._save_mode_to_settings()
        self._apply_mode_permissions()
        for t in self._registry.list_tools():
            self._controller._session_auto_approve[t.name] = True
        if self._controller.conversation:
            self._controller.conversation.system_prompt = self._default_system_prompt(agent_mode=True)
        self._controller.send(
            "Implement the plan above. Execute each step. "
            "Read files before editing. Run tests after changes. "
            "Use taskComplete when done.",
            agent_mode=True,
        )
        return {"ok": True, "mode": "agent", "previous_mode": old_mode}

    def _resolve_variable(self, name: str) -> str:
        """Resolve @-mention variables by reading editor state."""
        if name == "selection":
            r = self.editor_get_selection()
            return r.get("selection", "")
        if name == "editor":
            r = self.editor_get_content()
            return r.get("content", "")
        if name == "file":
            if self._window:
                try:
                    return self._window.evaluate_js("editorFileName") or "untitled"
                except Exception:
                    pass
            return "untitled"
        if name == "language":
            r = self.editor_get_language()
            return r.get("language", "plaintext")
        if name == "state":
            r = self.execute_tool("get_game_state", "{}")
            return r
        return ""

    def cancel(self) -> Dict:
        for call_id in list(self._pending_confirm):
            evt = self._pending_confirm.pop(call_id, None)
            if evt:
                evt.set()
        if self._controller:
            self._controller.cancel()
        return {"ok": True}

    # ── Window chrome (frameless) ──

    def win_minimize(self) -> Dict[str, Any]:
        if not self._window:
            return {"ok": False, "error": "No window"}
        try:
            self._window.minimize()
            return {"ok": True, "action": "minimize"}
        except Exception as exc:
            return {"ok": False, "error": str(exc)}

    def win_maximize(self) -> Dict[str, Any]:
        if not self._window:
            return {"ok": False, "error": "No window"}
        try:
            if getattr(self, '_maximized', False):
                self._window.restore()
                self._maximized = False
            else:
                self._window.maximize()
                self._maximized = True
            self._window_geometry = None
            return {"ok": True, "maximized": bool(self._maximized)}
        except Exception as exc:
            return {"ok": False, "error": str(exc)}

    def win_close(self) -> Dict[str, Any]:
        if not self._window:
            return {"ok": False, "error": "No window"}
        try:
            self._window.destroy()
            return {"ok": True, "action": "close"}
        except Exception as exc:
            return {"ok": False, "error": str(exc)}

    def win_resize_by(self, edge: str, dx: int, dy: int) -> Dict[str, Any]:
        if not self._window:
            return {"ok": False, "error": "No window"}
        if getattr(self, '_maximized', False):
            return {"ok": False, "error": "Window is maximized"}
        try:
            edge = str(edge or "").lower()
            delta_x = int(dx or 0)
            delta_y = int(dy or 0)
            min_w, min_h = _AI_EDITOR_MIN_SIZE
            geometry = self._capture_window_geometry()
            x = int(geometry["x"])
            y = int(geometry["y"])
            width = int(geometry["width"])
            height = int(geometry["height"])

            new_x, new_y = x, y
            new_w, new_h = width, height
            if "e" in edge:
                new_w = max(min_w, width + delta_x)
            if "s" in edge:
                new_h = max(min_h, height + delta_y)
            if "w" in edge:
                new_w = max(min_w, width - delta_x)
                new_x = x + (width - new_w)
            if "n" in edge:
                new_h = max(min_h, height - delta_y)
                new_y = y + (height - new_h)

            if new_w != width or new_h != height:
                self._apply_window_geometry(edge, new_x, new_y, new_w, new_h)
            return {"ok": True, "x": new_x, "y": new_y, "width": new_w, "height": new_h}
        except Exception as exc:
            return {"ok": False, "error": str(exc)}

    def open_file_dialog(self) -> Dict:
        """Open a native file dialog to pick an image, return base64."""
        if not self._window:
            return {"error": "No window"}
        import base64 as _b64
        try:
            result = self._window.create_file_dialog(
                dialog_type=10,  # OPEN_DIALOG
                allow_multiple=False,
                file_types=('Image Files (*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp)',),
            )
            if not result:
                return {}
            path = result[0] if isinstance(result, (list, tuple)) else str(result)
            with open(path, "rb") as f:
                data = f.read()
            ext = os.path.splitext(path)[1].lower()
            mime_map = {".png": "image/png", ".jpg": "image/jpeg",
                        ".jpeg": "image/jpeg", ".gif": "image/gif",
                        ".bmp": "image/bmp", ".webp": "image/webp"}
            return {
                "path": path,
                "name": os.path.basename(path),
                "base64": _b64.b64encode(data).decode("ascii"),
                "mime": mime_map.get(ext, "image/png"),
            }
        except Exception as exc:
            return {"error": str(exc)}

    def open_text_file(self) -> Dict:
        """Open a native file dialog and return a UTF-8 text preview."""
        if not self._window:
            return {"error": "No window"}
        try:
            result = self._window.create_file_dialog(
                dialog_type=10,  # OPEN_DIALOG
                allow_multiple=False,
                file_types=(
                    'Text Files (*.txt;*.md;*.py;*.js;*.ts;*.json;*.html;*.css;*.cs;*.xml;*.yaml;*.yml)',
                    'All Files (*.*)',
                ),
            )
            if not result:
                return {"cancelled": True}
            path = result[0] if isinstance(result, (list, tuple)) else str(result)
            with open(path, "rb") as fh:
                data = fh.read(_WORKSPACE_FILE_PREVIEW_BYTES + 1)
            truncated = len(data) > _WORKSPACE_FILE_PREVIEW_BYTES
            content = data[:_WORKSPACE_FILE_PREVIEW_BYTES].decode("utf-8", errors="replace")
            return {
                "ok": True,
                "path": path,
                "name": os.path.basename(path),
                "content": content,
                "language": self._editor_language_for_path(path),
                "truncated": truncated,
            }
        except Exception as exc:
            return {"error": str(exc)}

    def save_file_as(self, content: str, suggested_name: str = "") -> Dict:
        """Show a native Save-As dialog, write *content* to the chosen path."""
        if not self._window:
            return {"error": "No window"}
        try:
            result = self._window.create_file_dialog(
                dialog_type=20,  # SAVE_DIALOG
                save_filename=suggested_name or "untitled.txt",
                file_types=(
                    'Text Files (*.txt;*.md;*.py;*.js;*.ts;*.json;*.html;*.css;*.cs)',
                    'All Files (*.*)',
                ),
            )
            if not result:
                return {"cancelled": True}
            path = result if isinstance(result, str) else (
                result[0] if isinstance(result, (list, tuple)) else str(result))
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(content)
            return {
                "ok": True,
                "path": path,
                "name": os.path.basename(path),
                "language": self._editor_language_for_path(path),
            }
        except Exception as exc:
            return {"error": str(exc)}

    def _workspace_root(self) -> str:
        try:
            from ai_editor.scopes import _base_dir
            return os.path.abspath(_base_dir())
        except Exception:
            return os.path.abspath(os.path.dirname(os.path.dirname(__file__)))

    @staticmethod
    def _workspace_rel_path(root: str, path: str) -> str:
        rel = os.path.relpath(path, root).replace("\\", "/")
        return "" if rel == "." else rel

    @staticmethod
    def _is_workspace_safe_path(root: str, path: str) -> bool:
        try:
            root_real = os.path.realpath(os.path.abspath(root))
            path_real = os.path.realpath(os.path.abspath(path))
            root_norm = os.path.normcase(root_real)
            path_norm = os.path.normcase(path_real)
            return os.path.commonpath([root_norm, path_norm]) == root_norm
        except (OSError, ValueError):
            return False

    def _resolve_workspace_path(self, rel_path: str = "") -> str:
        root = self._workspace_root()
        parts: List[str] = []
        for raw in str(rel_path or "").replace("\\", "/").split("/"):
            part = raw.strip()
            if not part or part == ".":
                continue
            if part == "..":
                raise ValueError("Path escapes workspace")
            parts.append(part)
        full = os.path.abspath(os.path.join(root, *parts))
        if not self._is_workspace_safe_path(root, full):
            raise ValueError("Path escapes workspace")
        return full

    @staticmethod
    def _editor_language_for_path(path: str) -> str:
        return _EDITOR_LANGUAGE_BY_EXT.get(os.path.splitext(path)[1].lower(), "plaintext")

    def list_workspace_tree(self, rel_path: str = "") -> Dict:
        root = self._workspace_root()
        try:
            current = self._resolve_workspace_path(rel_path)
        except ValueError as exc:
            return {"error": str(exc), "entries": []}
        if not os.path.isdir(current):
            return {"error": f"Directory not found: {rel_path}", "entries": []}
        try:
            names = os.listdir(current)
        except Exception as exc:
            return {"error": str(exc), "entries": []}
        entries: List[Dict[str, Any]] = []
        for name in names:
            full = os.path.join(current, name)
            if not self._is_workspace_safe_path(root, full):
                continue
            is_dir = os.path.isdir(full)
            if is_dir and name.casefold() in _WORKSPACE_TREE_IGNORED_DIRS:
                continue
            entries.append({
                "name": name,
                "path": self._workspace_rel_path(root, full),
                "type": "directory" if is_dir else "file",
            })
        entries.sort(key=lambda entry: (
            entry.get("type") != "directory", str(entry.get("name", "")).casefold()))
        return {
            "root": root,
            "root_name": os.path.basename(root.rstrip("\\/")) or root,
            "path": self._workspace_rel_path(root, current),
            "entries": entries,
        }

    def open_workspace_file(self, rel_path: str) -> Dict:
        root = self._workspace_root()
        try:
            full = self._resolve_workspace_path(rel_path)
        except ValueError as exc:
            return {"error": str(exc)}
        if not os.path.isfile(full):
            return {"error": f"File not found: {rel_path}"}
        try:
            with open(full, "rb") as fh:
                data = fh.read(_WORKSPACE_FILE_PREVIEW_BYTES + 1)
        except Exception as exc:
            return {"error": str(exc)}
        truncated = len(data) > _WORKSPACE_FILE_PREVIEW_BYTES
        text = data[:_WORKSPACE_FILE_PREVIEW_BYTES].decode("utf-8", errors="replace")
        rel = self._workspace_rel_path(root, full)
        return {
            "name": os.path.basename(full),
            "path": rel,
            "absolute_path": full,
            "content": text,
            "language": self._editor_language_for_path(full),
            "truncated": truncated,
        }

    def new_chat(self) -> Dict:
        if self._controller:
            sp = self._engine.config.system_prompt if self._engine else ""
            self._controller.new_conversation(sp or self._default_system_prompt())
        return {"ok": True}

    # ── Custom Instructions API ──

    def get_instructions(self) -> Dict:
        """Return user instructions text + project instruction files."""
        from ai_editor.prompts import load_instructions, list_instruction_files
        ai = self._settings_getter("ai_editor", {}) or {}
        user_text = ai.get("user_instructions", "") if isinstance(ai, dict) else ""
        files = list_instruction_files()
        combined = load_instructions(settings_getter=self._settings_getter)
        return {"user_instructions": user_text, "files": files,
                "combined_preview": combined}

    def save_user_instructions(self, text: str) -> Dict:
        """Persist user-level instructions to settings."""
        settings = _resolve_settings(self._gui_ref)
        if not settings:
            return {"error": "Settings not available"}
        ai = settings.get("ai_editor", {}) or {}
        if not isinstance(ai, dict):
            ai = {}
        ai["user_instructions"] = text
        settings.set("ai_editor", ai)
        try:
            settings.save()
        except Exception as exc:
            if self._controller and self._controller.conversation:
                self._controller.conversation.system_prompt = self._default_system_prompt()
            return {
                "error": f"Instructions updated only for the current runtime: {exc}",
                "applied": True,
            }
        if self._controller and self._controller.conversation:
            self._controller.conversation.system_prompt = self._default_system_prompt()
        return {"ok": True}

    def get_instruction_files(self) -> Dict:
        """List .sao/instructions.md and .sao/instructions/*.md files."""
        from ai_editor.prompts import list_instruction_files
        return {"files": list_instruction_files()}

    def save_instruction_file(self, name: str, content: str) -> Dict:
        """Create or update an instruction file under .sao/."""
        from ai_editor.prompts import save_instruction_file as _save
        result = _save(name, content)
        if result.get("ok") and self._controller and self._controller.conversation:
            self._controller.conversation.system_prompt = self._default_system_prompt()
        return result

    def delete_instruction_file(self, name: str) -> Dict:
        """Delete an instruction file."""
        from ai_editor.prompts import delete_instruction_file as _del
        result = _del(name)
        if result.get("ok") and self._controller and self._controller.conversation:
            self._controller.conversation.system_prompt = self._default_system_prompt()
        return result

    # ── Mode & Permission API ──

    def get_mode(self, mode: str = "") -> Dict:
        from ai_editor.scopes import MODES, MODE_PERMISSIONS, tool_permission, normalize_mode
        mode = normalize_mode(mode) if mode else ""
        selected = mode if mode in MODES else self._mode
        registry_tools: Dict[str, Any] = {}
        tool_names = set(MODE_PERMISSIONS.get(selected, MODE_PERMISSIONS["agent"]).keys())
        self._sync_extension_tools()
        if self._registry:
            for tool in self._registry.list_tools(include_disabled=True):
                registry_tools[tool.name] = tool
                tool_names.add(tool.name)
        if self._mcp:
            for t in self._mcp.all_tools():
                tool_names.add(f"mcp_{t.server_id}_{t.name}")

        permissions: Dict[str, str] = {}
        defaults: Dict[str, str] = {}
        for tool_name in sorted(tool_names):
            tool = registry_tools.get(tool_name)
            category = getattr(tool, "category", "") if tool is not None else ""
            read_only = self._tool_read_only(tool) if tool is not None else None
            if tool_name.startswith("mcp_"):
                category = category or "mcp"
                read_only = self._is_probably_read_only_mcp_tool(
                    self._mcp_tool_source_name(tool_name))
            permissions[tool_name] = tool_permission(
                selected,
                tool_name,
                self._perm_overrides,
                read_only=read_only,
                category=category,
            )
            defaults[tool_name] = tool_permission(
                selected,
                tool_name,
                {},
                read_only=read_only,
                category=category,
            )
        return {
            "mode": self._mode,
            "selected_mode": selected,
            "modes": list(MODES),
            "permissions": permissions,
            "defaults": defaults,
            "overrides": dict(self._perm_overrides),
            "tools": sorted(tool_names),
        }

    def set_mode(self, mode: str) -> Dict:
        from ai_editor.scopes import MODES, normalize_mode
        mode = normalize_mode(mode)
        if mode not in MODES:
            return {"error": f"Invalid mode: {mode}. Valid: {list(MODES)}"}
        self._mode = mode
        persist_error = self._save_mode_to_settings()
        self._apply_mode_permissions()
        if persist_error:
            return {
                "error": f"Mode updated only for the current runtime: {persist_error}",
                "applied": True,
                "mode": mode,
            }
        return {"ok": True, "mode": mode}

    def set_tool_permission(self, tool_name: str, permission: str) -> Dict:
        if permission in ("", "default"):
            self._perm_overrides.pop(tool_name, None)
            persist_error = self._save_mode_to_settings()
            self._apply_mode_permissions()
            if persist_error:
                return {
                    "error": f"Permission updated only for the current runtime: {persist_error}",
                    "applied": True,
                }
            return {"ok": True}
        if permission not in ("allowed", "confirm", "disabled"):
            return {"error": "Invalid permission"}
        self._perm_overrides[tool_name] = permission
        persist_error = self._save_mode_to_settings()
        self._apply_mode_permissions()
        if persist_error:
            return {
                "error": f"Permission updated only for the current runtime: {persist_error}",
                "applied": True,
            }
        return {"ok": True}

    def get_scopes(self) -> Dict:
        from ai_editor.scopes import resolve_scopes
        scopes = resolve_scopes()
        for s in scopes:
            s["exists"] = os.path.isdir(s["path"])
        return {"scopes": scopes}

    def _save_mode_to_settings(self) -> Optional[str]:
        settings = _resolve_settings(self._gui_ref)
        if not settings:
            return "Settings not available"
        ai = settings.get("ai_editor", {}) or {}
        if not isinstance(ai, dict):
            ai = {}
        ai["mode"] = self._mode
        ai["permissions"] = self._perm_overrides
        settings.set("ai_editor", ai)
        try:
            settings.save()
        except Exception as exc:
            return str(exc)
        return None

    def _install_engine_policy(self) -> None:
        if not self._registry:
            return
        desc = self._registry.get("engine")
        if not desc or getattr(desc.handler, "_sao_engine_policy", False):
            return
        raw_handler = desc.handler

        def _guarded_engine_handler(**kw):
            action = str(kw.get("action", ""))
            if self._engine_action_blocked(action):
                return {"error": f"Engine action disabled by mode policy: {action}"}
            return raw_handler(**kw)

        setattr(_guarded_engine_handler, "_sao_engine_policy", True)
        desc.handler = _guarded_engine_handler

    @staticmethod
    def _tool_read_only(tool: Any) -> Optional[bool]:
        tags = getattr(tool, "tags", None)
        if isinstance(tags, dict) and isinstance(tags.get("readOnly"), bool):
            return bool(tags.get("readOnly"))
        return None

    def _permission_for_tool(self, tool: Any, name: str = "") -> str:
        from ai_editor.scopes import tool_permission
        tool_name = name or getattr(tool, "name", "")
        return tool_permission(
            self._mode,
            tool_name,
            self._perm_overrides,
            read_only=self._tool_read_only(tool) if tool is not None else None,
            category=getattr(tool, "category", "") if tool is not None else "",
        )

    @staticmethod
    def _engine_action_from_arguments(arguments: Any) -> str:
        if isinstance(arguments, str):
            try:
                arguments = json.loads(arguments) if arguments.strip() else {}
            except json.JSONDecodeError:
                return ""
        if isinstance(arguments, dict):
            return str(arguments.get("action", ""))
        return ""

    def _engine_action_blocked(self, action: str) -> bool:
        return self._mode == "ask" and action in _DANGEROUS_ENGINE_ACTIONS

    def _engine_action_gate(self, arguments: Any, confirmed: bool) -> Optional[Dict[str, Any]]:
        action = self._engine_action_from_arguments(arguments)
        if action not in _DANGEROUS_ENGINE_ACTIONS:
            return None
        if self._engine_action_blocked(action):
            return {"error": f"Engine action disabled by mode policy: {action}"}
        if self._mode != "agent" and not confirmed:
            return {"error": "Tool execution requires confirmation",
                    "requires_confirmation": True}
        return None

    def _apply_mode_permissions(self) -> None:
        """Update tool registry confirm flags + controller tool filter based on mode."""
        if not self._registry:
            return
        disabled: List[str] = []
        is_agent = self._mode == "agent"
        agent_allowed = self._active_agent_tool_allowlist()
        for tool in self._registry.list_tools(include_disabled=True):
            p = self._permission_for_tool(tool)
            if agent_allowed is not None and tool.name not in agent_allowed:
                disabled.append(tool.name)
                tool.enabled = False
            elif p == "disabled":
                disabled.append(tool.name)
                tool.enabled = False
            elif p == "confirm" and not is_agent:
                tool.enabled = True
                tool.requires_confirm = True
            else:
                tool.enabled = True
                tool.requires_confirm = False
        self._registry._openai_cache = None
        controllers = [self._controller] + list(getattr(self, "_provider_controllers", {}).values())
        for ctrl in controllers:
            if not ctrl:
                continue
            ctrl._disabled_tools = set(disabled)
            ctrl.extra_tools = self._controller_extra_tools()
            ctrl.mcp_dispatch = self._controller_mcp_dispatch()
            ctrl.mcp_tool_requires_confirm = self._mcp_tool_requires_confirm
            ctrl.mcp_tool_allowed = self._mcp_tool_allowed

    def _active_agent_tool_allowlist(self) -> Optional[set[str]]:
        active_agent_id = getattr(self, "_active_agent_id", None)
        agent_registry = getattr(self, "_agent_registry", None)
        if not active_agent_id or not agent_registry:
            return None
        agent = agent_registry.get(active_agent_id)
        tools = getattr(agent, "tools", None) if agent else None
        if tools is None:
            return None
        return {str(name).strip() for name in tools if str(name).strip()}

    # ── Chat Provider API ──

    def list_chat_providers(self) -> Dict:
        self._ensure_engine()
        self._sync_dynamic_webview_providers()
        providers = []
        base_cfg = self._engine.config if self._engine else None
        for prov in self._provider_registry.list_all():
            item = prov.to_dict()
            item.update(describe_provider_status(prov, self._settings_getter, base_cfg))
            item["builtin"] = prov.builtin
            providers.append(item)
        return {"providers": providers}

    def switch_provider(self, provider_id: str) -> Dict:
        self._ensure_engine()
        prov = self._provider_registry.get(provider_id)
        if not prov:
            return {"error": f"Unknown provider: {provider_id}"}
        self._active_provider = provider_id
        persist_error = None
        try:
            self._save_config_patch({"active_chat_provider": provider_id})
        except RuntimeError as exc:
            persist_error = str(exc)
        if provider_id == "chat":
            result = {"ok": True, "provider": "chat"}
            if persist_error:
                result.update({"persist_error": persist_error, "applied": True})
            return result
        unavailable = self._provider_unavailable_reason(prov)
        if unavailable:
            result = {
                "ok": True,
                "provider": provider_id,
                "available": False,
                "status": "unavailable",
                "unavailable_reason": unavailable,
            }
            if persist_error:
                result.update({"persist_error": persist_error, "applied": True})
            return result
        if self._provider_uses_native_chat(prov) or self._provider_uses_extension_webview(prov):
            result = {"ok": True, "provider": provider_id}
            if persist_error:
                result.update({"persist_error": persist_error, "applied": True})
            return result
        ctrl = self._provider_controllers.get(provider_id)
        if not ctrl:
            ctrl = self._create_provider_controller(prov)
            self._provider_controllers[provider_id] = ctrl
        result = {"ok": True, "provider": provider_id}
        if persist_error:
            result.update({"persist_error": persist_error, "applied": True})
        return result

    def provider_send(self, provider_id: str, text: str) -> Dict:
        """Send a message to a specific provider's conversation."""
        self._ensure_engine()
        if provider_id == "chat":
            return self.send_message(text)
        message = str(text or "").strip()
        if not message:
            return {"error": "Empty message", "provider": provider_id}
        prov = self._provider_registry.get(provider_id)
        if not prov:
            return {"error": f"Unknown provider: {provider_id}"}
        unavailable = self._provider_unavailable_reason(prov)
        if unavailable:
            return {"error": unavailable, "provider": provider_id, "available": False}
        if self._provider_uses_native_chat(prov):
            return self.send_message(message)
        if self._provider_uses_extension_webview(prov):
            return {"error": "Provider input is owned by its extension WebviewView.",
                    "provider": provider_id, "runtime_mode": "extension-webview"}
        ctrl = self._provider_controllers.get(provider_id)
        if not ctrl:
            ctrl = self._create_provider_controller(prov)
            self._provider_controllers[provider_id] = ctrl
        if ctrl.is_running:
            return {"error": "Already running"}
        self._sync_extension_tools()
        ctrl.send(message, agent_mode=prov.auto_agent)
        return {"ok": True}

    def provider_cancel(self, provider_id: str) -> Dict:
        if provider_id == "chat":
            return self.cancel()
        prov = self._provider_registry.get(provider_id)
        if not prov:
            return {"error": f"Unknown provider: {provider_id}"}
        ctrl = self._provider_controllers.get(provider_id)
        if ctrl:
            ctrl.cancel()
            return {"ok": True, "provider": provider_id, "cancelled": True}
        return {"ok": True, "provider": provider_id, "cancelled": False}

    def provider_new_chat(self, provider_id: str) -> Dict:
        self._ensure_engine()
        if provider_id == "chat":
            return self.new_chat()
        prov = self._provider_registry.get(provider_id)
        if not prov:
            return {"error": f"Unknown provider: {provider_id}"}
        unavailable = self._provider_unavailable_reason(prov)
        if unavailable:
            ctrl = self._provider_controllers.pop(provider_id, None)
            if ctrl:
                ctrl.cancel()
            return {
                "ok": True,
                "provider": provider_id,
                "available": False,
                "status": "unavailable",
                "unavailable_reason": unavailable,
            }
        if self._provider_uses_native_chat(prov):
            result = self.new_chat()
            result["provider"] = provider_id
            return result
        if self._provider_uses_extension_webview(prov):
            return {"ok": True, "provider": provider_id,
                    "runtime_mode": "extension-webview",
                    "created_controller": False}
        ctrl = self._provider_controllers.get(provider_id)
        created_controller = False
        if not ctrl:
            ctrl = self._create_provider_controller(prov)
            self._provider_controllers[provider_id] = ctrl
            created_controller = True
        ctrl.new_conversation(prov.system_prompt)
        return {"ok": True, "provider": provider_id,
                "created_controller": created_controller}

    def register_chat_provider(self, data: Dict) -> Dict:
        """Plugin API: register a custom chat provider tab."""
        self._ensure_engine()
        from ai_editor.chat_providers import ChatProviderDef
        try:
            prov = ChatProviderDef.from_dict(data)
        except TypeError as exc:
            return {"error": str(exc)}
        previous = self._provider_registry.get(prov.id)
        try:
            self._provider_registry.register(prov)
        except ValueError as exc:
            return {"error": str(exc), "id": prov.id}
        if previous and not previous.builtin:
            ctrl = self._provider_controllers.pop(prov.id, None)
            if ctrl:
                ctrl.cancel()
        return {"ok": True, "id": prov.id}

    def unregister_chat_provider(self, provider_id: str) -> Dict:
        self._ensure_engine()
        prov = self._provider_registry.get(provider_id)
        if not prov:
            return {"error": f"Unknown provider: {provider_id}"}
        if prov.builtin:
            return {"error": f"Cannot unregister built-in provider: {provider_id}"}
        ctrl = self._provider_controllers.pop(provider_id, None)
        if ctrl:
            ctrl.cancel()
        self._provider_registry.unregister(provider_id)
        return {"ok": True}

    def _create_provider_controller(self, prov):
        """Create a controller for a non-default provider.

        CLI controllers are only fallback command backends. Extension webview
        providers own their input boxes through VS Code WebviewView registration.
        """
        pid = prov.id

        if pid in self._CLI_PROVIDER_IDS:
            cli_ctrl = self._try_create_cli_controller(prov)
            if cli_ctrl is not None:
                return cli_ctrl
            raise RuntimeError(f"CLI runtime is unavailable for provider: {pid}")

        cfg = self._provider_config_for(prov)
        engine = LLMEngine(cfg)
        conv = Conversation(system_prompt=prov.system_prompt)
        ctrl = ChatController(engine, self._registry, conv)
        self._wire_provider_callbacks(ctrl, pid)
        ctrl.resolve_variable = self._resolve_variable
        self._configure_controller_tooling(ctrl)
        return ctrl

    def _try_create_cli_controller(self, prov):
        """Return a CliChatController that pipes messages through the real CLI."""
        try:
            from ai_editor.cli_controller import (
                CliChatController, get_cli_args,
            )
        except ImportError:
            return None
        cli_path = provider_runtime_cli_path(prov.id, self._settings_getter)
        if not cli_path:
            return None
        cli_args = get_cli_args(prov.id, self._settings_getter)
        cwd = self._workspace_root() or None
        ctrl = CliChatController(cli_path, prov.id, cli_args, cwd)
        self._wire_provider_callbacks(ctrl, prov.id)
        return ctrl

    # ── CLI provider controllers ──

    _CLI_PROVIDER_IDS = ("claude-code", "codex")
    _COPILOT_EXTENSION_IDS = {"github.copilot", "github.copilot-chat"}
    _CLI_WEBVIEW_EXTENSION_IDS = {"sao.cli.claude-code", "sao.cli.codex"}
    _DYNAMIC_WEBVIEW_METADATA_KEY = "dynamic_webview_provider"

    @staticmethod
    def _provider_uses_native_chat(prov: Any) -> bool:
        return str(getattr(prov, "id", "") or "") == "copilot"

    def _provider_uses_extension_webview(self, prov: Any) -> bool:
        if self._provider_uses_native_chat(prov):
            return False
        return bool(self._provider_candidate_view_ids(prov))

    def _provider_runtime_webview_id(self, provider_id: str) -> str:
        info = self._provider_runtime_webview_info(provider_id)
        return str(
            info.get("runtime_view_id")
            or info.get("manifest_view_id")
            or info.get("declared_view_id")
            or ""
        )

    def _provider_runtime_webview_info(self, provider_id: str) -> Dict[str, Any]:
        if not hasattr(self, "_provider_registry"):
            self._ensure_engine()
        registry = getattr(self, "_provider_registry", None)
        provider = registry.get(provider_id) if registry else None
        candidate_ids = self._provider_candidate_view_ids(provider)
        manifest_views = self._provider_manifest_views(provider)
        manifest_ids = [str(view.get("id") or "") for view in manifest_views if str(view.get("id") or "")]
        runtime_ids = self._provider_runtime_registered_view_ids()

        runtime_view_id = self._first_matching_value(candidate_ids, runtime_ids)
        if not runtime_view_id:
            runtime_view_id = self._first_matching_value(manifest_ids, runtime_ids)

        manifest_view = next((view for view in manifest_views if str(view.get("id") or "") == runtime_view_id), None)
        if manifest_view is None and manifest_views:
            manifest_view = manifest_views[0]

        declared_view_id = candidate_ids[0] if candidate_ids else ""
        manifest_view_id = str(manifest_view.get("id") or "") if manifest_view else ""
        return {
            "declared_view_id": declared_view_id,
            "runtime_view_id": runtime_view_id,
            "manifest_view_id": manifest_view_id,
            "manifest_view": manifest_view or {},
        }

    def _provider_candidate_view_ids(self, provider: Any) -> List[str]:
        if provider is None:
            return []
        if hasattr(provider, "candidate_webview_ids"):
            ids = provider.candidate_webview_ids()
        else:
            ids = []
        return [str(view_id).strip() for view_id in ids if str(view_id).strip()]

    def _provider_candidate_extension_ids(self, provider: Any) -> List[str]:
        if provider is None:
            return []
        if hasattr(provider, "candidate_extension_ids"):
            ids = provider.candidate_extension_ids()
        else:
            ids = []
        return [str(ext_id).strip() for ext_id in ids if str(ext_id).strip()]

    def _provider_runtime_registered_view_ids(self) -> List[str]:
        vscode_ns = getattr(self, "_vscode_ns", None)
        if not vscode_ns or not hasattr(vscode_ns, "list_webview_view_ids"):
            return []
        try:
            view_ids = vscode_ns.list_webview_view_ids()
        except Exception:
            return []
        return [str(view_id).strip() for view_id in view_ids if str(view_id).strip()]

    def _provider_manifest_views(self, provider: Any) -> List[Dict[str, Any]]:
        candidate_ids = self._provider_candidate_view_ids(provider)
        candidate_extensions = self._provider_candidate_extension_ids(provider)
        matched: List[tuple[tuple[int, int, int, str], Dict[str, Any]]] = []
        for raw_view in self._all_manifest_views(webview_only=False):
            view = dict(raw_view)
            view_id = str(view.get("id") or "").strip()
            extension_id = str(view.get("_extensionId") or view.get("extensionId") or "").strip()
            id_index = candidate_ids.index(view_id) if view_id in candidate_ids else -1
            ext_index = candidate_extensions.index(extension_id) if extension_id in candidate_extensions else -1
            if id_index < 0 and ext_index < 0:
                continue
            if id_index >= 0:
                score = (0, id_index, ext_index if ext_index >= 0 else len(candidate_extensions), view_id.casefold())
            else:
                score = (1, len(candidate_ids), ext_index, view_id.casefold())
            matched.append((score, view))

        matched.sort(key=lambda item: item[0])
        return [view for _, view in matched]

    @staticmethod
    def _first_matching_value(preferred: List[str], candidates: List[str]) -> str:
        candidate_set = {str(value).strip() for value in candidates if str(value).strip()}
        for value in preferred:
            normalized = str(value).strip()
            if normalized and normalized in candidate_set:
                return normalized
        return ""

    @staticmethod
    def _provider_fallback_view_id(provider_id: str) -> str:
        return f"provider.{provider_id}"

    def _provider_webview_state(self, view_id: str, fallback_id: str) -> Any:
        if view_id:
            state = self._webview_states.get(view_id)
            if state is not None:
                return state
        return self._webview_states.get(fallback_id)

    def _on_webview_view_changed(self, event: Dict[str, Any]) -> None:
        self._sync_dynamic_webview_providers()
        self._refresh_provider_tabs({"source": "runtime_webview", **_as_dict(event)})

    def _refresh_provider_tabs(self, change: Optional[Dict[str, Any]] = None) -> None:
        payload = {"change": _as_dict(change)}
        try:
            payload["providers"] = self.list_chat_providers().get("providers", [])
        except Exception:
            pass
        self._emit("chat_providers_changed", payload)
        self._emit("provider_tabs_changed", payload)
        self._eval_js(
            "(function(){if(typeof renderProviderTabs==='function')"
            "{renderProviderTabs();}})()"
        )

    def _activate_provider_view(self, view_id: str) -> bool:
        ext_host = getattr(self, "_ext_host", None)
        normalized = str(view_id or "").strip()
        if not ext_host or not normalized:
            return False
        try:
            activated = ext_host.activate_event(f"onView:{normalized}")
        except Exception:
            return False
        if activated:
            self._register_ext_tools()
        return bool(activated)

    def _sync_dynamic_webview_providers(self) -> bool:
        registry = getattr(self, "_provider_registry", None)
        if registry is None:
            return False
        from ai_editor.chat_providers import ChatProviderDef

        desired: Dict[str, ChatProviderDef] = {}
        for spec in self._dynamic_webview_provider_specs():
            provider_id = str(spec.get("provider_id") or "").strip()
            view_id = str(spec.get("view_id") or "").strip()
            if not provider_id or not view_id:
                continue
            extension_id = str(spec.get("extension_id") or "").strip()
            metadata = {
                self._DYNAMIC_WEBVIEW_METADATA_KEY: True,
                "view_id": view_id,
                "webview_ids": [view_id],
                "candidate_view_ids": [view_id],
                "extension_id": extension_id,
                "extension_ids": [extension_id] if extension_id else [],
                "view_location": spec.get("location", ""),
                "runtime_mode": "extension-webview",
                "source": spec.get("source", "manifest"),
            }
            manifest_view = spec.get("manifest_view")
            if isinstance(manifest_view, dict):
                metadata["manifest_view"] = dict(manifest_view)
            provider = ChatProviderDef(
                id=provider_id,
                name=str(spec.get("name") or view_id),
                icon=str(spec.get("icon") or "▣"),
                provider_type="extension-webview",
                model="WebviewView",
                auto_agent=False,
                builtin=False,
                webview_id=view_id,
                webview_ids=[view_id],
                extension_ids=[extension_id] if extension_id else [],
                metadata=metadata,
            )
            desired[provider_id] = provider

        changed = False
        for provider_id, provider in desired.items():
            existing = registry.get(provider_id)
            if existing and not self._is_dynamic_webview_provider(existing):
                continue
            if existing is None or not self._same_dynamic_webview_provider(existing, provider):
                changed = True
            registry.register(provider)

        for provider in list(registry.list_all()):
            if not self._is_dynamic_webview_provider(provider):
                continue
            if provider.id not in desired and registry.unregister(provider.id):
                changed = True
                if getattr(self, "_active_provider", "chat") == provider.id:
                    self._active_provider = "chat"
        return changed

    def _same_dynamic_webview_provider(self, left: Any, right: Any) -> bool:
        return (
            getattr(left, "name", "") == getattr(right, "name", "")
            and left.candidate_webview_ids() == right.candidate_webview_ids()
            and left.candidate_extension_ids() == right.candidate_extension_ids()
            and _as_dict(getattr(left, "metadata", {})) == _as_dict(getattr(right, "metadata", {}))
        )

    def _is_dynamic_webview_provider(self, provider: Any) -> bool:
        metadata = _as_dict(getattr(provider, "metadata", {}))
        return bool(metadata.get(self._DYNAMIC_WEBVIEW_METADATA_KEY))

    def _dynamic_webview_provider_specs(self) -> List[Dict[str, Any]]:
        specs: Dict[str, Dict[str, Any]] = {}
        for spec in self._manifest_webview_provider_specs():
            specs.setdefault(str(spec.get("view_id") or ""), spec)
        for spec in self._runtime_webview_provider_specs():
            view_id = str(spec.get("view_id") or "")
            current = specs.get(view_id, {})
            merged = dict(current)
            merged.update(spec)
            if current.get("manifest_view") and "manifest_view" not in merged:
                merged["manifest_view"] = current["manifest_view"]
            specs[view_id] = merged
        return list(specs.values())

    def _manifest_webview_provider_specs(self) -> List[Dict[str, Any]]:
        specs: List[Dict[str, Any]] = []
        if "views" not in set(self._enabled_extension_contributions()):
            return specs
        for view in self._all_manifest_views(webview_only=True):
            view_id = str(view.get("id") or "").strip()
            extension_id = str(view.get("_extensionId") or "").strip()
            if not view_id or self._is_copilot_extension_id(extension_id):
                continue
            if self._non_dynamic_provider_for_view_id(view_id):
                continue
            specs.append({
                "provider_id": self._provider_id_for_webview_view(view_id),
                "view_id": view_id,
                "extension_id": extension_id,
                "name": view.get("name") or view.get("title") or view_id,
                "icon": self._view_icon(view),
                "location": view.get("_viewLocation", ""),
                "source": "manifest",
                "manifest_view": dict(view),
            })
        return specs

    def _runtime_webview_provider_specs(self) -> List[Dict[str, Any]]:
        vscode_ns = getattr(self, "_vscode_ns", None)
        if not vscode_ns or not hasattr(vscode_ns, "list_webview_views"):
            return []
        try:
            rows = vscode_ns.list_webview_views()
        except Exception:
            return []
        specs: List[Dict[str, Any]] = []
        for row in rows:
            if not isinstance(row, dict):
                continue
            view_id = str(row.get("view_id") or "").strip()
            extension_id = str(row.get("extension_id") or "").strip()
            if not view_id:
                continue
            if self._is_copilot_extension_id(extension_id):
                continue
            if extension_id in self._CLI_WEBVIEW_EXTENSION_IDS:
                continue
            if self._non_dynamic_provider_for_view_id(view_id):
                continue
            specs.append({
                "provider_id": self._provider_id_for_webview_view(view_id),
                "view_id": view_id,
                "extension_id": extension_id,
                "name": row.get("title") or view_id,
                "icon": "▣",
                "location": "runtime",
                "source": "runtime",
            })
        return specs

    def _all_manifest_views(self, webview_only: bool = False) -> List[Dict[str, Any]]:
        ext_host = getattr(self, "_ext_host", None)
        registry = getattr(ext_host, "registry", None) if ext_host else None
        if registry is None:
            return []
        result: List[Dict[str, Any]] = []
        for ext in registry.list_all():
            if not getattr(ext, "enabled", True) or not self._extension_allowed(ext):
                continue
            contributes = getattr(ext, "contributes", {}) or {}
            views_by_location = contributes.get("views", {})
            if not isinstance(views_by_location, dict):
                continue
            for location, views in views_by_location.items():
                if not isinstance(views, list):
                    continue
                for raw_view in views:
                    if not isinstance(raw_view, dict):
                        continue
                    view = dict(raw_view)
                    if webview_only and not self._manifest_view_is_webview(view):
                        continue
                    view["_extensionId"] = getattr(ext, "id", "")
                    view["_viewLocation"] = str(location)
                    result.append(view)
        return result

    @staticmethod
    def _manifest_view_is_webview(view: Dict[str, Any]) -> bool:
        return str(view.get("type") or "").strip().lower() == "webview"

    @staticmethod
    def _view_icon(view: Dict[str, Any]) -> str:
        icon = view.get("icon")
        if isinstance(icon, str) and icon.strip():
            return icon.strip()
        return "▣"

    def _provider_id_for_webview_view(self, view_id: str) -> str:
        registry = getattr(self, "_provider_registry", None)
        normalized = str(view_id or "").strip()
        if not registry or not normalized:
            return normalized
        existing = registry.get(normalized)
        if existing is None or self._is_dynamic_webview_provider(existing):
            return normalized
        return f"view:{normalized}"

    def _non_dynamic_provider_for_view_id(self, view_id: str) -> str:
        registry = getattr(self, "_provider_registry", None)
        normalized = str(view_id or "").strip()
        if not registry or not normalized:
            return ""
        for provider in registry.list_all():
            if self._is_dynamic_webview_provider(provider):
                continue
            if normalized in self._provider_candidate_view_ids(provider):
                return str(getattr(provider, "id", "") or "")
        return ""

    def _is_copilot_extension_id(self, extension_id: str) -> bool:
        normalized = str(extension_id or "").strip().lower()
        return (
            normalized in self._COPILOT_EXTENSION_IDS
            or normalized.startswith("github.copilot")
        )

    # ── CLI launcher (real terminal window) ──

    _cli_launchers: Dict[str, Any] = {}

    def launch_provider_cli(self, provider_id: str) -> Dict:
        """Launch a CLI provider in its own interactive terminal window."""
        from ai_editor.cli_controller import CliLauncher, get_cli_args
        cli_path = provider_runtime_cli_path(provider_id, self._settings_getter)
        if not cli_path:
            return {"error": f"CLI not found for {provider_id}"}
        launcher = self._cli_launchers.get(provider_id)
        if launcher and launcher.is_running:
            return {"ok": True, "already_running": True, "pid": launcher.pid}
        cli_args = get_cli_args(provider_id, self._settings_getter)
        cwd = self._workspace_root() or None
        launcher = CliLauncher(cli_path, provider_id, cli_args, cwd)
        self._cli_launchers[provider_id] = launcher
        return launcher.launch()

    def stop_provider_cli(self, provider_id: str) -> Dict:
        """Stop a running CLI provider."""
        launcher = self._cli_launchers.get(provider_id)
        if not launcher:
            return {"ok": True, "was_running": False}
        return launcher.stop()

    def provider_cli_status(self, provider_id: str) -> Dict:
        """Get status of a CLI provider."""
        launcher = self._cli_launchers.get(provider_id)
        if not launcher:
            cli_path = provider_runtime_cli_path(provider_id, self._settings_getter)
            return {"running": False, "cli_available": bool(cli_path),
                    "provider": provider_id}
        return launcher.status()

    def _wire_provider_callbacks(self, ctrl, pid: str) -> None:
        """Attach event callbacks that emit to the webview."""
        ctrl.on_stream_delta = lambda msg, t: self._emit(
            "provider_stream_delta", {"provider": pid, "content": t})
        ctrl.on_thinking_delta = lambda msg, t: self._emit(
            "provider_thinking_delta", {"provider": pid, "content": t})
        ctrl.on_stream_end = lambda msg: self._emit(
            "provider_stream_end", {"provider": pid,
             "content": msg.content, "model": msg.model,
             "thinking": getattr(msg, "thinking", ""),
             **({"error": msg.content} if msg.is_error else {}),
             **({"usage": msg.usage} if msg.usage else {})})
        ctrl.on_tool_start = lambda cid, n, a, state="": self._emit(
            "provider_tool_start", {"provider": pid, "id": cid, "name": n, "arguments": a, "state": state})
        ctrl.on_tool_end = lambda cid, n, r, state="": self._emit(
            "provider_tool_end", {"provider": pid, "id": cid, "name": n, "result": r, "state": state})
        ctrl.on_tool_confirm = lambda cid, n, a, _pid=pid: self._on_provider_tool_confirm(_pid, cid, n, a)
        ctrl.on_tool_progress = lambda cid, n, p: self._emit(
            "provider_tool_progress", {"provider": pid, "id": cid, "name": n, "progress": p})
        ctrl.on_token_warning = lambda u, l, r: self._emit(
            "provider_token_warning", {"provider": pid, "used": u, "limit": l, "percent": int(r * 100)})
        ctrl.on_error = lambda e: self._emit(
            "provider_error", {"provider": pid, "error": e})
        ctrl.on_idle = lambda: self._emit(
            "provider_idle", {"provider": pid})

    def _controller_extra_tools(self) -> List[Dict[str, Any]]:
        if not self._mcp:
            return []
        tools: List[Dict[str, Any]] = []
        for tool in self._mcp.all_tools():
            tool_name = self._mcp_tool_name(tool)
            if not self._mcp_tool_allowed(tool_name):
                continue
            tools.append({
                "type": "function",
                "function": {
                    "name": tool_name,
                    "description": f"[MCP:{tool.server_id}] {tool.description}",
                    "parameters": normalize_tool_parameters(tool.input_schema),
                },
            })
        return tools

    def _controller_mcp_dispatch(self) -> Optional[Callable[[str, Any], str]]:
        if not self._mcp:
            return None
        def _dispatch(name: str, args: Any) -> str:
            ok, parsed = self._parse_mcp_arguments(name, args)
            if not ok:
                return json.dumps({"error": parsed}, ensure_ascii=False)
            if not self._mcp_tool_allowed(name):
                return json.dumps({"error": f"MCP tool disabled by policy: {name}"}, ensure_ascii=False)
            return self._mcp.call_tool(name, parsed)
        return _dispatch

    @staticmethod
    def _parse_mcp_arguments(name: str, args: Any) -> tuple[bool, Any]:
        if isinstance(args, str):
            raw = args.strip()
            if not raw:
                parsed: Any = {}
            else:
                try:
                    parsed = json.loads(raw)
                except json.JSONDecodeError as exc:
                    return False, (
                        f"Invalid JSON arguments for {name}: "
                        f"{exc.msg} at char {exc.pos}"
                    )
        else:
            parsed = args
        if parsed is None:
            parsed = {}
        if not isinstance(parsed, dict):
            return False, f"MCP tool arguments for {name} must be a JSON object"
        return True, parsed

    def _configure_controller_tooling(self, ctrl: Optional[ChatController]) -> None:
        if not ctrl:
            return
        ctrl.extra_tools = self._controller_extra_tools()
        ctrl.mcp_dispatch = self._controller_mcp_dispatch()
        ctrl.mcp_tool_requires_confirm = self._mcp_tool_requires_confirm
        ctrl.mcp_tool_allowed = self._mcp_tool_allowed

    def _provider_config_for(self, prov) -> ProviderConfig:
        """Build the exact runtime config for a right-sidebar provider tab."""
        base_cfg = self._engine.config if self._engine else ProviderConfig()
        runtime_cfg = build_provider_runtime_config(prov, self._settings_getter, base_cfg)
        if runtime_cfg and runtime_cfg.provider == "anthropic" and runtime_cfg.api_key and (
                not runtime_cfg.model or runtime_cfg.model in _STALE_ANTHROPIC_DEFAULT_MODELS):
            official_model = self._official_default_model_for_provider(
                runtime_cfg.provider,
                runtime_cfg.effective_base_url,
                runtime_cfg.api_key,
            )
            if official_model:
                runtime_cfg.model = official_model
        if runtime_cfg:
            return runtime_cfg
        fallback = ProviderConfig(**vars(base_cfg))
        fallback.system_prompt = prov.system_prompt
        return fallback

    def _provider_unavailable_reason(self, prov) -> str:
        status = describe_provider_status(
            prov,
            self._settings_getter,
            self._engine.config if self._engine else None,
        )
        return str(status.get("unavailable_reason") or "")

    def _resolve_provider_key(self, provider_type: str) -> str:
        ai = _normalize_ai_editor_config(self._settings_getter("ai_editor", {}) or {})
        if not isinstance(ai, dict):
            return ""
        if ai.get("provider") == provider_type and ai.get("api_key"):
            return ai.get("api_key", "")
        keys = ai.get("provider_keys", {})
        if isinstance(keys, dict):
            key = keys.get(provider_type, "")
            if key:
                return key
        legacy_keys = ai.get("_provider_keys", {})
        if isinstance(legacy_keys, dict):
            return legacy_keys.get(provider_type, "")
        return ""

    # ── Agent API (JS-callable) ──

    def list_agents(self) -> Dict:
        self._ensure_engine()
        agents = []
        for a in self._agent_registry.list_all():
            item = a.to_dict()
            if not getattr(a, "builtin", False):
                item["_scope"] = getattr(a, "_scope", "workspace")
                item["_plugin_id"] = getattr(a, "_plugin_id", "")
            agents.append(item)
        return {"agents": agents}

    def get_agent(self, agent_id: str) -> Dict:
        self._ensure_engine()
        a = self._agent_registry.get(agent_id)
        return a.to_dict() if a else {"error": "Not found"}

    def save_agent(self, data: Dict) -> Dict:
        self._ensure_engine()
        from ai_editor.agents import AgentDef
        scope = data.pop("_scope", "workspace")
        agent = AgentDef.from_dict(data)
        return self._agent_registry.save_custom(agent, scope=scope)

    def delete_agent(self, agent_id: str) -> Dict:
        self._ensure_engine()
        return self._agent_registry.delete_custom(agent_id)

    def set_active_agent(self, agent_id: str) -> Dict:
        self._ensure_engine()
        return self._set_active_agent(agent_id)

    def clear_active_agent(self) -> Dict:
        self._ensure_engine()
        self._active_agent_id = None
        if self._controller and self._controller.conversation:
            self._controller.conversation.system_prompt = self._default_system_prompt()
        self._apply_mode_permissions()
        return {"ok": True}

    def get_active_agent(self) -> Dict:
        return {"agent_id": self._active_agent_id or ""}

    def _set_active_agent(self, agent_id: str) -> Dict:
        agent = self._agent_registry.get(agent_id)
        if not agent:
            return {"error": f"Agent not found: {agent_id}"}
        self._active_agent_id = agent_id
        if self._controller and self._controller.conversation:
            base = self._default_system_prompt()
            self._controller.conversation.system_prompt = (
                base + f"\n\n# Active Agent: {agent.name}\n\n"
                + agent.system_prompt
            )
        self._apply_mode_permissions()
        return {"ok": True, "agent": agent.to_dict()}

    # ── Workflow API (JS-callable) ──

    def list_workflows(self) -> Dict:
        self._ensure_engine()
        workflows = []
        for w in self._wf_registry.list_all():
            item = w.to_dict()
            if not getattr(w, "builtin", False):
                item["_scope"] = getattr(w, "_scope", "workspace")
                item["_plugin_id"] = getattr(w, "_plugin_id", "")
            workflows.append(item)
        return {"workflows": workflows}

    def get_workflow(self, wf_id: str) -> Dict:
        self._ensure_engine()
        w = self._wf_registry.get(wf_id)
        return w.to_dict() if w else {"error": "Not found"}

    def save_workflow(self, data: Dict) -> Dict:
        self._ensure_engine()
        from ai_editor.workflows import WorkflowDef
        scope = data.pop("_scope", "workspace")
        wf = WorkflowDef.from_dict(data)
        return self._wf_registry.save_custom(wf, scope=scope)

    def delete_workflow(self, wf_id: str) -> Dict:
        self._ensure_engine()
        return self._wf_registry.delete_custom(wf_id)

    def run_workflow(self, wf_id: str, input_text: str) -> Dict:
        """Run a workflow from the UI. Executes in the calling thread."""
        self._ensure_engine()
        wf = self._wf_registry.get(wf_id)
        if not wf:
            return {"error": f"Workflow not found: {wf_id}"}

        def _on_start(i, total, step):
            self._emit("workflow_step", {
                "step": i, "total": total,
                "label": step.label, "status": "running"})

        def _on_end(i, total, step, output, error):
            self._emit("workflow_step", {
                "step": i, "total": total,
                "label": step.label, "status": "done",
                "preview": (output or "")[:300], "error": error})

        return self._wf_engine.run(wf, input_text, _on_start, _on_end)

    # ── Engine action handlers (registered on gui._ai_engine_actions) ──

    def _eng_list_agents(self) -> Dict:
        return {"agents": [
            {"id": a.id, "name": a.name, "description": a.description,
             "icon": a.icon, "when_to_use": a.when_to_use}
            for a in self._agent_registry.list_all()
        ]}

    def _eng_invoke_agent(self, kw: Dict) -> Dict:
        agent_id = kw.get("agent_id", "")
        message = kw.get("message", "")
        agent = self._agent_registry.get(agent_id)
        if not agent:
            return {"error": f"Agent not found: {agent_id}",
                    "available": [a.id for a in self._agent_registry.list_all()]}
        self._set_active_agent(agent_id)
        if str(message or "").strip():
            cfg = self._agent_provider_config(agent)
            engine = LLMEngine(cfg)
            try:
                resp = engine.chat_completion([
                    {"role": "system", "content": agent.system_prompt},
                    {"role": "user", "content": str(message)},
                ], tools=None)
            finally:
                engine.close()
            if resp.error:
                return {"error": resp.error, "agent": agent.id, "name": agent.name}
            return {
                "agent": agent.id,
                "name": agent.name,
                "content": resp.content,
                "thinking": resp.thinking,
                "usage": resp.usage,
                "model": resp.model or cfg.effective_model,
            }
        return {
            "agent": agent.id,
            "name": agent.name,
            "activated": True,
            "message": message,
            "instruction": (
                f"Now acting as {agent.name}. "
                f"Apply this guidance:\n\n{agent.system_prompt}"
            ),
        }

    def _agent_provider_config(self, agent: Any) -> ProviderConfig:
        base_cfg = self._engine.config if self._engine else ProviderConfig()
        return ProviderConfig(
            provider=base_cfg.provider,
            api_key=base_cfg.api_key,
            base_url=base_cfg.base_url,
            model=str(getattr(agent, "model", "") or base_cfg.model),
            temperature=base_cfg.temperature,
            max_tokens=base_cfg.max_tokens,
            system_prompt=getattr(agent, "system_prompt", "") or base_cfg.system_prompt,
            transport=base_cfg.transport,
            top_p=base_cfg.top_p,
            frequency_penalty=base_cfg.frequency_penalty,
            presence_penalty=base_cfg.presence_penalty,
            stop=list(base_cfg.stop),
            max_input_tokens=base_cfg.max_input_tokens,
            max_output_tokens=base_cfg.max_output_tokens,
            timeout=base_cfg.timeout,
            extra_headers=dict(base_cfg.extra_headers),
            extra_body=dict(base_cfg.extra_body),
        )

    def _eng_list_workflows(self) -> Dict:
        return {"workflows": [
            {"id": w.id, "name": w.name, "description": w.description,
             "icon": w.icon, "steps": len(w.steps),
             "when_to_use": w.when_to_use}
            for w in self._wf_registry.list_all()
        ]}

    def _eng_run_workflow(self, kw: Dict) -> Dict:
        wf_id = kw.get("workflow_id", "")
        input_text = kw.get("input", "")
        wf = self._wf_registry.get(wf_id)
        if not wf:
            return {"error": f"Workflow not found: {wf_id}",
                    "available": [w.id for w in self._wf_registry.list_all()]}
        return self._wf_engine.run(wf, input_text)

    # ── Extension Host API ──

    def init_extensions(self) -> None:
        """Scan and activate extensions. Safe to call from a background thread.

        Waits for ``_vscode_ns_ready`` so that the VS Code namespace and its
        UI bridge are guaranteed to exist before any extension code runs.
        """
        gate = getattr(self, "_vscode_ns_ready", None)
        if gate is not None:
            gate.wait(timeout=30)
        if self._extensions_inited:
            return
        self._extensions_inited = True
        self._init_extension_host()
        self._register_cli_provider_views()

    def _init_extension_host(self) -> None:
        """Scan extension directories and start the host.

        After scanning, extensions with a ``main`` entry point in their
        manifest are forwarded to a ``NodeExtensionHost`` subprocess (if
        Node.js is available).  Extensions **without** ``main`` continue
        through the existing Python-only activation path.
        """
        ext_dirs = self._extension_scan_dirs()
        self._ext_host.set_policy(
            self._extension_allowed,
            set(self._enabled_extension_contributions()),
        )
        count = self._ext_host.scan(ext_dirs)
        activated = self._ext_host.start()
        if count:
            print(f"[ExtHost] {count} extensions scanned, "
                  f"{len(activated)} activated")
            self._register_ext_tools()

        # --- Node.js extension host for extensions with "main" ---
        self._try_start_node_extension_host()

    def _try_start_node_extension_host(self) -> None:
        """Start a NodeExtensionHost for extensions that declare ``main``.

        Filters registered extensions, and if any have a JS entry point and
        Node.js is available, spawns the Node subprocess and activates them.
        Extensions without ``main`` are unaffected.
        """
        from ai_editor.extension_host import NodeExtensionHost
        from ai_editor.node_runtime import get_node_path

        all_exts = self._ext_host.registry.list_all()
        node_exts = [ext for ext in all_exts if ext.main and ext.enabled]
        if not node_exts:
            return

        node_path = get_node_path()
        if not node_path:
            print("[NodeExtHost] Node.js not found (bundled / PATH / "
                  f"common locations); {len(node_exts)} JS extension(s) "
                  "will run manifest-only.")
            return

        # Locate the extension host bootstrap script
        here = os.path.dirname(os.path.abspath(__file__))
        script_path = os.path.join(here, "node_ext_host.js")
        if not os.path.isfile(script_path):
            # Also check a sibling "runtime" directory
            alt = os.path.join(os.path.dirname(here), "runtime",
                               "node_ext_host.js")
            if os.path.isfile(alt):
                script_path = alt
            else:
                print("[NodeExtHost] Bootstrap script not found: "
                      f"{script_path}; JS extensions will run manifest-only.")
                return

        # Build the UI bridge reference
        ui_bridge = getattr(self, "_vscode_ns", None)
        if ui_bridge is not None:
            ui_bridge = ui_bridge._ui_bridge if hasattr(ui_bridge, "_ui_bridge") else None
        if ui_bridge is None:
            ui_bridge = _AIEditorUIBridge(self) if hasattr(self, "_emit") else None

        host = NodeExtensionHost(
            node_path=node_path,
            script_path=script_path,
            ui_bridge=ui_bridge,
        )
        host.set_command_service(self._ext_host.commands)

        if not host.start():
            print("[NodeExtHost] Failed to start Node subprocess.")
            return

        activated = host.activate_all(node_exts)
        self._node_ext_host = host
        print(f"[NodeExtHost] {activated}/{len(node_exts)} JS extension(s) "
              "sent for activation.")

        # Push current settings so getConfiguration() returns real values
        self._sync_settings_to_node_host()

        # Handle config_set messages from Node (extension called config.update)
        host.on_config_set(self._handle_node_config_set)

    def _sync_settings_to_node_host(self) -> None:
        """Push the full settings dict to the Node extension host.

        Builds a section-keyed dict from the Python settings manager and
        sends it via ``settings_sync`` so that ``workspace.getConfiguration``
        in Node returns real values.
        """
        host = self._node_ext_host
        if host is None or not host.is_running:
            return
        settings = _resolve_settings(self._gui_ref)
        if not settings:
            return
        try:
            # Build a flat section dict from all known settings
            raw: Dict[str, Any] = {}
            # Expose the full ai_editor config as a section
            ai_cfg = settings.get("ai_editor", {})
            if isinstance(ai_cfg, dict):
                raw["ai_editor"] = dict(ai_cfg)
            # Expose individual AI editor sub-sections at the top level too
            # so extensions can do getConfiguration("mcp") etc.
            for section in _AI_EDITOR_SECTION_DEFAULTS:
                if isinstance(ai_cfg, dict) and section in ai_cfg:
                    raw[section] = ai_cfg[section]
            # Expose panel_themes for theme-aware extensions
            themes = settings.get("panel_themes", {})
            if isinstance(themes, dict):
                raw["panel_themes"] = dict(themes)
            host.send_settings_sync(raw)
        except Exception as exc:
            print(f"[NodeExtHost] Failed to sync settings: {exc}")

    def _handle_node_config_set(self, section: str, key: str,
                                value: Any) -> None:
        """Handle a config_set message from Node.

        Persists the change via the Python settings manager and notifies
        the Node host of the final value (in case Python normalises it).
        """
        settings = _resolve_settings(self._gui_ref)
        if not settings:
            return
        try:
            if section == "ai_editor":
                ai_cfg = settings.get("ai_editor", {}) or {}
                if not isinstance(ai_cfg, dict):
                    ai_cfg = {}
                ai_cfg[key] = value
                settings.set("ai_editor", ai_cfg)
            elif section in _AI_EDITOR_SECTION_DEFAULTS:
                ai_cfg = settings.get("ai_editor", {}) or {}
                if not isinstance(ai_cfg, dict):
                    ai_cfg = {}
                sub = ai_cfg.get(section, {})
                if not isinstance(sub, dict):
                    sub = {}
                sub[key] = value
                ai_cfg[section] = sub
                settings.set("ai_editor", ai_cfg)
            else:
                # Generic top-level section
                current = settings.get(section, {})
                if isinstance(current, dict):
                    current[key] = value
                    settings.set(section, current)
                else:
                    settings.set(section, {key: value})
            settings.save()
        except Exception as exc:
            print(f"[NodeExtHost] Failed to persist config_set "
                  f"{section}.{key}: {exc}")

    def _shutdown_node_extension_host(self) -> None:
        """Stop the Node extension host if running."""
        host = self._node_ext_host
        if host is not None:
            host.stop()
            self._node_ext_host = None

    def relay_node_webview_message(self, view_id: str, message: Any) -> Dict:
        """Forward a webview message to the Node extension host."""
        host = self._node_ext_host
        if host is None or not host.is_running:
            return {"error": "Node extension host not running"}
        ok = host.relay_webview_message(view_id, message)
        return {"ok": ok, "view_id": view_id}

    # Theme CSS variable block injected into CLI provider webview HTML so
    # that extensions relying on VS Code theme tokens render correctly in
    # our standalone pywebview shell.
    _CLI_WEBVIEW_THEME_CSS = """\
<style>
:root {
  --vscode-editor-background: var(--sao-bg, #1e1e1e);
  --vscode-editor-foreground: var(--sao-fg, #cccccc);
  --vscode-input-background: var(--sao-input-bg, #3c3c3c);
  --vscode-input-foreground: var(--sao-fg, #cccccc);
  --vscode-input-border: var(--sao-border, #3c3c3c);
  --vscode-button-background: var(--sao-accent, #0e639c);
  --vscode-button-foreground: #ffffff;
  --vscode-button-hoverBackground: var(--sao-accent-hover, #1177bb);
  --vscode-focusBorder: var(--sao-accent, #0e639c);
  --vscode-foreground: var(--sao-fg, #cccccc);
  --vscode-descriptionForeground: var(--sao-fg-dim, #9e9e9e);
  --vscode-errorForeground: #f48771;
  --vscode-font-family: var(--sao-font, 'Segoe UI', sans-serif);
  --vscode-font-size: var(--sao-font-size, 13px);
  --vscode-sideBar-background: var(--sao-sidebar-bg, #252526);
  --vscode-panel-background: var(--sao-bg, #1e1e1e);
  --vscode-panel-border: var(--sao-border, #3c3c3c);
}
body {
  background: var(--vscode-editor-background);
  color: var(--vscode-editor-foreground);
  font-family: var(--vscode-font-family);
  font-size: var(--vscode-font-size);
  margin: 0;
  padding: 0;
}
</style>
"""

    def _register_cli_provider_views(self) -> None:
        """Register WebviewView providers for CLI-backed chat providers.

        Only claude-code and codex are registered as WebviewView providers.
        Copilot is deliberately excluded: it uses the VS Code native
        ChatWidget / chat participant protocol and does not expose a
        WebviewView.  Registering it as a WebviewView would create a
        non-functional empty panel.  Copilot stays as a regular
        ChatProvider backed by our native chat panel.
        """
        vscode_ns = getattr(self, "_vscode_ns", None)
        if vscode_ns is None:
            return

        for provider_id in ("claude-code", "codex"):
            view_id = f"sao.cli.{provider_id}"
            html = (
                "<!DOCTYPE html>"
                "<html><head><meta charset='utf-8'>"
                f"{self._CLI_WEBVIEW_THEME_CSS}"
                "</head><body>"
                f"<div id='root' data-provider='{provider_id}'>"
                f"<p>Waiting for {provider_id} runtime&hellip;</p>"
                "</div>"
                "<script>"
                "const vscode = acquireVsCodeApi();"
                "window.addEventListener('message', e => {"
                "  const msg = e.data;"
                "  if (msg && msg.type === 'setHtml') {"
                "    document.getElementById('root').innerHTML = msg.html;"
                "  }"
                "});"
                "</script>"
                "</body></html>"
            )

            class _CliViewProvider:
                """Minimal WebviewViewProvider for a CLI chat provider."""
                def __init__(self, pid: str, initial_html: str) -> None:
                    self._pid = pid
                    self._html = initial_html

                def resolveWebviewView(self, view: Any,
                                       context: Any = None,
                                       token: Any = None) -> None:
                    if hasattr(view, "webview"):
                        view.webview.html = self._html

            provider = _CliViewProvider(provider_id, html)
            vscode_ns._register_webview_view_provider(
                view_id, provider, _extension_id=f"sao.cli.{provider_id}")

    def _extension_scan_dirs(self) -> List[str]:
        """Return extension directories to scan without activating anything."""
        try:
            from ai_editor.scopes import _base_dir
            base = _base_dir()
        except Exception:
            base = os.path.dirname(os.path.dirname(__file__))
        ext_dirs = []
        ai_ext = os.path.join(base, "ai_editor_extensions")
        if os.path.isdir(ai_ext):
            ext_dirs.append(ai_ext)
        home_ext = os.path.join(os.path.expanduser("~"), ".sao", "extensions")
        if os.path.isdir(home_ext):
            ext_dirs.append(home_ext)
        # VSCode extensions directory
        vscode_ext = os.path.join(os.path.expanduser("~"),
                                   ".vscode", "extensions")
        if os.path.isdir(vscode_ext):
            ext_dirs.append(vscode_ext)
        vscode_insiders_ext = os.path.join(
            os.path.expanduser("~"), ".vscode-insiders", "extensions")
        if os.path.isdir(vscode_insiders_ext):
            ext_dirs.append(vscode_insiders_ext)
        return ext_dirs

    def _register_ext_tools(self) -> None:
        """Register extension-contributed tools and chat participants."""
        ep = self._ext_host.ext_points
        enabled = set(self._enabled_extension_contributions())
        runtime_participants = {}
        vscode_ns = getattr(self, "_vscode_ns", None)
        runtime_tools: Dict[str, Any] = {}
        if vscode_ns:
            runtime_participants = vscode_ns.chat_participants
            runtime_tools = vscode_ns.registered_tools
        manifest_tool_names: set[str] = set()
        for tool in ep.language_model_tools:
            if "languageModelTools" not in enabled:
                continue
            name = tool.get("name", "")
            if not name:
                continue
            manifest_tool_names.add(name)
            ext_id = str(tool.get("_extensionId", ""))
            runtime_tool = runtime_tools.get(name)
            runtime_available = self._lm_runtime_tool_available(runtime_tool)
            needs_runtime = not runtime_available and bool(
                tool.get("_runtimeSupport", {}).get("needsExtensionRuntime"))
            schema = (tool.get("inputSchema") or tool.get("parametersSchema")
                      or self._lm_runtime_tool_schema(runtime_tool) or {
                "type": "object", "properties": {}}
                      )
            runtime_message = self._extension_tool_runtime_message(
                "languageModelTool", ext_id, name, needs_runtime)
            self._registry.register(
                name=self._extension_tool_wrapper_name(ext_id, name),
                description=self._extension_tool_description(
                    name, tool, runtime_tool, needs_runtime),
                parameters=schema,
                handler=lambda _n=name, _tool=tool, **kw: self._invoke_extension_lm_tool(
                    _n, _tool, kw),
                category=f"ext:{ext_id}",
                tags={
                    "extension": True,
                    "extensionId": ext_id,
                    "sourceName": name,
                    "runtimeAvailable": runtime_available,
                    "needsExtensionRuntime": needs_runtime,
                    "runtimeMessage": runtime_message,
                },
            )
        if "languageModelTools" in enabled:
            self._register_runtime_lm_tools(runtime_tools, manifest_tool_names)
        for cp in ep.chat_participants:
            if "chatParticipants" not in enabled:
                continue
            pid = cp.get("id") or cp.get("name", "")
            if not pid:
                continue
            if pid not in runtime_participants:
                continue
            from ai_editor.chat_providers import ChatProviderDef
            prov = ChatProviderDef(
                id=f"ext-{pid}",
                name=cp.get("fullName") or cp.get("name", pid),
                icon=cp.get("icon", "\U0001f916"),
                provider_type=cp.get("_provider_type", "openai"),
                system_prompt=cp.get("description", ""),
                auto_agent=True,
            )
            self._provider_registry.register(prov)
        changed = self._sync_dynamic_webview_providers()
        if changed:
            self._refresh_provider_tabs({"source": "extension_contributions"})
        self._apply_mode_permissions()

    def _register_runtime_lm_tools(self, runtime_tools: Dict[str, Any],
                                   manifest_names: set[str]) -> None:
        for name, tool in runtime_tools.items():
            if not name or name in manifest_names:
                continue
            ext_id = str(self._lm_runtime_tool_extension_id(tool) or "runtime")
            runtime_available = self._lm_runtime_tool_available(tool)
            needs_runtime = not runtime_available
            runtime_message = self._extension_tool_runtime_message(
                "languageModelTool", ext_id, name, needs_runtime)
            self._registry.register(
                name=self._extension_tool_wrapper_name(ext_id, name),
                description=self._extension_tool_description(
                    name, None, tool, needs_runtime),
                parameters=self._lm_runtime_tool_schema(tool),
                handler=lambda _n=name, **kw: self._invoke_registered_lm_tool(_n, kw),
                category=f"ext:{ext_id}",
                tags={
                    "extension": True,
                    "extensionId": ext_id,
                    "sourceName": name,
                    "runtimeAvailable": runtime_available,
                    "needsExtensionRuntime": needs_runtime,
                    "runtimeMessage": runtime_message,
                },
            )

    @staticmethod
    def _lm_runtime_tool_extension_id(tool: Any) -> str:
        if isinstance(tool, dict):
            return str(tool.get("_extensionId") or tool.get("extensionId") or "")
        return ""

    @staticmethod
    def _lm_runtime_tool_schema(tool: Any) -> Dict[str, Any]:
        if isinstance(tool, dict):
            schema = tool.get("inputSchema") or tool.get("schema")
            if isinstance(schema, dict):
                return schema
            nested = tool.get("tool")
            nested_schema = getattr(nested, "inputSchema", None)
            if isinstance(nested_schema, dict):
                return nested_schema
        schema = getattr(tool, "inputSchema", None)
        if isinstance(schema, dict):
            return schema
        return {"type": "object", "properties": {}}

    @staticmethod
    def _lm_runtime_tool_available(tool: Any) -> bool:
        if tool is None:
            return False
        if isinstance(tool, dict):
            handler = tool.get("invoke") or tool.get("handler") or tool.get("callback")
            if callable(handler):
                return True
            nested = tool.get("tool")
            return hasattr(nested, "invoke") or callable(nested)
        return hasattr(tool, "invoke") or callable(tool)

    @staticmethod
    def _extension_tool_description(name: str, manifest_tool: Optional[Dict[str, Any]],
                                    runtime_tool: Any, needs_runtime: bool) -> str:
        desc = ""
        if manifest_tool:
            desc = str(
                manifest_tool.get("modelDescription")
                or manifest_tool.get("description")
                or manifest_tool.get("displayName")
                or name
            )
        if not desc and isinstance(runtime_tool, dict):
            nested = runtime_tool.get("tool")
            desc = str(runtime_tool.get("description") or getattr(nested, "description", "") or name)
        if not desc:
            desc = str(getattr(runtime_tool, "description", "") or name)
        if needs_runtime:
            desc = f"{desc} [No runtime callback registered yet.]"
        return desc

    @staticmethod
    def _extension_tool_runtime_message(contribution: str, extension_id: str,
                                        name: str, needs_runtime: bool) -> str:
        if not needs_runtime:
            return "Runtime handler registered; tool is invocable."
        return (
            f"VSCode {contribution} '{name}' from extension '{extension_id}' "
            "has metadata but no runtime callback is registered yet."
        )

    def _invoke_registered_lm_tool(self, name: str,
                                   arguments: Dict[str, Any]) -> Dict[str, Any]:
        result = self.invoke_lm_tool(name, arguments)
        return result if isinstance(result, dict) else {"result": result}

    def _invoke_extension_lm_tool(self, name: str, manifest_tool: Dict[str, Any],
                                  arguments: Dict[str, Any]) -> Dict[str, Any]:
        vscode_ns = getattr(self, "_vscode_ns", None)
        registered = vscode_ns.registered_tools.get(name) if vscode_ns else None
        if registered:
            result = self.invoke_lm_tool(name, arguments)
            return result if isinstance(result, dict) else {"result": result}
        runtime = _as_dict(manifest_tool.get("_runtimeSupport"))
        if not runtime:
            runtime = {
                "ok": False,
                "unsupported": True,
                "needsExtensionRuntime": True,
                "code": "needsExtensionRuntime",
                "contribution": "languageModelTool",
                "extensionId": manifest_tool.get("_extensionId", ""),
                "id": name,
                "message": (
                    f"VSCode languageModelTool '{name}' has metadata in "
                    "SAO AI Editor, but no runtime callback is registered yet."
                ),
            }
        payload = dict(runtime)
        payload["arguments"] = dict(arguments)
        return payload

    def _sync_extension_tools(self) -> None:
        if not hasattr(self, "_ext_host"):
            return
        if not getattr(self, "_extensions_inited", False):
            self.init_extensions()
        self._register_ext_tools()

    @staticmethod
    def _extension_tool_wrapper_name(extension_id: str, tool_name: str) -> str:
        safe_ext = re.sub(r"[^A-Za-z0-9_]+", "_", extension_id or "extension").strip("_")
        safe_name = re.sub(r"[^A-Za-z0-9_]+", "_", tool_name or "tool").strip("_")
        return f"ext_{safe_ext}_{safe_name}"

    def _manifest_chat_participant(self, participant_id: str) -> Dict[str, Any]:
        for participant in self._ext_host.ext_points.chat_participants:
            pid = participant.get("id") or participant.get("name", "")
            if pid == participant_id:
                return dict(participant)
        return {}

    def _extension_settings(self) -> Dict[str, Any]:
        ai = _normalize_ai_editor_config(self._settings_getter("ai_editor", {}) or {})
        return _as_dict(ai.get("extensions"))

    def _enabled_extension_contributions(self) -> List[str]:
        ext = self._extension_settings()
        raw = ext.get("enabled_contributions", [])
        if isinstance(raw, list) and raw:
            enabled = [str(x) for x in raw]
            for name in _AI_EDITOR_SECTION_DEFAULTS["extensions"]["enabled_contributions"]:
                if name not in enabled:
                    enabled.append(name)
            return enabled
        return list(_AI_EDITOR_SECTION_DEFAULTS["extensions"]["enabled_contributions"])

    @staticmethod
    def _publisher_from_extension_id(ext_id: str) -> str:
        return ext_id.split(".", 1)[0].strip().lower() if "." in ext_id else ""

    def _extension_allowed(self, ext: Any) -> bool:
        settings = self._extension_settings()
        publisher = str(getattr(ext, "publisher", "") or "").strip().lower()
        if not publisher:
            publisher = self._publisher_from_extension_id(str(getattr(ext, "id", "")))
        blocked = {str(x).strip().lower() for x in settings.get("blocked_publishers", []) if str(x).strip()}
        allowed = {str(x).strip().lower() for x in settings.get("allowed_publishers", []) if str(x).strip()}
        if publisher in blocked:
            return False
        if allowed and publisher not in allowed:
            return False
        return True

    def _extension_id_allowed(self, ext_id: str) -> bool:
        class _Ext:
            pass
        ext = _Ext()
        ext.id = ext_id
        ext.publisher = self._publisher_from_extension_id(ext_id)
        return self._extension_allowed(ext)

    def _count_extension_manifests(self) -> int:
        """Count VS Code-compatible package.json manifests without activation."""
        try:
            from ai_editor.extension_host import ExtensionScanner
        except Exception:
            return 0
        count = 0
        for directory in self._extension_scan_dirs():
            try:
                for ext in ExtensionScanner.scan_directory(directory):
                    if self._extension_allowed(ext):
                        count += 1
            except Exception:
                continue
        return count

    def _mcp_discovered_server_entries(self) -> List[Dict[str, Any]]:
        """Discover MCP server configs without starting stdio/SSE transports."""
        try:
            from ai_editor.mcp_client import _iter_server_configs, _parse_server_config
        except Exception:
            return []

        mcp = self._mcp_settings()
        collision = str(mcp.get("collision_behavior", "first")).strip().lower()
        if collision not in {"first", "last", "error"}:
            collision = "first"
        entries: List[Dict[str, Any]] = []
        seen: set[str] = set()

        def append(raw: Any, source: str) -> None:
            nonlocal entries
            for sid, sconf in _iter_server_configs(raw):
                if not sid:
                    continue
                if sid in seen:
                    if collision == "last":
                        entries = [e for e in entries if e.get("id") != sid]
                        seen.discard(sid)
                    elif collision == "error":
                        continue
                    else:
                        continue
                try:
                    cfg = _parse_server_config(sid, sconf)
                except Exception:
                    continue
                if not cfg.enabled:
                    continue
                seen.add(cfg.id)
                entries.append({
                    "id": cfg.id,
                    "name": cfg.name,
                    "transport": cfg.transport,
                    "enabled": cfg.enabled,
                    "source": source,
                })

        append(self._settings_getter("ai_editor_mcp_servers", []),
               "settings.ai_editor_mcp_servers")
        append(mcp.get("servers", []), "settings.ai_editor.mcp.servers")
        append(mcp.get("mcpServers", {}), "settings.ai_editor.mcp.mcpServers")

        if not _as_bool(mcp.get("discovery_enabled"), True):
            return entries

        try:
            from config import BASE_DIR
            base = BASE_DIR
        except Exception:
            base = os.path.dirname(os.path.dirname(__file__))
        for candidate in ("mcp.json", ".mcp/mcp.json", ".vscode/mcp.json"):
            path = os.path.join(base, candidate)
            if not os.path.isfile(path):
                continue
            try:
                with open(path, "r", encoding="utf-8") as f:
                    data = json.load(f)
                append(data.get("mcpServers") or data.get("servers") or {}, candidate)
            except Exception:
                continue

        try:
            home_mcp = os.path.join(os.path.expanduser("~"), ".sao", "mcp.json")
            if os.path.isfile(home_mcp):
                with open(home_mcp, "r", encoding="utf-8") as f:
                    data = json.load(f)
                append(data.get("mcpServers") or data.get("servers") or {}, "~/.sao/mcp.json")
        except Exception:
            pass

        plugins_dir = os.path.join(base, "plugins")
        if os.path.isdir(plugins_dir):
            for pname in sorted(os.listdir(plugins_dir)):
                manifest = os.path.join(plugins_dir, pname, "plugin.json")
                if not os.path.isfile(manifest):
                    continue
                try:
                    with open(manifest, "r", encoding="utf-8") as f:
                        pdata = json.load(f)
                    plugin_servers = {}
                    for sid, sconf in (pdata.get("mcpServers") or {}).items():
                        plugin_servers[f"{pname}.{sid}"] = sconf
                    append(plugin_servers, f"plugins/{pname}/plugin.json")
                except Exception:
                    continue
        return entries

    def get_runtime_support_summary(self) -> Dict:
        """Read-only JS API: summarize extension/MCP support visibility.

        This method intentionally does not call ``_ensure_engine()`` and does
        not start Node, CLI, stdio, or SSE transports.
        """
        installed: List[Dict[str, Any]] = []
        try:
            from ai_editor.extensions import list_installed
            installed = list_installed()
        except Exception:
            installed = []

        host = getattr(self, "_ext_host", None)
        scanned: List[Dict[str, Any]] = []
        contributions: Dict[str, Any] = {}
        if host:
            try:
                scanned = host.list_extensions()
            except Exception:
                scanned = []
            try:
                contributions = host.get_contributes_summary()
            except Exception:
                contributions = {}

        mcp = self._mcp_settings()
        autostart = _as_bool(mcp.get("autostart"), False)
        discovery_enabled = _as_bool(mcp.get("discovery_enabled"), True)
        discovered = self._mcp_discovered_server_entries()
        manager = getattr(self, "_mcp", None)
        live_servers = manager.list_servers() if manager else []
        live_by_id = {str(s.get("id")): s for s in live_servers}
        servers = []
        for entry in discovered:
            live = live_by_id.get(str(entry.get("id")), {})
            servers.append({
                "id": entry.get("id", ""),
                "name": entry.get("name", ""),
                "transport": entry.get("transport", ""),
                "enabled": entry.get("enabled", True),
                "source": entry.get("source", ""),
                "connected": bool(live.get("alive")),
                "tools": int(live.get("tools", 0) or 0),
            })
        for live in live_servers:
            sid = str(live.get("id", ""))
            if sid and sid not in {str(s.get("id")) for s in servers}:
                servers.append({
                    "id": sid,
                    "name": sid,
                    "transport": live.get("transport", ""),
                    "enabled": True,
                    "source": "runtime",
                    "connected": bool(live.get("alive")),
                    "tools": int(live.get("tools", 0) or 0),
                })
        connected_count = sum(1 for s in servers if s.get("connected"))
        tool_count = sum(int(s.get("tools", 0) or 0) for s in servers)
        status = "connected" if connected_count else (
            "configured" if discovered else "not_configured")
        if self._mcp_access() == "disabled":
            status = "disabled"

        return {
            "support_tier": "manifest_api_compatibility",
            "support_label": "Manifest/API compatibility; full Node VS Code host not yet enabled",
            "node_host_enabled": False,
            "node_sidecar_enabled": False,
            "summary_api_launches_external_commands": False,
            "extensions": {
                "installed_count": len(installed),
                "installed": installed,
                "scanned_manifest_count": len(scanned),
                "compatible_manifest_count": self._count_extension_manifests(),
                "activated_count": sum(1 for e in scanned if e.get("activated")),
                "enabled_contributions": self._enabled_extension_contributions(),
                "contribution_counts": contributions,
            },
            "mcp": {
                "access": self._mcp_access(),
                "autostart": autostart,
                "discovery_enabled": discovery_enabled,
                "discovered_server_count": len(discovered),
                "connected_server_count": connected_count,
                "tool_count": tool_count,
                "status": status,
                "servers": servers,
            },
        }

    def list_vscode_extensions(self) -> Dict:
        self._ensure_engine()
        return {"extensions": self._ext_host.list_extensions(),
                "contributes": self._ext_host.get_contributes_summary()}

    def activate_extension(self, ext_id: str) -> Dict:
        self._ensure_engine()
        ext = self._ext_host.registry.get(ext_id)
        if ext and not self._extension_allowed(ext):
            return {"error": f"Extension blocked by trust policy: {ext_id}"}
        act = self._ext_host.activator.activate(ext_id)
        if act:
            self._register_ext_tools()
            return {"ok": True, "id": ext_id,
                    "activationTimeMs": act.activation_time_ms}
        return {"error": f"Failed to activate: {ext_id}"}

    def execute_command(self, command_id: str, *args: Any) -> Dict:
        self._ensure_engine()
        try:
            result = self._ext_host.commands.execute(command_id, *args)
            if isinstance(result, dict):
                return result
            return {"result": result}
        except KeyError:
            return {"error": f"Command not found: {command_id}"}
        except Exception as exc:
            return {"error": str(exc)}

    def list_commands(self) -> Dict:
        self._ensure_engine()
        return {"commands": self._ext_host.commands.list_commands()}

    # ── VSCode API ──

    def get_vscode_api(self) -> Dict:
        """Return summary of the vscode.* namespace state."""
        self._ensure_engine()
        return {
            "chat_participants": list(self._vscode_ns.chat_participants.keys()),
            "lm_tools": list(self._vscode_ns.registered_tools.keys()),
            "variables": list(self._vscode_ns.variables.keys()),
            "commands": self._ext_host.commands.list_commands(),
            "contributes": self._ext_host.get_contributes_summary(),
        }

    def get_extension_contributions(self) -> Dict:
        """Return processed VSCode contribution details with runtime metadata."""
        self._ensure_engine()
        self._sync_extension_tools()
        contributions = self._decorate_extension_contributions(
            self._ext_host.ext_points.all_contributions)
        return {
            "summary": self._ext_host.get_contributes_summary(),
            "contributions": contributions,
        }

    def get_extension_settings(self, ext_id: str = "") -> Dict:
        """EXT-10: Return extension-contributed configuration schema and values.

        If *ext_id* is given, return only that extension's settings.
        Otherwise return all extension configurations.
        """
        self._ensure_engine()
        configs = list(self._ext_host.ext_points._configurations)
        if ext_id:
            configs = [c for c in configs if c.get("_extensionId") == ext_id]
        result: List[Dict[str, Any]] = []
        for cfg in configs:
            eid = cfg.get("_extensionId", "")
            ctx = self._ext_host.activator.get_context(eid)
            ws_state = ctx.workspace_state if ctx else None
            properties = cfg.get("properties", {})
            values: Dict[str, Any] = {}
            if isinstance(properties, dict) and ws_state:
                for key in properties:
                    stored = ws_state.get(key)
                    if stored is not None:
                        values[key] = stored
            result.append({
                "_extensionId": eid,
                "title": cfg.get("title", eid),
                "properties": properties,
                "values": values,
            })
        return {"configurations": result}

    def save_extension_setting(self, ext_id: str, key: str, value: Any) -> Dict:
        """EXT-10: Save a single extension configuration value to workspace state."""
        self._ensure_engine()
        ctx = self._ext_host.activator.get_context(ext_id)
        if not ctx:
            return {"error": f"Extension context not found: {ext_id}"}
        ctx.workspace_state.update(key, value)
        return {"ok": True, "extension": ext_id, "key": key}

    def _decorate_extension_contributions(
            self,
            contributions: Dict[str, Any]) -> Dict[str, Any]:
        decorated = dict(contributions)
        decorated["commands"] = [
            self._decorate_extension_command(item)
            for item in contributions.get("commands", [])
        ]
        decorated["chatParticipants"] = [
            self._decorate_chat_participant(item)
            for item in contributions.get("chatParticipants", [])
        ]
        decorated["languageModelTools"] = [
            self._decorate_language_model_tool(item)
            for item in contributions.get("languageModelTools", [])
        ]
        decorated["views"] = {
            location: [self._decorate_extension_view(item) for item in items]
            for location, items in contributions.get("views", {}).items()
        }
        return decorated

    def _decorate_extension_command(self, item: Dict[str, Any]) -> Dict[str, Any]:
        command = dict(item)
        command_id = str(command.get("command", ""))
        if not command_id:
            return command
        meta = self._ext_host.ext_points.describe_manifest_command(command_id)
        selected = dict(meta.get("selectedFallback") or {})
        runtime_available = False
        if selected:
            kind = str(selected.get("kind", ""))
            target_id = str(selected.get("id", ""))
            if kind == "chatParticipant":
                runtime_available = target_id in self._vscode_ns.chat_participants
            elif kind == "languageModelTool":
                runtime_available = self._lm_runtime_tool_available(
                    self._vscode_ns.registered_tools.get(target_id))
            elif kind in {"view", "treeView", "webviewView"}:
                runtime_available = bool(
                    self._extension_view_snapshot(target_id).get("runtimeAvailable"))
        command["runtimeAvailable"] = runtime_available
        command["fallbackAvailable"] = bool(selected)
        command["fallback"] = selected or None
        command["availableFallbacks"] = meta.get("availableFallbacks", [])
        command["activation"] = meta.get("activation", command.get("_activation", {}))
        command["runtimeMessage"] = meta.get("message", "")
        return command

    def _decorate_chat_participant(self, item: Dict[str, Any]) -> Dict[str, Any]:
        participant = dict(item)
        participant_id = str(participant.get("id") or participant.get("name") or "")
        participant["runtimeAvailable"] = participant_id in self._vscode_ns.chat_participants
        participant["runtimeMessage"] = (
            "Runtime chat participant registered."
            if participant["runtimeAvailable"] else
            participant.get("_runtimeSupport", {}).get("message", ""))
        return participant

    def _decorate_language_model_tool(self, item: Dict[str, Any]) -> Dict[str, Any]:
        tool = dict(item)
        tool_name = str(tool.get("name", ""))
        runtime = self._vscode_ns.registered_tools.get(tool_name)
        tool["runtimeAvailable"] = self._lm_runtime_tool_available(runtime)
        tool["runtimeMessage"] = self._extension_tool_runtime_message(
            "languageModelTool",
            str(tool.get("_extensionId", "")),
            tool_name,
            not tool["runtimeAvailable"],
        )
        return tool

    def _decorate_extension_view(self, item: Dict[str, Any]) -> Dict[str, Any]:
        view = dict(item)
        snapshot = self._extension_view_snapshot(str(view.get("id") or ""))
        view.update({
            "runtimeAvailable": bool(snapshot.get("runtimeAvailable")),
            "runtimeKind": snapshot.get("kind") or "view",
            "runtimeMessage": snapshot.get("message", ""),
        })
        if snapshot:
            view["runtimeState"] = snapshot
        return view

    def _resolve_extension_command_fallback(
            self,
            command_record: Dict[str, Any],
            selected_fallback: Dict[str, Any],
            arguments: List[Any]) -> Optional[Dict[str, Any]]:
        fallback_kind = str(selected_fallback.get("kind", ""))
        fallback_id = str(selected_fallback.get("id", ""))
        if fallback_kind == "chatParticipant" and fallback_id:
            prompt = self._coerce_extension_command_prompt(arguments)
            payload = self.invoke_chat_participant(fallback_id, prompt)
            payload.setdefault("ok", payload.get("error") is None)
            payload.update({
                "handledBy": "runtimeFallback",
                "fallbackKind": fallback_kind,
                "participantId": fallback_id,
                "prompt": prompt,
            })
            return payload
        if fallback_kind == "languageModelTool" and fallback_id:
            tool_input = self._coerce_extension_command_input(arguments)
            payload = self.invoke_lm_tool(fallback_id, tool_input)
            payload.setdefault("ok", payload.get("error") is None)
            payload.update({
                "handledBy": "runtimeFallback",
                "fallbackKind": fallback_kind,
                "toolName": fallback_id,
                "input": tool_input,
            })
            return payload
        if fallback_kind in {"view", "treeView", "webviewView"} and fallback_id:
            snapshot = self._extension_view_snapshot(fallback_id)
            snapshot.setdefault("ok", bool(snapshot.get("runtimeAvailable")))
            snapshot.update({
                "handledBy": "runtimeFallback" if snapshot.get("runtimeAvailable") else "manifestFallback",
                "fallbackKind": snapshot.get("kind") or fallback_kind,
                "viewId": fallback_id,
            })
            return snapshot
        return None

    @staticmethod
    def _coerce_extension_command_prompt(arguments: List[Any]) -> str:
        if not arguments:
            return ""
        first = arguments[0]
        if isinstance(first, str):
            return first
        if isinstance(first, dict):
            for key in ("prompt", "message", "input", "text", "query"):
                value = first.get(key)
                if value is not None:
                    return str(value)
        return str(first)

    @staticmethod
    def _coerce_extension_command_input(arguments: List[Any]) -> Any:
        if not arguments:
            return {}
        first = arguments[0]
        if isinstance(first, dict):
            return dict(first)
        if len(arguments) == 1:
            return {"value": first}
        return {"arguments": list(arguments)}

    def _extension_view_snapshot(self, view_id: str) -> Dict[str, Any]:
        if not view_id:
            return {}
        tree_provider = self._vscode_ns._tree_data_providers.get(view_id)
        tree_view = self._vscode_ns._tree_views.get(view_id)
        if tree_provider and tree_view is None:
            tree_view = self._vscode_ns._create_tree_view(
                view_id, treeDataProvider=tree_provider)
        if tree_provider or tree_view:
            return {
                "ok": True,
                "kind": "treeView",
                "runtimeAvailable": True,
                "message": "Runtime tree view provider registered.",
                "title": getattr(tree_view, "title", view_id),
                "selection": list(getattr(tree_view, "selection", []) or []),
                "children": self._tree_view_children_preview(tree_provider),
            }
        webview_provider = self._vscode_ns._webview_view_providers.get(view_id, {})
        webview_view = self._vscode_ns._webview_views.get(view_id)
        if webview_provider or webview_view:
            if webview_view is None:
                provider = webview_provider.get("provider")
                self._vscode_ns._register_webview_view_provider(view_id, provider)
                webview_view = self._vscode_ns._webview_views.get(view_id)
            return {
                "ok": True,
                "kind": "webviewView",
                "runtimeAvailable": True,
                "message": "Runtime webview provider registered.",
                "title": getattr(webview_view, "title", view_id),
                "html": getattr(getattr(webview_view, "webview", None), "html", ""),
                "visible": bool(getattr(webview_view, "visible", False)),
            }
        manifest_view = self._manifest_view(view_id)
        if manifest_view:
            return {
                "ok": False,
                "kind": str(manifest_view.get("type") or "view"),
                "runtimeAvailable": False,
                "message": manifest_view.get("_runtimeSupport", {}).get(
                    "message", "View manifest is present but no runtime provider is registered."),
                "title": manifest_view.get("name") or view_id,
                "location": manifest_view.get("_viewLocation", ""),
            }
        return {
            "ok": False,
            "kind": "view",
            "runtimeAvailable": False,
            "message": f"No registered runtime or manifest view found for '{view_id}'.",
        }

    def _manifest_view(self, view_id: str) -> Dict[str, Any]:
        for views in self._ext_host.ext_points.all_contributions.get("views", {}).values():
            for view in views:
                if str(view.get("id") or "") == view_id:
                    return dict(view)
        return {}

    @staticmethod
    def _tree_view_children_preview(provider: Any) -> List[str]:
        if provider is None or not hasattr(provider, "getChildren"):
            return []
        try:
            children = provider.getChildren(None)
        except TypeError:
            children = provider.getChildren()
        except Exception:
            return []
        if not isinstance(children, list):
            try:
                children = list(children)
            except Exception:
                return []
        return [str(child) for child in children[:20]]

    def invoke_chat_participant(self, participant_id: str,
                                 prompt: str) -> Dict:
        """Invoke a registered chat participant's handler."""
        self._ensure_engine()
        from ai_editor.vscode_api import (
            ChatRequest, ChatContext, ChatResponseStream)
        cp = self._vscode_ns.chat_participants.get(participant_id)
        if not cp:
            manifest = self._manifest_chat_participant(participant_id)
            runtime = _as_dict(manifest.get("_runtimeSupport"))
            if runtime:
                payload = dict(runtime)
                payload["participantId"] = participant_id
                return payload
            return {"error": f"Participant not found: {participant_id}"}
        req = ChatRequest(prompt=prompt)
        ctx = ChatContext()
        parts = []
        stream = ChatResponseStream(lambda kind, val: parts.append(val))
        try:
            result = cp.request_handler(req, ctx, stream, None)
            return {"ok": True, "content": stream.get_content(),
                    "result": str(result) if result else None}
        except Exception as exc:
            return {"error": str(exc)}

    def invoke_lm_tool(self, tool_name: str, input_data: Any = None) -> Dict:
        """Invoke a registered LM tool."""
        self._ensure_engine()
        tool = self._vscode_ns.registered_tools.get(tool_name)
        if not tool:
            return {"error": f"Tool not found: {tool_name}"}
        try:
            result = self._vscode_ns._invoke_tool(tool_name, input_data, None)
            if hasattr(result, "content"):
                unsupported = self._unsupported_lm_tool_result(result.content)
                if unsupported:
                    return unsupported
                return {"ok": True, "content": result.content}
            if isinstance(result, dict):
                if result.get("unsupported") or result.get("needsExtensionRuntime"):
                    payload = dict(result)
                    payload.setdefault("ok", False)
                    payload.setdefault("error", payload.get("message") or "Tool is unsupported")
                    return payload
                return {"ok": True, "result": result}
            return {"ok": True, "result": str(result)}
        except Exception as exc:
            return {"error": str(exc)}

    @staticmethod
    def _unsupported_lm_tool_result(content: Any) -> Dict[str, Any]:
        if not isinstance(content, list):
            return {}
        for part in content:
            if not isinstance(part, dict):
                continue
            text = str(part.get("text", ""))
            if text.startswith("unsupported/") or "needsExtensionRuntime" in text:
                return {
                    "ok": False,
                    "error": text,
                    "unsupported": True,
                    "needsExtensionRuntime": "needsExtensionRuntime" in text,
                }
        return {}

    # ── Authentication ──

    def list_auth_sessions(self, provider_id: str = "") -> Dict:
        from ai_editor.auth import get_auth_service
        sessions = get_auth_service().list_sessions(provider_id)
        return {"sessions": [s.to_dict() for s in sessions]}

    def create_auth_session(self, provider_id: str, token: str,
                             label: str = "") -> Dict:
        from ai_editor.auth import get_auth_service
        session = get_auth_service().create_session_from_token(
            provider_id, token, label)
        return {"ok": True, "session": session.to_dict()}

    def remove_auth_session(self, provider_id: str,
                             session_id: str) -> Dict:
        from ai_editor.auth import get_auth_service
        ok = get_auth_service().remove_session(provider_id, session_id)
        return {"ok": ok}

    # ── Claude Proxy ──

    def start_claude_proxy(self) -> Dict:
        """Start the local Anthropic-compatible proxy for Claude Code SDK."""
        self._ensure_engine()
        prov = self._provider_registry.get("claude-code") if self._provider_registry else None
        if not prov:
            return {"error": "Claude Code provider is not registered"}
        unavailable = self._provider_unavailable_reason(prov)
        if unavailable:
            return {"error": unavailable, "running": False}
        proxy_engine = LLMEngine(self._provider_config_for(prov))
        if self._claude_proxy and self._claude_proxy.is_running:
            self._claude_proxy.set_engine(proxy_engine)
            return {"ok": True, "port": self._claude_proxy.port,
                    "base_url": self._claude_proxy.base_url,
                    "env": self._claude_proxy.get_env(),
                    "model": proxy_engine.config.effective_model}
        from ai_editor.claude_proxy import ClaudeProxy
        self._claude_proxy = ClaudeProxy(proxy_engine)
        port = self._claude_proxy.start()
        return {"ok": True, "port": port,
                "base_url": self._claude_proxy.base_url,
                "env": self._claude_proxy.get_env(),
                "model": proxy_engine.config.effective_model}

    def stop_claude_proxy(self) -> Dict:
        if self._claude_proxy:
            self._claude_proxy.stop()
        return {"ok": True}

    def get_claude_proxy_status(self) -> Dict:
        if self._claude_proxy and self._claude_proxy.is_running:
            return {"running": True, "port": self._claude_proxy.port,
                    "base_url": self._claude_proxy.base_url,
                    "model": self._claude_proxy.model}
        return {"running": False}

    def install_extension_dir(self, ext_dir: str) -> Dict:
        self._ensure_engine()
        desc = self._ext_host.install_from_dir(ext_dir)
        if desc:
            if not self._extension_allowed(desc):
                return {"error": f"Extension blocked by trust policy: {desc.id}"}
            self._register_ext_tools()
            return {"ok": True, "id": desc.id, "name": desc.display_name}
        return {"error": "Failed to install from directory"}

    def export_chat(self) -> str:
        if not self._controller:
            return "[]"
        return self._controller.export_messages()

    def get_premium_guide(self) -> Dict:
        from ai_editor.prompts import _check_paid, _decrypt_engine_guide
        if not _check_paid():
            return {"paid": False, "content": ""}
        try:
            return {"paid": True, "content": _decrypt_engine_guide()}
        except Exception:
            return {"paid": True, "content": ""}

    def list_tools(self) -> Dict:
        try:
            self._ensure_engine()
        except Exception as exc:
            print(f"[AIEditor] _ensure_engine failed in list_tools: {exc}")
            return {"tools": [], "error": str(exc)}
        self._sync_extension_tools()
        tools = [
            self._tool_list_item(t)
            for t in self._registry.list_tools()
        ]
        # Add MCP tools
        if self._mcp:
            for t in self._mcp.all_tools():
                tool_name = self._mcp_tool_name(t)
                if not self._mcp_tool_allowed(tool_name):
                    continue
                tools.append({
                    "name": tool_name,
                    "description": f"[MCP:{t.server_id}] {t.description}",
                    "category": f"mcp:{t.server_id}",
                    "requires_confirm": self._mcp_tool_requires_confirm(tool_name),
                    "parameters": normalize_tool_parameters(t.input_schema),
                    "tags": {"mcp": True, "serverId": t.server_id, "sourceName": t.name},
                    "serverId": t.server_id,
                    "sourceName": t.name,
                    "runtimeAvailable": True,
                })
        return {"tools": tools}

    @staticmethod
    def _tool_list_item(t: Any) -> Dict[str, Any]:
        tags = dict(getattr(t, "tags", {}) or {})
        item = {
            "name": t.name,
            "description": t.description,
            "category": t.category,
            "requires_confirm": t.requires_confirm,
            "parameters": t.parameters,
        }
        if tags:
            item["tags"] = tags
        for key in (
                "extensionId", "sourceName", "runtimeAvailable",
                "needsExtensionRuntime", "runtimeMessage"):
            if key in tags:
                item[key] = tags[key]
        return item

    def execute_tool(self, name: str, arguments: str = "{}", confirmed: bool = False) -> str:
        self._ensure_engine()
        self._sync_extension_tools()
        if name.startswith("mcp_") and self._mcp:
            if not self._mcp_tool_allowed(name):
                return json.dumps({"error": f"MCP tool disabled by policy: {name}"}, ensure_ascii=False)
            if self._mcp_tool_requires_confirm(name) and not confirmed:
                return json.dumps({"error": "Tool execution requires confirmation", "requires_confirmation": True}, ensure_ascii=False)
            ok, args = self._parse_mcp_arguments(name, arguments)
            if not ok:
                return json.dumps({"error": args}, ensure_ascii=False)
            return self._mcp.call_tool(name, args)
        tool = self._registry.get(name) if self._registry else None
        perm = self._permission_for_tool(tool, name)
        if perm == "disabled":
            return json.dumps({"error": f"Tool disabled by mode policy: {name}"}, ensure_ascii=False)
        if name == "engine":
            engine_gate = self._engine_action_gate(arguments, confirmed)
            if engine_gate:
                return json.dumps(engine_gate, ensure_ascii=False)
        if perm == "confirm" and not confirmed:
            return json.dumps({"error": "Tool execution requires confirmation", "requires_confirmation": True}, ensure_ascii=False)
        return self._registry.execute(name, arguments)

    def _mcp_settings(self) -> Dict[str, Any]:
        ai = _normalize_ai_editor_config(self._settings_getter("ai_editor", {}) or {})
        return _as_dict(ai.get("mcp"))

    def _mcp_access(self) -> str:
        access = str(self._mcp_settings().get("access", "prompt")).strip().lower()
        return access if access in {"prompt", "read_only", "allow", "disabled"} else "prompt"

    @staticmethod
    def _mcp_tool_name(tool: Any) -> str:
        return f"mcp_{tool.server_id}_{tool.name}"

    def _mcp_tool_source_name(self, name: str) -> str:
        if self._mcp:
            for tool in self._mcp.all_tools():
                if self._mcp_tool_name(tool) == name:
                    return str(tool.name)
        return name[4:] if name.startswith("mcp_") else name

    @staticmethod
    def _is_probably_read_only_mcp_tool(name: str) -> bool:
        lowered = str(name or "").lower()
        parts = [part for part in re.split(r"[_\-.:]+", lowered) if part]
        return any(part.startswith((
            "read", "list", "get", "search", "show", "fetch", "query", "find"
        )) for part in parts)

    def _mcp_tool_allowed(self, name: str) -> bool:
        if self._mode == "ask":
            return False
        override = self._perm_overrides.get(name)
        if override == "disabled":
            return False
        if override in {"allowed", "confirm"}:
            return True
        access = self._mcp_access()
        if access == "disabled":
            return False
        if access == "read_only":
            return self._is_probably_read_only_mcp_tool(
                self._mcp_tool_source_name(name))
        return True

    def _mcp_tool_requires_confirm(self, name: str) -> bool:
        if not self._mcp_tool_allowed(name):
            return False
        override = self._perm_overrides.get(name)
        if override == "allowed":
            return False
        if override == "confirm":
            return True
        access = self._mcp_access()
        if access == "prompt":
            return True
        if access == "read_only":
            return False
        return self._mode == "agent" and access != "allow"

    def _refresh_mcp_tools(self) -> None:
        self._configure_controller_tooling(self._controller)
        for ctrl in self._provider_controllers.values():
            self._configure_controller_tooling(ctrl)

    def list_mcp_servers(self) -> Dict:
        self._ensure_engine()
        saved = self._saved_mcp_servers()
        live = {s.get("id"): s for s in (self._mcp.list_servers() if self._mcp else [])}
        result = []
        for entry in saved:
            item = dict(entry)
            if item.get("id") in live:
                item.update(live[item.get("id")])
                item["configured"] = True
            else:
                item["running"] = False
                item["configured"] = True
            result.append(item)
        for sid, item in live.items():
            if not any(s.get("id") == sid for s in result):
                item = dict(item)
                item["configured"] = False
                result.append(item)
        return {"servers": result}

    def add_mcp_server(self, config: Dict) -> Dict:
        self._ensure_engine()
        if not self._mcp:
            from ai_editor.mcp_client import McpManager
            self._mcp = McpManager()
        from ai_editor.mcp_client import McpServerConfig
        server = {
            "id": str(config.get("id", "")).strip(),
            "name": str(config.get("name", config.get("id", ""))).strip(),
            "transport": str(config.get("transport", "stdio")).strip().lower(),
            "command": str(config.get("command", "")),
            "args": list(config.get("args", [])) if isinstance(config.get("args", []), list) else [],
            "env": dict(config.get("env", {})) if isinstance(config.get("env", {}), dict) else {},
            "url": str(config.get("url", "")),
            "headers": dict(config.get("headers", {})) if isinstance(config.get("headers", {}), dict) else {},
            "enabled": bool(config.get("enabled", True)),
        }
        if not server["id"]:
            return {"error": "MCP server id is required"}
        if server["transport"] not in {"stdio", "sse"}:
            return {
                "error": (
                    f"Unsupported MCP transport: {server['transport']}. "
                    "This build supports stdio and SSE; streamable HTTP is not wired yet."
                )
            }
        try:
            self._upsert_saved_mcp_server(server)
        except RuntimeError as exc:
            return {
                "error": f"MCP server added only for the current runtime: {exc}",
                "applied": False,
            }
        cfg = McpServerConfig(
            id=server["id"],
            name=server["name"] or server["id"],
            transport=server["transport"],
            command=server["command"],
            args=server["args"],
            env=server["env"],
            url=server["url"],
            headers=server["headers"],
            enabled=server["enabled"],
        )
        ok = self._mcp.add_server(cfg) if server["enabled"] and self._mcp_settings().get("autostart") else True
        tool_count = 0
        if ok:
            client = self._mcp._clients.get(cfg.id)
            if client:
                tool_count = len(client.tools)
        self._refresh_mcp_tools()
        return {"ok": ok, "id": cfg.id, "tools": tool_count}

    def remove_mcp_server(self, server_id: str) -> Dict:
        try:
            self._remove_saved_mcp_server(server_id)
        except RuntimeError as exc:
            return {
                "error": f"MCP server removal updated only for the current runtime: {exc}",
                "applied": False,
            }
        if self._mcp:
            self._mcp.remove_server(server_id)
        self._refresh_mcp_tools()
        return {"ok": True}

    def restart_mcp_server(self, server_id: str) -> Dict:
        """Restart an MCP server by stopping and re-launching it."""
        self._ensure_engine()
        if not self._mcp:
            return {"error": "MCP manager not initialized"}
        ok = self._mcp.restart_server(server_id)
        if ok:
            self._refresh_mcp_tools()
            client = self._mcp._clients.get(server_id)
            tool_count = len(client.tools) if client else 0
            return {"ok": True, "id": server_id, "tools": tool_count}
        return {"error": f"Failed to restart MCP server: {server_id}"}

    def get_mcp_logs(self, server_id: str) -> Dict:
        """Return recent stderr log lines from an MCP server."""
        self._ensure_engine()
        if not self._mcp:
            return {"logs": []}
        logs = self._mcp.get_server_logs(server_id)
        return {"logs": logs, "id": server_id}

    def _saved_mcp_servers(self) -> List[Dict[str, Any]]:
        mcp = self._mcp_settings()
        raw = mcp.get("servers", [])
        if isinstance(raw, list):
            return [dict(s) for s in raw if isinstance(s, dict) and s.get("id")]
        if isinstance(raw, dict):
            return [dict(v, id=str(k)) for k, v in raw.items() if isinstance(v, dict)]
        return []

    def _save_mcp_servers(self, servers: List[Dict[str, Any]]) -> Optional[str]:
        settings = _resolve_settings(self._gui_ref)
        if not settings:
            return "Settings not available"
        ai = _normalize_ai_editor_config(settings.get("ai_editor", {}) or {})
        mcp = _as_dict(ai.get("mcp"))
        mcp["servers"] = servers
        ai["mcp"] = mcp
        settings.set("ai_editor", ai)
        try:
            settings.save()
        except Exception as exc:
            return str(exc)
        return None

    def _upsert_saved_mcp_server(self, server: Dict[str, Any]) -> None:
        servers = [s for s in self._saved_mcp_servers() if s.get("id") != server.get("id")]
        servers.append(server)
        persist_error = self._save_mcp_servers(servers)
        if persist_error:
            raise RuntimeError(persist_error)

    def _remove_saved_mcp_server(self, server_id: str) -> None:
        persist_error = self._save_mcp_servers(
            [s for s in self._saved_mcp_servers() if s.get("id") != server_id])
        if persist_error:
            raise RuntimeError(persist_error)

    def register_mcp_tools(self, server_id: str, tools: list, handlers: dict = None) -> Dict:
        """Register internal (Python-native) MCP tools — no subprocess needed.

        Called by plugins to expose their tools as MCP-compatible entries.
        tools: [{"name":"...", "description":"...", "inputSchema":{...}}]
        handlers: {"tool_name": callable}
        """
        self._ensure_engine()
        if not self._mcp:
            from ai_editor.mcp_client import McpManager
            self._mcp = McpManager()
        provider = self._mcp.register_internal(server_id, tools, handlers)
        self._refresh_mcp_tools()
        return {"ok": True, "id": server_id, "tools": len(provider.tools)}

    def test_connection(self, cfg: Dict) -> Dict:
        self._ensure_engine()
        test_cfg = ProviderConfig(
            provider=cfg.get("provider", "openai"),
            api_key=cfg.get("api_key", ""),
            base_url=cfg.get("base_url", ""),
            model=cfg.get("model", ""),
            transport=_normalize_transport(cfg.get("transport")),
            temperature=_as_float(cfg.get("temperature"), 0.7),
            max_tokens=_as_int(cfg.get("max_tokens"), 4096),
            top_p=_as_float(cfg.get("top_p"), 1.0),
            frequency_penalty=_as_float(cfg.get("frequency_penalty"), 0.0),
            presence_penalty=_as_float(cfg.get("presence_penalty"), 0.0),
            stop=_normalize_stop(cfg.get("stop")),
            max_input_tokens=_as_int(cfg.get("max_input_tokens"), 0),
            max_output_tokens=_as_int(cfg.get("max_output_tokens"), 0),
            timeout=_as_int(cfg.get("timeout"), 180),
            extra_headers={str(k): str(v) for k, v in _as_dict(
                cfg.get("extra_headers")).items()},
            extra_body=_as_dict(cfg.get("extra_body")),
        )
        ok, msg = self._engine.test_connection(test_cfg)
        return {"ok": ok, "message": msg}

    def list_history(self) -> Dict:
        try:
            from ai_editor.history import list_conversations
            return {"entries": list_conversations(limit=50)}
        except Exception as exc:
            return {"error": str(exc)}

    def load_history(self, conv_id: str) -> Dict:
        try:
            from ai_editor.history import load_conversation
            data = load_conversation(conv_id)
            return data or {"error": "Not found"}
        except Exception as exc:
            return {"error": str(exc)}

    def delete_history(self, conv_id: str) -> Dict:
        try:
            from ai_editor.history import delete_conversation
            return {"ok": delete_conversation(conv_id)}
        except Exception as exc:
            return {"error": str(exc)}

    def save_feedback(self, message_id: str, rating: str) -> Dict:
        """Persist lightweight local feedback for a rendered chat message."""
        msg_id = str(message_id or "").strip()
        value = str(rating or "").strip().lower()
        if not msg_id:
            return {"error": "message_id is required"}
        if value not in {"up", "down", "none"}:
            return {"error": "rating must be up, down, or none"}
        record = {"rating": value, "updated_at": time.time()}
        settings = _resolve_settings(self._gui_ref)
        if not settings:
            feedback = getattr(self, "_feedback", {})
            if not isinstance(feedback, dict):
                feedback = {}
            feedback[msg_id] = record
            self._feedback = feedback
            return {"ok": True, "message_id": msg_id, "rating": value, "stored": "memory"}
        try:
            ai = _normalize_ai_editor_config(settings.get("ai_editor", {}) or {})
            feedback = ai.get("feedback", {})
            if not isinstance(feedback, dict):
                feedback = {}
            feedback[msg_id] = record
            ai["feedback"] = feedback
            settings.set("ai_editor", ai)
            try:
                settings.save()
            except Exception as exc:
                return {"error": f"Feedback save failed: {exc}", "message_id": msg_id}
            return {"ok": True, "message_id": msg_id, "rating": value, "stored": "settings"}
        except Exception as exc:
            return {"error": str(exc)}

    # ── Extension marketplace API ──

    def search_extensions(self, query: str = "ai chat model", page: int = 1) -> Dict:
        """Search VSCode Marketplace. Returns list of extensions."""
        try:
            from ai_editor.extensions import search_extensions, is_installed
            results = search_extensions(query, page=page)
            for r in results:
                if isinstance(r, dict) and "id" in r:
                    r["installed"] = is_installed(r["id"])
            return {"extensions": results}
        except Exception as exc:
            return {"error": str(exc)}

    def install_extension(self, ext_id: str, vsix_url: str = "", confirmed: bool = False) -> Dict:
        """Install an extension from the marketplace, activate it immediately."""
        try:
            policy = self._extension_settings()
            if policy.get("confirm_install", True) and not confirmed:
                return {"error": "Extension install requires confirmation", "requires_confirmation": True}
            if not self._extension_id_allowed(ext_id):
                return {"error": f"Extension blocked by trust policy: {ext_id}"}
            from ai_editor.extensions import install_extension
            result = install_extension(ext_id, vsix_url)
            if result.get("ok") and result.get("ext_dir"):
                self._ensure_engine()
                # Re-scan so the host picks up the new extension
                desc = self._ext_host.install_from_dir(result["ext_dir"])
                if desc:
                    self._register_ext_tools()
                    result["activated"] = True
                    result["display_name"] = desc.display_name

                    # Activate in Node host if extension has a JS entry point
                    node_host = getattr(self, "_node_ext_host", None)
                    if desc.main and node_host is not None and node_host.is_running:
                        manifest = {
                            "name": desc.name,
                            "publisher": desc.publisher,
                            "version": desc.version,
                            "main": desc.main,
                            "activationEvents": desc.activation_events,
                            "contributes": desc.contributes,
                        }
                        node_host.activate(desc.extension_path, desc.id, manifest)
                        result["node_activated"] = True

                    # Emit event so the frontend refreshes the extensions list
                    display = desc.display_name or desc.name or ext_id
                    self._emit("extensions_changed", {
                        "installed": ext_id,
                        "extensions": self._ext_host.list_extensions(),
                    })
                    self._emit("show_message", {
                        "level": "info",
                        "message": f"Extension {display} installed and activated",
                    })
            return result
        except Exception as exc:
            return {"error": str(exc)}

    def uninstall_extension(self, ext_id: str, confirmed: bool = False) -> Dict:
        """Uninstall an extension."""
        try:
            policy = self._extension_settings()
            if policy.get("confirm_install", True) and not confirmed:
                return {"error": "Extension uninstall requires confirmation", "requires_confirmation": True}
            from ai_editor.extensions import uninstall_extension
            return uninstall_extension(ext_id)
        except Exception as exc:
            return {"error": str(exc)}

    def list_installed_extensions(self) -> Dict:
        """List locally installed VSCode-style extensions."""
        try:
            from ai_editor.extensions import list_installed
            return {"extensions": list_installed()}
        except Exception as exc:
            return {"error": str(exc)}

    def get_extension_detail(self, publisher: str, name: str) -> Dict:
        """Fetch a single extension detail from marketplace."""
        try:
            from ai_editor.extensions import get_extension_detail, is_installed
            result = get_extension_detail(publisher, name)
            if result:
                result["installed"] = is_installed(result["id"])
                return result
            return {"error": "Not found"}
        except Exception as exc:
            return {"error": str(exc)}

    def get_model_info(self, model: str = "") -> Dict:
        """Return context window info for a model."""
        from ai_editor.llm_engine import get_model_context, compaction_threshold
        m = model or (self._engine.config.effective_model if self._engine else "")
        ctx = get_model_context(m)
        return {"model": m, "max_input": ctx["max_input"],
                "max_output": ctx["max_output"],
                "compact_at": compaction_threshold(m)}

    def list_models(self) -> Dict:
        """Return all known models (built-in + custom)."""
        from ai_editor.llm_engine import list_all_models
        return {"models": list_all_models()}

    def list_provider_models(self, provider: str = "", base_url: str = "",
                              api_key: str = "") -> Dict:
        """Fetch available models from a provider's official /models endpoint."""
        self._ensure_engine()
        from ai_editor.llm_engine import _PROVIDER_DEFAULTS
        if not provider:
            provider = self._engine.config.provider
        if not base_url:
            defaults = _PROVIDER_DEFAULTS.get(provider, {})
            base_url = defaults.get("base_url", self._engine.config.effective_base_url)
        if not api_key:
            api_key = self._resolve_provider_key(provider) or self._engine.config.api_key
        return self._fetch_provider_models(provider, base_url, api_key)

    def _official_default_model_for_provider(self, provider: str, base_url: str,
                                             api_key: str) -> str:
        result = self._fetch_provider_models(provider, base_url, api_key, timeout=5.0)
        return str(result.get("default_model") or "")

    def _fetch_provider_models(self, provider: str, base_url: str, api_key: str,
                               timeout: float = 10.0) -> Dict:
        if not base_url:
            return {"error": "No base_url configured", "models": []}
        if provider in {"openai", "anthropic", "deepseek"} and not api_key:
            return {
                "error": f"{provider} API key is required to fetch official models",
                "models": [],
                "provider": provider,
                "default_model": "",
                "source": "api",
            }
        try:
            import httpx
            headers = {"Content-Type": "application/json"}
            if provider == "anthropic" and "anthropic.com" in base_url:
                headers["x-api-key"] = api_key
                headers["anthropic-version"] = "2023-06-01"
                url = f"{base_url.rstrip('/')}/models"
            else:
                if api_key:
                    headers["Authorization"] = f"Bearer {api_key}"
                url = f"{base_url.rstrip('/')}/models"
            with httpx.Client(timeout=timeout) as client:
                resp = client.get(url, headers=headers)
                resp.raise_for_status()
                data = resp.json()
            models = []
            for m in data.get("data", data.get("models", [])):
                if isinstance(m, dict):
                    mid = str(m.get("id") or m.get("name") or "").strip()
                    if not mid:
                        continue
                    models.append({
                        "id": mid,
                        "name": m.get("display_name") or m.get("name") or mid,
                        "created": m.get("created", m.get("created_at", 0)),
                    })
                elif isinstance(m, str):
                    models.append({"id": m, "name": m})
            default_model = models[0]["id"] if models else ""
            return {
                "models": models,
                "provider": provider,
                "default_model": default_model,
                "source": "api",
            }
        except Exception as exc:
            return {"error": str(exc), "models": [], "provider": provider, "default_model": ""}

    def save_custom_model(self, model_name: str, max_input: int = 128000,
                          max_output: int = 4096, tools: bool = True,
                          vision: bool = False, thinking: bool = False,
                          streaming: bool = True) -> Dict:
        """Add or update a model definition."""
        from ai_editor.llm_engine import register_model
        register_model(model_name, max_input, max_output,
                       tools, vision, thinking, streaming)
        persist_error = self._save_models_to_settings()
        if persist_error:
            return {
                "error": f"Model saved only for the current runtime: {persist_error}",
                "applied": True,
                "model": model_name,
            }
        return {"ok": True, "model": model_name}

    def delete_custom_model(self, model_name: str) -> Dict:
        from ai_editor.llm_engine import unregister_model
        unregister_model(model_name)
        persist_error = self._save_models_to_settings()
        if persist_error:
            return {
                "error": f"Model removal updated only for the current runtime: {persist_error}",
                "applied": True,
                "model": model_name,
            }
        return {"ok": True}

    def _save_models_to_settings(self) -> Optional[str]:
        from ai_editor.llm_engine import _model_registry
        settings = _resolve_settings(self._gui_ref)
        if not settings:
            return "Settings not available"
        ai = settings.get("ai_editor", {}) or {}
        if not isinstance(ai, dict):
            ai = {}
        ai["custom_models"] = dict(_model_registry)
        settings.set("ai_editor", ai)
        try:
            settings.save()
        except Exception as exc:
            return str(exc)
        return None

    def get_full_config(self) -> Dict:
        """Return ALL configurable parameters for the active endpoint."""
        self._ensure_engine()
        c = self._engine.config
        return {
            "provider": c.provider, "model": c.effective_model,
            "base_url": c.effective_base_url,
            "temperature": c.temperature, "top_p": c.top_p,
            "max_tokens": c.max_tokens,
            "frequency_penalty": c.frequency_penalty,
            "presence_penalty": c.presence_penalty,
            "stop": c.stop, "timeout": c.timeout,
            "max_input_tokens": c.max_input_tokens,
            "max_output_tokens": c.max_output_tokens,
            "extra_headers": c.extra_headers,
            "extra_body": c.extra_body,
            "context_window": c.effective_context,
        }

    def count_tokens(self, text: str = "") -> Dict:
        self._ensure_engine()
        count = self._engine.estimate_tokens(text)
        return {"tokens": count, "model": self._engine.config.effective_model}

    def count_conversation_tokens(self) -> Dict:
        self._ensure_engine()
        if not self._controller or not self._controller.conversation:
            return {"tokens": 0}
        msgs = self._controller.conversation.to_api_messages()
        count = self._engine.count_message_tokens(msgs)
        return {"tokens": count, "messages": len(msgs)}

    def send_image_message(self, text: str, image_base64: str, mime: str = "image/png") -> Dict:
        """Send a message with an attached image (vision)."""
        if not image_base64:
            return {"error": "No image data"}
        if self._controller and self._controller._running:
            return {"error": "Already running"}
        try:
            self._ensure_engine()
            display = text or "What is this image?"
            if self._is_anthropic():
                content = self._engine.make_image_content_anthropic(display, image_base64, mime)
            else:
                content = self._engine.make_image_content(display, image_base64, mime)
            self._controller.send_multimodal(display, content, agent_mode=(self._mode == "agent"))
            return {"ok": True}
        except Exception as exc:
            self._emit("error", {"error": str(exc)})
            return {"error": str(exc)}

    def _is_anthropic(self) -> bool:
        return self._engine and self._engine._is_anthropic_native(self._engine.config)

    def confirm_tool(self, call_id: str, allowed: bool) -> Dict:
        """UI calls this to allow/deny a pending tool confirmation."""
        evt = self._pending_confirm.pop(call_id, None)
        if not evt and call_id not in self._confirm_results:
            return {"error": "No pending confirmation"}
        self._confirm_results[call_id] = allowed
        if evt:
            evt.set()
        return {"ok": True}

    # ── Editor state API (called by JS, also used by editor tools) ──

    def editor_get_content(self) -> Dict:
        """Get editor content + language in a single JS eval."""
        if not self._window:
            return {"content": "", "language": "plaintext"}
        try:
            raw = self._window.evaluate_js(
                "JSON.stringify({c:document.getElementById('editor-text').value,l:editorLang})"
            )
            d = json.loads(raw) if raw else {}
            return {"content": d.get("c", ""), "language": d.get("l", "plaintext")}
        except Exception:
            return {"content": "", "language": "plaintext"}

    def editor_set_content(self, content: str, language: str = "", filename: str = "") -> Dict:
        """Set editor content."""
        js = json.dumps(content)
        if not self._eval_js(f"openInEditor({js},{json.dumps(language or '')})"):
            return {"error": "Editor window is not available"}
        if filename:
            if not self._eval_js(f"editorFileName={json.dumps(filename)}"):
                return {"error": "Editor filename update failed"}
        return {"ok": True, "length": len(content)}

    def editor_insert_text(self, text: str) -> Dict:
        """Insert text at cursor position."""
        js = json.dumps(text)
        if not self._eval_js(f"""(function(){{
            var ed=document.getElementById('editor-text');
            var s=ed.selectionStart;
            ed.value=ed.value.substring(0,s)+{js}+ed.value.substring(ed.selectionEnd);
            ed.selectionStart=ed.selectionEnd=s+{len(text)};
            updateLineNums();updateCursorPos();
        }})()"""):
            return {"error": "Editor window is not available"}
        return {"ok": True}

    def editor_get_selection(self) -> Dict:
        """Get selected text from editor."""
        if not self._window:
            return {"selection": "", "start": 0, "end": 0}
        try:
            result = self._window.evaluate_js("""
                (function(){
                    var ed=document.getElementById('editor-text');
                    return JSON.stringify({
                        selection:ed.value.substring(ed.selectionStart,ed.selectionEnd),
                        start:ed.selectionStart, end:ed.selectionEnd
                    });
                })()
            """)
            return json.loads(result) if result else {"selection": "", "start": 0, "end": 0}
        except Exception:
            return {"selection": "", "start": 0, "end": 0}

    def editor_go_to_line(self, line: int) -> Dict:
        """Navigate editor to a specific line."""
        if not self._eval_js(f"""(function(){{
            var ed=document.getElementById('editor-text');
            var lines=ed.value.split('\\n');
            var pos=0;for(var i=0;i<Math.min({line}-1,lines.length-1);i++)pos+=lines[i].length+1;
            ed.selectionStart=ed.selectionEnd=pos;ed.focus();
            updateCursorPos();ed.scrollTop=Math.max(0,({line}-10)*18);
        }})()"""):
            return {"error": "Editor window is not available"}
        return {"ok": True, "line": line}

    def editor_find_replace(self, find: str, replace: str, replace_all: bool = False) -> Dict:
        """Find and replace in editor."""
        f = json.dumps(find)
        r = json.dumps(replace)
        if replace_all:
            ok = self._eval_js(f"""(function(){{
                var ed=document.getElementById('editor-text');
                ed.value=ed.value.split({f}).join({r});updateLineNums();
            }})()""")
        else:
            ok = self._eval_js(f"""(function(){{
                var ed=document.getElementById('editor-text');
                var idx=ed.value.indexOf({f},ed.selectionEnd);
                if(idx===-1)idx=ed.value.indexOf({f});
                if(idx>=0){{
                    ed.value=ed.value.substring(0,idx)+{r}+ed.value.substring(idx+{len(find)});
                    ed.selectionStart=idx;ed.selectionEnd=idx+{len(replace)};
                    updateLineNums();
                }}
            }})()""")
        if not ok:
            return {"error": "Editor window is not available"}
        return {"ok": True}

    def editor_get_language(self) -> Dict:
        """Get current editor language mode."""
        if not self._window:
            return {"language": "plaintext"}
        try:
            lang = self._window.evaluate_js("editorLang")
            return {"language": lang or "plaintext"}
        except Exception:
            return {"language": "plaintext"}

    # ── Events pushed to JS ──

    def _eval_js(self, js: str) -> bool:
        if self._window:
            try:
                self._window.evaluate_js(js)
                return True
            except Exception:
                return False
        return False

    def _emit(self, event: str, data: Any) -> None:
        payload = json.dumps(data, ensure_ascii=False, default=str)
        self._eval_js(f"window._onEditorEvent&&window._onEditorEvent({json.dumps(event)},{payload})")

    def _on_stream_delta(self, msg: ChatMessage, text: str) -> None:
        self._delta_buf.append(text)
        now = time.monotonic()
        if len(self._delta_buf) >= 15 or (now - self._delta_last_flush) > 0.05:
            self._flush_deltas()

    def _on_thinking_delta(self, msg: ChatMessage, text: str) -> None:
        self._thinking_buf.append(text)
        if len(self._thinking_buf) >= 10:
            self._flush_thinking()

    def _flush_deltas(self) -> None:
        if not self._delta_buf:
            return
        combined = "".join(self._delta_buf)
        self._delta_buf.clear()
        self._delta_last_flush = time.monotonic()
        self._emit("stream_delta", {"content": combined})

    def _flush_thinking(self) -> None:
        if not self._thinking_buf:
            return
        combined = "".join(self._thinking_buf)
        self._thinking_buf.clear()
        self._emit("thinking_delta", {"content": combined})

    def _on_stream_end(self, msg: ChatMessage) -> None:
        self._flush_deltas()
        self._flush_thinking()
        payload: Dict[str, Any] = {"content": msg.content, "model": msg.model}
        if msg.thinking:
            payload["thinking"] = msg.thinking
        if msg.tool_calls:
            payload["tool_calls"] = [
                {"id": tc.id, "name": tc.name, "arguments": tc.arguments}
                for tc in msg.tool_calls
            ]
        if msg.is_error:
            payload["error"] = msg.content
        if msg.usage:
            payload["usage"] = msg.usage
        self._emit("stream_end", payload)

    def _on_tool_start(self, call_id: str, name: str, args: str, state: str = "") -> None:
        self._emit("tool_start", {"id": call_id, "name": name, "arguments": args, "state": state})

    def _on_tool_confirm(self, call_id: str, name: str, args: str) -> bool:
        """Called from background thread. Pushes confirm request to JS, blocks until response."""
        return self._wait_for_tool_confirmation("tool_confirm", {
            "id": call_id, "name": name, "arguments": args,
        }, call_id, name, args)

    def _on_provider_tool_confirm(self, provider_id: str, call_id: str, name: str, args: str) -> bool:
        return self._wait_for_tool_confirmation("provider_tool_confirm", {
            "provider": provider_id, "id": call_id, "name": name, "arguments": args,
        }, call_id, name, args)

    def _wait_for_tool_confirmation(self, event: str, payload: Dict[str, Any],
                                    call_id: str, name: str, args: str) -> bool:
        engine_action = self._engine_action_from_arguments(args) if name == "engine" else ""
        if name == "engine" and engine_action not in _DANGEROUS_ENGINE_ACTIONS:
            return True
        if self._mode == "agent":
            return True
        evt = threading.Event()
        self._pending_confirm[call_id] = evt
        self._confirm_results[call_id] = False
        self._emit(event, payload)
        timeout = float(getattr(self, "_confirmation_timeout", 30.0))
        deadline = time.monotonic() + timeout
        while not evt.is_set():
            ctrl = self._controller
            if ctrl is not None and not ctrl._running:
                break
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            evt.wait(timeout=min(0.5, remaining))
        self._pending_confirm.pop(call_id, None)
        result = self._confirm_results.pop(call_id, False)
        if not result and not evt.is_set():
            self._emit("tool_end", {"id": call_id,
                                     "name": name,
                                     "result": '{"error":"confirmation timeout"}',
                                     "state": "cancelled"})
        return result

    def _on_tool_end(self, call_id: str, name: str, result: str, state: str = "") -> None:
        self._emit("tool_end", {"id": call_id, "name": name, "result": result, "state": state})

    def _on_tool_progress(self, call_id: str, name: str, progress: float) -> None:
        self._emit("tool_progress", {"id": call_id, "name": name, "progress": progress})

    def _on_token_warning(self, used: int, limit: int, ratio: float) -> None:
        pct = int(ratio * 100)
        self._emit("token_warning", {"used": used, "limit": limit, "percent": pct})

    def _on_error(self, error: str) -> None:
        self._emit("error", {"error": error})

    def _on_idle(self) -> None:
        self._emit("idle", {})


class _DummyGui:
    """Fallback when launched standalone without SAO instance."""
    settings = None
    _ai_engine_actions = {}
    _plugin_manager = None
    _packet_bridge = None
    _mem_bridge = None
    _start_time = time.monotonic()


# ---------------------------------------------------------------------------
# Launcher
# ---------------------------------------------------------------------------

_running_window = None
_running_thread = None


def _html_path() -> str:
    # onedir: build_release.bat lifts web/ to BASE_DIR (exe top level);
    # _ROOT resolves to runtime/ which no longer contains web/.
    try:
        from config import BASE_DIR
        p = os.path.join(BASE_DIR, 'web', 'ai_editor_app.html')
        if os.path.isfile(p):
            return p
    except Exception:
        pass
    return os.path.join(_ROOT, 'web', 'ai_editor_app.html')


def launch(gui_ref: Any = None, blocking: bool = False) -> None:
    """Open the AI Editor in a pywebview window.

    pywebview requires ``webview.start()`` on the main thread. When called from
    a Tk-hosted SAO instance (main thread occupied by Tk), we spawn a child
    process so pywebview gets its own main thread. The child process imports
    this module and calls ``launch(blocking=True)``.

    If *blocking* is True, runs synchronously (used by the child process and
    CLI ``python -m ai_editor.app``).
    """
    global _running_window, _running_thread

    if _running_window is not None:
        try:
            _running_window.show()
            return
        except Exception:
            _running_window = None

    if blocking:
        _launch_webview_blocking(gui_ref)
        return

    # Non-blocking: pywebview.start() needs main thread. If a Tk mainloop
    # already owns the main thread, spawn a subprocess instead.
    _launch_subprocess()


def _launch_subprocess() -> None:
    """Spawn a separate process for the AI Editor pywebview window.

    Dev:    ``python -m ai_editor.app``
    Frozen: ``XiaoACTUI.exe --ai-editor``  (main.py handles the flag)
    """
    import subprocess as _sp
    html_file = _html_path()
    if not os.path.isfile(html_file):
        print(f"[AIEditor] HTML not found: {html_file}")
        return
    if _activate_existing_ai_editor_window():
        print("[AIEditor] activated existing window")
        return
    if getattr(sys, 'frozen', False):
        cmd = [sys.executable, '--ai-editor']
    else:
        cmd = [sys.executable, '-m', 'ai_editor.app']
    env = dict(os.environ)
    env.setdefault('PYTHONPATH', _ROOT)
    env.setdefault('PYTHONUNBUFFERED', '1')
    cwd = os.path.dirname(sys.executable) if getattr(sys, 'frozen', False) else _ROOT
    flags = 0
    if sys.platform == 'win32':
        BELOW_NORMAL = 0x00004000
        CREATE_NEW_PROCESS_GROUP = 0x00000200
        flags = BELOW_NORMAL | CREATE_NEW_PROCESS_GROUP
    try:
        log_path = os.path.join(os.path.expanduser("~"), ".sao", "ai_editor_subprocess.log")
        os.makedirs(os.path.dirname(log_path), exist_ok=True)
        with open(log_path, "a", encoding="utf-8") as log:
            log.write(f"\n[AIEditor] launch cmd={cmd!r} cwd={cwd!r}\n")
            log.flush()
            proc = _sp.Popen(cmd, cwd=cwd, env=env, creationflags=flags,
                             stdout=log, stderr=log, close_fds=True)
        print(f"[AIEditor] subprocess started (pid={proc.pid}, log={log_path})")
    except Exception as exc:
        print(f"[AIEditor] subprocess failed: {exc}")


_WIN_POS_FILE = os.path.join(os.path.expanduser("~"), ".sao", "ai_editor_pos.json")


def _load_window_pos() -> dict:
    try:
        with open(_WIN_POS_FILE, "r") as f:
            return json.load(f)
    except Exception:
        return {}


def _save_window_pos(x: int, y: int, w: int, h: int) -> None:
    os.makedirs(os.path.dirname(_WIN_POS_FILE), exist_ok=True)
    try:
        with open(_WIN_POS_FILE, "w") as f:
            json.dump({"x": x, "y": y, "w": w, "h": h}, f)
    except Exception:
        pass


def _append_ai_editor_log(message: str) -> None:
    try:
        log_path = os.path.join(os.path.expanduser("~"), ".sao", "ai_editor_subprocess.log")
        os.makedirs(os.path.dirname(log_path), exist_ok=True)
        with open(log_path, "a", encoding="utf-8") as log:
            log.write(f"[AIEditor] {message}\n")
    except Exception:
        pass


def _virtual_screen_bounds() -> tuple[int, int, int, int]:
    try:
        import ctypes
        user32 = ctypes.windll.user32
        x = int(user32.GetSystemMetrics(76))
        y = int(user32.GetSystemMetrics(77))
        width = int(user32.GetSystemMetrics(78))
        height = int(user32.GetSystemMetrics(79))
        if width > 0 and height > 0:
            return x, y, width, height
        width = int(user32.GetSystemMetrics(0))
        height = int(user32.GetSystemMetrics(1))
        if width > 0 and height > 0:
            return 0, 0, width, height
    except Exception:
        pass
    return 0, 0, 1920, 1080


def _primary_screen_bounds() -> tuple[int, int, int, int]:
    try:
        import ctypes
        user32 = ctypes.windll.user32
        width = int(user32.GetSystemMetrics(0))
        height = int(user32.GetSystemMetrics(1))
        if width > 0 and height > 0:
            return 0, 0, width, height
    except Exception:
        pass
    return _virtual_screen_bounds()


def _find_ai_editor_window() -> int:
    if sys.platform != "win32":
        return 0
    try:
        import ctypes
        user32 = ctypes.windll.user32
        user32.FindWindowW.restype = ctypes.c_void_p
        hwnd = user32.FindWindowW(None, _AI_EDITOR_WINDOW_TITLE)
        return int(hwnd or 0)
    except Exception:
        return 0


def _activate_window_handle(hwnd: int, keep_topmost_seconds: float = 0.9) -> bool:
    if sys.platform != "win32" or not hwnd:
        return False
    try:
        import ctypes
        user32 = ctypes.windll.user32
        hwnd_ptr = ctypes.c_void_p(int(hwnd))
        flags = 0x0001 | 0x0002 | 0x0040  # NOSIZE | NOMOVE | SHOWWINDOW
        user32.ShowWindow(hwnd_ptr, 9)  # SW_RESTORE
        user32.SetWindowPos(hwnd_ptr, ctypes.c_void_p(-1), 0, 0, 0, 0, flags)
        user32.SetForegroundWindow(hwnd_ptr)
        if keep_topmost_seconds > 0:
            def _release_topmost() -> None:
                time.sleep(keep_topmost_seconds)
                try:
                    user32.SetWindowPos(hwnd_ptr, ctypes.c_void_p(-2), 0, 0, 0, 0, flags)
                except Exception:
                    pass
            threading.Thread(target=_release_topmost, daemon=True).start()
        return True
    except Exception:
        return False


def _activate_existing_ai_editor_window() -> bool:
    return _activate_window_handle(_find_ai_editor_window())


def _activate_ai_editor_window_with_retry() -> None:
    for _ in range(60):
        if _activate_existing_ai_editor_window():
            return
        time.sleep(0.2)


def _window_int(value: Any, default: int) -> int:
    try:
        return int(value)
    except (TypeError, ValueError, OverflowError):
        return int(default)


def _normalize_window_geometry(saved: Dict[str, Any]) -> tuple[int, int, int, int]:
    screen_x, screen_y, screen_w, screen_h = _virtual_screen_bounds()
    min_w, min_h = _AI_EDITOR_MIN_SIZE
    width = min(max(min_w, _window_int(saved.get("w"), 1000)), max(min_w, screen_w))
    height = min(max(min_h, _window_int(saved.get("h"), 700)), max(min_h, screen_h))

    if saved.get("x") is None or saved.get("y") is None:
        x, y = _default_bottom_right_pos(width, height)
    else:
        x = _window_int(saved.get("x"), screen_x)
        y = _window_int(saved.get("y"), screen_y)

    min_visible = 80
    right = screen_x + screen_w
    bottom = screen_y + screen_h
    offscreen = (
        x + width < screen_x + min_visible
        or y + height < screen_y + min_visible
        or x > right - min_visible
        or y > bottom - min_visible
    )
    if offscreen:
        x, y = _default_bottom_right_pos(width, height)

    if width <= screen_w:
        x = max(screen_x, min(x, right - width))
    else:
        x = screen_x
    if height <= screen_h:
        y = max(screen_y, min(y, bottom - height))
    else:
        y = screen_y
    return x, y, width, height


def _default_bottom_right_pos(width: int = 1000, height: int = 700) -> tuple:
    """Screen bottom-right, 20px above taskbar."""
    screen_x, screen_y, screen_w, screen_h = _primary_screen_bounds()
    # Taskbar ~ 40px, 20px margin above it.
    x = max(screen_x, screen_x + screen_w - int(width) - 20)
    y = max(screen_y, screen_y + screen_h - int(height) - 60)
    return x, y


def _launch_webview_blocking(gui_ref: Any = None) -> None:
    """Run pywebview in the current thread (must be main thread)."""
    global _running_window
    try:
        _append_ai_editor_log("child launch start")
        import webview
        html_file = _html_path()
        if not os.path.isfile(html_file):
            print(f"[AIEditor] HTML not found: {html_file}")
            _append_ai_editor_log(f"HTML not found: {html_file}")
            return

        x, y, w, h = _normalize_window_geometry(_load_window_pos())
        _append_ai_editor_log(f"window geometry x={x} y={y} w={w} h={h}")

        api = AIEditorAPI(gui_ref)
        url = f"file:///{html_file.replace(os.sep, '/')}"
        window = webview.create_window(
            _AI_EDITOR_WINDOW_TITLE,
            url=url,
            width=w,
            height=h,
            x=x,
            y=y,
            min_size=_AI_EDITOR_MIN_SIZE,
            resizable=True,
            js_api=api,
            frameless=True,
            easy_drag=False,
            text_select=True,
        )
        _append_ai_editor_log("window object created")
        _running_window = window
        api.set_window(window, {"x": x, "y": y, "width": w, "height": h})
        _append_ai_editor_log("window bound to API")

        def _on_closed():
            global _running_window
            try:
                _save_window_pos(window.x, window.y, window.width, window.height)
            except Exception:
                pass
            _running_window = None

        def _on_closing():
            api.cancel()
            api._shutdown_node_extension_host()

        window.events.closing += _on_closing
        window.events.closed += _on_closed
        threading.Thread(target=_activate_ai_editor_window_with_retry, daemon=True).start()
        _append_ai_editor_log("webview.start entering")
        webview.start(debug=False)
        _append_ai_editor_log("webview.start returned")
    except Exception as exc:
        _append_ai_editor_log(f"child launch failed: {exc!r}")
        raise


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    launch(blocking=True)
