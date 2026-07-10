"""MCP (Model Context Protocol) client for the AI Editor.

Supports four transport modes:
  - **stdio** — spawn subprocess, JSON-RPC over stdin/stdout
  - **streamable_http** / **http** — MCP 2025-06-18 Streamable HTTP
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
from typing import Any, Callable, Dict, Iterable, List, Optional, Tuple
from urllib.parse import urljoin

from ai_editor.tool_registry import normalize_tool_parameters


_MCP_PROTOCOL_VERSION = "2025-06-18"
_MCP_SUPPORTED_PROTOCOL_VERSIONS = {
    "2025-06-18",
    "2025-03-26",
}
_MCP_STREAMABLE_ACCEPT = "application/json, text/event-stream"
_MCP_MAX_LIST_PAGES = 100
_MCP_MAX_MESSAGE_BYTES = 4 * 1024 * 1024
_MCP_MAX_HEADER_LINE_BYTES = 8 * 1024
_MCP_MAX_HEADER_BYTES = 64 * 1024
_MCP_MAX_HEADER_COUNT = 100


class _McpHttpStatusError(RuntimeError):
    """HTTP status failure with enough detail for session recovery/fallback."""

    def __init__(self, status_code: int, message: str) -> None:
        super().__init__(message)
        self.status_code = int(status_code)


class _McpProtocolError(RuntimeError):
    """Malformed or oversized MCP transport frame."""


@dataclass
class McpToolDef:
    name: str
    description: str
    input_schema: Dict[str, Any]
    server_id: str
    annotations: Dict[str, Any] = field(default_factory=dict)
    trusted_server: bool = False


@dataclass
class McpServerConfig:
    id: str
    name: str
    transport: str = "stdio"  # "stdio" | "streamable_http" | "sse"
    command: str = ""          # for stdio
    args: List[str] = field(default_factory=list)
    env: Dict[str, str] = field(default_factory=dict)
    url: str = ""              # for streamable_http/http/sse
    headers: Dict[str, str] = field(default_factory=dict)
    enabled: bool = True
    trusted: bool = False
    inherit_env: List[str] = field(default_factory=list)


_SAFE_INHERITED_ENV = {
    "PATH", "PATHEXT", "SYSTEMROOT", "WINDIR", "COMSPEC",
    "TEMP", "TMP", "TMPDIR", "HOME", "USERPROFILE", "HOMEDRIVE",
    "HOMEPATH", "APPDATA", "LOCALAPPDATA", "PROGRAMDATA",
    "LANG", "LC_ALL", "PYTHONIOENCODING", "PYTHONUTF8",
    "PROCESSOR_ARCHITECTURE", "NUMBER_OF_PROCESSORS",
}


def _stdio_environment(config: McpServerConfig) -> Dict[str, str]:
    """Build a minimal child environment without leaking host credentials."""
    requested = {
        str(name).strip() for name in (config.inherit_env or [])
        if str(name).strip()
    }
    allowed = _SAFE_INHERITED_ENV | requested
    result = {
        key: str(value) for key, value in os.environ.items()
        if key.upper() in {name.upper() for name in allowed}
    }
    result.update({str(key): str(value) for key, value in config.env.items()})
    result.setdefault("PYTHONIOENCODING", "utf-8")
    return result


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


def _line_payload_size(raw_line: bytes) -> int:
    if raw_line.endswith(b"\r\n"):
        return len(raw_line) - 2
    if raw_line.endswith(b"\n"):
        return len(raw_line) - 1
    return len(raw_line)


def _read_bounded_line(stream: Any, max_bytes: int, label: str) -> bytes:
    # Two bytes cover CRLF; one extra byte makes an oversized line observable
    # without allowing ``readline`` to buffer an unbounded child-process line.
    raw_line = stream.readline(max_bytes + 3)
    payload_size = _line_payload_size(raw_line)
    if payload_size > max_bytes:
        raise _McpProtocolError(
            f"{label} length {payload_size} exceeds {max_bytes} bytes"
        )
    return raw_line


def _read_rpc_message_from_stream(stream: Any) -> Optional[bytes]:
    first_line = _read_bounded_line(
        stream, _MCP_MAX_MESSAGE_BYTES, "MCP message line")
    if not first_line:
        return None
    stripped = first_line.strip()
    if not stripped:
        return b""
    if stripped.lower().startswith(b"content-length:"):
        if _line_payload_size(first_line) > _MCP_MAX_HEADER_LINE_BYTES:
            raise _McpProtocolError(
                "MCP header line length "
                f"{_line_payload_size(first_line)} exceeds "
                f"{_MCP_MAX_HEADER_LINE_BYTES} bytes"
            )
        try:
            length = int(stripped.split(b":", 1)[1].strip())
        except (IndexError, ValueError):
            return b""
        if length > _MCP_MAX_MESSAGE_BYTES:
            raise _McpProtocolError(
                "MCP message Content-Length "
                f"{length} exceeds {_MCP_MAX_MESSAGE_BYTES} bytes"
            )
        header_bytes = len(first_line)
        header_count = 1
        while True:
            header_line = _read_bounded_line(
                stream, _MCP_MAX_HEADER_LINE_BYTES, "MCP header line")
            if not header_line:
                return None
            header_bytes += len(header_line)
            if header_bytes > _MCP_MAX_HEADER_BYTES:
                raise _McpProtocolError(
                    "MCP headers length "
                    f"{header_bytes} exceeds {_MCP_MAX_HEADER_BYTES} bytes"
                )
            if header_line in {b"\r\n", b"\n", b""}:
                break
            header_count += 1
            if header_count > _MCP_MAX_HEADER_COUNT:
                raise _McpProtocolError(
                    "MCP header count "
                    f"{header_count} exceeds {_MCP_MAX_HEADER_COUNT}"
                )
        if length <= 0:
            return b""
        payload = stream.read(length)
        if not payload or len(payload) < length:
            return None
        return payload
    return first_line


def _http_response_header(response: Any, name: str) -> str:
    headers = getattr(response, "headers", {}) or {}
    getter = getattr(headers, "get", None)
    if not callable(getter):
        return ""
    return str(getter(name, "") or getter(name.title(), "") or "").strip()


def _read_bounded_http_body(
    response: Any,
    max_bytes: int = _MCP_MAX_MESSAGE_BYTES,
) -> bytes:
    """Read one HTTP response body without buffering beyond ``max_bytes``."""
    declared = _http_response_header(response, "content-length")
    if declared:
        try:
            declared_bytes = int(declared)
        except ValueError as exc:
            raise _McpProtocolError(
                f"Invalid MCP HTTP Content-Length: {declared!r}"
            ) from exc
        if declared_bytes < 0:
            raise _McpProtocolError(
                f"Invalid MCP HTTP Content-Length: {declared_bytes}"
            )
        if declared_bytes > max_bytes:
            raise _McpProtocolError(
                "MCP HTTP response Content-Length "
                f"{declared_bytes} exceeds {max_bytes} bytes"
            )

    chunks = getattr(response, "iter_bytes", None)
    if callable(chunks):
        body = bytearray()
        for chunk in chunks():
            if not chunk:
                continue
            raw = chunk if isinstance(chunk, bytes) else bytes(chunk)
            next_size = len(body) + len(raw)
            if next_size > max_bytes:
                raise _McpProtocolError(
                    "MCP HTTP response body length "
                    f"{next_size} exceeds {max_bytes} bytes"
                )
            body.extend(raw)
        return bytes(body)

    # Compatibility path for the small response doubles used by integrations.
    # Real httpx traffic enters through ``Client.stream`` and ``iter_bytes``.
    content = getattr(response, "content", None)
    if isinstance(content, bytes):
        raw_body = content
    elif isinstance(content, str):
        raw_body = content.encode("utf-8")
    elif content is not None:
        raw_body = bytes(content)
    else:
        raw_body = str(getattr(response, "text", "") or "").encode("utf-8")
    if len(raw_body) > max_bytes:
        raise _McpProtocolError(
            "MCP HTTP response body length "
            f"{len(raw_body)} exceeds {max_bytes} bytes"
        )
    return raw_body


def _bounded_http_request(
    client: Any,
    method: str,
    url: str,
    **kwargs: Any,
) -> Tuple[Any, bytes]:
    """Issue an HTTP request and bound the decoded body before buffering it."""
    normalized_method = str(method or "GET").upper()
    stream = getattr(client, "stream", None)
    if callable(stream):
        with stream(normalized_method, url, **kwargs) as response:
            return response, _read_bounded_http_body(response)

    # Preserve compatibility with existing lightweight clients that expose
    # only ``post``/``delete``.  Production httpx clients always use the
    # streaming branch above.
    request = getattr(client, normalized_method.lower(), None)
    if not callable(request):
        raise RuntimeError(
            f"MCP HTTP client does not support {normalized_method} requests"
        )
    response = request(url, **kwargs)
    return response, _read_bounded_http_body(response)


def _iter_bounded_sse_lines(
    response: Any,
    max_line_bytes: int = _MCP_MAX_MESSAGE_BYTES,
) -> Iterable[Tuple[bytes, int]]:
    """Yield UTF-8 SSE lines while bounding the pending raw line buffer."""
    chunks = getattr(response, "iter_bytes", None)
    if not callable(chunks):
        raise _McpProtocolError(
            "MCP SSE response does not support bounded byte streaming"
        )

    pending = bytearray()
    for chunk in chunks():
        if not chunk:
            continue
        raw_chunk = chunk if isinstance(chunk, bytes) else bytes(chunk)
        start = 0
        while start < len(raw_chunk):
            newline = raw_chunk.find(b"\n", start)
            end = len(raw_chunk) if newline < 0 else newline
            segment = raw_chunk[start:end]
            next_size = len(pending) + len(segment)
            if next_size > max_line_bytes:
                raise _McpProtocolError(
                    "MCP SSE line length "
                    f"{next_size} exceeds {max_line_bytes} bytes"
                )
            pending.extend(segment)
            if newline < 0:
                break
            wire_size = len(pending) + 1
            line = bytes(pending[:-1] if pending.endswith(b"\r") else pending)
            pending.clear()
            yield line, wire_size
            start = newline + 1

    if pending:
        wire_size = len(pending)
        line = bytes(pending[:-1] if pending.endswith(b"\r") else pending)
        yield line, wire_size


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


def _parse_sse_json_messages(raw_text: str) -> List[Dict[str, Any]]:
    """Decode finite SSE response bodies into JSON-RPC messages."""
    messages: List[Dict[str, Any]] = []
    for payload in _parse_sse_event_payloads(raw_text):
        try:
            decoded = json.loads(payload)
        except json.JSONDecodeError:
            continue
        candidates = decoded if isinstance(decoded, list) else [decoded]
        messages.extend(item for item in candidates if isinstance(item, dict))
    return messages


def _resolve_sse_session_url(base_url: str, raw_text: str) -> str:
    raw_text = str(raw_text or "").strip()
    if not raw_text:
        return ""
    candidates = _parse_sse_event_payloads(raw_text) or [raw_text]
    for payload in candidates:
        # Legacy MCP endpoint events normally carry a URI directly rather than
        # wrapping it in JSON.  Accept both shapes for compatibility.
        if payload.startswith(("/", "http://", "https://")):
            return urljoin(base_url, payload)
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
            env = _stdio_environment(self.config)
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
                annotations=dict(t.get("annotations") or {}),
                trusted_server=bool(self.config.trusted),
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


class McpStreamableHttpClient:
    """Synchronous MCP 2025-06-18 Streamable HTTP client.

    Each JSON-RPC message uses its own POST.  POST responses may be a JSON
    object or a finite SSE stream containing notifications followed by the
    matching response.  Stateful servers are supported through
    ``Mcp-Session-Id`` and are re-initialized once after a session-expiry 404.
    """

    def __init__(self, config: McpServerConfig) -> None:
        self.config = config
        self.tools: List[McpToolDef] = []
        self._http: Any = None
        self._alive = False
        self._req_id = 0
        self._id_lock = threading.Lock()
        self._request_lock = threading.RLock()
        self._session_id = ""
        self._protocol_version = _MCP_PROTOCOL_VERSION
        self._server_capabilities: Dict[str, Any] = {}
        self._tools_dirty = False
        self._discovering_tools = False
        self._last_error = ""
        self._legacy_fallback_recommended = False

    def _client(self):
        if self._http is None:
            import httpx
            self._http = httpx.Client(timeout=30.0)
        return self._http

    def _next_request_id(self) -> int:
        with self._id_lock:
            self._req_id += 1
            return self._req_id

    def _headers(self, *, initializing: bool = False) -> Dict[str, str]:
        reserved = {
            "accept", "content-type", "mcp-session-id", "mcp-protocol-version",
        }
        headers = {
            str(key): str(value) for key, value in self.config.headers.items()
            if str(key).lower() not in reserved
        }
        headers["Accept"] = _MCP_STREAMABLE_ACCEPT
        headers["Content-Type"] = "application/json"
        if not initializing:
            headers["MCP-Protocol-Version"] = self._protocol_version
            if self._session_id:
                headers["Mcp-Session-Id"] = self._session_id
        # Initializing starts a new logical session, so reserved headers from
        # configuration are deliberately excluded above.
        return headers

    @staticmethod
    def _response_messages(
        response: Any,
        raw_body: Optional[bytes] = None,
    ) -> List[Dict[str, Any]]:
        body = (
            _read_bounded_http_body(response)
            if raw_body is None else raw_body
        )
        text = body.decode("utf-8", errors="replace")
        if not text.strip():
            return []
        content_type = _http_response_header(response, "content-type").lower()
        if "text/event-stream" in content_type or text.lstrip().startswith(("data:", "event:")):
            return _parse_sse_json_messages(text)
        try:
            decoded = json.loads(text)
        except Exception as exc:
            raise RuntimeError(f"MCP HTTP response was not valid JSON or SSE: {exc}") from exc
        candidates = decoded if isinstance(decoded, list) else [decoded]
        return [item for item in candidates if isinstance(item, dict)]

    def _observe_message(self, message: Dict[str, Any]) -> None:
        method = str(message.get("method") or "")
        if method == "notifications/tools/list_changed":
            self._tools_dirty = True

    def _send_message(self, message: Dict[str, Any], *, initializing: bool = False) -> Any:
        response, raw_body = _bounded_http_request(
            self._client(),
            "POST",
            self.config.url,
            json=message,
            headers=self._headers(initializing=initializing),
        )
        status_code = int(getattr(response, "status_code", 0) or 0)
        if status_code >= 400:
            detail = raw_body.decode("utf-8", errors="replace").strip()[:300]
            suffix = f": {detail}" if detail else ""
            raise _McpHttpStatusError(
                status_code,
                f"MCP HTTP request failed with status {status_code}{suffix}",
            )

        if initializing:
            session_id = str(response.headers.get("Mcp-Session-Id", "") or "")
            if session_id:
                if not all(0x21 <= ord(ch) <= 0x7E for ch in session_id):
                    raise RuntimeError("MCP server returned an invalid session identifier")
                self._session_id = session_id

        request_id = message.get("id")
        if request_id is None:
            # Notifications are acknowledged with 202 and no body.  Be liberal
            # enough to process a body if a server supplies one.
            for item in self._response_messages(response, raw_body):
                self._observe_message(item)
            return None

        matched = False
        matched_result: Any = None
        for item in self._response_messages(response, raw_body):
            self._observe_message(item)
            if item.get("id") != request_id:
                continue
            matched = True
            if "error" in item:
                matched_result = {"_rpc_error": item.get("error")}
            else:
                matched_result = item.get("result")
        if matched:
            return matched_result
        return _rpc_error_result(
            "MCP HTTP response did not contain the matching JSON-RPC response",
            data={"requestId": request_id, "serverId": self.config.id},
        )

    def _notify(self, method: str, params: Any = None) -> bool:
        message: Dict[str, Any] = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        try:
            self._send_message(message)
            return True
        except Exception as exc:
            self._last_error = f"Failed to send MCP notification '{method}': {exc}"
            return False

    def _initialize_session(self) -> bool:
        self._session_id = ""
        self._protocol_version = _MCP_PROTOCOL_VERSION
        request = {
            "jsonrpc": "2.0",
            "id": self._next_request_id(),
            "method": "initialize",
            "params": {
                "protocolVersion": _MCP_PROTOCOL_VERSION,
                "capabilities": {},
                "clientInfo": {"name": "sao-ai-editor", "version": "1.0.0"},
            },
        }
        result = self._send_message(request, initializing=True)
        if not isinstance(result, dict) or _is_rpc_error(result):
            self._last_error = (
                f"MCP initialize failed: {_rpc_error_message(result)}"
                if _is_rpc_error(result) else "MCP initialize returned no result"
            )
            return False
        negotiated = str(result.get("protocolVersion") or "").strip()
        if negotiated not in _MCP_SUPPORTED_PROTOCOL_VERSIONS:
            self._last_error = f"Unsupported MCP protocol version: {negotiated or '<missing>'}"
            return False
        self._protocol_version = negotiated
        self._server_capabilities = dict(result.get("capabilities") or {})
        if not self._notify("notifications/initialized"):
            return False
        return True

    def start(self) -> bool:
        if not self.config.url:
            self._last_error = f"Missing Streamable HTTP URL for {self.config.id}"
            return False
        self._alive = True
        self._legacy_fallback_recommended = False
        try:
            if not self._initialize_session():
                raise RuntimeError(self._last_error or "MCP initialization failed")
            self._discover_tools()
            return True
        except _McpHttpStatusError as exc:
            self._legacy_fallback_recommended = exc.status_code in {404, 405}
            self._last_error = str(exc)
            if self._legacy_fallback_recommended:
                print(
                    f"[MCP/HTTP] Streamable HTTP unavailable for {self.config.id}; "
                    "trying legacy SSE"
                )
            else:
                print(f"[MCP/HTTP] Failed to connect {self.config.id}: {exc}")
        except Exception as exc:
            self._last_error = str(exc)
            print(f"[MCP/HTTP] Failed to connect {self.config.id}: {exc}")
        self.stop(send_delete=False)
        return False

    def stop(self, *, send_delete: bool = True) -> None:
        session_id = self._session_id
        client = self._http
        if send_delete and client is not None and session_id:
            try:
                response, _raw_body = _bounded_http_request(
                    client, "DELETE", self.config.url, headers=self._headers())
                if int(getattr(response, "status_code", 0) or 0) not in {
                    200, 202, 204, 404, 405,
                }:
                    self._last_error = (
                        f"MCP session DELETE returned HTTP {response.status_code}"
                    )
            except Exception as exc:
                self._last_error = f"Failed to terminate MCP HTTP session: {exc}"
        self._alive = False
        self._session_id = ""
        self._server_capabilities = {}
        self._tools_dirty = False
        self.tools = []
        if client is not None:
            try:
                client.close()
            except Exception:
                pass
        self._http = None

    def _rpc(self, method: str, params: Any = None) -> Any:
        if not self._alive:
            return _rpc_error_result(
                f"MCP HTTP server '{self.config.id}' is not connected")
        with self._request_lock:
            for attempt in range(2):
                message: Dict[str, Any] = {
                    "jsonrpc": "2.0",
                    "id": self._next_request_id(),
                    "method": method,
                }
                if params is not None:
                    message["params"] = params
                try:
                    return self._send_message(message)
                except _McpHttpStatusError as exc:
                    if exc.status_code == 404 and self._session_id and attempt == 0:
                        try:
                            if self._initialize_session():
                                self._tools_dirty = True
                                continue
                            return _rpc_error_result(
                                self._last_error or "MCP session reinitialization failed",
                                data={"method": method, "serverId": self.config.id},
                            )
                        except Exception as init_exc:
                            self._last_error = f"MCP session reinitialization failed: {init_exc}"
                            return _rpc_error_result(
                                self._last_error,
                                data={"method": method, "serverId": self.config.id},
                            )
                    self._last_error = str(exc)
                    return _rpc_error_result(
                        f"MCP HTTP request failed: {exc}",
                        code=exc.status_code,
                        data={"method": method, "serverId": self.config.id},
                    )
                except Exception as exc:
                    self._last_error = str(exc)
                    return _rpc_error_result(
                        f"MCP HTTP request failed: {exc}",
                        data={"method": method, "serverId": self.config.id},
                    )
        return _rpc_error_result(
            "MCP HTTP request failed after session recovery",
            data={"method": method, "serverId": self.config.id},
        )

    def _discover_tools(self) -> bool:
        if self._discovering_tools:
            return False
        self._discovering_tools = True
        discovered: List[McpToolDef] = []
        cursor = ""
        seen_cursors = set()
        try:
            for _page in range(_MCP_MAX_LIST_PAGES):
                params = {"cursor": cursor} if cursor else {}
                result = self._rpc("tools/list", params)
                if not isinstance(result, dict) or _is_rpc_error(result):
                    self._last_error = (
                        f"MCP tools/list failed: {_rpc_error_message(result)}"
                        if _is_rpc_error(result) else "MCP tools/list returned no result"
                    )
                    return False
                raw_tools = result.get("tools", [])
                if isinstance(raw_tools, list):
                    for item in raw_tools:
                        if not isinstance(item, dict):
                            continue
                        discovered.append(McpToolDef(
                            name=str(item.get("name") or ""),
                            description=str(item.get("description") or ""),
                            input_schema=normalize_tool_parameters(
                                item.get("inputSchema", {
                                    "type": "object", "properties": {},
                                })
                            ),
                            server_id=self.config.id,
                            annotations=dict(item.get("annotations") or {}),
                            trusted_server=bool(self.config.trusted),
                        ))
                next_cursor = str(result.get("nextCursor") or "").strip()
                if not next_cursor:
                    self.tools = discovered
                    self._tools_dirty = False
                    return True
                if next_cursor in seen_cursors:
                    self._last_error = "MCP tools/list repeated a pagination cursor"
                    return False
                seen_cursors.add(next_cursor)
                cursor = next_cursor
            self._last_error = (
                f"MCP tools/list exceeded {_MCP_MAX_LIST_PAGES} pages")
            return False
        finally:
            self._discovering_tools = False

    def refresh_tools_if_needed(self) -> None:
        if self._alive and self._tools_dirty and not self._discovering_tools:
            self._discover_tools()

    def call_tool(self, name: str, arguments: Dict[str, Any]) -> str:
        result = self._rpc("tools/call", {"name": name, "arguments": arguments})
        if self._tools_dirty:
            self._discover_tools()
        if _is_rpc_error(result):
            return json.dumps({
                "error": f"MCP call failed: {_rpc_error_message(result)}",
                "rpc_error": result.get("_rpc_error"),
            }, ensure_ascii=False)
        return _extract_tool_call_result(result)

    def read_resource(self, uri: str) -> Dict[str, Any]:
        result = self._rpc("resources/read", {"uri": uri})
        if self._tools_dirty:
            self._discover_tools()
        return _extract_resource_read_result(result, uri)

    @property
    def legacy_fallback_recommended(self) -> bool:
        return self._legacy_fallback_recommended

    @property
    def is_alive(self) -> bool:
        return self._alive


class McpSseClient:
    """Legacy MCP 2024-11-05 HTTP+SSE transport.

    The GET stream stays open on a reader thread.  Requests are POSTed to the
    endpoint announced by the initial ``endpoint`` event and their responses
    are resolved from later ``message`` events.  Finite JSON/SSE POST responses
    remain supported for older hybrid servers.
    """

    def __init__(self, config: McpServerConfig) -> None:
        self.config = config
        self.tools: List[McpToolDef] = []
        self._session_url: str = ""
        self._http: Any = None
        self._alive = False
        self._req_id = 0
        self._lock = threading.Lock()
        self._pending: Dict[int, threading.Event] = {}
        self._results: Dict[int, Any] = {}
        self._ready = threading.Event()
        self._stop_event = threading.Event()
        self._reader_thread: Optional[threading.Thread] = None
        self._stream_response: Any = None
        self._last_error = ""
        self._tools_dirty = False
        self._discovering_tools = False

    def _client(self):
        if self._http is None:
            import httpx
            self._http = httpx.Client(timeout=30.0)
        return self._http

    def start(self) -> bool:
        if not self.config.url:
            return False
        self._alive = True
        self._ready.clear()
        self._stop_event.clear()
        self._reader_thread = threading.Thread(target=self._read_sse_loop, daemon=True)
        self._reader_thread.start()
        try:
            if not self._ready.wait(timeout=10.0) or not self._session_url:
                raise RuntimeError(self._last_error or "Timed out waiting for MCP SSE endpoint")
            result = self._rpc("initialize", {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "sao-ai-editor", "version": "1.0.0"},
            })
            if result is None or _is_rpc_error(result):
                raise RuntimeError(
                    f"MCP SSE initialize failed: {_rpc_error_message(result)}")
            if not self._notify("notifications/initialized"):
                raise RuntimeError(self._last_error or "MCP SSE initialized notification failed")
            self._discover_tools()
            return True
        except Exception as exc:
            print(f"[MCP/SSE] Failed to connect {self.config.id}: {exc}")
            self.stop()
            return False

    def stop(self) -> None:
        self._alive = False
        self._stop_event.set()
        stream_response = self._stream_response
        self._stream_response = None
        if stream_response is not None:
            try:
                stream_response.close()
            except Exception:
                pass
        self._fail_pending(self._last_error or f"MCP SSE server '{self.config.id}' stopped")
        reader = self._reader_thread
        if reader and reader.is_alive() and reader is not threading.current_thread():
            reader.join(timeout=1.0)
        self._reader_thread = None
        if self._http:
            try:
                self._http.close()
            except Exception:
                pass
            self._http = None
        self._session_url = ""
        self.tools = []
        self._tools_dirty = False

    def _next_request_id(self) -> int:
        with self._lock:
            self._req_id += 1
            return self._req_id

    def _fail_pending(self, message: str) -> None:
        with self._lock:
            for request_id, event in list(self._pending.items()):
                self._results[request_id] = _rpc_error_result(message)
                event.set()

    def _handle_message(self, message: Dict[str, Any]) -> None:
        if str(message.get("method") or "") == "notifications/tools/list_changed":
            self._tools_dirty = True
            return
        request_id = message.get("id")
        if request_id is None:
            return
        with self._lock:
            event = self._pending.get(request_id)
            if event is None:
                return
            self._results[request_id] = (
                {"_rpc_error": message.get("error")}
                if "error" in message else message.get("result")
            )
            event.set()

    def _handle_sse_event(self, event_name: str, payload: str) -> None:
        if event_name == "endpoint":
            session_url = _resolve_sse_session_url(
                self.config.url, f"data: {payload}\n\n")
            if session_url:
                self._session_url = session_url
                self._ready.set()
            return
        try:
            decoded = json.loads(payload)
        except json.JSONDecodeError:
            return
        candidates = decoded if isinstance(decoded, list) else [decoded]
        for item in candidates:
            if isinstance(item, dict):
                self._handle_message(item)

    def _read_sse_loop(self) -> None:
        event_name = b"message"
        event_bytes = 0
        data_payload = bytearray()
        data_line_count = 0

        def _dispatch_event() -> None:
            nonlocal event_name, event_bytes, data_line_count
            if data_line_count:
                self._handle_sse_event(
                    event_name.decode("utf-8", errors="replace"),
                    bytes(data_payload).decode("utf-8", errors="replace"),
                )
            event_name = b"message"
            event_bytes = 0
            data_payload.clear()
            data_line_count = 0

        try:
            import httpx
            headers = {str(key): str(value) for key, value in self.config.headers.items()}
            headers["Accept"] = "text/event-stream"
            timeout = httpx.Timeout(connect=10.0, read=None, write=30.0, pool=10.0)
            with self._client().stream(
                "GET", self.config.url, headers=headers, timeout=timeout,
            ) as response:
                self._stream_response = response
                response.raise_for_status()
                content_type = str(response.headers.get("content-type", "")).lower()
                if "text/event-stream" not in content_type:
                    raise RuntimeError("MCP legacy SSE endpoint did not return text/event-stream")
                for line, wire_size in _iter_bounded_sse_lines(response):
                    if self._stop_event.is_set():
                        break
                    if line == b"":
                        _dispatch_event()
                        continue
                    next_event_bytes = event_bytes + wire_size
                    if next_event_bytes > _MCP_MAX_MESSAGE_BYTES:
                        raise _McpProtocolError(
                            "MCP SSE event length "
                            f"{next_event_bytes} exceeds "
                            f"{_MCP_MAX_MESSAGE_BYTES} bytes"
                        )
                    event_bytes = next_event_bytes
                    if line.startswith(b":"):
                        continue
                    if line.startswith(b"event:"):
                        event_name = line[6:].strip() or b"message"
                    elif line.startswith(b"data:"):
                        payload = line[5:].lstrip()
                        separator_bytes = 1 if data_line_count else 0
                        next_payload_bytes = (
                            len(data_payload) + separator_bytes + len(payload)
                        )
                        if next_payload_bytes > _MCP_MAX_MESSAGE_BYTES:
                            raise _McpProtocolError(
                                "MCP SSE event data length "
                                f"{next_payload_bytes} exceeds "
                                f"{_MCP_MAX_MESSAGE_BYTES} bytes"
                            )
                        if separator_bytes:
                            data_payload.extend(b"\n")
                        data_payload.extend(payload)
                        data_line_count += 1
                if data_line_count:
                    _dispatch_event()
        except Exception as exc:
            if not self._stop_event.is_set():
                self._last_error = f"MCP SSE reader failed: {exc}"
        finally:
            self._stream_response = None
            self._ready.set()
            if not self._stop_event.is_set():
                self._alive = False
                self._fail_pending(
                    self._last_error or f"MCP SSE server '{self.config.id}' disconnected")

    def _consume_post_response(
        self,
        response: Any,
        raw_body: Optional[bytes] = None,
    ) -> None:
        body = (
            _read_bounded_http_body(response)
            if raw_body is None else raw_body
        )
        text = body.decode("utf-8", errors="replace")
        if not text.strip():
            return
        content_type = _http_response_header(response, "content-type").lower()
        if "text/event-stream" in content_type or text.lstrip().startswith(("event:", "data:")):
            messages = _parse_sse_json_messages(text)
        else:
            try:
                decoded = json.loads(text)
            except Exception:
                return
            candidates = decoded if isinstance(decoded, list) else [decoded]
            messages = [item for item in candidates if isinstance(item, dict)]
        for item in messages:
            self._handle_message(item)

    def _notify(self, method: str, params: Any = None) -> bool:
        message: Dict[str, Any] = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        try:
            response, raw_body = _bounded_http_request(
                self._client(),
                "POST",
                self._session_url or self.config.url,
                json=message,
                headers=self.config.headers,
            )
            response.raise_for_status()
            self._consume_post_response(response, raw_body)
            return True
        except Exception as exc:
            self._last_error = f"MCP SSE notification failed: {exc}"
            return False

    def _rpc(self, method: str, params: Any = None) -> Any:
        if not self._alive:
            return _rpc_error_result(f"MCP SSE server '{self.config.id}' is not connected")
        request_id = self._next_request_id()
        event = threading.Event()
        with self._lock:
            self._pending[request_id] = event
        body = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            body["params"] = params
        try:
            response, raw_body = _bounded_http_request(
                self._client(),
                "POST",
                self._session_url or self.config.url,
                json=body,
                headers=self.config.headers,
            )
            response.raise_for_status()
            self._consume_post_response(response, raw_body)
        except Exception as exc:
            with self._lock:
                self._pending.pop(request_id, None)
            return _rpc_error_result(
                f"MCP SSE request failed: {exc}",
                data={"method": method, "serverId": self.config.id},
            )
        completed = event.wait(timeout=30.0)
        with self._lock:
            self._pending.pop(request_id, None)
            result = self._results.pop(request_id, None)
        if not completed:
            return _rpc_error_result(
                "MCP SSE call timeout after 30.0s",
                data={"method": method, "serverId": self.config.id},
            )
        if result is None:
            return _rpc_error_result(
                "MCP SSE server disconnected before replying",
                data={"method": method, "serverId": self.config.id},
            )
        return result

    def _discover_tools(self) -> None:
        if self._discovering_tools:
            return
        self._discovering_tools = True
        discovered: List[McpToolDef] = []
        cursor = ""
        seen_cursors = set()
        try:
            for _page in range(_MCP_MAX_LIST_PAGES):
                result = self._rpc("tools/list", {"cursor": cursor} if cursor else {})
                if not isinstance(result, dict) or _is_rpc_error(result):
                    return
                for item in result.get("tools", []):
                    if not isinstance(item, dict):
                        continue
                    discovered.append(McpToolDef(
                        name=str(item.get("name") or ""),
                        description=str(item.get("description") or ""),
                        input_schema=normalize_tool_parameters(
                            item.get("inputSchema", {
                                "type": "object", "properties": {},
                            })
                        ),
                        server_id=self.config.id,
                        annotations=dict(item.get("annotations") or {}),
                        trusted_server=bool(self.config.trusted),
                    ))
                next_cursor = str(result.get("nextCursor") or "").strip()
                if not next_cursor:
                    self.tools = discovered
                    self._tools_dirty = False
                    return
                if next_cursor in seen_cursors:
                    return
                seen_cursors.add(next_cursor)
                cursor = next_cursor
        finally:
            self._discovering_tools = False

    def refresh_tools_if_needed(self) -> None:
        if self._alive and self._tools_dirty and not self._discovering_tools:
            self._discover_tools()

    def call_tool(self, name: str, arguments: Dict[str, Any]) -> str:
        result = self._rpc("tools/call", {"name": name, "arguments": arguments})
        if self._tools_dirty:
            self._discover_tools()
        if _is_rpc_error(result):
            return json.dumps({
                "error": f"MCP call failed: {_rpc_error_message(result)}",
                "rpc_error": result.get("_rpc_error"),
            }, ensure_ascii=False)
        return _extract_tool_call_result(result)

    def read_resource(self, uri: str) -> Dict[str, Any]:
        result = self._rpc("resources/read", {"uri": uri})
        if self._tools_dirty:
            self._discover_tools()
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
        self._clients: Dict[str, Any] = {}  # stdio | Streamable HTTP | SSE | internal

    def add_server(self, config: McpServerConfig) -> bool:
        if not config.enabled:
            return False
        if config.id in self._clients:
            self.remove_server(config.id)
        transport = str(config.transport or "stdio").strip().lower()
        config.transport = transport
        if transport == "internal":
            return False  # use register_provider() for internal
        if transport in {"streamable_http", "http"}:
            if not config.url:
                print(f"[MCP] Missing Streamable HTTP URL for {config.id}")
                return False
            client = McpStreamableHttpClient(config)
        elif transport == "sse":
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
        if (not ok and isinstance(client, McpStreamableHttpClient)
                and client.legacy_fallback_recommended):
            # Official backwards compatibility: only probe legacy HTTP+SSE
            # after the Streamable HTTP initialize POST is rejected by 404/405.
            client = McpSseClient(config)
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
        if transport in {"streamable_http", "http"}:
            new_client = McpStreamableHttpClient(config)
        elif transport == "sse":
            new_client = McpSseClient(config)
        else:
            new_client = McpStdioClient(config)
        ok = new_client.start()
        if (not ok and isinstance(new_client, McpStreamableHttpClient)
                and new_client.legacy_fallback_recommended):
            new_client = McpSseClient(config)
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
                refresh = getattr(c, "refresh_tools_if_needed", None)
                if callable(refresh):
                    refresh()
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
    workspace_trusted = False

    # From settings
    if settings_get:
        raw_settings_servers = settings_get("ai_editor_mcp_servers", [])

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
            workspace_trusted = _as_bool(
                mcp_settings.get("workspace_trusted"), False)
            raw_collision = str(mcp_settings.get("collision_behavior", "first")).strip().lower()
            if raw_collision in {"first", "last", "error"}:
                collision_behavior = raw_collision

        settings_raw_groups = [raw_settings_servers]
        if has_mcp_settings and normalized_autostart:
            settings_raw_groups.extend([
                mcp_settings.get("servers", []),
                mcp_settings.get("mcpServers", {}),
            ])
        settings_secret_ids = [
            sid
            for raw_group in settings_raw_groups
            for sid, _sconf in _iter_server_configs(raw_group)
        ]
        _append_server_configs(
            configs,
            seen_ids,
            raw_settings_servers,
            legacy_id_candidates=settings_secret_ids,
        )
        if has_mcp_settings and normalized_autostart:
            _append_server_configs(
                configs,
                seen_ids,
                mcp_settings.get("servers", []),
                collision_behavior,
                settings_secret_ids,
            )
            _append_server_configs(
                configs,
                seen_ids,
                mcp_settings.get("mcpServers", {}),
                collision_behavior,
                settings_secret_ids,
            )

    if settings_get and has_mcp_settings and (not discovery_enabled or not normalized_autostart):
        return []

    if not discovery_enabled or not normalized_autostart:
        return configs

    # Workspace and plugin manifests are executable configuration.  They remain
    # inert until the workspace itself has been explicitly trusted.
    for candidate in (["mcp.json", ".mcp/mcp.json", ".vscode/mcp.json"]
                      if workspace_trusted else []):
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
    if workspace_trusted and os.path.isdir(plugins_dir):
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
    legacy_id_candidates: Optional[Iterable[str]] = (),
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
        config = _parse_server_config(sid, sconf, legacy_id_candidates)
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


def _protected_server_mapping(
    sid: str,
    sconf: Dict[str, Any],
    field_name: str,
    legacy_id_candidates: Optional[Iterable[str]] = None,
) -> Dict[str, str]:
    raw = _as_string_dict(sconf.get(field_name, {}))
    secret_fields = {
        str(item) for item in _as_string_list(sconf.get("_secret_fields", []))}
    if field_name not in secret_fields:
        return raw
    try:
        from ai_editor.secret_store import (
            SECRET_PRESENT,
            get_json_with_legacy_migration,
            get_secret_store,
            legacy_ref_is_unambiguous,
        )
        store = get_secret_store()
        new_ref = f"mcp/{_secret_ref_part(sid)}/{_secret_ref_part(field_name)}"
        candidates = (
            [sid] if legacy_id_candidates is None
            else list(legacy_id_candidates)
        )
        if candidates:
            protected = get_json_with_legacy_migration(
                store,
                new_ref,
                (
                    f"mcp/{_legacy_secret_ref_part(sid)}/"
                    f"{_legacy_secret_ref_part(field_name)}"
                ),
                allow_legacy=legacy_ref_is_unambiguous(sid, candidates),
                default={},
            )
        else:
            protected = store.get_json(new_ref, {})
    except Exception as exc:
        print(f"[MCP] Failed to load protected {field_name} for {sid}: {exc}")
        return {
            key: value for key, value in raw.items()
            if value != "__SAO_SECRET_PRESENT__"
        }
    result = _as_string_dict(protected)
    for key, value in raw.items():
        if value != SECRET_PRESENT:
            result[key] = value
    return result


def _secret_ref_part(value: Any) -> str:
    from ai_editor.secret_store import secret_ref_part
    return secret_ref_part(value)


def _legacy_secret_ref_part(value: Any) -> str:
    from ai_editor.secret_store import legacy_secret_ref_part
    return legacy_secret_ref_part(value)


def _parse_server_config(
    sid: str,
    sconf: Dict[str, Any],
    legacy_id_candidates: Optional[Iterable[str]] = None,
) -> McpServerConfig:
    return McpServerConfig(
        id=sid,
        name=sconf.get("name", sid),
        transport=str(sconf.get("transport", "stdio")),
        command=sconf.get("command", ""),
        args=_as_string_list(sconf.get("args", [])),
        env=_protected_server_mapping(
            sid, sconf, "env", legacy_id_candidates),
        url=sconf.get("url", ""),
        headers=_protected_server_mapping(
            sid, sconf, "headers", legacy_id_candidates),
        enabled=_as_bool(sconf.get("enabled"), True),
        trusted=_as_bool(sconf.get("trusted"), False),
        inherit_env=_as_string_list(sconf.get("inherit_env", [])),
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
