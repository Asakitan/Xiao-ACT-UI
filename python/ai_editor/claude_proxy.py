"""Local Anthropic-compatible proxy server for Claude Code SDK integration.

Starts a lightweight HTTP server on localhost with nonce auth.
The Claude Code SDK subprocess connects to this proxy, which forwards
requests through our LLMEngine to the actual Anthropic API.

Usage::

    proxy = ClaudeProxy(llm_engine)
    proxy.start()           # starts server on random port
    print(proxy.base_url)   # http://localhost:PORT
    print(proxy.nonce)       # auth nonce for SDK
    ...
    proxy.stop()

The proxy accepts Anthropic Messages API format:
  POST /v1/messages  (streaming SSE)
  GET  /health
"""

from __future__ import annotations

import json
import logging
import secrets
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
from typing import Any, Dict, Optional


logger = logging.getLogger(__name__)


class _ProxyHandler(BaseHTTPRequestHandler):
    server: "_ProxyServer"

    @staticmethod
    def _stringify_content(value: Any) -> str:
        if isinstance(value, str):
            return value
        if isinstance(value, list):
            parts = []
            for item in value:
                if isinstance(item, dict):
                    if item.get("type") == "text":
                        parts.append(str(item.get("text") or ""))
                    else:
                        parts.append(json.dumps(item, ensure_ascii=False))
                else:
                    parts.append(str(item))
            return "\n".join(part for part in parts if part)
        if isinstance(value, dict):
            return json.dumps(value, ensure_ascii=False)
        return str(value or "")

    @classmethod
    def _anthropic_messages_to_engine(
            cls, system: Any, messages: Any) -> list[Dict[str, Any]]:
        api_messages: list[Dict[str, Any]] = []
        system_text = cls._stringify_content(system).strip()
        if system_text:
            api_messages.append({"role": "system", "content": system_text})
        for message in messages if isinstance(messages, list) else []:
            role = str(message.get("role") or "user") if isinstance(message, dict) else "user"
            content = message.get("content", "") if isinstance(message, dict) else message
            if isinstance(content, list):
                text_parts: list[str] = []
                tool_calls: list[Dict[str, Any]] = []
                tool_results: list[Dict[str, Any]] = []
                for block in content:
                    if not isinstance(block, dict):
                        text_parts.append(str(block))
                        continue
                    block_type = str(block.get("type") or "")
                    if block_type == "text":
                        text_parts.append(str(block.get("text") or ""))
                    elif block_type == "tool_use":
                        tool_calls.append({
                            "id": str(block.get("id") or ""),
                            "type": "function",
                            "function": {
                                "name": str(block.get("name") or ""),
                                "arguments": json.dumps(block.get("input", {}), ensure_ascii=False),
                            },
                        })
                    elif block_type == "tool_result":
                        tool_results.append({
                            "role": "tool",
                            "tool_call_id": str(block.get("tool_use_id") or ""),
                            "content": cls._stringify_content(block.get("content", "")),
                        })
                text = "\n".join(part for part in text_parts if part)
                if tool_calls:
                    api_messages.append({
                        "role": role,
                        "content": text or None,
                        "tool_calls": tool_calls,
                    })
                elif text:
                    api_messages.append({"role": role, "content": text})
                api_messages.extend(tool_results)
                continue
            api_messages.append({"role": role, "content": cls._stringify_content(content)})
        return api_messages

    @staticmethod
    def _tool_input(arguments: str) -> Any:
        text = str(arguments or "").strip()
        if not text:
            return {}
        try:
            return json.loads(text)
        except json.JSONDecodeError:
            return {"_raw_arguments": text}

    @classmethod
    def _anthropic_content_from_response(cls, resp: Any) -> list[Dict[str, Any]]:
        blocks: list[Dict[str, Any]] = []
        if getattr(resp, "content", ""):
            blocks.append({"type": "text", "text": resp.content})
        for tc in getattr(resp, "tool_calls", []) or []:
            blocks.append({
                "type": "tool_use",
                "id": tc.id or f"toolu_{secrets.token_hex(8)}",
                "name": tc.name,
                "input": cls._tool_input(tc.arguments),
            })
        return blocks

    def do_GET(self) -> None:
        if self.path == "/health":
            self._json_response(200, {"status": "ok"})
            return
        self._json_response(404, {"error": "not found"})

    def do_POST(self) -> None:
        if not self._check_auth():
            self._json_response(401, {"error": "unauthorized"})
            return

        if self.path in ("/v1/messages", "/messages"):
            self._handle_messages()
        else:
            self._json_response(404, {"error": "not found"})

    def _check_auth(self) -> bool:
        auth = self.headers.get("x-api-key", "")
        if not auth:
            auth = self.headers.get("Authorization", "").replace("Bearer ", "")
        return auth == self.server.nonce

    def _handle_messages(self) -> None:
        try:
            length = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(length)) if length else {}
        except Exception:
            self._json_response(400, {"error": "invalid JSON"})
            return

        engine = self.server.engine
        if not engine:
            self._json_response(503, {"error": "engine not ready"})
            return

        model = body.get("model", engine.config.effective_model)
        messages = body.get("messages", [])
        system = body.get("system", "")
        max_tokens = body.get("max_tokens", engine.config.max_tokens)
        stream = body.get("stream", False)
        tools = body.get("tools")

        api_messages = self._anthropic_messages_to_engine(system, messages)

        if stream:
            self._handle_stream(api_messages, tools, model, max_tokens)
        else:
            self._handle_sync(api_messages, tools, model, max_tokens)

    def _handle_stream(self, messages, tools, model, max_tokens) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()

        engine = self.server.engine

        msg_id = f"msg_{secrets.token_hex(12)}"
        self._sse("message_start", {
            "type": "message_start",
            "message": {"id": msg_id, "type": "message", "role": "assistant",
                        "model": model, "content": [], "stop_reason": None,
                        "usage": {"input_tokens": 0, "output_tokens": 0}},
        })

        text_block_open = True
        self._sse("content_block_start", {
            "type": "content_block_start", "index": 0,
            "content_block": {"type": "text", "text": ""},
        })

        def on_delta(d):
            if d.content:
                self._sse("content_block_delta", {
                    "type": "content_block_delta", "index": 0,
                    "delta": {"type": "text_delta", "text": d.content},
                })

        try:
            engine.reset_cancel()
            resp = engine.chat_completion_stream(
                messages=messages,
                tools=self._tools_schema(tools),
                on_delta=on_delta,
            )

            if text_block_open:
                self._sse("content_block_stop", {
                    "type": "content_block_stop", "index": 0})
                text_block_open = False

            for index, tc in enumerate(getattr(resp, "tool_calls", []) or [], start=1):
                self._sse("content_block_start", {
                    "type": "content_block_start",
                    "index": index,
                    "content_block": {
                        "type": "tool_use",
                        "id": tc.id or f"toolu_{secrets.token_hex(8)}",
                        "name": tc.name,
                        "input": self._tool_input(tc.arguments),
                    },
                })
                self._sse("content_block_stop", {
                    "type": "content_block_stop", "index": index})

            self._sse("message_delta", {
                "type": "message_delta",
                "delta": {"stop_reason": "tool_use" if resp.tool_calls else "end_turn"},
                "usage": resp.usage,
            })
            self._sse("message_stop", {"type": "message_stop"})
        except Exception as exc:
            self._sse("error", {"type": "error",
                                "error": {"message": str(exc)}})

    def _handle_sync(self, messages, tools, model, max_tokens) -> None:
        engine = self.server.engine
        try:
            engine.reset_cancel()
            resp = engine.chat_completion_stream(
                messages=messages,
                tools=self._tools_schema(tools),
                on_delta=lambda d: None,
            )
            self._json_response(200, {
                "id": f"msg_{secrets.token_hex(12)}",
                "type": "message", "role": "assistant",
                "model": model,
                "content": self._anthropic_content_from_response(resp),
                "stop_reason": "tool_use" if resp.tool_calls else (resp.finish_reason or "end_turn"),
                "usage": resp.usage,
            })
        except Exception as exc:
            self._json_response(500, {"error": {"message": str(exc)}})

    def _json_response(self, code: int, data: Dict) -> None:
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _sse(self, event: str, data: Dict) -> None:
        payload = json.dumps(data, ensure_ascii=False)
        self.wfile.write(f"event: {event}\ndata: {payload}\n\n".encode("utf-8"))
        self.wfile.flush()

    @staticmethod
    def _tools_schema(tools: Any) -> Optional[list[Dict[str, Any]]]:
        if not tools:
            return None
        converted: list[Dict[str, Any]] = []
        for tool in tools if isinstance(tools, list) else []:
            if not isinstance(tool, dict):
                continue
            if isinstance(tool.get("function"), dict):
                converted.append({"type": "function", "function": tool["function"]})
                continue
            converted.append({
                "type": "function",
                "function": {
                    "name": str(tool.get("name") or ""),
                    "description": str(tool.get("description") or ""),
                    "parameters": tool.get("input_schema") or tool.get("parameters") or {"type": "object", "properties": {}},
                },
            })
        return converted or None

    def log_message(self, format, *args) -> None:
        logger.debug("ClaudeProxy %s", format % args)


