# -*- coding: utf-8 -*-
"""Focused MCP 2025-06-18 Streamable HTTP server regression tests."""

from __future__ import annotations

import json
import threading
import time
import unittest
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from typing import Any, Dict, Optional, Tuple

from ai_editor.mcp_client import McpServerConfig, McpStreamableHttpClient
from ai_editor.mcp_server import McpHttpServer, _PROTOCOL_VERSION


class _SerialProbeRuntime:
    def __init__(self) -> None:
        self._state_lock = threading.Lock()
        self.active = 0
        self.max_active = 0
        self.calls = 0
        self.block_started: Optional[threading.Event] = None
        self.block_release: Optional[threading.Event] = None

    def handle_tool(self, name: str, arguments: Dict[str, Any]) -> Any:
        with self._state_lock:
            self.active += 1
            self.calls += 1
            self.max_active = max(self.max_active, self.active)
        try:
            if self.block_started is not None and self.block_release is not None:
                self.block_started.set()
                self.block_release.wait(3.0)
            else:
                time.sleep(0.08)
            return {"name": name, "arguments": arguments}
        finally:
            with self._state_lock:
                self.active -= 1


class McpHttpServerHardeningTests(unittest.TestCase):
    def setUp(self) -> None:
        self.runtime = _SerialProbeRuntime()
        self.server = McpHttpServer(
            port=0, runtime=self.runtime, max_request_bytes=2048)
        self.thread = threading.Thread(target=self.server.run, daemon=True)
        self.thread.start()
        self.assertTrue(self.server.wait_until_ready(3.0))
        self.assertTrue(self.thread.is_alive())
        self.url = f"http://127.0.0.1:{self.server.port}/"

    def tearDown(self) -> None:
        self.server.stop()
        self.thread.join(timeout=3.0)

    def _request(
            self, method: str = "POST", message: Any = None,
            headers: Optional[Dict[str, str]] = None,
            path: str = "/", raw_body: Optional[bytes] = None,
            content_type: Optional[str] = "application/json",
            ) -> Tuple[int, Dict[str, str], bytes]:
        body = raw_body
        if body is None and message is not None:
            body = json.dumps(message).encode("utf-8")
        request_headers = dict(headers or {})
        if content_type is not None and body is not None:
            request_headers.setdefault("Content-Type", content_type)
        request = urllib.request.Request(
            f"http://127.0.0.1:{self.server.port}{path}",
            data=body, headers=request_headers, method=method)
        try:
            with urllib.request.urlopen(request, timeout=5.0) as response:
                return (
                    int(response.status), dict(response.headers.items()),
                    response.read())
        except urllib.error.HTTPError as exc:
            return int(exc.code), dict(exc.headers.items()), exc.read()

    @staticmethod
    def _rpc(
            method: str, request_id: Optional[int] = 1,
            params: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        payload: Dict[str, Any] = {
            "jsonrpc": "2.0", "method": method, "params": params or {}}
        if request_id is not None:
            payload["id"] = request_id
        return payload

    def test_initialize_negotiates_supported_version_and_returns_json(self) -> None:
        status, headers, body = self._request(
            message=self._rpc("initialize", params={
                "protocolVersion": "2025-03-26",
                "capabilities": {},
                "clientInfo": {"name": "focused-test", "version": "1"},
            }),
            headers={"Accept": "application/json, text/event-stream"})

        self.assertEqual(status, 200)
        self.assertTrue(headers["Content-Type"].startswith("application/json"))
        self.assertEqual(headers["Cache-Control"], "no-store")
        self.assertEqual(headers["X-Content-Type-Options"], "nosniff")
        payload = json.loads(body)
        self.assertEqual(payload["result"]["protocolVersion"], "2025-03-26")
        self.assertIn("tools", payload["result"]["capabilities"])

        status, _, body = self._request(
            message=self._rpc("initialize", request_id=2, params={
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "legacy", "version": "1"},
            }))
        self.assertEqual(status, 200)
        self.assertEqual(
            json.loads(body)["result"]["protocolVersion"], _PROTOCOL_VERSION)

    def test_legacy_urllib_flow_and_initialized_notification(self) -> None:
        # The existing editor selftest sends neither Accept nor the HTTP
        # protocol-version header; keep that integration working.
        status, _, _ = self._request(
            message=self._rpc("initialize", params={
                "clientInfo": {"name": "legacy-urllib", "version": "1"}}))
        self.assertEqual(status, 200)

        status, _, body = self._request(message=self._rpc("tools/list"))
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(body)["result"]["tools"])

        status, headers, body = self._request(
            message=self._rpc("notifications/initialized", request_id=None))
        self.assertEqual(status, 202)
        self.assertEqual(headers["Content-Length"], "0")
        self.assertEqual(body, b"")
        self.assertTrue(self.server.initialized)

    def test_notifications_and_client_responses_return_empty_202(self) -> None:
        status, _, body = self._request(
            message=self._rpc("notifications/cancelled", request_id=None,
                              params={"requestId": 99, "reason": "test"}))
        self.assertEqual((status, body), (202, b""))

        status, _, body = self._request(message={
            "jsonrpc": "2.0", "id": 99, "result": {"ok": True}})
        self.assertEqual((status, body), (202, b""))

    def test_initialized_notification_requires_completed_initialize_request(self) -> None:
        status, _, body = self._request(
            message=self._rpc("notifications/initialized", request_id=None))
        self.assertEqual(status, 400)
        self.assertIn("initialize", json.loads(body)["error"]["message"])
        self.assertFalse(self.server.initialized)

        status, _, body = self._request(
            message=self._rpc("initialize", request_id=None, params={
                "protocolVersion": _PROTOCOL_VERSION,
                "capabilities": {},
                "clientInfo": {"name": "invalid", "version": "1"},
            }))
        self.assertEqual(status, 400)
        self.assertIn("request", json.loads(body)["error"]["message"])

        status, _, _ = self._request(
            message=self._rpc("initialize", request_id=1, params={
                "protocolVersion": _PROTOCOL_VERSION,
                "capabilities": {},
                "clientInfo": {"name": "valid", "version": "1"},
            }))
        self.assertEqual(status, 200)
        status, _, body = self._request(
            message=self._rpc("notifications/initialized", request_id=None),
            headers={"MCP-Protocol-Version": _PROTOCOL_VERSION})
        self.assertEqual((status, body), (202, b""))
        self.assertTrue(self.server.initialized)

    def test_rejects_unsupported_protocol_and_media_types(self) -> None:
        status, _, body = self._request(
            message=self._rpc("ping"),
            headers={"MCP-Protocol-Version": "2099-01-01"})
        self.assertEqual(status, 400)
        self.assertIn("supported", json.loads(body)["error"]["data"])

        status, _, _ = self._request(
            message=self._rpc("ping"),
            headers={"Accept": "text/event-stream"})
        self.assertEqual(status, 406)

        status, _, _ = self._request(
            message=self._rpc("ping"), content_type="text/plain")
        self.assertEqual(status, 415)

    def test_origin_path_and_get_sse_guards(self) -> None:
        status, _, _ = self._request(
            message=self._rpc("ping"),
            headers={"Origin": "https://attacker.example"})
        self.assertEqual(status, 403)

        status, _, body = self._request(
            message=self._rpc("ping"),
            headers={"Origin": "http://localhost:3000"})
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["result"], {})

        status, headers, body = self._request(
            method="GET", message=None, content_type=None)
        self.assertEqual(status, 405)
        self.assertEqual(headers["Allow"], "POST")
        self.assertEqual(body, b"")

        status, _, _ = self._request(
            method="GET", message=None, content_type=None,
            headers={"Origin": "https://attacker.example"})
        self.assertEqual(status, 403)

        status, _, _ = self._request(
            method="GET", message=None, path="/not-mcp", content_type=None)
        self.assertEqual(status, 404)

    def test_rejects_invalid_or_oversized_request_body(self) -> None:
        status, _, body = self._request(raw_body=b"{not-json")
        self.assertEqual(status, 400)
        self.assertEqual(json.loads(body)["error"]["code"], -32700)

        oversized = self._rpc(
            "ping", params={"padding": "x" * self.server._max_request_bytes})
        status, _, body = self._request(message=oversized)
        self.assertEqual(status, 413)
        self.assertIn("limit", json.loads(body)["error"]["message"])

    def test_threaded_tool_calls_are_serialized_for_live_runtime(self) -> None:
        def call(index: int) -> Tuple[int, Dict[str, str], bytes]:
            return self._request(message=self._rpc(
                "tools/call", request_id=index, params={
                    "name": "read_file",
                    "arguments": {"path": f"file-{index}.txt"},
                }))

        with ThreadPoolExecutor(max_workers=2) as pool:
            responses = list(pool.map(call, (1, 2)))

        self.assertEqual([item[0] for item in responses], [200, 200])
        self.assertEqual(self.runtime.calls, 2)
        self.assertEqual(self.runtime.max_active, 1)

    def test_project_streamable_http_client_completes_full_lifecycle(self) -> None:
        client = McpStreamableHttpClient(McpServerConfig(
            id="focused-server", name="Focused server",
            transport="streamable_http", url=self.url))
        try:
            self.assertTrue(client.start(), client._last_error)
            self.assertTrue(self.server.initialized)
            self.assertIn("read_file", {tool.name for tool in client.tools})
            result = client.call_tool(
                "read_file", {"path": "through-real-client.txt"})
            self.assertIn("through-real-client.txt", result)
        finally:
            client.stop()

    def test_stop_is_idempotent_and_closes_server_thread(self) -> None:
        bound_port = self.server.port
        self.server.stop()
        self.thread.join(timeout=3.0)
        self.assertFalse(self.thread.is_alive())
        self.assertIsNone(self.server._server)
        self.server.stop()

        restarted = McpHttpServer(
            port=bound_port, runtime=_SerialProbeRuntime())
        restarted_thread = threading.Thread(target=restarted.run, daemon=True)
        restarted_thread.start()
        self.assertTrue(restarted.wait_until_ready(3.0))
        restarted.stop()
        restarted_thread.join(timeout=3.0)
        self.assertFalse(restarted_thread.is_alive())

    def test_bind_failure_reports_startup_error_through_ready_handshake(self) -> None:
        conflicting = McpHttpServer(
            port=self.server.port, runtime=_SerialProbeRuntime())
        conflicting_thread = threading.Thread(
            target=conflicting.run, daemon=True)
        conflicting_thread.start()
        self.assertFalse(conflicting.wait_until_ready(3.0))
        conflicting_thread.join(timeout=3.0)
        self.assertFalse(conflicting_thread.is_alive())
        self.assertTrue(conflicting.startup_error)

    def test_stop_waits_for_in_flight_runtime_mutation(self) -> None:
        self.runtime.block_started = threading.Event()
        self.runtime.block_release = threading.Event()
        response_holder = []
        call_thread = threading.Thread(target=lambda: response_holder.append(
            self._request(message=self._rpc(
                "tools/call", request_id=1, params={
                    "name": "read_file", "arguments": {"path": "slow.txt"},
                }))))
        call_thread.start()
        self.assertTrue(self.runtime.block_started.wait(2.0))

        stop_thread = threading.Thread(target=self.server.stop)
        stop_thread.start()
        time.sleep(0.05)
        self.assertTrue(stop_thread.is_alive())
        self.runtime.block_release.set()
        stop_thread.join(timeout=3.0)
        call_thread.join(timeout=3.0)
        self.thread.join(timeout=3.0)

        self.assertFalse(stop_thread.is_alive())
        self.assertFalse(call_thread.is_alive())
        self.assertFalse(self.thread.is_alive())
        self.assertEqual(response_holder[0][0], 200)
        self.assertEqual(self.runtime.active, 0)


if __name__ == "__main__":
    unittest.main()
