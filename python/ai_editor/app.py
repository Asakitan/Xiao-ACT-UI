"""SAO AI Editor — standalone pywebview GUI application.

Launch:
    python -m ai_editor.app            # standalone
    python -m ai_editor.app --attach   # attached to running SAO instance

From SAO menu, ``_toggle_ai_editor_panel`` calls ``launch()`` which opens
the pywebview window in a background thread if not already running.
"""

from __future__ import annotations

import base64
import fnmatch
import inspect
import json
import math
import mimetypes
import os
import re
import secrets
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional
from urllib.parse import quote, unquote, urlparse, urlsplit, urlunsplit

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
from ai_editor.extension_host import Position, Range, Uri
from ai_editor.vscode_api import (
    DataTransfer,
    DataTransferItem,
    WorkspaceEdit,
    _resolve_provider_result as _resolve_vscode_provider_result,
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
_WORKSPACE_CONTAINS_PREFIX = "workspaceContains:"
_WORKSPACE_CONTAINS_MAX_FILES = 8000
_WORKSPACE_CONTAINS_MAX_SECONDS = 2.0
_WORKSPACE_FILE_PREVIEW_BYTES = 1024 * 1024
_WEBVIEW_LOCAL_URL_RE = re.compile(
    r"https://(?:webview\.local|[^/\s\"'<>)]*\.vscode-resource\.webview\.local)"
    r"/[^\s\"'<>)]*"
)
_WEBVIEW_RESOURCE_MAX_BYTES = 2 * 1024 * 1024
_WEBVIEW_RESOURCE_TOTAL_MAX_BYTES = 8 * 1024 * 1024
_WEBVIEW_RESOURCE_ROUTE = "/__sao_webview_resource__"
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
_EDITOR_LANGUAGE_DISPLAY_NAMES = {
    "plaintext": "Plain Text",
    "python": "Python",
    "javascript": "JavaScript",
    "typescript": "TypeScript",
    "json": "JSON",
    "html": "HTML",
    "css": "CSS",
    "markdown": "Markdown",
    "yaml": "YAML",
    "xml": "XML",
    "sql": "SQL",
    "shell": "Shell Script",
    "lua": "Lua",
    "c": "C",
    "cpp": "C++",
    "csharp": "C#",
    "java": "Java",
    "go": "Go",
    "rust": "Rust",
    "toml": "TOML",
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
        "enabled_contributions": [
            "chatParticipants",
            "languageModelTools",
            "commands",
            "views",
            "customEditors",
        ],
        "enabled_contributions_explicit": False,
        "diagnostics_enabled": False,
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


def _strip_jsonc_comments(text: str) -> str:
    out: List[str] = []
    i = 0
    in_string = False
    escaped = False
    while i < len(text):
        ch = text[i]
        if in_string:
            out.append(ch)
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
            i += 1
            continue
        if ch == '"':
            in_string = True
            out.append(ch)
            i += 1
            continue
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if ch == "/" and nxt == "/":
            i += 2
            while i < len(text) and text[i] not in "\r\n":
                i += 1
            continue
        if ch == "/" and nxt == "*":
            i += 2
            while i < len(text):
                if text[i] in "\r\n":
                    out.append(text[i])
                if text[i] == "*" and i + 1 < len(text) and text[i + 1] == "/":
                    i += 2
                    break
                i += 1
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def _strip_json_trailing_commas(text: str) -> str:
    out: List[str] = []
    i = 0
    in_string = False
    escaped = False
    while i < len(text):
        ch = text[i]
        if in_string:
            out.append(ch)
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
            i += 1
            continue
        if ch == '"':
            in_string = True
            out.append(ch)
            i += 1
            continue
        if ch == ",":
            j = i + 1
            while j < len(text) and text[j] in " \t\r\n":
                j += 1
            if j < len(text) and text[j] in "}]":
                i += 1
                continue
        out.append(ch)
        i += 1
    return "".join(out)


def _load_jsonc_file(path: str) -> Any:
    with open(path, "r", encoding="utf-8-sig") as f:
        text = f.read()
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        normalized = _strip_json_trailing_commas(_strip_jsonc_comments(text))
        return json.loads(normalized)


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
    if "color_theme" in cfg:
        cfg["color_theme"] = str(cfg.get("color_theme") or "")
    if "file_icon_theme" in cfg:
        cfg["file_icon_theme"] = str(cfg.get("file_icon_theme") or "")
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


_LANGUAGE_RESULT_ATTRS = (
    "items", "isIncomplete", "label", "kind", "detail", "documentation",
    "sortText", "filterText", "insertText", "range", "textEdit",
    "additionalTextEdits", "command", "arguments", "contents", "uri", "targetUri",
    "targetRange", "originSelectionRange", "name", "containerName",
    "children", "selectionRange", "diagnostics", "edit", "title",
    "isPreferred", "disabled", "newText", "position", "value",
    "signatures", "activeSignature", "activeParameter", "parameters",
    "placeholder", "rejectReason", "target", "tooltip", "textEdits",
    "paddingLeft", "paddingRight", "location", "isResolved", "data",
    "resultId", "edits", "start", "end", "deleteCount", "tokenTypes",
    "tokenModifiers", "parent", "color", "red", "green", "blue", "alpha",
    "tags", "from", "fromRanges", "to", "yieldTo", "additionalEdit",
)


def _text_position_for_offset(text: str, offset: Any) -> Position:
    value = str(text or "")
    try:
        raw = int(offset)
    except Exception:
        raw = 0
    index = max(0, min(len(value), raw))
    line = value.count("\n", 0, index)
    line_start = value.rfind("\n", 0, index) + 1
    return Position(line, index - line_start)


def _offset_for_text_position(text: str, position: Any) -> int:
    value = str(text or "")
    pos = _editor_provider_position(position, value)
    target_line = max(0, int(getattr(pos, "line", 0)))
    character = max(0, int(getattr(pos, "character", 0)))
    offset = 0
    for _ in range(target_line):
        next_line = value.find("\n", offset)
        if next_line < 0:
            return len(value)
        offset = next_line + 1
    line_end = value.find("\n", offset)
    limit = len(value) if line_end < 0 else line_end
    return max(offset, min(limit, offset + character))


def _editor_provider_position(value: Any, content: str = "") -> Position:
    if isinstance(value, Position):
        return value
    if isinstance(value, dict):
        try:
            return Position(
                int(value.get("line", 0)),
                int(value.get("character", 0)),
            )
        except Exception:
            return Position()
    if value is not None:
        return _text_position_for_offset(content, value)
    return Position()


def _editor_provider_range(value: Any, content: str = "") -> Range:
    if isinstance(value, Range):
        return value
    if isinstance(value, dict):
        return Range(
            _editor_provider_position(value.get("start"), content),
            _editor_provider_position(value.get("end"), content),
        )
    return Range()


def _editor_provider_uri(payload: Dict[str, Any]) -> Uri:
    raw = (
        payload.get("uri")
        or payload.get("filePath")
        or payload.get("file_path")
        or payload.get("absolute_path")
        or payload.get("path")
    )
    if isinstance(raw, Uri):
        return raw
    if isinstance(raw, dict) and raw.get("scheme"):
        return Uri(
            str(raw.get("scheme") or "file"),
            str(raw.get("path") or ""),
            str(raw.get("authority") or ""),
            str(raw.get("query") or ""),
            str(raw.get("fragment") or ""),
        )
    raw_text = str(raw or "").strip()
    if raw_text:
        if os.path.isabs(raw_text) or (
                len(raw_text) >= 2 and raw_text[1] == ":"
                and raw_text[0].isalpha()):
            return Uri.file(raw_text)
        if ":" in raw_text:
            return Uri.parse(raw_text)
        return Uri.file(raw_text)
    name = re.sub(r"[\r\n?#]+", "-", str(payload.get("name") or "Untitled-1"))
    name = name.strip().replace("\\", "/") or "Untitled-1"
    return Uri.parse(f"untitled:{name}")


def _json_ready_language_value(value: Any, depth: int = 0) -> Any:
    if depth > 8:
        return str(value)
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, Uri):
        return str(value)
    if isinstance(value, Position):
        return {"line": int(value.line), "character": int(value.character)}
    if isinstance(value, Range):
        return {
            "start": _json_ready_language_value(value.start, depth + 1),
            "end": _json_ready_language_value(value.end, depth + 1),
        }
    if isinstance(value, WorkspaceEdit):
        return {
            "_edits": _json_ready_language_value(value.entries(), depth + 1),
        }
    if isinstance(value, DataTransfer):
        return _json_ready_language_value(value.to_payload(), depth + 1)
    if isinstance(value, DataTransferItem):
        return _json_ready_language_value(value.value, depth + 1)
    if isinstance(value, dict):
        return {
            str(key): _json_ready_language_value(item, depth + 1)
            for key, item in value.items()
            if not callable(item)
        }
    if isinstance(value, (list, tuple, set)):
        return [_json_ready_language_value(item, depth + 1) for item in value]

    items = getattr(value, "items", None)
    if items is not None and hasattr(value, "isIncomplete"):
        return {
            "items": _json_ready_language_value(list(items or []), depth + 1),
            "isIncomplete": bool(getattr(value, "isIncomplete", False)),
        }

    data: Dict[str, Any] = {}
    for attr in _LANGUAGE_RESULT_ATTRS:
        if hasattr(value, attr):
            try:
                item = getattr(value, attr)
            except Exception:
                continue
            if item is not None and not callable(item):
                data[attr] = _json_ready_language_value(item, depth + 1)
    if data:
        return data
    if hasattr(value, "__dict__"):
        return {
            str(key): _json_ready_language_value(item, depth + 1)
            for key, item in vars(value).items()
            if not key.startswith("_") and not callable(item)
        }
    return str(value)


# ---------------------------------------------------------------------------
# Local webview resource server
# ---------------------------------------------------------------------------

class _WebviewResourceRequestHandler(BaseHTTPRequestHandler):
    server: "_WebviewResourceHttpServer"

    def log_message(self, format: str, *args: Any) -> None:
        return

    def do_OPTIONS(self) -> None:
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.end_headers()

    def do_HEAD(self) -> None:
        self._serve_file(send_body=False)

    def do_GET(self) -> None:
        self._serve_file(send_body=True)

    def _serve_file(self, send_body: bool) -> None:
        resolved = self.server.owner.resolve_request(self.path)
        if not resolved:
            self.send_error(404)
            return
        fs_path, mime = resolved
        try:
            with open(fs_path, "rb") as fh:
                data = fh.read()
        except OSError:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cross-Origin-Resource-Policy", "cross-origin")
        self.end_headers()
        if send_body:
            self.wfile.write(data)


class _WebviewResourceHttpServer(ThreadingHTTPServer):
    def __init__(self, owner: "_WebviewResourceServer") -> None:
        super().__init__(("127.0.0.1", 0), _WebviewResourceRequestHandler)
        self.owner = owner
        self.daemon_threads = True


class _WebviewResourceServer:
    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._nonce = secrets.token_urlsafe(24)
        self._views: Dict[str, Optional[List[str]]] = {}
        self._server = _WebviewResourceHttpServer(self)
        host, port = self._server.server_address[:2]
        self.origin = f"http://{host}:{int(port)}"
        self._thread = threading.Thread(
            target=self._server.serve_forever, daemon=True)
        self._thread.start()

    def register_view(
            self, view_id: str, roots: Optional[List[str]]) -> str:
        key = str(view_id or "").strip()
        if not key:
            return ""
        normalized = None if roots is None else list(roots)
        with self._lock:
            self._views[key] = normalized
        return self.view_base_url(key)

    def unregister_view(self, view_id: str) -> None:
        key = str(view_id or "").strip()
        if not key:
            return
        with self._lock:
            self._views.pop(key, None)

    def view_base_url(self, view_id: str) -> str:
        key = str(view_id or "").strip()
        if not key:
            return ""
        return (
            f"{self.origin}{_WEBVIEW_RESOURCE_ROUTE}/"
            f"{quote(self._nonce, safe='')}/{quote(key, safe='')}"
        )

    def resource_url(self, view_id: str, original_url: str) -> str:
        parsed = urlsplit(str(original_url or ""))
        base = self.view_base_url(view_id)
        if not base or not parsed.netloc:
            return str(original_url or "")
        path = parsed.path or "/"
        url = (
            f"{base}/{quote(parsed.netloc, safe='')}{path}"
        )
        if parsed.query:
            url += f"?{parsed.query}"
        if parsed.fragment:
            url += f"#{parsed.fragment}"
        return url

    def resolve_request(self, raw_path: str) -> Optional[tuple[str, str]]:
        parsed = urlsplit(str(raw_path or ""))
        parts = parsed.path.split("/")
        if len(parts) < 6 or parts[1] != _WEBVIEW_RESOURCE_ROUTE.strip("/"):
            return None
        nonce = unquote(parts[2] or "")
        view_id = unquote(parts[3] or "")
        host = unquote(parts[4] or "")
        if not nonce or nonce != self._nonce or not view_id or not host:
            return None
        marker = object()
        with self._lock:
            roots = self._views.get(view_id, marker)
        if roots is marker:
            return None
        original_path = "/" + "/".join(parts[5:])
        original_url = urlunsplit((
            "https",
            host,
            original_path,
            parsed.query,
            "",
        ))
        fs_path = AIEditorAPI._webview_local_path_from_url(original_url)
        if not fs_path:
            return None
        if not AIEditorAPI._webview_path_allowed(fs_path, roots):
            return None
        if not os.path.isfile(fs_path):
            return None
        return fs_path, AIEditorAPI._webview_mime_for_path(fs_path)

    def stop(self) -> None:
        try:
            self._server.shutdown()
        except Exception:
            pass
        try:
            self._server.server_close()
        except Exception:
            pass


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

    def quick_input_changed(self, payload: Dict[str, Any]) -> None:
        """Forward extension-created QuickInput state to the frontend."""
        self._api._emit("quick_input", dict(payload or {}))

    # -- Status bar --
    def show_status_bar_item(self, item_id: str, text: str,
                             tooltip: str, command: str,
                             alignment: int = 2, priority: int = 0,
                             color: str = "",
                             backgroundColor: str = "") -> None:
        self._api._emit("show_status_bar_item", {
            "id": item_id, "text": text, "tooltip": tooltip,
            "command": command, "alignment": alignment,
            "priority": priority, "color": color,
            "backgroundColor": backgroundColor})

    def hide_status_bar_item(self, item_id: str) -> None:
        self._api._emit("hide_status_bar_item", {"id": item_id})

    def dispose_status_bar_item(self, item_id: str) -> None:
        self._api._emit("dispose_status_bar_item", {"id": item_id})

    # -- Webview panels --
    def render_webview_panel(
            self, view_id: str, html: str,
            local_resource_roots: Any = None) -> None:
        """Push HTML content for a webview panel to the frontend."""
        prepared = self._api._prepare_extension_webview_html(
            html, local_resource_roots, view_id=view_id)
        self._api._emit("render_webview_panel", {
            "view_id": view_id,
            "html": prepared,
        })

    def dispose_webview_panel(self, view_id: str) -> None:
        """Dispose a webview panel by emitting a dispose event to the frontend."""
        self._api._unregister_webview_resource_view(view_id)
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

    def custom_editor_changed(self, state: Dict[str, Any]) -> None:
        """Notify the frontend that a Node custom editor dirty state changed."""
        self._api._emit("custom_editor_changed", state)

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

    _webview_resource_server: Optional[_WebviewResourceServer] = None
    _webview_resource_server_lock = threading.Lock()

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

    @classmethod
    def _ensure_webview_resource_server(cls) -> _WebviewResourceServer:
        with cls._webview_resource_server_lock:
            server = cls._webview_resource_server
            if server is None:
                server = _WebviewResourceServer()
                cls._webview_resource_server = server
            return server

    @classmethod
    def _stop_webview_resource_server(cls) -> None:
        with cls._webview_resource_server_lock:
            server = cls._webview_resource_server
            cls._webview_resource_server = None
        if server is not None:
            server.stop()

    @staticmethod
    def _webview_local_path_from_url(url: str) -> str:
        parsed = urlparse(str(url or ""))
        if parsed.scheme != "https":
            return ""
        netloc = parsed.netloc.lower()
        if netloc != "webview.local":
            suffix = ".vscode-resource.webview.local"
            if not netloc.endswith(suffix):
                return ""
            resource_prefix = unquote(netloc[:-len(suffix)])
            if not resource_prefix.startswith("file+"):
                return ""
        path = unquote(parsed.path or "")
        if re.match(r"^/[A-Za-z]:/", path):
            path = path[1:]
        return os.path.abspath(path) if path else ""

    @staticmethod
    def _webview_mime_for_path(path: str) -> str:
        ext = os.path.splitext(path)[1].lower()
        if ext in {".js", ".mjs", ".cjs"}:
            return "text/javascript"
        if ext == ".css":
            return "text/css"
        return mimetypes.guess_type(path)[0] or "application/octet-stream"

    @staticmethod
    def _webview_should_endpoint_resource(path: str) -> bool:
        return os.path.splitext(path)[1].lower() in {".js", ".mjs", ".cjs"}

    @staticmethod
    def _webview_resource_root_path(value: Any) -> str:
        raw: Any = value
        if isinstance(value, dict):
            raw = (
                value.get("fsPath")
                or value.get("fs_path")
                or value.get("uri")
                or value.get("path")
            )
        text = str(raw or "").strip()
        if not text:
            return ""
        parsed = urlparse(text)
        if parsed.scheme == "file":
            path = unquote(parsed.path or "")
            if re.match(r"^/[A-Za-z]:/", path):
                path = path[1:]
            return os.path.realpath(os.path.abspath(path)) if path else ""
        if re.match(r"^[A-Za-z]:[\\/]", text) or text.startswith(("/", "\\")):
            return os.path.realpath(os.path.abspath(text))
        return ""

    @staticmethod
    def _webview_resource_roots(value: Any) -> Optional[List[str]]:
        if value is None:
            return None
        raw_items = value if isinstance(value, list) else [value]
        roots = []
        for item in raw_items:
            path = AIEditorAPI._webview_resource_root_path(item)
            if path:
                roots.append(path)
        return roots

    @staticmethod
    def _webview_path_allowed(path: str, roots: Optional[List[str]]) -> bool:
        if roots is None:
            return True
        if not roots:
            return False
        try:
            target = os.path.normcase(os.path.realpath(os.path.abspath(path)))
            for root in roots:
                root_norm = os.path.normcase(os.path.realpath(os.path.abspath(root)))
                if os.path.commonpath([root_norm, target]) == root_norm:
                    return True
        except (OSError, ValueError):
            return False
        return False

    def _register_webview_resource_view(
            self, view_id: str,
            local_resource_roots: Any = None) -> tuple[str, str]:
        key = str(view_id or "").strip()
        if not key:
            return "", ""
        roots = self._webview_resource_roots(local_resource_roots)
        server = self._ensure_webview_resource_server()
        return server.register_view(key, roots), server.origin

    def _unregister_webview_resource_view(self, view_id: str) -> None:
        key = str(view_id or "").strip()
        if not key:
            return
        server = self.__class__._webview_resource_server
        if server is not None:
            server.unregister_view(key)

    def _prepare_extension_webview_html(
            self, html: str, local_resource_roots: Any = None,
            view_id: str = "") -> str:
        """Inline local ``asWebviewUri`` resources for srcdoc webviews.

        Node-side extensions naturally emit local HTTPS resource URLs from
        ``webview.asWebviewUri``.  The AI Editor embeds webviews as sandboxed
        ``srcdoc`` iframes instead of running a local webview resource server,
        so readable local assets are converted to data URIs and larger or
        runtime-only assets are rewritten through a scoped local endpoint.
        """
        text = str(html or "")
        budget = {"bytes": 0}
        allowed_roots = self._webview_resource_roots(local_resource_roots)
        endpoint_base = ""
        endpoint_origin = ""
        view_key = str(view_id or "").strip()
        if view_key:
            endpoint_base, endpoint_origin = self._register_webview_resource_view(
                view_key, local_resource_roots)
        if "webview.local/" not in text and not endpoint_base:
            return text
        resource_map: Dict[str, str] = {}

        def file_to_data_uri(path: str) -> str:
            full = os.path.abspath(path)
            try:
                if not self._webview_path_allowed(full, allowed_roots):
                    return ""
                if not os.path.isfile(full):
                    return ""
                size = os.path.getsize(full)
                if (size > _WEBVIEW_RESOURCE_MAX_BYTES or
                        budget["bytes"] + size > _WEBVIEW_RESOURCE_TOTAL_MAX_BYTES):
                    return ""
                budget["bytes"] += size
                with open(full, "rb") as fh:
                    data = fh.read()
            except Exception:
                return ""
            mime = self._webview_mime_for_path(full)
            if mime == "text/css":
                try:
                    css = data.decode("utf-8", errors="replace")
                    css_dir = os.path.dirname(full)

                    def css_url_repl(match: re.Match) -> str:
                        quote = match.group(1) or ""
                        raw_ref = (match.group(2) or "").strip()
                        lowered = raw_ref.lower()
                        if (not raw_ref or lowered.startswith(("data:", "http:",
                                "https:", "//")) or raw_ref.startswith("#")):
                            return match.group(0)
                        ref_path = os.path.abspath(
                            os.path.join(css_dir, unquote(raw_ref)))
                        nested = file_to_data_uri(ref_path)
                        return f"url({quote}{nested}{quote})" if nested else match.group(0)

                    css = re.sub(
                        r"url\(\s*(['\"]?)([^'\")]+)\1\s*\)",
                        css_url_repl,
                        css,
                    )
                    data = css.encode("utf-8")
                except Exception:
                    pass
            encoded = base64.b64encode(data).decode("ascii")
            return f"data:{mime};base64,{encoded}"

        def local_url_repl(match: re.Match) -> str:
            url = match.group(0)
            path = self._webview_local_path_from_url(url)
            if not path or not self._webview_path_allowed(path, allowed_roots):
                return url
            if endpoint_base and self._webview_should_endpoint_resource(path):
                server = self.__class__._webview_resource_server
                if server is not None:
                    return server.resource_url(view_key, url)
            data_uri = file_to_data_uri(path)
            if data_uri:
                resource_map[url] = data_uri
                return data_uri
            if endpoint_base:
                server = self.__class__._webview_resource_server
                if server is not None:
                    return server.resource_url(view_key, url)
            return url

        prepared = _WEBVIEW_LOCAL_URL_RE.sub(local_url_repl, text)
        prefix_parts: List[str] = []
        if resource_map:
            map_json = json.dumps(resource_map, ensure_ascii=False).replace(
                "<", "\\u003c")
            prefix_parts.append(
                '<script id="sao-webview-resource-map" '
                'type="application/json">'
                f"{map_json}</script>"
            )
        if endpoint_base:
            endpoint_json = json.dumps(
                {"base": endpoint_base}, ensure_ascii=False).replace(
                    "<", "\\u003c")
            prefix_parts.append(
                '<script id="sao-webview-resource-endpoint" '
                'type="application/json">'
                f"{endpoint_json}</script>"
            )
        if prefix_parts:
            prepared = "".join(prefix_parts) + prepared
        return self._relax_webview_csp_for_data_uris(
            prepared, endpoint_origin=endpoint_origin)

    @staticmethod
    def _relax_webview_csp_for_data_uris(
            html: str, endpoint_origin: str = "") -> str:
        def csp_repl(match: re.Match) -> str:
            content = match.group(3)
            additions = {
                "font-src": ["data:"],
                "img-src": ["data:", "blob:"],
                "script-src": ["data:"],
                "style-src": ["data:", "'unsafe-inline'"],
            }
            if endpoint_origin:
                for directive in (
                        "child-src", "connect-src", "default-src",
                        "font-src", "img-src", "media-src",
                        "script-src", "style-src", "worker-src"):
                    additions.setdefault(directive, [])
                    if endpoint_origin not in additions[directive]:
                        additions[directive].append(endpoint_origin)
            rebuilt = []
            seen = set()
            for directive in content.split(";"):
                parts = directive.strip().split()
                if not parts:
                    continue
                name = parts[0].lower()
                seen.add(name)
                for token in additions.get(name, []):
                    if token not in parts:
                        parts.append(token)
                rebuilt.append(" ".join(parts))
            for name, tokens in additions.items():
                if name not in seen:
                    rebuilt.append(" ".join([name, *tokens]))
            return (
                f"{match.group(1)}{match.group(2)}"
                f"{'; '.join(rebuilt)}{match.group(4)}"
            )

        return re.sub(
            r"(<meta\b(?=[^>]*http-equiv=[\"']Content-Security-Policy[\"'])(?=[^>]*content=)[^>]*\bcontent=)([\"'])(.*?)(\2[^>]*>)",
            csp_repl,
            html,
            flags=re.IGNORECASE,
        )

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
        self._node_tree_disposables: Dict[str, Any] = {}
        self._node_lm_tool_disposables: Dict[str, Any] = {}
        self._node_chat_participant_disposables: Dict[str, Any] = {}
        self._extensions_inited = False
        self._vscode_ns_ready = threading.Event()

        from ai_editor.vscode_api import VscodeNamespace
        self._vscode_ns = VscodeNamespace(
            self._ext_host, self._engine, self._settings_getter)
        self._vscode_ns.set_ui_bridge(_AIEditorUIBridge(self))
        self._vscode_ns.set_tree_view_change_callback(
            self._on_tree_view_changed)
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
            "color_theme": ai_cfg.get("color_theme", ""),
            "file_icon_theme": ai_cfg.get("file_icon_theme", ""),
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

    def list_editor_languages(self) -> Dict:
        """Return built-in and extension-contributed editor languages.

        VS Code language extensions declare ``contributes.languages`` in their
        manifest. The lightweight editor keeps using its existing textarea
        surface, but it should still honor those dynamic declarations for
        language mode selection and file-extension detection.
        """
        self._ensure_engine()
        languages = self._editor_language_entries()
        return {
            "languages": languages,
            "extensionCount": sum(
                1 for item in languages if item.get("source") == "extension"),
            "grammarCount": sum(
                len(item.get("grammars") or []) for item in languages),
        }

    def list_editor_grammars(self) -> Dict:
        """Return VS Code TextMate grammar metadata contributed by extensions."""
        self._ensure_engine()
        grammars = self._editor_grammar_entries()
        return {
            "grammars": grammars,
            "count": len(grammars),
            "languages": sorted({
                str(item.get("language") or "")
                for item in grammars if item.get("language")
            }),
        }

    def list_editor_themes(self) -> Dict:
        """Return VS Code color/icon theme metadata contributed by extensions."""
        self._ensure_engine()
        themes = self._editor_theme_entries()
        return {
            "themes": themes,
            "count": len(themes),
            "colorThemes": sum(
                1 for item in themes if item.get("themeType") == "color"),
            "iconThemes": sum(
                1 for item in themes if item.get("themeType") != "color"),
        }

    def get_editor_theme(self, theme_id: str) -> Dict:
        """Return safe color data for one extension-contributed color theme."""
        self._ensure_engine()
        requested = str(theme_id or "").strip()
        if not requested:
            return {"error": "theme id is required"}
        theme = next((
            item for item in self._editor_theme_entries()
            if item.get("themeType") == "color"
            and requested in {
                str(item.get("id") or ""),
                str(item.get("label") or ""),
            }
        ), None)
        if not theme:
            return {"error": f"Color theme not found: {requested}"}
        path = str(theme.get("resolvedPath") or "")
        if not path or not os.path.isabs(path) or not os.path.exists(path):
            return {"error": "Theme file is not available", "theme": theme}
        try:
            payload = _load_jsonc_file(path)
        except Exception as exc:
            return {"error": f"Failed to read theme JSON: {exc}", "theme": theme}
        if not isinstance(payload, dict):
            return {"error": "Theme JSON must be an object", "theme": theme}
        colors = payload.get("colors") or {}
        if not isinstance(colors, dict):
            colors = {}
        safe_colors = {
            str(key): str(value)
            for key, value in colors.items()
            if isinstance(key, str) and isinstance(value, str)
        }
        semantic_token_colors = payload.get("semanticTokenColors") or {}
        safe_semantic_token_colors: Dict[str, Any] = {}
        if isinstance(semantic_token_colors, dict):
            for key, value in semantic_token_colors.items():
                selector = str(key or "").strip()
                if not selector or len(selector) > 160:
                    continue
                if isinstance(value, str):
                    safe_semantic_token_colors[selector] = value.strip()
                    continue
                if not isinstance(value, dict):
                    continue
                rule: Dict[str, Any] = {}
                foreground = value.get("foreground") or value.get("color")
                font_style = value.get("fontStyle")
                if isinstance(foreground, str):
                    rule["foreground"] = foreground.strip()
                if isinstance(font_style, str):
                    rule["fontStyle"] = font_style.strip()
                for attr in (
                        "bold", "italic", "underline",
                        "strikethrough"):
                    if isinstance(value.get(attr), bool):
                        rule[attr] = bool(value.get(attr))
                if rule:
                    safe_semantic_token_colors[selector] = rule
        return {
            "ok": True,
            "theme": theme,
            "colors": safe_colors,
            "semanticHighlighting": payload.get("semanticHighlighting"),
            "semanticTokenColors": safe_semantic_token_colors,
            "name": str(payload.get("name") or theme.get("label") or ""),
            "type": str(payload.get("type") or theme.get("uiTheme") or ""),
        }

    def get_editor_icon_theme(self, theme_id: str) -> Dict:
        """Return safe file icon theme data for extension-contributed icons."""
        self._ensure_engine()
        requested = str(theme_id or "").strip()
        if not requested:
            return {"error": "icon theme id is required"}
        theme = next((
            item for item in self._editor_theme_entries()
            if item.get("themeType") != "color"
            and requested in {
                str(item.get("id") or ""),
                str(item.get("label") or ""),
            }
        ), None)
        if not theme:
            return {"error": f"Icon theme not found: {requested}"}
        path = str(theme.get("resolvedPath") or "")
        if not path or not os.path.isabs(path) or not os.path.exists(path):
            return {"error": "Icon theme file is not available", "theme": theme}
        try:
            payload = _load_jsonc_file(path)
        except Exception as exc:
            return {"error": f"Failed to read icon theme JSON: {exc}", "theme": theme}
        if not isinstance(payload, dict):
            return {"error": "Icon theme JSON must be an object", "theme": theme}
        icon_definitions = self._safe_icon_theme_definitions(
            payload.get("iconDefinitions"), theme, path)
        return {
            "ok": True,
            "theme": theme,
            "name": str(payload.get("name") or theme.get("label") or ""),
            "hidesExplorerArrows": bool(payload.get("hidesExplorerArrows")),
            "showLanguageModeIcons": bool(payload.get("showLanguageModeIcons")),
            "iconDefinitions": icon_definitions,
            "fonts": self._safe_icon_theme_fonts(
                payload.get("fonts"), theme, path),
            "file": self._safe_icon_theme_id(payload.get("file")),
            "folder": self._safe_icon_theme_id(payload.get("folder")),
            "folderExpanded": self._safe_icon_theme_id(
                payload.get("folderExpanded")),
            "rootFolder": self._safe_icon_theme_id(payload.get("rootFolder")),
            "rootFolderExpanded": self._safe_icon_theme_id(
                payload.get("rootFolderExpanded")),
            "fileExtensions": self._safe_icon_theme_map(
                payload.get("fileExtensions")),
            "fileNames": self._safe_icon_theme_map(payload.get("fileNames")),
            "folderNames": self._safe_icon_theme_map(
                payload.get("folderNames")),
            "folderNamesExpanded": self._safe_icon_theme_map(
                payload.get("folderNamesExpanded")),
            "languageIds": self._safe_icon_theme_map(payload.get("languageIds")),
        }

    @staticmethod
    def _safe_icon_theme_id(value: Any) -> str:
        return str(value or "").strip() if value else ""

    @classmethod
    def _safe_icon_theme_map(cls, value: Any) -> Dict[str, str]:
        if not isinstance(value, dict):
            return {}
        return {
            str(key).strip().casefold(): cls._safe_icon_theme_id(icon_id)
            for key, icon_id in value.items()
            if str(key or "").strip() and cls._safe_icon_theme_id(icon_id)
        }

    def _safe_icon_theme_definitions(
            self, value: Any, theme: Dict,
            theme_path: str) -> Dict[str, Dict[str, str]]:
        if not isinstance(value, dict):
            return {}
        result: Dict[str, Dict[str, str]] = {}
        for icon_id, raw in value.items():
            normalized_id = self._safe_icon_theme_id(icon_id)
            if not normalized_id or not isinstance(raw, dict):
                continue
            item: Dict[str, str] = {}
            for key in ("fontCharacter", "fontColor"):
                raw_value = raw.get(key)
                if isinstance(raw_value, str) and raw_value.strip():
                    item[key] = raw_value.strip()
            raw_icon_path = raw.get("iconPath")
            if isinstance(raw_icon_path, str) and raw_icon_path.strip():
                item["iconPath"] = raw_icon_path.strip()
                icon_uri = self._safe_icon_theme_asset_uri(
                    theme, theme_path, raw_icon_path, {".svg", ".png"})
                if icon_uri:
                    item["iconUri"] = icon_uri
            for key in ("fontId", "fontSize"):
                raw_value = raw.get(key)
                if isinstance(raw_value, str) and raw_value.strip():
                    item[key] = raw_value.strip()
            if item:
                result[normalized_id] = item
        return result

    def _safe_icon_theme_asset_uri(
            self, theme: Dict, theme_path: str, raw_path: str,
            allowed_extensions: set) -> str:
        raw = str(raw_path or "").strip()
        if not raw or re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", raw):
            return ""
        asset_ref = raw.split("?", 1)[0].split("#", 1)[0]
        ext = os.path.splitext(asset_ref)[1].lower()
        if ext not in allowed_extensions:
            return ""
        theme_dir = os.path.dirname(os.path.abspath(theme_path))
        full = os.path.abspath(os.path.join(theme_dir, asset_ref))
        try:
            extension_id = str(theme.get("extension_id") or "")
            ext_desc = self._ext_host.registry.get(extension_id)
            root = getattr(ext_desc, "extension_path", "") if ext_desc else ""
            root_real = os.path.realpath(os.path.abspath(root or theme_dir))
            full_real = os.path.realpath(full)
            if os.path.commonpath([
                    os.path.normcase(root_real),
                    os.path.normcase(full_real)]) != os.path.normcase(root_real):
                return ""
        except Exception:
            return ""
        if not os.path.exists(full):
            return ""
        try:
            return Path(full).resolve().as_uri()
        except Exception:
            return ""

    def _safe_icon_theme_fonts(
            self, value: Any, theme: Dict,
            theme_path: str) -> List[Dict[str, Any]]:
        if not isinstance(value, list):
            return []
        formats = {
            "woff", "woff2", "truetype", "opentype",
            "embedded-opentype", "svg",
        }
        result: List[Dict[str, Any]] = []
        for raw_font in value:
            if not isinstance(raw_font, dict):
                continue
            font_id = str(raw_font.get("id") or "").strip()
            if not font_id or len(font_id) > 80:
                continue
            raw_src = raw_font.get("src")
            if not isinstance(raw_src, list):
                continue
            src_items: List[Dict[str, str]] = []
            for raw_item in raw_src:
                if not isinstance(raw_item, dict):
                    continue
                font_format = str(raw_item.get("format") or "").strip().lower()
                if font_format not in formats:
                    continue
                uri = self._safe_icon_theme_asset_uri(
                    theme, theme_path, str(raw_item.get("path") or ""),
                    {".woff", ".woff2", ".ttf", ".otf", ".eot", ".svg"})
                if uri:
                    src_items.append({"uri": uri, "format": font_format})
            if not src_items:
                continue
            item: Dict[str, Any] = {"id": font_id, "src": src_items}
            for key in ("weight", "style", "size"):
                raw_value = raw_font.get(key)
                if isinstance(raw_value, str) and raw_value.strip():
                    item[key] = raw_value.strip()
            result.append(item)
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

    def _extension_diagnostics_enabled(self) -> bool:
        settings = _resolve_settings(self._gui_ref)
        ai_cfg = {}
        if settings:
            ai_cfg = _normalize_ai_editor_config(
                settings.get("ai_editor", {}) or {})
        ext_cfg = ai_cfg.get("extensions", {}) if isinstance(ai_cfg, dict) else {}
        return bool(
            isinstance(ext_cfg, dict)
            and ext_cfg.get("diagnostics_enabled") is True)

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
        if delivered:
            return {"ok": True, "view_id": view_id}
        node_host = getattr(self, "_node_ext_host", None)
        if node_host is not None and getattr(node_host, "is_running", False):
            try:
                if node_host.relay_webview_message(view_id, message):
                    return {
                        "ok": True,
                        "view_id": view_id,
                        "transport": "node_ext_host",
                    }
            except Exception:
                pass
        return {"error": f"No webview receiver found for: {view_id}"}

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
        if not activate_view_id:
            for mv in self._all_manifest_views(webview_only=True):
                if str(mv.get("id") or "") == provider_id:
                    activate_view_id = provider_id
                    break
        if activate_view_id:
            self._activate_provider_view(activate_view_id)
            self._sync_dynamic_webview_providers()
            runtime_info = self._provider_runtime_webview_info(provider_id)
        vscode_ns = getattr(self, "_vscode_ns", None)
        runtime_view_id = str(
            runtime_info.get("runtime_view_id") or activate_view_id or "")
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

    def save_file_dialog(self, suggested_name: str = "") -> Dict:
        """Show a native Save-As dialog and return the chosen target path."""
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
            return {
                "ok": True,
                "path": path,
                "name": os.path.basename(path),
                "language": self._editor_language_for_path(path),
            }
        except Exception as exc:
            return {"error": str(exc)}

    def save_file_as(self, content: str, suggested_name: str = "") -> Dict:
        """Show a native Save-As dialog, write *content* to the chosen path."""
        target = self.save_file_dialog(suggested_name)
        if not target.get("ok"):
            return target
        try:
            path = target.get("path", "")
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(content)
            return {
                "ok": True,
                "path": path,
                "name": target.get("name") or os.path.basename(path),
                "language": target.get("language") or self._editor_language_for_path(path),
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

    @staticmethod
    def _normalize_workspace_contains_pattern(pattern: str) -> str:
        text = str(pattern or "").strip().replace("\\", "/")
        while text.startswith("./"):
            text = text[2:]
        return text.lstrip("/")

    @staticmethod
    def _workspace_contains_pattern_variants(pattern: str) -> List[str]:
        variants = [pattern]
        for _ in range(4):
            next_variants: List[str] = []
            expanded = False
            for item in variants:
                match = re.search(r"\{([^{}]+)\}", item)
                if not match:
                    next_variants.append(item)
                    continue
                expanded = True
                for raw_part in match.group(1).split(","):
                    part = raw_part.strip()
                    if not part:
                        continue
                    next_variants.append(
                        item[:match.start()] + part + item[match.end():])
                    if len(next_variants) >= 20:
                        break
                if len(next_variants) >= 20:
                    break
            variants = list(dict.fromkeys(next_variants))
            if not expanded:
                break
        return variants

    @staticmethod
    def _workspace_contains_is_glob(pattern: str) -> bool:
        return any(ch in pattern for ch in "*?[")

    @staticmethod
    def _workspace_contains_rel_matches(rel_path: str, pattern: str) -> bool:
        rel = rel_path.replace("\\", "/")
        if fnmatch.fnmatch(rel, pattern):
            return True
        if pattern.startswith("**/"):
            tail = pattern[3:]
            if rel == tail or fnmatch.fnmatch(rel, tail):
                return True
        if ("/" not in pattern
                and fnmatch.fnmatch(os.path.basename(rel), pattern)):
            return True
        return False

    def _workspace_contains_match_map(
            self, root: str, patterns: List[str]) -> Dict[str, bool]:
        result = {str(pattern or ""): False for pattern in patterns}
        root_abs = os.path.abspath(root or "")
        if not root_abs or not os.path.isdir(root_abs):
            return result

        glob_entries: List[tuple[str, str]] = []
        for raw_pattern in result:
            normalized = self._normalize_workspace_contains_pattern(raw_pattern)
            if not normalized:
                continue
            variants = self._workspace_contains_pattern_variants(normalized)
            for item in variants:
                if self._workspace_contains_is_glob(item):
                    glob_entries.append((raw_pattern, item))
                    continue
                target = os.path.abspath(
                    os.path.join(root_abs, *item.split("/")))
                if (self._is_workspace_safe_path(root_abs, target)
                        and os.path.exists(target)):
                    result[raw_pattern] = True
                    break

        glob_entries = [
            item for item in glob_entries
            if not result.get(item[0], False)
        ]
        if not glob_entries:
            return result

        checked = 0
        deadline = time.monotonic() + _WORKSPACE_CONTAINS_MAX_SECONDS
        root_real = os.path.realpath(root_abs)
        remaining = {raw_pattern for raw_pattern, _ in glob_entries}
        for current, dirs, files in os.walk(root_real):
            dirs[:] = [
                name for name in dirs
                if name not in _WORKSPACE_TREE_IGNORED_DIRS
            ]
            for name in files:
                checked += 1
                if (checked > _WORKSPACE_CONTAINS_MAX_FILES
                        or time.monotonic() > deadline):
                    return result
                full = os.path.join(current, name)
                try:
                    rel = os.path.relpath(full, root_real).replace("\\", "/")
                except ValueError:
                    continue
                for raw_pattern, glob_pattern in glob_entries:
                    if raw_pattern not in remaining:
                        continue
                    if self._workspace_contains_rel_matches(rel, glob_pattern):
                        result[raw_pattern] = True
                        remaining.discard(raw_pattern)
                if not remaining:
                    return result
        return result

    def _workspace_contains_matches(self, root: str, pattern: str) -> bool:
        return self._workspace_contains_match_map(
            root, [pattern]).get(str(pattern or ""), False)

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

    def _workspace_path_for_edit_uri(self, uri_value: Any) -> str:
        root = self._workspace_root()
        raw = uri_value
        if isinstance(raw, Uri):
            raw = raw.fs_path if raw.scheme == "file" else str(raw)
        elif isinstance(raw, dict):
            if raw.get("scheme") == "file":
                raw = Uri(
                    "file",
                    str(raw.get("path") or ""),
                    str(raw.get("authority") or ""),
                    str(raw.get("query") or ""),
                    str(raw.get("fragment") or ""),
                ).fs_path
            else:
                raw = raw.get("uri") or raw.get("path") or ""
        raw_text = str(raw or "").strip()
        if raw_text.startswith("file:"):
            parsed = Uri.parse(raw_text)
            if parsed.scheme != "file":
                raise ValueError("Only file workspace edits are supported")
            full = os.path.abspath(parsed.fs_path)
        elif os.path.isabs(raw_text) or (
                len(raw_text) >= 2 and raw_text[1] == ":"
                and raw_text[0].isalpha()):
            full = os.path.abspath(raw_text)
        else:
            full = self._resolve_workspace_path(raw_text)
        if not self._is_workspace_safe_path(root, full):
            raise ValueError("Path escapes workspace")
        return full

    def apply_workspace_text_edits(self, edits: List[Dict[str, Any]]) -> Dict:
        """Apply text edits to existing files under the active workspace."""
        if not isinstance(edits, list):
            return {"error": "Workspace edits must be a list"}
        grouped: Dict[str, List[Dict[str, Any]]] = {}
        skipped: List[Dict[str, Any]] = []
        root = self._workspace_root()
        for index, edit in enumerate(edits):
            if not isinstance(edit, dict):
                skipped.append({"index": index, "reason": "Edit must be an object"})
                continue
            if not edit.get("range") and edit.get("position"):
                edit = dict(edit)
                edit["range"] = {
                    "start": edit.get("position"),
                    "end": edit.get("position"),
                }
            if not edit.get("range"):
                skipped.append({"index": index, "reason": "Missing edit range"})
                continue
            try:
                full = self._workspace_path_for_edit_uri(
                    edit.get("uri") or edit.get("targetUri") or edit.get("path"))
            except Exception as exc:
                skipped.append({"index": index, "reason": str(exc)})
                continue
            if not os.path.isfile(full):
                skipped.append({
                    "index": index,
                    "path": self._workspace_rel_path(root, full),
                    "reason": "File not found",
                })
                continue
            grouped.setdefault(full, []).append(edit)

        applied: List[Dict[str, Any]] = []
        errors: List[Dict[str, Any]] = []
        for full, items in grouped.items():
            try:
                with open(full, "r", encoding="utf-8") as fh:
                    text = fh.read()
                ops: List[Dict[str, Any]] = []
                for edit in items:
                    rng = edit.get("range") or {}
                    start = _offset_for_text_position(text, rng.get("start"))
                    end = _offset_for_text_position(text, rng.get("end"))
                    ops.append({
                        "start": min(start, end),
                        "end": max(start, end),
                        "text": str(edit.get("newText", edit.get("text", ""))),
                    })
                next_text = text
                for op in sorted(ops, key=lambda item: (
                        item["start"], item["end"]), reverse=True):
                    next_text = (
                        next_text[:op["start"]]
                        + op["text"]
                        + next_text[op["end"]:]
                    )
                with open(full, "w", encoding="utf-8", newline="") as fh:
                    fh.write(next_text)
                applied.append({
                    "path": self._workspace_rel_path(root, full),
                    "absolute_path": full,
                    "edits": len(items),
                })
            except Exception as exc:
                errors.append({
                    "path": self._workspace_rel_path(root, full),
                    "error": str(exc),
                })
        return {
            "ok": not errors,
            "applied": applied,
            "skipped": skipped,
            "errors": errors,
        }

    def _editor_language_entries(self) -> List[Dict[str, Any]]:
        rows: Dict[str, Dict[str, Any]] = {}

        def _entry(language_id: str, source: str = "builtin") -> Dict[str, Any]:
            item = rows.setdefault(language_id, {
                "id": language_id,
                "name": _EDITOR_LANGUAGE_DISPLAY_NAMES.get(
                    language_id, language_id.replace("-", " ").title()),
                "aliases": [],
                "extensions": [],
                "filenames": [],
                "grammars": [],
                "grammarScopes": [],
                "tokenizer": "",
                "source": source,
                "extension_id": "",
                "configurationPath": "",
                "configurationResolvedPath": "",
                "configuration": {},
            })
            if item.get("source") != "builtin" and source == "builtin":
                item["source"] = "builtin"
            return item

        for language_id in ["plaintext", *_EDITOR_LANGUAGE_DISPLAY_NAMES.keys()]:
            _entry(language_id)
        for extension, language_id in _EDITOR_LANGUAGE_BY_EXT.items():
            item = _entry(language_id)
            if extension not in item["extensions"]:
                item["extensions"].append(extension)

        try:
            contributed = self._ext_host.ext_points.all_contributions.get(
                "languages", [])
        except Exception:
            contributed = []
        for lang in contributed:
            if not isinstance(lang, dict):
                continue
            language_id = str(lang.get("id") or "").strip()
            if not language_id:
                continue
            item = _entry(language_id, "extension")
            if item.get("source") != "builtin":
                item["source"] = "extension"
            extension_id = str(
                lang.get("_extensionId") or item.get("extension_id") or "")
            item["extension_id"] = extension_id
            aliases = [
                str(value).strip()
                for value in (lang.get("aliases") or [])
                if str(value).strip()
            ]
            if aliases:
                item["name"] = aliases[0]
            for alias in aliases:
                if alias not in item["aliases"]:
                    item["aliases"].append(alias)
            for extension in (lang.get("extensions") or []):
                normalized = self._normalize_editor_language_extension(extension)
                if normalized and normalized not in item["extensions"]:
                    item["extensions"].append(normalized)
            for filename in (lang.get("filenames") or []):
                normalized_name = str(filename or "").strip()
                if normalized_name and normalized_name not in item["filenames"]:
                    item["filenames"].append(normalized_name)
            config_path = str(lang.get("configuration") or "").strip()
            if config_path:
                item["configurationPath"] = config_path
                resolved, configuration = self._editor_language_configuration(
                    extension_id, config_path)
                if resolved:
                    item["configurationResolvedPath"] = resolved
                if configuration:
                    item["configuration"] = configuration

        for language_id, grammars in self._editor_grammars_by_language().items():
            if not language_id:
                continue
            item = _entry(language_id, "extension")
            if item.get("source") != "builtin":
                item["source"] = "extension"
            if grammars and not item.get("extension_id"):
                item["extension_id"] = str(
                    grammars[0].get("extension_id") or "")
            item["tokenizer"] = "textmate"
            for grammar in grammars:
                if grammar not in item["grammars"]:
                    item["grammars"].append(grammar)
                scope = str(grammar.get("scopeName") or "").strip()
                if scope and scope not in item["grammarScopes"]:
                    item["grammarScopes"].append(scope)

        return sorted(rows.values(), key=lambda item: (
            0 if item.get("id") == "plaintext" else 1,
            str(item.get("name") or item.get("id") or "").casefold(),
        ))

    def _editor_language_configuration(
            self, extension_id: str, rel_path: str) -> tuple[str, Dict[str, Any]]:
        resolved = self._extension_contribution_path(extension_id, rel_path)
        if not resolved or not os.path.isabs(resolved):
            return "", {}
        if not os.path.exists(resolved):
            return resolved, {}
        try:
            payload = _load_jsonc_file(resolved)
        except Exception:
            return resolved, {}
        return resolved, self._safe_editor_language_configuration(payload)

    @classmethod
    def _safe_editor_language_configuration(
            cls, payload: Any) -> Dict[str, Any]:
        if not isinstance(payload, dict):
            return {}
        result: Dict[str, Any] = {}
        comments = cls._safe_editor_language_comments(payload.get("comments"))
        if comments:
            result["comments"] = comments
        brackets = cls._safe_editor_language_pairs(payload.get("brackets"))
        if brackets:
            result["brackets"] = brackets
        auto_pairs = cls._safe_editor_auto_closing_pairs(
            payload.get("autoClosingPairs"))
        if auto_pairs:
            result["autoClosingPairs"] = auto_pairs
        surrounding_pairs = cls._safe_editor_language_pairs(
            payload.get("surroundingPairs"), allow_empty_close=True)
        if surrounding_pairs:
            result["surroundingPairs"] = surrounding_pairs
        indentation_rules = cls._safe_editor_indentation_rules(
            payload.get("indentationRules"))
        if indentation_rules:
            result["indentationRules"] = indentation_rules
        on_enter_rules = cls._safe_editor_on_enter_rules(
            payload.get("onEnterRules"))
        if on_enter_rules:
            result["onEnterRules"] = on_enter_rules
        word_pattern = cls._safe_editor_regex(payload.get("wordPattern"))
        if word_pattern:
            result["wordPattern"] = word_pattern
        folding = cls._safe_editor_folding(payload.get("folding"))
        if folding:
            result["folding"] = folding
        return result

    @classmethod
    def _safe_editor_language_comments(cls, value: Any) -> Dict[str, Any]:
        if not isinstance(value, dict):
            return {}
        comments: Dict[str, Any] = {}
        line = cls._safe_editor_language_text(
            value.get("lineComment"), strip=True)
        if line:
            comments["lineComment"] = line
        block = cls._safe_editor_language_pair(value.get("blockComment"))
        if block:
            comments["blockComment"] = block
        return comments

    @classmethod
    def _safe_editor_language_pairs(
            cls, value: Any, *,
            allow_empty_close: bool = False) -> List[List[str]]:
        if not isinstance(value, list):
            return []
        pairs: List[List[str]] = []
        for raw in value[:64]:
            pair = cls._safe_editor_language_pair(
                raw, allow_empty_close=allow_empty_close)
            if pair:
                pairs.append(pair)
        return pairs

    @classmethod
    def _safe_editor_auto_closing_pairs(
            cls, value: Any) -> List[Dict[str, Any]]:
        if not isinstance(value, list):
            return []
        pairs: List[Dict[str, Any]] = []
        for raw in value[:64]:
            open_token = ""
            close_token = ""
            not_in: List[str] = []
            if isinstance(raw, dict):
                open_token = cls._safe_editor_language_text(raw.get("open"))
                close_token = cls._safe_editor_language_text(
                    raw.get("close"), allow_empty=True)
                not_in = cls._extension_string_list(raw.get("notIn"))[:16]
            elif isinstance(raw, list):
                open_token = cls._safe_editor_language_text(raw[0] if raw else "")
                close_token = cls._safe_editor_language_text(
                    raw[1] if len(raw) > 1 else "", allow_empty=True)
            if not open_token:
                continue
            pairs.append({
                "open": open_token,
                "close": close_token,
                "notIn": not_in,
            })
        return pairs

    @classmethod
    def _safe_editor_language_pair(
            cls, value: Any, *,
            allow_empty_close: bool = False) -> List[str]:
        if not isinstance(value, list) or len(value) < 2:
            return []
        open_token = cls._safe_editor_language_text(value[0])
        close_token = cls._safe_editor_language_text(
            value[1], allow_empty=allow_empty_close)
        if not open_token or (not close_token and not allow_empty_close):
            return []
        return [open_token, close_token]

    @classmethod
    def _safe_editor_indentation_rules(cls, value: Any) -> Dict[str, Any]:
        if not isinstance(value, dict):
            return {}
        result: Dict[str, Any] = {}
        for key in (
                "increaseIndentPattern", "decreaseIndentPattern",
                "indentNextLinePattern", "unIndentedLinePattern"):
            pattern = cls._safe_editor_regex(value.get(key))
            if pattern:
                result[key] = pattern
        return result

    @classmethod
    def _safe_editor_folding(cls, value: Any) -> Dict[str, Any]:
        if not isinstance(value, dict):
            return {}
        result: Dict[str, Any] = {}
        markers = value.get("markers")
        if isinstance(markers, dict):
            start = cls._safe_editor_regex(markers.get("start"))
            end = cls._safe_editor_regex(markers.get("end"))
            if start and end:
                result["markers"] = {"start": start, "end": end}
        if isinstance(value.get("offSide"), bool):
            result["offSide"] = bool(value.get("offSide"))
        return result

    @classmethod
    def _safe_editor_on_enter_rules(cls, value: Any) -> List[Dict[str, Any]]:
        if not isinstance(value, list):
            return []
        rules: List[Dict[str, Any]] = []
        for raw in value[:64]:
            if not isinstance(raw, dict):
                continue
            action = cls._safe_editor_enter_action(raw.get("action"))
            if not action:
                continue
            rule: Dict[str, Any] = {"action": action}
            has_pattern = False
            for key in ("beforeText", "afterText", "previousLineText"):
                pattern = cls._safe_editor_regex(raw.get(key))
                if pattern:
                    rule[key] = pattern
                    has_pattern = True
            if has_pattern:
                rules.append(rule)
        return rules

    @classmethod
    def _safe_editor_enter_action(cls, value: Any) -> Dict[str, Any]:
        if not isinstance(value, dict):
            return {}
        action: Dict[str, Any] = {}
        indent = str(value.get("indent") or "").strip()
        if indent in {"none", "indent", "indentOutdent", "outdent"}:
            action["indent"] = indent
        append_text = cls._safe_editor_language_text(
            value.get("appendText"), allow_empty=True)
        if append_text:
            action["appendText"] = append_text
        try:
            remove_text = int(value.get("removeText"))
        except (TypeError, ValueError, OverflowError):
            remove_text = 0
        if 0 < remove_text <= 80:
            action["removeText"] = remove_text
        return action

    @classmethod
    def _safe_editor_regex(cls, value: Any) -> Dict[str, str]:
        pattern_value: Any = value
        flags_value = ""
        if isinstance(value, dict):
            pattern_value = value.get("pattern")
            flags_value = value.get("flags", "")
        pattern = cls._safe_editor_regex_pattern(pattern_value)
        if not pattern:
            return {}
        flags = "".join(
            ch for ch in str(flags_value or "") if ch in "dgimsuvy")
        return {"pattern": pattern, "flags": "".join(dict.fromkeys(flags))}

    @staticmethod
    def _safe_editor_regex_pattern(value: Any) -> str:
        if not isinstance(value, str):
            return ""
        if "\r" in value or "\n" in value or len(value) > 1000:
            return ""
        return value

    @staticmethod
    def _safe_editor_language_text(
            value: Any, *, allow_empty: bool = False,
            strip: bool = False) -> str:
        if not isinstance(value, str):
            return ""
        text = value.strip() if strip else value
        if "\r" in text or "\n" in text or len(text) > 80:
            return ""
        if not text and not allow_empty:
            return ""
        return text

    def _editor_grammars_by_language(self) -> Dict[str, List[Dict[str, Any]]]:
        result: Dict[str, List[Dict[str, Any]]] = {}
        for grammar in self._editor_grammar_entries():
            language_id = str(grammar.get("language") or "").strip()
            if language_id:
                result.setdefault(language_id, []).append(grammar)
        return result

    def _editor_grammar_entries(self) -> List[Dict[str, Any]]:
        try:
            raw_grammars = self._ext_host.ext_points.all_contributions.get(
                "grammars", [])
        except Exception:
            raw_grammars = []
        entries: List[Dict[str, Any]] = []
        for raw in raw_grammars:
            if not isinstance(raw, dict):
                continue
            language_id = str(raw.get("language") or "").strip()
            scope_name = str(raw.get("scopeName") or "").strip()
            grammar_path = str(raw.get("path") or "").strip()
            if not language_id and not scope_name:
                continue
            extension_id = str(raw.get("_extensionId") or "")
            embedded_languages = raw.get("embeddedLanguages")
            token_types = raw.get("tokenTypes")
            entries.append({
                "language": language_id,
                "scopeName": scope_name,
                "path": grammar_path,
                "resolvedPath": self._extension_contribution_path(
                    extension_id, grammar_path),
                "extension_id": extension_id,
                "embeddedLanguages": (
                    dict(embedded_languages)
                    if isinstance(embedded_languages, dict) else {}),
                "tokenTypes": (
                    dict(token_types)
                    if isinstance(token_types, dict) else {}),
                "injectTo": self._extension_string_list(raw.get("injectTo")),
                "balancedBracketScopes": self._extension_string_list(
                    raw.get("balancedBracketScopes")),
                "unbalancedBracketScopes": self._extension_string_list(
                    raw.get("unbalancedBracketScopes")),
            })
        return sorted(entries, key=lambda item: (
            str(item.get("language") or "").casefold(),
            str(item.get("scopeName") or "").casefold(),
        ))

    def _editor_theme_entries(self) -> List[Dict[str, Any]]:
        try:
            raw_themes = self._ext_host.ext_points.all_contributions.get(
                "themes", [])
        except Exception:
            raw_themes = []
        entries: List[Dict[str, Any]] = []
        for raw in raw_themes:
            if not isinstance(raw, dict):
                continue
            extension_id = str(raw.get("_extensionId") or "")
            theme_path = str(raw.get("path") or "").strip()
            theme_type = str(raw.get("_themeType") or "").strip()
            if not theme_type:
                theme_type = "color" if raw.get("uiTheme") else "icon"
            label = str(
                raw.get("label") or raw.get("id") or raw.get("uiTheme")
                or os.path.basename(theme_path) or "Theme")
            entries.append({
                "id": str(raw.get("id") or label),
                "label": label,
                "uiTheme": str(raw.get("uiTheme") or ""),
                "themeType": theme_type,
                "path": theme_path,
                "resolvedPath": self._extension_contribution_path(
                    extension_id, theme_path),
                "extension_id": extension_id,
            })
        return sorted(entries, key=lambda item: (
            str(item.get("themeType") or "").casefold(),
            str(item.get("label") or "").casefold(),
        ))

    def _extension_contribution_path(self, extension_id: str,
                                     rel_path: str) -> str:
        rel = str(rel_path or "").strip()
        if not rel:
            return ""
        try:
            ext = self._ext_host.registry.get(str(extension_id or ""))
        except Exception:
            ext = None
        root = getattr(ext, "extension_path", "") if ext else ""
        if not root:
            return rel
        full = os.path.abspath(os.path.join(root, rel))
        try:
            root_real = os.path.realpath(os.path.abspath(root))
            full_real = os.path.realpath(full)
            if os.path.commonpath([
                    os.path.normcase(root_real),
                    os.path.normcase(full_real)]) != os.path.normcase(root_real):
                return ""
        except Exception:
            return ""
        return full

    @staticmethod
    def _extension_string_list(value: Any) -> List[str]:
        if not isinstance(value, list):
            return []
        return [
            str(item).strip()
            for item in value
            if str(item or "").strip()
        ]

    def _editor_language_by_ext(self) -> Dict[str, str]:
        language_by_ext = dict(_EDITOR_LANGUAGE_BY_EXT)
        for item in self._editor_language_entries():
            language_id = str(item.get("id") or "")
            if not language_id:
                continue
            for extension in item.get("extensions") or []:
                normalized = self._normalize_editor_language_extension(extension)
                if normalized:
                    language_by_ext[normalized] = language_id
        return language_by_ext

    def _editor_language_by_filename(self) -> Dict[str, str]:
        language_by_filename: Dict[str, str] = {}
        for item in self._editor_language_entries():
            language_id = str(item.get("id") or "")
            if not language_id:
                continue
            for filename in item.get("filenames") or []:
                normalized = str(filename or "").strip().casefold()
                if normalized:
                    language_by_filename[normalized] = language_id
        return language_by_filename

    @staticmethod
    def _normalize_editor_language_extension(value: Any) -> str:
        extension = str(value or "").strip().lower()
        if not extension:
            return ""
        return extension if extension.startswith(".") else f".{extension}"

    def _editor_language_for_path(self, path: str) -> str:
        filename = os.path.basename(path).casefold()
        if filename:
            by_filename = self._editor_language_by_filename()
            if filename in by_filename:
                return by_filename[filename]
        return self._editor_language_by_ext().get(
            os.path.splitext(path)[1].lower(), "plaintext")

    @staticmethod
    def _expand_custom_editor_glob(pattern: str) -> List[str]:
        """Expand simple VS Code-style brace globs such as ``*.{png,jpg}``."""
        text = str(pattern or "").strip().replace("\\", "/")
        if not text:
            return []
        match = re.search(r"\{([^{}]+)\}", text)
        if not match:
            return [text]
        expanded: List[str] = []
        prefix, suffix = text[:match.start()], text[match.end():]
        for part in match.group(1).split(","):
            expanded.extend(
                AIEditorAPI._expand_custom_editor_glob(
                    f"{prefix}{part.strip()}{suffix}"))
        return expanded

    @staticmethod
    def _custom_editor_glob_matches(pattern: str, rel_path: str,
                                    filename: str) -> bool:
        candidates = [
            rel_path.replace("\\", "/"),
            filename.replace("\\", "/"),
        ]
        for raw_pattern in AIEditorAPI._expand_custom_editor_glob(pattern):
            patterns = [raw_pattern]
            if raw_pattern.startswith("**/"):
                patterns.append(raw_pattern[3:])
            for pat in patterns:
                normalized_pat = pat.casefold()
                for candidate in candidates:
                    if fnmatch.fnmatchcase(
                            candidate.casefold(), normalized_pat):
                        return True
        return False

    def _custom_editor_match_score(self, editor: Dict[str, Any],
                                   full: str, rel: str) -> int:
        selectors = editor.get("selector")
        if not isinstance(selectors, list):
            selectors = [selectors] if isinstance(selectors, dict) else []
        if not selectors:
            return -1
        filename = os.path.basename(full)
        best = -1
        for selector in selectors:
            if not isinstance(selector, dict):
                continue
            scheme = str(selector.get("scheme") or "file").strip().lower()
            if scheme not in {"", "*", "file"}:
                continue
            pattern = selector.get("filenamePattern") or selector.get("pattern")
            if not pattern:
                continue
            for glob_pattern in self._expand_custom_editor_glob(str(pattern)):
                if self._custom_editor_glob_matches(glob_pattern, rel, filename):
                    priority = str(editor.get("priority") or "").casefold()
                    priority_score = {"default": 2000, "option": 1000}.get(
                        priority, 500)
                    specificity = min(len(glob_pattern), 999)
                    best = max(best, priority_score + specificity)
        return best

    def _matching_custom_editor(self, full: str,
                                rel: str) -> Optional[Dict[str, Any]]:
        ext_host = getattr(self, "_ext_host", None)
        if ext_host is None:
            return None
        ext_points = getattr(ext_host, "ext_points", None)
        contributions = getattr(ext_points, "all_contributions", {}) or {}
        custom_editors = contributions.get("customEditors", [])
        if not isinstance(custom_editors, list):
            return None
        matches: List[tuple[int, Dict[str, Any]]] = []
        for editor in custom_editors:
            if not isinstance(editor, dict) or not editor.get("viewType"):
                continue
            score = self._custom_editor_match_score(editor, full, rel)
            if score >= 0:
                matches.append((score, editor))
        if not matches:
            return None
        matches.sort(key=lambda item: item[0], reverse=True)
        return dict(matches[0][1])

    def _open_extension_custom_editor(
            self, full: str, rel: str) -> Optional[Dict[str, Any]]:
        contribution = self._matching_custom_editor(full, rel)
        if not contribution:
            return None
        view_type = str(contribution.get("viewType") or "").strip()
        if not view_type:
            return None
        ext_host = getattr(self, "_ext_host", None)
        if ext_host is not None:
            try:
                ext_host.activate_event(f"onCustomEditor:{view_type}")
            except Exception:
                pass
        ext_id = str(contribution.get("_extensionId") or "").strip()
        node_host = getattr(self, "_node_ext_host", None)
        is_activated = getattr(node_host, "is_extension_activated", None)
        if (ext_id and node_host is not None
                and getattr(node_host, "is_running", False)
                and callable(is_activated)):
            deadline = time.time() + 8.0
            while (time.time() < deadline
                   and not is_activated(ext_id)):
                time.sleep(0.05)
        result = self.resolve_extension_custom_editor(
            view_type, full, title=os.path.basename(full), timeout=10.0)
        if not result.get("ok"):
            return None
        view_id = result.get("viewId") or result.get("view_id") or ""
        if not view_id:
            return None
        return {
            "name": os.path.basename(full),
            "path": rel,
            "absolute_path": full,
            "content": "",
            "language": self._editor_language_for_path(full),
            "truncated": False,
            "runtime_mode": "extension-custom-editor",
            "custom_editor": {
                "view_type": view_type,
                "display_name": (
                    contribution.get("displayName")
                    or contribution.get("display_name")
                    or contribution.get("name")
                    or view_type),
                "priority": contribution.get("priority", ""),
                "extension_id": contribution.get("_extensionId", ""),
                "dirty": bool(result.get("dirty", False)),
                "editable": bool(result.get("editable", False)),
                "text_editor": bool(result.get("textEditor", False)),
                "supports_save": bool(result.get("supportsSave", False)),
                "supports_save_as": bool(result.get("supportsSaveAs", False)),
                "supports_revert": bool(result.get("supportsRevert", False)),
                "supports_backup": bool(result.get("supportsBackup", False)),
            },
            "webview": {
                "view_id": view_id,
                "uri": result.get("uri", ""),
                "html": result.get("html", ""),
            },
            "view_id": view_id,
        }

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
        rel = self._workspace_rel_path(root, full)
        custom_editor = self._open_extension_custom_editor(full, rel)
        if custom_editor:
            return custom_editor
        try:
            with open(full, "rb") as fh:
                data = fh.read(_WORKSPACE_FILE_PREVIEW_BYTES + 1)
        except Exception as exc:
            return {"error": str(exc)}
        truncated = len(data) > _WORKSPACE_FILE_PREVIEW_BYTES
        text = data[:_WORKSPACE_FILE_PREVIEW_BYTES].decode("utf-8", errors="replace")
        return {
            "name": os.path.basename(full),
            "path": rel,
            "absolute_path": full,
            "content": text,
            "language": self._editor_language_for_path(full),
            "truncated": truncated,
        }

    def editor_language_provider(self, payload: Dict[str, Any]) -> Dict:
        """Run VS Code language providers against the live editor buffer."""
        if not isinstance(payload, dict):
            return {"error": "Language provider payload must be an object"}
        kind_aliases = {
            "completion": "completion",
            "completions": "completion",
            "hover": "hover",
            "signatureHelp": "signatureHelp",
            "signature": "signatureHelp",
            "signatures": "signatureHelp",
            "definition": "definition",
            "definitions": "definition",
            "typeDefinition": "typeDefinition",
            "typeDefinitions": "typeDefinition",
            "type_definition": "typeDefinition",
            "declaration": "declaration",
            "declarations": "declaration",
            "implementation": "implementation",
            "implementations": "implementation",
            "reference": "references",
            "references": "references",
            "documentHighlight": "documentHighlight",
            "documentHighlights": "documentHighlight",
            "highlights": "documentHighlight",
            "prepareRename": "prepareRename",
            "prepare_rename": "prepareRename",
            "rename": "rename",
            "documentLink": "documentLink",
            "documentLinks": "documentLink",
            "links": "documentLink",
            "inlayHint": "inlayHint",
            "inlayHints": "inlayHint",
            "hints": "inlayHint",
            "inlineCompletion": "inlineCompletion",
            "inlineCompletions": "inlineCompletion",
            "ghostText": "inlineCompletion",
            "codeLens": "codeLens",
            "codeLenses": "codeLens",
            "lens": "codeLens",
            "lenses": "codeLens",
            "foldingRange": "foldingRange",
            "foldingRanges": "foldingRange",
            "folds": "foldingRange",
            "selectionRange": "selectionRange",
            "selectionRanges": "selectionRange",
            "expandSelection": "selectionRange",
            "linkedEditing": "linkedEditing",
            "linkedEditingRange": "linkedEditing",
            "linkedEditingRanges": "linkedEditing",
            "prepareCallHierarchy": "prepareCallHierarchy",
            "callHierarchy": "prepareCallHierarchy",
            "callHierarchyPrepare": "prepareCallHierarchy",
            "incomingCalls": "callHierarchyIncoming",
            "callHierarchyIncoming": "callHierarchyIncoming",
            "outgoingCalls": "callHierarchyOutgoing",
            "callHierarchyOutgoing": "callHierarchyOutgoing",
            "prepareTypeHierarchy": "prepareTypeHierarchy",
            "typeHierarchy": "prepareTypeHierarchy",
            "typeHierarchyPrepare": "prepareTypeHierarchy",
            "supertypes": "typeHierarchySupertypes",
            "typeHierarchySupertypes": "typeHierarchySupertypes",
            "subtypes": "typeHierarchySubtypes",
            "typeHierarchySubtypes": "typeHierarchySubtypes",
            "documentColor": "documentColor",
            "documentColors": "documentColor",
            "colors": "documentColor",
            "colorPresentation": "colorPresentation",
            "colorPresentations": "colorPresentation",
            "semanticToken": "semanticTokens",
            "semanticTokens": "semanticTokens",
            "documentSemanticTokens": "semanticTokens",
            "semanticTokenRange": "semanticTokensRange",
            "semanticTokensRange": "semanticTokensRange",
            "documentRangeSemanticTokens": "semanticTokensRange",
            "documentSymbol": "documentSymbol",
            "documentSymbols": "documentSymbol",
            "symbols": "documentSymbol",
            "workspaceSymbol": "workspaceSymbol",
            "workspaceSymbols": "workspaceSymbol",
            "resolveWorkspaceSymbol": "resolveWorkspaceSymbol",
            "codeAction": "codeActions",
            "codeActions": "codeActions",
            "format": "formatting",
            "formatting": "formatting",
            "formatDocument": "formatting",
            "formatRange": "rangeFormatting",
            "rangeFormatting": "rangeFormatting",
            "formatSelection": "rangeFormatting",
            "formatOnType": "onTypeFormatting",
            "onTypeFormatting": "onTypeFormatting",
            "prepareDocumentPaste": "prepareDocumentPaste",
            "documentPaste": "documentPaste",
            "documentPasteEdit": "documentPaste",
            "documentPasteEdits": "documentPaste",
            "paste": "documentPaste",
            "pasteEdit": "documentPaste",
            "pasteEdits": "documentPaste",
            "pasteAs": "documentPaste",
            "documentDrop": "documentDrop",
            "documentDropEdit": "documentDrop",
            "documentDropEdits": "documentDrop",
            "drop": "documentDrop",
            "dropEdit": "documentDrop",
            "dropEdits": "documentDrop",
        }
        kind = kind_aliases.get(str(payload.get("kind") or "").strip())
        if not kind:
            return {"error": "Unsupported language provider kind"}

        self._ensure_engine()
        content = "" if payload.get("content") is None else str(payload.get("content"))
        language = str(
            payload.get("language")
            or payload.get("languageId")
            or "plaintext")
        if language:
            try:
                self._ext_host.activate_event(f"onLanguage:{language}")
            except Exception:
                pass
        document = self._vscode_ns.update_text_document_snapshot(
            _editor_provider_uri(payload), content, language)
        if "dirty" in payload:
            document.isDirty = bool(payload.get("dirty"))
        pos_value = payload.get("position")
        if pos_value is None:
            pos_value = payload.get("offset")
        position = _editor_provider_position(pos_value, content)

        try:
            if kind == "completion":
                try:
                    item_resolve_count = int(
                        payload.get("itemResolveCount")
                        if payload.get("itemResolveCount") is not None
                        else payload.get("resolveCount") or 0)
                except Exception:
                    item_resolve_count = 0
                result = self._ext_host.commands.execute(
                    "vscode.executeCompletionItemProvider",
                    document.uri,
                    position,
                    payload.get("triggerCharacter"),
                    max(0, item_resolve_count),
                )
                value = _json_ready_language_value(result)
                if isinstance(value, dict):
                    items = value.get("items") or []
                    incomplete = bool(value.get("isIncomplete"))
                else:
                    items = value if isinstance(value, list) else []
                    incomplete = False
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "items": items,
                    "isIncomplete": incomplete,
                }
            if kind == "hover":
                result = self._ext_host.commands.execute(
                    "vscode.executeHoverProvider", document.uri, position)
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "hovers": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "signatureHelp":
                result = self._ext_host.commands.execute(
                    "vscode.executeSignatureHelpProvider",
                    document.uri,
                    position,
                    payload.get("triggerCharacter"),
                    payload.get("triggerKind"),
                    payload.get("isRetrigger"),
                    payload.get("activeSignatureHelp"),
                )
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "signatureHelp": _json_ready_language_value(result),
                }
            if kind == "definition":
                result = self._ext_host.commands.execute(
                    "vscode.executeDefinitionProvider", document.uri, position)
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "definitions": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "typeDefinition":
                result = self._ext_host.commands.execute(
                    "vscode.executeTypeDefinitionProvider",
                    document.uri, position)
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "typeDefinitions": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "declaration":
                result = self._ext_host.commands.execute(
                    "vscode.executeDeclarationProvider",
                    document.uri, position)
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "declarations": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "implementation":
                result = self._ext_host.commands.execute(
                    "vscode.executeImplementationProvider",
                    document.uri, position)
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "implementations": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "references":
                context = {
                    "includeDeclaration": bool(
                        payload.get("includeDeclaration", True)),
                }
                result = self._ext_host.commands.execute(
                    "vscode.executeReferenceProvider",
                    document.uri,
                    position,
                    context,
                )
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "references": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "documentHighlight":
                result = self._ext_host.commands.execute(
                    "vscode.executeDocumentHighlightProvider",
                    document.uri,
                    position,
                )
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "highlights": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "prepareRename":
                result = self._ext_host.commands.execute(
                    "_executePrepareRename", document.uri, position)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "prepareRename": _json_ready_language_value(result),
                }
            if kind == "rename":
                new_name = str(payload.get("newName") or "")
                if not new_name:
                    return {"error": "New name is required"}
                result = self._ext_host.commands.execute(
                    "_executeDocumentRenameProvider",
                    document.uri,
                    position,
                    new_name,
                )
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "edit": _json_ready_language_value(result),
                }
            if kind == "documentLink":
                try:
                    link_resolve_count = int(
                        payload.get("linkResolveCount")
                        if payload.get("linkResolveCount") is not None
                        else payload.get("resolveCount") or 0)
                except Exception:
                    link_resolve_count = 0
                result = self._ext_host.commands.execute(
                    "vscode.executeLinkProvider",
                    document.uri,
                    max(0, link_resolve_count),
                )
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "links": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "inlayHint":
                hint_range = _editor_provider_range(
                    payload.get("range"), content)
                try:
                    hint_resolve_count = int(
                        payload.get("hintResolveCount")
                        if payload.get("hintResolveCount") is not None
                        else payload.get("resolveCount") or 0)
                except Exception:
                    hint_resolve_count = 0
                result = self._ext_host.commands.execute(
                    "vscode.executeInlayHintProvider",
                    document.uri,
                    hint_range,
                    max(0, hint_resolve_count),
                )
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "hints": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "inlineCompletion":
                try:
                    trigger_kind = int(payload.get("triggerKind", 1))
                except Exception:
                    trigger_kind = 1
                context = payload.get("context")
                if not isinstance(context, dict):
                    context = {}
                context = {
                    **context,
                    "triggerKind": 0 if trigger_kind == 0 else 1,
                    "selectedCompletionInfo": (
                        payload.get("selectedCompletionInfo")
                        if payload.get("selectedCompletionInfo") is not None
                        else context.get("selectedCompletionInfo")),
                }
                result = self._ext_host.commands.execute(
                    "_executeInlineCompletionProvider",
                    document.uri,
                    position,
                    context,
                )
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "items": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "codeLens":
                try:
                    resolve_count = int(
                        payload.get("itemResolveCount")
                        if payload.get("itemResolveCount") is not None
                        else payload.get("resolveCount") or 0)
                except Exception:
                    resolve_count = 0
                result = self._ext_host.commands.execute(
                    "vscode.executeCodeLensProvider",
                    document.uri,
                    max(0, resolve_count),
                )
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "lenses": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "foldingRange":
                result = self._ext_host.commands.execute(
                    "vscode.executeFoldingRangeProvider",
                    document.uri,
                )
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "ranges": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "selectionRange":
                positions = payload.get("positions")
                if isinstance(positions, list):
                    selection_positions = [
                        _editor_provider_position(item, content)
                        for item in positions
                    ]
                else:
                    selection_positions = [position]
                result = self._ext_host.commands.execute(
                    "vscode.executeSelectionRangeProvider",
                    document.uri,
                    selection_positions,
                )
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "ranges": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "linkedEditing":
                result = self._ext_host.commands.execute(
                    "_executeLinkedEditingProvider",
                    document.uri,
                    position,
                )
                value = _json_ready_language_value(result)
                ranges = []
                if isinstance(value, dict):
                    raw_ranges = value.get("ranges")
                    ranges = raw_ranges if isinstance(raw_ranges, list) else []
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "linkedEditing": value,
                    "ranges": ranges,
                }
            if kind == "prepareCallHierarchy":
                result = self._ext_host.commands.execute(
                    "vscode.prepareCallHierarchy", document.uri, position)
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "items": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "callHierarchyIncoming":
                result = self._ext_host.commands.execute(
                    "vscode.provideIncomingCalls",
                    payload.get("item") or payload.get("callHierarchyItem"),
                )
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "calls": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "callHierarchyOutgoing":
                result = self._ext_host.commands.execute(
                    "vscode.provideOutgoingCalls",
                    payload.get("item") or payload.get("callHierarchyItem"),
                )
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "calls": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "prepareTypeHierarchy":
                result = self._ext_host.commands.execute(
                    "vscode.prepareTypeHierarchy", document.uri, position)
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "items": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "typeHierarchySupertypes":
                result = self._ext_host.commands.execute(
                    "vscode.provideSupertypes",
                    payload.get("item") or payload.get("typeHierarchyItem"),
                )
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "items": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "typeHierarchySubtypes":
                result = self._ext_host.commands.execute(
                    "vscode.provideSubtypes",
                    payload.get("item") or payload.get("typeHierarchyItem"),
                )
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "items": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "documentColor":
                result = self._ext_host.commands.execute(
                    "vscode.executeDocumentColorProvider",
                    document.uri,
                )
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "colors": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "colorPresentation":
                color_range = _editor_provider_range(
                    payload.get("range"), content)
                result = self._ext_host.commands.execute(
                    "vscode.executeColorPresentationProvider",
                    payload.get("color") or {},
                    {"uri": document.uri, "range": color_range},
                )
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "presentations": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "semanticTokens":
                legend = self._ext_host.commands.execute(
                    "vscode.provideDocumentSemanticTokensLegend",
                    document.uri,
                )
                result = self._ext_host.commands.execute(
                    "vscode.provideDocumentSemanticTokens",
                    document.uri,
                )
                legend_value = _json_ready_language_value(legend)
                value = _json_ready_language_value(result)
                if isinstance(value, dict) and "tokens" in value:
                    if not legend_value and value.get("legend") is not None:
                        legend_value = value.get("legend")
                    value = value.get("tokens")
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "legend": legend_value,
                    "tokens": value,
                }
            if kind == "semanticTokensRange":
                token_range = _editor_provider_range(
                    payload.get("range"), content)
                legend = self._ext_host.commands.execute(
                    "vscode.provideDocumentRangeSemanticTokensLegend",
                    document.uri,
                )
                result = self._ext_host.commands.execute(
                    "vscode.provideDocumentRangeSemanticTokens",
                    document.uri,
                    token_range,
                )
                legend_value = _json_ready_language_value(legend)
                value = _json_ready_language_value(result)
                if isinstance(value, dict) and "tokens" in value:
                    if not legend_value and value.get("legend") is not None:
                        legend_value = value.get("legend")
                    value = value.get("tokens")
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "legend": legend_value,
                    "tokens": value,
                }
            if kind == "documentSymbol":
                result = self._ext_host.commands.execute(
                    "vscode.executeDocumentSymbolProvider", document.uri)
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "symbols": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "workspaceSymbol":
                query = str(payload.get("query")
                            if payload.get("query") is not None
                            else payload.get("search") or "")
                result = self._ext_host.commands.execute(
                    "vscode.executeWorkspaceSymbolProvider", query)
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "symbols": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "resolveWorkspaceSymbol":
                result = self._ext_host.commands.execute(
                    "_resolveWorkspaceSymbolProvider",
                    payload.get("symbol") or {},
                )
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "symbol": _json_ready_language_value(result),
                }
            if kind == "codeActions":
                action_range = _editor_provider_range(
                    payload.get("range"), content)
                try:
                    item_resolve_count = int(
                        payload.get("itemResolveCount")
                        if payload.get("itemResolveCount") is not None
                        else payload.get("resolveCount") or 0)
                except Exception:
                    item_resolve_count = 0
                result = self._ext_host.commands.execute(
                    "vscode.executeCodeActionProvider",
                    document.uri,
                    action_range,
                    payload.get("only"),
                    max(0, item_resolve_count),
                )
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "actions": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind in {"prepareDocumentPaste", "documentPaste"}:
                raw_ranges = payload.get("ranges")
                if isinstance(raw_ranges, list):
                    paste_ranges = [
                        _editor_provider_range(item, content)
                        for item in raw_ranges
                    ]
                elif payload.get("range") is not None:
                    paste_ranges = [
                        _editor_provider_range(payload.get("range"), content)]
                else:
                    paste_ranges = [Range(position, position)]
                data_transfer = (
                    payload.get("dataTransfer")
                    if payload.get("dataTransfer") is not None
                    else payload.get("data_transfer"))
                if data_transfer is None:
                    text_value = (
                        payload.get("pasteText")
                        if payload.get("pasteText") is not None
                        else payload.get("clipboardText"))
                    if text_value is not None:
                        data_transfer = {"text/plain": str(text_value)}
                if kind == "prepareDocumentPaste":
                    result = self._ext_host.commands.execute(
                        "_prepareDocumentPasteProvider",
                        document.uri,
                        paste_ranges,
                        data_transfer or {},
                    )
                    return {
                        "ok": True,
                        "kind": kind,
                        "uri": str(document.uri),
                        "version": document.version,
                        "dataTransfer": _json_ready_language_value(result),
                    }
                context = payload.get("context")
                if not isinstance(context, dict):
                    context = {}
                context = dict(context)
                if payload.get("triggerKind") is not None:
                    context["triggerKind"] = payload.get("triggerKind")
                if payload.get("only") is not None:
                    context["only"] = payload.get("only")
                try:
                    resolve_count = int(
                        payload.get("pasteResolveCount")
                        if payload.get("pasteResolveCount") is not None
                        else payload.get("resolveCount") or 0)
                except Exception:
                    resolve_count = 0
                result = self._ext_host.commands.execute(
                    "_executeDocumentPasteEditProvider",
                    document.uri,
                    paste_ranges,
                    data_transfer or {},
                    context,
                    max(0, resolve_count),
                )
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "pasteEdits": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "documentDrop":
                data_transfer = (
                    payload.get("dataTransfer")
                    if payload.get("dataTransfer") is not None
                    else payload.get("data_transfer"))
                if data_transfer is None:
                    text_value = (
                        payload.get("dropText")
                        if payload.get("dropText") is not None
                        else payload.get("text"))
                    if text_value is not None:
                        data_transfer = {"text/plain": str(text_value)}
                try:
                    resolve_count = int(
                        payload.get("dropResolveCount")
                        if payload.get("dropResolveCount") is not None
                        else payload.get("resolveCount") or 0)
                except Exception:
                    resolve_count = 0
                result = self._ext_host.commands.execute(
                    "_executeDocumentDropEditProvider",
                    document.uri,
                    position,
                    data_transfer or {},
                    max(0, resolve_count),
                )
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "dropEdits": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            options = payload.get("options")
            if not isinstance(options, dict):
                options = {"tabSize": 4, "insertSpaces": True}
            if kind == "rangeFormatting":
                format_range = _editor_provider_range(
                    payload.get("range"), content)
                result = self._ext_host.commands.execute(
                    "vscode.executeFormatRangeProvider",
                    document.uri,
                    format_range,
                    options,
                )
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "edits": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            if kind == "onTypeFormatting":
                trigger = (
                    payload.get("triggerCharacter")
                    if payload.get("triggerCharacter") is not None
                    else payload.get("ch")
                )
                if trigger is None:
                    trigger = payload.get("character")
                result = self._ext_host.commands.execute(
                    "vscode.executeFormatOnTypeProvider",
                    document.uri,
                    position,
                    "" if trigger is None else str(trigger),
                    options,
                )
                value = _json_ready_language_value(result)
                return {
                    "ok": True,
                    "kind": kind,
                    "uri": str(document.uri),
                    "version": document.version,
                    "edits": value if isinstance(value, list) else (
                        [] if value is None else [value]),
                }
            result = self._ext_host.commands.execute(
                "vscode.executeFormatDocumentProvider",
                document.uri,
                options,
            )
            value = _json_ready_language_value(result)
            return {
                "ok": True,
                "kind": kind,
                "uri": str(document.uri),
                "version": document.version,
                "edits": value if isinstance(value, list) else (
                    [] if value is None else [value]),
            }
        except KeyError as exc:
            return {"error": f"Language provider command not found: {exc}"}
        except Exception as exc:
            return {"error": str(exc)}

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

    def _on_tree_view_changed(self, event: Dict[str, Any]) -> None:
        self._emit("extension_tree_changed", {
            "change": {"source": "runtime_tree", **_as_dict(event)},
        })

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

        vscode_ns = getattr(self, "_vscode_ns", None)

        desired: Dict[str, ChatProviderDef] = {}
        for spec in self._dynamic_webview_provider_specs():
            provider_id = str(spec.get("provider_id") or "").strip()
            view_id = str(spec.get("view_id") or "").strip()
            if not provider_id or not view_id:
                continue
            # Only create dynamic providers for views that have REAL HTML
            # content from an activated extension. Manifest-only declarations
            # should not create phantom tabs.
            if vscode_ns:
                html = vscode_ns.get_webview_html(view_id) if hasattr(vscode_ns, "get_webview_html") else ""
                if not html:
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
        # CLI provider views removed: claude-code/codex use native chat
        # panel with CliChatController, not separate webview views.

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
        workspace_activated = self._activate_workspace_contains_extensions()
        if workspace_activated:
            print("[ExtHost] "
                  f"{workspace_activated} workspaceContains activation "
                  "event(s) triggered.")
            self._register_ext_tools()

    def _workspace_contains_activation_events(self) -> List[str]:
        try:
            root = self._workspace_root()
        except Exception:
            return []
        if not root or not os.path.isdir(root):
            return []
        entries: List[tuple[str, str]] = []
        patterns: List[str] = []
        seen_patterns = set()
        for ext in self._ext_host.registry.list_all():
            if not getattr(ext, "enabled", True):
                continue
            for raw_event in getattr(ext, "activation_events", []) or []:
                event = str(raw_event or "")
                if not event.startswith(_WORKSPACE_CONTAINS_PREFIX):
                    continue
                pattern = event[len(_WORKSPACE_CONTAINS_PREFIX):].strip()
                if not pattern:
                    continue
                entries.append((event, pattern))
                if pattern not in seen_patterns:
                    seen_patterns.add(pattern)
                    patterns.append(pattern)
        match_map = self._workspace_contains_match_map(root, patterns)
        matched: List[str] = []
        seen_events = set()
        for event, pattern in entries:
            if event in seen_events:
                continue
            if match_map.get(pattern, False):
                seen_events.add(event)
                matched.append(event)
        return matched

    def _activate_workspace_contains_extensions(self) -> int:
        triggered = 0
        for event in self._workspace_contains_activation_events():
            self._ext_host.activate_event(event)
            triggered += 1
        return triggered

    def _install_node_runtime_event_bridge(self, host: Any) -> None:
        if host is None or getattr(host, "_sao_runtime_bridge_installed", False):
            return
        host.set_lm_model_request_callback(self._handle_node_lm_model_request)
        host.set_lm_model_cancel_callback(self._handle_node_lm_model_cancel)
        host.on_tree_event(self._handle_node_tree_event)
        host.on_config_set(self._handle_node_config_set)
        host.on_lm_tool_event(self._handle_node_lm_tool_event)
        host.on_chat_participant_event(
            self._handle_node_chat_participant_event)
        setattr(host, "_sao_runtime_bridge_installed", True)

    def _try_start_node_extension_host(self) -> None:
        """Start a NodeExtensionHost for extensions that declare ``main``.

        Filters registered extensions, and if any have a JS entry point and
        Node.js is available, spawns the Node subprocess and syncs their
        descriptions. Runtime activation follows VS Code activation events
        instead of eagerly activating every extension at startup.
        Extensions without ``main`` are unaffected.
        """
        from ai_editor.extension_host import NodeExtensionHost
        from ai_editor.node_runtime import get_node_path

        all_exts = self._ext_host.registry.list_all()
        node_exts = [ext for ext in all_exts if ext.main and ext.enabled]
        if not node_exts:
            return
        existing_host = getattr(self, "_node_ext_host", None)
        if existing_host is not None and existing_host.is_running:
            existing_host.set_diagnostics_enabled(
                self._extension_diagnostics_enabled())
            existing_host.set_command_service(self._ext_host.commands)
            self._install_node_runtime_event_bridge(existing_host)
            existing_host.register_extensions(node_exts)
            self._install_node_activation_event_bridge()
            activated = self._activate_node_startup_extensions(wait=False)
            if activated:
                print(
                    f"[NodeExtHost] {activated} startup JS extension(s) "
                    "sent for activation.")
                self._sync_settings_to_node_host()
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
        host.set_diagnostics_enabled(self._extension_diagnostics_enabled())
        host.set_command_service(self._ext_host.commands)
        self._install_node_runtime_event_bridge(host)

        if not host.start():
            print("[NodeExtHost] Failed to start Node subprocess.")
            return

        self._node_ext_host = host
        self._vscode_ns.set_language_provider_request_callback(
            self._request_node_language_provider)
        self._install_node_activation_event_bridge()
        host.register_extensions(node_exts)

        # Push current settings so getConfiguration() returns real values
        self._sync_settings_to_node_host()
        activated = self._activate_node_startup_extensions(wait=False)
        print(f"[NodeExtHost] {len(node_exts)} JS extension(s) registered; "
              f"{activated} startup activation(s) sent.")

    def _install_node_activation_event_bridge(self) -> None:
        if getattr(self, "_node_activation_event_bridge_installed", False):
            return
        self._node_activation_event_bridge_installed = True
        self._node_activation_event_bridge_dispose = (
            self._ext_host.on_activation_event(
                lambda event, results: self._activate_node_extensions_for_event(
                    event,
                    wait=self._node_activation_event_should_wait(event),
                )
            )
        )

    @staticmethod
    def _node_activation_event_should_wait(event: str) -> bool:
        event_text = str(event or "")
        return event_text.startswith((
            "onCommand:",
            "onView:",
            "onCustomEditor:",
            "onLanguage:",
            _WORKSPACE_CONTAINS_PREFIX,
        ))

    def _node_extensions_for_activation_event(self, event: str) -> List[Any]:
        try:
            candidates = self._ext_host.registry.get_for_activation_event(event)
        except Exception:
            return []
        node_exts: List[Any] = []
        seen = set()
        for ext in candidates:
            ext_id = getattr(ext, "id", "")
            if (not getattr(ext, "main", "")
                    or not getattr(ext, "enabled", True)
                    or ext_id in seen):
                continue
            if not self._extension_allowed(ext):
                continue
            node_exts.append(ext)
            seen.add(ext_id)
        return node_exts

    def _node_extensions_with_dependencies(
            self, extensions: List[Any]) -> List[Any]:
        ordered: List[Any] = []
        seen = set()
        visiting = set()

        def visit(ext: Any) -> None:
            ext_id = getattr(ext, "id", "")
            if not ext_id or ext_id in seen or ext_id in visiting:
                return
            visiting.add(ext_id)
            for dep_id in getattr(ext, "extension_dependencies", []) or []:
                try:
                    dep = self._ext_host.registry.get(str(dep_id))
                except Exception:
                    dep = None
                if (dep is None
                        or not getattr(dep, "main", "")
                        or not getattr(dep, "enabled", True)):
                    continue
                if self._extension_allowed(dep):
                    visit(dep)
            visiting.discard(ext_id)
            seen.add(ext_id)
            ordered.append(ext)

        for ext in extensions:
            visit(ext)
        return ordered

    def _activate_node_extensions_for_event(
            self, event: str, wait: bool = True) -> int:
        node_host = getattr(self, "_node_ext_host", None)
        if node_host is None or not getattr(node_host, "is_running", False):
            return 0
        candidates = self._node_extensions_with_dependencies(
            self._node_extensions_for_activation_event(event))
        targets = [
            ext for ext in candidates
            if (not node_host.is_extension_activated(ext.id)
                and not node_host.is_extension_activation_pending(ext.id))
        ]
        if not targets:
            return 0
        sent = node_host.activate_all(targets, wait=wait, timeout=8.0)
        if sent:
            self._sync_settings_to_node_host()
        return sent

    def _activate_node_startup_extensions(self, wait: bool = False) -> int:
        activated = self._activate_node_extensions_for_event("*", wait=wait)
        activated += self._activate_node_extensions_for_event(
            "onStartupFinished", wait=wait)
        return activated

    def _request_node_language_provider(self, payload: Dict[str, Any]) -> Dict[str, Any]:
        """Bridge VS Code language-provider execution into the Node host."""
        host = getattr(self, "_node_ext_host", None)
        if host is None or not host.is_running:
            return {
                "ok": False,
                "value": None,
                "error": "Node extension host is not running",
            }
        return host.request_language_provider_result(payload, default=None)

    def _handle_node_lm_model_request(
            self, payload: Dict[str, Any]) -> Dict[str, Any]:
        """Bridge Node ``vscode.lm.selectChatModels`` model APIs."""
        action = str(payload.get("action") or "")
        try:
            self._ensure_engine()
            if action == "selectChatModels":
                selector = _as_dict(payload.get("selector"))
                models = self._vscode_ns._select_chat_models(selector or None)
                return {
                    "ok": True,
                    "value": [
                        self._serialize_node_lm_model(model)
                        for model in models
                    ],
                }
            model_id = str(payload.get("modelId") or "")
            model = self._node_lm_model_by_id(model_id)
            if model is None:
                return {
                    "ok": False,
                    "error": f"Language model not found: {model_id}",
                }
            if action == "countTokens":
                return {
                    "ok": True,
                    "value": model.count_tokens(payload.get("text", "")),
                }
            if action == "sendRequest":
                return {
                    "ok": True,
                    "value": self._send_node_lm_request(
                        model,
                        payload.get("messages") or [],
                        _as_dict(payload.get("options")),
                    ),
                }
            return {"ok": False, "error": f"Unknown LM model action: {action}"}
        except Exception as exc:
            return {"ok": False, "error": str(exc)}

    def _handle_node_lm_model_cancel(self, payload: Dict[str, Any]) -> Dict[str, Any]:
        """Cancel an in-flight Python-backed Node language model request."""
        action = str(payload.get("action") or "")
        if action != "sendRequest":
            return {"ok": True, "cancelled": False}
        engine = getattr(self, "_engine", None)
        if engine is None:
            return {"ok": True, "cancelled": False}
        cancel = getattr(engine, "cancel", None)
        if not callable(cancel):
            return {"ok": True, "cancelled": False}
        cancel()
        return {
            "ok": True,
            "cancelled": True,
            "requestId": str(payload.get("requestId") or ""),
        }

    def _node_lm_model_by_id(self, model_id: str) -> Any:
        models = self._vscode_ns._select_chat_models(
            {"id": model_id} if model_id else None)
        for model in models:
            if str(getattr(model, "id", "")) == model_id:
                return model
        return models[0] if models and not model_id else None

    @staticmethod
    def _serialize_node_lm_model(model: Any) -> Dict[str, Any]:
        caps = getattr(model, "capabilities", None)
        return {
            "id": str(getattr(model, "id", "") or ""),
            "name": str(getattr(model, "name", "") or getattr(model, "id", "")),
            "vendor": str(getattr(model, "vendor", "") or ""),
            "family": str(getattr(model, "family", "") or ""),
            "version": str(getattr(model, "version", "") or ""),
            "maxInputTokens": int(getattr(model, "max_input_tokens", 0) or 0),
            "capabilities": {
                "supportsImageToText": bool(
                    getattr(caps, "supports_image_to_text", False)),
                "supportsToolCalling": bool(
                    getattr(caps, "supports_tool_calling", True)),
                "editToolsHint": bool(getattr(caps, "edit_tools_hint", False)),
            },
        }

    @staticmethod
    def _node_lm_role(value: Any) -> str:
        if value == 0:
            return "system"
        if value == 2:
            return "assistant"
        text = str(value or "user").lower()
        if text in {"system", "assistant", "user", "tool"}:
            return text
        return "user"

    @classmethod
    def _node_lm_content_text(cls, value: Any) -> str:
        if value is None:
            return ""
        if isinstance(value, (str, int, float, bool)):
            return str(value)
        if isinstance(value, list):
            return "".join(cls._node_lm_content_text(item) for item in value)
        if isinstance(value, dict):
            for key in ("value", "text", "content"):
                if key in value:
                    return cls._node_lm_content_text(value.get(key))
            return json.dumps(value, ensure_ascii=False, default=str)
        return str(value)

    @classmethod
    def _node_lm_messages(cls, messages: Any) -> List[Dict[str, Any]]:
        if not isinstance(messages, list):
            messages = [messages]
        result = []
        for message in messages:
            if isinstance(message, dict):
                result.append({
                    "role": cls._node_lm_role(message.get("role")),
                    "content": cls._node_lm_content_text(
                        message.get("content")),
                })
            else:
                result.append({"role": "user", "content": str(message)})
        return result

    @staticmethod
    def _node_lm_response_part_text(part: Any) -> str:
        if part is None:
            return ""
        if isinstance(part, (str, int, float, bool)):
            return str(part)
        if isinstance(part, dict):
            part_type = str(part.get("type") or "").lower()
            if part_type == "text":
                return str(
                    part.get("value")
                    if part.get("value") is not None
                    else part.get("text", ""))
            if part_type:
                return ""
            for key in ("value", "text", "content"):
                if key in part and isinstance(part.get(key), (str, int, float, bool)):
                    return str(part.get(key))
            return ""
        value = getattr(part, "value", None)
        return str(value) if value is not None else ""

    @classmethod
    def _serialize_node_lm_response_part(cls, part: Any) -> Dict[str, Any]:
        if isinstance(part, dict) and part.get("type"):
            if str(part.get("type") or "").lower() in {"data", "data_part", "datapart"}:
                return cls._serialize_node_lm_data_part(part)
            return dict(part)
        if hasattr(part, "call_id") and hasattr(part, "name"):
            return {
                "type": "tool_call",
                "callId": str(getattr(part, "call_id", "") or ""),
                "name": str(getattr(part, "name", "") or ""),
                "input": getattr(part, "input", None) or {},
            }
        if hasattr(part, "data") and hasattr(part, "mime_type"):
            return cls._serialize_node_lm_data_part(part)
        if hasattr(part, "metadata") and hasattr(part, "id"):
            return {
                "type": "thinking",
                "value": cls._node_lm_response_part_text(part),
                "id": str(getattr(part, "id", "") or ""),
                "metadata": getattr(part, "metadata", None) or {},
            }
        return {"type": "text", "value": cls._node_lm_response_part_text(part)}

    @staticmethod
    def _node_lm_data_bytes(value: Any, encoding: Any = None) -> bytes:
        if value is None:
            return b""
        if isinstance(value, bytes):
            return value
        if isinstance(value, bytearray):
            return bytes(value)
        if isinstance(value, memoryview):
            return value.tobytes()
        if isinstance(value, (list, tuple)):
            try:
                return bytes(int(item) & 0xff for item in value)
            except Exception:
                return json.dumps(value, ensure_ascii=False).encode("utf-8")
        if isinstance(value, str):
            if str(encoding or "").lower() == "base64":
                try:
                    return base64.b64decode(value)
                except Exception:
                    return value.encode("utf-8")
            return value.encode("utf-8")
        try:
            return json.dumps(value, ensure_ascii=False).encode("utf-8")
        except Exception:
            return str(value).encode("utf-8")

    @classmethod
    def _serialize_node_lm_data_part(cls, part: Any) -> Dict[str, Any]:
        if isinstance(part, dict):
            raw_data = (
                part.get("data")
                if "data" in part else part.get("value")
                if "value" in part else part.get("content", b"")
            )
            mime_type = (
                part.get("mimeType")
                or part.get("mime_type")
                or part.get("mime")
                or "application/octet-stream"
            )
            encoding = part.get("encoding") or ("base64" if part.get("base64") else None)
        else:
            raw_data = getattr(part, "data", b"") or b""
            mime_type = (
                getattr(part, "mime_type", None)
                or getattr(part, "mimeType", None)
                or getattr(part, "mime", None)
                or "application/octet-stream"
            )
            encoding = getattr(part, "encoding", None)
        data = cls._node_lm_data_bytes(raw_data, encoding)
        return {
            "type": "data",
            "data": base64.b64encode(data).decode("ascii"),
            "encoding": "base64",
            "mimeType": str(mime_type or "application/octet-stream"),
        }

    def _send_node_lm_request(
            self, model: Any, messages: Any,
            options: Dict[str, Any]) -> Dict[str, Any]:
        response = model.send_request(
            self._node_lm_messages(messages),
            options or {},
            None,
        )
        raw_chunks = list(getattr(response, "_chunks", []) or [])
        if not raw_chunks:
            stream = getattr(response, "stream", None)
            if stream is not None:
                try:
                    raw_chunks = list(stream)
                except Exception:
                    raw_chunks = []
        text = str(
            getattr(response, "text", "")
            or "".join(
                self._node_lm_response_part_text(item)
                for item in raw_chunks))
        chunks = [
            self._serialize_node_lm_response_part(item)
            for item in raw_chunks
        ]
        return {
            "text": text,
            "chunks": chunks or ([{"type": "text", "value": text}] if text else []),
        }

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
            host.set_diagnostics_enabled(self._extension_diagnostics_enabled())
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
            settings_data = getattr(settings, "data", None)
            if not isinstance(settings_data, dict):
                settings_data = getattr(settings, "_data", None)
            if isinstance(settings_data, dict):
                for key, value in settings_data.items():
                    key_str = str(key)
                    if (key_str.startswith("[") and key_str.endswith("]")
                            and isinstance(value, dict)):
                        raw[key_str] = dict(value)
            host.send_settings_sync(raw)
        except Exception as exc:
            print(f"[NodeExtHost] Failed to sync settings: {exc}")

    def _handle_node_config_set(self, section: str, key: str,
                                value: Any, remove: bool = False,
                                override_identifier: str = "") -> None:
        """Handle a config_set message from Node.

        Persists the change via the Python settings manager and notifies
        the Node host of the final value (in case Python normalises it).
        """
        settings = _resolve_settings(self._gui_ref)
        if not settings:
            return

        def _parts(text: str) -> List[str]:
            return [
                part for part in str(text or "").strip(".").split(".")
                if part
            ]

        def _set_nested(target: Dict[str, Any],
                        path: List[str],
                        next_value: Any) -> None:
            if not path:
                return
            current = target
            for part in path[:-1]:
                child = current.get(part)
                if not isinstance(child, dict):
                    child = {}
                current[part] = child
                current = child
            current[path[-1]] = next_value

        def _delete_nested(target: Dict[str, Any], path: List[str]) -> None:
            if not path:
                return
            current = target
            for part in path[:-1]:
                child = current.get(part)
                if not isinstance(child, dict):
                    return
                current = child
            current.pop(path[-1], None)

        def _set_ai_editor(path: List[str], next_value: Any) -> None:
            if not path:
                return
            ai_cfg = settings.get("ai_editor", {}) or {}
            if not isinstance(ai_cfg, dict):
                ai_cfg = {}
            if remove:
                _delete_nested(ai_cfg, path)
            else:
                _set_nested(ai_cfg, path, next_value)
            settings.set("ai_editor", ai_cfg)

        def _set_top_level(path: List[str], next_value: Any) -> None:
            if not path:
                return
            top_key = path[0]
            rest = path[1:]
            if not rest:
                if remove:
                    current_data = getattr(settings, "data", None)
                    if not isinstance(current_data, dict):
                        current_data = getattr(settings, "_data", None)
                    if isinstance(current_data, dict):
                        current_data.pop(top_key, None)
                else:
                    settings.set(top_key, next_value)
                return
            current = settings.get(top_key, {}) or {}
            if not isinstance(current, dict):
                current = {}
            if remove:
                _delete_nested(current, rest)
            else:
                _set_nested(current, rest, next_value)
            settings.set(top_key, current)

        def _delete_top_level_key(top_key: str) -> None:
            current_data = getattr(settings, "data", None)
            if not isinstance(current_data, dict):
                current_data = getattr(settings, "_data", None)
            if isinstance(current_data, dict):
                current_data.pop(top_key, None)

        def _set_language_override(language_id: str, setting_key: str,
                                   next_value: Any) -> None:
            language_id = str(language_id or "").strip()
            setting_key = str(setting_key or "").strip(".")
            if not language_id or not setting_key:
                return
            override_key = f"[{language_id}]"
            current = settings.get(override_key, {}) or {}
            if not isinstance(current, dict):
                current = {}
            setting_keys = [setting_key]
            setting_parts = _parts(setting_key)
            if (len(setting_parts) > 1 and setting_parts[0] == "ai_editor"
                    and setting_parts[1] in _AI_EDITOR_SECTION_DEFAULTS):
                setting_keys.append(".".join(setting_parts[1:]))
            elif (setting_parts
                    and setting_parts[0] in _AI_EDITOR_SECTION_DEFAULTS):
                setting_keys.append("ai_editor." + setting_key)
            for item_key in dict.fromkeys(setting_keys):
                if remove:
                    current.pop(item_key, None)
                else:
                    current[item_key] = next_value
            if current:
                settings.set(override_key, current)
            else:
                _delete_top_level_key(override_key)

        try:
            section_path = _parts(section)
            key_path = _parts(key)
            if not key_path:
                return
            full_setting_key = ".".join(section_path + key_path)
            if str(override_identifier or "").strip():
                _set_language_override(
                    override_identifier, full_setting_key, value)
                settings.save()
                self._notify_node_settings_changed()
                return

            if section_path == ["ai_editor"]:
                _set_ai_editor(key_path, value)
            elif len(section_path) > 1 and section_path[0] == "ai_editor":
                _set_ai_editor(section_path[1:] + key_path, value)
            elif section_path and section_path[0] in _AI_EDITOR_SECTION_DEFAULTS:
                _set_ai_editor(section_path + key_path, value)
            elif not section_path and key_path[0] == "ai_editor":
                _set_ai_editor(key_path[1:], value)
            elif not section_path and key_path[0] in _AI_EDITOR_SECTION_DEFAULTS:
                _set_ai_editor(key_path, value)
            else:
                _set_top_level(section_path + key_path, value)
            settings.save()
            self._notify_node_settings_changed()
        except Exception as exc:
            print(f"[NodeExtHost] Failed to persist config_set "
                  f"{section}.{key}: {exc}")

    def _handle_node_lm_tool_event(
            self, event: str, name: str, payload: Dict[str, Any]) -> None:
        """Bridge Node ``vscode.lm.registerTool`` into Python tools."""
        tool_name = str(name or "")
        if not tool_name:
            return
        if event == "lm_tool_disposed":
            record = self._node_lm_tool_disposables.pop(tool_name, None)
            disposable = (
                record.get("disposable")
                if isinstance(record, dict) else record)
            if disposable is not None and hasattr(disposable, "dispose"):
                disposable.dispose()
            ext_id = str(
                (record or {}).get("extensionId")
                if isinstance(record, dict)
                else payload.get("extensionId") or "")
            if ext_id and getattr(self, "_registry", None):
                self._registry.unregister(
                    self._extension_tool_wrapper_name(ext_id, tool_name))
            return
        if event != "lm_tool_registered":
            return
        existing = self._node_lm_tool_disposables.pop(tool_name, None)
        disposable = (
            existing.get("disposable")
            if isinstance(existing, dict) else existing)
        if disposable is not None and hasattr(disposable, "dispose"):
            disposable.dispose()
        ext_id = str(payload.get("extensionId") or "runtime")
        schema = _as_dict(payload.get("inputSchema"))
        if not schema:
            schema = {"type": "object", "properties": {}}
        description = str(payload.get("description") or tool_name)
        record = {
            "description": description,
            "inputSchema": schema,
            "schema": schema,
            "_extensionId": ext_id,
            "extensionId": ext_id,
            "tags": payload.get("tags") if isinstance(
                payload.get("tags"), list) else [],
            "handler": (
                lambda options, token=None, _name=tool_name:
                self._invoke_node_lm_tool(_name, options, token)),
        }
        disp = self._vscode_ns._register_lm_tool(
            tool_name, record, extension_id=ext_id)
        self._node_lm_tool_disposables[tool_name] = {
            "disposable": disp,
            "extensionId": ext_id,
        }

    def _invoke_node_lm_tool(
            self, name: str, options: Any, token: Any = None) -> Any:
        host = getattr(self, "_node_ext_host", None)
        if host is None or not host.is_running:
            raise RuntimeError("Node extension host is not running")
        input_data = getattr(options, "input", options)
        response = host.request_lm_tool_result(name, input_data)
        if not response.get("ok"):
            raise RuntimeError(response.get("error") or "Node tool failed")
        return self._node_lm_tool_result(response.get("value"))

    @staticmethod
    def _node_lm_tool_result(value: Any) -> Any:
        from ai_editor.vscode_api import LanguageModelToolResult
        if isinstance(value, dict) and isinstance(value.get("content"), list):
            content = []
            for part in value.get("content", []):
                if isinstance(part, dict):
                    text = part.get("text")
                    if text is None:
                        text = part.get("value")
                    if text is not None:
                        content.append({
                            "type": part.get("type") or "text",
                            "text": str(text),
                        })
                    else:
                        content.append(part)
                else:
                    content.append({"type": "text", "text": str(part)})
            return LanguageModelToolResult(content=content)
        if value is None:
            return LanguageModelToolResult.text("")
        if isinstance(value, str):
            return LanguageModelToolResult.text(value)
        return LanguageModelToolResult.text(
            json.dumps(value, ensure_ascii=False, default=str))

    def _handle_node_chat_participant_event(
            self, event: str, participant_id: str,
            payload: Dict[str, Any]) -> None:
        """Bridge Node ``vscode.chat.createChatParticipant`` into providers."""
        pid = str(participant_id or "")
        if not pid:
            return
        provider_id = f"ext-{pid}"
        if event == "chat_participant_disposed":
            disposable = self._node_chat_participant_disposables.pop(
                pid, None)
            if disposable is not None and hasattr(disposable, "dispose"):
                disposable.dispose()
            registry = getattr(self, "_provider_registry", None)
            if registry is not None:
                registry.unregister(provider_id)
            return
        if event != "chat_participant_registered":
            return
        existing = self._node_chat_participant_disposables.pop(pid, None)
        if existing is not None and hasattr(existing, "dispose"):
            existing.dispose()
        cp = self._vscode_ns._create_chat_participant(
            pid,
            lambda req, ctx, stream, token, _pid=pid:
            self._invoke_node_chat_participant(_pid, req, ctx, stream, token),
        )
        self._node_chat_participant_disposables[pid] = cp
        registry = getattr(self, "_provider_registry", None)
        if registry is None:
            return
        manifest = self._manifest_chat_participant(pid)
        from ai_editor.chat_providers import ChatProviderDef
        ext_id = str(payload.get("extensionId") or "")
        registry.register(ChatProviderDef(
            id=provider_id,
            name=str(
                manifest.get("fullName")
                or manifest.get("name")
                or payload.get("name")
                or pid),
            provider_type=manifest.get("_provider_type", "openai"),
            system_prompt=str(manifest.get("description") or ""),
            auto_agent=True,
            extension_ids=[ext_id] if ext_id else [],
            metadata={
                "source": "node",
                "participant_id": pid,
                "extension_ids": [ext_id] if ext_id else [],
            },
        ))

    def _invoke_node_chat_participant(
            self, participant_id: str, req: Any, ctx: Any,
            stream: Any, token: Any = None) -> Any:
        host = getattr(self, "_node_ext_host", None)
        if host is None or not host.is_running:
            raise RuntimeError("Node extension host is not running")
        prompt = str(getattr(req, "prompt", "") or "")
        request = {
            "prompt": prompt,
            "command": getattr(req, "command", ""),
            "references": getattr(req, "references", []),
            "toolReferences": getattr(req, "tool_references", []),
            "attempt": getattr(req, "attempt", 0),
            "enableCommandDetection": getattr(
                req, "enable_command_detection", True),
            "location": getattr(req, "location", 1),
            "acceptedConfirmationData": getattr(
                req, "accepted_confirmation_data", None),
        }
        response = host.request_chat_participant_result(
            participant_id, prompt, request=request)
        if not response.get("ok"):
            raise RuntimeError(
                response.get("error") or "Node chat participant failed")
        value = response.get("value")
        if isinstance(value, dict):
            content = value.get("content")
            if content:
                stream.markdown(str(content))
            return value.get("result")
        if value:
            stream.markdown(str(value))
        return None

    def _handle_node_tree_event(
            self, event: str, view_id: str, payload: Dict[str, Any]) -> None:
        """Bridge Node TreeDataProvider events into the Python VSCode API."""
        normalized_view_id = str(view_id or "")
        if not normalized_view_id:
            return
        try:
            from ai_editor.extension_host import (
                NodeTreeDataProvider, NodeTreeElement)
            if event == "tree_data_provider_registered":
                disposable = self._node_tree_disposables.pop(
                    normalized_view_id, None)
                if disposable is not None and hasattr(disposable, "dispose"):
                    disposable.dispose()
                host = self._node_ext_host
                if host is None:
                    return
                provider = NodeTreeDataProvider(host, normalized_view_id)
                self._node_tree_disposables[normalized_view_id] = (
                    self._vscode_ns._register_tree_data_provider(
                        normalized_view_id, provider))
            elif event == "tree_data_provider_disposed":
                disposable = self._node_tree_disposables.pop(
                    normalized_view_id, None)
                if disposable is not None and hasattr(disposable, "dispose"):
                    disposable.dispose()
            elif event == "tree_data_changed":
                provider = self._vscode_ns._tree_data_providers.get(
                    normalized_view_id)
                refresh = getattr(provider, "refresh", None)
                if callable(refresh):
                    element = payload.get("element")
                    refresh(NodeTreeElement(element) if isinstance(
                        element, dict) else None)
            elif event == "tree_view_reveal":
                element = payload.get("element")
                if not isinstance(element, dict):
                    return
                view = self._vscode_ns._tree_views.get(normalized_view_id)
                provider = self._vscode_ns._tree_data_providers.get(
                    normalized_view_id)
                if view is None and provider is not None:
                    view = self._vscode_ns._create_tree_view(
                        normalized_view_id, treeDataProvider=provider)
                reveal = getattr(view, "reveal", None)
                if callable(reveal):
                    reveal(
                        NodeTreeElement(element),
                        payload.get("options") if isinstance(
                            payload.get("options"), dict) else None)
        except Exception as exc:
            print(f"[NodeExtHost] Failed to bridge tree event "
                  f"{event}:{normalized_view_id}: {exc}")

    def _shutdown_node_extension_host(self) -> None:
        """Stop the Node extension host if running."""
        for disposable in list(self._node_tree_disposables.values()):
            try:
                dispose = getattr(disposable, "dispose", None)
                if callable(dispose):
                    dispose()
            except Exception:
                pass
        self._node_tree_disposables.clear()
        for record in list(self._node_lm_tool_disposables.values()):
            try:
                disposable = (
                    record.get("disposable")
                    if isinstance(record, dict) else record)
                dispose = getattr(disposable, "dispose", None)
                if callable(dispose):
                    dispose()
            except Exception:
                pass
        self._node_lm_tool_disposables.clear()
        for participant_id, disposable in list(
                self._node_chat_participant_disposables.items()):
            try:
                dispose = getattr(disposable, "dispose", None)
                if callable(dispose):
                    dispose()
            except Exception:
                pass
            registry = getattr(self, "_provider_registry", None)
            if registry is not None:
                registry.unregister(f"ext-{participant_id}")
        self._node_chat_participant_disposables.clear()
        host = self._node_ext_host
        if host is not None:
            try:
                host.set_lm_model_request_callback(None)
                host.set_lm_model_cancel_callback(None)
            except Exception:
                pass
            host.stop()
            self._node_ext_host = None
        self._vscode_ns.set_language_provider_request_callback(None)

    def relay_node_webview_message(self, view_id: str, message: Any) -> Dict:
        """Forward a webview message to the Node extension host."""
        host = self._node_ext_host
        if host is None or not host.is_running:
            return {"error": "Node extension host not running"}
        ok = host.relay_webview_message(view_id, message)
        return {"ok": ok, "view_id": view_id}

    def extension_quick_input_action(
            self, input_id: str, action: str,
            payload: Optional[Dict[str, Any]] = None) -> Dict:
        """Forward a visible QuickInput UI action to the Node extension host."""
        host = self._node_ext_host
        if host is None or not host.is_running:
            return {"error": "Node extension host not running"}
        ok = host.send_quick_input_action(input_id, action, payload or {})
        return {"ok": ok, "id": input_id, "action": action}

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
        # VSCode extension directories: only scan when the user has
        # explicitly configured an allowed_publishers list, otherwise every
        # installed extension (themes, language packs, linters, etc.) gets
        # picked up as phantom extension entries and webview tabs.
        ext_settings = self._extension_settings()
        allowed = [str(x).strip() for x in ext_settings.get("allowed_publishers", []) if str(x).strip()]
        if allowed:
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
        explicit = ext.get("enabled_contributions_explicit") is True
        if isinstance(raw, list):
            enabled = [str(x) for x in raw]
            if explicit:
                return enabled
            if not enabled:
                return list(
                    _AI_EDITOR_SECTION_DEFAULTS["extensions"][
                        "enabled_contributions"])
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

    @staticmethod
    def _normalize_extension_list_row(row: Dict[str, Any]) -> Dict[str, Any]:
        ext_id = str(row.get("id") or "").strip()
        ext_dir = str(
            row.get("ext_dir")
            or row.get("extensionPath")
            or row.get("extension_path")
            or ""
        ).strip()
        manifest_path = str(row.get("manifest_path") or "").strip()
        if not manifest_path and ext_dir:
            manifest_path = os.path.join(ext_dir, "package.json")
        contributes = row.get("contributes") or row.get("contributes_keys") or []
        if not isinstance(contributes, list):
            contributes = []
        return {
            "id": ext_id,
            "name": str(row.get("name") or (ext_id.split(".", 1)[-1] if ext_id else "")),
            "displayName": str(row.get("displayName") or row.get("display_name") or row.get("name") or ext_id),
            "description": str(row.get("description") or ""),
            "version": str(row.get("version") or ""),
            "publisher": str(row.get("publisher") or ""),
            "ext_dir": ext_dir,
            "manifest_path": manifest_path,
            "contributes": [str(item) for item in contributes if str(item)],
            "installed_at": float(row.get("installed_at") or 0),
            "activated": bool(row.get("activated")),
            "activationTimeMs": float(row.get("activationTimeMs") or 0),
            "isBuiltin": bool(row.get("isBuiltin")),
            "has_manifest": bool(row.get("has_manifest", True)),
            "source": str(row.get("source") or ("persisted" if row.get("installed_at") else "runtime")),
        }

    def _installed_extension_rows(self) -> List[Dict[str, Any]]:
        self._ensure_engine()
        rows_by_id: Dict[str, Dict[str, Any]] = {}
        try:
            from ai_editor.extensions import list_installed
            for item in list_installed():
                if not isinstance(item, dict):
                    continue
                normalized = self._normalize_extension_list_row(item)
                ext_id = normalized.get("id", "")
                if ext_id:
                    rows_by_id[ext_id.casefold()] = normalized
        except Exception:
            pass
        try:
            for item in self._ext_host.list_extensions():
                if not isinstance(item, dict):
                    continue
                normalized = self._normalize_extension_list_row(item)
                ext_id = normalized.get("id", "")
                if not ext_id:
                    continue
                key = ext_id.casefold()
                previous = rows_by_id.get(key, {})
                merged = dict(previous)
                merged.update({k: v for k, v in normalized.items() if v not in ("", [], 0, 0.0) or k in {"id", "name", "displayName", "source", "activated", "isBuiltin"}})
                if previous:
                    merged["source"] = previous.get("source", merged.get("source", "runtime"))
                    merged["installed_at"] = previous.get("installed_at", merged.get("installed_at", 0))
                    merged["has_manifest"] = previous.get("has_manifest", True) or merged.get("has_manifest", False)
                rows_by_id[key] = merged
        except Exception:
            pass
        rows = list(rows_by_id.values())
        rows.sort(key=lambda item: (
            -int(bool(item.get("installed_at"))),
            -float(item.get("installed_at") or 0),
            str(item.get("displayName") or item.get("name") or item.get("id") or "").casefold(),
        ))
        return rows

    def _installed_extension_id_set(self) -> set[str]:
        return {
            str(item.get("id") or "").casefold()
            for item in self._installed_extension_rows()
            if str(item.get("id") or "").strip()
        }

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

        node_host = getattr(self, "_node_ext_host", None)
        node_running = node_host is not None and getattr(node_host, "is_running", False)
        if node_running:
            support_tier = "full_node_host"
            support_label = "Full Node VS Code extension host active"
        else:
            from ai_editor.node_runtime import is_available as _node_available
            if _node_available():
                support_tier = "node_available"
                support_label = "Node.js available; host starts on demand when extensions need it"
            else:
                support_tier = "manifest_api_compatibility"
                support_label = "Manifest/API compatibility; install Node.js for full extension runtime"

        return {
            "support_tier": support_tier,
            "support_label": support_label,
            "node_host_enabled": node_running,
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

    def open_external_uri(self, uri: str) -> Dict:
        self._ensure_engine()
        uri_text = str(uri or "").strip()
        if not uri_text:
            return {"error": "URI is required"}
        ok = self._vscode_ns._open_external(uri_text)
        return {"ok": bool(ok)}

    def select_extension_tree_item(self, view_id: str, handle: str) -> Dict:
        """Select a runtime extension TreeView node by frontend snapshot handle."""
        self._ensure_engine()
        normalized_view_id = str(view_id or "")
        normalized_handle = str(handle or "")
        if not normalized_view_id or not normalized_handle:
            return {"error": "Tree view id and node handle are required"}
        view = self._vscode_ns._tree_views.get(normalized_view_id)
        if view is None:
            return {"error": f"Tree view not found: {normalized_view_id}"}
        if not view.select_handle(normalized_handle):
            return {
                "error": "Tree node handle is stale or unknown",
                "view_id": normalized_view_id,
                "handle": normalized_handle,
            }
        self._notify_node_tree_view_event(
            normalized_view_id, "selection", view, normalized_handle)
        snapshot = self._extension_view_snapshot(normalized_view_id)
        return {
            "ok": True,
            "view_id": normalized_view_id,
            "handle": normalized_handle,
            "selection": [str(item) for item in getattr(view, "selection", [])],
            "runtimeState": snapshot,
        }

    def load_extension_tree_children(self, view_id: str,
                                     handle: str = "") -> Dict:
        """Load direct children for a runtime extension TreeView node."""
        self._ensure_engine()
        normalized_view_id = str(view_id or "")
        normalized_handle = str(handle or "")
        if not normalized_view_id:
            return {"error": "Tree view id is required"}
        view = self._vscode_ns._tree_views.get(normalized_view_id)
        provider = self._vscode_ns._tree_data_providers.get(normalized_view_id)
        if view is None and provider is None:
            return {"error": f"Tree view not found: {normalized_view_id}"}
        if view is None:
            view = self._vscode_ns._create_tree_view(
                normalized_view_id, treeDataProvider=provider)
        if provider is None:
            provider = getattr(view, "provider", None)
        if provider is None:
            return {"error": f"Tree data provider not found: {normalized_view_id}"}
        parent = None
        seen = None
        if normalized_handle:
            lookup = getattr(view, "element_for_handle", None)
            parent = lookup(normalized_handle) if callable(lookup) else None
            if parent is None:
                return {
                    "error": "Tree node handle is stale or unknown",
                    "view_id": normalized_view_id,
                    "handle": normalized_handle,
                }
            seen = {id(parent)}
        view.begin_snapshot()
        errors: List[Dict[str, Any]] = []
        nodes = self._tree_view_nodes_preview(
            provider, parent, depth=0, seen=seen,
            tree_view=view, max_depth=0, errors=errors)
        if errors:
            error = errors[-1]
            return {
                "error": error.get("message", "Tree data provider failed"),
                "operation": error.get("operation", "getChildren"),
                "retryable": error.get("retryable", True),
                "view_id": normalized_view_id,
                "handle": normalized_handle,
                "nodes": nodes,
                "refreshVersion": getattr(view, "refresh_version", 0),
            }
        return {
            "ok": True,
            "view_id": normalized_view_id,
            "handle": normalized_handle,
            "nodes": nodes,
            "refreshVersion": getattr(view, "refresh_version", 0),
        }

    def set_extension_tree_item_expanded(
            self, view_id: str, handle: str, expanded: bool) -> Dict:
        """Report frontend TreeView expand/collapse state to extensions."""
        self._ensure_engine()
        normalized_view_id = str(view_id or "")
        normalized_handle = str(handle or "")
        if not normalized_view_id or not normalized_handle:
            return {"error": "Tree view id and node handle are required"}
        view = self._vscode_ns._tree_views.get(normalized_view_id)
        if view is None:
            return {"error": f"Tree view not found: {normalized_view_id}"}
        set_expanded = getattr(view, "set_expanded", None)
        if not callable(set_expanded) or not set_expanded(
                normalized_handle, bool(expanded)):
            return {
                "error": "Tree node handle is stale or unknown",
                "view_id": normalized_view_id,
                "handle": normalized_handle,
            }
        self._notify_node_tree_view_event(
            normalized_view_id, "expand" if expanded else "collapse",
            view, normalized_handle)
        return {
            "ok": True,
            "view_id": normalized_view_id,
            "handle": normalized_handle,
            "expanded": bool(expanded),
        }

    def execute_extension_tree_item_action(
            self, view_id: str, handle: str, command_id: str) -> Dict:
        """Execute a contributed TreeView item action with the item as argument."""
        self._ensure_engine()
        normalized_view_id = str(view_id or "")
        normalized_handle = str(handle or "")
        normalized_command = str(command_id or "")
        if not normalized_view_id or not normalized_handle or not normalized_command:
            return {"error": "Tree view id, node handle, and command id are required"}
        view = self._vscode_ns._tree_views.get(normalized_view_id)
        if view is None:
            return {"error": f"Tree view not found: {normalized_view_id}"}
        if not view.select_handle(normalized_handle):
            return {
                "error": "Tree node handle is stale or unknown",
                "view_id": normalized_view_id,
                "handle": normalized_handle,
            }
        self._notify_node_tree_view_event(
            normalized_view_id, "selection", view, normalized_handle)
        selection = list(getattr(view, "selection", []) or [])
        element = selection[0] if selection else None
        try:
            result = self._ext_host.commands.execute(normalized_command, element)
            payload: Dict[str, Any]
            if isinstance(result, dict):
                payload = dict(result)
            else:
                payload = {"result": result}
            payload.setdefault("ok", True)
            payload.update({
                "view_id": normalized_view_id,
                "command": normalized_command,
                "selection": [str(item) for item in selection],
            })
            return json.loads(json.dumps(payload, ensure_ascii=False, default=str))
        except KeyError:
            return {"error": f"Command not found: {normalized_command}"}
        except Exception as exc:
            return {"error": str(exc), "command": normalized_command}

    def _notify_node_tree_view_event(
            self, view_id: str, event: str, view: Any, handle: str) -> None:
        host = getattr(self, "_node_ext_host", None)
        if host is None or not getattr(host, "is_running", False):
            return
        provider = getattr(view, "provider", None)
        if provider is None:
            provider = self._vscode_ns._tree_data_providers.get(view_id)
        try:
            from ai_editor.extension_host import NodeTreeDataProvider
        except Exception:
            NodeTreeDataProvider = None
        if (provider is None
                or NodeTreeDataProvider is None
                or not isinstance(provider, NodeTreeDataProvider)):
            return
        lookup = getattr(view, "element_for_handle", None)
        element = lookup(handle) if callable(lookup) else None
        if element is None:
            return
        try:
            host.send_tree_view_event(
                view_id, event, element=element,
                selection=list(getattr(view, "selection", []) or []))
        except Exception:
            pass

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

    def get_extension_host_diagnostics(self, reset: bool = False) -> Dict:
        """Return lightweight Node extension host request diagnostics."""
        host = getattr(self, "_node_ext_host", None)
        if host is None:
            return {
                "enabled": self._extension_diagnostics_enabled(),
                "running": False,
                "activated": 0,
                "pending": {
                    "commands": 0,
                    "tree": 0,
                    "language": 0,
                    "customEditors": 0,
                },
                "categories": {},
            }
        if reset:
            host.reset_diagnostics()
        return host.diagnostics_snapshot()

    def set_extension_host_diagnostics(self, enabled: bool) -> Dict:
        """Persist and apply the default-off Node extension diagnostics flag."""
        settings = _resolve_settings(self._gui_ref)
        current = settings.get("ai_editor", {}) if settings else self.load_config()
        normalized = _normalize_ai_editor_config(current or {})
        ext_cfg = dict(normalized.get("extensions", {}) or {})
        ext_cfg["diagnostics_enabled"] = bool(enabled)
        merged = self._save_config_patch({"extensions": ext_cfg})
        host = getattr(self, "_node_ext_host", None)
        if host is not None:
            host.set_diagnostics_enabled(bool(enabled))
        return {
            "ok": True,
            "enabled": bool(enabled),
            "extensions": merged.get("extensions", {}),
            "diagnostics": self.get_extension_host_diagnostics(),
        }

    def resolve_extension_custom_editor(
            self, view_type: str, uri: str, title: str = "",
            timeout: float = 2.0) -> Dict:
        """Resolve a Node-registered custom editor into a dynamic webview."""
        host = getattr(self, "_node_ext_host", None)
        if host is None or not getattr(host, "is_running", False):
            return {
                "ok": False,
                "error": "Node extension host is not running",
            }
        try:
            timeout_value = float(timeout)
        except (TypeError, ValueError):
            timeout_value = 2.0
        timeout_value = max(0.5, min(timeout_value, 30.0))
        return host.request_custom_editor_result(
            view_type, uri, title=title, timeout=timeout_value)

    def custom_editor_state(
            self, view_id: str = "", view_type: str = "",
            uri: str = "") -> Dict:
        """Return a resolved extension custom editor state snapshot."""
        host = getattr(self, "_node_ext_host", None)
        if host is None or not getattr(host, "is_running", False):
            return {"ok": False, "error": "Node extension host is not running"}
        state = host.custom_editor_state(view_id, view_type, uri)
        if not state:
            return {"ok": False, "error": "Custom editor state not found"}
        state["ok"] = True
        return state

    def list_custom_editor_states(self) -> Dict:
        """Return state snapshots for Node extension custom editors."""
        host = getattr(self, "_node_ext_host", None)
        if host is None or not getattr(host, "is_running", False):
            return {"ok": False, "states": [],
                    "error": "Node extension host is not running"}
        return {"ok": True, "states": host.list_custom_editor_states()}

    def _extension_custom_editor_lifecycle(
            self, action: str, view_id: str = "", view_type: str = "",
            uri: str = "", target: str = "", timeout: float = 5.0) -> Dict:
        host = getattr(self, "_node_ext_host", None)
        if host is None or not getattr(host, "is_running", False):
            return {"ok": False, "error": "Node extension host is not running"}
        try:
            timeout_value = float(timeout)
        except (TypeError, ValueError):
            timeout_value = 5.0
        timeout_value = max(0.5, min(timeout_value, 30.0))
        return host.request_custom_editor_lifecycle(
            action, view_type=view_type, uri=uri, view_id=view_id,
            target=target, timeout=timeout_value)

    def save_extension_custom_editor(
            self, view_id: str = "", view_type: str = "",
            uri: str = "") -> Dict:
        """Run saveCustomDocument for a Node extension custom editor."""
        return self._extension_custom_editor_lifecycle(
            "save", view_id=view_id, view_type=view_type, uri=uri)

    def revert_extension_custom_editor(
            self, view_id: str = "", view_type: str = "",
            uri: str = "") -> Dict:
        """Run revertCustomDocument for a Node extension custom editor."""
        return self._extension_custom_editor_lifecycle(
            "revert", view_id=view_id, view_type=view_type, uri=uri)

    def backup_extension_custom_editor(
            self, view_id: str = "", view_type: str = "",
            uri: str = "") -> Dict:
        """Run backupCustomDocument for a Node extension custom editor."""
        return self._extension_custom_editor_lifecycle(
            "backup", view_id=view_id, view_type=view_type, uri=uri)

    def undo_extension_custom_editor(
            self, view_id: str = "", view_type: str = "",
            uri: str = "") -> Dict:
        """Run the latest custom editor edit undo callback."""
        return self._extension_custom_editor_lifecycle(
            "undo", view_id=view_id, view_type=view_type, uri=uri)

    def redo_extension_custom_editor(
            self, view_id: str = "", view_type: str = "",
            uri: str = "") -> Dict:
        """Run the next custom editor edit redo callback."""
        return self._extension_custom_editor_lifecycle(
            "redo", view_id=view_id, view_type=view_type, uri=uri)

    def save_extension_custom_editor_as(
            self, view_id: str = "", view_type: str = "",
            uri: str = "", target: str = "") -> Dict:
        """Run saveCustomDocumentAs for a Node extension custom editor."""
        if not str(target or "").strip():
            return {"ok": False, "error": "Custom editor save target is required"}
        return self._extension_custom_editor_lifecycle(
            "saveAs", view_id=view_id, view_type=view_type,
            uri=uri, target=target)

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

    def list_extension_settings(self) -> Dict:
        """Return all extension configuration contributions with current values."""
        self._ensure_engine()
        contributions = self._ext_host.ext_points.configuration_contributions
        default_overrides = self._extension_configuration_default_overrides()
        result: List[Dict[str, Any]] = []
        for entry in contributions:
            eid = entry.get("extension_id", "")
            ext = self._ext_host.registry.get(eid)
            display_name = ext.display_name if ext else eid
            ctx = self._ext_host.activator.get_context(eid)
            ws_state = ctx.workspace_state if ctx else None
            props = entry.get("properties", {})
            values: Dict[str, Any] = {}
            defaults: Dict[str, Any] = {}
            configured_values: Dict[str, Any] = {}
            modified: Dict[str, bool] = {}
            configured_keys = set(ws_state.keys()) if ws_state else set()
            for key, schema in props.items():
                if not isinstance(schema, dict):
                    continue
                has_default = key in default_overrides or "default" in schema
                if has_default:
                    defaults[key] = (
                        default_overrides[key]
                        if key in default_overrides else schema["default"])
                if key in configured_keys:
                    stored = ws_state.get(key) if ws_state else None
                    values[key] = stored
                    configured_values[key] = stored
                    modified[key] = True
                elif has_default:
                    values[key] = defaults[key]
                    modified[key] = False
                else:
                    modified[key] = False
            result.append({
                "extension_id": eid,
                "display_name": display_name,
                "title": entry.get("title", ""),
                "properties": props,
                "values": values,
                "defaults": defaults,
                "configuredValues": configured_values,
                "modified": modified,
            })
        return {"configurations": result}

    def get_extension_setting(self, key: str, default: Any = None) -> Dict:
        """Read a single extension setting value from workspace state."""
        self._ensure_engine()
        contributions = self._ext_host.ext_points.configuration_contributions
        default_overrides = self._extension_configuration_default_overrides()
        for entry in contributions:
            props = entry.get("properties", {})
            if key in props:
                eid = entry.get("extension_id", "")
                ctx = self._ext_host.activator.get_context(eid)
                if ctx:
                    keys = set(ctx.workspace_state.keys())
                    if key in keys:
                        stored = ctx.workspace_state.get(key)
                        return {"ok": True, "key": key, "value": stored}
                if key in default_overrides:
                    return {"ok": True, "key": key,
                            "value": default_overrides[key]}
                schema = props[key] if isinstance(props[key], dict) else {}
                if "default" in schema:
                    return {"ok": True, "key": key,
                            "value": schema["default"]}
                return {"ok": True, "key": key,
                        "value": default}
        return {"ok": True, "key": key, "value": default}

    def set_extension_setting(self, key: str, value: Any) -> Dict:
        """Write a single extension setting value and persist."""
        self._ensure_engine()
        contributions = self._ext_host.ext_points.configuration_contributions
        for entry in contributions:
            props = entry.get("properties", {})
            if key in props:
                eid = entry.get("extension_id", "")
                ctx = self._ext_host.activator.get_context(eid)
                if not ctx:
                    return {"error": f"Extension context not found: {eid}"}
                ctx.workspace_state.update(key, value)
                self._notify_extension_setting_changed(key, value)
                return {"ok": True, "key": key, "extension_id": eid}
        return {"error": f"Setting key not found in any extension: {key}"}

    def reset_extension_setting(self, key: str) -> Dict:
        """Remove a workspace override for an extension setting."""
        self._ensure_engine()
        contributions = self._ext_host.ext_points.configuration_contributions
        default_overrides = self._extension_configuration_default_overrides()
        for entry in contributions:
            props = entry.get("properties", {})
            if key in props:
                eid = entry.get("extension_id", "")
                ctx = self._ext_host.activator.get_context(eid)
                if not ctx:
                    return {"error": f"Extension context not found: {eid}"}
                ctx.workspace_state.delete(key)
                schema = props[key] if isinstance(props[key], dict) else {}
                default_value = (
                    default_overrides[key]
                    if key in default_overrides else schema.get("default"))
                self._notify_extension_setting_changed(
                    key, default_value, remove=True)
                return {
                    "ok": True,
                    "key": key,
                    "extension_id": eid,
                    "value": default_value,
                    "modified": False,
                }
        return {"error": f"Setting key not found in any extension: {key}"}

    def _extension_configuration_default_overrides(self) -> Dict[str, Any]:
        try:
            entries = self._ext_host.ext_points.all_contributions.get(
                "configurationDefaults", [])
        except Exception:
            entries = []
        defaults: Dict[str, Any] = {}
        for entry in entries:
            if not isinstance(entry, dict):
                continue
            for key, value in entry.items():
                key_str = str(key or "")
                if not key_str or key_str == "_extensionId":
                    continue
                if key_str.startswith("["):
                    continue
                defaults[key_str] = value
        return defaults

    def _notify_extension_setting_changed(
            self, key: str, value: Any,
            remove: bool = False) -> None:
        section = key.rsplit(".", 1)[0] if "." in key else ""
        short_key = key.rsplit(".", 1)[-1] if "." in key else key
        host = getattr(self, "_node_ext_host", None)
        if host is not None and host.is_running:
            try:
                host.send_settings_changed(
                    section, short_key, value, remove=remove)
            except Exception:
                pass

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
        view_id = str(view.get("id") or "")
        snapshot = self._extension_view_snapshot(view_id)
        view.update({
            "runtimeAvailable": bool(snapshot.get("runtimeAvailable")),
            "runtimeKind": snapshot.get("kind") or "view",
            "runtimeMessage": snapshot.get("message", ""),
            "titleActions": self._view_title_actions(view_id),
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
        if tree_provider is None and tree_view is not None:
            tree_provider = getattr(tree_view, "provider", None)
        if tree_provider and tree_view is None:
            tree_view = self._vscode_ns._create_tree_view(
                view_id, treeDataProvider=tree_provider)
        if tree_provider or tree_view:
            if tree_view is not None and hasattr(tree_view, "begin_snapshot"):
                tree_view.begin_snapshot()
            errors: List[Dict[str, Any]] = []
            nodes = self._tree_view_nodes_preview(
                tree_provider, tree_view=tree_view, max_depth=0,
                errors=errors)
            state = {
                "ok": True,
                "kind": "treeView",
                "runtimeAvailable": True,
                "message": "Runtime tree view provider registered.",
                "title": getattr(tree_view, "title", view_id),
                "titleActions": self._view_title_actions(view_id),
                "selection": [
                    str(item)
                    for item in list(getattr(tree_view, "selection", []) or [])
                ],
                "refreshVersion": getattr(tree_view, "refresh_version", 0),
                "children": [
                    str(node.get("label", ""))
                    for node in nodes[:20]
                ],
                "nodes": nodes,
            }
            if errors:
                state["treeError"] = errors[-1]
            return state
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
                "titleActions": self._view_title_actions(view_id),
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
                "titleActions": self._view_title_actions(view_id),
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

    def _view_title_actions(self, view_id: str) -> List[Dict[str, Any]]:
        return self._extension_menu_actions("view/title", {
            "view": str(view_id or ""),
        })

    def _view_item_actions(
            self, view_id: str, context_value: Any) -> List[Dict[str, Any]]:
        return self._extension_menu_actions("view/item/context", {
            "view": str(view_id or ""),
            "viewItem": "" if context_value is None else str(context_value),
        })

    def _extension_menu_actions(
            self, menu_id: str, context: Dict[str, str]) -> List[Dict[str, Any]]:
        menus = self._ext_host.ext_points.all_contributions.get("menus", {})
        if not isinstance(menus, dict):
            return []
        result: List[Dict[str, Any]] = []
        for item in menus.get(menu_id, []):
            if not isinstance(item, dict):
                continue
            if not self._extension_when_matches(item.get("when", ""), context):
                continue
            action = self._extension_menu_action_preview(item, context)
            if action:
                result.append(action)
        result.sort(key=lambda action: (
            str(action.get("group", "")),
            float(action.get("order", 0.0)),
            str(action.get("title", "")).lower(),
        ))
        return result

    def _extension_menu_action_preview(
            self, item: Dict[str, Any],
            context: Dict[str, str]) -> Dict[str, Any]:
        command_id = str(item.get("command") or "")
        if not command_id:
            return {}
        command_record = self._ext_host.ext_points.get_command_contribution(
            command_id)
        title = (
            item.get("title")
            or command_record.get("title")
            or item.get("alt")
            or command_id
        )
        return {
            "command": command_id,
            "title": str(title),
            "shortTitle": str(
                item.get("shortTitle")
                or command_record.get("shortTitle")
                or title),
            "icon": self._extension_action_icon(
                item.get("icon") or command_record.get("icon")),
            "extension_id": str(item.get("_extensionId", "")),
            "group": str(item.get("group", "")),
            "order": self._extension_menu_order(item.get("group", "")),
            "when": str(item.get("when", "")),
            "view": context.get("view", ""),
            "viewItem": context.get("viewItem", ""),
        }

    @staticmethod
    def _extension_menu_order(group: Any) -> float:
        text = str(group or "")
        if "@" not in text:
            return 0.0
        try:
            return float(text.rsplit("@", 1)[1])
        except Exception:
            return 0.0

    @staticmethod
    def _extension_action_icon(icon: Any) -> str:
        if not icon:
            return ""
        if isinstance(icon, str):
            return icon
        if isinstance(icon, dict):
            for key in ("id", "light", "dark"):
                value = icon.get(key)
                if value:
                    return str(value)
        icon_id = getattr(icon, "id", "")
        return str(icon_id or "")

    @classmethod
    def _extension_when_matches(
            cls, when: Any, context: Dict[str, str]) -> bool:
        expr = str(when or "").strip()
        if not expr:
            return True
        # Common view menu clauses use simple disjunctions/conjunctions. Be
        # conservative for unknown contexts so actions do not leak to views
        # where VS Code would hide them.
        for or_part in re.split(r"\s*\|\|\s*", expr):
            clauses = [
                clause.strip()
                for clause in re.split(r"\s*&&\s*", or_part)
                if clause.strip()
            ]
            if clauses and all(
                    cls._extension_when_clause_matches(clause, context)
                    for clause in clauses):
                return True
        return False

    @classmethod
    def _extension_when_clause_matches(
            cls, clause: str, context: Dict[str, str]) -> bool:
        clause = clause.strip()
        while clause.startswith("(") and clause.endswith(")"):
            clause = clause[1:-1].strip()
        negated = clause.startswith("!")
        if negated:
            clause = clause[1:].strip()
        regex_match = re.match(
            r"^(view|viewItem)\s*=~\s*/(.+)/(i)?$", clause)
        if regex_match:
            key, pattern, flags = regex_match.groups()
            try:
                matched = re.search(
                    pattern,
                    context.get(key, ""),
                    re.IGNORECASE if flags else 0) is not None
            except re.error:
                matched = False
            return not matched if negated else matched
        compare_match = re.match(
            r"^(view|viewItem)\s*(==|!=|===|!==)\s*(.+)$", clause)
        if compare_match:
            key, op, raw_expected = compare_match.groups()
            expected = cls._strip_when_value(raw_expected)
            actual = context.get(key, "")
            matched = actual == expected
            if op in ("!=", "!=="):
                matched = not matched
            return not matched if negated else matched
        if clause in {"view", "viewItem"}:
            matched = bool(context.get(clause, ""))
            return not matched if negated else matched
        # Unknown context keys should not make an action visible.
        return bool(negated)

    @staticmethod
    def _strip_when_value(value: str) -> str:
        value = value.strip()
        if ((value.startswith("'") and value.endswith("'"))
                or (value.startswith('"') and value.endswith('"'))):
            return value[1:-1]
        return value

    @staticmethod
    def _append_tree_provider_error(
            errors: Optional[List[Dict[str, Any]]],
            operation: str, message: Any) -> None:
        if errors is None:
            return
        text = str(message or "").strip()
        if not text:
            return
        errors.append({
            "operation": operation,
            "message": text,
            "retryable": True,
        })

    @classmethod
    def _append_last_tree_provider_error(
            cls, provider: Any, errors: Optional[List[Dict[str, Any]]],
            operation: str) -> None:
        cls._append_tree_provider_error(
            errors, operation, getattr(provider, "last_error", ""))

    @staticmethod
    def _tree_view_children_preview(provider: Any) -> List[str]:
        if provider is None or not hasattr(provider, "getChildren"):
            return []
        try:
            children = _resolve_vscode_provider_result(
                provider.getChildren(None), default=[])
        except TypeError:
            children = _resolve_vscode_provider_result(
                provider.getChildren(), default=[])
        except Exception:
            return []
        if not isinstance(children, list):
            try:
                children = list(children)
            except Exception:
                return []
        return [str(child) for child in children[:20]]

    def _tree_view_nodes_preview(self, provider: Any, element: Any = None,
                                 depth: int = 0,
                                 seen: Optional[set] = None,
                                 tree_view: Any = None,
                                 max_depth: int = 2,
                                 errors: Optional[
                                     List[Dict[str, Any]]] = None
                                 ) -> List[Dict[str, Any]]:
        if provider is None or not hasattr(provider, "getChildren"):
            return []
        if seen is None:
            seen = set()
        if depth > max_depth:
            return []
        try:
            children = _resolve_vscode_provider_result(
                provider.getChildren(element), default=[])
        except TypeError as exc:
            if element is None:
                try:
                    children = _resolve_vscode_provider_result(
                        provider.getChildren(), default=[])
                except Exception as exc:
                    self._append_tree_provider_error(
                        errors, "getChildren", exc)
                    return []
            else:
                self._append_tree_provider_error(errors, "getChildren", exc)
                return []
        except Exception as exc:
            self._append_tree_provider_error(errors, "getChildren", exc)
            return []
        self._append_last_tree_provider_error(provider, errors, "getChildren")
        if not isinstance(children, list):
            try:
                children = list(children)
            except Exception as exc:
                self._append_tree_provider_error(
                    errors, "getChildren", exc)
                return []
        nodes: List[Dict[str, Any]] = []
        for child in children[:30]:
            marker = id(child)
            if marker in seen:
                continue
            child_seen = set(seen)
            child_seen.add(marker)
            item = self._tree_item_for_element(provider, child, errors=errors)
            node = self._tree_node_preview(
                child, item, tree_view=tree_view, view_id=getattr(tree_view, "id", ""))
            force_reveal_children = False
            reveal_ancestor = getattr(tree_view, "is_reveal_ancestor", None)
            if callable(reveal_ancestor):
                try:
                    force_reveal_children = bool(reveal_ancestor(child))
                except Exception:
                    force_reveal_children = False
            if force_reveal_children:
                node["revealAncestor"] = True
            reveal_expand_remaining = 0
            reveal_expand_level = getattr(tree_view, "reveal_expand_level", None)
            if callable(reveal_expand_level):
                try:
                    reveal_expand_remaining = int(
                        reveal_expand_level(child) or 0)
                except Exception:
                    reveal_expand_remaining = 0
            if reveal_expand_remaining > 0:
                node["revealExpand"] = reveal_expand_remaining
            child_nodes: List[Dict[str, Any]] = []
            attempted_children = False
            if force_reveal_children or reveal_expand_remaining > 0 or (
                    depth < max_depth
                    and (node.get("collapsibleState", 0) or depth < 1)):
                attempted_children = True
                child_max_depth = (
                    max(max_depth, depth + 1)
                    if force_reveal_children else max_depth)
                if reveal_expand_remaining > 0:
                    child_max_depth = max(
                        child_max_depth, depth + reveal_expand_remaining)
                child_nodes = self._tree_view_nodes_preview(
                    provider, child, depth + 1, child_seen,
                    tree_view=tree_view, max_depth=child_max_depth,
                    errors=errors)
                if child_nodes and not node.get("collapsibleState", 0):
                    node["collapsibleState"] = 2 if force_reveal_children else 1
                elif force_reveal_children and node.get("collapsibleState", 0):
                    node["collapsibleState"] = 2
            node["children"] = child_nodes
            node["childrenLoaded"] = attempted_children or not bool(
                node.get("collapsibleState", 0))
            node["lazyChildren"] = bool(
                node.get("collapsibleState", 0)) and not attempted_children
            nodes.append(node)
        return nodes

    def _tree_item_for_element(self, provider: Any, element: Any,
                               errors: Optional[
                                   List[Dict[str, Any]]] = None) -> Any:
        if provider is None or not hasattr(provider, "getTreeItem"):
            return element
        try:
            item = _resolve_vscode_provider_result(
                provider.getTreeItem(element), default=None)
            self._append_last_tree_provider_error(
                provider, errors, "getTreeItem")
            return item if item is not None else element
        except Exception as exc:
            self._append_tree_provider_error(errors, "getTreeItem", exc)
            return element

    def _tree_node_preview(self, element: Any, item: Any,
                           tree_view: Any = None,
                           view_id: str = "") -> Dict[str, Any]:
        label = self._tree_value(item, "label")
        if isinstance(label, dict):
            label = label.get("label") or label.get("text") or ""
        if not label:
            label = str(element)
        description = self._tree_value(item, "description")
        tooltip = self._tree_value(item, "tooltip")
        resource_uri = self._tree_uri_preview(
            self._tree_value(item, "resourceUri"))
        collapsible = self._tree_value(item, "collapsibleState", 0)
        try:
            collapsible_state = int(collapsible or 0)
        except Exception:
            collapsible_state = 0
        command = self._tree_command_preview(self._tree_value(item, "command"))
        raw_icon = self._tree_value(item, "iconPath")
        icon_path = self._tree_icon_path_preview(raw_icon)
        icon = self._tree_icon_preview(raw_icon, resource_uri, collapsible_state)
        context_value = self._tree_value(item, "contextValue")
        node = {
            "label": str(label),
            "description": self._tree_description_preview(description),
            "descriptionIsDerived": description is True,
            "tooltip": self._tree_text_preview(tooltip),
            "collapsibleState": collapsible_state,
            "command": command,
            "icon": icon,
            "contextValue": "" if context_value is None else str(context_value),
        }
        if resource_uri:
            node["resourceUri"] = resource_uri
        if icon_path:
            node["iconPath"] = icon_path
            if icon_path.get("kind") == "theme":
                node["themeIcon"] = dict(icon_path)
        checkbox = self._tree_checkbox_preview(
            self._tree_value(item, "checkboxState"))
        if checkbox:
            node["checkbox"] = checkbox
        accessibility = self._tree_accessibility_preview(
            self._tree_value(item, "accessibilityInformation"))
        if accessibility:
            node["accessibilityInformation"] = accessibility
        node["actions"] = self._view_item_actions(
            view_id, node.get("contextValue", ""))
        if tree_view is not None:
            remember = getattr(tree_view, "remember_element", None)
            if callable(remember):
                try:
                    node["handle"] = str(remember(element))
                except Exception:
                    node["handle"] = ""
            selected = getattr(tree_view, "is_selected", None)
            if callable(selected):
                try:
                    node["selected"] = bool(selected(element))
                except Exception:
                    node["selected"] = False
            revealed = getattr(tree_view, "is_revealed", None)
            if callable(revealed):
                try:
                    if bool(revealed(element)):
                        node["revealed"] = True
                        node["revealVersion"] = int(
                            getattr(tree_view, "reveal_version", 0) or 0)
                except Exception:
                    pass
            focused = getattr(tree_view, "is_focused", None)
            if callable(focused):
                try:
                    if bool(focused(element)):
                        node["focused"] = True
                except Exception:
                    pass
        return node

    @staticmethod
    def _tree_value(item: Any, key: str, default: Any = None) -> Any:
        if isinstance(item, dict):
            return item.get(key, default)
        return getattr(item, key, default)

    @staticmethod
    def _tree_command_preview(command: Any) -> Dict[str, Any]:
        if not command:
            return {}
        if isinstance(command, dict):
            command_id = command.get("command") or command.get("id") or ""
            return {
                "command": str(command_id),
                "title": str(command.get("title") or command_id),
                "arguments": AIEditorAPI._tree_command_arguments(
                    command.get("arguments")),
            }
        command_id = getattr(command, "command", "") or getattr(command, "id", "")
        if not command_id:
            return {}
        return {
            "command": str(command_id),
            "title": str(getattr(command, "title", command_id) or command_id),
            "arguments": AIEditorAPI._tree_command_arguments(
                getattr(command, "arguments", [])),
        }

    @staticmethod
    def _tree_command_arguments(arguments: Any) -> List[Any]:
        if arguments is None:
            return []
        if isinstance(arguments, list):
            return list(arguments)
        if isinstance(arguments, tuple):
            return list(arguments)
        return [arguments]

    @staticmethod
    def _tree_text_preview(value: Any) -> str:
        if value is None:
            return ""
        if isinstance(value, str):
            return value
        if isinstance(value, dict):
            markdown = value.get("value")
            if markdown is not None:
                return str(markdown)
            label = value.get("label") or value.get("text")
            if label is not None:
                return str(label)
        markdown = getattr(value, "value", None)
        if markdown is not None:
            return str(markdown)
        return str(value)

    @staticmethod
    def _tree_description_preview(value: Any) -> str:
        if value is None or isinstance(value, bool):
            return ""
        return AIEditorAPI._tree_text_preview(value)

    @staticmethod
    def _tree_uri_preview(uri: Any) -> str:
        if not uri:
            return ""
        if isinstance(uri, str):
            return uri
        if isinstance(uri, dict):
            raw = uri.get("uri") or uri.get("external") or uri.get("fsPath")
            if raw:
                return str(raw)
            scheme = uri.get("scheme")
            path = uri.get("path")
            if scheme and path is not None:
                authority = str(uri.get("authority") or "")
                path_text = str(path)
                query = str(uri.get("query") or "")
                fragment = str(uri.get("fragment") or "")
                if scheme == "file":
                    if path_text and not path_text.startswith("/"):
                        path_text = "/" + path_text
                    result = f"file://{authority}{path_text}"
                    if query:
                        result += f"?{query}"
                    if fragment:
                        result += f"#{fragment}"
                    return result
                if authority:
                    result = f"{scheme}://{authority}{path_text}"
                else:
                    result = f"{scheme}:{path_text}"
                if query:
                    result += f"?{query}"
                if fragment:
                    result += f"#{fragment}"
                return result
        to_string = getattr(uri, "to_string", None)
        if callable(to_string):
            try:
                return str(to_string())
            except Exception:
                pass
        js_to_string = getattr(uri, "toString", None)
        if callable(js_to_string):
            try:
                return str(js_to_string())
            except Exception:
                pass
        return str(uri)

    @staticmethod
    def _tree_icon_path_preview(icon: Any) -> Dict[str, str]:
        if not icon:
            return {}
        if isinstance(icon, str):
            if icon.startswith("$(") and icon.endswith(")"):
                return {"kind": "theme", "id": icon[2:-1]}
            return {"kind": "path", "path": icon}
        if isinstance(icon, dict):
            icon_id = icon.get("id")
            if icon_id:
                result = {"kind": "theme", "id": str(icon_id)}
                color = AIEditorAPI._tree_icon_color_preview(icon.get("color"))
                if color:
                    result["color"] = color
                return result
            result = {}
            for key in ("light", "dark"):
                value = icon.get(key)
                if value:
                    result[key] = AIEditorAPI._tree_uri_preview(value)
            if result:
                result["kind"] = "themedPath"
                return result
            path = icon.get("path") or icon.get("uri")
            if path:
                return {
                    "kind": "path",
                    "path": AIEditorAPI._tree_uri_preview(path),
                }
        icon_id = getattr(icon, "id", "")
        if icon_id:
            result = {"kind": "theme", "id": str(icon_id)}
            color = AIEditorAPI._tree_icon_color_preview(
                getattr(icon, "color", None))
            if color:
                result["color"] = color
            return result
        return {}

    @staticmethod
    def _tree_icon_color_preview(color: Any) -> str:
        if not color:
            return ""
        if isinstance(color, str):
            return color
        if isinstance(color, dict):
            return str(color.get("id") or color.get("value") or "")
        return str(getattr(color, "id", "") or "")

    @staticmethod
    def _tree_icon_preview(icon: Any, resource_uri: str = "",
                           collapsible_state: int = 0) -> str:
        icon_path = AIEditorAPI._tree_icon_path_preview(icon)
        if icon_path.get("kind") == "theme":
            return f"$({icon_path.get('id', '')})"
        raw_path = (
            icon_path.get("path")
            or icon_path.get("light")
            or icon_path.get("dark"))
        if raw_path:
            name = os.path.basename(str(raw_path).replace("\\", "/")).lower()
            if "folder" in name:
                return "$(folder)"
            if "file" in name:
                return "$(file)"
            return "$(symbol-file)"
        if resource_uri:
            return "$(file)"
        if collapsible_state:
            return "$(folder)"
        return ""

    @staticmethod
    def _tree_checkbox_preview(value: Any) -> Dict[str, Any]:
        if value is None:
            return {}
        if isinstance(value, (int, float)):
            return {"isChecked": int(value) == 1}
        if isinstance(value, dict):
            state = value.get("state", value.get("checkboxState"))
            if state is None:
                state = value.get("checked")
            try:
                checked = (
                    bool(state) if isinstance(state, bool)
                    else int(state or 0) == 1)
            except Exception:
                checked = str(state).strip().lower() in {
                    "1", "true", "checked", "yes"}
            result: Dict[str, Any] = {"isChecked": checked}
            tooltip = AIEditorAPI._tree_text_preview(value.get("tooltip"))
            if tooltip:
                result["tooltip"] = tooltip
            accessibility = AIEditorAPI._tree_accessibility_preview(
                value.get("accessibilityInformation"))
            if accessibility:
                result["accessibilityInformation"] = accessibility
            return result
        state = getattr(value, "state", value)
        try:
            checked = int(state or 0) == 1
        except Exception:
            checked = bool(state)
        result = {"isChecked": checked}
        tooltip = AIEditorAPI._tree_text_preview(getattr(value, "tooltip", ""))
        if tooltip:
            result["tooltip"] = tooltip
        accessibility = AIEditorAPI._tree_accessibility_preview(
            getattr(value, "accessibilityInformation", None))
        if accessibility:
            result["accessibilityInformation"] = accessibility
        return result

    @staticmethod
    def _tree_accessibility_preview(value: Any) -> Dict[str, str]:
        if not value:
            return {}
        if isinstance(value, dict):
            label = value.get("label")
            role = value.get("role")
        else:
            label = getattr(value, "label", "")
            role = getattr(value, "role", "")
        result: Dict[str, str] = {}
        if label:
            result["label"] = str(label)
        if role:
            result["role"] = str(role)
        return result

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
            self._try_start_node_extension_host()
            self._activate_workspace_contains_extensions()
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

    def list_extension_activity_bar_items(self) -> Dict:
        """Return activity bar view containers contributed by extensions."""
        self._ensure_engine()
        items: List[Dict[str, Any]] = []
        try:
            for vc in self._ext_host.ext_points.activity_bar_items:
                vc_id = str(vc.get("id", ""))
                if not vc_id:
                    continue
                ext_id = str(vc.get("_extensionId", ""))
                title = str(vc.get("title", vc_id))
                raw_views = self._ext_host.ext_points.all_contributions.get(
                    "views", {}).get(vc_id, [])
                views = [
                    self._decorate_extension_view(view)
                    for view in raw_views
                    if isinstance(view, dict)
                ]
                # Resolve icon: use extension's emoji-style icon or default
                icon_text = ""
                ext_desc = self._ext_host.registry.get(ext_id)
                if ext_desc and ext_desc.icon:
                    raw_icon = ext_desc.icon.strip()
                    # If it looks like an emoji or short text icon, use it
                    if len(raw_icon) <= 4 and not raw_icon.endswith((".svg", ".png")):
                        icon_text = raw_icon
                if not icon_text:
                    icon_text = "▣"  # default: ▣
                items.append({
                    "id": vc_id,
                    "title": title,
                    "icon_text": icon_text,
                    "extension_id": ext_id,
                    "views": views,
                    "view_count": len(views),
                })
        except Exception:
            pass
        return {"items": items}

    def list_editor_title_actions(self) -> Dict:
        """Return editor title bar actions contributed by extensions.

        VSCode extensions declare these in contributes.menus["editor/title"].
        Each action has a command, icon, and optional when-clause.
        """
        self._ensure_engine()
        actions: List[Dict[str, Any]] = []
        try:
            ep = self._ext_host.ext_points
            for item in getattr(ep, "editor_title_actions", []):
                cmd_id = str(item.get("command", ""))
                if not cmd_id:
                    continue
                ext_id = str(item.get("_extensionId", ""))
                ext_desc = self._ext_host.registry.get(ext_id)
                icon = ""
                if ext_desc and ext_desc.icon:
                    raw = ext_desc.icon.strip()
                    if len(raw) <= 4 and not raw.endswith((".svg", ".png")):
                        icon = raw
                title = str(item.get("title", ""))
                if not title:
                    cmd_info = self._ext_host.commands.get_info(cmd_id) if hasattr(
                        self._ext_host.commands, "get_info") else None
                    title = str(cmd_info.get("title", cmd_id)) if cmd_info else cmd_id
                actions.append({
                    "command": cmd_id,
                    "title": title,
                    "icon": icon or "▣",
                    "extension_id": ext_id,
                    "group": str(item.get("group", "")),
                })
        except Exception:
            pass
        return {"actions": actions}

    # ── Extension marketplace API ──

    def search_extensions(self, query: str = "ai chat model", page: int = 1) -> Dict:
        """Search VSCode Marketplace. Returns list of extensions."""
        try:
            from ai_editor.extensions import search_extensions
            results = search_extensions(query, page=page)
            installed_ids = self._installed_extension_id_set()
            for r in results:
                if isinstance(r, dict) and "id" in r:
                    r["installed"] = str(r["id"]).casefold() in installed_ids
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
                        node_host.register_extensions([desc])
                        node_host.activate(
                            desc.extension_path,
                            desc.id,
                            node_host.extension_manifest(desc),
                        )
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
            return {"extensions": self._installed_extension_rows()}
        except Exception as exc:
            return {"error": str(exc)}

    def get_extension_detail(self, publisher: str, name: str) -> Dict:
        """Fetch a single extension detail from marketplace."""
        try:
            from ai_editor.extensions import get_extension_detail
            result = get_extension_detail(publisher, name)
            if result:
                result["installed"] = (
                    str(result.get("id") or "").casefold()
                    in self._installed_extension_id_set())
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
        from config import get_main_executable
        cmd = [get_main_executable(), '--ai-editor']
    else:
        cmd = [sys.executable, '-m', 'ai_editor.app']
    env = dict(os.environ)
    env.setdefault('PYTHONPATH', _ROOT)
    env.setdefault('PYTHONUNBUFFERED', '1')
    if getattr(sys, 'frozen', False):
        env['PYWEBVIEW_GUI'] = 'edgechromium'
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
            api._stop_webview_resource_server()

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