class _ProxyServer(HTTPServer):
    engine: Any = None
    nonce: str = ""


class ClaudeProxy:
    """Local Anthropic Messages API proxy."""

    def __init__(self, engine: Any = None) -> None:
        self._engine = engine
        self._server: Optional[_ProxyServer] = None
        self._thread: Optional[threading.Thread] = None
        self._nonce = secrets.token_hex(16)

    @property
    def base_url(self) -> str:
        if self._server:
            port = self._server.server_address[1]
            return f"http://127.0.0.1:{port}"
        return ""

    @property
    def nonce(self) -> str:
        return self._nonce

    @property
    def port(self) -> int:
        if self._server:
            return self._server.server_address[1]
        return 0

    @property
    def model(self) -> str:
        if self._engine:
            return self._engine.config.effective_model
        return ""

    def set_engine(self, engine: Any) -> None:
        self._engine = engine
        if self._server:
            self._server.engine = engine

    def start(self) -> int:
        if self._server:
            return self.port
        self._server = _ProxyServer(("127.0.0.1", 0), _ProxyHandler)
        self._server.engine = self._engine
        self._server.nonce = self._nonce
        self._thread = threading.Thread(
            target=self._server.serve_forever, daemon=True)
        self._thread.start()
        port = self._server.server_address[1]
        logger.info("ClaudeProxy listening on http://127.0.0.1:%s", port)
        return port

    def stop(self) -> None:
        if self._server:
            server = self._server
            thread = self._thread
            server.shutdown()
            server.server_close()
            if thread and thread.is_alive():
                thread.join(timeout=1.0)
            self._server = None
            self._thread = None

    @property
    def is_running(self) -> bool:
        return self._server is not None

    def get_env(self) -> Dict[str, str]:
        """Return env vars for the Claude Code SDK subprocess."""
        return {
            "ANTHROPIC_BASE_URL": self.base_url + "/v1",
            "ANTHROPIC_API_KEY": self._nonce,
            "ANTHROPIC_MODEL": self.model,
        }
