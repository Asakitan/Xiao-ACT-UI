# -*- coding: utf-8 -*-
"""SAO AI Editor — MCP Server.

Exposes the AI Editor's full capabilities via the Model Context Protocol,
allowing IDE AI assistants (Claude Code, Copilot, etc.) to use the editor
as a tool server.

Usage:
    python -m ai_editor.mcp_server              # stdio server (default)
    python -m ai_editor.mcp_server --port 9820   # Streamable HTTP server

IDE configuration examples:

    # Claude Code (~/.claude.json or project mcp.json)
    {"mcpServers": {"sao-ai-editor": {
        "command": "python",
        "args": ["-m", "ai_editor.mcp_server"],
        "cwd": "<path-to-sao_auto/python>"
    }}}

    # VS Code (.vscode/mcp.json)
    {"servers": {"sao-ai-editor": {
        "type": "stdio",
        "command": "python",
        "args": ["-m", "ai_editor.mcp_server"]
    }}}
"""

from __future__ import annotations

import json
import os
import sys
import threading
import time
import traceback
from typing import Any, Callable, Dict, List, Optional
from urllib.parse import urlsplit

_HERE = os.path.dirname(os.path.abspath(__file__))
_PKG_ROOT = os.path.dirname(_HERE)
if _PKG_ROOT not in sys.path:
    sys.path.insert(0, _PKG_ROOT)

_PROTOCOL_VERSION = "2025-06-18"
_SUPPORTED_PROTOCOL_VERSIONS = frozenset({
    _PROTOCOL_VERSION,
    "2025-03-26",
})
_HTTP_DEFAULT_PROTOCOL_VERSION = "2025-03-26"
_HTTP_MAX_REQUEST_BYTES = 4 * 1024 * 1024
_HTTP_REJECTION_DRAIN_BYTES = 64 * 1024
_HTTP_SHUTDOWN_GRACE_SECONDS = 3.0
_SERVER_NAME = "sao-ai-editor"
_SERVER_VERSION = "1.0.0"


# =========================================================================
# Tool definitions
# =========================================================================

def _tool(name: str, description: str, schema: Dict[str, Any]) -> Dict[str, Any]:
    return {"name": name, "description": description, "inputSchema": schema}


_TOOLS: List[Dict[str, Any]] = [
    # ── File Operations ──
    _tool("read_file", "Read a file's content. Returns text with optional line range.",
          {"type": "object", "properties": {
              "path": {"type": "string", "description": "Absolute or workspace-relative file path"},
              "start_line": {"type": "integer", "description": "Start line (1-based, optional)"},
              "end_line": {"type": "integer", "description": "End line (inclusive, optional)"},
          }, "required": ["path"]}),

    _tool("edit_file", "Create or edit a file. Full rewrite or line-range replacement.",
          {"type": "object", "properties": {
              "path": {"type": "string", "description": "File path to create or edit"},
              "content": {"type": "string", "description": "New content (full or partial)"},
              "start_line": {"type": "integer", "description": "Start line to replace (1-based, 0=full rewrite)"},
              "end_line": {"type": "integer", "description": "End line to replace (inclusive)"},
          }, "required": ["path", "content"]}),

    _tool("list_files", "List files and directories at a path.",
          {"type": "object", "properties": {
              "path": {"type": "string", "description": "Directory path (default: workspace root)", "default": "."},
              "pattern": {"type": "string", "description": "Glob filter (e.g. '*.py')", "default": ""},
              "recursive": {"type": "boolean", "description": "Recurse into subdirectories", "default": False},
              "limit": {"type": "integer", "description": "Max entries", "default": 200},
          }}),

    _tool("search_files", "Search for text/regex in files. Returns matching lines.",
          {"type": "object", "properties": {
              "query": {"type": "string", "description": "Search text or regex"},
              "path": {"type": "string", "description": "Directory to search", "default": "."},
              "pattern": {"type": "string", "description": "File glob filter (e.g. '*.py')", "default": ""},
              "case_sensitive": {"type": "boolean", "description": "Case-sensitive search", "default": False},
              "regex": {"type": "boolean", "description": "Treat query as regex", "default": False},
              "limit": {"type": "integer", "description": "Max results", "default": 100},
          }, "required": ["query"]}),

    # ── Terminal ──
    _tool("run_terminal", "Execute a shell command and return stdout/stderr.",
          {"type": "object", "properties": {
              "command": {"type": "string", "description": "Shell command to execute"},
              "cwd": {"type": "string", "description": "Working directory", "default": ""},
              "timeout": {"type": "integer", "description": "Timeout in seconds", "default": 30},
          }, "required": ["command"]}),

    # ── LLM Chat ──
    _tool("chat", "Send a message to the AI Editor's LLM and get a response. "
          "Uses the configured provider (OpenAI/Anthropic/DeepSeek/Ollama/Custom).",
          {"type": "object", "properties": {
              "message": {"type": "string", "description": "User message to send"},
              "system_prompt": {"type": "string", "description": "Override system prompt (optional)"},
              "model": {"type": "string", "description": "Override model (optional)"},
              "temperature": {"type": "number", "description": "Sampling temperature", "default": 0.7},
              "max_tokens": {"type": "integer", "description": "Max response tokens", "default": 4096},
          }, "required": ["message"]}),

    _tool("chat_with_agent", "Send a message using a specific AI agent persona "
          "(code-reviewer, explainer, debugger, optimizer, documenter, or custom).",
          {"type": "object", "properties": {
              "agent_id": {"type": "string", "description": "Agent ID (e.g. 'code-reviewer')"},
              "message": {"type": "string", "description": "User message"},
              "model": {"type": "string", "description": "Override model (optional)"},
          }, "required": ["agent_id", "message"]}),

    _tool("run_workflow", "Execute a multi-step AI workflow (review-and-fix, "
          "explain-and-improve, debug-trace, or custom).",
          {"type": "object", "properties": {
              "workflow_id": {"type": "string", "description": "Workflow ID"},
              "input": {"type": "string", "description": "Input text/code for the workflow"},
          }, "required": ["workflow_id", "input"]}),

    # ── Registries ──
    _tool("list_agents", "List all available AI agents with their descriptions and capabilities.",
          {"type": "object", "properties": {}}),

    _tool("list_workflows", "List all available multi-step workflows.",
          {"type": "object", "properties": {}}),

    # ── Engine / Platform ──
    _tool("engine", "Query the SAO ACT platform engine.\n\n"
          "Actions: system_info, plugins, settings_get (key), settings_set (key, value), "
          "memory_status, list_processes, select_process (name/pid), eval (expression), "
          "exec (code).",
          {"type": "object", "properties": {
              "action": {"type": "string", "description": "Engine action to run"},
              "key": {"type": "string", "description": "For settings_get/set", "default": ""},
              "value": {"description": "For settings_set"},
              "expression": {"type": "string", "description": "For eval", "default": ""},
              "code": {"type": "string", "description": "For exec", "default": ""},
              "name": {"type": "string", "description": "For select_process", "default": ""},
              "pid": {"type": "integer", "description": "For select_process", "default": 0},
          }, "required": ["action"]}),

    _tool("sdk_dumper", "Dump game engine SDK from a running process via memory reading.\n"
          "Supports: il2cpp (Unity IL2CPP), mono (Unity Mono), unreal (UE4/5), source (Valve Source).\n"
          "Actions: detect (pid), dump (pid, engine?), list_engines, save (output, format?).",
          {"type": "object", "properties": {
              "action": {"type": "string", "description": "detect | dump | list_engines | save",
                         "enum": ["detect", "dump", "list_engines", "save"]},
              "pid": {"type": "integer", "description": "Target process ID"},
              "engine": {"type": "string", "description": "Engine type (auto-detected if omitted)"},
              "output": {"type": "string", "description": "Output file path (for save)"},
              "format": {"type": "string", "description": "json or header", "default": "json"},
          }, "required": ["action"]}),

    # ── Network ──
    _tool("web_fetch", "Fetch a URL and return its response.",
          {"type": "object", "properties": {
              "url": {"type": "string", "description": "URL to fetch"},
              "method": {"type": "string", "description": "HTTP method", "default": "GET"},
              "headers": {"type": "object", "description": "Request headers"},
              "body": {"type": "string", "description": "Request body", "default": ""},
          }, "required": ["url"]}),

    # ── Configuration ──
    _tool("get_config", "Read the AI Editor's current configuration (provider, model, mode, etc.).",
          {"type": "object", "properties": {
              "section": {"type": "string",
                          "description": "Config section: 'all', 'provider', 'mcp', 'terminal', 'agents', 'workflows'",
                          "default": "all"},
          }}),

    _tool("set_config", "Update AI Editor configuration values.",
          {"type": "object", "properties": {
              "key": {"type": "string", "description": "Config key (e.g. 'provider', 'model', 'temperature')"},
              "value": {"description": "New value"},
          }, "required": ["key", "value"]}),

    # ── Instructions ──
    _tool("get_instructions", "Get the AI Editor's custom instructions (system + workspace + plugin scopes).",
          {"type": "object", "properties": {}}),

    _tool("save_instructions", "Save a custom instruction file.",
          {"type": "object", "properties": {
              "name": {"type": "string", "description": "Instruction file name (e.g. 'my-rules')"},
              "content": {"type": "string", "description": "Instruction content (markdown)"},
              "scope": {"type": "string", "description": "system or workspace", "default": "workspace"},
          }, "required": ["name", "content"]}),

    # ── MCP Meta ──
    _tool("list_mcp_servers", "List MCP servers currently connected to the AI Editor.",
          {"type": "object", "properties": {}}),
]


