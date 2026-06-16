"""Conversation state management for the AI Editor.

Tracks message history, tool-call bookkeeping, and supports multi-turn
conversations with streaming.  Thread-safe for the Tk after-based UI loop.
"""

from __future__ import annotations

import json
import threading
import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional

from ai_editor.llm_engine import LLMEngine, LLMResponse, ProviderConfig, StreamDelta, ToolCall
from ai_editor.tool_registry import ToolRegistry


# ---------------------------------------------------------------------------
# Message types
# ---------------------------------------------------------------------------

@dataclass
class ChatMessage:
    id: str = field(default_factory=lambda: uuid.uuid4().hex[:12])
    role: str = "user"  # user | assistant | system | tool
    content: str = ""
    thinking: str = ""
    tool_calls: List[ToolCall] = field(default_factory=list)
    tool_call_id: Optional[str] = None
    tool_name: Optional[str] = None
    timestamp: float = field(default_factory=time.time)
    model: str = ""
    usage: Dict[str, int] = field(default_factory=dict)
    is_streaming: bool = False
    is_error: bool = False

    def to_api_dict(self) -> Dict[str, Any]:
        """Convert to OpenAI messages-array format."""
        d: Dict[str, Any] = {"role": self.role, "content": self.content}
        if self.role == "tool":
            d["tool_call_id"] = self.tool_call_id or ""
        if self.tool_calls:
            d["tool_calls"] = [
                {
                    "id": tc.id,
                    "type": "function",
                    "function": {"name": tc.name, "arguments": tc.arguments},
                }
                for tc in self.tool_calls
            ]
            if not self.content:
                d["content"] = None
        return d


# ---------------------------------------------------------------------------
# Conversation
# ---------------------------------------------------------------------------

class Conversation:
    """A single conversation thread."""

    def __init__(self, system_prompt: str = "") -> None:
        self.id: str = uuid.uuid4().hex[:8]
        self.title: str = "New Chat"
        self.messages: List[ChatMessage] = []
        self.system_prompt: str = system_prompt
        self.created_at: float = time.time()
        self._lock = threading.Lock()

    def add_message(self, msg: ChatMessage) -> None:
        with self._lock:
            self.messages.append(msg)
            if len(self.messages) == 1 and msg.role == "user" and not self.title_set:
                self.title = msg.content[:40].replace("\n", " ")

    @property
    def title_set(self) -> bool:
        return self.title != "New Chat"

    def to_api_messages(self) -> List[Dict[str, Any]]:
        msgs: List[Dict[str, Any]] = []
        if self.system_prompt:
            msgs.append({"role": "system", "content": self.system_prompt})
        with self._lock:
            for m in self.messages:
                msgs.append(m.to_api_dict())
        return msgs

    def clear(self) -> None:
        with self._lock:
            self.messages.clear()
            self.title = "New Chat"

    @property
    def total_tokens(self) -> int:
        total = 0
        for m in self.messages:
            total += m.usage.get("total_tokens", 0)
        return total


# ---------------------------------------------------------------------------
# Chat Controller (orchestrates LLM + tool loop)
# ---------------------------------------------------------------------------

