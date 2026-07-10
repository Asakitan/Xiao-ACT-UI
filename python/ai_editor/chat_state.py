"""Conversation state management for the AI Editor.

Tracks message history, tool-call bookkeeping, and supports multi-turn
conversations with streaming.  Thread-safe for the Tk after-based UI loop.
"""

from __future__ import annotations

import copy
import json
import re
import threading
import time
import uuid
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable, Dict, List, Optional

from ai_editor.llm_engine import LLMEngine, LLMResponse, ProviderConfig, StreamDelta, ToolCall
from ai_editor.tool_registry import ToolRegistry


# ---------------------------------------------------------------------------
# Tool invocation state machine
# ---------------------------------------------------------------------------

class ToolInvocationState(Enum):
    """Lifecycle states for a single tool call."""
    PENDING = "pending"
    CONFIRMED = "confirmed"
    EXECUTING = "executing"
    COMPLETED = "completed"
    CANCELLED = "cancelled"


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
    usage: Dict[str, Any] = field(default_factory=dict)
    is_streaming: bool = False
    is_error: bool = False

    def to_api_dict(self) -> Dict[str, Any]:
        """Convert to OpenAI messages-array format."""
        d: Dict[str, Any] = {"role": self.role, "content": self.content}
        if self.role == "tool":
            d["tool_call_id"] = self.tool_call_id or ""
            if self.tool_name:
                d["name"] = self.tool_name
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


