"""MCP (Model Context Protocol) client for the AI Editor.

Connects to MCP servers via stdio or HTTP/SSE, discovers tools, and
dispatches tool calls.  Registered MCP tools appear alongside engine
tools in the LLM's tool list.

Supports:
  - stdio transport (spawn subprocess, communicate via stdin/stdout JSON-RPC)
  - HTTP+SSE transport (POST to endpoint, stream results)
  - Tool discovery (tools/list)
  - Tool invocation (tools/call)
  - Resource reading (resources/read)
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
            self._initialize()
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

    def _call(self, method: str, params: Any = None, timeout: float = 30.0) -> Any:
        evt = threading.Event()
        rid = self._send(method, params)
        self._pending[rid] = evt
        evt.wait(timeout=timeout)
        return self._results.pop(rid, None)

    def _read_loop(self) -> None:
        while self._alive and self._proc and self._proc.stdout:
            try:
                line = self._proc.stdout.readline()
                if not line:
                    break
                data = json.loads(line.decode("utf-8", errors="replace"))
                rid = data.get("id")
                if rid and rid in self._pending:
                    self._results[rid] = data.get("result")
                    self._pending.pop(rid).set()
            except json.JSONDecodeError:
                continue
            except Exception:
                break
        self._alive = False

    def _initialize(self) -> None:
        result = self._call("initialize", {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "sao-ai-editor", "version": "1.0.0"},
        })
        self._call("notifications/initialized")
        self._discover_tools()

    def _discover_tools(self) -> None:
        result = self._call("tools/list", {})
        if not result or not isinstance(result, dict):
            return
        self.tools = []
        for t in result.get("tools", []):
            self.tools.append(McpToolDef(
                name=t.get("name", ""),
                description=t.get("description", ""),
                input_schema=t.get("inputSchema", {"type": "object", "properties": {}}),
                server_id=self.config.id,
            ))

    def call_tool(self, name: str, arguments: Dict[str, Any]) -> str:
        result = self._call("tools/call", {"name": name, "arguments": arguments})
        if result is None:
            return json.dumps({"error": "MCP call timeout"})
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
                input_schema=t.get("inputSchema", {"type": "object", "properties": {}}),
                server_id=self.config.id,
            ))

    def call_tool(self, name: str, arguments: Dict[str, Any]) -> str:
        result = self._rpc("tools/call", {"name": name, "arguments": arguments})
        if result is None:
            return json.dumps({"error": "MCP call failed"})
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

class McpManager:
    """Manages multiple MCP server connections and unified tool dispatch."""

    def __init__(self) -> None:
        self._clients: Dict[str, McpStdioClient | McpSseClient] = {}

    def add_server(self, config: McpServerConfig) -> bool:
        if config.id in self._clients:
            self.remove_server(config.id)
        if config.transport == "sse":
            client = McpSseClient(config)
        else:
            client = McpStdioClient(config)
        ok = client.start()
        if ok:
            self._clients[config.id] = client
        return ok

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
                    "parameters": t.input_schema,
                },
            }
            for t in self.all_tools()
        ]

    def call_tool(self, prefixed_name: str, arguments: Dict[str, Any]) -> str:
        """Dispatch a tool call by prefixed name (mcp_<server>_<tool>)."""
        if not prefixed_name.startswith("mcp_"):
            return json.dumps({"error": f"Not an MCP tool: {prefixed_name}"})
        rest = prefixed_name[4:]
        for sid, client in self._clients.items():
            prefix = f"{sid}_"
            if rest.startswith(prefix):
                tool_name = rest[len(prefix):]
                return client.call_tool(tool_name, arguments)
        return json.dumps({"error": f"MCP server not found for: {prefixed_name}"})

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
    # From settings
    if settings_get:
        raw = settings_get("ai_editor_mcp_servers", [])
        if isinstance(raw, list):
            for entry in raw:
                if isinstance(entry, dict) and entry.get("id"):
                    configs.append(McpServerConfig(
                        id=entry["id"],
                        name=entry.get("name", entry["id"]),
                        transport=entry.get("transport", "stdio"),
                        command=entry.get("command", ""),
                        args=entry.get("args", []),
                        env=entry.get("env", {}),
                        url=entry.get("url", ""),
                        headers=entry.get("headers", {}),
                    ))
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
                for sid, sconf in servers.items():
                    if sid not in {c.id for c in configs}:
                        configs.append(McpServerConfig(
                            id=sid,
                            name=sconf.get("name", sid),
                            transport=sconf.get("transport", "stdio"),
                            command=sconf.get("command", ""),
                            args=sconf.get("args", []),
                            env=sconf.get("env", {}),
                            url=sconf.get("url", ""),
                            headers=sconf.get("headers", {}),
                        ))
            except Exception:
                pass
    return configs