# =========================================================================
# Prompts — MCP's own mechanism for teaching a connecting client how to
# use this server, fetched via prompts/list + prompts/get instead of
# living only in human-facing docs.
# =========================================================================

_HOW_TO_USE_PROMPT_TEXT = """\
You are connected to the SAO AI Editor MCP server ("sao-ai-editor"). It \
exposes a real, running instance of the editor - not a sandbox - so \
file/terminal tools act on the user's actual project and chat/agent/\
workflow tools use the user's actual configured LLM provider.

When to use which tool group:

- read_file / edit_file / list_files / search_files: your default for any \
direct code change or lookup. Paths may be relative - they resolve \
against the editor's OPEN WORKSPACE, not your own cwd.
- run_terminal: run a real shell command in that same workspace. Prefer \
the most direct command; don't chain unrelated steps into one call.
- chat / chat_with_agent / run_workflow: use these instead of doing the \
work yourself when the user specifically wants the EDITOR's own \
configured model/agent/workflow to answer - e.g. "have your \
code-reviewer agent look at this" should call chat_with_agent with \
agent_id="code-reviewer", not a plain read_file + your own review. \
Call list_agents / list_workflows first if you're not sure an id exists.
- get_config / set_config: read or change the editor's own settings \
(provider, model, temperature, etc.) - only call set_config when the \
user explicitly asked to change a setting.
- get_instructions / save_instructions: the editor's own custom \
instructions (system/workspace/plugin scopes) - read these if you want \
to understand project-specific conventions the user has already set up.
- engine / sdk_dumper: SAO ACT platform-specific tools (process/plugin \
introspection, game engine SDK extraction from a running process). Only \
relevant if the user is actually working on that platform or reverse \
engineering a running process - not general-purpose.
- web_fetch: fetch a URL when the user needs live external data.
- list_mcp_servers: see what OTHER MCP servers this editor is itself \
connected to as a client, in case their tools are relevant too.

General guidance: this is a real, stateful editor a human may be looking \
at right now. Prefer small, explainable, reversible actions over broad \
ones, and don't use engine/sdk_dumper/run_terminal for anything outside \
what the user actually asked for.
"""

_PROMPTS: List[Dict[str, Any]] = [
    {
        "name": "how_to_use_sao_ai_editor",
        "description": "How to use the SAO AI Editor's tools effectively - which "
                        "tool group to reach for and when.",
        "arguments": [],
    },
]


def _prompt_messages(name: str) -> Optional[List[Dict[str, Any]]]:
    if name == "how_to_use_sao_ai_editor":
        return [{
            "role": "user",
            "content": {"type": "text", "text": _HOW_TO_USE_PROMPT_TEXT},
        }]
    return None


# =========================================================================
# Headless runtime — initializes AI Editor subsystems without GUI/pywebview
# =========================================================================

class _HeadlessGui:
    """Minimal stand-in for gui_ref when running without the SAO GUI."""

    def __init__(self) -> None:
        self.settings = self._load_settings()
        self._todo_list: List[str] = []
        self._ai_engine_actions: Dict[str, Callable] = {}
        self._start_time = time.monotonic()
        self._plugin_manager = None
        self._mem_bridge = None
        self._packet_bridge = None

    @staticmethod
    def _load_settings():
        try:
            from config import SettingsManager
            return SettingsManager()
        except Exception:
            return _DictSettings()


class _DictSettings(dict):
    def get(self, key, default=None):
        return super().get(key, default)
    def set(self, key, value):
        self[key] = value
    def save(self):
        pass