@dataclass
class _RunContext:
    """Immutable run identity plus per-run cancellation and input state.

    The controller deliberately keeps this private: callers receive ``run_id``
    from :meth:`ChatController.send`, while the worker uses object identity to
    reject callbacks and side effects from a retired thread.
    """

    run_id: str
    conversation: "Conversation"
    cancel_event: threading.Event
    engine: Any
    agent_mode: bool = False
    multimodal_content: Optional[Any] = None
    thread: Optional[threading.Thread] = None
    current_assistant: Optional[ChatMessage] = None
    finished_event: threading.Event = field(default_factory=threading.Event)


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
        self._msg_version = 0
        self._api_cache: Optional[List[Dict[str, Any]]] = None
        self._api_cache_ver = -1

    def add_message(self, msg: ChatMessage) -> None:
        with self._lock:
            self.messages.append(msg)
            self._msg_version += 1
            if len(self.messages) == 1 and msg.role == "user" and not self.title_set:
                self.title = msg.content[:40].replace("\n", " ")

    @property
    def title_set(self) -> bool:
        return self.title != "New Chat"

    def to_api_messages(self) -> List[Dict[str, Any]]:
        with self._lock:
            if self._api_cache is not None and self._api_cache_ver == self._msg_version:
                return [dict(message) for message in self._api_cache]
            msgs: List[Dict[str, Any]] = []
            if self.system_prompt:
                msgs.append({"role": "system", "content": self.system_prompt})
            for m in self.messages:
                msgs.append(m.to_api_dict())
            self._api_cache = msgs
            self._api_cache_ver = self._msg_version
            return [dict(message) for message in msgs]

    def clear(self) -> None:
        with self._lock:
            self.messages.clear()
            self.title = "New Chat"
            self._msg_version += 1
            self._api_cache = None

    @property
    def total_tokens(self) -> int:
        with self._lock:
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
    CANCEL_JOIN_TIMEOUT = 5.0

    def __init__(
        self,
        engine: LLMEngine,
        registry: ToolRegistry,
        conversation: Optional[Conversation] = None,
    ) -> None:
        self.engine = engine
        self.registry = registry
        self.conversation = conversation or Conversation()
        # Auto-compress runs at the start of each _run_loop based on token count
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._active_run: Optional[_RunContext] = None
        self._state_lock = threading.RLock()
        self._lifecycle_lock = threading.RLock()
        self.extra_tools: Optional[List[Dict[str, Any]]] = None  # MCP tools injected by app
        self.mcp_dispatch: Optional[Callable[[str, str], str]] = None  # MCP tool call dispatcher
        self.mcp_tool_requires_confirm: Optional[Callable[[str], bool]] = None
        self.mcp_tool_allowed: Optional[Callable[[str], bool]] = None
        self._agent_mode: bool = False
        self._disabled_tools: set = set()
        self._tool_result_cache: Dict[str, str] = {}
        self._tool_result_cache_keys: List[str] = []
        self._tool_result_cache_lock = threading.Lock()
        self._multimodal_override: Optional[Any] = None
        self._last_save_key: Optional[tuple[str, int]] = None
        self._last_save_error = ""

        # Session-level auto-approve (tool name → always allow for this session)
        self._session_auto_approve: Dict[str, bool] = {}

        # Per-call tool invocation state tracking
        self._tool_states: Dict[str, ToolInvocationState] = {}
        self._tool_states_lock = threading.Lock()

        # UI callbacks
        self.on_message_added: Optional[Callable[[ChatMessage], None]] = None
        self.on_stream_delta: Optional[Callable[[ChatMessage, str], None]] = None
        self.on_thinking_delta: Optional[Callable[[ChatMessage, str], None]] = None
        self.on_token_warning: Optional[Callable[[int, int, float], None]] = None  # used, limit, ratio
        self.on_stream_end: Optional[Callable[[ChatMessage], None]] = None
        self.on_tool_start: Optional[Callable[[str, str, str, str], None]] = None  # call_id, name, args, state
        self.on_tool_end: Optional[Callable[[str, str, str, str], None]] = None  # call_id, name, result, state
        self.on_tool_confirm: Optional[Callable[[str, str, str], Any]] = None  # call_id, name, args → bool|"always_approve"
        self.on_tool_progress: Optional[Callable[[str, str, float], None]] = None  # call_id, name, progress 0-1
        self.on_error: Optional[Callable[[str], None]] = None
        self.on_save_error: Optional[Callable[[str], None]] = None
        self.on_idle: Optional[Callable[[], None]] = None
        self.resolve_variable: Optional[Callable[[str], str]] = None  # @mention resolver

        # Confirmation state (thread-safe)
        self._confirm_lock = threading.Lock()
        self._confirm_event: Optional[threading.Event] = None
        self._confirm_result: bool = True

    @property
    def is_running(self) -> bool:
        with self._state_lock:
            return self._running

    @property
    def active_run_id(self) -> Optional[str]:
        """Return the current public run identifier, if a run is active."""
        with self._state_lock:
            run = self._active_run
            if run is not None and not run.cancel_event.is_set():
                return run.run_id
            return None

    def _is_active_run_locked(self, run: _RunContext) -> bool:
        return self._active_run is run and not run.cancel_event.is_set()

    def _is_active_run(self, run: _RunContext) -> bool:
        with self._state_lock:
            return self._is_active_run_locked(run)

    def _add_message_for_run(self, run: _RunContext, message: ChatMessage,
                             *, current_assistant: bool = False) -> bool:
        """Append and notify as one cancellation-linearized operation."""
        with self._state_lock:
            if not self._is_active_run_locked(run):
                return False
            run.conversation.add_message(message)
            if current_assistant:
                run.current_assistant = message
            if self.on_message_added:
                self.on_message_added(message)
            return True

    def _fork_engine_for_run(self) -> Any:
        """Snapshot mutable provider settings and isolate request cancel state."""
        config = copy.deepcopy(self.engine.config)
        fork = getattr(self.engine, "fork_for_run", None)
        request_engine = fork(config) if callable(fork) else self.engine
        reset = getattr(request_engine, "reset_cancel", None)
        if callable(reset):
            reset()
        return request_engine

    def _start_run(self, user_msg: ChatMessage, *, agent_mode: bool,
                   multimodal_content: Optional[Any] = None) -> Dict[str, Any]:
        """Atomically reserve the controller and start one worker."""
        with self._lifecycle_lock:
            with self._state_lock:
                active = self._active_run
                if active is not None and not active.cancel_event.is_set():
                    return {
                        "ok": False,
                        "accepted": False,
                        "busy": True,
                        "run_id": active.run_id,
                    }

                run = _RunContext(
                    run_id=uuid.uuid4().hex[:12],
                    conversation=self.conversation,
                    cancel_event=threading.Event(),
                    engine=self._fork_engine_for_run(),
                    agent_mode=bool(agent_mode),
                    multimodal_content=multimodal_content,
                )
                thread = threading.Thread(
                    target=self._run_loop,
                    args=(run,),
                    name=f"ai-chat-{run.run_id}",
                    daemon=True,
                )
                run.thread = thread
                self._active_run = run
                self._running = True
                self._thread = thread
                # Compatibility mirrors for code that still inspects these
                # private fields.  The worker never relies on them.
                self._agent_mode = run.agent_mode
                self._multimodal_override = multimodal_content
                run.conversation.add_message(user_msg)
                if self.on_message_added:
                    self.on_message_added(user_msg)
                thread.start()
                return {
                    "ok": True,
                    "accepted": True,
                    "busy": False,
                    "run_id": run.run_id,
                }

    def _set_tool_state(self, call_id: str, state: ToolInvocationState) -> None:
        """Update the state machine for a tool invocation."""
        with self._tool_states_lock:
            self._tool_states[call_id] = state

    def get_tool_state(self, call_id: str) -> Optional[ToolInvocationState]:
        """Query current state of a tool invocation."""
        with self._tool_states_lock:
            return self._tool_states.get(call_id)

    def _notify_tool_end(self, call_id: str, name: str, result: str,
                         state: ToolInvocationState) -> None:
        if not self.on_tool_end:
            return
        state_value = state.value
        try:
            self.on_tool_end(call_id, name, result, state_value)
        except TypeError:
            self.on_tool_end(call_id, result, state_value)  # type: ignore[misc]

    def send(self, text: str, agent_mode: bool = False) -> Dict[str, Any]:
        resolved = self._resolve_at_mentions(text)
        user_msg = ChatMessage(role="user", content=resolved)
        return self._start_run(user_msg, agent_mode=agent_mode)

    def send_multimodal(self, display_text: str, multimodal_content: Any,
                        agent_mode: bool = False) -> Dict[str, Any]:
        user_msg = ChatMessage(role="user", content=display_text)
        return self._start_run(
            user_msg,
            agent_mode=agent_mode,
            multimodal_content=multimodal_content,
        )

    # @-mention variable resolution
    _AT_RE = re.compile(r'@(\w+)')

    def _resolve_at_mentions(self, text: str) -> str:
        """Replace @file, @selection, @editor, @state etc with real content."""
        def _replace(m):
            var = m.group(1).lower()
            if var == "selection" and self.resolve_variable:
                r = self.resolve_variable("selection")
                return f"[Selected text: {r}]" if r else "@selection"
            if var == "editor" and self.resolve_variable:
                r = self.resolve_variable("editor")
                return f"[Editor content ({len(r)} chars)]:\n```\n{r[:2000]}\n```" if r else "@editor"
            if var == "file" and self.resolve_variable:
                r = self.resolve_variable("file")
                return f"[Current file: {r}]" if r else "@file"
            if var == "state" and self.resolve_variable:
                r = self.resolve_variable("state")
                return f"[Game state: {r}]" if r else "@state"
            if var == "language" and self.resolve_variable:
                r = self.resolve_variable("language")
                return f"[Language: {r}]" if r else "@language"
            return m.group(0)
        return self._AT_RE.sub(_replace, text)

    def cancel(self) -> None:
        with self._lifecycle_lock:
            with self._state_lock:
                run = self._active_run
                thread = run.thread if run is not None else None
                request_engine = run.engine if run is not None else self.engine
                if run is not None:
                    self._close_cancelled_run_locked(run)
                    run.cancel_event.set()
                    self._active_run = None
                self._running = False
                if self._thread is thread:
                    self._thread = None
                self._multimodal_override = None

            request_engine.cancel()
            with self._confirm_lock:
                evt = self._confirm_event
                if evt is not None:
                    evt.set()
            if (thread is not None and thread.is_alive()
                    and thread is not threading.current_thread()):
                thread.join(timeout=self.CANCEL_JOIN_TIMEOUT)

    def _close_cancelled_run_locked(self, run: _RunContext) -> None:
        """Leave a cancelled conversation in a valid, non-streaming state.

        This method runs synchronously as part of ``cancel`` while the run is
        still active.  The retired worker is not allowed to perform this
        cleanup later because a new conversation may already exist by then.
        """
        assistant = run.current_assistant
        if assistant is None:
            return
        assistant.is_streaming = False
        if not assistant.content and not assistant.thinking and not assistant.tool_calls:
            with run.conversation._lock:
                try:
                    run.conversation.messages.remove(assistant)
                except ValueError:
                    pass
                else:
                    run.conversation._msg_version += 1
                    run.conversation._api_cache = None
            return

        with run.conversation._lock:
            completed_ids = {
                message.tool_call_id
                for message in run.conversation.messages
                if message.role == "tool" and message.tool_call_id
            }
        for tool_call in assistant.tool_calls:
            if tool_call.id in completed_ids:
                continue
            result = json.dumps({"error": "Tool execution cancelled"})
            tool_call.result = result
            self._set_tool_state(tool_call.id, ToolInvocationState.CANCELLED)
            tool_msg = ChatMessage(
                role="tool",
                content=result,
                tool_call_id=tool_call.id,
                tool_name=tool_call.name,
            )
            run.conversation.add_message(tool_msg)
            try:
                if self.on_message_added:
                    self.on_message_added(tool_msg)
                if self.on_tool_end:
                    self._notify_tool_end(
                        tool_call.id, tool_call.name, result,
                        ToolInvocationState.CANCELLED,
                    )
            except Exception:
                pass

    MAX_AGENT_ROUNDS = 25
    KEEP_RECENT_MIN = 6

    @staticmethod
    def _tool_sequence_is_valid(messages: List[ChatMessage]) -> bool:
        """Return whether tool calls and results form complete API sequences."""
        pending: Optional[set[str]] = None
        for message in messages:
            if pending is not None:
                if (message.role != "tool" or not message.tool_call_id
                        or message.tool_call_id not in pending):
                    return False
                pending.remove(message.tool_call_id)
                if not pending:
                    pending = None
                continue
            if message.role == "tool":
                return False
            if message.role == "assistant" and message.tool_calls:
                call_ids = [call.id for call in message.tool_calls if call.id]
                if (len(call_ids) != len(message.tool_calls)
                        or len(set(call_ids)) != len(call_ids)):
                    return False
                pending = set(call_ids)
        return pending is None

    @staticmethod
    def _compaction_groups(messages: List[ChatMessage]) -> List[List[ChatMessage]]:
        """Group complete user turns without splitting tool-call/result clusters."""
        groups: List[List[ChatMessage]] = []
        current: List[ChatMessage] = []
        for message in messages:
            if message.role == "user" and current:
                groups.append(current)
                current = []
            current.append(message)
        if current:
            groups.append(current)
        return groups

    def _auto_compress(self, conversation: Optional[Conversation] = None,
                       run: Optional[_RunContext] = None) -> None:
        """Compact conversation when token usage exceeds 90% of model context window.

        Compaction removes only complete, oldest user turns.  An assistant
        ``tool_calls`` message and every matching ``tool`` result therefore
        remain adjacent and are either retained or summarized together.
        """
        from ai_editor.llm_engine import compaction_threshold
        target_conversation = conversation or self.conversation
        if run is not None and not self._is_active_run(run):
            return
        engine = run.engine if run is not None else self.engine
        model = engine.config.effective_model
        threshold = compaction_threshold(model)
        if threshold <= 0:
            return

        api_msgs = target_conversation.to_api_messages()
        total_tokens = self.engine.count_message_tokens(api_msgs)
        # Emit token usage warnings at 50/75/90/95% thresholds
        if self.on_token_warning and threshold > 0:
            ratio = total_tokens / threshold
            for pct in (0.50, 0.75, 0.90, 0.95):
                if ratio >= pct:
                    if run is None:
                        self.on_token_warning(total_tokens, threshold, ratio)
                    else:
                        with self._state_lock:
                            if self._is_active_run_locked(run):
                                self.on_token_warning(total_tokens, threshold, ratio)
                    break
        if total_tokens < threshold:
            return

        with target_conversation._lock:
            msgs = list(target_conversation.messages)
        if (len(msgs) <= self.KEEP_RECENT_MIN
                or not self._tool_sequence_is_valid(msgs)):
            return

        groups = self._compaction_groups(msgs)
        if len(groups) <= 1:
            return

        remaining = list(groups)
        removed: List[List[ChatMessage]] = []
        target_tokens = threshold * 0.7
        while len(remaining) > 1:
            flat_remaining = [message for group in remaining for message in group]
            test_msgs = [{
                "role": "system",
                "content": target_conversation.system_prompt or "",
            }]
            test_msgs.extend(message.to_api_dict() for message in flat_remaining)
            if removed and self.engine.count_message_tokens(test_msgs) < target_tokens:
                break
            candidate = remaining[1:]
            if sum(len(group) for group in candidate) < self.KEEP_RECENT_MIN:
                break
            removed.append(remaining.pop(0))

        if not removed:
            return

        old = [message for group in removed for message in group]
        recent = [message for group in remaining for message in group]
        if not self._tool_sequence_is_valid(recent):
            return

        roles: dict = {}
        for m in old:
            roles[m.role] = roles.get(m.role, 0) + 1
        topics = [m.content[:150] for m in old if m.role == "user" and m.content][:3]
        summary = ChatMessage(
            role="system",
            content=(
                f"[Compacted: {len(old)} earlier messages "
                f"({', '.join(f'{c} {r}' for r, c in roles.items())}). "
                f"Topics: {'; '.join(topics)}]"
            ),
        )
        if run is None:
            with target_conversation._lock:
                target_conversation.messages[:] = [summary, *recent]
                target_conversation._msg_version += 1
                target_conversation._api_cache = None
        else:
            with self._state_lock:
                if not self._is_active_run_locked(run):
                    return
                with target_conversation._lock:
                    target_conversation.messages[:] = [summary, *recent]
                    target_conversation._msg_version += 1
                    target_conversation._api_cache = None

    @staticmethod
    def _tool_cache_key(name: str, arguments: str) -> str:
        try:
            normalized = json.dumps(
                json.loads(arguments), ensure_ascii=False,
                sort_keys=True, separators=(",", ":"),
            )
        except (TypeError, ValueError, json.JSONDecodeError):
            normalized = str(arguments)
        return f"{name}:{normalized}"

    def _tool_cache_get(self, key: str) -> Optional[str]:
        with self._tool_result_cache_lock:
            if key not in self._tool_result_cache:
                return None
            value = self._tool_result_cache[key]
            self._tool_result_cache_keys[:] = [
                existing for existing in self._tool_result_cache_keys
                if existing != key
            ]
            self._tool_result_cache_keys.append(key)
            return value

    def _tool_cache_put(self, key: str, value: str) -> None:
        with self._tool_result_cache_lock:
            self._tool_result_cache[key] = value
            self._tool_result_cache_keys[:] = [
                existing for existing in self._tool_result_cache_keys
                if existing != key
            ]
            self._tool_result_cache_keys.append(key)
            while len(self._tool_result_cache_keys) > 50:
                old_key = self._tool_result_cache_keys.pop(0)
                self._tool_result_cache.pop(old_key, None)

    def _invalidate_tool_cache(self) -> None:
        with self._tool_result_cache_lock:
            self._tool_result_cache.clear()
            self._tool_result_cache_keys.clear()

    def _record_tool_result(self, run: _RunContext, tool_call: ToolCall,
                            result: str, state: ToolInvocationState) -> bool:
        with self._state_lock:
            if not self._is_active_run_locked(run):
                return False
            tool_call.result = result
            self._set_tool_state(tool_call.id, state)
            tool_msg = ChatMessage(
                role="tool",
                content=result,
                tool_call_id=tool_call.id,
                tool_name=tool_call.name,
            )
            run.conversation.add_message(tool_msg)
            if self.on_message_added:
                self.on_message_added(tool_msg)
            if self.on_tool_end:
                self._notify_tool_end(
                    tool_call.id, tool_call.name, result, state)
            return True

    def _legacy_run_context(self) -> _RunContext:
        """Support older direct ``_run_loop()`` tests and integrations."""
        with self._state_lock:
            if self._active_run is not None:
                return self._active_run
            run = _RunContext(
                run_id=uuid.uuid4().hex[:12],
                conversation=self.conversation,
                cancel_event=threading.Event(),
                engine=self._fork_engine_for_run(),
                agent_mode=bool(self._agent_mode),
                multimodal_content=self._multimodal_override,
                thread=threading.current_thread(),
            )
            self._active_run = run
            self._running = True
            return run

    def _run_loop(self, run: Optional[_RunContext] = None) -> None:
        run = run or self._legacy_run_context()
        max_rounds = self.MAX_AGENT_ROUNDS if run.agent_mode else self.MAX_TOOL_ROUNDS
        try:
            self._auto_compress(run.conversation, run)
            for _round in range(max_rounds):
                if not self._is_active_run(run):
                    break

                # Build the request before adding the streaming placeholder;
                # the placeholder is UI state and must never be sent upstream.
                messages = run.conversation.to_api_messages()
                multimodal_content = run.multimodal_content
                if multimodal_content is not None:
                    for i in range(len(messages) - 1, -1, -1):
                        if messages[i].get("role") == "user":
                            messages[i]["content"] = multimodal_content
                            break
                    run.multimodal_content = None
                    with self._state_lock:
                        if self._is_active_run_locked(run):
                            self._multimodal_override = None

                assistant_msg = ChatMessage(
                    role="assistant",
                    is_streaming=True,
                    model=run.engine.config.effective_model,
                )
                if not self._add_message_for_run(
                        run, assistant_msg, current_assistant=True):
                    break

                tools = self.registry.to_openai_tools()
                if self.extra_tools:
                    tools = (tools or []) + self.extra_tools
                if tools and self._disabled_tools:
                    tools = [
                        tool for tool in tools
                        if tool.get("function", {}).get("name")
                        not in self._disabled_tools
                    ]
                tools = tools or None

                first_token_time: List[Optional[float]] = [None]
                request_start = time.monotonic()
                request_tokens = run.engine.count_message_tokens(messages)

                def _on_delta(delta: StreamDelta, _msg=assistant_msg) -> None:
                    with self._state_lock:
                        if not self._is_active_run_locked(run):
                            return
                        if ((delta.content or delta.thinking)
                                and first_token_time[0] is None):
                            first_token_time[0] = time.monotonic()
                        if delta.content and self.on_stream_delta:
                            self.on_stream_delta(_msg, delta.content)
                        if delta.thinking and self.on_thinking_delta:
                            self.on_thinking_delta(_msg, delta.thinking)

                with self._state_lock:
                    if not self._is_active_run_locked(run):
                        break
                    run.engine.reset_cancel()
                try:
                    resp = run.engine.chat_completion_stream(
                        messages=messages,
                        tools=tools,
                        on_delta=_on_delta,
                    )
                except Exception:
                    if not self._is_active_run(run):
                        break
                    raise

                with self._state_lock:
                    if not self._is_active_run_locked(run):
                        break
                    assistant_msg.content = resp.content
                    assistant_msg.thinking = resp.thinking
                    assistant_msg.tool_calls = resp.tool_calls
                    assistant_msg.usage = dict(resp.usage or {})
                    if request_tokens and "context_tokens" not in assistant_msg.usage:
                        assistant_msg.usage["context_tokens"] = request_tokens
                    if not assistant_msg.usage.get("total_tokens"):
                        output_tokens = run.engine.estimate_tokens(
                            (resp.content or "") + (resp.thinking or ""))
                        if output_tokens or request_tokens:
                            assistant_msg.usage.setdefault("input_tokens", request_tokens)
                            assistant_msg.usage.setdefault("output_tokens", output_tokens)
                            assistant_msg.usage["total_tokens"] = (
                                int(assistant_msg.usage.get("input_tokens") or 0)
                                + int(assistant_msg.usage.get("output_tokens") or 0)
                            )
                            assistant_msg.usage.setdefault("estimated", True)
                    if first_token_time[0] is not None:
                        assistant_msg.usage["ttft_ms"] = round(
                            (first_token_time[0] - request_start) * 1000, 1)
                        assistant_msg.usage["total_ms"] = round(
                            (time.monotonic() - request_start) * 1000, 1)
                    assistant_msg.model = resp.model
                    assistant_msg.is_streaming = False
                    if resp.error:
                        assistant_msg.is_error = True
                        assistant_msg.content = resp.error
                    with run.conversation._lock:
                        run.conversation._msg_version += 1
                        run.conversation._api_cache = None
                    if self.on_stream_end:
                        self.on_stream_end(assistant_msg)

                if resp.error:
                    break

                if not resp.tool_calls:
                    if not run.agent_mode:
                        break
                    content_lower = (resp.content or "").strip().lower()
                    continues = resp.finish_reason in {"length", "max_tokens"}
                    if not continues:
                        continues = any(marker in content_lower for marker in (
                            "[continue]",
                            "<continue>",
                            "<continue/>",
                            "continue with the next step",
                            "continue from here",
                            "继续",
                        ))
                    if not continues:
                        continues = content_lower.endswith(("...", "…"))
                    if not continues:
                        break
                    cont_msg = ChatMessage(
                        role="user",
                        content=(
                            "Continue from the previous step without repeating finished work. "
                            "Use tools again if they are still needed."
                        ),
                    )
                    if not self._add_message_for_run(run, cont_msg):
                        break

                for tool_call in resp.tool_calls:
                    with self._state_lock:
                        if not self._is_active_run_locked(run):
                            break
                        self._set_tool_state(tool_call.id, ToolInvocationState.PENDING)
                        if self.on_tool_start:
                            self.on_tool_start(
                                tool_call.id, tool_call.name, tool_call.arguments,
                                ToolInvocationState.PENDING.value,
                            )

                    desc = self.registry.get(tool_call.name)
                    mcp_allowed = True
                    if tool_call.name.startswith("mcp_") and self.mcp_tool_allowed:
                        mcp_allowed = self.mcp_tool_allowed(tool_call.name)
                    if not mcp_allowed:
                        result = json.dumps({
                            "error": f"MCP tool disabled by policy: {tool_call.name}",
                        })
                        if not self._record_tool_result(
                                run, tool_call, result,
                                ToolInvocationState.CANCELLED):
                            break
                        continue

                    needs_confirm = bool(desc and desc.requires_confirm)
                    if (tool_call.name.startswith("mcp_")
                            and self.mcp_tool_requires_confirm):
                        needs_confirm = self.mcp_tool_requires_confirm(tool_call.name)
                    if (needs_confirm and self.on_tool_confirm
                            and not self._session_auto_approve.get(
                                tool_call.name, False)):
                        with self._confirm_lock:
                            confirm_event = threading.Event()
                            self._confirm_event = confirm_event
                            self._confirm_result = True
                        try:
                            allowed = self.on_tool_confirm(
                                tool_call.id, tool_call.name, tool_call.arguments)
                        finally:
                            with self._confirm_lock:
                                if self._confirm_event is confirm_event:
                                    self._confirm_event = None
                        with self._state_lock:
                            if not self._is_active_run_locked(run):
                                break
                            if allowed == "always_approve":
                                self._session_auto_approve[tool_call.name] = True
                        if isinstance(allowed, bool) and not allowed:
                            result = json.dumps({
                                "error": "User denied tool execution",
                            })
                            if not self._record_tool_result(
                                    run, tool_call, result,
                                    ToolInvocationState.CANCELLED):
                                break
                            continue

                    with self._state_lock:
                        if not self._is_active_run_locked(run):
                            break
                        self._set_tool_state(
                            tool_call.id, ToolInvocationState.CONFIRMED)
                        # This lock-protected transition is the linearization
                        # point for tool start.  Cancellation after it may not
                        # undo an already-running synchronous tool, but a
                        # retired worker can never initiate another one.
                        self._set_tool_state(
                            tool_call.id, ToolInvocationState.EXECUTING)

                    is_read_only = tool_call.name in _READ_ONLY_TOOLS
                    cache_key = self._tool_cache_key(
                        tool_call.name, tool_call.arguments)
                    cached = self._tool_cache_get(cache_key) if is_read_only else None
                    if cached is not None:
                        result = cached
                    else:
                        try:
                            if (tool_call.name.startswith("mcp_")
                                    and self.mcp_dispatch):
                                result = self.mcp_dispatch(
                                    tool_call.name, tool_call.arguments)
                            else:
                                result = self.registry.execute(
                                    tool_call.name, tool_call.arguments)
                        finally:
                            if not is_read_only:
                                # Also invalidate when an old synchronous write
                                # finishes after cancellation; a new run must
                                # not retain reads made while that write ran.
                                self._invalidate_tool_cache()

                    result = _compress_tool_result(result, tool_call.name)
                    if is_read_only:
                        with self._state_lock:
                            if self._is_active_run_locked(run):
                                self._tool_cache_put(cache_key, result)
                    if not self._record_tool_result(
                            run, tool_call, result,
                            ToolInvocationState.COMPLETED):
                        break

        except Exception as exc:
            with self._state_lock:
                if self._is_active_run_locked(run) and self.on_error:
                    self.on_error(str(exc))
        finally:
            idle_callback: Optional[Callable[[], None]] = None
            with self._state_lock:
                if self._is_active_run_locked(run):
                    self._auto_save(run.conversation)
                    self._running = False
                    self._multimodal_override = None
                    idle_callback = self.on_idle
            try:
                if idle_callback:
                    idle_callback()
            finally:
                with self._state_lock:
                    if self._active_run is run:
                        self._active_run = None
                        if self._thread is run.thread:
                            self._thread = None
                run.finished_event.set()
                if run.engine is not self.engine:
                    close = getattr(run.engine, "close", None)
                    if callable(close):
                        try:
                            close()
                        except Exception:
                            pass

    _last_save_key: Optional[tuple[str, int]] = None

    def _auto_save(self, conversation: Optional[Conversation] = None) -> bool:
        """Persist conversation — skip if nothing changed since last save."""
        try:
            target_conversation = conversation or self.conversation
            if not target_conversation.messages:
                return True
            ver = target_conversation._msg_version
            save_key = (target_conversation.id, ver)
            if save_key == self._last_save_key:
                return True
            from ai_editor.history import save_conversation
            # Build messages list directly instead of serialize+deserialize
            # round-trip through export_messages()/json.loads().
            saved_model = self.engine.config.effective_model
            with target_conversation._lock:
                msgs = []
                for m in target_conversation.messages:
                    d = {
                        "role": m.role,
                        "content": m.content,
                        "timestamp": m.timestamp,
                    }
                    if m.tool_calls:
                        d["tool_calls"] = [
                            {"id": tc.id, "name": tc.name,
                             "arguments": tc.arguments, "result": tc.result}
                            for tc in m.tool_calls
                        ]
                    if m.tool_call_id:
                        d["tool_call_id"] = m.tool_call_id
                        d["tool_name"] = m.tool_name
                    msgs.append(d)
                    if m.role == "assistant" and m.model:
                        saved_model = m.model
            save_conversation(
                conv_id=target_conversation.id,
                title=target_conversation.title,
                messages=msgs,
                system_prompt=target_conversation.system_prompt,
                model=saved_model,
            )
            self._last_save_key = save_key
            self._last_save_error = ""
            return True
        except Exception as exc:
            self._last_save_error = str(exc)
            if self.on_save_error:
                self.on_save_error(self._last_save_error)
            return False

    def new_conversation(self, system_prompt: str = "") -> Conversation:
        with self._lifecycle_lock:
            old_conversation = self.conversation
            self.cancel()
            self._auto_save(old_conversation)
            sp = system_prompt or self.engine.config.system_prompt
            with self._state_lock:
                self.conversation = Conversation(system_prompt=sp)
                # Auto-compress runs at the start of each _run_loop based on token count
                self._session_auto_approve.clear()
                self._tool_states.clear()
                self._invalidate_tool_cache()
                return self.conversation

    def export_messages(self) -> str:
        with self.conversation._lock:
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


