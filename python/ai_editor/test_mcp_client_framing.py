from __future__ import annotations

import io
import json
import threading
import unittest

from ai_editor.mcp_client import (
    McpServerConfig,
    McpSseClient,
    McpStreamableHttpClient,
    _read_bounded_http_body,
    _read_rpc_message_from_stream,
)


class _FakeResponse:
    def __init__(self, chunks, *, status_code=200, headers=None) -> None:
        self._chunks = list(chunks)
        self.status_code = status_code
        self.headers = dict(headers or {})
        self.iterated = False
        self.closed = False

    @property
    def text(self):
        raise AssertionError("bounded response paths must not access response.text")

    def iter_bytes(self):
        self.iterated = True
        yield from self._chunks

    def raise_for_status(self) -> None:
        if self.status_code >= 400:
            raise RuntimeError(f"HTTP {self.status_code}")

    def close(self) -> None:
        self.closed = True


class _ResponseContext:
    def __init__(self, response: _FakeResponse) -> None:
        self.response = response

    def __enter__(self) -> _FakeResponse:
        return self.response

    def __exit__(self, *_args) -> None:
        self.response.close()


class _StreamingClient:
    def __init__(self, *responses: _FakeResponse) -> None:
        self.responses = list(responses)
        self.calls = []

    def stream(self, method, url, **kwargs):
        self.calls.append((method, url, kwargs))
        if not self.responses:
            raise AssertionError("unexpected streaming request")
        return _ResponseContext(self.responses.pop(0))

    def post(self, *_args, **_kwargs):
        raise AssertionError("production path must stream before buffering")

    def close(self) -> None:
        return None


class McpClientFramingLimitTests(unittest.TestCase):
    MAX_MESSAGE_BYTES = 4 * 1024 * 1024

    def test_oversized_content_length_is_rejected_before_payload_read(self) -> None:
        class _ProbeStream:
            def __init__(self) -> None:
                self._lines = iter([
                    b"Content-Length: 4194305\r\n",
                    b"\r\n",
                ])
                self.payload_read = False

            def readline(self, size: int = -1) -> bytes:
                return next(self._lines, b"")

            def read(self, size: int = -1) -> bytes:
                self.payload_read = True
                raise AssertionError("oversized payload must not be read")

        stream = _ProbeStream()
        with self.assertRaisesRegex(RuntimeError, "exceeds.*4194304"):
            _read_rpc_message_from_stream(stream)
        self.assertFalse(stream.payload_read)

    def test_oversized_newline_delimited_message_is_rejected(self) -> None:
        stream = io.BytesIO(
            b"x" * (self.MAX_MESSAGE_BYTES + 1) + b"\n")
        with self.assertRaisesRegex(RuntimeError, "line.*exceeds.*4194304"):
            _read_rpc_message_from_stream(stream)

    def test_maximum_newline_delimited_message_is_accepted(self) -> None:
        stream = io.BytesIO(b"x" * self.MAX_MESSAGE_BYTES + b"\n")
        result = _read_rpc_message_from_stream(stream)
        self.assertIsNotNone(result)
        self.assertEqual(len(result.rstrip(b"\r\n")), self.MAX_MESSAGE_BYTES)

    def test_oversized_content_length_header_line_is_rejected(self) -> None:
        stream = io.BytesIO(
            b"Content-Length: 2\r\n"
            + b"X-Oversized: " + b"x" * 8193 + b"\r\n"
            + b"\r\n{}"
        )
        with self.assertRaisesRegex(RuntimeError, "header line.*exceeds.*8192"):
            _read_rpc_message_from_stream(stream)

    def test_excessive_content_length_header_count_is_rejected(self) -> None:
        headers = b"".join(
            f"X-Header-{index}: value\r\n".encode("ascii")
            for index in range(100)
        )
        stream = io.BytesIO(
            b"Content-Length: 2\r\n" + headers + b"\r\n{}")
        with self.assertRaisesRegex(RuntimeError, "header count.*exceeds.*100"):
            _read_rpc_message_from_stream(stream)

    def test_excessive_total_header_bytes_are_rejected(self) -> None:
        headers = b"".join(
            f"X-{index}: ".encode("ascii") + b"x" * 800 + b"\r\n"
            for index in range(82)
        )
        stream = io.BytesIO(
            b"Content-Length: 2\r\n" + headers + b"\r\n{}")
        with self.assertRaisesRegex(RuntimeError, "headers length.*exceeds.*65536"):
            _read_rpc_message_from_stream(stream)


