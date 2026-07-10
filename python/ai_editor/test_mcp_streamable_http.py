from __future__ import annotations

import json
import queue
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from types import SimpleNamespace
from typing import Any, Dict, List

from ai_editor.mcp_client import (
    McpManager,
    McpServerConfig,
    McpStreamableHttpClient,
)


class _QuietThreadingHTTPServer(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, _request: Any, _client_address: Any) -> None:
        return


class _MockMcpHandler(BaseHTTPRequestHandler):
    server_version = "SaoMcpTest/1.0"
    protocol_version = "HTTP/1.1"

    @property
    def state(self) -> SimpleNamespace:
        return self.server.state  # type: ignore[attr-defined]

    def log_message(self, _format: str, *_args: Any) -> None:
        return

    def _read_json(self) -> Dict[str, Any]:
        length = int(self.headers.get("Content-Length", "0") or 0)
        payload = self.rfile.read(length) if length else b"{}"
        decoded = json.loads(payload.decode("utf-8"))
        return decoded if isinstance(decoded, dict) else {}

    def _record(self, message: Dict[str, Any]) -> None:
        self.state.requests.append({
            "message": message,
            "headers": {key.lower(): value for key, value in self.headers.items()},
        })

    def _send_json(self, payload: Dict[str, Any], status: int = 200,
                   headers: Dict[str, str] | None = None) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)

    def _send_empty(self, status: int) -> None:
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        message = self._read_json()
        self._record(message)
        method = str(message.get("method") or "")

        if self.state.legacy_mode:
            if self.path == "/mcp":
                self._send_empty(405)
                return
            if method == "notifications/initialized":
                self._send_empty(202)
                return
            if method == "initialize":
                result = {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {"tools": {"listChanged": True}},
                    "serverInfo": {"name": "legacy", "version": "1.0"},
                }
            elif method == "tools/list":
                result = {"tools": [{
                    "name": "legacy-tool",
                    "description": "legacy",
                    "inputSchema": {"type": "object", "properties": {}},
                }]}
            else:
                result = {"content": [{"type": "text", "text": "legacy-ok"}]}
            self.state.legacy_messages.put({
                "jsonrpc": "2.0", "id": message.get("id"), "result": result,
            })
            self._send_empty(202)
            return

        if method == "initialize":
            self.state.session_counter += 1
            self.state.current_session = f"session-{self.state.session_counter}"
            self._send_json({
                "jsonrpc": "2.0",
                "id": message.get("id"),
                "result": {
                    "protocolVersion": "2025-06-18",
                    "capabilities": {"tools": {"listChanged": True}},
                    "serverInfo": {"name": "mock", "version": "1.0"},
                },
            }, headers={"Mcp-Session-Id": self.state.current_session})
            return

        session_id = self.headers.get("Mcp-Session-Id", "")
        protocol_version = self.headers.get("MCP-Protocol-Version", "")
        if session_id != self.state.current_session or protocol_version != "2025-06-18":
            self._send_json({"error": "missing session or protocol headers"}, status=400)
            return

        if method == "notifications/initialized":
            self.state.initialized_sessions.append(session_id)
            self._send_empty(202)
            return

        if method == "tools/list":
            self.state.tool_list_calls += 1
            cursor = str((message.get("params") or {}).get("cursor") or "")
            if not cursor:
                result = {
                    "tools": [{
                        "name": "first",
                        "description": "first page",
                        "inputSchema": {"type": "object", "properties": {}},
                        "annotations": {"readOnlyHint": True},
                    }],
                    "nextCursor": "page-2",
                }
            else:
                result = {
                    "tools": [{
                        "name": "second",
                        "description": "second page",
                        "inputSchema": {"type": "object", "properties": {}},
                    }],
                }
            self._send_json({
                "jsonrpc": "2.0", "id": message.get("id"), "result": result,
            })
            return

        if method == "tools/call":
            response_id = message.get("id")
            notification = json.dumps({
                "jsonrpc": "2.0", "method": "notifications/tools/list_changed",
            })
            response = json.dumps({
                "jsonrpc": "2.0",
                "id": response_id,
                "result": {"content": [{"type": "text", "text": "sse-ok"}]},
            })
            body = (
                f"event: message\ndata: {notification}\n\n"
                f"event: message\ndata: {response}\n\n"
            ).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if method == "resources/read":
            if self.state.expire_next_resource:
                self.state.expire_next_resource = False
                self._send_empty(404)
                return
            uri = str((message.get("params") or {}).get("uri") or "")
            self._send_json({
                "jsonrpc": "2.0",
                "id": message.get("id"),
                "result": {"contents": [{
                    "uri": uri, "text": "recovered", "mimeType": "text/plain",
                }]},
            })
            return

        self._send_json({
            "jsonrpc": "2.0",
            "id": message.get("id"),
            "error": {"code": -32601, "message": "unknown method"},
        })

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        if not self.state.legacy_mode or self.path != "/mcp":
            self._send_empty(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        try:
            self.wfile.write(b"event: endpoint\ndata: /legacy/messages\n\n")
            self.wfile.flush()
            while not self.state.legacy_stop:
                try:
                    message = self.state.legacy_messages.get(timeout=0.1)
                    payload = json.dumps(message).encode("utf-8")
                    self.wfile.write(b"event: message\ndata: " + payload + b"\n\n")
                except queue.Empty:
                    self.wfile.write(b": keepalive\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            return

    def do_DELETE(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self.state.delete_sessions.append(self.headers.get("Mcp-Session-Id", ""))
        self.state.delete_protocols.append(
            self.headers.get("MCP-Protocol-Version", ""))
        self._send_empty(204)


class McpStreamableHttpTests(unittest.TestCase):
    def setUp(self) -> None:
        self.state = SimpleNamespace(
            requests=[],
            session_counter=0,
            current_session="",
            initialized_sessions=[],
            delete_sessions=[],
            delete_protocols=[],
            tool_list_calls=0,
            expire_next_resource=False,
            legacy_mode=False,
            legacy_stop=False,
            legacy_messages=queue.Queue(),
        )
        self.server = _QuietThreadingHTTPServer(("127.0.0.1", 0), _MockMcpHandler)
        self.server.state = self.state  # type: ignore[attr-defined]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        host, port = self.server.server_address
        self.url = f"http://{host}:{port}/mcp"
        self.clients: List[McpStreamableHttpClient] = []

    def tearDown(self) -> None:
        for client in self.clients:
            if client.is_alive:
                client.stop()
        self.state.legacy_stop = True
        time.sleep(0.05)
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2.0)

    def _client(self, transport: str = "streamable_http") -> McpStreamableHttpClient:
        client = McpStreamableHttpClient(McpServerConfig(
            id="mock",
            name="Mock MCP",
            transport=transport,
            url=self.url,
            headers={"Authorization": "Bearer test"},
            trusted=True,
        ))
        self.clients.append(client)
        return client

    def test_initialize_headers_session_pagination_and_cleanup(self) -> None:
        client = self._client()
        self.assertTrue(client.start())
        self.assertEqual([tool.name for tool in client.tools], ["first", "second"])
        self.assertEqual(client.tools[0].annotations, {"readOnlyHint": True})
        self.assertTrue(client.tools[0].trusted_server)

        requests = self.state.requests
        initialize = requests[0]
        self.assertEqual(initialize["message"]["method"], "initialize")
        self.assertNotIn("mcp-session-id", initialize["headers"])
        self.assertNotIn("mcp-protocol-version", initialize["headers"])
        self.assertIn("application/json", initialize["headers"]["accept"])
        self.assertIn("text/event-stream", initialize["headers"]["accept"])

        subsequent = requests[1:]
        self.assertTrue(subsequent)
        self.assertTrue(all(
            item["headers"].get("mcp-session-id") == "session-1"
            and item["headers"].get("mcp-protocol-version") == "2025-06-18"
            for item in subsequent
        ))
        request_ids = [
            item["message"]["id"] for item in requests
            if "id" in item["message"]
        ]
        self.assertEqual(len(request_ids), len(set(request_ids)))

        client.stop()
        self.assertEqual(self.state.delete_sessions, ["session-1"])
        self.assertEqual(self.state.delete_protocols, ["2025-06-18"])

    def test_sse_post_response_marks_tool_list_dirty_and_refreshes(self) -> None:
        client = self._client()
        self.assertTrue(client.start())
        initial_list_calls = self.state.tool_list_calls
        self.assertEqual(client.call_tool("first", {}), "sse-ok")
        self.assertEqual(self.state.tool_list_calls, initial_list_calls + 2)
        self.assertEqual([tool.name for tool in client.tools], ["first", "second"])

    def test_session_404_reinitializes_once_and_retries(self) -> None:
        client = self._client()
        self.assertTrue(client.start())
        self.state.expire_next_resource = True

        result = client.read_resource("mock://resource")
        self.assertTrue(result.get("ok"), result)
        self.assertEqual(result.get("content"), "recovered")
        self.assertEqual(self.state.session_counter, 2)
        self.assertEqual(self.state.initialized_sessions, ["session-1", "session-2"])

        resource_requests = [
            item for item in self.state.requests
            if item["message"].get("method") == "resources/read"
        ]
        self.assertEqual(len(resource_requests), 2)
        self.assertEqual(
            [item["headers"].get("mcp-session-id") for item in resource_requests],
            ["session-1", "session-2"],
        )
        client.stop()
        self.assertEqual(self.state.delete_sessions[-1], "session-2")

    def test_manager_accepts_http_transport_alias(self) -> None:
        manager = McpManager()
        config = McpServerConfig(
            id="alias", name="Alias", transport="http", url=self.url)
        self.assertTrue(manager.add_server(config))
        self.assertEqual(manager.list_servers()[0]["transport"], "http")
        manager.remove_server("alias")

    def test_http_transport_falls_back_to_bounded_legacy_sse(self) -> None:
        self.state.legacy_mode = True
        manager = McpManager()
        config = McpServerConfig(
            id="legacy", name="Legacy", transport="http", url=self.url)
        self.assertTrue(manager.add_server(config))
        self.assertEqual(
            [tool.name for tool in manager.all_tools()], ["legacy-tool"])
        self.assertEqual(
            manager.call_tool("mcp_legacy_legacy-tool", {}), "legacy-ok")
        manager.remove_server("legacy")


if __name__ == "__main__":
    unittest.main()
