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

import collections
import json
import inspect
import os
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Tuple
from urllib.parse import urljoin

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


def _rpc_error_result(message: str, code: int = -32000,
                      data: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    payload: Dict[str, Any] = {"message": str(message), "code": code}
    if data:
        payload["data"] = data
    return {"_rpc_error": payload}


def _extract_tool_call_result(result: Any) -> str:
    if result is None:
        return json.dumps({"error": "MCP tool returned no result"}, ensure_ascii=False)
    if isinstance(result, dict):
        content = result.get("content", [])
        texts = [c.get("text", "") for c in content if c.get("type") == "text"]
        if texts:
            return "\n".join(texts)
        return json.dumps(result, ensure_ascii=False, default=str)
    if isinstance(result, str):
        return result
    return json.dumps(result, ensure_ascii=False, default=str)


def _extract_resource_read_result(result: Any, uri: str = "") -> Dict[str, Any]:
    if _is_rpc_error(result):
        return {
            "ok": False,
            "error": f"MCP resource read failed: {_rpc_error_message(result)}",
            "rpc_error": result.get("_rpc_error") if isinstance(result, dict) else None,
            "uri": uri,
        }
    if result is None:
        return {"ok": False, "error": "MCP resource returned no result", "uri": uri}
    if isinstance(result, str):
        return {
            "ok": True,
            "uri": uri,
            "content": result,
            "mimeType": "text/plain",
            "contentType": "text",
        }
    if not isinstance(result, dict):
        return {
            "ok": True,
            "uri": uri,
            "content": json.dumps(result, ensure_ascii=False, default=str),
            "mimeType": "application/json",
            "contentType": "json",
        }
    contents = result.get("contents")
    if not isinstance(contents, list) or not contents:
        return {
            "ok": True,
            "uri": uri,
            "content": json.dumps(result, ensure_ascii=False, default=str),
            "mimeType": "application/json",
            "contentType": "json",
        }
    first = contents[0] if isinstance(contents[0], dict) else {"text": contents[0]}
    resource_uri = str(first.get("uri") or uri or "")
    mime = str(first.get("mimeType") or first.get("mime_type") or "")
    if first.get("text") is not None:
        return {
            "ok": True,
            "uri": resource_uri,
            "content": str(first.get("text") or ""),
            "mimeType": mime or "text/plain",
            "contentType": "text",
            "contents": contents,
        }
    if first.get("blob") is not None:
        content_type = "image" if mime.lower().startswith("image/") else "blob"
        return {
            "ok": True,
            "uri": resource_uri,
            "blob": str(first.get("blob") or ""),
            "mimeType": mime or "application/octet-stream",
            "contentType": content_type,
            "contents": contents,
        }
    return {
        "ok": True,
        "uri": resource_uri,
        "content": json.dumps(first, ensure_ascii=False, default=str),
        "mimeType": mime or "application/json",
        "contentType": "json",
        "contents": contents,
    }


def _read_rpc_message_from_stream(stream: Any) -> Optional[bytes]:
    first_line = stream.readline()
    if not first_line:
        return None
    stripped = first_line.strip()
    if not stripped:
        return b""
    if stripped.lower().startswith(b"content-length:"):
        try:
            length = int(stripped.split(b":", 1)[1].strip())
        except (IndexError, ValueError):
            return b""
        while True:
            header_line = stream.readline()
            if not header_line:
                return None
            if header_line in {b"\r\n", b"\n", b""}:
                break
        if length <= 0:
            return b""
        payload = stream.read(length)
        if not payload or len(payload) < length:
            return None
        return payload
    return first_line


def _parse_sse_event_payloads(raw_text: str) -> List[str]:
    payloads: List[str] = []
    data_lines: List[str] = []
    for line in str(raw_text or "").replace("\r\n", "\n").split("\n"):
        if not line:
            if data_lines:
                payloads.append("\n".join(data_lines).strip())
                data_lines = []
            continue
        if line.startswith(":"):
            continue
        if line.startswith("data:"):
            data_lines.append(line[5:].lstrip())
    if data_lines:
        payloads.append("\n".join(data_lines).strip())
    return [payload for payload in payloads if payload]


def _resolve_sse_session_url(base_url: str, raw_text: str) -> str:
    raw_text = str(raw_text or "").strip()
    if not raw_text:
        return ""
    candidates = _parse_sse_event_payloads(raw_text) or [raw_text]
    for payload in candidates:
        try:
            decoded = json.loads(payload)
        except json.JSONDecodeError:
            continue
        if not isinstance(decoded, dict):
            continue
        for key in ("sessionUrl", "session_url", "url"):
            value = decoded.get(key)
            if isinstance(value, str) and value.strip():
                return urljoin(base_url, value.strip())
    return ""


def _invoke_handler(handler: Callable[..., Any], arguments: Any) -> Any:
    if not isinstance(arguments, dict):
        return handler(arguments)
    try:
        signature = inspect.signature(handler)
    except (TypeError, ValueError):
        return handler(**arguments)

    parameters = list(signature.parameters.values())
    if not parameters:
        return handler()

    positional = [
        p for p in parameters
        if p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD)
    ]
    has_varkw = any(p.kind == p.VAR_KEYWORD for p in parameters)
    keyword_only = [p for p in parameters if p.kind == p.KEYWORD_ONLY]

    if has_varkw or keyword_only:
        return handler(**arguments)

    if len(positional) == 1:
        param = positional[0]
        if param.kind == param.POSITIONAL_ONLY:
            return handler(arguments)
        if arguments and set(arguments.keys()).issubset({param.name}):
            return handler(**arguments)
        return handler(arguments)

    return handler(**arguments)


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
        self._stderr_thread: Optional[threading.Thread] = None
        self._log_buf: collections.deque = collections.deque(maxlen=200)
        self.tools: List[McpToolDef] = []
        self._alive = False
        self._last_error = ""

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
                stderr=subprocess.PIPE,
                env=env,
                bufsize=8192,
            )
            self._alive = True
            self._reader_thread = threading.Thread(target=self._read_loop, daemon=True)
            self._reader_thread.start()
            self._stderr_thread = threading.Thread(target=self._stderr_loop, daemon=True)
            self._stderr_thread.start()
            if not self._initialize():
                self.stop()
                return False
            return True
        except Exception as exc:
            print(f"[MCP] Failed to start {self.config.id}: {exc}")
            return False

    def stop(self) -> None:
        self._alive = False
        proc = self._proc
        self._proc = None
        if proc:
            try:
                if proc.stdin:
                    proc.stdin.close()
            except Exception:
                self._last_error = "Failed to close MCP stdin"
            try:
                if proc.poll() is None:
                    proc.terminate()
                    proc.wait(timeout=5)
            except Exception:
                try:
                    proc.kill()
                except Exception:
                    self._last_error = "Failed to terminate MCP process"
            for stream_name in ("stdout", "stderr"):
                stream = getattr(proc, stream_name, None)
                if not stream:
                    continue
                try:
                    stream.close()
                except Exception:
                    self._last_error = f"Failed to close MCP {stream_name}"
        self._fail_pending(self._last_error or f"MCP server '{self.config.id}' stopped")
        reader = self._reader_thread
        if reader and reader.is_alive() and reader is not threading.current_thread():
            reader.join(timeout=1.0)
        self._reader_thread = None

    def _next_request_id(self) -> int:
        with self._lock:
            self._req_id += 1
            return self._req_id

    def _write_message(self, msg: Dict[str, Any]) -> None:
        proc = self._proc
        if not proc or not proc.stdin:
            raise RuntimeError("MCP stdio transport is not connected")
        raw = (json.dumps(msg) + "\n").encode("utf-8")
        proc.stdin.write(raw)
        proc.stdin.flush()

    def _notify(self, method: str, params: Any = None) -> bool:
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        try:
            self._write_message(msg)
            return True
        except Exception as exc:
            self._last_error = f"Failed to send MCP notification '{method}': {exc}"
            return False

    def _fail_pending(self, message: str) -> None:
        with self._lock:
            pending = list(self._pending.items())
            for rid, evt in pending:
                self._results[rid] = _rpc_error_result(message)
                evt.set()

    def _call(self, method: str, params: Any = None, timeout: float = 30.0) -> Any:
        if not self._alive:
            return _rpc_error_result(f"MCP server '{self.config.id}' is not connected")
        evt = threading.Event()
        rid = self._next_request_id()
        with self._lock:
            self._pending[rid] = evt
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        try:
            self._write_message(msg)
        except Exception as exc:
            with self._lock:
                self._pending.pop(rid, None)
            return _rpc_error_result(
                f"Failed to send MCP request '{method}': {exc}",
                data={"method": method, "serverId": self.config.id},
            )
        completed = evt.wait(timeout=timeout)
        with self._lock:
            self._pending.pop(rid, None)
            result = self._results.pop(rid, None)
        if not completed:
            return _rpc_error_result(
                f"MCP call timeout after {timeout:.1f}s",
                data={"method": method, "serverId": self.config.id},
            )
        if result is None:
            return _rpc_error_result(
                "MCP server disconnected before replying",
                data={"method": method, "serverId": self.config.id},
            )
        return result

    def _handle_message(self, data: Dict[str, Any]) -> None:
        rid = data.get("id")
        if rid is None:
            return
        with self._lock:
            evt = self._pending.get(rid)
            if evt is None:
                return
            if "error" in data:
                self._results[rid] = {"_rpc_error": data.get("error")}
            else:
                self._results[rid] = data.get("result")
            evt.set()

    def _read_loop(self) -> None:
        while self._alive and self._proc and self._proc.stdout:
            try:
                raw = _read_rpc_message_from_stream(self._proc.stdout)
                if raw is None:
                    break
                if not raw:
                    continue
                decoded = raw.decode("utf-8", errors="replace").strip()
                if not decoded:
                    continue
                data = json.loads(decoded)
                if isinstance(data, dict):
                    self._handle_message(data)
            except json.JSONDecodeError:
                self._last_error = "Received non-JSON data on MCP stdout"
                continue
            except Exception as exc:
                self._last_error = f"MCP stdout reader failed: {exc}"
                break
        self._alive = False
        self._fail_pending(self._last_error or f"MCP server '{self.config.id}' disconnected")

    def _stderr_loop(self) -> None:
        proc = self._proc
        if not proc or not proc.stderr:
            return
        try:
            for line in proc.stderr:
                if isinstance(line, bytes):
                    line = line.decode("utf-8", errors="replace")
                self._log_buf.append(line.rstrip("\n\r"))
        except Exception as exc:
            self._last_error = f"MCP stderr reader failed: {exc}"
            self._log_buf.append(self._last_error)

    def get_logs(self) -> List[str]:
        """Return recent stderr lines."""
        return list(self._log_buf)

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
        if _is_rpc_error(result):
            return json.dumps({
                "error": f"MCP call failed: {_rpc_error_message(result)}",
                "rpc_error": result.get("_rpc_error"),
            }, ensure_ascii=False)
        return _extract_tool_call_result(result)

    def read_resource(self, uri: str) -> Dict[str, Any]:
        result = self._call("resources/read", {"uri": uri})
        return _extract_resource_read_result(result, uri)

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
        self._alive = False

    def _client(self):
        if self._http is None:
            import httpx
            self._http = httpx.Client(timeout=30.0)
        return self._http

    def start(self) -> bool:
        try:
            resp = self._client().get(self.config.url, headers=self.config.headers)
            resp.raise_for_status()
            self._session_url = _resolve_sse_session_url(self.config.url, resp.text)
            if not self._session_url:
                self._session_url = self.config.url
            self._alive = True
            self._discover_tools()
            return True
        except Exception as exc:
            print(f"[MCP/SSE] Failed to connect {self.config.id}: {exc}")
            self.stop()
            return False

    def stop(self) -> None:
        self._alive = False
        if self._http:
            try:
                self._http.close()
            except Exception:
                self.tools = []
            self._http = None
        self._session_url = ""
        self.tools = []

    def _rpc(self, method: str, params: Any = None) -> Any:
        if not self._alive:
            return _rpc_error_result(f"MCP SSE server '{self.config.id}' is not connected")
        body = {"jsonrpc": "2.0", "id": 1, "method": method}
        if params:
            body["params"] = params
        try:
            resp = self._client().post(
                self._session_url or self.config.url,
                json=body,
                headers=self.config.headers,
            )
            resp.raise_for_status()
            data = resp.json()
        except Exception as exc:
            return _rpc_error_result(
                f"MCP SSE request failed: {exc}",
                data={"method": method, "serverId": self.config.id},
            )
        if not isinstance(data, dict):
            return _rpc_error_result(
                "MCP SSE response was not a JSON object",
                data={"method": method, "serverId": self.config.id},
            )
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
        if _is_rpc_error(result):
            return json.dumps({
                "error": f"MCP call failed: {_rpc_error_message(result)}",
                "rpc_error": result.get("_rpc_error"),
            }, ensure_ascii=False)
        return _extract_tool_call_result(result)

    def read_resource(self, uri: str) -> Dict[str, Any]:
        result = self._rpc("resources/read", {"uri": uri})
        return _extract_resource_read_result(result, uri)

    @property
    def is_alive(self) -> bool:
        return self._alive


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
        self._resource_handlers: Dict[str, Callable] = {}
        self._alive = True

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

    def add_resource(self, uri: str, handler: Callable[..., Any]) -> None:
        self._resource_handlers[str(uri or "")] = handler

    def start(self) -> bool:
        self._alive = True
        return True

    def stop(self) -> None:
        self._alive = False

    def call_tool(self, name: str, arguments: Dict[str, Any]) -> str:
        if not self._alive:
            return json.dumps({"error": f"MCP server '{self.config.id}' is not connected"}, ensure_ascii=False)
        handler = self._handlers.get(name)
        if not handler:
            return json.dumps({"error": f"Tool not found: {name}"})
        try:
            result = _invoke_handler(handler, arguments)
            if result is None:
                return json.dumps({
                    "error": f"Internal MCP tool '{name}' returned no result"
                }, ensure_ascii=False)
            if isinstance(result, str):
                return result
            return json.dumps(result, ensure_ascii=False, default=str)
        except Exception as exc:
            return json.dumps({"error": str(exc)})

    def read_resource(self, uri: str) -> Dict[str, Any]:
        if not self._alive:
            return {
                "ok": False,
                "error": f"MCP server '{self.config.id}' is not connected",
                "uri": uri,
            }
        handler = self._resource_handlers.get(str(uri or ""))
        if not handler:
            return {
                "ok": False,
                "error": f"MCP resource not found on server '{self.config.id}': {uri}",
                "uri": uri,
                "notFound": True,
            }
        try:
            result = _invoke_handler(handler, {"uri": uri})
            return _extract_resource_read_result(result, uri)
        except Exception as exc:
            return {"ok": False, "error": str(exc), "uri": uri}

    @property
    def is_alive(self) -> bool:
        return self._alive


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

    def restart_server(self, server_id: str) -> bool:
        """Stop and restart an MCP server, preserving its config."""
        client = self._clients.get(server_id)
        if not client:
            return False
        config = client.config
        client.stop()
        self._clients.pop(server_id, None)
        transport = str(config.transport or "stdio").strip().lower()
        if transport == "internal":
            return False
        if transport == "sse":
            new_client = McpSseClient(config)
        else:
            new_client = McpStdioClient(config)
        ok = new_client.start()
        if ok:
            self._clients[server_id] = new_client
        return ok

    def get_server_logs(self, server_id: str) -> List[str]:
        """Return recent stderr/stdout log lines for a stdio server."""
        client = self._clients.get(server_id)
        if client and hasattr(client, "get_logs"):
            return client.get_logs()
        return []

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

    @staticmethod
    def _parse_resource_uri(uri: str) -> Dict[str, str]:
        from urllib.parse import unquote, urlparse

        text = str(uri or "").strip()
        if not text:
            return {"server_id": "", "resource_uri": "", "error": "MCP resource URI is required"}
        parsed = urlparse(text)
        if parsed.scheme in {"mcp-resource", "mcp"}:
            if parsed.netloc:
                resource = unquote((parsed.path or "").lstrip("/")) or text
                return {"server_id": parsed.netloc, "resource_uri": resource, "error": ""}
            rest = text[len(parsed.scheme) + 1:]
            if ":" in rest:
                server_id, resource = rest.split(":", 1)
                return {"server_id": server_id.strip("/"), "resource_uri": resource, "error": ""}
            if "/" in rest:
                server_id, resource = rest.split("/", 1)
                return {"server_id": server_id.strip("/"), "resource_uri": unquote(resource), "error": ""}
        return {"server_id": "", "resource_uri": text, "error": ""}

    def read_resource(self, uri: str) -> Dict[str, Any]:
        parsed = self._parse_resource_uri(uri)
        if parsed.get("error"):
            return {"ok": False, "error": parsed["error"], "uri": uri}
        server_id = parsed.get("server_id", "")
        resource_uri = parsed.get("resource_uri", "") or uri
        client = None
        if server_id:
            client = self._clients.get(server_id)
            if client is None:
                return {
                    "ok": False,
                    "error": f"MCP server not found for resource: {server_id}",
                    "uri": uri,
                    "serverId": server_id,
                }
        else:
            alive = [
                c for c in self._clients.values()
                if getattr(c, "is_alive", False) and hasattr(c, "read_resource")
            ]
            if len(alive) != 1:
                return {
                    "ok": False,
                    "error": "MCP resource URI must identify a server when multiple or no MCP servers are connected",
                    "uri": uri,
                    "serverCount": len(alive),
                }
            client = alive[0]
            server_id = str(getattr(getattr(client, "config", None), "id", ""))
        if not getattr(client, "is_alive", False):
            return {
                "ok": False,
                "error": f"MCP server '{server_id}' is not connected",
                "uri": uri,
                "serverId": server_id,
            }
        if not hasattr(client, "read_resource"):
            return {
                "ok": False,
                "error": f"MCP server '{server_id}' does not support resource reads",
                "uri": uri,
                "serverId": server_id,
            }
        result = client.read_resource(resource_uri)
        if isinstance(result, dict):
            result.setdefault("serverId", server_id)
            result.setdefault("requestedUri", uri)
            result.setdefault("uri", resource_uri)
            return result
        return {
            "ok": True,
            "serverId": server_id,
            "requestedUri": uri,
            "uri": resource_uri,
            "content": str(result or ""),
            "mimeType": "text/plain",
            "contentType": "text",
        }

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
                continue
        self._clients.clear()


def load_mcp_configs(settings_get: Callable = None) -> List[McpServerConfig]:
    """Load MCP server configs from settings or mcp.json."""
    configs: List[McpServerConfig] = []
    seen_ids: set[str] = set()
    discovery_enabled = True
    normalized_autostart = False
    has_mcp_settings = False
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
                continue

    # From user home ~/.sao/mcp.json
    try:
        home_mcp = os.path.join(os.path.expanduser("~"), ".sao", "mcp.json")
        if os.path.isfile(home_mcp):
            with open(home_mcp, "r", encoding="utf-8") as f:
                data = json.load(f)
            servers = data.get("mcpServers") or data.get("servers") or {}
            _append_server_configs(configs, seen_ids, servers, collision_behavior)
    except Exception as exc:
        print(f"[MCP] Failed to load user mcp.json: {exc}")

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