class McpHttpResponseLimitTests(unittest.TestCase):
    MAX_MESSAGE_BYTES = 4 * 1024 * 1024

    def test_small_text_response_double_remains_compatible(self) -> None:
        class _TextResponse:
            headers = {"content-type": "application/json"}
            content = '{"ok":true}'

        self.assertEqual(
            b'{"ok":true}', _read_bounded_http_body(_TextResponse()))

    def test_declared_oversized_http_body_is_rejected_before_iteration(self) -> None:
        response = _FakeResponse(
            [b"must-not-read"],
            headers={"content-length": str(self.MAX_MESSAGE_BYTES + 1)},
        )

        with self.assertRaisesRegex(
                RuntimeError, "Content-Length.*exceeds.*4194304"):
            _read_bounded_http_body(response)

        self.assertFalse(response.iterated)

    def test_chunked_oversized_http_body_is_rejected_while_streaming(self) -> None:
        response = _FakeResponse([
            b"x" * self.MAX_MESSAGE_BYTES,
            b"y",
        ])

        with self.assertRaisesRegex(
                RuntimeError, "body length.*exceeds.*4194304"):
            _read_bounded_http_body(response)

        self.assertTrue(response.iterated)

    def test_streamable_http_uses_bounded_streaming_request(self) -> None:
        body = json.dumps({
            "jsonrpc": "2.0",
            "id": 1,
            "result": {"ok": True},
        }).encode("utf-8")
        response = _FakeResponse(
            [body],
            headers={
                "content-type": "application/json",
                "content-length": str(len(body)),
            },
        )
        transport = _StreamingClient(response)
        client = McpStreamableHttpClient(McpServerConfig(
            id="bounded-http",
            name="Bounded HTTP",
            transport="streamable_http",
            url="https://example.invalid/mcp",
        ))
        client._http = transport

        result = client._send_message({
            "jsonrpc": "2.0", "id": 1, "method": "ping",
        })

        self.assertEqual(result, {"ok": True})
        self.assertEqual(transport.calls[0][0], "POST")
        self.assertTrue(response.iterated)

    def test_legacy_sse_post_uses_bounded_streaming_request(self) -> None:
        body = json.dumps({
            "jsonrpc": "2.0",
            "id": 1,
            "result": {"ok": True},
        }).encode("utf-8")
        response = _FakeResponse(
            [body],
            headers={"content-type": "application/json"},
        )
        transport = _StreamingClient(response)
        client = McpSseClient(McpServerConfig(
            id="bounded-sse-post",
            name="Bounded SSE POST",
            transport="sse",
            url="https://example.invalid/mcp",
        ))
        client._http = transport
        client._alive = True
        client._session_url = "https://example.invalid/messages"

        result = client._rpc("ping", {})

        self.assertEqual(result, {"ok": True})
        self.assertEqual(transport.calls[0][0], "POST")
        self.assertTrue(response.iterated)


class McpLegacySseStreamLimitTests(unittest.TestCase):
    MAX_MESSAGE_BYTES = 4 * 1024 * 1024

    def _run_oversized_stream(self, chunks):
        response = _FakeResponse(
            chunks,
            headers={"content-type": "text/event-stream"},
        )
        client = McpSseClient(McpServerConfig(
            id="bounded-sse-get",
            name="Bounded SSE GET",
            transport="sse",
            url="https://example.invalid/mcp",
        ))
        client._http = _StreamingClient(response)
        client._alive = True
        pending = threading.Event()
        client._pending[7] = pending

        client._read_sse_loop()

        self.assertTrue(pending.is_set())
        self.assertFalse(client.is_alive)
        self.assertIn("exceeds 4194304 bytes", client._last_error)
        self.assertIn("_rpc_error", client._results[7])
        return client

    def test_oversized_sse_line_disconnects_and_fails_pending(self) -> None:
        client = self._run_oversized_stream([
            b"data: " + b"x" * self.MAX_MESSAGE_BYTES,
        ])
        self.assertIn("SSE line length", client._last_error)

    def test_multiline_oversized_sse_event_disconnects_and_fails_pending(self) -> None:
        payload = b"x" * (self.MAX_MESSAGE_BYTES // 4)
        client = self._run_oversized_stream([
            b"data: " + payload + b"\n",
            b"data: " + payload + b"\n",
            b"data: " + payload + b"\n",
            b"data: " + payload + b"\n\n",
        ])
        self.assertIn("SSE event length", client._last_error)


if __name__ == "__main__":
    unittest.main()