# ---------------------------------------------------------------------------
# Tool result compression (VSCode pattern: >1024 chars → compressed)
# ---------------------------------------------------------------------------

_MIN_COMPRESSIBLE = 1024
_MAX_COMPRESSED = 3000
_JSON_PREVIEW_ITEMS = 8
_JSON_PREVIEW_DEPTH = 3

_READ_ONLY_TOOLS = frozenset({
    "readFile", "listFiles", "searchFiles",
    "editor_getContent", "editor_getSelection", "editor_getLanguage",
})

def _compress_tool_result(result: str, tool_name: str) -> str:
    if not isinstance(result, str):
        result = json.dumps(result, ensure_ascii=False, default=str)
    if not result or len(result) <= _MIN_COMPRESSIBLE:
        return result
    stripped = result.lstrip()
    before = len(result)
    if stripped.startswith(("{", "[")):
        try:
            return _compress_json_tool_result(json.loads(result), tool_name, before)
        except (json.JSONDecodeError, ValueError):
            pass
    return _compress_text_tool_result(result, tool_name, before)


def _compress_text_tool_result(result: str, tool_name: str, before: int) -> str:
    lines = result.split("\n")
    if len(lines) > 80:
        head = "\n".join(lines[:30])
        tail = "\n".join(lines[-15:])
        result = (f"{head}\n\n[... {len(lines) - 45} lines omitted, "
                  f"{before} → ~{len(head) + len(tail) + 80} chars. "
                  f"Use readFile for full content ...]\n\n{tail}")
    if len(result) > _MAX_COMPRESSED:
        budget = max(200, _MAX_COMPRESSED - 120)
        result = (_truncate_middle(result, budget)
                  + f"\n\n[Output compressed by {tool_name}: {before} → <= {_MAX_COMPRESSED} chars]")
    return result


