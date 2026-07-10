"""Focused regression coverage for ChatController run isolation."""

from __future__ import annotations

import json
import threading
import time
import unittest
from unittest.mock import patch

from ai_editor.chat_state import ChatController, ChatMessage, Conversation
from ai_editor.llm_engine import LLMResponse, ProviderConfig, StreamDelta, ToolCall
from ai_editor.tool_registry import ToolRegistry


class _BaseEngine:
    def __init__(self) -> None:
        self.config = ProviderConfig(provider="openai", model="chat-hardening-test")

    def reset_cancel(self) -> None:
        return None

    def cancel(self) -> None:
        return None

    def count_message_tokens(self, messages) -> int:
        return len(messages) * 20

    def estimate_tokens(self, text: str) -> int:
        return max(1, len(text) // 4) if text else 0


class _SingleBlockingEngine(_BaseEngine):
    def __init__(self) -> None:
        super().__init__()
        self.started = threading.Event()
        self.release = threading.Event()

    def chat_completion_stream(self, messages, tools=None, on_delta=None):
        self.started.set()
        if not self.release.wait(2.0):
            raise TimeoutError("test engine was not released")
        return LLMResponse(content="done")


class _RetiredWorkerEngine(_BaseEngine):
    def __init__(self) -> None:
        super().__init__()
        self._lock = threading.Lock()
        self._calls = 0
        self.old_started = threading.Event()
        self.old_release = threading.Event()

    def chat_completion_stream(self, messages, tools=None, on_delta=None):
        with self._lock:
            call_index = self._calls
            self._calls += 1
        if call_index == 0:
            self.old_started.set()
            if not self.old_release.wait(2.0):
                raise TimeoutError("retired worker was not released")
            if on_delta:
                on_delta(StreamDelta(content="stale delta"))
            return LLMResponse(tool_calls=[ToolCall(
                id="old_write",
                name="writeFile",
                arguments=json.dumps({"path": "old.txt"}),
            )])
        return LLMResponse(content="new response")


class _ScriptedEngine(_BaseEngine):
    def __init__(self, responses) -> None:
        super().__init__()
        self.responses = list(responses)
        self.requests = []

    def chat_completion_stream(self, messages, tools=None, on_delta=None):
        self.requests.append(messages)
        return self.responses.pop(0)


def _wait_until(predicate, timeout: float = 2.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.01)
    return bool(predicate())


class ChatControllerHardeningTests(unittest.TestCase):
    def test_concurrent_send_has_one_atomic_winner(self) -> None:
        engine = _SingleBlockingEngine()
        controller = ChatController(engine, ToolRegistry())
        controller._auto_save = lambda conversation=None: None
        barrier = threading.Barrier(3)
        results = []

        def _send(text: str) -> None:
            barrier.wait()
            results.append(controller.send(text))

        threads = [
            threading.Thread(target=_send, args=("first",)),
            threading.Thread(target=_send, args=("second",)),
        ]
        for thread in threads:
            thread.start()
        barrier.wait()
        for thread in threads:
            thread.join(1.0)

        self.assertEqual(2, len(results))
        accepted = [result for result in results if result["accepted"]]
        busy = [result for result in results if result["busy"]]
        self.assertEqual(1, len(accepted))
        self.assertEqual(1, len(busy))
        self.assertEqual(accepted[0]["run_id"], busy[0]["run_id"])
        self.assertEqual(
            1,
            len([message for message in controller.conversation.messages
                 if message.role == "user"]),
        )

        self.assertTrue(engine.started.wait(1.0))
        engine.release.set()
        self.assertTrue(_wait_until(lambda: not controller.is_running))

    def test_retired_worker_cannot_touch_new_conversation(self) -> None:
        engine = _RetiredWorkerEngine()
        registry = ToolRegistry()
        write_count = []
        registry.register(
            "writeFile", "write", {"type": "object"},
            lambda **kwargs: write_count.append(kwargs) or {"ok": True},
        )
        controller = ChatController(engine, registry)
        controller.CANCEL_JOIN_TIMEOUT = 0.01
        saved_ids = []
        controller._auto_save = (
            lambda conversation=None: saved_ids.append(
                (conversation or controller.conversation).id))
        idle_ids = []
        stale_deltas = []
        controller.on_idle = lambda: idle_ids.append(controller.conversation.id)
        controller.on_stream_delta = lambda _message, text: stale_deltas.append(text)

        old_result = controller.send("old request")
        self.assertTrue(old_result["accepted"])
        self.assertTrue(engine.old_started.wait(1.0))
        old_thread = controller._thread

        new_conversation = controller.new_conversation("new system")
        saves_after_switch = list(saved_ids)
        new_result = controller.send("new request")
        self.assertTrue(new_result["accepted"])
        self.assertNotEqual(old_result["run_id"], new_result["run_id"])
        self.assertTrue(_wait_until(lambda: not controller.is_running))
        self.assertEqual(1, len(idle_ids))

        engine.old_release.set()
        self.assertIsNotNone(old_thread)
        old_thread.join(1.0)
        self.assertFalse(old_thread.is_alive())
        self.assertEqual([], write_count)
        self.assertEqual([], stale_deltas)
        self.assertEqual(1, len(idle_ids))
        self.assertEqual(saves_after_switch + [new_conversation.id], saved_ids)
        self.assertEqual(
            ["new request", "new response"],
            [message.content for message in new_conversation.messages],
        )
        self.assertTrue(all(
            message.tool_call_id != "old_write"
            for message in new_conversation.messages
        ))

    def test_cancel_closes_incomplete_tool_protocol_without_idle(self) -> None:
        engine = _ScriptedEngine([LLMResponse(tool_calls=[ToolCall(
            id="confirm_write", name="writeFile", arguments='{"path":"a"}',
        )])])
        registry = ToolRegistry()
        executed = []
        registry.register(
            "writeFile", "write", {"type": "object"},
            lambda **kwargs: executed.append(kwargs) or {"ok": True},
            requires_confirm=True,
        )
        controller = ChatController(engine, registry)
        controller._auto_save = lambda conversation=None: None
        confirm_started = threading.Event()
        idle_count = []

        def _confirm(*_args):
            confirm_started.set()
            while controller.is_running:
                time.sleep(0.005)
            return False

        controller.on_tool_confirm = _confirm
        controller.on_idle = lambda: idle_count.append(True)
        controller.send("write")
        self.assertTrue(confirm_started.wait(1.0))
        worker = controller._thread
        controller.cancel()
        self.assertIsNotNone(worker)
        worker.join(1.0)

        self.assertFalse(worker.is_alive())
        self.assertEqual([], executed)
        self.assertEqual([], idle_count)
        self.assertTrue(controller._tool_sequence_is_valid(
            controller.conversation.messages))
        tool_results = [
            message for message in controller.conversation.messages
            if message.role == "tool"
        ]
        self.assertEqual(1, len(tool_results))
        self.assertEqual("confirm_write", tool_results[0].tool_call_id)
        self.assertIn("cancelled", tool_results[0].content)

    def test_read_cache_lru_is_unique_and_new_chat_invalidates(self) -> None:
        controller = ChatController(_ScriptedEngine([]), ToolRegistry())
        controller._auto_save = lambda conversation=None: None
        key_a = controller._tool_cache_key("readFile", '{"b":2,"a":1}')
        key_b = controller._tool_cache_key("readFile", '{"a":1,"b":2}')
        self.assertEqual(key_a, key_b)
        controller._tool_cache_put(key_a, "old")
        controller._tool_cache_put(key_b, "new")
        self.assertEqual("new", controller._tool_cache_get(key_a))
        self.assertEqual([key_a], controller._tool_result_cache_keys)

        controller.new_conversation("fresh")
        self.assertEqual({}, controller._tool_result_cache)
        self.assertEqual([], controller._tool_result_cache_keys)

    def test_write_tool_invalidates_read_cache(self) -> None:
        engine = _ScriptedEngine([
            LLMResponse(tool_calls=[ToolCall(
                id="write_1", name="writeFile", arguments='{"path":"a"}',
            )]),
            LLMResponse(content="written"),
        ])
        registry = ToolRegistry()
        registry.register(
            "writeFile", "write", {"type": "object"},
            lambda **kwargs: {"ok": True},
        )
        conversation = Conversation()
        conversation.add_message(ChatMessage(role="user", content="write"))
        controller = ChatController(engine, registry, conversation)
        controller._auto_save = lambda conversation=None: None
        controller._tool_cache_put("readFile:cached", "stale")

        controller._running = True
        controller._run_loop()
        self.assertEqual({}, controller._tool_result_cache)
        self.assertEqual([], controller._tool_result_cache_keys)
        self.assertEqual(["user"], [
            message["role"] for message in engine.requests[0]
        ])
        self.assertEqual(["user", "assistant", "tool"], [
            message["role"] for message in engine.requests[1]
        ])

    def test_compaction_keeps_tool_protocol_atomic(self) -> None:
        engine = _ScriptedEngine([])
        conversation = Conversation(system_prompt="system")
        for turn in range(6):
            call_id = f"call_{turn}"
            conversation.add_message(ChatMessage(
                role="user", content=f"request {turn}"))
            conversation.add_message(ChatMessage(
                role="assistant",
                tool_calls=[ToolCall(
                    id=call_id, name="readFile",
                    arguments=json.dumps({"turn": turn}),
                )],
            ))
            conversation.add_message(ChatMessage(
                role="tool", content=f"result {turn}",
                tool_call_id=call_id, tool_name="readFile",
            ))
            conversation.add_message(ChatMessage(
                role="assistant", content=f"answer {turn}"))

        controller = ChatController(engine, ToolRegistry(), conversation)
        original_count = len(conversation.messages)
        with patch("ai_editor.llm_engine.compaction_threshold", return_value=100):
            controller._auto_compress()

        self.assertLess(len(conversation.messages), original_count)
        self.assertEqual("system", conversation.messages[0].role)
        self.assertIn("[Compacted:", conversation.messages[0].content)
        self.assertTrue(controller._tool_sequence_is_valid(
            conversation.messages))
        retained_calls = {
            call.id
            for message in conversation.messages
            for call in message.tool_calls
        }
        retained_results = {
            message.tool_call_id
            for message in conversation.messages
            if message.role == "tool"
        }
        self.assertEqual(retained_calls, retained_results)


if __name__ == "__main__":
    unittest.main()