class McpRuntime:
    """Headless AI Editor runtime for the MCP server."""

    def __init__(self) -> None:
        self._gui = _HeadlessGui()
        self._engine = None
        self._registry = None
        self._agent_registry = None
        self._wf_registry = None
        self._wf_engine = None
        self._mcp_manager = None
        self._config = None
        self._lock = threading.Lock()
        self.workspace_root = ""

    def _ensure_engine(self) -> None:
        if self._engine is not None:
            return
        with self._lock:
            if self._engine is not None:
                return
            self._config = self._load_config()
            try:
                from ai_editor.llm_engine import LLMEngine, ProviderConfig
                self._engine = LLMEngine(self._config)
            except Exception as exc:
                _log(f"LLM engine init failed: {exc}")
                self._engine = None

            from ai_editor.tool_registry import ToolRegistry
            self._registry = ToolRegistry()
            try:
                from ai_editor.engine_tools import register_engine_tools
                register_engine_tools(self._registry, self._gui)
            except Exception as exc:
                _log(f"Engine tools registration failed: {exc}")

            try:
                from ai_editor.agents import get_agent_registry
                self._agent_registry = get_agent_registry()
            except Exception as exc:
                _log(f"Agent registry init failed: {exc}")

            try:
                from ai_editor.workflows import get_workflow_registry, WorkflowEngine
                self._wf_registry = get_workflow_registry()
                if self._engine and self._agent_registry:
                    self._wf_engine = WorkflowEngine(self._engine, self._agent_registry)
            except Exception as exc:
                _log(f"Workflow init failed: {exc}")

            try:
                from ai_editor.mcp_client import McpManager, load_mcp_configs
                s = self._gui.settings
                getter = (lambda k, d=None: s.get(k, d)) if s else None
                configs = load_mcp_configs(getter)
                if configs:
                    self._mcp_manager = McpManager()
                    for cfg in configs:
                        ok = self._mcp_manager.add_server(cfg)
                        if ok:
                            _log(f"MCP sub-server connected: {cfg.id}")
            except Exception as exc:
                _log(f"MCP client init failed: {exc}")

    def _load_config(self):
        try:
            from ai_editor.llm_engine import ProviderConfig
            s = self._gui.settings
            ai_cfg = s.get("ai_editor", {}) if s else {}
            if not isinstance(ai_cfg, dict):
                ai_cfg = {}
            return ProviderConfig(
                provider=str(ai_cfg.get("provider") or "openai"),
                api_key=str(ai_cfg.get("api_key") or ""),
                base_url=str(ai_cfg.get("base_url") or ""),
                model=str(ai_cfg.get("model") or ""),
                temperature=float(ai_cfg.get("temperature", 0.7)),
                max_tokens=int(ai_cfg.get("max_tokens", 4096)),
                system_prompt=str(ai_cfg.get("system_prompt") or ""),
            )
        except Exception:
            return None

    # ── Tool dispatch ──

    def handle_tool(self, name: str, arguments: Dict[str, Any]) -> Any:
        self._ensure_engine()
        handler = _TOOL_HANDLERS.get(name)
        if not handler:
            return {"error": f"Unknown tool: {name}"}
        try:
            return handler(self, arguments)
        except Exception:
            return {"error": traceback.format_exc(limit=5)}


class LiveMcpRuntime:
    """MCP runtime backed by the ALREADY-RUNNING AIEditorAPI instance, so
    tool calls act on the real, live editor a user has open (its actual
    workspace, provider config, agents/workflows, sub-MCP connections) -
    not a second, disconnected headless one. This is what start_mcp_server()
    uses; the standalone ``--mcp-server`` CLI flag still uses McpRuntime
    (there is no running editor for it to attach to).

    Exposes the same attribute surface _TOOL_HANDLERS already expects
    (_engine/_registry/_agent_registry/_wf_registry/_wf_engine/_mcp_manager/
    _gui/_config), each read live off the wrapped api so it stays correct
    even if the user changes providers or reloads config while connected.
    """

    def __init__(self, api: Any) -> None:
        self._api = api

    def _ensure_engine(self) -> None:
        self._api._ensure_engine()

    @property
    def workspace_root(self) -> str:
        try:
            return self._api._workspace_root()
        except Exception:
            return ""

    @property
    def _engine(self) -> Any:
        return getattr(self._api, "_engine", None)

    @property
    def _registry(self) -> Any:
        return getattr(self._api, "_registry", None)

    @property
    def _agent_registry(self) -> Any:
        return getattr(self._api, "_agent_registry", None)

    @property
    def _wf_registry(self) -> Any:
        return getattr(self._api, "_wf_registry", None)

    @property
    def _wf_engine(self) -> Any:
        return getattr(self._api, "_wf_engine", None)

    @property
    def _mcp_manager(self) -> Any:
        return getattr(self._api, "_mcp", None)

    @property
    def _gui(self) -> Any:
        return _LiveGuiAdapter(self._api)

    @property
    def _config(self) -> Any:
        engine = self._engine
        return getattr(engine, "config", None) if engine is not None else None

    def handle_tool(self, name: str, arguments: Dict[str, Any]) -> Any:
        self._ensure_engine()
        handler = _TOOL_HANDLERS.get(name)
        if not handler:
            return {"error": f"Unknown tool: {name}"}
        try:
            return handler(self, arguments)
        except Exception:
            return {"error": traceback.format_exc(limit=5)}


class _LiveGuiAdapter:
    """Thin ``rt._gui.settings``-shaped view over the live api's own
    settings source, for the get_config/set_config tool handlers."""

    def __init__(self, api: Any) -> None:
        self._api = api

    @property
    def settings(self) -> Any:
        from ai_editor.app import _resolve_settings
        return _resolve_settings(getattr(self._api, "_gui_ref", None))


# =========================================================================
# Tool handler implementations
# =========================================================================

def _resolve_path(rt: "McpRuntime", raw: str) -> str:
    """Resolve a tool-supplied path against the runtime's workspace root
    when relative, so tools operate on the actual open project when this
    server is running live inside the editor (not just the process CWD,
    which is what a standalone headless --mcp-server has instead)."""
    text = str(raw or "")
    if os.path.isabs(text):
        return text
    root = getattr(rt, "workspace_root", "") or os.getcwd()
    return os.path.abspath(os.path.join(root, text)) if text else root


def _h_read_file(rt: McpRuntime, args: Dict[str, Any]) -> Any:
    path = _resolve_path(rt, args.get("path", ""))
    start = int(args.get("start_line", 0))
    end = int(args.get("end_line", 0))
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
        if start > 0:
            s = max(0, start - 1)
            e = end if end > 0 else len(lines)
            selected = lines[s:e]
            return {"path": path, "lines": len(selected), "startLine": start,
                    "content": "".join(selected)}
        content = "".join(lines[:10000])
        return {"path": path, "lines": len(lines), "content": content,
                "truncated": len(lines) > 10000}
    except Exception as exc:
        return {"error": str(exc)}


def _h_edit_file(rt: McpRuntime, args: Dict[str, Any]) -> Any:
    path = _resolve_path(rt, args.get("path", ""))
    content = args.get("content", "")
    start = int(args.get("start_line", 0))
    end = int(args.get("end_line", 0))
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if start > 0:
            with open(path, "r", encoding="utf-8") as f:
                lines = f.readlines()
            s = max(0, start - 1)
            e = end if end > 0 else start
            new_lines = content.split("\n")
            lines[s:e] = [line + "\n" for line in new_lines]
            with open(path, "w", encoding="utf-8") as f:
                f.writelines(lines)
            return {"ok": True, "path": path, "linesModified": len(new_lines)}
        with open(path, "w", encoding="utf-8") as f:
            f.write(content)
        return {"ok": True, "path": path, "bytesWritten": len(content.encode("utf-8"))}
    except Exception as exc:
        return {"error": str(exc)}