def _compress_json_tool_result(value: Any, tool_name: str, before: int) -> str:
    preview = _json_preview(value, 0)
    payload = {
        "_compressed": True,
        "tool": tool_name,
        "original_chars": before,
        "note": "Large JSON tool result summarized to protect chat context. Re-run a narrower tool call or read the source file for full content.",
        "preview": preview,
    }
    text = json.dumps(payload, ensure_ascii=False, indent=2, default=str)
    if len(text) <= _MAX_COMPRESSED:
        return text
    compact = json.dumps(value, ensure_ascii=False, default=str)
    payload["preview"] = _truncate_middle(compact, max(200, _MAX_COMPRESSED - 450))
    payload["preview_truncated"] = True
    text = json.dumps(payload, ensure_ascii=False, indent=2, default=str)
    if len(text) > _MAX_COMPRESSED:
        payload["preview"] = _truncate_middle(str(payload["preview"]), max(80, _MAX_COMPRESSED - 650))
        text = json.dumps(payload, ensure_ascii=False, separators=(",", ":"), default=str)
    return text


def _json_preview(value: Any, depth: int) -> Any:
    if depth >= _JSON_PREVIEW_DEPTH:
        return _json_leaf(value)
    if isinstance(value, dict):
        items = list(value.items())
        out: Dict[str, Any] = {}
        for key, item in items[:_JSON_PREVIEW_ITEMS]:
            out[str(key)] = _json_preview(item, depth + 1)
        if len(items) > _JSON_PREVIEW_ITEMS:
            out["_omitted_keys"] = len(items) - _JSON_PREVIEW_ITEMS
        return out
    if isinstance(value, list):
        out = [_json_preview(item, depth + 1) for item in value[:_JSON_PREVIEW_ITEMS]]
        if len(value) > _JSON_PREVIEW_ITEMS:
            out.append({"_omitted_items": len(value) - _JSON_PREVIEW_ITEMS})
        return out
    return _json_leaf(value)


def _json_leaf(value: Any) -> Any:
    if isinstance(value, str):
        return _truncate_middle(value, 500)
    if isinstance(value, (int, float, bool)) or value is None:
        return value
    if isinstance(value, dict):
        return {"_type": "object", "keys": len(value)}
    if isinstance(value, list):
        return {"_type": "array", "items": len(value)}
    return _truncate_middle(str(value), 500)


def _truncate_middle(text: str, limit: int) -> str:
    if len(text) <= limit:
        return text
    marker = f"... [{len(text) - limit} chars omitted] ..."
    if limit <= len(marker) + 20:
        return text[:limit]
    keep = limit - len(marker)
    head = keep // 2
    tail = keep - head
    return text[:head] + marker + text[-tail:]
