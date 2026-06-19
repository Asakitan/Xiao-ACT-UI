"""Conversation state management for the AI Editor.

Tracks message history, tool-call bookkeeping, and supports multi-turn
conversations with streaming.  Thread-safe for the Tk after-based UI loop.
"""

from __future__ import annotations

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
                return self._api_cache
            msgs: List[Dict[str, Any]] = []
            if self.system_prompt:
                msgs.append({"role": "system", "content": self.system_prompt})
            for m in self.messages:
                msgs.append(m.to_api_dict())
            self._api_cache = msgs
            self._api_cache_ver = self._msg_version
            return list(msgs)

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
        self.extra_tools: Optional[List[Dict[str, Any]]] = None  # MCP tools injected by app
        self.mcp_dispatch: Optional[Callable[[str, str], str]] = None  # MCP tool call dispatcher
        self.mcp_tool_requires_confirm: Optional[Callable[[str], bool]] = None
        self.mcp_tool_allowed: Optional[Callable[[str], bool]] = None
        self._agent_mode: bool = False
        self._disabled_tools: set = set()
        self._tool_result_cache: Dict[str, str] = {}
        self._tool_result_cache_keys: List[str] = []
        self._multimodal_override: Optional[Any] = None

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
        self.on_tool_end: Optional[Callable[[str, str, str], None]] = None  # call_id, result, state
        self.on_tool_confirm: Optional[Callable[[str, str, str], Any]] = None  # call_id, name, args → bool|"always_approve"
        self.on_tool_progress: Optional[Callable[[str, str, float], None]] = None  # call_id, name, progress 0-1
        self.on_error: Optional[Callable[[str], None]] = None
        self.on_idle: Optional[Callable[[], None]] = None
        self.resolve_variable: Optional[Callable[[str], str]] = None  # @mention resolver

        # Confirmation state (thread-safe)
        self._confirm_lock = threading.Lock()
        self._confirm_event: Optional[threading.Event] = None
        self._confirm_result: bool = True

    @property
    def is_running(self) -> bool:
        return self._running

    def _set_tool_state(self, call_id: str, state: ToolInvocationState) -> None:
        """Update the state machine for a tool invocation."""
        with self._tool_states_lock:
            self._tool_states[call_id] = state

    def get_tool_state(self, call_id: str) -> Optional[ToolInvocationState]:
        """Query current state of a tool invocation."""
        with self._tool_states_lock:
            return self._tool_states.get(call_id)

    def send(self, text: str, agent_mode: bool = False) -> None:
        if self._running:
            return
        resolved = self._resolve_at_mentions(text)
        user_msg = ChatMessage(role="user", content=resolved)
        self.conversation.add_message(user_msg)
        if self.on_message_added:
            self.on_message_added(user_msg)
        self._running = True
        self._agent_mode = agent_mode
        self._thread = threading.Thread(target=self._run_loop, daemon=True)
        self._thread.start()

    def send_multimodal(self, display_text: str, multimodal_content: Any,
                        agent_mode: bool = False) -> None:
        if self._running:
            return
        user_msg = ChatMessage(role="user", content=display_text)
        self.conversation.add_message(user_msg)
        if self.on_message_added:
            self.on_message_added(user_msg)
        self._multimodal_override = multimodal_content
        self._running = True
        self._agent_mode = agent_mode
        self._thread = threading.Thread(target=self._run_loop, daemon=True)
        self._thread.start()

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
        self._running = False
        self.engine.cancel()
        with self._confirm_lock:
            evt = self._confirm_event
            if evt is not None:
                evt.set()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=5.0)
            if self._thread.is_alive():
                self._thread = None

    MAX_AGENT_ROUNDS = 25
    KEEP_RECENT_MIN = 6

    # Role weights for compaction priority (higher = more important to keep)
    _COMPACTION_WEIGHTS: Dict[str, int] = {
        "system": 10, "user": 3, "assistant": 5, "tool": 1,
    }

    def _auto_compress(self) -> None:
        """Compact conversation when token usage exceeds 90% of model context window.

        Uses weighted compaction: messages with lower role-based weights are
        trimmed first, keeping high-weight messages (system, assistant) longer.
        Threshold is purely token-driven — no message count limit.
        """
        from ai_editor.llm_engine import compaction_threshold
        model = self.engine.config.effective_model
        threshold = compaction_threshold(model)
        if threshold <= 0:
            return

        api_msgs = self.conversation.to_api_messages()
        total_tokens = self.engine.count_message_tokens(api_msgs)
        # Emit token usage warnings at 50/75/90/95% thresholds
        if self.on_token_warning and threshold > 0:
            ratio = total_tokens / threshold
            for pct in (0.50, 0.75, 0.90, 0.95):
                if ratio >= pct:
                    self.on_token_warning(total_tokens, threshold, ratio)
                    break
        if total_tokens < threshold:
            return

        with self.conversation._lock:
            msgs = list(self.conversation.messages)
        if len(msgs) <= self.KEEP_RECENT_MIN * 2:
            return

        keep = self.KEEP_RECENT_MIN
        while keep < len(msgs) - 2:
            test_msgs = [{"role": "system", "content": self.conversation.system_prompt or ""}]
            test_msgs.extend(m.to_api_dict() for m in msgs[-keep:])
            if self.engine.count_message_tokens(test_msgs) < threshold * 0.7:
                keep += 2
            else:
                break

        cut = len(msgs) - keep
        if cut <= 0:
            return

        # Sort candidates by weight (ascending) so lowest-weight messages are
        # trimmed first while higher-weight messages survive compaction.
        candidates = list(enumerate(msgs[:cut]))
        candidates.sort(key=lambda pair: self._COMPACTION_WEIGHTS.get(pair[1].role, 1))

        # Build the trimmed set: drop lowest-weight messages first
        trim_count = max(1, cut // 2)
        trim_indices = {idx for idx, _m in candidates[:trim_count]}

        old = [m for i, m in enumerate(msgs[:cut]) if i in trim_indices]
        kept_from_old = [m for i, m in enumerate(msgs[:cut]) if i not in trim_indices]

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
        recent = msgs[cut:]
        with self.conversation._lock:
            self.conversation.messages.clear()
            self.conversation.messages.append(summary)
            self.conversation.messages.extend(kept_from_old)
            self.conversation.messages.extend(recent)
            self.conversation._msg_version += 1
            self.conversation._api_cache = None

    def _run_loop(self) -> None:
        max_rounds = self.MAX_AGENT_ROUNDS if self._agent_mode else self.MAX_TOOL_ROUNDS
        try:
            self._auto_compress()
            for _round in range(max_rounds):
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
                if self._multimodal_override is not None:
                    for i in range(len(messages) - 1, -1, -1):
                        if messages[i].get("role") == "user":
                            messages[i]["content"] = self._multimodal_override
                            break
                    self._multimodal_override = None
                tools = self.registry.to_openai_tools()
                if self.extra_tools:
                    tools = (tools or []) + self.extra_tools
                if tools and self._disabled_tools:
                    tools = [t for t in tools
                             if t.get("function", {}).get("name")
                             not in self._disabled_tools]
                tools = tools or None

                _first_token_time = [None]
                _request_start = time.monotonic()

                def _on_delta(delta: StreamDelta, _msg=assistant_msg) -> None:
                    if (delta.content or delta.thinking) and _first_token_time[0] is None:
                        _first_token_time[0] = time.monotonic()
                    if delta.content and self.on_stream_delta:
                        self.on_stream_delta(_msg, delta.content)
                    if delta.thinking and self.on_thinking_delta:
                        self.on_thinking_delta(_msg, delta.thinking)

                self.engine.reset_cancel()
                if not self._running:
                    break
                try:
                    resp = self.engine.chat_completion_stream(
                        messages=messages,
                        tools=tools,
                        on_delta=_on_delta,
                    )
                except Exception as exc:
                    if not self._running:
                        break
                    raise

                assistant_msg.content = resp.content
                assistant_msg.thinking = resp.thinking
                assistant_msg.tool_calls = resp.tool_calls
                assistant_msg.usage = resp.usage or {}
                if _first_token_time[0] is not None:
                    assistant_msg.usage["ttft_ms"] = round(
                        (_first_token_time[0] - _request_start) * 1000, 1)
                    assistant_msg.usage["total_ms"] = round(
                        (time.monotonic() - _request_start) * 1000, 1)
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
                    if not self._agent_mode:
                        break
                    # Agent mode: continue if the response ends with a continuation signal
                    content_lower = (resp.content or "").strip().lower()
                    continues = content_lower.endswith("...") or "[continue]" in content_lower
                    if not continues:
                        break
                    # Auto-inject continuation prompt
                    cont_msg = ChatMessage(role="user", content="Continue with the next step.")
                    self.conversation.add_message(cont_msg)
                    if self.on_message_added:
                        self.on_message_added(cont_msg)

                for tc in resp.tool_calls:
                    if not self._running:
                        break

                    # Track tool invocation state
                    self._set_tool_state(tc.id, ToolInvocationState.PENDING)

                    if self.on_tool_start:
                        self.on_tool_start(tc.id, tc.name, tc.arguments,
                                           ToolInvocationState.PENDING.value)

                    # Confirmation gate for dangerous tools
                    desc = self.registry.get(tc.name)
                    mcp_allowed = True
                    if tc.name.startswith("mcp_") and self.mcp_tool_allowed:
                        mcp_allowed = self.mcp_tool_allowed(tc.name)
                    if not mcp_allowed:
                        self._set_tool_state(tc.id, ToolInvocationState.CANCELLED)
                        result = json.dumps({"error": f"MCP tool disabled by policy: {tc.name}"})
                        tc.result = result
                        tool_msg = ChatMessage(role="tool", content=result,
                                               tool_call_id=tc.id, tool_name=tc.name)
                        self.conversation.add_message(tool_msg)
                        if self.on_tool_end:
                            self.on_tool_end(tc.id, result,
                                             ToolInvocationState.CANCELLED.value)
                        continue
                    needs_confirm = bool(desc and desc.requires_confirm)
                    if tc.name.startswith("mcp_") and self.mcp_tool_requires_confirm:
                        needs_confirm = self.mcp_tool_requires_confirm(tc.name)
                    if (needs_confirm and self.on_tool_confirm
                            and not self._session_auto_approve.get(tc.name, False)):
                        with self._confirm_lock:
                            self._confirm_event = threading.Event()
                            self._confirm_result = True
                        allowed = self.on_tool_confirm(tc.id, tc.name, tc.arguments)
                        if allowed == "always_approve":
                            self._session_auto_approve[tc.name] = True
                        elif isinstance(allowed, bool) and not allowed:
                            self._set_tool_state(tc.id, ToolInvocationState.CANCELLED)
                            result = json.dumps({"error": "User denied tool execution"})
                            tc.result = result
                            tool_msg = ChatMessage(role="tool", content=result,
                                                   tool_call_id=tc.id, tool_name=tc.name)
                            self.conversation.add_message(tool_msg)
                            if self.on_tool_end:
                                self.on_tool_end(tc.id, result,
                                                 ToolInvocationState.CANCELLED.value)
                            continue

                    self._set_tool_state(tc.id, ToolInvocationState.CONFIRMED)

                    # Build progress callback for this tool call
                    def _make_progress_cb(call_id: str, name: str):
                        def _progress(fraction: float) -> None:
                            if self.on_tool_progress:
                                self.on_tool_progress(call_id, name, fraction)
                        return _progress
                    _progress_cb = _make_progress_cb(tc.id, tc.name)

                    self._set_tool_state(tc.id, ToolInvocationState.EXECUTING)

                    cache_key = f"{tc.name}:{tc.arguments}"
                    cached = self._tool_result_cache.get(cache_key)
                    if cached is not None and tc.name in _READ_ONLY_TOOLS:
                        result = cached
                    elif tc.name.startswith("mcp_") and self.mcp_dispatch:
                        result = self.mcp_dispatch(tc.name, tc.arguments)
                    else:
                        result = self.registry.execute(tc.name, tc.arguments)

                    result = _compress_tool_result(result, tc.name)

                    if tc.name in _READ_ONLY_TOOLS:
                        self._tool_result_cache[cache_key] = result
                        self._tool_result_cache_keys.append(cache_key)
                        if len(self._tool_result_cache_keys) > 50:
                            old_key = self._tool_result_cache_keys.pop(0)
                            self._tool_result_cache.pop(old_key, None)

                    tc.result = result
                    self._set_tool_state(tc.id, ToolInvocationState.COMPLETED)

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
                        self.on_tool_end(tc.id, result,
                                         ToolInvocationState.COMPLETED.value)

        except Exception as exc:
            if self.on_error:
                self.on_error(str(exc))
        finally:
            self._running = False
            self._auto_save()
            if self.on_idle:
                self.on_idle()

    _last_save_ver: int = -1

    def _auto_save(self) -> None:
        """Persist conversation — skip if nothing changed since last save."""
        try:
            if not self.conversation.messages:
                return
            ver = self.conversation._msg_version
            if ver == self._last_save_ver:
                return
            from ai_editor.history import save_conversation
            # Build messages list directly instead of serialize+deserialize
            # round-trip through export_messages()/json.loads().
            with self.conversation._lock:
                msgs = []
                for m in self.conversation.messages:
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
            save_conversation(
                conv_id=self.conversation.id,
                title=self.conversation.title,
                messages=msgs,
                system_prompt=self.conversation.system_prompt,
                model=self.engine.config.effective_model,
            )
            self._last_save_ver = ver
        except Exception:
            pass

    def new_conversation(self, system_prompt: str = "") -> Conversation:
        self._auto_save()
        self.cancel()
        sp = system_prompt or self.engine.config.system_prompt
        self.conversation = Conversation(system_prompt=sp)
        # Auto-compress runs at the start of each _run_loop based on token count
        self._session_auto_approve.clear()
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

_READ_ONLY_TOOLS = frozenset({
    "readFile", "listFiles", "searchFiles",
    "editor_getContent", "editor_getSelection", "editor_getLanguage",
})

_STRUCTURED_PREFIXES = ('{', '[', '---', '<!', '<?xml')


def _compress_tool_result(result: str, tool_name: str) -> str:
    if not result or len(result) <= _MIN_COMPRESSIBLE:
        return result
    stripped = result.lstrip()
    if any(stripped.startswith(p) for p in _STRUCTURED_PREFIXES):
        try:
            json.loads(result)
            return result
        except (json.JSONDecodeError, ValueError):
            pass
    before = len(result)
    lines = result.split("\n")
    if len(lines) > 80:
        head = "\n".join(lines[:30])
        tail = "\n".join(lines[-15:])
        result = (f"{head}\n\n[... {len(lines) - 45} lines omitted, "
                  f"{before} → ~{len(head) + len(tail) + 80} chars. "
                  f"Use readFile for full content ...]\n\n{tail}")
    elif before > _MAX_COMPRESSED:
        result = (result[:_MAX_COMPRESSED]
                  + f"\n\n[Output compressed: {before} → {_MAX_COMPRESSED} chars]")
    return result