def _h_list_files(rt: McpRuntime, args: Dict[str, Any]) -> Any:
    import glob as _glob
    path = _resolve_path(rt, args.get("path", "."))
    pattern = args.get("pattern", "")
    recursive = bool(args.get("recursive", False))
    limit = int(args.get("limit", 200))
    try:
        if pattern:
            if recursive:
                entries = _glob.glob(os.path.join(path, "**", pattern), recursive=True)
            else:
                entries = _glob.glob(os.path.join(path, pattern))
        elif recursive:
            entries = []
            for root, _dirs, files in os.walk(path):
                for f in files:
                    entries.append(os.path.join(root, f))
                    if len(entries) >= limit:
                        break
                if len(entries) >= limit:
                    break
        else:
            entries = [os.path.join(path, e) for e in os.listdir(path)]
        result = []
        for e in entries[:limit]:
            is_dir = os.path.isdir(e)
            result.append({"name": os.path.relpath(e, path),
                           "type": "directory" if is_dir else "file",
                           "size": os.path.getsize(e) if not is_dir else 0})
        return {"path": path, "entries": result, "total": len(result),
                "truncated": len(entries) > limit}
    except Exception as exc:
        return {"error": str(exc)}


def _h_search_files(rt: McpRuntime, args: Dict[str, Any]) -> Any:
    import re
    import glob as _glob
    query = args.get("query", "")
    path = _resolve_path(rt, args.get("path", "."))
    pattern = args.get("pattern", "")
    case_sensitive = bool(args.get("case_sensitive", False))
    use_regex = bool(args.get("regex", False))
    limit = int(args.get("limit", 100))
    try:
        flags = 0 if case_sensitive else re.IGNORECASE
        pat = re.compile(query, flags) if use_regex else re.compile(re.escape(query), flags)
        results = []
        file_pattern = pattern or "*"
        for fpath in _glob.glob(os.path.join(path, "**", file_pattern), recursive=True):
            if os.path.isdir(fpath):
                continue
            try:
                with open(fpath, "r", encoding="utf-8", errors="replace") as f:
                    for i, line in enumerate(f, 1):
                        if pat.search(line):
                            results.append({"file": os.path.relpath(fpath, path),
                                            "line": i, "text": line.rstrip()[:300]})
                            if len(results) >= limit:
                                break
            except (OSError, UnicodeDecodeError):
                continue
            if len(results) >= limit:
                break
        return {"query": query, "results": results, "total": len(results)}
    except Exception as exc:
        return {"error": str(exc)}


def _h_run_terminal(rt: McpRuntime, args: Dict[str, Any]) -> Any:
    import subprocess
    command = args.get("command", "")
    cwd = args.get("cwd", "")
    timeout = int(args.get("timeout", 30))
    try:
        kwargs: Dict[str, Any] = {"shell": True, "capture_output": True,
                                  "text": True, "timeout": timeout}
        kwargs["cwd"] = _resolve_path(rt, cwd) if cwd else (
            getattr(rt, "workspace_root", "") or None)
        result = subprocess.run(command, **kwargs)
        return {"exitCode": result.returncode,
                "stdout": (result.stdout or "")[:16000],
                "stderr": (result.stderr or "")[:8000]}
    except subprocess.TimeoutExpired:
        return {"error": f"Timed out after {timeout}s", "exitCode": -1}
    except Exception as exc:
        return {"error": str(exc)}


def _h_chat(rt: McpRuntime, args: Dict[str, Any]) -> Any:
    if rt._engine is None:
        return {"error": "LLM engine not configured. Set provider/api_key in AI Editor settings."}
    message = args.get("message", "")
    system_prompt = args.get("system_prompt", "")
    model = args.get("model", "")
    temperature = float(args.get("temperature", 0.7))
    max_tokens = int(args.get("max_tokens", 4096))

    messages = []
    if system_prompt:
        messages.append({"role": "system", "content": system_prompt})
    messages.append({"role": "user", "content": message})

    try:
        from ai_editor.llm_engine import ProviderConfig
        override = None
        config = rt._config
        if model or temperature != config.temperature or max_tokens != config.max_tokens:
            override = ProviderConfig(
                provider=config.provider,
                api_key=config.api_key,
                base_url=config.base_url,
                model=model or config.model,
                temperature=temperature,
                max_tokens=max_tokens,
                system_prompt=system_prompt or config.system_prompt,
            )

        rt._engine.reset_cancel()
        resp = rt._engine.chat_completion_stream(
            messages=messages, tools=None,
            config_override=override,
            on_delta=lambda _d: None,
        )
        result = {"content": resp.content or ""}
        if resp.thinking:
            result["thinking"] = resp.thinking
        if resp.usage:
            result["usage"] = resp.usage
        if resp.error:
            result["error"] = resp.error
        return result
    except Exception as exc:
        return {"error": str(exc)}


def _h_chat_with_agent(rt: McpRuntime, args: Dict[str, Any]) -> Any:
    if rt._engine is None:
        return {"error": "LLM engine not configured"}
    agent_id = args.get("agent_id", "")
    message = args.get("message", "")
    model = args.get("model", "")

    if not rt._agent_registry:
        return {"error": "Agent registry not initialized"}
    agent = rt._agent_registry.get(agent_id)
    if not agent:
        available = [a.id for a in rt._agent_registry.list_all()]
        return {"error": f"Agent not found: {agent_id}", "available": available}

    messages = []
    if agent.system_prompt:
        messages.append({"role": "system", "content": agent.system_prompt})
    messages.append({"role": "user", "content": message})

    try:
        override = None
        if model:
            from ai_editor.llm_engine import ProviderConfig
            override = ProviderConfig(
                provider=rt._config.provider, api_key=rt._config.api_key,
                base_url=rt._config.base_url, model=model,
                temperature=rt._config.temperature, max_tokens=rt._config.max_tokens,
            )

        rt._engine.reset_cancel()
        resp = rt._engine.chat_completion_stream(
            messages=messages, tools=None,
            config_override=override,
            on_delta=lambda _d: None,
        )
        result = {"agent": agent_id, "agent_name": agent.name, "content": resp.content or ""}
        if resp.thinking:
            result["thinking"] = resp.thinking
        if resp.error:
            result["error"] = resp.error
        return result
    except Exception as exc:
        return {"error": str(exc)}


def _h_run_workflow(rt: McpRuntime, args: Dict[str, Any]) -> Any:
    if rt._wf_engine is None:
        return {"error": "Workflow engine not initialized (LLM engine required)"}
    wf_id = args.get("workflow_id", "")
    input_text = args.get("input", "")

    if not rt._wf_registry:
        return {"error": "Workflow registry not initialized"}
    wf = rt._wf_registry.get(wf_id)
    if not wf:
        available = [w.id for w in rt._wf_registry.list_all()]
        return {"error": f"Workflow not found: {wf_id}", "available": available}

    try:
        result = rt._wf_engine.run(wf, input_text)
        return result
    except Exception as exc:
        return {"error": str(exc)}


