"""MCP (Model Context Protocol) client for the AI Editor.

Supports three transport modes:
  - **stdio** — spawn subprocess, JSON-RPC over stdin/stdout
  - **sse** — HTTP+SSE to remote endpoint
  - **internal** — Python-native tools registered directly by plugins

External MCP config sources (in priority order):
1. settings ``ai_editor_mcp_servers`` list and ``ai_editor.mcp`` servers
2. Workspace ``mcp.json`` / ``.mcp/mcp.json`` / ``.vscode/mcp.json``
3. User home ``~/.sao/mcp.json``
4. Plugin manifests (``plugin.json`` → ``mcpServers``)

Internal (plugin) MCP:
  Plugins call ``mcp_manager.register_internal(server_id, tools)``
  to expose Python functions as MCP tools without spawning a process.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Tuple

from ai_editor.tool_registry import normalize_tool_parameters


@dataclass
class McpToolDef:
    name: str
    description: str
    input_schema: Dict[str, Any]
    server_id: str


@dataclass
class McpServerConfig:
    id: str
    name: str
    transport: str = "stdio"  # "stdio" | "sse"
    command: str = ""          # for stdio
    args: List[str] = field(default_factory=list)
    env: Dict[str, str] = field(default_factory=dict)
    url: str = ""              # for sse
    headers: Dict[str, str] = field(default_factory=dict)
    enabled: bool = True


class McpStdioClient:
    """JSON-RPC over stdin/stdout for a single MCP server."""

    def __init__(self, config: McpServerConfig) -> None:
        self.config = config
        self._proc: Optional[subprocess.Popen] = None
        self._req_id = 0
        self._lock = threading.Lock()
        self._pending: Dict[int, threading.Event] = {}
        self._results: Dict[int, Any] = {}
        self._reader_thread: Optional[threading.Thread] = None
        self.tools: List[McpToolDef] = []
        self._alive = False

    def start(self) -> bool:
        try:
            if not self.config.command:
                print(f"[MCP] Missing stdio command for {self.config.id}")
                return False
            env = {**os.environ, **self.config.env}
            self._proc = subprocess.Popen(
                [self.config.command, *self.config.args],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                env=env,
                bufsize=8192,
            )
            self._alive = True
            self._reader_thread = threading.Thread(target=self._read_loop, daemon=True)
            self._reader_thread.start()
            if not self._initialize():
                self.stop()
                return False
            return True
        except Exception as exc:
            print(f"[MCP] Failed to start {self.config.id}: {exc}")
            return False

    def stop(self) -> None:
        self._alive = False
        if self._proc:
            try:
                self._proc.stdin.close()
                self._proc.terminate()
                self._proc.wait(timeout=5)
            except Exception:
                try:
                    self._proc.kill()
                except Exception:
                    pass
            self._proc = None

    def _send(self, method: str, params: Any = None) -> int:
        with self._lock:
            self._req_id += 1
            rid = self._req_id
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        raw = json.dumps(msg) + "\n"
        try:
            self._proc.stdin.write(raw.encode("utf-8"))
            self._proc.stdin.flush()
        except Exception:
            pass
        return rid

    def _notify(self, method: str, params: Any = None) -> None:
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        raw = json.dumps(msg) + "\n"
        try:
            self._proc.stdin.write(raw.encode("utf-8"))
            self._proc.stdin.flush()
        except Exception:
            pass

    def _call(self, method: str, params: Any = None, timeout: float = 30.0) -> Any:
        if not self._alive:
            return None
        evt = threading.Event()
        rid = self._send(method, params)
        self._pending[rid] = evt
        completed = evt.wait(timeout=timeout)
        self._pending.pop(rid, None)
        if not completed:
            self._results.pop(rid, None)
            return None
        return self._results.pop(rid, None)

    def _read_loop(self) -> None:
        while self._alive and self._proc and self._proc.stdout:
            try:
                line = self._proc.stdout.readline()
                if not line:
                    break
                data = json.loads(line.decode("utf-8", errors="replace"))
                rid = data.get("id")
                if rid is not None and rid in self._pending:
                    if "error" in data:
                        self._results[rid] = {"_rpc_error": data.get("error")}
                    else:
                        self._results[rid] = data.get("result")
                    self._pending.pop(rid).set()
            except json.JSONDecodeError:
                continue
            except Exception:
                break
        self._alive = False
        for evt in self._pending.values():
            evt.set()

    def _initialize(self) -> bool:
        result = self._call("initialize", {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "sao-ai-editor", "version": "1.0.0"},
        })
        if result is None or _is_rpc_error(result):
            return False
        self._notify("notifications/initialized")
        self._discover_tools()
        return True

    def _discover_tools(self) -> None:
        result = self._call("tools/list", {})
        if not result or not isinstance(result, dict):
            return
        self.tools = []
        for t in result.get("tools", []):
            self.tools.append(McpToolDef(
                name=t.get("name", ""),
                description=t.get("description", ""),
                input_schema=normalize_tool_parameters(
                    t.get("inputSchema", {"type": "object", "properties": {}})
                ),
                server_id=self.config.id,
            ))

    def call_tool(self, name: str, arguments: Dict[str, Any]) -> str:
        result = self._call("tools/call", {"name": name, "arguments": arguments})
        if result is None:
            return json.dumps({"error": "MCP call timeout"})
        if _is_rpc_error(result):
            return json.dumps({
                "error": f"MCP call failed: {_rpc_error_message(result)}",
                "rpc_error": result.get("_rpc_error"),
            }, ensure_ascii=False)
        if isinstance(result, dict):
            content = result.get("content", [])
            texts = [c.get("text", "") for c in content if c.get("type") == "text"]
            if texts:
                return "\n".join(texts)
            return json.dumps(result, ensure_ascii=False)
        return str(result)

    def read_resource(self, uri: str) -> str:
        result = self._call("resources/read", {"uri": uri})
        if result and isinstance(result, dict):
            contents = result.get("contents", [])
            if contents:
                return contents[0].get("text", "") or json.dumps(contents[0])
        return ""

    @property
    def is_alive(self) -> bool:
        return self._alive and self._proc is not None and self._proc.poll() is None


class McpSseClient:
    """HTTP+SSE transport for MCP servers."""

    def __init__(self, config: McpServerConfig) -> None:
        self.config = config
        self.tools: List[McpToolDef] = []
        self._session_url: str = ""
        self._http: Any = None

    def _client(self):
        if self._http is None:
            import httpx
            self._http = httpx.Client(timeout=30.0)
        return self._http

    def start(self) -> bool:
        try:
            resp = self._client().get(self.config.url, headers=self.config.headers)
            resp.raise_for_status()
            for line in resp.text.split("\n"):
                if line.startswith("data:"):
                    data = json.loads(line[5:].strip())
                    if "sessionUrl" in data:
                        self._session_url = data["sessionUrl"]
                        break
            if not self._session_url:
                self._session_url = self.config.url
            self._discover_tools()
            return True
        except Exception as exc:
            print(f"[MCP/SSE] Failed to connect {self.config.id}: {exc}")
            return False

    def stop(self) -> None:
        if self._http:
            try:
                self._http.close()
            except Exception:
                pass
            self._http = None

    def _rpc(self, method: str, params: Any = None) -> Any:
        body = {"jsonrpc": "2.0", "id": 1, "method": method}
        if params:
            body["params"] = params
        resp = self._client().post(self._session_url or self.config.url,
                                   json=body, headers=self.config.headers)
        resp.raise_for_status()
        data = resp.json()
        if "error" in data:
            return {"_rpc_error": data.get("error")}
        return data.get("result")

    def _discover_tools(self) -> None:
        result = self._rpc("tools/list", {})
        if not result:
            return
        self.tools = []
        for t in result.get("tools", []):
            self.tools.append(McpToolDef(
                name=t.get("name", ""),
                description=t.get("description", ""),
                input_schema=normalize_tool_parameters(
                    t.get("inputSchema", {"type": "object", "properties": {}})
                ),
                server_id=self.config.id,
            ))

    def call_tool(self, name: str, arguments: Dict[str, Any]) -> str:
        result = self._rpc("tools/call", {"name": name, "arguments": arguments})
        if result is None:
            return json.dumps({"error": "MCP call failed"})
        if _is_rpc_error(result):
            return json.dumps({
                "error": f"MCP call failed: {_rpc_error_message(result)}",
                "rpc_error": result.get("_rpc_error"),
            }, ensure_ascii=False)
        if isinstance(result, dict):
            content = result.get("content", [])
            texts = [c.get("text", "") for c in content if c.get("type") == "text"]
            if texts:
                return "\n".join(texts)
            return json.dumps(result, ensure_ascii=False)
        return str(result)

    @property
    def is_alive(self) -> bool:
        return bool(self._session_url or self.config.url)


# ---------------------------------------------------------------------------
# MCP Manager — orchestrates multiple servers
# ---------------------------------------------------------------------------

class InternalMcpProvider:
    """Python-native MCP provider — tools registered directly, no subprocess.

    Usage by plugins::

        provider = InternalMcpProvider("my_plugin")
        provider.add_tool("get_hp", "Read player HP", {"type":"object","properties":{}},
                          handler=lambda **kw: {"hp": 50000})
        mcp_manager.register_provider(provider)
    """

    def __init__(self, server_id: str, name: str = "") -> None:
        self.config = McpServerConfig(id=server_id, name=name or server_id, transport="internal")
        self.tools: List[McpToolDef] = []
        self._handlers: Dict[str, Callable] = {}

    def add_tool(
        self,
        name: str,
        description: str,
        input_schema: Dict[str, Any],
        handler: Callable[..., Any],
    ) -> None:
        self.tools.append(McpToolDef(
            name=name,
            description=description,
            input_schema=normalize_tool_parameters(input_schema),
            server_id=self.config.id,
        ))
        self._handlers[name] = handler

    def start(self) -> bool:
        return True

    def stop(self) -> None:
        pass

    def call_tool(self, name: str, arguments: Dict[str, Any]) -> str:
        handler = self._handlers.get(name)
        if not handler:
            return json.dumps({"error": f"Tool not found: {name}"})
        try:
            result = handler(**arguments) if isinstance(arguments, dict) else handler(arguments)
            if isinstance(result, str):
                return result
            return json.dumps(result, ensure_ascii=False, default=str)
        except Exception as exc:
            return json.dumps({"error": str(exc)})

    @property
    def is_alive(self) -> bool:
        return True


class McpManager:
    """Manages multiple MCP server connections and unified tool dispatch."""

    def __init__(self) -> None:
        self._clients: Dict[str, Any] = {}  # McpStdioClient | McpSseClient | InternalMcpProvider

    def add_server(self, config: McpServerConfig) -> bool:
        if not config.enabled:
            return False
        if config.id in self._clients:
            self.remove_server(config.id)
        transport = str(config.transport or "stdio").strip().lower()
        config.transport = transport
        if transport == "internal":
            return False  # use register_provider() for internal
        if transport == "sse":
            if not config.url:
                print(f"[MCP] Missing SSE URL for {config.id}")
                return False
            client = McpSseClient(config)
        elif transport == "stdio":
            if not config.command:
                print(f"[MCP] Missing stdio command for {config.id}")
                return False
            client = McpStdioClient(config)
        else:
            print(f"[MCP] Unsupported transport for {config.id}: {transport}")
            return False
        ok = client.start()
        if ok:
            self._clients[config.id] = client
        return ok

    def register_provider(self, provider: InternalMcpProvider) -> None:
        """Register an internal (Python-native) MCP provider."""
        self._clients[provider.config.id] = provider

    def register_internal(
        self,
        server_id: str,
        tools: List[Dict[str, Any]],
        handlers: Optional[Dict[str, Callable]] = None,
    ) -> InternalMcpProvider:
        """Convenience: create and register an internal provider from a tools list.

        Each tool dict: {"name": "...", "description": "...", "inputSchema": {...}}
        handlers: {"tool_name": callable} — missing handlers return explicit errors.
        """
        provider = InternalMcpProvider(server_id)
        handlers = handlers or {}
        for t in tools:
            name = t.get("name", "")
            handler = handlers.get(name)
            if handler is None:
                def _missing_handler(_tool_name=name, **_kw: Any) -> Dict[str, Any]:
                    return {
                        "error": (
                            f"Internal MCP tool '{_tool_name}' has no registered handler; "
                            "register a callable handler before invoking it."
                        )
                    }
                handler = _missing_handler
            provider.add_tool(
                name=name,
                description=t.get("description", ""),
                input_schema=t.get("inputSchema", {"type": "object", "properties": {}}),
                handler=handler,
            )
        self._clients[server_id] = provider
        return provider

    def remove_server(self, server_id: str) -> None:
        client = self._clients.pop(server_id, None)
        if client:
            client.stop()

    def list_servers(self) -> List[Dict[str, Any]]:
        return [
            {
                "id": sid,
                "alive": c.is_alive,
                "tools": len(c.tools),
                "transport": c.config.transport,
            }
            for sid, c in self._clients.items()
        ]

    def all_tools(self) -> List[McpToolDef]:
        tools = []
        for c in self._clients.values():
            if c.is_alive:
                tools.extend(c.tools)
        return tools

    def to_openai_tools(self) -> List[Dict[str, Any]]:
        return [
            {
                "type": "function",
                "function": {
                    "name": f"mcp_{t.server_id}_{t.name}",
                    "description": f"[MCP:{t.server_id}] {t.description}",
                    "parameters": normalize_tool_parameters(t.input_schema),
                },
            }
            for t in self.all_tools()
        ]

    def call_tool(self, prefixed_name: str, arguments: Dict[str, Any]) -> str:
        """Dispatch a tool call by prefixed name (mcp_<server>_<tool>)."""
        if not prefixed_name.startswith("mcp_"):
            return json.dumps({"error": f"Not an MCP tool: {prefixed_name}"})
        if arguments is None:
            arguments = {}
        if not isinstance(arguments, dict):
            return json.dumps({
                "error": f"MCP tool arguments for {prefixed_name} must be a JSON object"
            }, ensure_ascii=False)
        resolved = self._resolve_tool(prefixed_name)
        if resolved["error"]:
            return json.dumps({"error": resolved["error"]}, ensure_ascii=False)
        client = resolved["client"]
        if client is None:
            return json.dumps({"error": f"MCP tool resolution failed for: {prefixed_name}"}, ensure_ascii=False)
        if not client.is_alive:
            return json.dumps({
                "error": f"MCP server '{resolved['server_id']}' is not connected"
            }, ensure_ascii=False)
        return client.call_tool(resolved["tool_name"], arguments)

    def _resolve_tool(self, prefixed_name: str) -> Dict[str, Any]:
        rest = prefixed_name[4:]
        matches: List[Tuple[int, str, str, Any]] = []
        for sid, client in self._clients.items():
            prefix = f"{sid}_"
            if rest.startswith(prefix):
                matches.append((len(prefix), sid, rest[len(prefix):], client))
        if not matches:
            return {"client": None, "server_id": "", "tool_name": "", "error": f"MCP server not found for: {prefixed_name}"}

        matches.sort(key=lambda item: item[0], reverse=True)
        for _, sid, tool_name, client in matches:
            if any(t.name == tool_name for t in client.tools):
                return {"client": client, "server_id": sid, "tool_name": tool_name, "error": ""}

        _, sid, tool_name, client = matches[0]
        available = [t.name for t in client.tools]
        if available:
            return {
                "client": None,
                "server_id": sid,
                "tool_name": tool_name,
                "error": (
                    f"MCP tool not found on server '{sid}': {tool_name}. "
                    f"Available: {', '.join(sorted(available))}"
                ),
            }
        return {
            "client": None,
            "server_id": sid,
            "tool_name": tool_name,
            "error": f"MCP server '{sid}' exposes no tools",
        }

    def shutdown(self) -> None:
        for c in list(self._clients.values()):
            try:
                c.stop()
            except Exception:
                pass
        self._clients.clear()


def load_mcp_configs(settings_get: Callable = None) -> List[McpServerConfig]:
    """Load MCP server configs from settings or mcp.json."""
    configs: List[McpServerConfig] = []
    seen_ids: set[str] = set()
    discovery_enabled = True
    normalized_autostart = False
    collision_behavior = "first"

    # From settings
    if settings_get:
        raw = settings_get("ai_editor_mcp_servers", [])
        _append_server_configs(configs, seen_ids, raw)

        ai_editor = settings_get("ai_editor", {})
        has_mcp_settings = isinstance(ai_editor, dict) and isinstance(ai_editor.get("mcp"), dict)
        mcp_settings = ai_editor.get("mcp", {}) if isinstance(ai_editor, dict) else {}
        if has_mcp_settings:
            if str(mcp_settings.get("access", "")).strip().lower() == "disabled":
                return []
            if not _as_bool(mcp_settings.get("enabled"), True):
                return []
            discovery_enabled = _as_bool(mcp_settings.get("discovery_enabled"), True)
            normalized_autostart = _as_bool(mcp_settings.get("autostart"), False)
            raw_collision = str(mcp_settings.get("collision_behavior", "first")).strip().lower()
            if raw_collision in {"first", "last", "error"}:
                collision_behavior = raw_collision
            if normalized_autostart:
                _append_server_configs(configs, seen_ids, mcp_settings.get("servers", []), collision_behavior)
                _append_server_configs(configs, seen_ids, mcp_settings.get("mcpServers", {}), collision_behavior)

    if settings_get and has_mcp_settings and (not discovery_enabled or not normalized_autostart):
        return []

    if not discovery_enabled or not normalized_autostart:
        return configs

    # From mcp.json in workspace
    for candidate in ["mcp.json", ".mcp/mcp.json", ".vscode/mcp.json"]:
        try:
            from config import BASE_DIR
            path = os.path.join(BASE_DIR, candidate)
        except ImportError:
            path = os.path.join(os.path.dirname(__file__), "..", candidate)
        if os.path.isfile(path):
            try:
                with open(path, "r", encoding="utf-8") as f:
                    data = json.load(f)
                servers = data.get("mcpServers") or data.get("servers") or {}
                _append_server_configs(configs, seen_ids, servers, collision_behavior)
            except Exception:
                pass

    # From user home ~/.sao/mcp.json
    try:
        home_mcp = os.path.join(os.path.expanduser("~"), ".sao", "mcp.json")
        if os.path.isfile(home_mcp):
            with open(home_mcp, "r", encoding="utf-8") as f:
                data = json.load(f)
            servers = data.get("mcpServers") or data.get("servers") or {}
            _append_server_configs(configs, seen_ids, servers, collision_behavior)
    except Exception:
        pass

    # From plugin manifests (plugins/*/plugin.json → mcpServers)
    try:
        from config import BASE_DIR
        plugins_dir = os.path.join(BASE_DIR, "plugins")
    except ImportError:
        plugins_dir = os.path.join(os.path.dirname(__file__), "..", "plugins")
    if os.path.isdir(plugins_dir):
        for pname in os.listdir(plugins_dir):
            manifest = os.path.join(plugins_dir, pname, "plugin.json")
            if not os.path.isfile(manifest):
                continue
            try:
                with open(manifest, "r", encoding="utf-8") as f:
                    pdata = json.load(f)
                for sid, sconf in (pdata.get("mcpServers") or {}).items():
                    full_id = f"{pname}.{sid}"
                    _append_server_configs(configs, seen_ids, {full_id: sconf}, collision_behavior)
            except Exception:
                continue

    return configs


def _append_server_configs(
    configs: List[McpServerConfig],
    seen_ids: set[str],
    raw: Any,
    collision_behavior: str = "first",
) -> None:
    for sid, sconf in _iter_server_configs(raw):
        if not sid:
            continue
        if sid in seen_ids:
            if collision_behavior == "last":
                configs[:] = [c for c in configs if c.id != sid]
                seen_ids.discard(sid)
            elif collision_behavior == "error":
                raise ValueError(f"Duplicate MCP server id: {sid}")
            else:
                continue
        config = _parse_server_config(sid, sconf)
        seen_ids.add(sid)
        if config.enabled:
            configs.append(config)


def _iter_server_configs(raw: Any) -> List[Tuple[str, Dict[str, Any]]]:
    if isinstance(raw, dict):
        entries = []
        for sid, sconf in raw.items():
            if isinstance(sconf, dict):
                entries.append((str(sid), sconf))
        return entries
    if isinstance(raw, list):
        entries = []
        for entry in raw:
            if not isinstance(entry, dict):
                continue
            sid = entry.get("id")
            if sid:
                entries.append((str(sid), entry))
        return entries
    return []


def _as_bool(value: Any, default: bool = True) -> bool:
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


def _as_string_list(value: Any) -> List[str]:
    if not isinstance(value, list):
        return []
    return [str(item) for item in value]


def _as_string_dict(value: Any) -> Dict[str, str]:
    if not isinstance(value, dict):
        return {}
    return {str(k): str(v) for k, v in value.items()}


def _parse_server_config(sid: str, sconf: Dict[str, Any]) -> McpServerConfig:
    return McpServerConfig(
        id=sid,
        name=sconf.get("name", sid),
        transport=str(sconf.get("transport", "stdio")),
        command=sconf.get("command", ""),
        args=_as_string_list(sconf.get("args", [])),
        env=_as_string_dict(sconf.get("env", {})),
        url=sconf.get("url", ""),
        headers=_as_string_dict(sconf.get("headers", {})),
        enabled=_as_bool(sconf.get("enabled"), True),
    )


def _is_rpc_error(result: Any) -> bool:
    return isinstance(result, dict) and "_rpc_error" in result


def _rpc_error_message(result: Any) -> str:
    if not _is_rpc_error(result):
        return "Unknown RPC error"
    error = result.get("_rpc_error")
    if isinstance(error, dict):
        message = str(error.get("message") or "").strip()
        code = error.get("code")
        if message and code is not None:
            return f"{message} (code={code})"
        if message:
            return message
    return str(error or "Unknown RPC error")