class ChatController:
    """Drives the send → stream → tool-call → resume loop."""

    MAX_TOOL_ROUNDS = 10

    def __init__(
        self,
        engine: LLMEngine,
        registry: ToolRegistry,
        conversation: Optional[Conversation] = None,
    ) -> None:
        self.engine = engine
        self.registry = registry
        self.conversation = conversation or Conversation()
        self._running = False
        self._thread: Optional[threading.Thread] = None

        # UI callbacks
        self.on_message_added: Optional[Callable[[ChatMessage], None]] = None
        self.on_stream_delta: Optional[Callable[[ChatMessage, str], None]] = None
        self.on_thinking_delta: Optional[Callable[[ChatMessage, str], None]] = None
        self.on_stream_end: Optional[Callable[[ChatMessage], None]] = None
        self.on_tool_start: Optional[Callable[[str, str, str], None]] = None  # call_id, name, args
        self.on_tool_end: Optional[Callable[[str, str], None]] = None  # call_id, result
        self.on_error: Optional[Callable[[str], None]] = None
        self.on_idle: Optional[Callable[[], None]] = None

    @property
    def is_running(self) -> bool:
        return self._running

    def send(self, text: str) -> None:
        if self._running:
            return
        user_msg = ChatMessage(role="user", content=text)
        self.conversation.add_message(user_msg)
        if self.on_message_added:
            self.on_message_added(user_msg)
        self._running = True
        self._thread = threading.Thread(target=self._run_loop, daemon=True)
        self._thread.start()

    def cancel(self) -> None:
        self.engine.cancel()
        self._running = False

    def _run_loop(self) -> None:
        try:
            for _round in range(self.MAX_TOOL_ROUNDS):
                if not self._running:
                    break

                assistant_msg = ChatMessage(
                    role="assistant",
                    is_streaming=True,
                    model=self.engine.config.effective_model,
                )
                self.conversation.add_message(assistant_msg)
                if self.on_message_added:
                    self.on_message_added(assistant_msg)

                messages = self.conversation.to_api_messages()
                tools = self.registry.to_openai_tools() or None

                def _on_delta(delta: StreamDelta, _msg=assistant_msg) -> None:
                    if delta.content and self.on_stream_delta:
                        self.on_stream_delta(_msg, delta.content)
                    if delta.thinking and self.on_thinking_delta:
                        self.on_thinking_delta(_msg, delta.thinking)

                self.engine.reset_cancel()
                resp = self.engine.chat_completion_stream(
                    messages=messages,
                    tools=tools,
                    on_delta=_on_delta,
                )

                assistant_msg.content = resp.content
                assistant_msg.thinking = resp.thinking
                assistant_msg.tool_calls = resp.tool_calls
                assistant_msg.usage = resp.usage
                assistant_msg.model = resp.model
                assistant_msg.is_streaming = False

                if resp.error:
                    assistant_msg.is_error = True
                    assistant_msg.content = resp.error
                    if self.on_stream_end:
                        self.on_stream_end(assistant_msg)
                    break

                if self.on_stream_end:
                    self.on_stream_end(assistant_msg)

                if not resp.tool_calls:
                    break

                for tc in resp.tool_calls:
                    if not self._running:
                        break
                    if self.on_tool_start:
                        self.on_tool_start(tc.id, tc.name, tc.arguments)

                    result = self.registry.execute(tc.name, tc.arguments)
                    tc.result = result

                    tool_msg = ChatMessage(
                        role="tool",
                        content=result,
                        tool_call_id=tc.id,
                        tool_name=tc.name,
                    )
                    self.conversation.add_message(tool_msg)
                    if self.on_message_added:
                        self.on_message_added(tool_msg)
                    if self.on_tool_end:
                        self.on_tool_end(tc.id, result)

        except Exception as exc:
            if self.on_error:
                self.on_error(str(exc))
        finally:
            self._running = False
            self._auto_save()
            if self.on_idle:
                self.on_idle()

    def _auto_save(self) -> None:
        """Persist current conversation to disk after each turn."""
        try:
            if not self.conversation.messages:
                return
            from ai_editor.history import save_conversation
            msgs = json.loads(self.export_messages())
            save_conversation(
                conv_id=self.conversation.id,
                title=self.conversation.title,
                messages=msgs,
                system_prompt=self.conversation.system_prompt,
                model=self.engine.config.effective_model,
            )
        except Exception:
            pass

    def new_conversation(self, system_prompt: str = "") -> Conversation:
        self._auto_save()
        self.cancel()
        sp = system_prompt or self.engine.config.system_prompt
        self.conversation = Conversation(system_prompt=sp)
        return self.conversation

    def export_messages(self) -> str:
        data = []
        for m in self.conversation.messages:
            d = {
                "role": m.role,
                "content": m.content,
                "timestamp": m.timestamp,
            }
            if m.tool_calls:
                d["tool_calls"] = [
                    {"id": tc.id, "name": tc.name, "arguments": tc.arguments, "result": tc.result}
                    for tc in m.tool_calls
                ]
            if m.tool_call_id:
                d["tool_call_id"] = m.tool_call_id
                d["tool_name"] = m.tool_name
            data.append(d)
        return json.dumps(data, ensure_ascii=False, indent=2)