def _h_list_agents(rt: McpRuntime, args: Dict[str, Any]) -> Any:
    if not rt._agent_registry:
        return {"agents": []}
    agents = []
    for a in rt._agent_registry.list_all():
        agents.append({
            "id": a.id, "name": a.name, "description": a.description,
            "icon": a.icon, "when_to_use": a.when_to_use,
            "builtin": a.builtin,
            "tools": a.tools or [],
        })
    return {"agents": agents}


def _h_list_workflows(rt: McpRuntime, args: Dict[str, Any]) -> Any:
    if not rt._wf_registry:
        return {"workflows": []}
    workflows = []
    for w in rt._wf_registry.list_all():
        workflows.append({
            "id": w.id, "name": w.name, "description": w.description,
            "icon": w.icon, "when_to_use": w.when_to_use,
            "builtin": w.builtin,
            "steps": [{"agent": s.agent, "label": s.label, "output_var": s.output_var}
                      for s in w.steps],
        })
    return {"workflows": workflows}


def _h_engine(rt: McpRuntime, args: Dict[str, Any]) -> Any:
    if rt._registry is None:
        return {"error": "Engine tools not initialized"}
    return rt._registry.execute("engine", args)


def _h_sdk_dumper(rt: McpRuntime, args: Dict[str, Any]) -> Any:
    if rt._registry is None:
        return {"error": "Engine tools not initialized"}
    desc = rt._registry.get("sdkDumper")
    if not desc:
        return {"error": "SDK Dumper not available (ai_editor.sdk_dumper module not found)"}
    return rt._registry.execute("sdkDumper", args)


def _h_web_fetch(rt: McpRuntime, args: Dict[str, Any]) -> Any:
    import urllib.request
    import urllib.error
    url = args.get("url", "")
    method = str(args.get("method", "GET")).upper()
    headers = args.get("headers") or {}
    body = args.get("body", "")
    try:
        data = body.encode("utf-8") if body else None
        req = urllib.request.Request(url, data=data, method=method)
        for k, v in headers.items():
            req.add_header(str(k), str(v))
        with urllib.request.urlopen(req, timeout=15) as resp:
            raw = resp.read()
            body_text = raw.decode("utf-8", errors="replace")
            return {"status": resp.status,
                    "headers": {k: v for k, v in resp.getheaders()},
                    "body": body_text[:8000],
                    "truncated": len(body_text) > 8000}
    except urllib.error.HTTPError as exc:
        try:
            err_body = exc.read().decode("utf-8", errors="replace")[:4000]
        except Exception:
            err_body = ""
        return {"status": exc.code, "error": str(exc.reason), "body": err_body}
    except Exception as exc:
        return {"error": str(exc)}


def _h_get_config(rt: McpRuntime, args: Dict[str, Any]) -> Any:
    section = args.get("section", "all")
    s = rt._gui.settings
    ai_cfg = s.get("ai_editor", {}) if s else {}
    if not isinstance(ai_cfg, dict):
        ai_cfg = {}

    if section == "provider":
        return {k: ai_cfg.get(k) for k in
                ("provider", "model", "base_url", "temperature",
                 "max_tokens", "transport", "system_prompt") if k in ai_cfg}

    if section == "mcp":
        return ai_cfg.get("mcp", {})

    if section == "terminal":
        return ai_cfg.get("terminal", {})

    if section == "agents":
        return _h_list_agents(rt, {})

    if section == "workflows":
        return _h_list_workflows(rt, {})

    safe_cfg = dict(ai_cfg)
    safe_cfg.pop("api_key", None)
    safe_cfg.pop("provider_keys", None)
    return safe_cfg


def _h_set_config(rt: McpRuntime, args: Dict[str, Any]) -> Any:
    key = args.get("key", "")
    value = args.get("value")
    if not key:
        return {"error": "key is required"}
    s = rt._gui.settings
    ai_cfg = s.get("ai_editor", {}) if s else {}
    if not isinstance(ai_cfg, dict):
        ai_cfg = {}
    ai_cfg[key] = value
    try:
        s.set("ai_editor", ai_cfg)
        save_fn = getattr(s, "save", None)
        if callable(save_fn):
            save_fn()
        return {"ok": True, "key": key}
    except Exception as exc:
        return {"error": str(exc)}


def _h_get_instructions(rt: McpRuntime, args: Dict[str, Any]) -> Any:
    try:
        from ai_editor.prompts import load_instructions, list_instruction_files
        s = rt._gui.settings
        getter = (lambda k, d=None: s.get(k, d)) if s else None
        text = load_instructions(getter)
        files = list_instruction_files()
        return {"instructions": text, "files": files}
    except Exception as exc:
        return {"error": str(exc)}


def _h_save_instructions(rt: McpRuntime, args: Dict[str, Any]) -> Any:
    name = args.get("name", "")
    content = args.get("content", "")
    scope = args.get("scope", "workspace")
    if not name:
        return {"error": "name is required"}
    try:
        from ai_editor.prompts import save_instruction_file
        result = save_instruction_file(name, content)
        return result if isinstance(result, dict) else {"ok": True}
    except Exception as exc:
        return {"error": str(exc)}


def _h_list_mcp_servers(rt: McpRuntime, args: Dict[str, Any]) -> Any:
    if not rt._mcp_manager:
        return {"servers": [], "note": "No MCP client manager initialized"}
    return {"servers": rt._mcp_manager.list_servers()}


_TOOL_HANDLERS: Dict[str, Callable[[McpRuntime, Dict[str, Any]], Any]] = {
    "read_file": _h_read_file,
    "edit_file": _h_edit_file,
    "list_files": _h_list_files,
    "search_files": _h_search_files,
    "run_terminal": _h_run_terminal,
    "chat": _h_chat,
    "chat_with_agent": _h_chat_with_agent,
    "run_workflow": _h_run_workflow,
    "list_agents": _h_list_agents,
    "list_workflows": _h_list_workflows,
    "engine": _h_engine,
    "sdk_dumper": _h_sdk_dumper,
    "web_fetch": _h_web_fetch,
    "get_config": _h_get_config,
    "set_config": _h_set_config,
    "get_instructions": _h_get_instructions,
    "save_instructions": _h_save_instructions,
    "list_mcp_servers": _h_list_mcp_servers,
}


# =========================================================================
# JSON-RPC stdio transport (server side)
# =========================================================================

def _log(msg: str) -> None:
    print(f"[MCP-Server] {msg}", file=sys.stderr, flush=True)


