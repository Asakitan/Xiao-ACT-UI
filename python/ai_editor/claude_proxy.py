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
import os
import secrets
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
from typing import Any, Dict, Optional


class _ProxyHandler(BaseHTTPRequestHandler):
    server: "_ProxyServer"

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

        api_messages = []
        if system:
            api_messages.append({"role": "system", "content": system})
        for m in messages:
            api_messages.append({
                "role": m.get("role", "user"),
                "content": m.get("content", ""),
            })

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

        idx = [0]

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
            tools_schema = None
            if tools:
                tools_schema = [{"type": "function", "function": t}
                                for t in tools]
            engine.reset_cancel()
            resp = engine.chat_completion_stream(
                messages=messages, tools=tools_schema, on_delta=on_delta)

            self._sse("content_block_stop", {
                "type": "content_block_stop", "index": 0})

            self._sse("message_delta", {
                "type": "message_delta",
                "delta": {"stop_reason": "end_turn"},
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
                messages=messages, tools=None, on_delta=lambda d: None)
            self._json_response(200, {
                "id": f"msg_{secrets.token_hex(12)}",
                "type": "message", "role": "assistant",
                "model": model,
                "content": [{"type": "text", "text": resp.content or ""}],
                "stop_reason": resp.finish_reason or "end_turn",
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

    def log_message(self, format, *args) -> None:
        pass


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
        print(f"[ClaudeProxy] listening on http://127.0.0.1:{port}")
        return port

    def stop(self) -> None:
        if self._server:
            self._server.shutdown()
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
            "ANTHROPIC_MODEL": (self._engine.config.effective_model
                                if self._engine else "claude-sonnet-4-20250514"),
        }