def _write_response(data: Dict[str, Any]) -> None:
    raw = json.dumps(data, ensure_ascii=False)
    encoded = raw.encode("utf-8")
    header = f"Content-Length: {len(encoded)}\r\n\r\n"
    sys.stdout.buffer.write(header.encode("ascii"))
    sys.stdout.buffer.write(encoded)
    sys.stdout.buffer.flush()


def _read_message() -> Optional[Dict[str, Any]]:
    first_line = sys.stdin.buffer.readline()
    if not first_line:
        return None
    stripped = first_line.strip()
    if not stripped:
        return None

    if stripped.lower().startswith(b"content-length:"):
        try:
            length = int(stripped.split(b":", 1)[1].strip())
        except (IndexError, ValueError):
            return None
        while True:
            header_line = sys.stdin.buffer.readline()
            if not header_line:
                return None
            if header_line in (b"\r\n", b"\n", b""):
                break
        if length <= 0:
            return None
        payload = sys.stdin.buffer.read(length)
        if not payload or len(payload) < length:
            return None
    else:
        payload = first_line

    try:
        return json.loads(payload.decode("utf-8", errors="replace"))
    except json.JSONDecodeError:
        return None


def _make_response(req_id: Any, result: Any) -> Dict[str, Any]:
    return {"jsonrpc": "2.0", "id": req_id, "result": result}


def _make_error(req_id: Any, code: int, message: str,
                data: Any = None) -> Dict[str, Any]:
    error: Dict[str, Any] = {"code": code, "message": message}
    if data is not None:
        error["data"] = data
    return {"jsonrpc": "2.0", "id": req_id, "error": error}


def _negotiate_protocol_version(params: Any) -> str:
    """Choose the newest mutually understood protocol version.

    MCP clients request a single preferred version.  When it is supported the
    server must echo it; otherwise the server advertises its own latest version
    and lets the client decide whether it can continue.
    """
    if isinstance(params, dict):
        requested = str(params.get("protocolVersion") or "").strip()
        if requested in _SUPPORTED_PROTOCOL_VERSIONS:
            return requested
    return _PROTOCOL_VERSION


def _initialize_result(params: Any) -> Dict[str, Any]:
    return {
        "protocolVersion": _negotiate_protocol_version(params),
        "capabilities": {
            "tools": {"listChanged": False},
            "resources": {},
            "prompts": {"listChanged": False},
        },
        "serverInfo": {
            "name": _SERVER_NAME,
            "version": _SERVER_VERSION,
        },
    }


def _tool_result_content(result: Any) -> Dict[str, Any]:
    if isinstance(result, str):
        text = result
    elif isinstance(result, dict):
        text = json.dumps(result, ensure_ascii=False, indent=2, default=str)
    else:
        text = json.dumps(result, ensure_ascii=False, default=str)
    return {"content": [{"type": "text", "text": text}]}


class McpServer:
    """Stdio JSON-RPC MCP server exposing AI Editor capabilities."""

    def __init__(self) -> None:
        self._runtime = McpRuntime()
        self._initialized = False

    def run(self) -> None:
        _log("Starting SAO AI Editor MCP server (stdio)")
        while True:
            msg = _read_message()
            if msg is None:
                _log("stdin closed, shutting down")
                break
            if not isinstance(msg, dict):
                continue
            method = msg.get("method", "")
            req_id = msg.get("id")
            params = msg.get("params") or {}

            if method == "initialize":
                self._handle_initialize(req_id, params)
            elif method == "notifications/initialized":
                self._initialized = True
                _log("Client initialized")
            elif method == "tools/list":
                self._handle_tools_list(req_id, params)
            elif method == "tools/call":
                self._handle_tools_call(req_id, params)
            elif method == "resources/list":
                _write_response(_make_response(req_id, {"resources": []}))
            elif method == "resources/read":
                _write_response(_make_error(req_id, -32601, "No resources available"))
            elif method == "prompts/list":
                _write_response(_make_response(req_id, {"prompts": _PROMPTS}))
            elif method == "prompts/get":
                name = str(params.get("name", ""))
                messages = _prompt_messages(name)
                if messages is None:
                    _write_response(_make_error(req_id, -32602, f"Unknown prompt: {name}"))
                else:
                    _write_response(_make_response(req_id, {"messages": messages}))
            elif method == "ping":
                _write_response(_make_response(req_id, {}))
            elif req_id is not None:
                _write_response(_make_error(req_id, -32601, f"Method not found: {method}"))

    def _handle_initialize(self, req_id: Any, params: Dict[str, Any]) -> None:
        client_info = params.get("clientInfo", {})
        _log(f"Initialize from {client_info.get('name', '?')} v{client_info.get('version', '?')}")
        _write_response(_make_response(req_id, _initialize_result(params)))

    def _handle_tools_list(self, req_id: Any, params: Dict[str, Any]) -> None:
        _write_response(_make_response(req_id, {"tools": _TOOLS}))

    def _handle_tools_call(self, req_id: Any, params: Dict[str, Any]) -> None:
        name = params.get("name", "")
        arguments = params.get("arguments") or {}

        if name not in _TOOL_HANDLERS:
            _write_response(_make_error(
                req_id, -32602,
                f"Unknown tool: {name}",
                {"available": sorted(_TOOL_HANDLERS.keys())}))
            return

        _log(f"Calling tool: {name}")
        try:
            result = self._runtime.handle_tool(name, arguments)
            _write_response(_make_response(req_id, _tool_result_content(result)))
        except Exception as exc:
            _write_response(_make_response(req_id, _tool_result_content(
                {"error": str(exc), "traceback": traceback.format_exc(limit=3)}
            )))


# =========================================================================
# Optional Streamable HTTP server mode
# =========================================================================

def _http_origin_allowed(origin: str) -> bool:
    """Allow non-browser clients and loopback browser origins only.

    Validating an explicitly supplied Origin is required for local Streamable
    HTTP servers because otherwise a hostile website can use DNS rebinding to
    reach the editor's privileged tools.
    """
    value = str(origin or "").strip()
    if not value:
        return True
    try:
        parsed = urlsplit(value)
        hostname = str(parsed.hostname or "").lower()
    except (TypeError, ValueError):
        return False
    return (
        parsed.scheme.lower() in {"http", "https"}
        and hostname in {"127.0.0.1", "localhost", "::1"}
        and not parsed.username
        and not parsed.password
    )


def _http_accepts_json(accept: str) -> bool:
    """Return whether an Accept header permits a JSON response.

    Missing Accept remains supported for the editor's pre-Streamable-HTTP
    urllib integration.  Explicitly SSE-only callers receive 406 because this
    deliberately small server does not implement optional SSE streams.
    """
    value = str(accept or "").strip()
    if not value:
        return True
    for raw_item in value.split(","):
        parts = [part.strip() for part in raw_item.split(";")]
        media_type = parts[0].lower()
        quality = 1.0
        for parameter in parts[1:]:
            key, separator, raw_value = parameter.partition("=")
            if separator and key.strip().lower() == "q":
                try:
                    quality = float(raw_value.strip())
                except ValueError:
                    quality = 0.0
        if quality > 0 and media_type in {
                "application/json", "application/*", "*/*"}:
            return True
    return False


class McpHttpServer:
    """JSON-only subset of MCP 2025-06-18 Streamable HTTP.

    The endpoint is intentionally stateless: it does not issue session IDs or
    provide optional GET/SSE streams.  Every JSON-RPC request is answered with
    one JSON object, while notifications and client responses are acknowledged
    with HTTP 202 and an empty body.
    """

    def __init__(self, port: int = 9820, runtime: Optional[Any] = None,
                 max_request_bytes: int = _HTTP_MAX_REQUEST_BYTES) -> None:
        request_limit = int(max_request_bytes)
        if request_limit <= 0:
            raise ValueError("max_request_bytes must be positive")
        self._port = int(port)
        self._runtime = runtime if runtime is not None else McpRuntime()
        self._max_request_bytes = request_limit
        self._server = None
        self._state_lock = threading.Lock()
        self._tool_call_lock = threading.RLock()
        self._closing = threading.Event()
        self._ready = threading.Event()
        self._stopped = threading.Event()
        self._initialize_seen = threading.Event()
        self._initialized = threading.Event()
        self._startup_error = ""

    def run(self) -> None:
        from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

        owner = self
        runtime = self._runtime
        port = self._port

        class ReadyThreadingHTTPServer(ThreadingHTTPServer):
            # HTTPServer enables SO_REUSEADDR by default.  On Windows that can
            # let a second process bind the same local MCP port, so retain the
            # safer exclusive default there while keeping Unix restartability.
            allow_reuse_address = os.name != "nt"

            def service_actions(self) -> None:
                # service_actions runs from inside serve_forever's loop, so
                # readiness means shutdown() is now safe to call.
                owner._ready.set()
                super().service_actions()

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_POST(self) -> None:
                origin_allowed = _http_origin_allowed(
                    self.headers.get("Origin", ""))
                endpoint_exists = urlsplit(self.path).path == "/"
                if self.headers.get("Transfer-Encoding"):
                    self.close_connection = True
                    self._send_rpc_error(
                        400 if origin_allowed else 403, None, -32600,
                        ("Transfer-Encoding is not supported; use Content-Length"
                         if origin_allowed else "Origin is not allowed"))
                    return
                raw_length = str(self.headers.get("Content-Length") or "").strip()
                try:
                    length = int(raw_length)
                except ValueError:
                    self.close_connection = True
                    self._send_rpc_error(
                        411 if origin_allowed else 403, None, -32600,
                        ("A valid Content-Length is required"
                         if origin_allowed else "Origin is not allowed"))
                    return
                if length <= 0:
                    self.close_connection = True
                    self._send_rpc_error(
                        400, None, -32700, "Request body must not be empty")
                    return
                if length > owner._max_request_bytes:
                    self.close_connection = True
                    self._send_rpc_error(
                        413 if origin_allowed else 403, None, -32600,
                        ("Request body exceeds the configured limit"
                         if origin_allowed else "Origin is not allowed"))
                    self._drain_rejected_body(length)
                    return

                body = self.rfile.read(length)
                if len(body) != length:
                    self.close_connection = True
                    self._send_rpc_error(
                        400, None, -32700, "Incomplete request body")
                    return
                if not origin_allowed:
                    self._send_rpc_error(
                        403, None, -32600, "Origin is not allowed")
                    return
                if not endpoint_exists:
                    self._send_rpc_error(
                        404, None, -32600, "MCP endpoint not found")
                    return
                content_type = str(
                    self.headers.get("Content-Type") or "").split(";", 1)[0]
                if content_type.strip().lower() != "application/json":
                    self._send_rpc_error(
                        415, None, -32600,
                        "Content-Type must be application/json")
                    return
                if not _http_accepts_json(self.headers.get("Accept", "")):
                    self._send_rpc_error(
                        406, None, -32600,
                        "This endpoint returns application/json; SSE is not supported")
                    return
                try:
                    msg = json.loads(body.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError):
                    self._send_rpc_error(400, None, -32700, "Invalid JSON")
                    return
                if not isinstance(msg, dict) or msg.get("jsonrpc") != "2.0":
                    request_id = msg.get("id") if isinstance(msg, dict) else None
                    self._send_rpc_error(
                        400, request_id, -32600, "Invalid JSON-RPC request")
                    return

                header_version = str(
                    self.headers.get("MCP-Protocol-Version") or
                    _HTTP_DEFAULT_PROTOCOL_VERSION).strip()
                if header_version not in _SUPPORTED_PROTOCOL_VERSIONS:
                    self._send_rpc_error(
                        400, msg.get("id"), -32602,
                        "Unsupported MCP-Protocol-Version",
                        {"supported": sorted(_SUPPORTED_PROTOCOL_VERSIONS)})
                    return

                method = msg.get("method")
                if method is None:
                    if "id" in msg and ("result" in msg or "error" in msg):
                        self._send_empty(202)
                    else:
                        self._send_rpc_error(
                            400, msg.get("id"), -32600,
                            "Invalid JSON-RPC message")
                    return
                if not isinstance(method, str) or not method:
                    self._send_rpc_error(
                        400, msg.get("id"), -32600,
                        "JSON-RPC method must be a non-empty string")
                    return
                raw_params = msg.get("params", {})
                params = {} if raw_params is None else raw_params
                if not isinstance(params, dict):
                    if "id" not in msg:
                        self._send_empty(202)
                    else:
                        self._send_json(
                            200, _make_error(
                                msg.get("id"), -32602,
                                "JSON-RPC params must be an object"))
                    return

                if "id" not in msg:
                    if method == "initialize":
                        self._send_rpc_error(
                            400, None, -32600,
                            "initialize must be a JSON-RPC request")
                        return
                    if method == "notifications/initialized":
                        if not owner._initialize_seen.is_set():
                            self._send_rpc_error(
                                400, None, -32600,
                                "initialize must complete before initialized")
                            return
                        owner._initialized.set()
                        _log("HTTP MCP client initialized")
                    self._send_empty(202)
                    return

                req_id = msg.get("id")
                if method == "initialize":
                    owner._initialize_seen.set()
                    client_info = params.get("clientInfo")
                    if isinstance(client_info, dict):
                        _log(
                            "HTTP initialize from "
                            f"{client_info.get('name', '?')} "
                            f"v{client_info.get('version', '?')}")
                    self._send_json(
                        200, _make_response(req_id, _initialize_result(params)))
                elif method == "tools/list":
                    self._send_json(
                        200, _make_response(req_id, {"tools": _TOOLS}))
                elif method == "tools/call":
                    self._handle_tool_call(req_id, params)
                elif method == "resources/list":
                    self._send_json(
                        200, _make_response(req_id, {"resources": []}))
                elif method == "resources/read":
                    self._send_json(
                        200, _make_error(
                            req_id, -32601, "No resources available"))
                elif method == "prompts/list":
                    self._send_json(
                        200, _make_response(req_id, {"prompts": _PROMPTS}))
                elif method == "prompts/get":
                    prompt_name = str(params.get("name", ""))
                    messages = _prompt_messages(prompt_name)
                    if messages is None:
                        self._send_json(
                            200, _make_error(
                                req_id, -32602,
                                f"Unknown prompt: {prompt_name}"))
                    else:
                        self._send_json(
                            200, _make_response(
                                req_id, {"messages": messages}))
                elif method == "ping":
                    self._send_json(200, _make_response(req_id, {}))
                else:
                    self._send_json(
                        200, _make_error(
                            req_id, -32601,
                            f"Method not found: {method}"))

            def do_GET(self) -> None:
                if self._validate_endpoint_and_origin():
                    self._send_empty(405, {"Allow": "POST"})

            def do_DELETE(self) -> None:
                if self._validate_endpoint_and_origin():
                    self._send_empty(405, {"Allow": "POST"})

            def do_OPTIONS(self) -> None:
                if self._validate_endpoint_and_origin():
                    self._send_empty(405, {"Allow": "POST"})

            def _handle_tool_call(
                    self, req_id: Any, params: Dict[str, Any]) -> None:
                name = str(params.get("name") or "")
                arguments = params.get("arguments") or {}
                if not name or name not in _TOOL_HANDLERS:
                    self._send_json(
                        200, _make_error(
                            req_id, -32602, f"Unknown tool: {name}",
                            {"available": sorted(_TOOL_HANDLERS.keys())}))
                    return
                if not isinstance(arguments, dict):
                    self._send_json(
                        200, _make_error(
                            req_id, -32602,
                            "Tool arguments must be an object"))
                    return
                _log(f"HTTP calling tool: {name}")
                try:
                    # The live runtime mutates editor/workspace state and is not
                    # safe to enter concurrently from ThreadingHTTPServer workers.
                    with owner._tool_call_lock:
                        if owner._closing.is_set():
                            self._send_json(
                                503, _make_error(
                                    req_id, -32000,
                                    "MCP server is shutting down"))
                            return
                        result = runtime.handle_tool(name, arguments)
                    self._send_json(
                        200, _make_response(
                            req_id, _tool_result_content(result)))
                except Exception as exc:
                    error_result = _tool_result_content({"error": str(exc)})
                    error_result["isError"] = True
                    self._send_json(
                        200, _make_response(req_id, error_result))

            def _validate_endpoint_and_origin(self) -> bool:
                if not _http_origin_allowed(self.headers.get("Origin", "")):
                    self._send_rpc_error(
                        403, None, -32600, "Origin is not allowed")
                    return False
                if urlsplit(self.path).path != "/":
                    self._send_rpc_error(
                        404, None, -32600, "MCP endpoint not found")
                    return False
                return True

            def _drain_rejected_body(self, declared_length: int) -> None:
                """Best-effort small drain so clients receive 413 before close.

                The cap avoids turning rejection into an unbounded slow-read;
                larger or stalled uploads are simply disconnected.
                """
                remaining = min(
                    max(0, int(declared_length)),
                    _HTTP_REJECTION_DRAIN_BYTES)
                if not remaining:
                    return
                previous_timeout = self.connection.gettimeout()
                try:
                    self.connection.settimeout(0.1)
                    while remaining:
                        chunk = self.rfile.read(min(remaining, 8192))
                        if not chunk:
                            break
                        remaining -= len(chunk)
                except OSError:
                    pass
                finally:
                    try:
                        self.connection.settimeout(previous_timeout)
                    except OSError:
                        pass

            def _send_rpc_error(
                    self, status: int, req_id: Any, code: int,
                    message: str, data: Any = None) -> None:
                self._send_json(
                    status, _make_error(req_id, code, message, data))

            def _send_json(self, status: int, data: Any) -> None:
                raw = json.dumps(
                    data, ensure_ascii=False, separators=(",", ":"),
                    default=str).encode("utf-8")
                self.send_response(status)
                self._send_common_headers()
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                try:
                    self.wfile.write(raw)
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def _send_empty(
                    self, status: int,
                    headers: Optional[Dict[str, str]] = None) -> None:
                self.send_response(status)
                self._send_common_headers()
                for key, value in (headers or {}).items():
                    self.send_header(str(key), str(value))
                self.send_header("Content-Length", "0")
                self.end_headers()

            def _send_common_headers(self) -> None:
                self.send_header("Cache-Control", "no-store")
                self.send_header("X-Content-Type-Options", "nosniff")

            def log_message(self, fmt: str, *args: Any) -> None:
                _log(fmt % args)

        server = None
        self._ready.clear()
        self._stopped.clear()
        self._closing.clear()
        self._startup_error = ""
        try:
            server = ReadyThreadingHTTPServer(("127.0.0.1", port), Handler)
            server.daemon_threads = True
            with self._state_lock:
                self._server = server
                self._port = int(server.server_address[1])
            _log(
                "Streamable HTTP MCP server listening on "
                f"http://127.0.0.1:{self._port}")
            server.serve_forever(poll_interval=0.05)
        except KeyboardInterrupt:
            _log("HTTP server stopped")
        except OSError as exc:
            self._startup_error = str(exc)
            _log(f"HTTP server failed to start: {exc}")
        finally:
            self._ready.set()
            if server is not None:
                server.server_close()
            with self._state_lock:
                if self._server is server:
                    self._server = None
            self._stopped.set()

    @property
    def port(self) -> int:
        return self._port

    @property
    def startup_error(self) -> str:
        return self._startup_error

    @property
    def initialized(self) -> bool:
        return self._initialized.is_set()

    def wait_until_ready(self, timeout: float = 3.0) -> bool:
        """Wait until bind succeeds or the server reports a startup error."""
        return self._ready.wait(max(0.0, float(timeout))) and not self._startup_error

    def stop(self) -> None:
        with self._state_lock:
            server = self._server
        if server is None:
            return
        self._closing.set()
        server.shutdown()
        # Wait for the one permitted runtime mutation to finish.  Workers that
        # were queued on the serialization lock observe _closing and abort.
        acquired = self._tool_call_lock.acquire(
            timeout=_HTTP_SHUTDOWN_GRACE_SECONDS)
        if acquired:
            self._tool_call_lock.release()
        else:
            _log("Timed out waiting for an in-flight MCP tool during shutdown")
        self._stopped.wait(_HTTP_SHUTDOWN_GRACE_SECONDS)


# =========================================================================
# Entry point
# =========================================================================

def main() -> None:
    import argparse
    parser = argparse.ArgumentParser(description="SAO AI Editor MCP Server")
    parser.add_argument("--port", type=int, default=0,
                        help="Run as HTTP server on this port instead of stdio")
    args = parser.parse_args()

    if args.port:
        server = McpHttpServer(port=args.port)
    else:
        server = McpServer()
    server.run()


if __name__ == "__main__":
    main()
