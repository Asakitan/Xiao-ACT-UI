"""VSCode API compatibility layer for the SAO AI Editor.

Provides the ``vscode.*`` namespace that VSCode extensions expect.
Maps API calls to our native infrastructure (LLMEngine, ToolRegistry,
CommandService, ChatController, etc.).

Implemented namespaces:
  vscode.commands     registerCommand / executeCommand
  vscode.window       showInformationMessage / createOutputChannel / ...
  vscode.workspace    getConfiguration / onDidChangeConfiguration / fs
  vscode.env          appName / language / uriScheme / clipboard
  vscode.extensions   getExtension / all
  vscode.lm           selectChatModels / registerTool
  vscode.chat         createChatParticipant
  vscode.authentication  getSession / registerAuthenticationProvider
  vscode.Uri / Position / Range / Disposable / EventEmitter / ...
"""

from __future__ import annotations

import asyncio
import base64
import inspect
import json
import os
import fnmatch
import re
import secrets
import subprocess
import threading
import time
import uuid
import webbrowser
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Protocol, Sequence, runtime_checkable

from ai_editor.extension_host import (
    CommandService, ExtensionHost, ExtensionDescription,
    Position, Range, Uri, Disposable, EventEmitter,
)

_PROVIDER_RESULT_TIMEOUT = 0.75


def _resolve_provider_result(
        value: Any,
        default: Any = None,
        timeout: float = _PROVIDER_RESULT_TIMEOUT) -> Any:
    """Resolve VS Code ProviderResult/Thenable-like values with a short bound."""
    if value is None:
        return default
    if inspect.isawaitable(value):
        return _resolve_awaitable_provider_result(value, default, timeout)
    result = getattr(value, "result", None)
    if callable(result):
        try:
            return _resolve_provider_result(
                result(timeout=timeout), default=default, timeout=timeout)
        except TypeError:
            done = getattr(value, "done", None)
            if callable(done):
                try:
                    if not done():
                        return default
                except Exception:
                    return default
            return _resolve_provider_result(
                result(), default=default, timeout=timeout)
        except TimeoutError:
            return default
    then = getattr(value, "then", None)
    if callable(then):
        return _resolve_thenable_provider_result(value, then, default, timeout)
    return value


def _resolve_awaitable_provider_result(
        value: Any, default: Any, timeout: float) -> Any:
    box: Dict[str, Any] = {}
    finished = threading.Event()

    def _run() -> None:
        async def _await_value() -> Any:
            return await asyncio.wait_for(value, timeout=timeout)

        try:
            box["value"] = asyncio.run(_await_value())
        except TimeoutError:
            box["value"] = default
        except Exception as exc:
            box["error"] = exc
        finally:
            finished.set()

    threading.Thread(target=_run, daemon=True).start()
    if not finished.wait(timeout + 0.05):
        return default
    if "error" in box:
        raise box["error"]
    return _resolve_provider_result(box.get("value"), default=default,
                                    timeout=timeout)


def _resolve_thenable_provider_result(
        value: Any, then: Callable, default: Any, timeout: float) -> Any:
    box: Dict[str, Any] = {}
    finished = threading.Event()

    def _resolve(resolved: Any = None) -> None:
        box["value"] = resolved
        finished.set()

    def _reject(error: Any = None) -> None:
        box["error"] = error
        finished.set()

    try:
        chained = then(_resolve, _reject)
    except TypeError:
        chained = then(_resolve)
    if finished.wait(timeout):
        if "error" in box:
            error = box["error"]
            if isinstance(error, BaseException):
                raise error
            raise RuntimeError(str(error))
        return _resolve_provider_result(box.get("value"), default=default,
                                        timeout=timeout)
    if chained is not None and chained is not value:
        return _resolve_provider_result(chained, default=default,
                                        timeout=timeout)
    return default


def _call_with_compatible_args(fn: Callable, args: Sequence[Any]) -> Any:
    try:
        signature = inspect.signature(fn)
    except (TypeError, ValueError):
        return fn(*args)
    params = list(signature.parameters.values())
    if any(p.kind == inspect.Parameter.VAR_POSITIONAL for p in params):
        return fn(*args)
    positional = [
        p for p in params
        if p.kind in (
            inspect.Parameter.POSITIONAL_ONLY,
            inspect.Parameter.POSITIONAL_OR_KEYWORD,
        )
    ]
    if not positional:
        return fn()
    required = [
        p for p in positional
        if p.default is inspect.Parameter.empty
    ]
    count = min(len(args), len(positional))
    if count < len(required):
        count = len(args)
    return fn(*args[:count])


def _completion_list_from_provider_result(value: Any) -> Optional["CompletionList"]:
    if value is None:
        return None
    if isinstance(value, CompletionList):
        return value
    if isinstance(value, list):
        return CompletionList(value)
    if isinstance(value, dict) and "items" in value:
        return CompletionList(
            value.get("items") or [],
            bool(value.get("isIncomplete") or value.get("is_incomplete")),
        )
    items = getattr(value, "items", None)
    if items is not None:
        return CompletionList(
            items,
            bool(getattr(value, "isIncomplete",
                         getattr(value, "is_incomplete", False))),
        )
    return CompletionList([value])


# NOTE: UIBridge Protocol is defined once below (after WorkspaceEdit) with
# the complete method set including show_quick_pick, show_input_box, and
# webview panel methods.  Do NOT add a duplicate here.


# ---------------------------------------------------------------------------
# Language Model types
# ---------------------------------------------------------------------------

@dataclass
class _LMCapabilities:
    supports_image_to_text: bool = False
    supports_tool_calling: bool = True
    edit_tools_hint: bool = False


@dataclass
class LanguageModelChat:
    id: str = ""
    name: str = ""
    vendor: str = ""
    family: str = ""
    version: str = ""
    max_input_tokens: int = 128000
    capabilities: _LMCapabilities = field(default_factory=_LMCapabilities)
    pricing: Optional[Dict[str, float]] = None

    @staticmethod
    def _parse_tool_call_input(value: Any) -> Any:
        if value is None or value == "":
            return {}
        if isinstance(value, str):
            try:
                return json.loads(value)
            except json.JSONDecodeError:
                return value
        return value

    @classmethod
    def _tool_call_part(cls, value: Any) -> Optional["LanguageModelToolCallPart"]:
        if isinstance(value, dict):
            fn = value.get("function") if isinstance(value.get("function"), dict) else {}
            call_id = (
                value.get("call_id")
                or value.get("toolCallId")
                or value.get("callId")
                or value.get("id")
                or value.get("item_id")
                or ""
            )
            name = value.get("name") or fn.get("name") or ""
            raw_input = (
                value.get("input")
                if "input" in value else value.get("parameters")
                if "parameters" in value else value.get("arguments")
                if "arguments" in value else fn.get("arguments")
            )
            if not call_id and not name:
                return None
            return LanguageModelToolCallPart(
                call_id=str(call_id or ""),
                name=str(name or ""),
                input=cls._parse_tool_call_input(raw_input),
            )
        call_id = getattr(value, "id", "") or getattr(value, "call_id", "")
        name = getattr(value, "name", "")
        raw_input = getattr(value, "input", None)
        if raw_input is None:
            raw_input = getattr(value, "arguments", None)
        if not call_id and not name:
            return None
        return LanguageModelToolCallPart(
            call_id=str(call_id or ""),
            name=str(name or ""),
            input=cls._parse_tool_call_input(raw_input),
        )

    @staticmethod
    def _data_bytes(value: Any, encoding: Any = None) -> bytes:
        if value is None:
            return b""
        if isinstance(value, bytes):
            return value
        if isinstance(value, bytearray):
            return bytes(value)
        if isinstance(value, memoryview):
            return value.tobytes()
        if isinstance(value, (list, tuple)):
            try:
                return bytes(int(item) & 0xff for item in value)
            except Exception:
                return json.dumps(value, ensure_ascii=False).encode("utf-8")
        if isinstance(value, str):
            if str(encoding or "").lower() == "base64":
                try:
                    return base64.b64decode(value)
                except Exception:
                    return value.encode("utf-8")
            return value.encode("utf-8")
        try:
            return json.dumps(value, ensure_ascii=False).encode("utf-8")
        except Exception:
            return str(value).encode("utf-8")

    @classmethod
    def _data_part(cls, value: Any) -> Optional["LanguageModelDataPart"]:
        if isinstance(value, LanguageModelDataPart):
            return value
        if isinstance(value, dict):
            part_type = str(value.get("type") or value.get("kind") or "").lower()
            has_data_shape = (
                "data" in value or "value" in value or "content" in value
                or "mimeType" in value or "mime_type" in value or "mime" in value
            )
            if part_type and part_type not in {"data", "data_part", "datapart"}:
                return None
            if not has_data_shape:
                return None
            raw_data = (
                value.get("data")
                if "data" in value else value.get("value")
                if "value" in value else value.get("content", b"")
            )
            mime = (
                value.get("mimeType")
                or value.get("mime_type")
                or value.get("mime")
                or "application/octet-stream"
            )
            encoding = value.get("encoding") or ("base64" if value.get("base64") else None)
            return LanguageModelDataPart(
                data=cls._data_bytes(raw_data, encoding),
                mime_type=str(mime or "application/octet-stream"),
            )
        raw_data = getattr(value, "data", None)
        mime = (
            getattr(value, "mime_type", None)
            or getattr(value, "mimeType", None)
            or getattr(value, "mime", None)
        )
        if raw_data is None and mime is None:
            return None
        return LanguageModelDataPart(
            data=cls._data_bytes(raw_data),
            mime_type=str(mime or "application/octet-stream"),
        )

    @classmethod
    def _data_parts(cls, value: Any) -> List["LanguageModelDataPart"]:
        if value is None:
            return []
        if isinstance(value, (list, tuple)):
            parts = []
            for item in value:
                part = cls._data_part(item)
                if part is not None:
                    parts.append(part)
            return parts
        part = cls._data_part(value)
        return [part] if part is not None else []

    @classmethod
    def _data_parts_from_message_part(
            cls, value: Any) -> List["LanguageModelDataPart"]:
        parts: List[LanguageModelDataPart] = []
        for attr in ("data_parts", "dataParts"):
            raw = (
                value.get(attr) if isinstance(value, dict)
                else getattr(value, attr, None)
            )
            parts.extend(cls._data_parts(raw))
        part = cls._data_part(value)
        if part is not None:
            parts.append(part)
        return parts

    def send_request(self, messages: Any, options: Dict = None,
                     token: Any = None) -> "LanguageModelChatResponse":
        resp = LanguageModelChatResponse()
        if self._engine:
            api_msgs = []
            for m in messages:
                if isinstance(m, LanguageModelChatMessage):
                    api_msgs.append(m.to_api_dict())
                elif isinstance(m, dict):
                    role = m.get("role", "user")
                    if isinstance(m.get("content"), str):
                        api_msgs.append({"role": role, "content": m["content"]})
                    else:
                        api_msgs.append(m)
            tools_schema = None
            if options and options.get("tools"):
                tools_schema = [{"type": "function", "function": t}
                                for t in options["tools"]]
            try:
                self._engine.reset_cancel()
                seen_tool_call_ids: set[str] = set()

                def _append_tool_call(value: Any) -> None:
                    part = self._tool_call_part(value)
                    if not part:
                        return
                    if part.call_id:
                        if part.call_id in seen_tool_call_ids:
                            return
                        seen_tool_call_ids.add(part.call_id)
                    resp._chunks.append(part)

                def _append_data_parts(value: Any) -> None:
                    resp._chunks.extend(self._data_parts_from_message_part(value))

                def _on_delta(delta: Any) -> None:
                    content = getattr(delta, "content", "") or ""
                    if content:
                        resp._chunks.append(LanguageModelTextPart(content))
                    thinking = getattr(delta, "thinking", "") or ""
                    if thinking:
                        resp._chunks.append(LanguageModelThinkingPart(value=thinking))
                    for tool_call in getattr(delta, "tool_calls", []) or []:
                        _append_tool_call(tool_call)
                    _append_data_parts(delta)

                r = self._engine.chat_completion_stream(
                    messages=api_msgs, tools=tools_schema,
                    on_delta=_on_delta)
                resp.text = r.content or ""
                if getattr(r, "thinking", "") and not any(
                        isinstance(item, LanguageModelThinkingPart)
                        for item in resp._chunks):
                    resp._chunks.append(
                        LanguageModelThinkingPart(value=r.thinking))
                for tool_call in getattr(r, "tool_calls", []) or []:
                    _append_tool_call(tool_call)
                _append_data_parts(r)
                if not resp._chunks:
                    resp._chunks = (
                        [LanguageModelTextPart(resp.text)] if resp.text else [])
                resp._done = True
            except Exception as exc:
                resp.text = f"Error: {exc}"
                resp._chunks = [LanguageModelTextPart(resp.text)]
                resp._done = True
        return resp

    def count_tokens(self, text: Any, token: Any = None) -> int:
        if isinstance(text, LanguageModelChatMessage):
            text = "".join(
                p.value for p in text.content
                if isinstance(p, LanguageModelTextPart)
            )
        if self._engine:
            return self._engine.estimate_tokens(str(text))
        return len(str(text)) // 4

    _engine: Any = None


@dataclass
class LanguageModelChatResponse:
    text: str = ""
    _chunks: List[Any] = field(default_factory=list)
    _done: bool = False

    @property
    def stream(self):
        return iter(self._chunks) if self._done else iter([])


# ---------------------------------------------------------------------------
# Chat Participant types
# ---------------------------------------------------------------------------

@dataclass
class ChatRequest:
    prompt: str = ""
    command: str = ""
    references: List[Dict] = field(default_factory=list)
    tool_references: List[Dict] = field(default_factory=list)
    model: Optional[LanguageModelChat] = None
    attempt: int = 0
    enable_command_detection: bool = True
    location: int = 1  # ChatLocation.Panel
    tool_invocation_token: Any = None
    tools: Dict[str, Any] = field(default_factory=dict)
    accepted_confirmation_data: Optional[Dict] = None


class ChatContext:
    def __init__(self) -> None:
        self.history: List[Any] = []
        self.participant: str = ""


class ChatResponseStream:
    def __init__(self, callback: Callable = None) -> None:
        self._parts: List[str] = []
        self._callback = callback

    def markdown(self, value: str) -> None:
        self._parts.append(value)
        if self._callback:
            self._callback("markdown", value)

    def text(self, value: str) -> None:
        self._parts.append(value)
        if self._callback:
            self._callback("text", value)

    def anchor(self, value: Any, title: str = "") -> None:
        label = title or str(value)
        self._parts.append(f"[{label}]({value})")
        if self._callback:
            self._callback("anchor", {"uri": str(value), "title": title})

    def button(self, command: Any) -> None:
        if self._callback:
            self._callback("button", command)

    def progress(self, value: str) -> None:
        if self._callback:
            self._callback("progress", value)

    def warning(self, value: str) -> None:
        self._parts.append(f"⚠ {value}")
        if self._callback:
            self._callback("warning", value)

    def reference(self, uri: Any, icon_path: Any = None) -> None:
        if self._callback:
            self._callback("reference", str(uri))

    def reference2(self, uri: Any, icon_path: Any = None,
                    options: Dict = None) -> None:
        if self._callback:
            self._callback("reference", str(uri))

    def filetree(self, value: Any, base_uri: Any = None) -> None:
        if self._callback:
            self._callback("filetree", {"value": value, "baseUri": str(base_uri) if base_uri else ""})

    def codeblock_uri(self, value: Any, is_edit: bool = False) -> None:
        if self._callback:
            self._callback("codeblockUri", {"uri": str(value), "isEdit": is_edit})

    def code_citation(self, value: Any, license: str = "",
                       snippet: str = "") -> None:
        if self._callback:
            self._callback("codeCitation", {"uri": str(value), "license": license, "snippet": snippet})

    def text_edit(self, target: Any, edits: Any = None) -> None:
        if self._callback:
            self._callback("textEdit", {"target": str(target), "edits": edits})

    def confirmation(self, title: str = "", message: str = "",
                      data: Any = None, buttons: List[str] = None) -> None:
        if self._callback:
            self._callback("confirmation", {"title": title, "message": message,
                                             "data": data, "buttons": buttons or []})

    def info(self, value: str) -> None:
        self._parts.append(f"ℹ {value}")
        if self._callback:
            self._callback("info", value)

    def usage(self, value: Dict[str, int] = None) -> None:
        if self._callback:
            self._callback("usage", value or {})

    def notebook_edit(self, target: Any, edits: Any = None) -> None:
        if self._callback:
            self._callback("notebookEdit", {"target": str(target), "edits": edits})

    def workspace_edit(self, edits: Any = None) -> None:
        if self._callback:
            self._callback("workspaceEdit", {"edits": edits})

    def thinking_progress(self, thinking_delta: str = "") -> None:
        if self._callback:
            self._callback("thinkingProgress", thinking_delta)

    def begin_tool_invocation(self, tool_call_id: str = "",
                               tool_name: str = "",
                               stream_data: Any = None) -> None:
        if self._callback:
            self._callback("beginToolInvocation", {
                "toolCallId": tool_call_id, "toolName": tool_name,
                "streamData": stream_data})

    def update_tool_invocation(self, tool_call_id: str = "",
                                stream_data: Any = None) -> None:
        if self._callback:
            self._callback("updateToolInvocation", {
                "toolCallId": tool_call_id, "streamData": stream_data})

    def push(self, part: Any) -> None:
        self._parts.append(str(part))

    def get_content(self) -> str:
        return "".join(self._parts)


@dataclass
class ChatResult:
    metadata: Dict[str, Any] = field(default_factory=dict)
    error_details: Optional[Dict] = None


class ChatParticipant:
    def __init__(self, participant_id: str,
                 handler: Callable,
                 on_dispose: Optional[Callable[[], None]] = None) -> None:
        self.id = participant_id
        self.request_handler = handler
        self._on_dispose = on_dispose
        self.icon_path: Any = None
        self.followup_provider: Any = None
        self.welcome_message_provider: Any = None
        self.title_provider: Any = None
        self.help_text_provider: Any = None
        self.sample_request: str = ""
        self.is_sticky: bool = False
        self.supports_slow_references: bool = False
        self._feedback_emitter = EventEmitter()
        self._action_emitter = EventEmitter()
        self._pause_state_emitter = EventEmitter()
        self._disposed = False

    def invoke(self, prompt: str = "", command: str = "",
               token: Any = None, **request_fields: Any) -> Dict[str, Any]:
        req = ChatRequest(prompt=prompt, command=command)
        for key, value in request_fields.items():
            if hasattr(req, key):
                setattr(req, key, value)
        ctx = ChatContext()
        stream = ChatResponseStream()
        result = self.request_handler(req, ctx, stream, token)
        return {"content": stream.get_content(), "result": result}

    @property
    def on_did_receive_feedback(self):
        return self._feedback_emitter.event

    @property
    def on_did_perform_action(self):
        return self._action_emitter.event

    @property
    def on_did_change_pause_state(self):
        return self._pause_state_emitter.event

    def dispose(self) -> None:
        self._disposed = True
        if self._on_dispose:
            try:
                self._on_dispose()
            except Exception:
                pass


# ---------------------------------------------------------------------------
# Language Model Tool types
# ---------------------------------------------------------------------------

@dataclass
class LanguageModelToolInvocationOptions:
    input: Any = None
    token: Any = None
    model: Optional[LanguageModelChat] = None


@dataclass
class LanguageModelToolResult:
    content: List[Dict] = field(default_factory=list)

    @staticmethod
    def text(value: str) -> "LanguageModelToolResult":
        return LanguageModelToolResult(content=[{"type": "text", "text": value}])


class PreparedToolInvocation:
    def __init__(self, confirmation: Dict = None) -> None:
        self.invocation_message: str = ""
        self.confirmation_messages: Dict = confirmation or {}


def _coerce_tool_options(value: Any, token: Any = None
                         ) -> LanguageModelToolInvocationOptions:
    if isinstance(value, LanguageModelToolInvocationOptions):
        return LanguageModelToolInvocationOptions(
            input=value.input, token=token or value.token, model=value.model)
    option_keys = {"input", "token", "model", "toolInvocationToken"}
    if isinstance(value, dict) and "input" in value and (
            set(value).issubset(option_keys) or
            any(k in value for k in option_keys - {"input"})):
        return LanguageModelToolInvocationOptions(
            input=value.get("input"),
            token=value.get("token") or value.get("toolInvocationToken") or token,
            model=value.get("model"),
        )
    return LanguageModelToolInvocationOptions(input=value, token=token)


def _tool_schema(tool: Any) -> Dict:
    if isinstance(tool, dict):
        return tool.get("inputSchema") or tool.get("schema") or {}
    return getattr(tool, "inputSchema", {}) or {}


def _unsupported_tool_result(name: str, detail: str) -> LanguageModelToolResult:
    return LanguageModelToolResult.text(
        "unsupported/needsExtensionRuntime: "
        f"LanguageModelTool '{name}' {detail} "
        f"Register a local runtime with vscode.lm.registerTool('{name}', handler) "
        "or provide an object with invoke(options, token).")


def _tool_confirmation_denied_result(name: str) -> LanguageModelToolResult:
    return LanguageModelToolResult.text(json.dumps({
        "ok": False,
        "error": f"Tool invocation was not confirmed: {name}",
        "requiresConfirmation": True,
    }, ensure_ascii=False))


def _call_registered_handler(handler: Callable,
                             options: LanguageModelToolInvocationOptions,
                             token: Any) -> Any:
    try:
        sig = inspect.signature(handler)
    except (TypeError, ValueError):
        return handler(options, token)
    params = list(sig.parameters.values())
    positional = [
        p for p in params
        if p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD)
    ]
    has_varargs = any(p.kind == p.VAR_POSITIONAL for p in params)
    has_varkw = any(p.kind == p.VAR_KEYWORD for p in params)
    keyword_only = [p for p in params if p.kind == p.KEYWORD_ONLY]
    if has_varargs or len(positional) >= 2:
        return handler(options, token)
    if len(positional) == 1:
        return handler(options.input)
    if (has_varkw or keyword_only) and isinstance(options.input, dict):
        return handler(**options.input)
    return handler()


# ---------------------------------------------------------------------------
# Language Model Part Types (VSCode LanguageModelTextPart etc.)
# ---------------------------------------------------------------------------

@dataclass
class LanguageModelTextPart:
    value: str = ""


@dataclass
class LanguageModelToolCallPart:
    call_id: str = ""
    name: str = ""
    input: Any = None


@dataclass
class LanguageModelDataPart:
    data: bytes = b""
    mime_type: str = "application/octet-stream"

    @staticmethod
    def image(data: bytes, mime_type: str = "image/png") -> "LanguageModelDataPart":
        return LanguageModelDataPart(data=data, mime_type=mime_type)

    @staticmethod
    def json_data(value: Any, mime: str = "text/x-json") -> "LanguageModelDataPart":
        raw = json.dumps(value, ensure_ascii=False).encode("utf-8")
        return LanguageModelDataPart(data=raw, mime_type=mime)


@dataclass
class LanguageModelThinkingPart:
    value: str = ""
    id: str = ""
    metadata: Dict[str, Any] = field(default_factory=dict)


class LanguageModelChatMessage:
    """Structured chat message with typed content parts."""

    def __init__(self, role: int, content: Any, name: str = "") -> None:
        self.role = role
        self.name = name
        if isinstance(content, str):
            self._content = [LanguageModelTextPart(value=content)]
        elif isinstance(content, list):
            self._content = list(content)
        else:
            self._content = [LanguageModelTextPart(value=str(content))]

    @staticmethod
    def System(content: Any, name: str = "") -> "LanguageModelChatMessage":
        return LanguageModelChatMessage(0, content, name)

    @staticmethod
    def User(content: Any, name: str = "") -> "LanguageModelChatMessage":
        return LanguageModelChatMessage(1, content, name)

    @staticmethod
    def Assistant(content: Any, name: str = "") -> "LanguageModelChatMessage":
        return LanguageModelChatMessage(2, content, name)

    @property
    def content(self) -> list:
        return self._content

    @content.setter
    def content(self, value: Any) -> None:
        if isinstance(value, str):
            self._content = [LanguageModelTextPart(value=value)]
        elif isinstance(value, list):
            self._content = list(value)

    def to_api_dict(self) -> Dict[str, Any]:
        roles = {0: "system", 1: "user", 2: "assistant"}
        text_parts = []
        for p in self._content:
            if isinstance(p, LanguageModelTextPart):
                text_parts.append(p.value)
            elif isinstance(p, str):
                text_parts.append(p)
        return {"role": roles.get(self.role, "user"),
                "content": "".join(text_parts)}


class LanguageModelError(Exception):
    """Typed LM error with static factory methods matching VSCode."""

    def __init__(self, message: str = "", code: str = "",
                 cause: Any = None) -> None:
        super().__init__(message)
        self.code = code
        self.cause = cause

    @staticmethod
    def NotFound(message: str = "") -> "LanguageModelError":
        return LanguageModelError(message or "Model not found", "NotFound")

    @staticmethod
    def NoPermissions(message: str = "") -> "LanguageModelError":
        return LanguageModelError(message or "No permissions", "NoPermissions")

    @staticmethod
    def Blocked(message: str = "") -> "LanguageModelError":
        return LanguageModelError(message or "Request blocked", "Blocked")


# ---------------------------------------------------------------------------
# CancellationToken
# ---------------------------------------------------------------------------

class CancellationTokenSource:
    def __init__(self) -> None:
        self._cancelled = False
        self._listeners: List[Callable] = []

    @property
    def token(self) -> "CancellationToken":
        return CancellationToken(self)

    def cancel(self) -> None:
        if not self._cancelled:
            self._cancelled = True
            for fn in self._listeners:
                try:
                    fn()
                except Exception:
                    pass

    def dispose(self) -> None:
        self._listeners.clear()


class CancellationToken:
    NONE: "CancellationToken"

    def __init__(self, source: CancellationTokenSource = None) -> None:
        self._source = source

    @property
    def is_cancellation_requested(self) -> bool:
        return self._source._cancelled if self._source else False

    @property
    def isCancellationRequested(self) -> bool:
        return self.is_cancellation_requested

    def on_cancellation_requested(self, listener: Callable) -> Disposable:
        if self._source:
            self._source._listeners.append(listener)
            return Disposable(lambda: self._source._listeners.remove(listener)
                              if listener in self._source._listeners else None)
        return Disposable()

    def onCancellationRequested(self, listener: Callable) -> Disposable:
        return self.on_cancellation_requested(listener)


CancellationToken.NONE = CancellationToken()


# ---------------------------------------------------------------------------
# Configuration API
# ---------------------------------------------------------------------------

_CONFIG_MISSING = object()
_CONFIG_AI_EDITOR_ALIASES = {
    "claude_code",
    "codex",
    "mcp",
    "terminal",
    "extensions",
    "customization",
}


def _config_path(value: Any) -> List[str]:
    text = str(value or "").strip(".")
    return [part for part in text.split(".") if part]


def _config_lookup(data: Any, path: Sequence[str]) -> Any:
    current = data
    for part in path:
        if isinstance(current, dict) and part in current:
            current = current[part]
        else:
            return _CONFIG_MISSING
    return current


def _config_clone(value: Any) -> Any:
    if isinstance(value, dict):
        return {str(key): _config_clone(val) for key, val in value.items()}
    if isinstance(value, list):
        return [_config_clone(item) for item in value]
    return value


def _config_merge_layer(base: Any, layer: Any) -> Any:
    if layer is _CONFIG_MISSING:
        return base
    if base is _CONFIG_MISSING:
        return _config_clone(layer)
    if isinstance(base, dict) and isinstance(layer, dict):
        merged = _config_clone(base)
        for key, value in layer.items():
            merged[str(key)] = _config_merge_layer(
                merged.get(str(key), _CONFIG_MISSING), value)
        return merged
    return _config_clone(layer)


def _config_set(data: Dict[str, Any], path: Sequence[str], value: Any) -> None:
    if not path:
        return
    current = data
    for part in path[:-1]:
        child = current.get(part)
        if not isinstance(child, dict):
            child = {}
            current[part] = child
        current = child
    current[path[-1]] = value


def _config_mirror_ai_editor_alias(
        data: Dict[str, Any],
        path: Sequence[str],
        value: Any) -> None:
    if not path:
        return
    mirror_path = None
    if (path[0] == "ai_editor" and len(path) > 1
            and path[1] in _CONFIG_AI_EDITOR_ALIASES):
        mirror_path = list(path[1:])
    elif path[0] in _CONFIG_AI_EDITOR_ALIASES:
        mirror_path = ["ai_editor", *path]
    if mirror_path and list(mirror_path) != list(path):
        _config_set(data, mirror_path, value)


def _config_override_identifier_from_scope(scope: Any) -> str:
    if isinstance(scope, dict):
        value = scope.get("languageId") or scope.get("language_id")
        return str(value or "").strip()
    value = getattr(scope, "languageId", None)
    if value is None:
        value = getattr(scope, "language_id", None)
    return str(value or "").strip()


def _config_language_override_key(override_identifier: str) -> str:
    text = str(override_identifier or "").strip()
    return f"[{text}]" if text else ""


def _config_language_override_store(
        data: Dict[str, Any],
        override_identifier: str) -> Dict[str, Any]:
    key = _config_language_override_key(override_identifier)
    if not key:
        return {}
    value = data.get(key)
    return value if isinstance(value, dict) else {}


def _config_language_override_lookup(
        data: Dict[str, Any],
        path: Sequence[str],
        override_identifier: str) -> Any:
    store = _config_language_override_store(data, override_identifier)
    if not store or not path:
        return _CONFIG_MISSING
    dotted = ".".join(path)
    if dotted in store:
        return store[dotted]
    return _config_lookup(store, path)


def _config_language_override_section(
        data: Dict[str, Any],
        section_path: Sequence[str],
        override_identifier: str) -> Any:
    store = _config_language_override_store(data, override_identifier)
    if not store:
        return _CONFIG_MISSING
    if not section_path:
        return store
    prefix = ".".join(section_path)
    result = _CONFIG_MISSING
    nested = _config_lookup(store, section_path)
    result = _config_merge_layer(result, nested)
    for key, value in store.items():
        if key == prefix:
            result = _config_merge_layer(result, value)
            continue
        if not key.startswith(prefix + "."):
            continue
        suffix = _config_path(key[len(prefix) + 1:])
        if not suffix:
            continue
        if result is _CONFIG_MISSING or not isinstance(result, dict):
            result = {}
        _config_set(result, suffix, value)
    return result


class WorkspaceConfiguration:
    def __init__(
            self,
            section: str = "",
            data: Dict = None,
            override_identifier: str = "") -> None:
        self._section = str(section or "")
        self._data = data or {}
        self._override_identifier = str(override_identifier or "").strip()

    def _full_path(self, key: str = "") -> List[str]:
        path = _config_path(self._section)
        path.extend(_config_path(key))
        return path

    def _section_data(self) -> Any:
        section_path = _config_path(self._section)
        configured = (self._data if not section_path
                      else _config_lookup(self._data, section_path))
        override = _config_language_override_section(
            self._data, section_path, self._override_identifier)
        merged = _config_merge_layer(configured, override)
        return {} if merged is _CONFIG_MISSING else merged

    def _effective_lookup(self, path: Sequence[str]) -> Any:
        override = _config_language_override_lookup(
            self._data, path, self._override_identifier)
        if override is not _CONFIG_MISSING:
            return override
        return _config_lookup(self._data, path)

    def _global_lookup(self, path: Sequence[str]) -> Any:
        return _config_lookup(self._data, path)

    def _language_lookup(self, path: Sequence[str]) -> Any:
        return _config_language_override_lookup(
            self._data, path, self._override_identifier)

    def get(self, key: str = "", default: Any = None) -> Any:
        if not key:
            value = self._section_data()
            return default if value is _CONFIG_MISSING else value
        value = self._effective_lookup(self._full_path(key))
        return default if value is _CONFIG_MISSING else value

    def has(self, key: str) -> bool:
        return self._effective_lookup(self._full_path(key)) is not _CONFIG_MISSING

    def update(
            self,
            key: str,
            value: Any,
            global_scope: bool = True,
            override_in_language: bool = False) -> None:
        path = self._full_path(key)
        if override_in_language and self._override_identifier:
            language_key = _config_language_override_key(self._override_identifier)
            store = self._data.setdefault(language_key, {})
            if isinstance(store, dict):
                store[".".join(path)] = value
            return
        _config_set(self._data, path, value)
        _config_mirror_ai_editor_alias(self._data, path, value)

    def inspect(self, key: str) -> Optional[Dict[str, Any]]:
        path = self._full_path(key)
        value = self._global_lookup(path)
        language_value = self._language_lookup(path)
        if value is _CONFIG_MISSING and language_value is _CONFIG_MISSING:
            return None
        full_key = ".".join(path)
        return {
            "key": full_key,
            "defaultValue": None,
            "globalValue": None if value is _CONFIG_MISSING else value,
            "workspaceValue": None if value is _CONFIG_MISSING else value,
            "globalLanguageValue": (
                None if language_value is _CONFIG_MISSING else language_value),
            "workspaceLanguageValue": (
                None if language_value is _CONFIG_MISSING else language_value),
            "workspaceFolderValue": None,
        }


# ---------------------------------------------------------------------------
# Authentication types
# ---------------------------------------------------------------------------

@dataclass
class AuthenticationSession:
    id: str = ""
    access_token: str = ""
    account: Dict = field(default_factory=lambda: {"id": "", "label": ""})
    scopes: List[str] = field(default_factory=list)

    @property
    def accessToken(self) -> str:
        return self.access_token


class AuthenticationProviderBase:
    def __init__(self) -> None:
        self._sessions_changed = EventEmitter()

    @property
    def on_did_change_sessions(self):
        return self._sessions_changed.event

    def get_sessions(self, scopes=None, options=None):
        return []

    def create_session(self, scopes=None, options=None):
        return None

    def remove_session(self, session_id: str):
        return False


# ---------------------------------------------------------------------------
# Chat Variable types
# ---------------------------------------------------------------------------

@dataclass
class ChatVariableValue:
    level: int = 2  # 1=Short, 2=Medium, 3=Full
    value: str = ""
    description: str = ""


@dataclass
class Location:
    uri: Uri
    range: Range = field(default_factory=Range)


class ThemeColor:
    def __init__(self, id: Any) -> None:
        self.id = "" if id is None else str(id)


class FileDecoration:
    def __init__(self, badge: Any = None, tooltip: Any = None,
                 color: Any = None) -> None:
        if badge is not None:
            self.badge = str(badge)
        if tooltip is not None:
            self.tooltip = str(tooltip)
        if color is not None:
            self.color = color
        self.propagate = False


@dataclass
class DocumentHighlight:
    range: Range = field(default_factory=Range)
    kind: int = 0


class EvaluatableExpression:
    def __init__(self, range: Any, expression: Any = None) -> None:
        self.range = range
        if expression is not None:
            self.expression = str(expression)


class InlineValueText:
    def __init__(self, range: Any, text: Any) -> None:
        self.range = _coerce_range(range)
        self.text = "" if text is None else str(text)


class InlineValueVariableLookup:
    def __init__(
            self, range: Any, variableName: Any = None,
            caseSensitiveLookup: Any = True) -> None:
        self.range = _coerce_range(range)
        if variableName is not None:
            self.variableName = str(variableName)
        self.caseSensitiveLookup = bool(caseSensitiveLookup)


class InlineValueEvaluatableExpression:
    def __init__(self, range: Any, expression: Any = None) -> None:
        self.range = _coerce_range(range)
        if expression is not None:
            self.expression = str(expression)


class InlineValueContext:
    def __init__(self, frameId: Any = 0, stoppedLocation: Any = None) -> None:
        try:
            self.frameId = int(frameId)
        except Exception:
            self.frameId = 0
        self.stoppedLocation = _coerce_range(stoppedLocation)


class SymbolInformation:
    def __init__(
            self,
            name: Any,
            kind: Any,
            container_or_range: Any = "",
            location_or_uri: Any = None,
            container_name: Any = None) -> None:
        self.name = "" if name is None else str(name)
        try:
            self.kind = int(kind)
        except Exception:
            self.kind = kind
        self.tags = None
        if isinstance(location_or_uri, Location):
            self.containerName = (
                "" if container_or_range is None
                else str(container_or_range))
            self.location = location_or_uri
            return
        if isinstance(container_or_range, (Range, dict)):
            uri = _coerce_uri(location_or_uri) or Uri.file("")
            self.containerName = (
                "" if container_name is None else str(container_name))
            self.location = Location(uri, _coerce_range(container_or_range))
            return
        self.containerName = (
            "" if container_or_range is None else str(container_or_range))
        self.location = location_or_uri if isinstance(
            location_or_uri, Location) else Location(Uri.file(""))


class DocumentLink:
    def __init__(self, range: Any, target: Any = None) -> None:
        self.range = range
        self.target = target
        self.tooltip = None


def _plain_json_value(value: Any, depth: int = 0) -> Any:
    if depth > 8:
        return str(value)
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, bytes):
        return list(value)
    if isinstance(value, bytearray):
        return list(bytes(value))
    if isinstance(value, Uri):
        return str(value)
    if isinstance(value, Position):
        return {"line": int(value.line), "character": int(value.character)}
    if isinstance(value, Range):
        return {
            "start": _plain_json_value(value.start, depth + 1),
            "end": _plain_json_value(value.end, depth + 1),
        }
    if isinstance(value, dict):
        return {
            str(key): _plain_json_value(item, depth + 1)
            for key, item in value.items()
            if not callable(item)
        }
    if isinstance(value, (list, tuple, set)):
        return [_plain_json_value(item, depth + 1) for item in value]
    if hasattr(value, "__dict__"):
        return {
            str(key): _plain_json_value(item, depth + 1)
            for key, item in vars(value).items()
            if not key.startswith("_") and not callable(item)
        }
    return str(value)


class DataTransferFile:
    """Best-effort file payload used by editor drop/paste providers."""

    def __init__(self, name: Any = "", uri: Any = None,
                 data: Any = b"") -> None:
        self.name = "" if name is None else str(name)
        self.uri = _coerce_uri(uri) if uri is not None else None
        self._data = data

    def data(self) -> bytes:
        value = self._data
        if isinstance(value, bytes):
            return value
        if isinstance(value, bytearray):
            return bytes(value)
        if isinstance(value, list):
            try:
                return bytes(int(item) & 0xFF for item in value)
            except Exception:
                return bytes(str(value), "utf-8")
        return bytes(str(value or ""), "utf-8")


class DataTransferItem:
    def __init__(self, value: Any) -> None:
        self.value = value

    def asString(self) -> str:
        value = self.value
        if isinstance(value, bytes):
            return value.decode("utf-8", errors="replace")
        if isinstance(value, bytearray):
            return bytes(value).decode("utf-8", errors="replace")
        if isinstance(value, (dict, list, tuple, set)):
            try:
                return json.dumps(_plain_json_value(value), ensure_ascii=False)
            except Exception:
                return str(value)
        return "" if value is None else str(value)

    def asFile(self) -> Optional[DataTransferFile]:
        value = self.value
        if isinstance(value, DataTransferFile):
            return value
        if isinstance(value, dict) and (
                value.get("name") is not None
                or value.get("uri") is not None
                or value.get("data") is not None):
            return DataTransferFile(
                value.get("name") or value.get("fileName") or "",
                value.get("uri") or value.get("path"),
                value.get("data") or value.get("contents") or b"",
            )
        return None


class DataTransfer:
    def __init__(self, entries: Any = None) -> None:
        self._items: Dict[str, DataTransferItem] = {}
        if isinstance(entries, DataTransfer):
            for mime, item in entries:
                self.set(mime, item)
            return
        if isinstance(entries, dict):
            for mime, item in entries.items():
                self.set(str(mime), item)
            return
        if entries:
            try:
                for mime, item in entries:
                    self.set(str(mime), item)
            except Exception:
                pass

    @staticmethod
    def _key(mime_type: Any) -> str:
        return str(mime_type or "").lower()

    @staticmethod
    def _item(value: Any) -> DataTransferItem:
        return value if isinstance(value, DataTransferItem) else DataTransferItem(value)

    def get(self, mimeType: Any) -> Optional[DataTransferItem]:
        return self._items.get(self._key(mimeType))

    def set(self, mimeType: Any, value: Any) -> None:
        key = self._key(mimeType)
        if key:
            self._items[key] = self._item(value)

    def delete(self, mimeType: Any) -> None:
        self._items.pop(self._key(mimeType), None)

    def has(self, mimeType: Any) -> bool:
        return self._key(mimeType) in self._items

    def forEach(self, callback: Callable, thisArg: Any = None) -> None:
        for mime, item in list(self._items.items()):
            if thisArg is None:
                callback(item, mime, self)
            else:
                callback(thisArg, item, mime, self)

    def __iter__(self):
        return iter(self._items.items())

    def items(self):
        return self._items.items()

    def to_payload(self) -> Dict[str, Any]:
        return {
            mime: _plain_json_value(item.value)
            for mime, item in self._items.items()
        }


class DocumentDropOrPasteEditKind:
    Empty: "DocumentDropOrPasteEditKind"
    Text: "DocumentDropOrPasteEditKind"
    TextUpdateImports: "DocumentDropOrPasteEditKind"

    def __init__(self, value: Any = "") -> None:
        self.value = "" if value is None else str(value)

    def append(self, *parts: Any) -> "DocumentDropOrPasteEditKind":
        suffix = ".".join(str(part).strip(".") for part in parts if part)
        return DocumentDropOrPasteEditKind(
            ".".join(part for part in (self.value, suffix) if part))

    def intersects(self, other: Any) -> bool:
        other_kind = _coerce_drop_or_paste_kind(other)
        return self.contains(other_kind) or other_kind.contains(self)

    def contains(self, other: Any) -> bool:
        other_value = _coerce_drop_or_paste_kind(other).value
        if not self.value:
            return not other_value
        return other_value == self.value or other_value.startswith(
            self.value + ".")

    def __str__(self) -> str:
        return self.value

    def __repr__(self) -> str:
        return f"DocumentDropOrPasteEditKind({self.value!r})"


DocumentDropOrPasteEditKind.Empty = DocumentDropOrPasteEditKind("")
DocumentDropOrPasteEditKind.Text = DocumentDropOrPasteEditKind("text")
DocumentDropOrPasteEditKind.TextUpdateImports = DocumentDropOrPasteEditKind(
    "text.updateImports")


class DocumentDropEdit:
    def __init__(self, insert_text: Any, title: Any = None,
                 kind: Any = None) -> None:
        self.insertText = insert_text
        self.title = None if title is None else str(title)
        self.kind = _coerce_drop_or_paste_kind(kind) if kind is not None else None
        self.yieldTo = None
        self.additionalEdit = None


class DocumentPasteEdit:
    def __init__(self, insert_text: Any, title: Any,
                 kind: Any) -> None:
        self.insertText = insert_text
        self.title = "" if title is None else str(title)
        self.kind = _coerce_drop_or_paste_kind(kind)
        self.additionalEdit = None
        self.yieldTo = None


def _coerce_drop_or_paste_kind(value: Any) -> DocumentDropOrPasteEditKind:
    if isinstance(value, DocumentDropOrPasteEditKind):
        return value
    if isinstance(value, dict) and "value" in value:
        return DocumentDropOrPasteEditKind(value.get("value"))
    return DocumentDropOrPasteEditKind("" if value is None else str(value))


class Color:
    def __init__(
            self, red: Any, green: Any, blue: Any,
            alpha: Any = 1) -> None:
        self.red = self._component(red, 0.0)
        self.green = self._component(green, 0.0)
        self.blue = self._component(blue, 0.0)
        self.alpha = self._component(alpha, 1.0)

    @staticmethod
    def _component(value: Any, default: float) -> float:
        try:
            return max(0.0, min(1.0, float(value)))
        except Exception:
            return default


class ColorInformation:
    def __init__(self, range: Any, color: Any) -> None:
        self.range = range
        self.color = _coerce_color(color)


class ColorPresentation:
    def __init__(self, label: Any) -> None:
        self.label = "" if label is None else str(label)
        self.textEdit = None
        self.additionalTextEdits = None


class InlayHintLabelPart:
    def __init__(self, value: Any) -> None:
        self.value = "" if value is None else str(value)
        self.tooltip = None
        self.location = None
        self.command = None


class InlayHint:
    def __init__(self, position: Any, label: Any, kind: Any = None) -> None:
        self.position = position
        self.label = label
        self.kind = kind
        self.tooltip = None
        self.textEdits = None
        self.paddingLeft = None
        self.paddingRight = None


class InlineCompletionItem:
    def __init__(
            self, insert_text: Any, range: Any = None,
            command: Any = None) -> None:
        self.insertText = insert_text
        self.range = range
        self.command = command
        self.filterText = None


class InlineCompletionList:
    def __init__(self, items: Any = None) -> None:
        self.items = list(items or [])


class CodeLens:
    def __init__(self, range: Any, command: Any = None) -> None:
        self.range = range
        self.command = command

    @property
    def isResolved(self) -> bool:
        return self.command is not None


class FoldingRange:
    def __init__(self, start: Any, end: Any, kind: Any = None) -> None:
        try:
            self.start = max(0, int(start or 0))
        except Exception:
            self.start = 0
        try:
            self.end = max(0, int(end or 0))
        except Exception:
            self.end = 0
        self.kind = kind


class SelectionRange:
    def __init__(self, range: Any, parent: Any = None) -> None:
        self.range = range
        self.parent = parent


class CallHierarchyItem:
    def __init__(
            self, kind: Any, name: Any, detail: Any, uri: Any,
            range: Any, selection_range: Any) -> None:
        try:
            self.kind = int(kind)
        except Exception:
            self.kind = kind
        self.name = "" if name is None else str(name)
        self.detail = "" if detail is None else str(detail)
        self.uri = _coerce_uri(uri) or Uri.file("")
        self.range = range
        self.selectionRange = selection_range
        self.tags = None


class CallHierarchyIncomingCall:
    def __init__(self, item: Any, from_ranges: Any) -> None:
        self.from_ = item
        setattr(self, "from", item)
        self.fromRanges = list(from_ranges or [])

    @property
    def from_item(self) -> Any:
        return self.from_


class CallHierarchyOutgoingCall:
    def __init__(self, item: Any, from_ranges: Any) -> None:
        self.to = item
        self.fromRanges = list(from_ranges or [])


class TypeHierarchyItem:
    def __init__(
            self, kind: Any, name: Any, detail: Any, uri: Any,
            range: Any, selection_range: Any) -> None:
        try:
            self.kind = int(kind)
        except Exception:
            self.kind = kind
        self.name = "" if name is None else str(name)
        self.detail = "" if detail is None else str(detail)
        self.uri = _coerce_uri(uri) or Uri.file("")
        self.range = range
        self.selectionRange = selection_range
        self.tags = None


class SemanticTokensLegend:
    def __init__(
            self,
            token_types: Sequence[Any],
            token_modifiers: Optional[Sequence[Any]] = None) -> None:
        self.tokenTypes = [str(item) for item in (token_types or [])]
        self.tokenModifiers = [
            str(item) for item in (token_modifiers or [])]


class SemanticTokens:
    def __init__(self, data: Any, result_id: Any = None) -> None:
        self.data = _semantic_tokens_data(data)
        self.resultId = None if result_id is None else str(result_id)


class SemanticTokensEdit:
    def __init__(
            self, start: Any, delete_count: Any, data: Any = None) -> None:
        try:
            self.start = max(0, int(start or 0))
        except Exception:
            self.start = 0
        try:
            self.deleteCount = max(0, int(delete_count or 0))
        except Exception:
            self.deleteCount = 0
        self.data = None if data is None else _semantic_tokens_data(data)


class SemanticTokensEdits:
    def __init__(self, edits: Any, result_id: Any = None) -> None:
        self.edits = list(edits or [])
        self.resultId = None if result_id is None else str(result_id)


def _semantic_tokens_data(value: Any) -> List[int]:
    if value is None:
        return []
    if isinstance(value, (bytes, bytearray)):
        return [int(item) for item in value]
    try:
        return [max(0, int(item)) for item in value]
    except Exception:
        return []


def _semantic_legend_payload(value: Any) -> Optional[Dict[str, List[str]]]:
    if value is None:
        return None
    if isinstance(value, dict):
        token_types = value.get("tokenTypes") or value.get("token_types") or []
        token_modifiers = (
            value.get("tokenModifiers")
            or value.get("token_modifiers")
            or [])
    else:
        token_types = (
            getattr(value, "tokenTypes", None)
            or getattr(value, "token_types", None)
            or [])
        token_modifiers = (
            getattr(value, "tokenModifiers", None)
            or getattr(value, "token_modifiers", None)
            or [])
    return {
        "tokenTypes": [str(item) for item in token_types],
        "tokenModifiers": [str(item) for item in token_modifiers],
    }


class SemanticTokensBuilder:
    def __init__(self, legend: Any = None) -> None:
        payload = _semantic_legend_payload(legend) or {
            "tokenTypes": [],
            "tokenModifiers": [],
        }
        self._legend = payload
        self._tokens: List[tuple[int, int, int, int, int]] = []

    def push(self, *args: Any) -> None:
        if not args:
            return
        if len(args) >= 2 and not isinstance(args[0], (int, float)):
            rng = _coerce_range(args[0])
            line = int(getattr(rng.start, "line", 0))
            char = int(getattr(rng.start, "character", 0))
            if int(getattr(rng.end, "line", line)) != line:
                return
            length = max(0, int(getattr(rng.end, "character", char)) - char)
            token_type = args[1]
            token_modifiers = args[2] if len(args) > 2 else 0
        elif len(args) >= 4:
            line = int(args[0] or 0)
            char = int(args[1] or 0)
            length = int(args[2] or 0)
            token_type = args[3]
            token_modifiers = args[4] if len(args) > 4 else 0
        else:
            return
        if length <= 0:
            return
        self._tokens.append((
            max(0, line),
            max(0, char),
            max(0, length),
            self._token_type_index(token_type),
            self._token_modifier_bits(token_modifiers),
        ))

    def _token_type_index(self, token_type: Any) -> int:
        if isinstance(token_type, str):
            try:
                return self._legend["tokenTypes"].index(token_type)
            except ValueError:
                self._legend["tokenTypes"].append(token_type)
                return len(self._legend["tokenTypes"]) - 1
        try:
            return max(0, int(token_type or 0))
        except Exception:
            return 0

    def _token_modifier_bits(self, token_modifiers: Any) -> int:
        if token_modifiers is None:
            return 0
        if isinstance(token_modifiers, str):
            token_modifiers = [token_modifiers]
        if isinstance(token_modifiers, (list, tuple, set)):
            bits = 0
            for modifier in token_modifiers:
                name = str(modifier)
                try:
                    index = self._legend["tokenModifiers"].index(name)
                except ValueError:
                    self._legend["tokenModifiers"].append(name)
                    index = len(self._legend["tokenModifiers"]) - 1
                bits |= 1 << index
            return bits
        try:
            return max(0, int(token_modifiers or 0))
        except Exception:
            return 0

    def build(self, result_id: Any = None) -> SemanticTokens:
        data: List[int] = []
        previous_line = 0
        previous_char = 0
        for line, char, length, token_type, token_modifiers in sorted(
                self._tokens, key=lambda item: (item[0], item[1])):
            delta_line = line - previous_line
            delta_char = char if delta_line else char - previous_char
            data.extend([
                max(0, delta_line),
                max(0, delta_char),
                max(0, length),
                max(0, token_type),
                max(0, token_modifiers),
            ])
            previous_line = line
            previous_char = char
        return SemanticTokens(data, result_id)


@dataclass
class Diagnostic:
    range: Range
    message: str = ""
    severity: int = 0
    source: str = ""
    code: Any = None
    tags: List[int] = field(default_factory=list)
    related_information: List[Any] = field(default_factory=list)


class CompletionItem:
    def __init__(self, label: Any = "", kind: Any = None) -> None:
        self.label = label
        self.kind = kind
        self.tags = []
        self.detail = None
        self.documentation = None
        self.sortText = None
        self.filterText = None
        self.preselect = None
        self.insertText = None
        self.insertTextRules = None
        self.keepWhitespace = None
        self.range = None
        self.textEdit = None
        self.commitCharacters = None
        self.additionalTextEdits = []
        self.command = None


class CompletionList:
    def __init__(self, items: Any = None, is_incomplete: bool = False) -> None:
        self.items = list(items or [])
        self.isIncomplete = bool(is_incomplete)


class Hover:
    def __init__(self, contents: Any, range: Any = None) -> None:
        self.contents = contents if isinstance(contents, list) else [contents]
        self.range = range


class ParameterInformation:
    def __init__(self, label: Any = "", documentation: Any = None) -> None:
        self.label = label
        self.documentation = documentation


class SignatureInformation:
    def __init__(self, label: str = "", documentation: Any = None) -> None:
        self.label = str(label or "")
        self.documentation = documentation
        self.parameters: List[Any] = []


class SignatureHelp:
    def __init__(
            self, signatures: Any = None, active_signature: int = 0,
            active_parameter: int = 0) -> None:
        self.signatures = list(signatures or [])
        self.activeSignature = int(active_signature or 0)
        self.activeParameter = int(active_parameter or 0)


class CodeAction:
    def __init__(self, title: str = "", kind: Any = None) -> None:
        self.title = title
        self.kind = kind
        self.diagnostics = []
        self.edit = None
        self.command = None
        self.isPreferred = False
        self.disabled = None


class TextEdit:
    @staticmethod
    def replace(range: Any, new_text: str) -> Dict[str, Any]:
        return {"kind": "replace", "range": range, "newText": str(new_text or "")}

    @staticmethod
    def insert(position: Any, new_text: str) -> Dict[str, Any]:
        return {"kind": "insert", "position": position, "newText": str(new_text or "")}

    @staticmethod
    def delete(range: Any) -> Dict[str, Any]:
        return {"kind": "delete", "range": range, "newText": ""}


class WorkspaceEdit:
    """Collect workspace edits without pretending SAO can safely apply them."""

    def __init__(self) -> None:
        self._edits: List[Dict[str, Any]] = []

    def replace(self, uri: Any, range: Range, new_text: str) -> None:
        self._edits.append({
            "kind": "replace", "uri": uri,
            "range": range, "newText": new_text,
        })

    def insert(self, uri: Any, position: Position, new_text: str) -> None:
        self._edits.append({
            "kind": "insert", "uri": uri,
            "position": position, "newText": new_text,
        })

    def delete(self, uri: Any, range: Range) -> None:
        self._edits.append({"kind": "delete", "uri": uri, "range": range})

    def createFile(self, uri: Any, options: Dict = None) -> None:
        self._edits.append({"kind": "createFile", "uri": uri,
                            "options": options or {}})

    def deleteFile(self, uri: Any, options: Dict = None) -> None:
        self._edits.append({"kind": "deleteFile", "uri": uri,
                            "options": options or {}})

    def renameFile(self, old_uri: Any, new_uri: Any, options: Dict = None) -> None:
        self._edits.append({
            "kind": "renameFile", "oldUri": old_uri,
            "newUri": new_uri, "options": options or {},
        })

    def entries(self) -> List[Dict[str, Any]]:
        return list(self._edits)


# ---------------------------------------------------------------------------
# UIBridge protocol — connects vscode API calls to the HTML webview
# ---------------------------------------------------------------------------

@runtime_checkable
class UIBridge(Protocol):
    """Protocol for pushing vscode API state changes to the HTML UI.

    Implementations live outside this module (e.g. AIEditorAPI) and own the
    actual ``_emit`` / ``_eval_js`` calls.  Every method MUST be safe to call
    from any thread.  All methods are optional — a partial implementation is
    fine; callers guard with ``hasattr`` / ``getattr``.
    """

    # -- Output channel --
    def show_output(self, channel_name: str, content: str) -> None: ...
    def clear_output(self, channel_name: str) -> None: ...
    def dispose_output(self, channel_name: str) -> None: ...

    # -- Terminal --
    def show_terminal(self, name: str) -> None: ...
    def hide_terminal(self, name: str) -> None: ...
    def run_terminal_command(self, name: str, text: str) -> Optional[str]: ...
    def write_terminal_data(self, name: str, text: str) -> None: ...

    # -- Messages / toasts --
    def show_message(self, level: str, message: str) -> None: ...
    def confirm_tool_invocation(self, tool_name: str,
                                confirmation: Dict[str, Any],
                                input_data: Any) -> bool: ...

    # -- Progress --
    def show_progress(self, message: Optional[str],
                      increment: Optional[float]) -> None: ...

    # -- Status bar --
    def show_status_bar_item(self, item_id: str, text: str,
                             tooltip: str, command: str,
                             alignment: int = 2, priority: int = 0,
                             color: str = "", backgroundColor: str = "") -> None: ...
    def hide_status_bar_item(self, item_id: str) -> None: ...
    def dispose_status_bar_item(self, item_id: str) -> None: ...

    # -- Pickers / dialogs --
    def show_quick_pick(self, items: List[Any],
                        options: Dict[str, Any]) -> Any: ...
    def show_input_box(self, options: Dict[str, Any]) -> Optional[str]: ...

    # -- Webview panels --
    def render_webview_panel(
            self, view_id: str, html: str,
            local_resource_roots: Any = None,
            state: Any = None,
            title: str = "") -> None: ...
    def get_webview_state(self, view_id: str) -> Any: ...
    def update_webview_panel_title(
            self, view_id: str, title: str, view_type: str = "") -> None: ...
    def post_webview_message(self, view_id: str, message: Any) -> None: ...
    def receive_webview_message(self, view_id: str, message: Any) -> None: ...
    def dispose_webview_panel(self, view_id: str) -> None: ...


# ---------------------------------------------------------------------------
# VscodeNamespace — the main vscode.* API object
# ---------------------------------------------------------------------------

class VscodeNamespace:
    """Builds a per-extension vscode.* API namespace.

    Usage::

        ns = VscodeNamespace(ext_host, engine)
        vscode = ns.build(extension_description)
        # vscode.commands.registerCommand(...)
        # vscode.lm.selectChatModels(...)
    """

    def __init__(self, host: ExtensionHost, engine: Any = None,
                 settings_getter: Callable = None) -> None:
        self._host = host
        self._engine = engine
        self._settings_getter = settings_getter
        self._ui_bridge: Optional[UIBridge] = None
        self._clipboard_text = ""
        self._active_text_editor: Optional[_TextEditor] = None
        self._visible_text_editors: List[_TextEditor] = []
        self._text_documents: List[_TextDocument] = []
        self._terminals: List[_Terminal] = []
        self._active_terminal: Optional[_Terminal] = None
        self._webview_panels: Dict[str, List[Any]] = {}
        self._webview_tokens: Dict[str, str] = {}
        self._chat_participants: Dict[str, ChatParticipant] = {}
        self._lm_tools: Dict[str, Any] = {}
        self._mcp_definition_providers: Dict[str, Any] = {}
        self._auth_providers: Dict[str, Any] = {}
        self._auth_sessions: Dict[str, List[AuthenticationSession]] = {}
        self._auth_provider_listeners: Dict[str, Any] = {}
        self._variables: Dict[str, Callable] = {}
        self._lm_providers: Dict[str, Any] = {}
        self._language_providers: Dict[str, List[Any]] = {}
        self._language_configurations: Dict[str, List[Dict[str, Any]]] = {}
        self._diagnostic_collections: Dict[str, Any] = {}
        self._file_system_providers: Dict[str, Dict[str, Any]] = {}
        self._chat_context_providers: Dict[str, Dict[str, Any]] = {
            "workspace": {},
            "explicit": {},
            "resource": {},
        }
        self._diagnostics_change_emitter = EventEmitter()
        self._window_active_text_editor_emitter = EventEmitter()
        self._window_visible_text_editors_emitter = EventEmitter()
        self._window_active_terminal_emitter = EventEmitter()
        self._window_open_terminal_emitter = EventEmitter()
        self._window_close_terminal_emitter = EventEmitter()
        self._workspace_open_text_document_emitter = EventEmitter()
        self._workspace_close_text_document_emitter = EventEmitter()
        self._workspace_change_text_document_emitter = EventEmitter()
        self._workspace_save_text_document_emitter = EventEmitter()
        self._workspace_create_files_emitter = EventEmitter()
        self._workspace_delete_files_emitter = EventEmitter()
        self._workspace_rename_files_emitter = EventEmitter()
        self._workspace_folders_change_emitter = EventEmitter()
        self._auth_sessions_emitter = EventEmitter()
        self._tree_views: Dict[str, Any] = {}
        self._tree_data_providers: Dict[str, Any] = {}
        self._tree_view_change_callback: Optional[
            Callable[[Dict[str, Any]], None]
        ] = None
        self._webview_view_providers: Dict[str, Any] = {}
        self._webview_views: Dict[str, Any] = {}
        self._webview_view_change_callback: Optional[
            Callable[[Dict[str, Any]], None]
        ] = None
        self._file_decoration_providers: List[Any] = []
        self._file_decoration_request_callback: Optional[
            Callable[[Dict[str, Any]], Any]
        ] = None
        self._task_providers: Dict[str, Any] = {}
        self._debug_providers: Dict[str, Any] = {}
        self._task_executions: List[_TaskExecution] = []
        self._debug_sessions: List[_DebugSession] = []
        self._workspace_watchers: List[_FileSystemWatcher] = []
        self._ignored_file_providers: List[Any] = []
        self._tasks_start_emitter = EventEmitter()
        self._tasks_end_emitter = EventEmitter()
        self._debug_start_emitter = EventEmitter()
        self._debug_terminate_emitter = EventEmitter()
        self._debug_breakpoints_emitter = EventEmitter()
        self._config_change_emitter = EventEmitter()
        self._tools_change_emitter = EventEmitter()
        self._models_change_emitter = EventEmitter()
        self._extensions_change_emitter = EventEmitter()
        self._window_tab_groups_emitter = EventEmitter()
        self._window_tabs_emitter = EventEmitter()
        self._window_api: Optional[Dict[str, Any]] = None
        self._workspace_api: Optional[Dict[str, Any]] = None
        self._auth_api: Optional[Dict[str, Any]] = None
        self._tasks_api: Optional[Dict[str, Any]] = None
        self._debug_api: Optional[Dict[str, Any]] = None
        self._extensions_api: Optional[Dict[str, Any]] = None
        self._language_command_disposables: List[Callable] = []
        self._language_provider_request_callback: Optional[Callable[[Dict[str, Any]], Any]] = None
        self._external_language_document_versions: Dict[str, Any] = {}
        try:
            host.on_did_change(self._on_host_extensions_changed)
        except Exception:
            pass
        self._register_language_execute_commands()

    def _on_host_extensions_changed(self, event: Any = None) -> None:
        self._sync_extensions_state()
        self._sync_tasks_state()
        self._sync_debug_state()
        self._extensions_change_emitter.fire(event or {})

    def _register_language_execute_commands(self) -> None:
        commands = {
            "vscode.executeCompletionItemProvider": self._execute_completion_item_provider,
            "vscode.executeHoverProvider": self._execute_hover_provider,
            "vscode.executeSignatureHelpProvider": self._execute_signature_help_provider,
            "vscode.executeDefinitionProvider": self._execute_definition_provider,
            "_executeTypeDefinitionProvider": self._execute_type_definition_provider,
            "vscode.executeTypeDefinitionProvider": self._execute_type_definition_provider,
            "_executeDeclarationProvider": self._execute_declaration_provider,
            "vscode.executeDeclarationProvider": self._execute_declaration_provider,
            "_executeImplementationProvider": self._execute_implementation_provider,
            "vscode.executeImplementationProvider": self._execute_implementation_provider,
            "vscode.executeReferenceProvider": self._execute_reference_provider,
            "_executeDocumentHighlightProvider": self._execute_document_highlight_provider,
            "vscode.executeDocumentHighlightProvider": self._execute_document_highlight_provider,
            "_executeEvaluatableExpressionProvider": self._execute_evaluatable_expression_provider,
            "_executeInlineValueProvider": self._execute_inline_value_provider,
            "_executeDocumentRenameProvider": self._execute_rename_provider,
            "vscode.executeDocumentRenameProvider": self._execute_rename_provider,
            "_executePrepareRename": self._execute_prepare_rename_provider,
            "vscode.executePrepareRenameProvider": self._execute_prepare_rename_provider,
            "_executeLinkProvider": self._execute_link_provider,
            "vscode.executeLinkProvider": self._execute_link_provider,
            "_executeInlayHintProvider": self._execute_inlay_hint_provider,
            "vscode.executeInlayHintProvider": self._execute_inlay_hint_provider,
            "_executeInlineCompletionProvider": self._execute_inline_completion_provider,
            "_executeCodeLensProvider": self._execute_code_lens_provider,
            "vscode.executeCodeLensProvider": self._execute_code_lens_provider,
            "_executeFoldingRangeProvider": self._execute_folding_range_provider,
            "vscode.executeFoldingRangeProvider": self._execute_folding_range_provider,
            "_executeSelectionRangeProvider": self._execute_selection_range_provider,
            "vscode.executeSelectionRangeProvider": self._execute_selection_range_provider,
            "_executeLinkedEditingProvider": self._execute_linked_editing_provider,
            "_executeDocumentColorProvider": self._execute_document_color_provider,
            "vscode.executeDocumentColorProvider": self._execute_document_color_provider,
            "_executeColorPresentationProvider": self._execute_color_presentation_provider,
            "vscode.executeColorPresentationProvider": self._execute_color_presentation_provider,
            "_executePrepareCallHierarchy": self._execute_prepare_call_hierarchy_provider,
            "vscode.prepareCallHierarchy": self._execute_prepare_call_hierarchy_provider,
            "_executeProvideIncomingCalls": self._execute_call_hierarchy_incoming_provider,
            "vscode.provideIncomingCalls": self._execute_call_hierarchy_incoming_provider,
            "_executeProvideOutgoingCalls": self._execute_call_hierarchy_outgoing_provider,
            "vscode.provideOutgoingCalls": self._execute_call_hierarchy_outgoing_provider,
            "_executePrepareTypeHierarchy": self._execute_prepare_type_hierarchy_provider,
            "vscode.prepareTypeHierarchy": self._execute_prepare_type_hierarchy_provider,
            "_executeProvideSupertypes": self._execute_type_hierarchy_supertypes_provider,
            "vscode.provideSupertypes": self._execute_type_hierarchy_supertypes_provider,
            "_executeProvideSubtypes": self._execute_type_hierarchy_subtypes_provider,
            "vscode.provideSubtypes": self._execute_type_hierarchy_subtypes_provider,
            "_executeWorkspaceSymbolProvider": self._execute_workspace_symbol_provider,
            "vscode.executeWorkspaceSymbolProvider": self._execute_workspace_symbol_provider,
            "_resolveWorkspaceSymbolProvider": self._execute_resolve_workspace_symbol_provider,
            "_provideDocumentSemanticTokensLegend": self._execute_document_semantic_tokens_legend,
            "vscode.provideDocumentSemanticTokensLegend": self._execute_document_semantic_tokens_legend,
            "_provideDocumentSemanticTokens": self._execute_document_semantic_tokens_provider,
            "vscode.provideDocumentSemanticTokens": self._execute_document_semantic_tokens_provider,
            "_provideDocumentRangeSemanticTokensLegend": self._execute_document_range_semantic_tokens_legend,
            "vscode.provideDocumentRangeSemanticTokensLegend": self._execute_document_range_semantic_tokens_legend,
            "_provideDocumentRangeSemanticTokens": self._execute_document_range_semantic_tokens_provider,
            "vscode.provideDocumentRangeSemanticTokens": self._execute_document_range_semantic_tokens_provider,
            "vscode.executeDocumentSymbolProvider": self._execute_document_symbol_provider,
            "vscode.executeCodeActionProvider": self._execute_code_action_provider,
            "vscode.executeFormatDocumentProvider": self._execute_format_document_provider,
            "_executeFormattingProviderList": self._execute_formatting_provider_list,
            "_executeFormatRangeProvider": self._execute_format_range_provider,
            "vscode.executeFormatRangeProvider": self._execute_format_range_provider,
            "_executeFormatOnTypeProvider": self._execute_format_on_type_provider,
            "vscode.executeFormatOnTypeProvider": self._execute_format_on_type_provider,
            "_prepareDocumentPasteProvider": self._execute_prepare_document_paste_provider,
            "_executeDocumentPasteEditProvider": self._execute_document_paste_edit_provider,
            "_executeDocumentDropEditProvider": self._execute_document_drop_edit_provider,
        }
        for command_id, handler in commands.items():
            try:
                if self._host.commands.has(command_id):
                    continue
                self._language_command_disposables.append(
                    self._host.commands.register(command_id, handler))
            except Exception:
                pass

    def _resolve_language_document(self, document_or_uri: Any) -> "_TextDocument":
        if isinstance(document_or_uri, _TextDocument):
            return document_or_uri
        uri = _coerce_uri(document_or_uri)
        if uri is None:
            raise ValueError("language provider execution requires a document or URI")
        document = self._get_or_open_document(uri)
        if document is None:
            document = self._open_text_document(uri)
        return document

    def _matching_language_providers(
            self, kind: str, document: Any) -> List[Dict[str, Any]]:
        matches: List[Any] = []
        for entry in list(self._language_providers.get(kind, [])):
            score = self._language_match(entry.get("selector"), document)
            if score > 0:
                matches.append((score, entry))
        matches.sort(key=lambda item: item[0], reverse=True)
        return [entry for _score, entry in matches]

    @staticmethod
    def _language_provider_metadata_value(metadata: Any,
                                          *keys: str) -> Any:
        if not isinstance(metadata, dict):
            return None
        for key in keys:
            if metadata.get(key) not in (None, ""):
                return metadata.get(key)
        return None

    @staticmethod
    def _language_provider_attr(provider: Any, *names: str) -> Any:
        for name in names:
            if isinstance(provider, dict):
                value = provider.get(name)
            else:
                value = getattr(provider, name, None)
            if value not in (None, "") and not callable(value):
                return value
        return None

    @staticmethod
    def _language_provider_selector_label(selector: Any) -> str:
        if isinstance(selector, str) and selector.strip():
            return selector.strip()
        if isinstance(selector, dict):
            language = selector.get("language")
            if language:
                return str(language).strip()
        return "provider"

    @classmethod
    def _language_provider_id(cls, kind: str, selector: Any,
                              provider: Any, metadata: Any) -> str:
        value = cls._language_provider_metadata_value(
            metadata, "providerId", "extensionId", "id")
        if value in (None, ""):
            value = cls._language_provider_attr(
                provider, "providerId", "provider_id",
                "extensionId", "extension_id", "id")
        if value in (None, ""):
            provider_type = getattr(provider, "__class__", type(provider))
            module = getattr(provider_type, "__module__", "") or ""
            name = getattr(provider_type, "__name__", "") or kind
            value = f"{module}.{name}" if module else name
        text = str(value or "").strip()
        if text:
            return text
        return f"{cls._language_provider_selector_label(selector)}:{kind}"

    @classmethod
    def _language_provider_display_name(cls, kind: str, provider: Any,
                                        metadata: Any,
                                        provider_id: str) -> str:
        value = cls._language_provider_metadata_value(
            metadata, "displayName", "name", "label")
        if value in (None, ""):
            value = cls._language_provider_attr(
                provider, "displayName", "display_name", "name", "label")
        if value in (None, ""):
            value = provider_id
        return str(value or provider_id or kind)

    @classmethod
    def _language_provider_entry_matches_id(
            cls, entry: Dict[str, Any], provider_id: Any) -> bool:
        expected = str(provider_id or "").strip().lower()
        if not expected:
            return True
        values = [
            entry.get("providerId"),
            entry.get("id"),
            entry.get("extensionId"),
            entry.get("displayName"),
        ]
        metadata = entry.get("metadata")
        values.extend([
            cls._language_provider_metadata_value(
                metadata, "providerId", "extensionId", "id"),
            cls._language_provider_metadata_value(
                metadata, "displayName", "name", "label"),
        ])
        provider = entry.get("provider")
        values.extend([
            cls._language_provider_attr(
                provider, "providerId", "provider_id",
                "extensionId", "extension_id", "id"),
            cls._language_provider_attr(
                provider, "displayName", "display_name", "name", "label"),
        ])
        return any(str(value or "").strip().lower() == expected
                   for value in values)

    @classmethod
    def _formatting_provider_payload(
            cls, entry: Dict[str, Any], capability: str) -> Dict[str, Any]:
        provider_id = str(entry.get("providerId") or entry.get("id") or "")
        if not provider_id:
            provider_id = cls._language_provider_id(
                str(entry.get("kind") or capability),
                entry.get("selector"),
                entry.get("provider"),
                entry.get("metadata"))
        display_name = str(
            entry.get("displayName")
            or cls._language_provider_display_name(
                str(entry.get("kind") or capability),
                entry.get("provider"),
                entry.get("metadata"),
                provider_id))
        return {
            "id": provider_id,
            "providerId": provider_id,
            "extensionId": str(entry.get("extensionId") or provider_id),
            "displayName": display_name,
            "kind": entry.get("kind") or capability,
            "capability": capability,
        }

    @staticmethod
    def _merge_formatting_provider(
            providers: List[Dict[str, Any]],
            index: Dict[str, Dict[str, Any]],
            item: Dict[str, Any]) -> None:
        provider_id = str(
            item.get("providerId") or item.get("id")
            or item.get("extensionId") or "").strip()
        if not provider_id:
            return
        capability = str(
            item.get("capability") or item.get("kind")
            or "formatting").strip()
        raw_capabilities = item.get("capabilities")
        if isinstance(raw_capabilities, list):
            capabilities = [
                str(value).strip()
                for value in raw_capabilities
                if str(value or "").strip()
            ]
        else:
            capabilities = []
        if capability and capability not in capabilities:
            capabilities.append(capability)
        existing = index.get(provider_id.lower())
        if existing is None:
            payload = dict(item)
            payload["id"] = provider_id
            payload["providerId"] = provider_id
            payload.setdefault("extensionId", provider_id)
            payload.setdefault("displayName", provider_id)
            payload["capabilities"] = capabilities or ["formatting"]
            providers.append(payload)
            index[provider_id.lower()] = payload
            return
        caps = existing.setdefault("capabilities", [])
        for value in capabilities:
            if value and value not in caps:
                caps.append(value)

    def _formatting_provider_list(self, document: Any) -> List[Dict[str, Any]]:
        providers: List[Dict[str, Any]] = []
        index: Dict[str, Dict[str, Any]] = {}
        for kind, capability in (
                ("formatting", "documentFormatting"),
                ("rangeFormatting", "rangeFormatting")):
            for entry in self._matching_language_providers(kind, document):
                self._merge_formatting_provider(
                    providers,
                    index,
                    self._formatting_provider_payload(entry, capability))
        external = self._request_external_language_provider(
            "formattingProviders", document)
        for item in self._provider_values(external):
            if isinstance(item, dict):
                self._merge_formatting_provider(providers, index, item)
        return providers

    @staticmethod
    def _position_payload(position: Any) -> Dict[str, int]:
        pos = _coerce_position(position)
        return {
            "line": int(getattr(pos, "line", 0)),
            "character": int(getattr(pos, "character", 0)),
        }

    @classmethod
    def _range_payload(cls, range_value: Any) -> Dict[str, Dict[str, int]]:
        rng = _coerce_range(range_value)
        return {
            "start": cls._position_payload(rng.start),
            "end": cls._position_payload(rng.end),
        }

    @classmethod
    def _language_value_payload(cls, value: Any, depth: int = 0) -> Any:
        if depth > 8:
            return str(value)
        if value is None or isinstance(value, (str, int, float, bool)):
            return value
        if isinstance(value, Uri):
            return str(value)
        if isinstance(value, Position):
            return cls._position_payload(value)
        if isinstance(value, Range):
            return cls._range_payload(value)
        if isinstance(value, dict):
            return {
                str(key): cls._language_value_payload(item, depth + 1)
                for key, item in value.items()
                if not callable(item)
            }
        if isinstance(value, (list, tuple, set)):
            return [cls._language_value_payload(item, depth + 1)
                    for item in value]
        attrs = (
            "name", "kind", "detail", "uri", "range", "text", "expression",
            "badge", "tooltip", "color", "propagate", "id",
            "variableName", "caseSensitiveLookup", "frameId",
            "stoppedLocation", "selectionRange",
            "tags", "from", "fromRanges", "to",
        )
        data: Dict[str, Any] = {}
        for attr in attrs:
            if hasattr(value, attr):
                try:
                    item = getattr(value, attr)
                except Exception:
                    continue
                if item is not None and not callable(item):
                    data[attr] = cls._language_value_payload(item, depth + 1)
        if data:
            return data
        if hasattr(value, "__dict__"):
            return {
                str(key): cls._language_value_payload(item, depth + 1)
                for key, item in vars(value).items()
                if not key.startswith("_") and not callable(item)
            }
        return str(value)

    @staticmethod
    def _hierarchy_item_uri(item: Any) -> Uri:
        if isinstance(item, dict):
            uri = _coerce_uri(item.get("uri"))
        else:
            uri = _coerce_uri(getattr(item, "uri", None))
        return uri or Uri.file("")

    @staticmethod
    def _call_hierarchy_item_from_payload(item: Any) -> Any:
        if not isinstance(item, dict):
            return item
        value = CallHierarchyItem(
            item.get("kind"),
            item.get("name"),
            item.get("detail"),
            item.get("uri"),
            _coerce_range(item.get("range")),
            _coerce_range(item.get("selectionRange")),
        )
        if item.get("tags") is not None:
            value.tags = item.get("tags")
        return value

    @staticmethod
    def _type_hierarchy_item_from_payload(item: Any) -> Any:
        if not isinstance(item, dict):
            return item
        value = TypeHierarchyItem(
            item.get("kind"),
            item.get("name"),
            item.get("detail"),
            item.get("uri"),
            _coerce_range(item.get("range")),
            _coerce_range(item.get("selectionRange")),
        )
        if item.get("tags") is not None:
            value.tags = item.get("tags")
        return value

    @staticmethod
    def _document_full_range(document: Any) -> Range:
        text = document.getText() if hasattr(document, "getText") else ""
        lines = str(text or "").split("\n")
        return Range(
            Position(0, 0),
            Position(max(0, len(lines) - 1), len(lines[-1] if lines else "")),
        )

    @staticmethod
    def _color_payload(color: Any) -> Dict[str, float]:
        value = _coerce_color(color)
        return {
            "red": float(getattr(value, "red", 0.0)),
            "green": float(getattr(value, "green", 0.0)),
            "blue": float(getattr(value, "blue", 0.0)),
            "alpha": float(getattr(value, "alpha", 1.0)),
        }

    @classmethod
    def _diagnostic_payload(cls, diagnostic: Any) -> Any:
        if isinstance(diagnostic, dict):
            return dict(diagnostic)
        return {
            "range": cls._range_payload(getattr(diagnostic, "range", None)),
            "message": str(getattr(diagnostic, "message", "")),
            "severity": getattr(diagnostic, "severity", None),
            "source": getattr(diagnostic, "source", ""),
            "code": getattr(diagnostic, "code", None),
        }

    @classmethod
    def _diagnostic_from_payload(cls, diagnostic: Any) -> Any:
        if isinstance(diagnostic, Diagnostic):
            return diagnostic
        if not isinstance(diagnostic, dict):
            return diagnostic
        value = Diagnostic(
            _coerce_range(diagnostic.get("range")),
            str(diagnostic.get("message") or ""),
            diagnostic.get("severity", 0),
            str(diagnostic.get("source") or ""),
            diagnostic.get("code"),
        )
        tags = diagnostic.get("tags")
        if isinstance(tags, list):
            value.tags = list(tags)
        related = (
            diagnostic.get("relatedInformation")
            or diagnostic.get("related_information"))
        if isinstance(related, list):
            value.related_information = list(related)
        return value

    @classmethod
    def _diagnostic_key(cls, diagnostic: Any) -> str:
        payload = cls._diagnostic_payload(diagnostic)
        if not isinstance(payload, dict):
            return repr(payload)
        comparable = {
            "range": payload.get("range"),
            "message": payload.get("message"),
            "severity": payload.get("severity"),
            "source": payload.get("source"),
            "code": payload.get("code"),
        }
        return json.dumps(comparable, sort_keys=True, default=str)

    def _code_action_context_diagnostics(
            self, resource: Any, diagnostics: Any = None) -> List[Any]:
        values = list(self._get_diagnostics(resource))
        seen = {self._diagnostic_key(item) for item in values}
        incoming = diagnostics if isinstance(diagnostics, list) else []
        for item in incoming:
            diagnostic = self._diagnostic_from_payload(item)
            key = self._diagnostic_key(diagnostic)
            if key in seen:
                continue
            seen.add(key)
            values.append(diagnostic)
        return values

    @staticmethod
    def _provider_values(value: Any) -> List[Any]:
        if value is None:
            return []
        if isinstance(value, list):
            return value
        return [value]

    @staticmethod
    def _code_action_kind_value(kind: Any) -> str:
        if kind is None:
            return ""
        if isinstance(kind, dict):
            kind = kind.get("value", kind.get("kind", ""))
        else:
            kind = getattr(kind, "value", kind)
        return str(kind or "")

    @classmethod
    def _code_action_matches_kind(cls, action: Any, only: Any) -> bool:
        only_value = cls._code_action_kind_value(only)
        if not only_value:
            return True
        if isinstance(action, dict):
            action_kind = action.get("kind")
        else:
            action_kind = getattr(action, "kind", None)
        action_value = cls._code_action_kind_value(action_kind)
        return bool(action_value) and (
            action_value == only_value
            or action_value.startswith(only_value + ".")
        )

    @classmethod
    def _inline_completion_items(cls, value: Any) -> List[Any]:
        if value is None:
            return []
        if isinstance(value, dict) and "items" in value:
            return cls._provider_values(value.get("items"))
        items = getattr(value, "items", None)
        if items is not None:
            return cls._provider_values(items)
        return cls._provider_values(value)

    def _request_external_language_provider(
            self, kind: str, document: Any, **payload: Any) -> Any:
        callback = self._language_provider_request_callback
        if callback is None:
            return None
        if document is None:
            request = {"kind": kind}
            request.update(payload)
            try:
                result = _resolve_provider_result(
                    callback(request), default=None)
            except Exception:
                return None
            if isinstance(result, dict) and "ok" in result:
                return result.get("value") if result.get("ok") else None
            return result
        request = {
            "kind": kind,
            "uri": str(getattr(document, "uri", "")),
            "languageId": str(getattr(document, "languageId", "plaintext")),
            "version": getattr(document, "version", 1),
        }
        uri_key = request["uri"]
        version = request["version"]
        send_text = (
            self._external_language_document_versions.get(uri_key) != version
        )
        if send_text:
            request["text"] = (
                document.getText() if hasattr(document, "getText") else "")
        request.update(payload)
        try:
            result = _resolve_provider_result(callback(request), default=None)
        except Exception:
            return None
        if isinstance(result, dict) and "ok" in result:
            if not result.get("ok"):
                return None
            if send_text:
                self._external_language_document_versions[uri_key] = version
            return result.get("value")
        if send_text:
            self._external_language_document_versions[uri_key] = version
        return result

    def _call_language_provider(
            self,
            provider: Any,
            method_name: str,
            args: Sequence[Any],
            default: Any = None) -> Any:
        method = None
        if isinstance(provider, dict):
            method = provider.get(method_name)
        if method is None:
            method = getattr(provider, method_name, None)
        if method is None and callable(provider):
            method = provider
        if not callable(method):
            return default
        try:
            value = _call_with_compatible_args(method, args)
            return _resolve_provider_result(value, default=default)
        except Exception:
            return default

    def _collect_language_provider_results(
            self,
            kind: str,
            document: Any,
            method_name: str,
            args: Sequence[Any],
            provider_id: Any = None) -> List[Any]:
        results: List[Any] = []
        for entry in self._matching_language_providers(kind, document):
            if not self._language_provider_entry_matches_id(
                    entry, provider_id):
                continue
            value = self._call_language_provider(
                entry.get("provider"), method_name, args, default=None)
            if value is None:
                continue
            if isinstance(value, list):
                results.extend(value)
            else:
                results.append(value)
        return results

    def _provide_completion_items(
            self,
            document_or_uri: Any,
            position: Any = None,
            trigger_character: Any = None,
            item_resolve_count: Any = None,
            context: Any = None) -> CompletionList:
        document = self._resolve_language_document(document_or_uri)
        pos = _coerce_position(position)
        context = self._completion_context(trigger_character, context)
        trigger = "" if context.get("triggerCharacter") is None else str(
            context.get("triggerCharacter"))
        items: List[Any] = []
        incomplete = False
        try:
            remaining_resolves = max(0, int(item_resolve_count or 0))
        except Exception:
            remaining_resolves = 0
        for entry in self._matching_language_providers("completion", document):
            triggers = tuple(str(item) for item in (entry.get("metadata") or ()))
            if trigger and trigger not in triggers:
                continue
            provider = entry.get("provider")
            value = self._call_language_provider(
                provider,
                "provideCompletionItems",
                (document, pos, CancellationToken.NONE, context),
                default=None)
            normalized = _completion_list_from_provider_result(value)
            if normalized is None:
                continue
            provider_items = list(normalized.items)
            if remaining_resolves > 0:
                resolve_method = (
                    provider.get("resolveCompletionItem")
                    if isinstance(provider, dict)
                    else getattr(provider, "resolveCompletionItem", None)
                )
                if callable(resolve_method):
                    resolved_items: List[Any] = []
                    for item in provider_items:
                        current = item
                        if remaining_resolves > 0:
                            resolved = self._call_language_provider(
                                provider,
                                "resolveCompletionItem",
                                (current, CancellationToken.NONE),
                                default=current)
                            current = resolved if resolved is not None else current
                            remaining_resolves -= 1
                        resolved_items.append(current)
                    provider_items = resolved_items
            items.extend(provider_items)
            incomplete = incomplete or bool(normalized.isIncomplete)
        external = self._request_external_language_provider(
            "completion",
            document,
            position=self._position_payload(pos),
            triggerCharacter=trigger or None,
            context=context,
            itemResolveCount=max(0, remaining_resolves))
        normalized_external = _completion_list_from_provider_result(external)
        if normalized_external is not None:
            items.extend(normalized_external.items)
            incomplete = incomplete or bool(normalized_external.isIncomplete)
        return CompletionList(items, incomplete)

    def _execute_completion_item_provider(
            self,
            uri: Any,
            position: Any = None,
            trigger_character: Any = None,
            item_resolve_count: Any = None,
            context: Any = None) -> CompletionList:
        return self._provide_completion_items(
            uri, position, trigger_character, item_resolve_count, context)

    @staticmethod
    def _completion_context(
            trigger_character: Any = None,
            context: Any = None) -> Dict[str, Any]:
        payload = dict(context) if isinstance(context, dict) else {}
        trigger = payload.get("triggerCharacter")
        if trigger is None and trigger_character is not None:
            trigger = trigger_character
        trigger_text = "" if trigger is None else str(trigger)
        try:
            trigger_kind = int(payload.get("triggerKind"))
        except Exception:
            trigger_kind = 1 if trigger_text else 0
        if trigger_kind not in (0, 1, 2):
            trigger_kind = 1 if trigger_text else 0
        return {
            "triggerKind": trigger_kind,
            "triggerCharacter": trigger_text or None,
        }

    def _execute_hover_provider(
            self, uri: Any, position: Any = None) -> List[Any]:
        document = self._resolve_language_document(uri)
        pos = _coerce_position(position)
        results = self._collect_language_provider_results(
            "hover", document, "provideHover",
            (document, pos, CancellationToken.NONE))
        results.extend(self._provider_values(
            self._request_external_language_provider(
                "hover", document, position=self._position_payload(pos))))
        return results

    @staticmethod
    def _signature_help_trigger_metadata(entry: Dict[str, Any]) -> Dict[str, List[str]]:
        metadata = entry.get("metadata")
        if isinstance(metadata, dict):
            triggers = metadata.get("triggerCharacters")
            retriggers = metadata.get("retriggerCharacters")
            return {
                "triggerCharacters": [str(item) for item in (triggers or [])],
                "retriggerCharacters": [str(item) for item in (retriggers or [])],
            }
        return {
            "triggerCharacters": [str(item) for item in (metadata or [])],
            "retriggerCharacters": [],
        }

    def _execute_signature_help_provider(
            self,
            uri: Any,
            position: Any = None,
            trigger_character: Any = None,
            trigger_kind: Any = None,
            is_retrigger: Any = False,
            active_signature_help: Any = None) -> Any:
        document = self._resolve_language_document(uri)
        pos = _coerce_position(position)
        trigger = "" if trigger_character is None else str(trigger_character)
        try:
            kind_value = int(trigger_kind) if trigger_kind is not None else (
                2 if trigger else 1)
        except Exception:
            kind_value = 2 if trigger else 1
        context = {
            "triggerKind": kind_value,
            "triggerCharacter": trigger or None,
            "isRetrigger": bool(is_retrigger),
            "activeSignatureHelp": active_signature_help,
        }
        for entry in self._matching_language_providers("signatureHelp", document):
            metadata = self._signature_help_trigger_metadata(entry)
            triggers = set(metadata.get("triggerCharacters") or [])
            retriggers = set(metadata.get("retriggerCharacters") or [])
            if trigger and trigger not in triggers and trigger not in retriggers:
                continue
            value = self._call_language_provider(
                entry.get("provider"),
                "provideSignatureHelp",
                (document, pos, CancellationToken.NONE, context),
                default=None)
            if value is not None:
                return value
        return self._request_external_language_provider(
            "signatureHelp",
            document,
            position=self._position_payload(pos),
            triggerCharacter=trigger or None,
            context=context)

    def _execute_definition_provider(
            self, uri: Any, position: Any = None) -> List[Any]:
        return self._execute_location_provider(
            "definition", "provideDefinition", uri, position)

    def _execute_type_definition_provider(
            self, uri: Any, position: Any = None) -> List[Any]:
        return self._execute_location_provider(
            "typeDefinition", "provideTypeDefinition", uri, position)

    def _execute_declaration_provider(
            self, uri: Any, position: Any = None) -> List[Any]:
        return self._execute_location_provider(
            "declaration", "provideDeclaration", uri, position)

    def _execute_implementation_provider(
            self, uri: Any, position: Any = None) -> List[Any]:
        return self._execute_location_provider(
            "implementation", "provideImplementation", uri, position)

    def _execute_location_provider(
            self, kind: str, method_name: str, uri: Any,
            position: Any = None) -> List[Any]:
        document = self._resolve_language_document(uri)
        pos = _coerce_position(position)
        results = self._collect_language_provider_results(
            kind, document, method_name,
            (document, pos, CancellationToken.NONE))
        results.extend(self._provider_values(
            self._request_external_language_provider(
                kind, document, position=self._position_payload(pos))))
        return results

    def _execute_reference_provider(
            self, uri: Any, position: Any = None, context: Any = None) -> List[Any]:
        document = self._resolve_language_document(uri)
        pos = _coerce_position(position)
        ref_context = context if isinstance(context, dict) else {}
        if "includeDeclaration" not in ref_context:
            ref_context = dict(ref_context)
            ref_context["includeDeclaration"] = True
        results = self._collect_language_provider_results(
            "references", document, "provideReferences",
            (document, pos, ref_context, CancellationToken.NONE))
        results.extend(self._provider_values(
            self._request_external_language_provider(
                "references",
                document,
                position=self._position_payload(pos),
                context=ref_context)))
        return results

    def _execute_document_highlight_provider(
            self, uri: Any, position: Any = None) -> List[Any]:
        document = self._resolve_language_document(uri)
        pos = _coerce_position(position)
        results = self._collect_language_provider_results(
            "documentHighlight", document, "provideDocumentHighlights",
            (document, pos, CancellationToken.NONE))
        results.extend(self._provider_values(
            self._request_external_language_provider(
                "documentHighlight",
                document,
                position=self._position_payload(pos))))
        return results

    def _execute_evaluatable_expression_provider(
            self, uri: Any, position: Any = None) -> Any:
        document = self._resolve_language_document(uri)
        pos = _coerce_position(position)
        for entry in self._matching_language_providers(
                "evaluatableExpression", document):
            value = self._call_language_provider(
                entry.get("provider"), "provideEvaluatableExpression",
                (document, pos, CancellationToken.NONE),
                default=None)
            if value is not None:
                return value
        external = self._request_external_language_provider(
            "evaluatableExpression",
            document,
            position=self._position_payload(pos))
        if isinstance(external, list):
            return external[0] if external else None
        return external

    def _execute_inline_value_provider(
            self, uri: Any, range: Any = None,
            context: Any = None) -> List[Any]:
        document = self._resolve_language_document(uri)
        view_range = _coerce_range(range)
        inline_context = _coerce_inline_value_context(context, view_range)
        results: List[Any] = []
        for entry in self._matching_language_providers("inlineValue", document):
            value = self._call_language_provider(
                entry.get("provider"), "provideInlineValues",
                (document, view_range, inline_context, CancellationToken.NONE),
                default=None)
            results.extend(self._provider_values(value))
        external = self._request_external_language_provider(
            "inlineValue",
            document,
            range=self._range_payload(view_range),
            context=self._language_value_payload(inline_context))
        results.extend(self._provider_values(external))
        return results

    def _execute_prepare_rename_provider(
            self, uri: Any, position: Any = None) -> Any:
        document = self._resolve_language_document(uri)
        pos = _coerce_position(position)
        for entry in self._matching_language_providers("rename", document):
            provider = entry.get("provider")
            method = provider.get("prepareRename") if isinstance(provider, dict) else (
                getattr(provider, "prepareRename", None))
            if not callable(method):
                continue
            value = self._call_language_provider(
                provider, "prepareRename",
                (document, pos, CancellationToken.NONE),
                default=None)
            if value is not None:
                return value
        return self._request_external_language_provider(
            "prepareRename",
            document,
            position=self._position_payload(pos))

    def _execute_rename_provider(
            self, uri: Any, position: Any = None, new_name: str = "") -> Any:
        document = self._resolve_language_document(uri)
        pos = _coerce_position(position)
        replacement = str(new_name or "")
        for entry in self._matching_language_providers("rename", document):
            value = self._call_language_provider(
                entry.get("provider"), "provideRenameEdits",
                (document, pos, replacement, CancellationToken.NONE),
                default=None)
            if value is not None:
                return value
        external = self._request_external_language_provider(
            "rename",
            document,
            position=self._position_payload(pos),
            newName=replacement)
        if isinstance(external, list):
            return external[0] if external else None
        return external

    def _execute_link_provider(
            self, uri: Any, link_resolve_count: Any = 0) -> List[Any]:
        document = self._resolve_language_document(uri)
        try:
            remaining_resolves = max(0, int(link_resolve_count or 0))
        except Exception:
            remaining_resolves = 0
        results: List[Any] = []
        for entry in self._matching_language_providers("documentLink", document):
            provider = entry.get("provider")
            value = self._call_language_provider(
                provider, "provideDocumentLinks",
                (document, CancellationToken.NONE),
                default=None)
            links = self._provider_values(value)
            if remaining_resolves:
                resolved_links: List[Any] = []
                resolve_method = None
                if isinstance(provider, dict):
                    resolve_method = provider.get("resolveDocumentLink")
                if resolve_method is None:
                    resolve_method = getattr(
                        provider, "resolveDocumentLink", None)
                for link in links:
                    if remaining_resolves > 0:
                        if callable(resolve_method):
                            try:
                                resolved = _resolve_provider_result(
                                    _call_with_compatible_args(
                                        resolve_method,
                                        (link, CancellationToken.NONE)),
                                    default=None)
                                if resolved is not None:
                                    link = resolved
                            except Exception:
                                pass
                        remaining_resolves -= 1
                    resolved_links.append(link)
                links = resolved_links
            results.extend(links)
        external = self._request_external_language_provider(
            "documentLink",
            document,
            linkResolveCount=remaining_resolves)
        results.extend(self._provider_values(external))
        return results

    def _execute_inlay_hint_provider(
            self, uri: Any, range: Any = None,
            hint_resolve_count: Any = 0) -> List[Any]:
        document = self._resolve_language_document(uri)
        hint_range = _coerce_range(range)
        try:
            remaining_resolves = max(0, int(hint_resolve_count or 0))
        except Exception:
            remaining_resolves = 0
        results: List[Any] = []
        for entry in self._matching_language_providers("inlayHint", document):
            provider = entry.get("provider")
            value = self._call_language_provider(
                provider,
                "provideInlayHints",
                (document, hint_range, CancellationToken.NONE),
                default=None)
            hints = self._provider_values(value)
            if remaining_resolves:
                resolved_hints: List[Any] = []
                resolve_method = (
                    provider.get("resolveInlayHint")
                    if isinstance(provider, dict)
                    else getattr(provider, "resolveInlayHint", None)
                )
                for hint in hints:
                    current = hint
                    if remaining_resolves > 0:
                        if callable(resolve_method):
                            resolved = self._call_language_provider(
                                provider,
                                "resolveInlayHint",
                                (current, CancellationToken.NONE),
                                default=current)
                            current = (
                                resolved if resolved is not None else current)
                        remaining_resolves -= 1
                    resolved_hints.append(current)
                hints = resolved_hints
            results.extend(hints)
        results.extend(self._provider_values(
            self._request_external_language_provider(
                "inlayHint",
                document,
                range=self._range_payload(hint_range),
                hintResolveCount=max(0, remaining_resolves))))
        return results

    def _execute_inline_completion_provider(
            self, uri: Any, position: Any = None,
            context: Any = None) -> List[Any]:
        document = self._resolve_language_document(uri)
        pos = _coerce_position(position)
        inline_context = context if isinstance(context, dict) else {}
        if "triggerKind" not in inline_context:
            inline_context = dict(inline_context)
            inline_context["triggerKind"] = 1
        if "selectedCompletionInfo" not in inline_context:
            inline_context = dict(inline_context)
            inline_context["selectedCompletionInfo"] = None
        results: List[Any] = []
        for entry in self._matching_language_providers(
                "inlineCompletion", document):
            value = self._call_language_provider(
                entry.get("provider"), "provideInlineCompletionItems",
                (document, pos, inline_context, CancellationToken.NONE),
                default=None)
            results.extend(self._inline_completion_items(value))
        external = self._request_external_language_provider(
            "inlineCompletion",
            document,
            position=self._position_payload(pos),
            context=inline_context)
        results.extend(self._inline_completion_items(external))
        return results

    def _execute_code_lens_provider(
            self, uri: Any, item_resolve_count: Any = 0) -> List[Any]:
        document = self._resolve_language_document(uri)
        try:
            remaining_resolves = max(0, int(item_resolve_count or 0))
        except Exception:
            remaining_resolves = 0
        results: List[Any] = []
        for entry in self._matching_language_providers("codeLens", document):
            provider = entry.get("provider")
            value = self._call_language_provider(
                provider, "provideCodeLenses",
                (document, CancellationToken.NONE),
                default=None)
            lenses = self._provider_values(value)
            if remaining_resolves:
                resolved_lenses: List[Any] = []
                resolve_method = None
                if isinstance(provider, dict):
                    resolve_method = provider.get("resolveCodeLens")
                if resolve_method is None:
                    resolve_method = getattr(provider, "resolveCodeLens", None)
                for lens in lenses:
                    if remaining_resolves > 0:
                        if callable(resolve_method):
                            try:
                                resolved = _resolve_provider_result(
                                    _call_with_compatible_args(
                                        resolve_method,
                                        (lens, CancellationToken.NONE)),
                                    default=None)
                                if resolved is not None:
                                    lens = resolved
                            except Exception:
                                pass
                        remaining_resolves -= 1
                    resolved_lenses.append(lens)
                lenses = resolved_lenses
            results.extend(lenses)
        external = self._request_external_language_provider(
            "codeLens",
            document,
            itemResolveCount=remaining_resolves)
        results.extend(self._provider_values(external))
        return results

    def _execute_folding_range_provider(self, uri: Any) -> List[Any]:
        document = self._resolve_language_document(uri)
        results = self._collect_language_provider_results(
            "foldingRange", document, "provideFoldingRanges",
            (document, {}, CancellationToken.NONE))
        results.extend(self._provider_values(
            self._request_external_language_provider(
                "foldingRange", document, context={})))
        return results

    def _execute_selection_range_provider(
            self, uri: Any, positions: Any = None) -> List[Any]:
        document = self._resolve_language_document(uri)
        if isinstance(positions, (list, tuple)):
            selection_positions = [_coerce_position(item) for item in positions]
        else:
            selection_positions = [_coerce_position(positions)]
        for entry in self._matching_language_providers(
                "selectionRange", document):
            value = self._call_language_provider(
                entry.get("provider"), "provideSelectionRanges",
                (document, selection_positions, CancellationToken.NONE),
                default=None)
            if value is not None:
                return self._provider_values(value)
        external = self._request_external_language_provider(
            "selectionRange",
            document,
            positions=[self._position_payload(pos)
                       for pos in selection_positions])
        return self._provider_values(external)

    def _execute_linked_editing_provider(
            self, uri: Any, position: Any = None) -> Any:
        document = self._resolve_language_document(uri)
        pos = _coerce_position(position)
        for entry in self._matching_language_providers(
                "linkedEditing", document):
            value = self._call_language_provider(
                entry.get("provider"),
                "provideLinkedEditingRanges",
                (document, pos, CancellationToken.NONE),
                default=None)
            if value is not None:
                return value
        return self._request_external_language_provider(
            "linkedEditing",
            document,
            position=self._position_payload(pos))

    def _execute_document_color_provider(self, uri: Any) -> List[Any]:
        document = self._resolve_language_document(uri)
        results = self._collect_language_provider_results(
            "documentColor", document, "provideDocumentColors",
            (document, CancellationToken.NONE))
        results.extend(self._provider_values(
            self._request_external_language_provider(
                "documentColor", document)))
        return results

    def _execute_color_presentation_provider(
            self, uri_or_color_or_context: Any, color_or_context: Any = None,
            range: Any = None) -> List[Any]:
        if isinstance(color_or_context, dict) and (
                "uri" in color_or_context or "document" in color_or_context):
            context_value = color_or_context
            uri = context_value.get("uri") or context_value.get("document")
            color = uri_or_color_or_context
            range = context_value.get("range")
        elif color_or_context is None and isinstance(
                uri_or_color_or_context, dict) and (
                    "uri" in uri_or_color_or_context
                    or "document" in uri_or_color_or_context):
            context_value = uri_or_color_or_context
            uri = context_value.get("uri") or context_value.get("document")
            color = context_value.get("color")
            range = context_value.get("range")
        else:
            uri = uri_or_color_or_context
            color = color_or_context
        document = self._resolve_language_document(uri)
        color_value = _coerce_color(color)
        color_range = _coerce_range(range)
        context = {"document": document, "range": color_range}
        results = self._collect_language_provider_results(
            "documentColor", document, "provideColorPresentations",
            (color_value, context, CancellationToken.NONE))
        results.extend(self._provider_values(
            self._request_external_language_provider(
                "colorPresentation",
                document,
                color=self._color_payload(color_value),
                range=self._range_payload(color_range))))
        return results

    def _execute_prepare_call_hierarchy_provider(
            self, uri: Any, position: Any = None) -> List[Any]:
        document = self._resolve_language_document(uri)
        pos = _coerce_position(position)
        for entry in self._matching_language_providers("callHierarchy", document):
            value = self._call_language_provider(
                entry.get("provider"),
                "prepareCallHierarchy",
                (document, pos, CancellationToken.NONE),
                default=None)
            if value is not None:
                return self._provider_values(value)
        external = self._request_external_language_provider(
            "prepareCallHierarchy",
            document,
            position=self._position_payload(pos))
        return self._provider_values(external)

    def _execute_call_hierarchy_incoming_provider(
            self, item: Any = None) -> List[Any]:
        if item is None:
            return []
        if isinstance(item, dict) and item.get("_nodeHierarchyHandle"):
            document = self._resolve_language_document(
                self._hierarchy_item_uri(item))
            external = self._request_external_language_provider(
                "callHierarchyIncoming",
                document,
                item=self._language_value_payload(item))
            return self._provider_values(external)
        local_item = self._call_hierarchy_item_from_payload(item)
        document = self._resolve_language_document(self._hierarchy_item_uri(item))
        for entry in self._matching_language_providers("callHierarchy", document):
            value = self._call_language_provider(
                entry.get("provider"),
                "provideCallHierarchyIncomingCalls",
                (local_item, CancellationToken.NONE),
                default=None)
            if value is not None:
                return self._provider_values(value)
        external = self._request_external_language_provider(
            "callHierarchyIncoming",
            document,
            item=self._language_value_payload(item))
        return self._provider_values(external)

    def _execute_call_hierarchy_outgoing_provider(
            self, item: Any = None) -> List[Any]:
        if item is None:
            return []
        if isinstance(item, dict) and item.get("_nodeHierarchyHandle"):
            document = self._resolve_language_document(
                self._hierarchy_item_uri(item))
            external = self._request_external_language_provider(
                "callHierarchyOutgoing",
                document,
                item=self._language_value_payload(item))
            return self._provider_values(external)
        local_item = self._call_hierarchy_item_from_payload(item)
        document = self._resolve_language_document(self._hierarchy_item_uri(item))
        for entry in self._matching_language_providers("callHierarchy", document):
            value = self._call_language_provider(
                entry.get("provider"),
                "provideCallHierarchyOutgoingCalls",
                (local_item, CancellationToken.NONE),
                default=None)
            if value is not None:
                return self._provider_values(value)
        external = self._request_external_language_provider(
            "callHierarchyOutgoing",
            document,
            item=self._language_value_payload(item))
        return self._provider_values(external)

    def _execute_prepare_type_hierarchy_provider(
            self, uri: Any, position: Any = None) -> List[Any]:
        document = self._resolve_language_document(uri)
        pos = _coerce_position(position)
        for entry in self._matching_language_providers("typeHierarchy", document):
            value = self._call_language_provider(
                entry.get("provider"),
                "prepareTypeHierarchy",
                (document, pos, CancellationToken.NONE),
                default=None)
            if value is not None:
                return self._provider_values(value)
        external = self._request_external_language_provider(
            "prepareTypeHierarchy",
            document,
            position=self._position_payload(pos))
        return self._provider_values(external)

    def _execute_type_hierarchy_supertypes_provider(
            self, item: Any = None) -> List[Any]:
        if item is None:
            return []
        if isinstance(item, dict) and item.get("_nodeTypeHierarchyHandle"):
            document = self._resolve_language_document(
                self._hierarchy_item_uri(item))
            external = self._request_external_language_provider(
                "typeHierarchySupertypes",
                document,
                item=self._language_value_payload(item))
            return self._provider_values(external)
        local_item = self._type_hierarchy_item_from_payload(item)
        document = self._resolve_language_document(self._hierarchy_item_uri(item))
        for entry in self._matching_language_providers("typeHierarchy", document):
            value = self._call_language_provider(
                entry.get("provider"),
                "provideTypeHierarchySupertypes",
                (local_item, CancellationToken.NONE),
                default=None)
            if value is not None:
                return self._provider_values(value)
        external = self._request_external_language_provider(
            "typeHierarchySupertypes",
            document,
            item=self._language_value_payload(item))
        return self._provider_values(external)

    def _execute_type_hierarchy_subtypes_provider(
            self, item: Any = None) -> List[Any]:
        if item is None:
            return []
        if isinstance(item, dict) and item.get("_nodeTypeHierarchyHandle"):
            document = self._resolve_language_document(
                self._hierarchy_item_uri(item))
            external = self._request_external_language_provider(
                "typeHierarchySubtypes",
                document,
                item=self._language_value_payload(item))
            return self._provider_values(external)
        local_item = self._type_hierarchy_item_from_payload(item)
        document = self._resolve_language_document(self._hierarchy_item_uri(item))
        for entry in self._matching_language_providers("typeHierarchy", document):
            value = self._call_language_provider(
                entry.get("provider"),
                "provideTypeHierarchySubtypes",
                (local_item, CancellationToken.NONE),
                default=None)
            if value is not None:
                return self._provider_values(value)
        external = self._request_external_language_provider(
            "typeHierarchySubtypes",
            document,
            item=self._language_value_payload(item))
        return self._provider_values(external)

    def _execute_document_semantic_tokens_legend(self, uri: Any) -> Any:
        document = self._resolve_language_document(uri)
        for entry in self._matching_language_providers(
                "semanticTokens", document):
            legend = _semantic_legend_payload(entry.get("metadata"))
            if legend is not None:
                return SemanticTokensLegend(
                    legend.get("tokenTypes", []),
                    legend.get("tokenModifiers", []))
        external = self._request_external_language_provider(
            "semanticTokensLegend", document)
        return external

    def _execute_document_range_semantic_tokens_legend(self, uri: Any) -> Any:
        document = self._resolve_language_document(uri)
        for entry in self._matching_language_providers(
                "semanticTokensRange", document):
            legend = _semantic_legend_payload(entry.get("metadata"))
            if legend is not None:
                return SemanticTokensLegend(
                    legend.get("tokenTypes", []),
                    legend.get("tokenModifiers", []))
        external = self._request_external_language_provider(
            "semanticTokensRangeLegend", document)
        return external

    def _execute_document_semantic_tokens_provider(self, uri: Any) -> Any:
        document = self._resolve_language_document(uri)
        for entry in self._matching_language_providers(
                "semanticTokens", document):
            value = self._call_language_provider(
                entry.get("provider"), "provideDocumentSemanticTokens",
                (document, CancellationToken.NONE),
                default=None)
            if value is not None:
                return value
        external = self._request_external_language_provider(
            "semanticTokens", document)
        if isinstance(external, dict) and "tokens" in external:
            return external.get("tokens")
        return external

    def _execute_document_range_semantic_tokens_provider(
            self, uri: Any, range: Any = None) -> Any:
        document = self._resolve_language_document(uri)
        token_range = _coerce_range(range)
        for entry in self._matching_language_providers(
                "semanticTokensRange", document):
            value = self._call_language_provider(
                entry.get("provider"), "provideDocumentRangeSemanticTokens",
                (document, token_range, CancellationToken.NONE),
                default=None)
            if value is not None:
                return value
        external = self._request_external_language_provider(
            "semanticTokensRange",
            document,
            range=self._range_payload(token_range))
        if isinstance(external, dict) and "tokens" in external:
            return external.get("tokens")
        return external

    def _execute_document_symbol_provider(self, uri: Any) -> List[Any]:
        document = self._resolve_language_document(uri)
        results = self._collect_language_provider_results(
            "documentSymbol", document, "provideDocumentSymbols",
            (document, CancellationToken.NONE))
        results.extend(self._provider_values(
            self._request_external_language_provider("documentSymbol", document)))
        return results

    def _execute_workspace_symbol_provider(
            self, query: Any = "") -> List[Any]:
        search = "" if query is None else str(query)
        results: List[Any] = []
        for entry in list(self._language_providers.get("workspaceSymbol", [])):
            value = self._call_language_provider(
                entry.get("provider"), "provideWorkspaceSymbols",
                (search, CancellationToken.NONE), default=None)
            results.extend(self._provider_values(value))
        results.extend(self._provider_values(
            self._request_external_language_provider(
                "workspaceSymbol", None, query=search)))
        return results

    def _execute_resolve_workspace_symbol_provider(
            self, symbol: Any = None) -> Any:
        if symbol is None:
            return None
        if isinstance(symbol, dict) and symbol.get("_workspaceSymbolHandle"):
            external = self._request_external_language_provider(
                "workspaceSymbolResolve", None, symbol=symbol)
            return external if external is not None else symbol
        for entry in list(self._language_providers.get("workspaceSymbol", [])):
            provider = entry.get("provider")
            method = provider.get("resolveWorkspaceSymbol") if isinstance(
                provider, dict) else getattr(
                    provider, "resolveWorkspaceSymbol", None)
            if not callable(method):
                continue
            value = self._call_language_provider(
                provider, "resolveWorkspaceSymbol",
                (symbol, CancellationToken.NONE), default=None)
            if value is not None:
                return value
        external = self._request_external_language_provider(
            "workspaceSymbolResolve", None, symbol=symbol)
        return external if external is not None else symbol

    def _execute_code_action_provider(
            self, uri: Any, range: Any = None, kind: Any = None,
            item_resolve_count: Any = 0, diagnostics: Any = None,
            trigger_kind: Any = None) -> List[Any]:
        document = self._resolve_language_document(uri)
        action_range = _coerce_range(range)
        context = {
            "diagnostics": self._code_action_context_diagnostics(
                document.uri, diagnostics)
        }
        if kind is not None:
            context["only"] = kind
        if trigger_kind is not None:
            context["triggerKind"] = trigger_kind
        try:
            remaining_resolves = max(0, int(item_resolve_count or 0))
        except Exception:
            remaining_resolves = 0
        results: List[Any] = []
        for entry in self._matching_language_providers("codeActions", document):
            provider = entry.get("provider")
            value = self._call_language_provider(
                provider,
                "provideCodeActions",
                (document, action_range, context, CancellationToken.NONE),
                default=None)
            actions = self._provider_values(value)
            if kind is not None:
                actions = [
                    action for action in actions
                    if self._code_action_matches_kind(action, kind)
                ]
            if remaining_resolves:
                resolved_actions: List[Any] = []
                resolve_method = (
                    provider.get("resolveCodeAction")
                    if isinstance(provider, dict)
                    else getattr(provider, "resolveCodeAction", None)
                )
                for action in actions:
                    current = action
                    if remaining_resolves > 0:
                        if callable(resolve_method):
                            resolved = self._call_language_provider(
                                provider,
                                "resolveCodeAction",
                                (current, CancellationToken.NONE),
                                default=current)
                            current = (
                                resolved if resolved is not None else current)
                        remaining_resolves -= 1
                    resolved_actions.append(current)
                actions = resolved_actions
            results.extend(actions)
        external_actions = self._provider_values(
            self._request_external_language_provider(
                "codeActions",
                document,
                range=self._range_payload(action_range),
                diagnostics=[
                    self._diagnostic_payload(item)
                    for item in context.get("diagnostics", [])
                ],
                only=kind,
                triggerKind=trigger_kind,
                itemResolveCount=max(0, remaining_resolves)))
        if kind is not None:
            external_actions = [
                action for action in external_actions
                if self._code_action_matches_kind(action, kind)
            ]
        results.extend(external_actions)
        return results

    def _execute_formatting_provider_list(self, uri: Any) -> List[Any]:
        document = self._resolve_language_document(uri)
        return self._formatting_provider_list(document)

    def _execute_format_document_provider(
            self, uri: Any, options: Any = None,
            provider_id: Any = None) -> List[Any]:
        document = self._resolve_language_document(uri)
        format_options = options or {}
        results = self._collect_language_provider_results(
            "formatting", document, "provideDocumentFormattingEdits",
            (document, format_options, CancellationToken.NONE),
            provider_id=provider_id)
        external_payload = {"options": format_options}
        if provider_id:
            external_payload["providerId"] = str(provider_id)
        results.extend(self._provider_values(
            self._request_external_language_provider(
                "formatting", document, **external_payload)))
        full_range = self._document_full_range(document)
        results.extend(self._collect_language_provider_results(
            "rangeFormatting",
            document,
            "provideDocumentRangeFormattingEdits",
            (document, full_range, format_options, CancellationToken.NONE),
            provider_id=provider_id))
        range_payload = {
            "range": self._range_payload(full_range),
            "options": format_options,
        }
        if provider_id:
            range_payload["providerId"] = str(provider_id)
        results.extend(self._provider_values(
            self._request_external_language_provider(
                "rangeFormatting",
                document,
                **range_payload)))
        return results

    def _execute_format_range_provider(
            self, uri: Any, range: Any = None,
            options: Any = None,
            provider_id: Any = None) -> List[Any]:
        document = self._resolve_language_document(uri)
        format_range = _coerce_range(range)
        format_options = options or {}
        results = self._collect_language_provider_results(
            "rangeFormatting",
            document,
            "provideDocumentRangeFormattingEdits",
            (document, format_range, format_options, CancellationToken.NONE),
            provider_id=provider_id)
        range_payload = {
            "range": self._range_payload(format_range),
            "options": format_options,
        }
        if provider_id:
            range_payload["providerId"] = str(provider_id)
        results.extend(self._provider_values(
            self._request_external_language_provider(
                "rangeFormatting",
                document,
                **range_payload)))
        return results

    def _execute_format_on_type_provider(
            self, uri: Any, position: Any = None, ch: Any = None,
            options: Any = None) -> List[Any]:
        document = self._resolve_language_document(uri)
        pos = _coerce_position(position)
        trigger = "" if ch is None else str(ch)
        format_options = options or {}
        results: List[Any] = []
        for entry in self._matching_language_providers(
                "onTypeFormatting", document):
            triggers = tuple(str(item) for item in (entry.get("metadata") or ()))
            if trigger and triggers and trigger not in triggers:
                continue
            value = self._call_language_provider(
                entry.get("provider"),
                "provideOnTypeFormattingEdits",
                (document, pos, trigger, format_options,
                 CancellationToken.NONE),
                default=None)
            results.extend(self._provider_values(value))
        results.extend(self._provider_values(
            self._request_external_language_provider(
                "onTypeFormatting",
                document,
                position=self._position_payload(pos),
                triggerCharacter=trigger or None,
                options=format_options)))
        return results

    @staticmethod
    def _coerce_data_transfer(value: Any) -> DataTransfer:
        return value if isinstance(value, DataTransfer) else DataTransfer(value or {})

    @classmethod
    def _data_transfer_payload(cls, value: Any) -> Dict[str, Any]:
        data_transfer = cls._coerce_data_transfer(value)
        return data_transfer.to_payload()

    @staticmethod
    def _data_transfer_matches_mime(
            data_transfer: DataTransfer, mime_type: str) -> bool:
        requested = str(mime_type or "").lower()
        if not requested:
            return False
        if requested == "files":
            return any(item.asFile() is not None for _mime, item in data_transfer)
        if requested.endswith("/*"):
            prefix = requested[:-1]
            return any(mime.startswith(prefix) for mime, _item in data_transfer)
        return data_transfer.has(requested)

    @classmethod
    def _data_transfer_matches_metadata(
            cls, data_transfer: DataTransfer, metadata: Any,
            key: str) -> bool:
        if not isinstance(metadata, dict):
            return True
        mime_types = metadata.get(key)
        if not mime_types:
            return True
        return any(
            cls._data_transfer_matches_mime(data_transfer, str(item))
            for item in mime_types)

    @staticmethod
    def _coerce_language_ranges(ranges: Any) -> List[Range]:
        if isinstance(ranges, (list, tuple)):
            return [_coerce_range(item) for item in ranges]
        if ranges is None:
            return [Range()]
        return [_coerce_range(ranges)]

    @staticmethod
    def _paste_context_payload(context: Any) -> Dict[str, Any]:
        if isinstance(context, dict):
            payload = dict(context)
        else:
            payload = {}
        try:
            payload["triggerKind"] = int(payload.get("triggerKind", 0) or 0)
        except Exception:
            payload["triggerKind"] = 0
        if payload.get("only") is not None:
            payload["only"] = _coerce_drop_or_paste_kind(
                payload.get("only")).value
        else:
            payload["only"] = None
        return payload

    @staticmethod
    def _paste_context_for_provider(context: Any) -> Dict[str, Any]:
        payload = VscodeNamespace._paste_context_payload(context)
        return {
            "triggerKind": payload["triggerKind"],
            "only": (
                _coerce_drop_or_paste_kind(payload.get("only"))
                if payload.get("only") is not None else None),
        }

    def _execute_prepare_document_paste_provider(
            self, uri: Any, ranges: Any = None,
            data_transfer: Any = None) -> DataTransfer:
        document = self._resolve_language_document(uri)
        paste_ranges = self._coerce_language_ranges(ranges)
        transfer = self._coerce_data_transfer(data_transfer)
        for entry in self._matching_language_providers("documentPaste", document):
            provider = entry.get("provider")
            method = provider.get("prepareDocumentPaste") if isinstance(
                provider, dict) else getattr(
                    provider, "prepareDocumentPaste", None)
            if not callable(method):
                continue
            self._call_language_provider(
                provider,
                "prepareDocumentPaste",
                (document, paste_ranges, transfer, CancellationToken.NONE),
                default=None)
        external = self._request_external_language_provider(
            "prepareDocumentPaste",
            document,
            ranges=[self._range_payload(rng) for rng in paste_ranges],
            dataTransfer=transfer.to_payload())
        if isinstance(external, dict):
            transfer = self._coerce_data_transfer(
                external.get("dataTransfer") or external)
        return transfer

    def _execute_document_paste_edit_provider(
            self, uri: Any, ranges: Any = None, data_transfer: Any = None,
            context: Any = None, paste_resolve_count: Any = 0) -> List[Any]:
        document = self._resolve_language_document(uri)
        paste_ranges = self._coerce_language_ranges(ranges)
        transfer = self._coerce_data_transfer(data_transfer)
        provider_context = self._paste_context_for_provider(context)
        payload_context = self._paste_context_payload(context)
        try:
            remaining_resolves = max(0, int(paste_resolve_count or 0))
        except Exception:
            remaining_resolves = 0
        results: List[Any] = []
        for entry in self._matching_language_providers("documentPaste", document):
            if not self._data_transfer_matches_metadata(
                    transfer, entry.get("metadata"), "pasteMimeTypes"):
                continue
            provider = entry.get("provider")
            value = self._call_language_provider(
                provider,
                "provideDocumentPasteEdits",
                (document, paste_ranges, transfer, provider_context,
                 CancellationToken.NONE),
                default=None)
            edits = self._provider_values(value)
            if remaining_resolves:
                resolve_method = provider.get("resolveDocumentPasteEdit") if (
                    isinstance(provider, dict)) else getattr(
                        provider, "resolveDocumentPasteEdit", None)
                if callable(resolve_method):
                    resolved_edits: List[Any] = []
                    for edit in edits:
                        if remaining_resolves > 0:
                            resolved = self._call_language_provider(
                                provider,
                                "resolveDocumentPasteEdit",
                                (edit, CancellationToken.NONE),
                                default=None)
                            if resolved is not None:
                                edit = resolved
                            remaining_resolves -= 1
                        resolved_edits.append(edit)
                    edits = resolved_edits
            results.extend(edits)
        external = self._request_external_language_provider(
            "documentPaste",
            document,
            ranges=[self._range_payload(rng) for rng in paste_ranges],
            dataTransfer=transfer.to_payload(),
            context=payload_context,
            pasteResolveCount=remaining_resolves)
        results.extend(self._provider_values(external))
        return results

    def _execute_document_drop_edit_provider(
            self, uri: Any, position: Any = None, data_transfer: Any = None,
            drop_resolve_count: Any = 0) -> List[Any]:
        document = self._resolve_language_document(uri)
        pos = _coerce_position(position)
        transfer = self._coerce_data_transfer(data_transfer)
        try:
            remaining_resolves = max(0, int(drop_resolve_count or 0))
        except Exception:
            remaining_resolves = 0
        results: List[Any] = []
        for entry in self._matching_language_providers("documentDrop", document):
            if not self._data_transfer_matches_metadata(
                    transfer, entry.get("metadata"), "dropMimeTypes"):
                continue
            provider = entry.get("provider")
            value = self._call_language_provider(
                provider,
                "provideDocumentDropEdits",
                (document, pos, transfer, CancellationToken.NONE),
                default=None)
            edits = self._provider_values(value)
            if remaining_resolves:
                resolve_method = provider.get("resolveDocumentDropEdit") if (
                    isinstance(provider, dict)) else getattr(
                        provider, "resolveDocumentDropEdit", None)
                if callable(resolve_method):
                    resolved_edits: List[Any] = []
                    for edit in edits:
                        if remaining_resolves > 0:
                            resolved = self._call_language_provider(
                                provider,
                                "resolveDocumentDropEdit",
                                (edit, CancellationToken.NONE),
                                default=None)
                            if resolved is not None:
                                edit = resolved
                            remaining_resolves -= 1
                        resolved_edits.append(edit)
                    edits = resolved_edits
            results.extend(edits)
        external = self._request_external_language_provider(
            "documentDrop",
            document,
            position=self._position_payload(pos),
            dataTransfer=transfer.to_payload(),
            dropResolveCount=remaining_resolves)
        results.extend(self._provider_values(external))
        return results

    def set_ui_bridge(self, bridge: UIBridge) -> None:
        """Connect this namespace to a live HTML UI bridge (e.g. AIEditorAPI).

        Must be called after construction but before any extension activation
        so that created OutputChannels / StatusBarItems / Terminals can push
        state changes to the webview.
        """
        self._ui_bridge = bridge

    def set_webview_view_change_callback(
            self,
            callback: Optional[Callable[[Dict[str, Any]], None]]) -> None:
        """Notify the host when runtime WebviewView providers change."""
        self._webview_view_change_callback = callback

    def set_tree_view_change_callback(
            self,
            callback: Optional[Callable[[Dict[str, Any]], None]]) -> None:
        """Notify the host when runtime TreeView providers or data change."""
        self._tree_view_change_callback = callback

    def set_language_provider_request_callback(
            self,
            callback: Optional[Callable[[Dict[str, Any]], Any]]) -> None:
        """Ask an external extension host for language-provider results."""
        self._language_provider_request_callback = callback
        self._external_language_document_versions.clear()

    def set_file_decoration_request_callback(
            self,
            callback: Optional[Callable[[Dict[str, Any]], Any]]) -> None:
        """Ask an external extension host for file-decoration results."""
        self._file_decoration_request_callback = callback

    def _notify_tree_view_changed(
            self,
            event: str,
            view_id: str,
            payload: Optional[Dict[str, Any]] = None) -> None:
        data = {
            "event": event,
            "view_id": view_id,
        }
        if payload:
            data.update(dict(payload))
        self._window_tabs_emitter.fire(data)
        self._window_tab_groups_emitter.fire(data)
        callback = self._tree_view_change_callback
        if callback is not None:
            try:
                callback(data)
            except Exception:
                pass

    def _notify_webview_view_changed(
            self,
            event: str,
            view_id: str,
            extension_id: str = "",
            options: Optional[Dict[str, Any]] = None) -> None:
        payload = {
            "event": event,
            "view_id": view_id,
            "extension_id": extension_id,
            "options": dict(options or {}),
        }
        self._window_tabs_emitter.fire(payload)
        self._window_tab_groups_emitter.fire(payload)
        callback = self._webview_view_change_callback
        if callback is not None:
            try:
                callback(payload)
            except Exception:
                pass

    def _generate_view_token(self, view_id: str) -> str:
        """Create a per-webview nonce token and store it for later verification."""
        token = secrets.token_hex(16)
        self._webview_tokens[view_id] = token
        return token

    def verify_webview_token(self, view_id: str, token: str) -> bool:
        """Verify that *token* matches the stored nonce for *view_id*."""
        expected = self._webview_tokens.get(view_id)
        if expected is None:
            return False
        return secrets.compare_digest(expected, token)

    def build(self, ext: ExtensionDescription = None) -> Dict[str, Any]:
        """Return a dict that serves as the ``vscode`` module for an extension."""
        extension_id = ext.id if ext else ""
        window_api = self._build_window()
        if extension_id:
            window_api = dict(window_api)
            window_api["registerWebviewViewProvider"] = (
                lambda view_id, provider, options=None, _eid=extension_id:
                self._register_webview_view_provider(
                    view_id, provider, options, _extension_id=_eid)
            )
        return {
            # Namespaces
            "commands": self._build_commands(),
            "window": window_api,
            "workspace": self._build_workspace(),
            "env": self._build_env(),
            "extensions": self._build_extensions(),
            "languages": self._build_languages(),
            "tasks": self._build_tasks(),
            "debug": self._build_debug(),
            "notebooks": self._build_notebooks(),
            "lm": self._build_lm(extension_id),
            "chat": self._build_chat(),
            "authentication": self._build_auth(),
            # Types
            "Position": Position,
            "Range": Range,
            "Uri": Uri,
            "Disposable": Disposable,
            "EventEmitter": EventEmitter,
            "CancellationTokenSource": CancellationTokenSource,
            "CancellationToken": CancellationToken,
            "LanguageModelChatMessage": LanguageModelChatMessage,
            "LanguageModelTextPart": LanguageModelTextPart,
            "LanguageModelToolCallPart": LanguageModelToolCallPart,
            "LanguageModelToolResultPart": LanguageModelToolResult,
            "LanguageModelDataPart": LanguageModelDataPart,
            "LanguageModelThinkingPart": LanguageModelThinkingPart,
            "LanguageModelError": LanguageModelError,
            "Location": Location,
            "ThemeColor": ThemeColor,
            "FileDecoration": FileDecoration,
            "DocumentHighlight": DocumentHighlight,
            "EvaluatableExpression": EvaluatableExpression,
            "InlineValueText": InlineValueText,
            "InlineValueVariableLookup": InlineValueVariableLookup,
            "InlineValueEvaluatableExpression": InlineValueEvaluatableExpression,
            "InlineValueContext": InlineValueContext,
            "SymbolInformation": SymbolInformation,
            "DataTransfer": DataTransfer,
            "DataTransferItem": DataTransferItem,
            "Diagnostic": Diagnostic,
            "CompletionItem": CompletionItem,
            "CompletionList": CompletionList,
            "Hover": Hover,
            "ParameterInformation": ParameterInformation,
            "SignatureInformation": SignatureInformation,
            "SignatureHelp": SignatureHelp,
            "CodeAction": CodeAction,
            "DocumentLink": DocumentLink,
            "Color": Color,
            "ColorInformation": ColorInformation,
            "ColorPresentation": ColorPresentation,
            "InlayHint": InlayHint,
            "InlayHintLabelPart": InlayHintLabelPart,
            "InlineCompletionItem": InlineCompletionItem,
            "InlineCompletionList": InlineCompletionList,
            "CodeLens": CodeLens,
            "FoldingRange": FoldingRange,
            "SelectionRange": SelectionRange,
            "CallHierarchyItem": CallHierarchyItem,
            "CallHierarchyIncomingCall": CallHierarchyIncomingCall,
            "CallHierarchyOutgoingCall": CallHierarchyOutgoingCall,
            "TypeHierarchyItem": TypeHierarchyItem,
            "SemanticTokensLegend": SemanticTokensLegend,
            "SemanticTokensBuilder": SemanticTokensBuilder,
            "SemanticTokens": SemanticTokens,
            "SemanticTokensEdit": SemanticTokensEdit,
            "SemanticTokensEdits": SemanticTokensEdits,
            "TextEdit": TextEdit,
            "WorkspaceEdit": WorkspaceEdit,
            "DocumentDropOrPasteEditKind": DocumentDropOrPasteEditKind,
            "DocumentDropEdit": DocumentDropEdit,
            "DocumentPasteEdit": DocumentPasteEdit,
            "ChatResultFeedback": ChatResult,
            "ChatResponseStream": ChatResponseStream,
            "LanguageModelToolResult": LanguageModelToolResult,
            # Enums
            "ChatVariableLevel": {"Short": 1, "Medium": 2, "Full": 3},
            "LanguageModelChatToolMode": {"Auto": 1, "Required": 2},
            "LanguageModelChatMessageRole": {"System": 0, "User": 1, "Assistant": 2},
            "ChatResultFeedbackKind": {"Unhelpful": 0, "Helpful": 1},
            "ChatLocation": {"Panel": 1, "Terminal": 2, "Notebook": 3, "Editor": 4},
            "ChatSessionStatus": {"Failed": 0, "Completed": 1, "InProgress": 2},
            "ExtensionMode": {"Production": 1, "Development": 2, "Test": 3},
            "InlayHintKind": {"Type": 1, "Parameter": 2},
            "InlineCompletionTriggerKind": {"Invoke": 0, "Automatic": 1},
            "CompletionTriggerKind": {
                "Invoke": 0,
                "TriggerCharacter": 1,
                "TriggerForIncompleteCompletions": 2,
            },
            "DocumentPasteTriggerKind": {"Automatic": 0, "PasteAs": 1},
            "FoldingRangeKind": {"Comment": 1, "Imports": 2, "Region": 3},
            "CompletionItemKind": {
                "Text": 0, "Method": 1, "Function": 2, "Constructor": 3,
                "Field": 4, "Variable": 5, "Class": 6, "Interface": 7,
                "Module": 8, "Property": 9, "Unit": 10, "Value": 11,
                "Enum": 12, "Keyword": 13, "Snippet": 14, "Color": 15,
                "File": 16, "Reference": 17, "Folder": 18,
                "EnumMember": 19, "Constant": 20, "Struct": 21,
                "Event": 22, "Operator": 23, "TypeParameter": 24,
                "User": 25, "Issue": 26,
            },
            "CompletionItemInsertTextRule": {
                "None": 0,
                "KeepWhitespace": 1,
                "InsertAsSnippet": 4,
            },
            "CompletionItemTag": {"Deprecated": 1},
            "SignatureHelpTriggerKind": {
                "Invoke": 1,
                "TriggerCharacter": 2,
                "ContentChange": 3,
            },
            "SymbolKind": {
                "File": 0, "Module": 1, "Namespace": 2, "Package": 3,
                "Class": 4, "Method": 5, "Property": 6, "Field": 7,
                "Constructor": 8, "Enum": 9, "Interface": 10,
                "Function": 11, "Variable": 12, "Constant": 13,
                "String": 14, "Number": 15, "Boolean": 16, "Array": 17,
                "Object": 18, "Key": 19, "Null": 20,
                "EnumMember": 21, "Struct": 22, "Event": 23,
                "Operator": 24, "TypeParameter": 25,
            },
            "SymbolTag": {"Deprecated": 1},
            "DocumentHighlightKind": {
                "Text": 0,
                "Read": 1,
                "Write": 2,
            },
            "CodeActionKind": {
                "QuickFix": "quickfix",
                "Refactor": "refactor",
                "RefactorExtract": "refactor.extract",
                "RefactorInline": "refactor.inline",
                "RefactorRewrite": "refactor.rewrite",
                "Source": "source",
                "SourceOrganizeImports": "source.organizeImports",
                "SourceFixAll": "source.fixAll",
                "Empty": "",
            },
            "DiagnosticSeverity": {"Error": 0, "Warning": 1, "Information": 2, "Hint": 3},
            "DiagnosticTag": {"Unnecessary": 1, "Deprecated": 2},
            "FileType": {"Unknown": 0, "File": 1, "Directory": 2, "SymbolicLink": 64},
            "ProgressLocation": {"SourceControl": 1, "Window": 10, "Notification": 15},
            "TaskScope": {"Global": 1, "Workspace": 2},
        }

    # ── commands ──

    def _build_commands(self) -> Dict[str, Callable]:
        return {
            "registerCommand": self._register_command,
            "registerTextEditorCommand": self._register_text_editor_command,
            "executeCommand": self._host.commands.execute,
            "getCommands": lambda filter_internal=False: self._host.commands.list_commands(),
        }

    def _register_command(self, command_id: str, callback: Callable) -> Disposable:
        dispose = self._host.commands.register(command_id, callback)
        return Disposable(dispose)

    def _register_text_editor_command(self, command_id: str,
                                      callback: Callable) -> Disposable:
        def _handler(*args: Any) -> Any:
            editor = self._active_text_editor
            edit = _TextEditorEdit(editor.document) if editor else _TextEditorEdit(None)
            return callback(editor, edit, *args)
        dispose = self._host.commands.register(command_id, _handler)
        return Disposable(dispose)

    # ── window ──

    def _build_window(self) -> Dict[str, Any]:
        if self._window_api is None:
            self._window_api = {
                "showInformationMessage": lambda msg, *items: self._show_message("info", msg, items),
                "showWarningMessage": lambda msg, *items: self._show_message("warn", msg, items),
                "showErrorMessage": lambda msg, *items: self._show_message("error", msg, items),
                "showQuickPick": self._show_quick_pick,
                "showInputBox": self._show_input_box,
                "createOutputChannel": lambda name, **kw: _OutputChannel(name, self._ui_bridge),
                "createStatusBarItem": lambda *a, **kw: _StatusBarItem(
                    alignment=a[0] if a else kw.get("alignment", 2),
                    priority=a[1] if len(a) > 1 else kw.get("priority", 0),
                    bridge=self._ui_bridge),
                "createWebviewPanel": self._create_webview_panel,
                "showTextDocument": self._show_text_document,
                "createTreeView": self._create_tree_view,
                "registerTreeDataProvider": self._register_tree_data_provider,
                "registerWebviewViewProvider": self._register_webview_view_provider,
                "registerFileDecorationProvider": self._register_file_decoration_provider,
                "createTerminal": self._create_terminal,
                "withProgress": lambda opts, task: task(_UIProgress(self._ui_bridge), CancellationToken.NONE),
                "activeTextEditor": None,
                "visibleTextEditors": [],
                "activeTerminal": None,
                "terminals": [],
                "onDidChangeActiveTextEditor": self._window_active_text_editor_emitter.event,
                "onDidChangeVisibleTextEditors": self._window_visible_text_editors_emitter.event,
                "onDidChangeActiveTerminal": self._window_active_terminal_emitter.event,
                "onDidOpenTerminal": self._window_open_terminal_emitter.event,
                "onDidCloseTerminal": self._window_close_terminal_emitter.event,
                "tabGroups": {"all": [], "activeTabGroup": None,
                              "onDidChangeTabGroups": self._window_tab_groups_emitter.event,
                              "onDidChangeTabs": self._window_tabs_emitter.event},
            }
        self._sync_window_state()
        return self._window_api

    def _sync_window_state(self) -> None:
        if self._window_api is None:
            return
        self._window_api["activeTextEditor"] = self._active_text_editor
        self._window_api["visibleTextEditors"] = list(self._visible_text_editors)
        self._window_api["activeTerminal"] = self._active_terminal
        self._window_api["terminals"] = list(self._terminals)

    def _register_file_decoration_provider(self, provider: Any) -> Disposable:
        self._file_decoration_providers.append(provider)

        def _dispose() -> None:
            try:
                self._file_decoration_providers.remove(provider)
            except ValueError:
                pass

        return Disposable(_dispose)

    def provide_file_decorations(self, uri: Any) -> List[Dict[str, Any]]:
        """Return extension-provided VS Code FileDecoration payloads."""
        uri_obj = _coerce_uri(uri) or Uri.file(str(uri or ""))
        results: List[Dict[str, Any]] = []
        for provider in list(self._file_decoration_providers):
            method = (
                provider.get("provideFileDecoration")
                if isinstance(provider, dict)
                else getattr(provider, "provideFileDecoration", None)
            )
            if not callable(method):
                continue
            try:
                value = method(uri_obj, CancellationToken.NONE)
            except TypeError:
                try:
                    value = method(uri_obj)
                except Exception:
                    continue
            except Exception:
                continue
            for item in self._provider_values(
                    _resolve_provider_result(value, default=None)):
                payload = self._language_value_payload(item)
                if isinstance(payload, dict):
                    results.append(payload)

        callback = self._file_decoration_request_callback
        if callback is not None:
            try:
                external = _resolve_provider_result(
                    callback({"uri": str(uri_obj)}), default=None)
            except Exception:
                external = None
            if isinstance(external, dict) and "ok" in external:
                external = external.get("value") if external.get("ok") else None
            for item in self._provider_values(external):
                if isinstance(item, dict):
                    results.append(dict(item))
                else:
                    payload = self._language_value_payload(item)
                    if isinstance(payload, dict):
                        results.append(payload)
        return results

    def _show_message(self, level: str, message: Any,
                      items: Sequence[Any]) -> Any:
        if self._ui_bridge is not None:
            try:
                self._ui_bridge.show_message(level, str(message))
            except Exception:
                print(f"[{level}] {message}")
        else:
            print(f"[{level}] {message}")
        return items[0] if items else None

    def _show_quick_pick(self, items: Any, **kw: Any) -> Any:
        """Show a quick-pick selection UI or fall back to first item."""
        if self._ui_bridge is not None:
            try:
                result = self._ui_bridge.show_quick_pick(
                    list(items) if items else [], kw)
                if result is not None:
                    return result
            except Exception:
                pass
        # Fallback: return first item
        return items[0] if items else None

    def _show_input_box(self, **kw: Any) -> Any:
        """Show an input box UI or fall back to default value."""
        if self._ui_bridge is not None:
            try:
                result = self._ui_bridge.show_input_box(kw)
                if result is not None:
                    return result
            except Exception:
                pass
        # Fallback: return default value
        return kw.get("value", "")

    def _create_tree_view(self, view_id: str, **kw: Any) -> Any:
        provider = kw.get("treeDataProvider")
        view = self._tree_views.get(view_id)
        if view is None:
            view = _TreeView(view_id)
        self._tree_views[view_id] = view
        initial_state = {
            key: kw[key]
            for key in ("message", "title", "description", "badge")
            if key in kw
        }
        if initial_state:
            view.apply_state(initial_state, notify=False)
        if "dragAndDropController" in kw:
            view.bind_drag_and_drop_controller(
                kw.get("dragAndDropController"))
        if provider is not None:
            self._tree_data_providers[view_id] = provider
            view.bind_provider(provider, self._notify_tree_view_changed)
        self._notify_tree_view_changed("registered", view_id, {
            "refreshVersion": getattr(view, "refresh_version", 0),
            "state": view.state_payload(),
        })
        return view

    def _register_tree_data_provider(self, view_id: str,
                                     provider: Any) -> Disposable:
        self._tree_data_providers[view_id] = provider
        view = self._tree_views.get(view_id)
        if view is None:
            view = _TreeView(view_id)
            self._tree_views[view_id] = view
        view.bind_provider(provider, self._notify_tree_view_changed)
        self._notify_tree_view_changed("registered", view_id, {
            "refreshVersion": getattr(view, "refresh_version", 0),
            "state": view.state_payload(),
        })

        def _dispose() -> None:
            self._tree_data_providers.pop(view_id, None)
            current = self._tree_views.get(view_id)
            if current is view:
                view.bind_provider(None, self._notify_tree_view_changed)
                self._notify_tree_view_changed("disposed", view_id, {
                    "refreshVersion": getattr(view, "refresh_version", 0),
                    "state": view.state_payload(),
                })

        return Disposable(_dispose)

    def _register_webview_view_provider(self, view_id: str, provider: Any,
                                        options: Dict[str, Any] = None,
                                        _extension_id: str = "") -> Disposable:
        # Risk 3: check for view ID conflicts with another extension
        existing = self._webview_view_providers.get(view_id)
        if existing is not None:
            existing_ext = existing.get("_extensionId", "")
            registering_ext = _extension_id or ""
            if existing_ext and registering_ext and existing_ext != registering_ext:
                print(f"[webviewView] view ID conflict: '{view_id}' already "
                      f"registered by '{existing_ext}', skipping "
                      f"registration from '{registering_ext}'")
                return Disposable()

        # Risk 2: generate per-webview nonce token
        token = self._generate_view_token(view_id)

        self._webview_view_providers[view_id] = {
            "provider": provider,
            "options": dict(options or {}),
            "_extensionId": _extension_id or "",
        }
        view = _WebviewView(view_id, bridge=self._ui_bridge, token=token)
        self._webview_views[view_id] = view
        if hasattr(provider, "resolveWebviewView"):
            try:
                provider.resolveWebviewView(view, None, CancellationToken.NONE)
            except TypeError:
                provider.resolveWebviewView(view)
        self._notify_webview_view_changed(
            "registered", view_id, _extension_id or "", dict(options or {}))

        # Risk 4: disposable cleans up view, dicts, and token
        def _dispose_registration() -> None:
            v = self._webview_views.pop(view_id, None)
            if v is not None:
                v.dispose()
            self._webview_view_providers.pop(view_id, None)
            self._webview_tokens.pop(view_id, None)
            self._notify_webview_view_changed(
                "disposed", view_id, _extension_id or "", dict(options or {}))

        return Disposable(_dispose_registration)

    def _create_webview_panel(self, view_type: str, title: str,
                              column: Any = None, **kw: Any) -> Any:
        panel = _WebviewPanel(view_type, title, bridge=self._ui_bridge)
        if column is not None:
            panel.viewColumn = column
        if kw:
            panel.options = dict(kw)
        self._webview_panels.setdefault(view_type, []).append(panel)
        panel._on_dispose = lambda vt=view_type, instance=panel: self._dispose_webview_panel(vt, instance)
        return panel

    def _dispose_webview_panel(self, view_type: str,
                               panel: "_WebviewPanel") -> None:
        panels = self._webview_panels.get(view_type, [])
        if panel in panels:
            panels.remove(panel)
        if not panels and view_type in self._webview_panels:
            self._webview_panels.pop(view_type, None)

    def _create_terminal(self, *args: Any, **kw: Any) -> Any:
        name = ""
        if args and isinstance(args[0], str):
            name = args[0]
        elif args and isinstance(args[0], dict):
            name = str(args[0].get("name", ""))
        name = str(kw.get("name") or name or "SAO Terminal")
        terminal = _Terminal(name, self._ui_bridge)
        terminal._on_dispose = lambda t=terminal: self._on_terminal_disposed(t)
        self._terminals.append(terminal)
        self._active_terminal = terminal
        self._sync_window_state()
        self._window_active_terminal_emitter.fire(terminal)
        self._window_open_terminal_emitter.fire(terminal)
        return terminal

    def _on_terminal_disposed(self, terminal: "_Terminal") -> None:
        if terminal in self._terminals:
            self._terminals.remove(terminal)
            if self._active_terminal is terminal:
                self._active_terminal = self._terminals[-1] if self._terminals else None
            self._sync_window_state()
            self._window_active_terminal_emitter.fire(self._active_terminal)
            self._window_close_terminal_emitter.fire(terminal)

    # ── workspace ──

    def _build_workspace(self) -> Dict[str, Any]:
        if self._workspace_api is None:
            self._workspace_api = {
                "getConfiguration": self._get_configuration,
                "onDidChangeConfiguration": self._config_change_emitter.event,
                "workspaceFolders": self._get_workspace_folders(),
                "rootPath": self._get_root_path(),
                "name": "SAO Workspace",
                "fs": _FileSystem(self),
                "openTextDocument": self._open_text_document,
                "applyEdit": self._apply_workspace_edit,
                "findFiles": self._find_files,
                "createFileSystemWatcher": self._create_file_system_watcher,
                "asRelativePath": self._as_relative_path,
                "registerFileSystemProvider": self._register_file_system_provider,
                "saveAll": lambda include_untitled=False: True,
                "onDidOpenTextDocument": self._workspace_open_text_document_emitter.event,
                "onDidCloseTextDocument": self._workspace_close_text_document_emitter.event,
                "onDidChangeTextDocument": self._workspace_change_text_document_emitter.event,
                "onDidSaveTextDocument": self._workspace_save_text_document_emitter.event,
                "onDidCreateFiles": self._workspace_create_files_emitter.event,
                "onDidDeleteFiles": self._workspace_delete_files_emitter.event,
                "onDidRenameFiles": self._workspace_rename_files_emitter.event,
                "onDidChangeWorkspaceFolders": self._workspace_folders_change_emitter.event,
                "textDocuments": [],
            }
        self._sync_workspace_state()
        return self._workspace_api

    def _sync_workspace_state(self) -> None:
        if self._workspace_api is None:
            return
        self._workspace_api["workspaceFolders"] = self._get_workspace_folders()
        self._workspace_api["rootPath"] = self._get_root_path()
        self._workspace_api["textDocuments"] = list(self._text_documents)

    @staticmethod
    def _document_key(document_or_uri: Any) -> str:
        if isinstance(document_or_uri, _TextDocument):
            return str(document_or_uri.uri)
        if hasattr(document_or_uri, "uri"):
            return str(document_or_uri.uri)
        return str(document_or_uri)

    def _remember_text_document(self, document: "_TextDocument") -> "_TextDocument":
        doc_key = self._document_key(document)
        for existing in self._text_documents:
            if self._document_key(existing) == doc_key:
                existing._save_callback = self._save_text_document
                return existing
        document._save_callback = self._save_text_document
        self._text_documents.append(document)
        self._sync_workspace_state()
        self._workspace_open_text_document_emitter.fire(document)
        return document

    def update_text_document_snapshot(
            self, uri: Any, content: str,
            language_id: str = "") -> "_TextDocument":
        """Update an in-memory editor buffer without saving it to disk."""
        uri_obj = _coerce_uri(uri) or Uri.parse("untitled:Untitled-1")
        doc_key = str(uri_obj)
        text = "" if content is None else str(content)
        language = str(language_id or "").strip()
        if not language:
            language = _language_id_for_path(
                uri_obj.fs_path if uri_obj.scheme == "file" else uri_obj.path)

        document = None
        for existing in self._text_documents:
            if self._document_key(existing) == doc_key:
                document = existing
                break
        if document is None:
            document = self._remember_text_document(
                _TextDocument(uri_obj, text, language))
            document.isDirty = False
            return document

        changed = document.getText() != text
        language_changed = document.languageId != language
        if language_changed:
            document.languageId = language
        if changed:
            document._content = text
            document.version += 1
            document.isDirty = True
        if changed or language_changed:
            self._workspace_change_text_document_emitter.fire(
                {"document": document})
            self._sync_workspace_state()
        return document

    def _get_configuration(
            self,
            section: str = "",
            scope: Any = None) -> WorkspaceConfiguration:
        data: Dict[str, Any] = {}
        if self._settings_getter:
            ai_cfg = self._settings_getter("ai_editor", {}) or {}
            if isinstance(ai_cfg, dict):
                data["ai_editor"] = dict(ai_cfg)
                for name, value in ai_cfg.items():
                    if isinstance(value, dict):
                        data.setdefault(str(name), value)
            themes = self._settings_getter("panel_themes", {}) or {}
            if isinstance(themes, dict):
                data["panel_themes"] = dict(themes)
        return WorkspaceConfiguration(
            section, data, _config_override_identifier_from_scope(scope))

    def _get_workspace_folders(self) -> List[Dict]:
        try:
            from ai_editor.scopes import _base_dir
            return [{"uri": Uri.file(_base_dir()), "name": "workspace", "index": 0}]
        except Exception:
            return []

    def _get_root_path(self) -> str:
        try:
            from ai_editor.scopes import _base_dir
            return _base_dir()
        except Exception:
            return ""

    def _get_file_system_provider(self, scheme: str) -> Optional[Dict[str, Any]]:
        if not scheme or scheme == "file":
            return None
        return self._file_system_providers.get(str(scheme))

    @staticmethod
    def _call_provider_method(provider: Any,
                              names: Sequence[str],
                              *args: Any,
                              **kwargs: Any) -> Any:
        for name in names:
            method = getattr(provider, name, None)
            if callable(method):
                return method(*args, **kwargs)
        raise AttributeError(f"Provider missing methods: {', '.join(names)}")

    def _read_document_bytes(self, uri: Uri) -> bytes:
        provider_entry = self._get_file_system_provider(uri.scheme)
        if provider_entry is None:
            with open(uri.fs_path, "rb") as fh:
                return fh.read()
        raw = self._call_provider_method(
            provider_entry["provider"], ("readFile", "read_file"), uri)
        if isinstance(raw, bytes):
            return raw
        if isinstance(raw, bytearray):
            return bytes(raw)
        if isinstance(raw, memoryview):
            return raw.tobytes()
        if raw is None:
            return b""
        return str(raw).encode("utf-8")

    def _write_document_bytes(self, uri: Uri, content: bytes) -> None:
        provider_entry = self._get_file_system_provider(uri.scheme)
        if provider_entry is None:
            parent = os.path.dirname(uri.fs_path)
            if parent:
                os.makedirs(parent, exist_ok=True)
            with open(uri.fs_path, "wb") as fh:
                fh.write(content)
            return
        self._call_provider_method(
            provider_entry["provider"], ("writeFile", "write_file"),
            uri, bytes(content))

    def _open_text_document(self, uri: Any = None, **kw: Any) -> "_TextDocument":
        if isinstance(uri, dict):
            uri_value = uri.get("uri")
            if uri_value is not None:
                return self._open_text_document(uri_value, **{k: v for k, v in uri.items() if k != "uri"})
            content = str(uri.get("content", ""))
            language = str(uri.get("language", "plaintext"))
            return self._remember_text_document(
                _TextDocument(Uri.parse("untitled:Untitled-1"), content, language))
        if uri is None and kw:
            content = str(kw.get("content", ""))
            language = str(kw.get("language", "plaintext"))
            return self._remember_text_document(
                _TextDocument(Uri.parse("untitled:Untitled-1"), content, language))
        raw_uri = uri
        path = uri.fs_path if hasattr(uri, "fs_path") else str(uri or "")
        if not path:
            raise ValueError("openTextDocument requires a file URI/path or content")
        uri_obj = raw_uri if isinstance(raw_uri, Uri) else (
            Uri.parse(path) if isinstance(path, str) and ":" in path and not os.path.isabs(path)
            else Uri.file(path)
        )
        if isinstance(uri_obj, Uri) and uri_obj.scheme != "file":
            language = str(kw.get("language") or _language_id_for_path(uri_obj.path or path))
            try:
                content = self._read_document_bytes(uri_obj).decode("utf-8", errors="replace")
            except Exception:
                content = ""
            return self._remember_text_document(_TextDocument(uri_obj, content, language))
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            content = fh.read()
        return self._remember_text_document(
            _TextDocument(Uri.file(path), content, _language_id_for_path(path)))

    def _apply_workspace_edit(self, edit: Any) -> bool:
        if not edit:
            return True
        entries = self._workspace_edit_entries(edit)
        if entries == []:
            return True
        if entries is None:
            return False
        try:
            for entry in entries:
                if not self._apply_workspace_edit_entry(entry):
                    return False
        except Exception:
            return False
        return True

    def _workspace_edit_entries(self, edit: Any) -> Optional[List[Dict[str, Any]]]:
        if isinstance(edit, WorkspaceEdit):
            return edit.entries()
        if isinstance(edit, dict):
            if isinstance(edit.get("edits"), list):
                entries = edit.get("edits", [])
                if not all(isinstance(item, dict) for item in entries):
                    return None
                return list(entries)
            if isinstance(edit.get("changes"), dict):
                entries: List[Dict[str, Any]] = []
                for uri, text_edits in edit["changes"].items():
                    if not isinstance(text_edits, list):
                        return None
                    entries.append({"kind": "textEdits", "uri": uri, "edits": text_edits})
                return entries
            if isinstance(edit.get("documentChanges"), list):
                entries = []
                for item in edit["documentChanges"]:
                    if not isinstance(item, dict):
                        return None
                    if item.get("kind"):
                        entries.append(item)
                        continue
                    text_document = item.get("textDocument") or {}
                    uri = text_document.get("uri") or item.get("uri")
                    if uri is None or not isinstance(item.get("edits"), list):
                        return None
                    entries.append({"kind": "textEdits", "uri": uri, "edits": item.get("edits", [])})
                return entries
        return None

    def _apply_workspace_edit_entry(self, entry: Dict[str, Any]) -> bool:
        kind = str(entry.get("kind") or "")
        if kind in {"replace", "insert", "delete", "textEdits"}:
            return self._apply_text_entry(entry)
        if kind == "createFile":
            return self._apply_create_file(entry)
        if kind == "deleteFile":
            return self._apply_delete_file(entry)
        if kind == "renameFile":
            return self._apply_rename_file(entry)
        return False

    def _apply_text_entry(self, entry: Dict[str, Any]) -> bool:
        uri = _coerce_uri(entry.get("uri"))
        if uri is None:
            return False
        document = self._get_or_open_document(uri)
        if document is None:
            return False
        current = document.getText()
        if entry.get("kind") == "textEdits":
            updated = _apply_structured_text_edits(current, entry.get("edits", []))
        else:
            updated = _apply_structured_text_edits(current, [entry])
        if updated is None:
            return False
        self._apply_document_content(document, updated)
        return True

    def _apply_create_file(self, entry: Dict[str, Any]) -> bool:
        uri = _coerce_uri(entry.get("uri"))
        if uri is None:
            return False
        if uri.scheme != "file":
            try:
                self._write_document_bytes(uri, b"")
            except Exception:
                return False
            doc = _TextDocument(uri, "", _language_id_for_path(uri.path))
            self._remember_text_document(doc)
            self._workspace_create_files_emitter.fire({"files": [uri]})
            return True
        path = uri.fs_path
        overwrite = bool((entry.get("options") or {}).get("overwrite"))
        if os.path.exists(path) and not overwrite:
            return False
        parent = os.path.dirname(path)
        if parent:
            os.makedirs(parent, exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("")
        doc = _TextDocument(uri, "", _language_id_for_path(path))
        self._remember_text_document(doc)
        self._workspace_create_files_emitter.fire({"files": [uri]})
        self._notify_workspace_watchers(path, "create")
        return True

    def _apply_delete_file(self, entry: Dict[str, Any]) -> bool:
        uri = _coerce_uri(entry.get("uri"))
        if uri is None:
            return False
        if uri.scheme != "file":
            provider_entry = self._get_file_system_provider(uri.scheme)
            if provider_entry is None:
                return False
            try:
                self._call_provider_method(
                    provider_entry["provider"], ("delete", "deleteFile", "delete_file"),
                    uri, entry.get("options") or {})
            except Exception:
                return bool((entry.get("options") or {}).get("ignoreIfNotExists"))
            self._text_documents = [doc for doc in self._text_documents if self._document_key(doc) != str(uri)]
            self._sync_workspace_state()
            self._workspace_delete_files_emitter.fire({"files": [uri]})
            return True
        path = uri.fs_path
        if not os.path.exists(path):
            return bool((entry.get("options") or {}).get("ignoreIfNotExists"))
        if os.path.isdir(path):
            import shutil
            shutil.rmtree(path)
        else:
            os.remove(path)
        self._text_documents = [doc for doc in self._text_documents if self._document_key(doc) != str(uri)]
        self._sync_workspace_state()
        self._workspace_delete_files_emitter.fire({"files": [uri]})
        self._notify_workspace_watchers(path, "delete")
        return True

    def _apply_rename_file(self, entry: Dict[str, Any]) -> bool:
        old_uri = _coerce_uri(entry.get("oldUri"))
        new_uri = _coerce_uri(entry.get("newUri"))
        if old_uri is None or new_uri is None:
            return False
        if old_uri.scheme != "file" or new_uri.scheme != "file":
            if old_uri.scheme != new_uri.scheme:
                return False
            provider_entry = self._get_file_system_provider(old_uri.scheme)
            if provider_entry is None:
                return False
            try:
                self._call_provider_method(
                    provider_entry["provider"], ("rename", "renameFile", "rename_file"),
                    old_uri, new_uri, entry.get("options") or {})
            except Exception:
                return False
            for document in self._text_documents:
                if self._document_key(document) == str(old_uri):
                    document.uri = new_uri
                    document.fileName = str(new_uri)
                    document.languageId = _language_id_for_path(new_uri.path)
            self._sync_workspace_state()
            self._workspace_rename_files_emitter.fire({"files": [{"oldUri": old_uri, "newUri": new_uri}]})
            return True
        old_path = old_uri.fs_path
        new_path = new_uri.fs_path
        if not os.path.exists(old_path):
            return False
        parent = os.path.dirname(new_path)
        if parent:
            os.makedirs(parent, exist_ok=True)
        os.replace(old_path, new_path)
        for document in self._text_documents:
            if self._document_key(document) == str(old_uri):
                document.uri = new_uri
                document.fileName = new_path
                document.languageId = _language_id_for_path(new_path)
        self._sync_workspace_state()
        self._workspace_rename_files_emitter.fire({"files": [{"oldUri": old_uri, "newUri": new_uri}]})
        self._notify_workspace_watchers(old_path, "delete")
        self._notify_workspace_watchers(new_path, "create")
        return True

    def _get_or_open_document(self, uri: Uri) -> Optional["_TextDocument"]:
        key = str(uri)
        for document in self._text_documents:
            if self._document_key(document) == key:
                return document
        if uri.scheme != "file":
            return self._open_text_document(uri)
        path = uri.fs_path
        if os.path.exists(path):
            return self._open_text_document(uri)
        parent = os.path.dirname(path)
        if parent:
            os.makedirs(parent, exist_ok=True)
        return self._remember_text_document(_TextDocument(uri, "", _language_id_for_path(path)))

    def _apply_document_content(self, document: "_TextDocument", content: str) -> None:
        document._content = content
        document.version += 1
        document.isDirty = True
        self._workspace_change_text_document_emitter.fire({"document": document})
        self._sync_workspace_state()

    def _save_text_document(self, document: "_TextDocument") -> bool:
        if document.isUntitled:
            return False
        content = document.getText()
        if getattr(document.uri, "scheme", "") == "file":
            parent = os.path.dirname(document.fileName)
            if parent:
                os.makedirs(parent, exist_ok=True)
            with open(document.fileName, "w", encoding="utf-8") as fh:
                fh.write(content)
            document.isDirty = False
            self._workspace_save_text_document_emitter.fire(document)
            self._notify_workspace_watchers(document.fileName, "change")
            self._sync_workspace_state()
            return True
        else:
            try:
                self._write_document_bytes(document.uri, content.encode("utf-8"))
                document.isDirty = False
                self._workspace_save_text_document_emitter.fire(document)
                self._sync_workspace_state()
                return True
            except Exception:
                document.isDirty = True
                self._sync_workspace_state()
                return False

    def _create_file_system_watcher(self, pattern: Any, *args: Any,
                                    **kw: Any) -> Any:
        watcher = _FileSystemWatcher(pattern, self._get_root_path)
        if args:
            watcher.ignoreCreateEvents = bool(args[0])
        if len(args) > 1:
            watcher.ignoreChangeEvents = bool(args[1])
        if len(args) > 2:
            watcher.ignoreDeleteEvents = bool(args[2])
        for key, attr in (
                ("ignoreCreateEvents", "ignoreCreateEvents"),
                ("ignoreChangeEvents", "ignoreChangeEvents"),
                ("ignoreDeleteEvents", "ignoreDeleteEvents")):
            if key in kw:
                setattr(watcher, attr, bool(kw[key]))
        self._workspace_watchers.append(watcher)
        watcher._on_dispose = lambda w=watcher: self._dispose_workspace_watcher(w)
        return watcher

    def _dispose_workspace_watcher(self, watcher: "_FileSystemWatcher") -> None:
        if watcher in self._workspace_watchers:
            self._workspace_watchers.remove(watcher)

    def _notify_workspace_watchers(self, path: str, event_kind: str) -> None:
        for watcher in list(self._workspace_watchers):
            watcher.notify_path(path, event_kind)

    def _as_relative_path(self, path_or_uri: Any,
                          include_workspace_folder: bool = False) -> str:
        path = path_or_uri.fs_path if hasattr(path_or_uri, "fs_path") else str(path_or_uri)
        root = self._get_root_path()
        try:
            rel = os.path.relpath(path, root).replace("\\", "/")
        except Exception:
            rel = path.replace("\\", "/")
        if include_workspace_folder:
            folder = os.path.basename(root.rstrip("\\/")) or "workspace"
            return f"{folder}/{rel}"
        return rel

    def _register_file_system_provider(self, scheme: str, provider: Any,
                                       options: Dict = None) -> Disposable:
        key = str(scheme or "")
        self._file_system_providers[key] = {
            "provider": provider,
            "options": dict(options or {}),
        }

        def _dispose() -> None:
            self._file_system_providers.pop(key, None)

        return Disposable(_dispose)

    def _find_files(self, include: Any, exclude: Any = None,
                    max_results: Any = None, token: Any = None) -> List[Uri]:
        root = self._get_root_path()
        if not root:
            return []
        include_pattern = _glob_pattern(include)
        exclude_pattern = _glob_pattern(exclude)
        try:
            limit = int(max_results) if max_results else 0
        except (TypeError, ValueError):
            limit = 0
        matches: List[Uri] = []
        for current, dirs, files in os.walk(root):
            dirs[:] = [d for d in dirs if d not in {".git", "__pycache__", "node_modules"}]
            for file_name in files:
                full = os.path.join(current, file_name)
                rel = os.path.relpath(full, root).replace("\\", "/")
                if include_pattern and not _glob_matches(rel, include_pattern):
                    continue
                if exclude_pattern and _glob_matches(rel, exclude_pattern):
                    continue
                matches.append(Uri.file(full))
                if limit and len(matches) >= limit:
                    return matches
        return matches

    # ── env ──

    def _build_env(self) -> Dict[str, Any]:
        return {
            "appName": "SAO AI Editor",
            "appRoot": self._get_root_path(),
            "language": "zh-cn",
            "uriScheme": "sao-editor",
            "clipboard": {"readText": self._read_clipboard,
                          "writeText": self._write_clipboard},
            "openExternal": self._open_external,
            "asExternalUri": lambda uri, *a, **kw: uri,
            "machineId": "sao-" + os.environ.get("COMPUTERNAME", "local"),
            "sessionId": "",
            "isNewAppInstall": False,
            "isTelemetryEnabled": False,
        }

    def _read_clipboard(self) -> str:
        return self._clipboard_text

    def _write_clipboard(self, text: str) -> None:
        self._clipboard_text = str(text or "")

    def _open_external(self, uri: Any) -> bool:
        """Open a URI in the system default browser / handler."""
        try:
            uri_str = str(uri)
            if hasattr(uri, "toString"):
                uri_str = uri.toString()
            elif hasattr(uri, "fsPath"):
                uri_str = uri.fsPath
            if os.name == "nt":
                os.startfile(uri_str)
            else:
                webbrowser.open(uri_str)
            return True
        except Exception:
            return False

    def _show_text_document(self, doc: Any, **kw: Any) -> "_TextEditor":
        document = doc if isinstance(doc, _TextDocument) else self._open_text_document(doc)
        document = self._remember_text_document(document)
        editor = _TextEditor(document)
        self._active_text_editor = editor
        self._visible_text_editors = [editor]
        self._sync_window_state()
        self._window_active_text_editor_emitter.fire(editor)
        self._window_visible_text_editors_emitter.fire(list(self._visible_text_editors))
        return editor

    def _build_languages(self) -> Dict[str, Any]:
        return {
            "createDiagnosticCollection": self._create_diagnostic_collection,
            "getDiagnostics": self._get_diagnostics,
            "getLanguages": self._get_languages,
            "match": self._language_match,
            "onDidChangeDiagnostics": self._diagnostics_change_emitter.event,
            "setLanguageConfiguration": self._set_language_configuration,
            "registerHoverProvider": lambda selector, provider: self._register_language_provider("hover", selector, provider),
            "registerCompletionItemProvider": lambda selector, provider, *trigger: self._register_language_provider("completion", selector, provider, trigger),
            "registerSignatureHelpProvider": lambda selector, provider, *metadata: self._register_language_provider("signatureHelp", selector, provider, self._signature_help_registration_metadata(metadata)),
            "registerDefinitionProvider": lambda selector, provider: self._register_language_provider("definition", selector, provider),
            "registerTypeDefinitionProvider": lambda selector, provider: self._register_language_provider("typeDefinition", selector, provider),
            "registerDeclarationProvider": lambda selector, provider: self._register_language_provider("declaration", selector, provider),
            "registerImplementationProvider": lambda selector, provider: self._register_language_provider("implementation", selector, provider),
            "registerReferenceProvider": lambda selector, provider: self._register_language_provider("references", selector, provider),
            "registerDocumentHighlightProvider": lambda selector, provider: self._register_language_provider("documentHighlight", selector, provider),
            "registerEvaluatableExpressionProvider": lambda selector, provider: self._register_language_provider("evaluatableExpression", selector, provider),
            "registerInlineValuesProvider": lambda selector, provider: self._register_language_provider("inlineValue", selector, provider),
            "registerRenameProvider": lambda selector, provider: self._register_language_provider("rename", selector, provider),
            "registerDocumentSymbolProvider": lambda selector, provider, metadata=None: self._register_language_provider("documentSymbol", selector, provider, metadata),
            "registerWorkspaceSymbolProvider": lambda provider: self._register_language_provider("workspaceSymbol", None, provider),
            "registerDocumentFormattingEditProvider": lambda selector, provider: self._register_language_provider("formatting", selector, provider),
            "registerDocumentRangeFormattingEditProvider": lambda selector, provider: self._register_language_provider("rangeFormatting", selector, provider),
            "registerOnTypeFormattingEditProvider": lambda selector, provider, first, *more: self._register_language_provider("onTypeFormatting", selector, provider, self._on_type_formatting_triggers(first, more)),
            "registerCodeActionsProvider": lambda selector, provider, metadata=None: self._register_language_provider("codeActions", selector, provider, metadata),
            "registerCodeLensProvider": lambda selector, provider: self._register_language_provider("codeLens", selector, provider),
            "registerDocumentLinkProvider": lambda selector, provider: self._register_language_provider("documentLink", selector, provider),
            "registerInlayHintsProvider": lambda selector, provider: self._register_language_provider("inlayHint", selector, provider),
            "registerInlineCompletionItemProvider": lambda selector, provider: self._register_language_provider("inlineCompletion", selector, provider),
            "registerFoldingRangeProvider": lambda selector, provider: self._register_language_provider("foldingRange", selector, provider),
            "registerSelectionRangeProvider": lambda selector, provider: self._register_language_provider("selectionRange", selector, provider),
            "registerLinkedEditingRangeProvider": lambda selector, provider: self._register_language_provider("linkedEditing", selector, provider),
            "registerCallHierarchyProvider": lambda selector, provider: self._register_language_provider("callHierarchy", selector, provider),
            "registerTypeHierarchyProvider": lambda selector, provider: self._register_language_provider("typeHierarchy", selector, provider),
            "registerDocumentDropEditProvider": lambda selector, provider, metadata=None: self._register_language_provider("documentDrop", selector, provider, metadata),
            "registerDocumentPasteEditProvider": lambda selector, provider, metadata: self._register_language_provider("documentPaste", selector, provider, metadata),
            "registerColorProvider": lambda selector, provider: self._register_language_provider("documentColor", selector, provider),
            "registerDocumentSemanticTokensProvider": lambda selector, provider, legend: self._register_language_provider("semanticTokens", selector, provider, legend),
            "registerDocumentRangeSemanticTokensProvider": lambda selector, provider, legend: self._register_language_provider("semanticTokensRange", selector, provider, legend),
            "setTextDocumentLanguage": lambda doc, language_id: _set_document_language(doc, language_id),
        }

    def _create_diagnostic_collection(self, name: str = "") -> Any:
        collection = _DiagnosticCollection(name, self._diagnostics_change_emitter)
        self._diagnostic_collections[name or "default"] = collection
        return collection

    def _get_diagnostics(self, resource: Any = None) -> Any:
        if resource is not None:
            result = []
            for collection in self._diagnostic_collections.values():
                result.extend(collection.get(resource))
            return result
        rows = []
        for collection in self._diagnostic_collections.values():
            rows.extend(collection.entries())
        return rows

    def _get_languages(self) -> List[str]:
        contributed = []
        try:
            for item in self._host.ext_points.all_contributions.get("languages", []):
                language_id = item.get("id")
                if language_id:
                    contributed.append(str(language_id))
        except Exception:
            pass
        builtins = [
            "plaintext", "python", "javascript", "typescript", "json", "html",
            "css", "markdown", "yaml", "xml", "sql", "shell", "lua", "c",
            "cpp", "csharp", "java", "go", "rust", "toml",
        ]
        configured = list(self._language_configurations.keys())
        return sorted(set(builtins + contributed + configured))

    def _set_language_configuration(
            self, language: Any, configuration: Any) -> Disposable:
        language_id = str(language or "").strip()
        if not language_id:
            return Disposable()
        payload = self._language_value_payload(configuration)
        if not isinstance(payload, dict):
            payload = {}
        entries = self._language_configurations.setdefault(language_id, [])
        entries.append(payload)

        def _dispose() -> None:
            current = self._language_configurations.get(language_id)
            if not current:
                return
            try:
                current.remove(payload)
            except ValueError:
                return
            if not current:
                self._language_configurations.pop(language_id, None)

        return Disposable(_dispose)

    def _language_match(self, selector: Any, document: Any) -> int:
        language_id = str(self._document_selector_attr(
            document, "languageId", "language") or "")
        uri = self._document_selector_attr(document, "uri")
        notebook_uri = self._document_selector_attr(
            document, "notebookUri", "notebook_uri")
        notebook_type = self._document_selector_attr(
            document, "notebookType", "notebook_type")
        scheme = str(getattr(uri, "scheme", "") or "file")
        if isinstance(selector, str):
            if selector == "*":
                return 5
            return 10 if selector == language_id else 0
        if isinstance(selector, list):
            return max((self._language_match(item, document) for item in selector), default=0)
        if isinstance(selector, dict):
            score = 0
            wanted_scheme = selector.get("scheme")
            if wanted_scheme:
                wanted_scheme = str(wanted_scheme)
                if wanted_scheme == scheme:
                    score = 10
                elif wanted_scheme == "*":
                    score = max(score, 5)
                else:
                    return 0
            wanted_language = selector.get("language")
            if wanted_language:
                wanted_language = str(wanted_language)
                if wanted_language == language_id:
                    score = 10
                elif wanted_language == "*":
                    score = max(score, 5)
                else:
                    return 0
            wanted_notebook = selector.get("notebookType")
            if wanted_notebook:
                wanted_notebook = str(wanted_notebook)
                if wanted_notebook == str(notebook_type or ""):
                    score = 10
                    if notebook_uri is not None:
                        uri = notebook_uri
                elif wanted_notebook == "*" and notebook_type is not None:
                    score = max(score, 5)
                    if notebook_uri is not None:
                        uri = notebook_uri
                else:
                    return 0
            pattern = selector.get("pattern")
            if pattern is None:
                pattern = selector.get("filenamePattern")
            if pattern:
                if self._document_pattern_match(pattern, uri):
                    score = 10
                else:
                    return 0
            return score
        return 0

    @staticmethod
    def _document_selector_attr(document: Any, *names: str) -> Any:
        if document is None:
            return None
        if isinstance(document, dict):
            for name in names:
                if name in document:
                    return document.get(name)
            notebook = document.get("notebook")
        else:
            notebook = None
        for name in names:
            if hasattr(document, name):
                return getattr(document, name)
        if notebook is None and hasattr(document, "notebook"):
            notebook = getattr(document, "notebook")
        if any(name in {"notebookUri", "notebook_uri"} for name in names):
            return VscodeNamespace._document_selector_attr(
                notebook, "uri", "notebookUri", "notebook_uri")
        if any(name in {"notebookType", "notebook_type"} for name in names):
            return VscodeNamespace._document_selector_attr(
                notebook, "notebookType", "notebook_type", "type")
        return None

    @staticmethod
    def _document_pattern_match(pattern: Any, uri: Any) -> bool:
        if uri is None:
            return False
        base = None
        if isinstance(pattern, dict):
            pattern_text = str(pattern.get("pattern") or "")
            base = (
                pattern.get("base")
                or pattern.get("baseUri")
                or pattern.get("uri"))
        else:
            pattern_text = str(pattern or "")
        if not pattern_text:
            return False
        path_text = str(getattr(uri, "fs_path", "") or "").replace("\\", "/")
        uri_path = str(getattr(uri, "path", "") or "").replace("\\", "/")
        fallback_path = VscodeNamespace._selector_path_text(uri)
        candidates = {
            path_text,
            uri_path,
            fallback_path,
            os.path.basename(uri_path or fallback_path),
        }
        base_path = VscodeNamespace._selector_path_text(base)
        if base_path and path_text:
            try:
                rel = os.path.relpath(
                    path_text.replace("/", os.sep),
                    base_path.replace("/", os.sep)).replace("\\", "/")
                if rel and not rel.startswith("../") and rel != "..":
                    candidates.add(rel)
            except Exception:
                pass
        candidates = {item for item in candidates if item}
        patterns = VscodeNamespace._expand_glob_braces(
            pattern_text.replace("\\", "/"))
        for candidate in candidates:
            if candidate in patterns:
                return True
            for item in patterns:
                if fnmatch.fnmatchcase(candidate, item):
                    return True
        return False

    @staticmethod
    def _selector_path_text(value: Any) -> str:
        if value is None:
            return ""
        if isinstance(value, Uri):
            return str(value.fs_path if value.scheme == "file" else value.path).replace("\\", "/")
        if isinstance(value, dict):
            if value.get("fsPath"):
                return str(value.get("fsPath")).replace("\\", "/")
            if value.get("path"):
                return str(value.get("path")).replace("\\", "/")
            if value.get("uri"):
                return VscodeNamespace._selector_path_text(value.get("uri"))
        return str(value).replace("\\", "/")

    @staticmethod
    def _expand_glob_braces(pattern: str) -> List[str]:
        match = re.search(r"\{([^{}]+)\}", pattern)
        if not match:
            return [pattern]
        before, after = pattern[:match.start()], pattern[match.end():]
        expanded: List[str] = []
        for option in match.group(1).split(","):
            expanded.extend(
                VscodeNamespace._expand_glob_braces(
                    before + option.strip() + after))
        return expanded

    @staticmethod
    def _signature_help_registration_metadata(values: Sequence[Any]) -> Any:
        if len(values) == 1 and isinstance(values[0], dict):
            return {
                "triggerCharacters": list(values[0].get("triggerCharacters") or []),
                "retriggerCharacters": list(values[0].get("retriggerCharacters") or []),
            }
        return tuple(str(item) for item in values)

    @staticmethod
    def _on_type_formatting_triggers(first: Any,
                                     more: Sequence[Any]) -> Sequence[str]:
        return tuple(
            str(item)
            for item in (first, *more)
            if item is not None
        )

    def _register_language_provider(self, kind: str, selector: Any,
                                    provider: Any, metadata: Any = None) -> Disposable:
        provider_id = self._language_provider_id(
            kind, selector, provider, metadata)
        entry = {"kind": kind, "selector": selector,
                 "provider": provider, "metadata": metadata,
                 "providerId": provider_id,
                 "id": provider_id,
                 "extensionId": provider_id,
                 "displayName": self._language_provider_display_name(
                     kind, provider, metadata, provider_id)}
        self._language_providers.setdefault(kind, []).append(entry)
        return Disposable(lambda: self._language_providers.get(kind, []).remove(entry)
                          if entry in self._language_providers.get(kind, []) else None)

    def _build_tasks(self) -> Dict[str, Any]:
        if self._tasks_api is None:
            self._tasks_api = {
                "registerTaskProvider": self._register_task_provider,
                "fetchTasks": self._fetch_tasks,
                "executeTask": self._execute_task,
                "taskExecutions": [],
                "taskDefinitions": [],
                "onDidStartTask": self._tasks_start_emitter.event,
                "onDidEndTask": self._tasks_end_emitter.event,
            }
        self._sync_tasks_state()
        return self._tasks_api

    def _register_task_provider(self, task_type: str, provider: Any) -> Disposable:
        self._task_providers[task_type] = provider
        return Disposable(lambda: self._task_providers.pop(task_type, None))

    def _sync_tasks_state(self) -> None:
        if self._tasks_api is None:
            return
        self._tasks_api["taskExecutions"] = list(self._task_executions)
        self._tasks_api["taskDefinitions"] = list(
            self._host.ext_points.all_contributions.get("taskDefinitions", []))

    def _fetch_tasks(self, filter: Any = None) -> List[Any]:
        tasks: List[Any] = []
        for task_type, provider in list(self._task_providers.items()):
            provided = self._call_provider_tasks(provider)
            for task in provided:
                normalized = self._normalize_task(task, task_type)
                if self._task_matches_filter(normalized, filter):
                    tasks.append(normalized)
        return tasks

    @staticmethod
    def _call_provider_tasks(provider: Any) -> List[Any]:
        if hasattr(provider, "provideTasks"):
            result = provider.provideTasks()
        elif hasattr(provider, "provide_tasks"):
            result = provider.provide_tasks()
        elif callable(provider):
            result = provider()
        else:
            result = []
        return list(result or [])

    @staticmethod
    def _normalize_task(task: Any, task_type: str = "") -> Any:
        if not isinstance(task, dict):
            return task
        normalized = dict(task)
        if task_type and not normalized.get("type"):
            normalized["type"] = task_type
        definition = normalized.get("definition")
        if isinstance(definition, dict) and definition.get("type") and not normalized.get("type"):
            normalized["type"] = definition.get("type")
        return normalized

    @staticmethod
    def _task_matches_filter(task: Any, filter_value: Any) -> bool:
        if not filter_value or not isinstance(task, dict):
            return True
        if isinstance(filter_value, dict):
            task_type = str(task.get("type") or "")
            filter_type = str(filter_value.get("type") or "")
            if filter_type and task_type != filter_type:
                return False
        return True

    def _resolve_task(self, task: Any) -> Any:
        if not isinstance(task, dict):
            return task
        task_type = str(task.get("type") or task.get("definition", {}).get("type") or "")
        provider = self._task_providers.get(task_type)
        if provider and hasattr(provider, "resolveTask"):
            resolved = provider.resolveTask(task)
            if resolved is not None:
                return self._normalize_task(resolved, task_type)
        if provider and hasattr(provider, "resolve_task"):
            resolved = provider.resolve_task(task)
            if resolved is not None:
                return self._normalize_task(resolved, task_type)
        return task

    def _execute_task(self, task: Any) -> Any:
        resolved = self._resolve_task(task)
        if isinstance(resolved, dict) and callable(resolved.get("run")):
            execution = _TaskExecution(
                task=resolved,
                name=str(resolved.get("name") or resolved.get("label") or "task"),
                kind="callable",
            )
            self._task_executions.append(execution)
            self._sync_tasks_state()
            self._tasks_start_emitter.fire({"execution": execution, "task": resolved})
            execution.start_callable(
                resolved["run"],
                on_finish=lambda e=execution: self._on_task_finished(e),
            )
            return execution

        spec = self._task_command_spec(resolved)
        if not spec:
            return {
                "ok": False,
                "error": "Task has no runnable local command. Provide command/args, execution, or run().",
            }
        process = self._spawn_local_process(**spec)
        execution = _TaskExecution(
            task=resolved,
            name=str(spec.get("name") or "task"),
            kind=str(spec.get("kind") or "process"),
            process=process,
        )
        self._task_executions.append(execution)
        self._sync_tasks_state()
        self._tasks_start_emitter.fire({"execution": execution, "task": resolved})
        execution.start_waiter(on_finish=lambda e=execution: self._on_task_finished(e))
        return execution

    def _on_task_finished(self, execution: "_TaskExecution") -> None:
        if execution in self._task_executions:
            self._task_executions.remove(execution)
        self._sync_tasks_state()
        self._tasks_end_emitter.fire({"execution": execution, "task": execution.task})

    def _task_command_spec(self, task: Any) -> Dict[str, Any]:
        if not isinstance(task, dict):
            return {}
        execution = task.get("execution")
        if isinstance(execution, dict):
            command = execution.get("command") or execution.get("process")
            args = execution.get("args") or []
            shell = bool(execution.get("shell", execution.get("type") == "shell"))
            cwd = execution.get("cwd") or task.get("cwd")
        else:
            shell_command = task.get("shellExecution")
            command = task.get("command") or task.get("process") or shell_command
            args = task.get("args") or []
            if "shell" in task:
                shell = bool(task.get("shell"))
            else:
                shell = bool(shell_command)
            cwd = task.get("cwd") or task.get("options", {}).get("cwd")
        if not command:
            return {}
        name = str(task.get("name") or task.get("label") or command)
        return {
            "command": command,
            "args": args,
            "shell": shell,
            "cwd": cwd or self._get_root_path() or None,
            "name": name,
            "kind": "shell" if shell else "process",
        }

    def _build_debug(self) -> Dict[str, Any]:
        if self._debug_api is None:
            self._debug_api = {
                "registerDebugConfigurationProvider": self._register_debug_provider,
                "startDebugging": self._start_debugging,
                "activeDebugSession": None,
                "breakpoints": [],
                "debuggers": [],
                "onDidStartDebugSession": self._debug_start_emitter.event,
                "onDidTerminateDebugSession": self._debug_terminate_emitter.event,
                "onDidChangeBreakpoints": self._debug_breakpoints_emitter.event,
            }
        self._sync_debug_state()
        return self._debug_api

    def _register_debug_provider(self, debug_type: str, provider: Any,
                                 trigger_kind: Any = None) -> Disposable:
        self._debug_providers[debug_type] = {
            "provider": provider, "triggerKind": trigger_kind,
        }
        return Disposable(lambda: self._debug_providers.pop(debug_type, None))

    def _sync_debug_state(self) -> None:
        if self._debug_api is None:
            return
        active = None
        for session in reversed(self._debug_sessions):
            if not session.terminated:
                active = session
                break
        self._debug_api["activeDebugSession"] = active
        self._debug_api["debuggers"] = list(
            self._host.ext_points.all_contributions.get("debuggers", []))

    def _start_debugging(self, folder: Any, name_or_config: Any,
                         parent: Any = None) -> bool:
        config = self._resolve_debug_configuration(folder, name_or_config)
        if not isinstance(config, dict):
            return False
        debug_type = str(config.get("type") or "")
        provider_info = self._debug_providers.get(debug_type, {})
        provider = provider_info.get("provider")
        if provider and hasattr(provider, "resolveDebugConfiguration"):
            resolved = provider.resolveDebugConfiguration(folder, config)
            if resolved is None:
                return False
            config = resolved
        elif provider and hasattr(provider, "resolve_debug_configuration"):
            resolved = provider.resolve_debug_configuration(folder, config)
            if resolved is None:
                return False
            config = resolved
        spec = self._debug_command_spec(config)
        if not spec:
            return False
        process = self._spawn_local_process(**spec)
        session = _DebugSession(
            name=str(config.get("name") or spec.get("name") or "debug"),
            debug_type=str(config.get("type") or "local"),
            configuration=dict(config),
            process=process,
            parent_session=parent,
        )
        self._debug_sessions.append(session)
        self._sync_debug_state()
        self._debug_start_emitter.fire(session)
        session.start_waiter(on_finish=lambda s=session: self._on_debug_finished(s))
        return True

    def _on_debug_finished(self, session: "_DebugSession") -> None:
        self._sync_debug_state()
        self._debug_terminate_emitter.fire(session)

    def _resolve_debug_configuration(self, folder: Any,
                                     name_or_config: Any) -> Any:
        if isinstance(name_or_config, dict):
            return dict(name_or_config)
        if isinstance(folder, dict):
            for config in folder.get("configurations", []) or []:
                if isinstance(config, dict) and config.get("name") == name_or_config:
                    return dict(config)
        return None

    def _debug_command_spec(self, config: Dict[str, Any]) -> Dict[str, Any]:
        if not isinstance(config, dict):
            return {}
        if config.get("command"):
            return {
                "command": config.get("command"),
                "args": config.get("args") or [],
                "shell": not isinstance(config.get("command"), list),
                "cwd": config.get("cwd") or self._get_root_path() or None,
                "name": str(config.get("name") or config.get("command")),
                "kind": "debug-command",
            }
        if config.get("program"):
            debug_type = str(config.get("type") or "")
            args = list(config.get("args") or [])
            if debug_type == "python":
                command = config.get("python") or config.get("pythonPath") or os.environ.get("PYTHON", "python")
                return {
                    "command": command,
                    "args": [config.get("program")] + args,
                    "shell": False,
                    "cwd": config.get("cwd") or self._get_root_path() or None,
                    "name": str(config.get("name") or config.get("program")),
                    "kind": "debug-python",
                }
            return {
                "command": config.get("program"),
                "args": args,
                "shell": False,
                "cwd": config.get("cwd") or self._get_root_path() or None,
                "name": str(config.get("name") or config.get("program")),
                "kind": "debug-program",
            }
        return {}

    @staticmethod
    def _spawn_local_process(command: Any, args: Any = None,
                             shell: bool = True, cwd: Optional[str] = None,
                             name: str = "", kind: str = "process") -> subprocess.Popen:
        arg_list = [str(item) for item in list(args or [])]
        if shell:
            if isinstance(command, list):
                cmd_value = " ".join(str(item) for item in command + arg_list)
            else:
                cmd_value = " ".join([str(command)] + arg_list).strip()
            return subprocess.Popen(
                cmd_value,
                cwd=cwd or None,
                shell=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        command_value = command if isinstance(command, list) else [str(command)]
        if not isinstance(command_value, list):
            command_value = [str(command_value)]
        return subprocess.Popen(
            [str(item) for item in command_value] + arg_list,
            cwd=cwd or None,
            shell=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    def _build_notebooks(self) -> Dict[str, Any]:
        return {"registerNotebookSerializer": lambda *a, **kw: Disposable()}

    # ── extensions ──

    def _build_extensions(self) -> Dict[str, Any]:
        if self._extensions_api is None:
            self._extensions_api = {
                "getExtension": self._get_extension,
                "all": [],
                "onDidChange": self._extensions_change_emitter.event,
            }
        self._sync_extensions_state()
        return self._extensions_api

    def _sync_extensions_state(self) -> None:
        if self._extensions_api is None:
            return
        self._extensions_api["all"] = list(self._host.registry.list_all())

    def _get_extension(self, ext_id: str) -> Any:
        return self._host.registry.get(ext_id)

    # ── lm (Language Model) ──

    def _build_lm(self, extension_id: str = "") -> Dict[str, Any]:
        return {
            "selectChatModels": self._select_chat_models,
            "registerTool": (
                lambda name, tool: self._register_lm_tool(
                    name, tool, extension_id=extension_id)),
            "registerToolDefinition": (
                lambda name, schema: self._register_tool_definition(
                    name, schema, extension_id=extension_id)),
            "registerLanguageModelChatProvider": self._register_lm_provider,
            "registerMcpServerDefinitionProvider": self._register_mcp_provider,
            "fileIsIgnored": self._file_is_ignored,
            "registerIgnoredFileProvider": self._register_ignored_file_provider,
            "invokeTool": self._invoke_tool,
            "getTools": self._get_tools_list,
            "onDidChangeChatModels": self._models_change_emitter.event,
            "onDidChangeTools": self._tools_change_emitter.event,
            "mcpServerDefinitionProviders": self._mcp_definition_providers,
            "tools": self._lm_tools,
        }

    def _get_tools_list(self) -> List[Dict[str, Any]]:
        """Return tool descriptors (matching VSCode lm.tools shape)."""
        result = []
        for name, tool in self._lm_tools.items():
            desc: Dict[str, Any] = {"name": name}
            if isinstance(tool, dict):
                nested = tool.get("tool")
                extension_id = tool.get("_extensionId") or tool.get("extensionId")
                if extension_id:
                    desc["extensionId"] = extension_id
                desc["description"] = (
                    tool.get("description", "")
                    or getattr(nested, "description", "")
                )
                desc["inputSchema"] = _tool_schema(tool)
                desc["tags"] = tool.get("tags") or getattr(nested, "tags", [])
                if tool.get("needsExtensionRuntime"):
                    desc["unsupported"] = True
                    desc["needsExtensionRuntime"] = True
                    desc["code"] = "needsExtensionRuntime"
            elif hasattr(tool, "description"):
                desc["description"] = getattr(tool, "description", "")
                desc["inputSchema"] = _tool_schema(tool)
                desc["tags"] = getattr(tool, "tags", [])
            result.append(desc)
        return result

    def _select_chat_models(self, selector: Dict = None) -> List[LanguageModelChat]:
        models = []
        if self._engine:
            from ai_editor.llm_engine import get_model_context, get_model_capabilities
            cfg = self._engine.config
            model_name = cfg.effective_model
            ctx = get_model_context(model_name)
            caps = get_model_capabilities(model_name)
            lm = LanguageModelChat(
                id=model_name, name=model_name,
                vendor=cfg.provider, family=cfg.provider,
                version="1", max_input_tokens=ctx["max_input"],
                capabilities=_LMCapabilities(
                    supports_image_to_text=caps.get("vision", False),
                    supports_tool_calling=caps.get("tools", True),
                ),
            )
            lm._engine = self._engine
            if selector:
                if selector.get("vendor") and selector["vendor"] != cfg.provider:
                    return []
                if selector.get("family") and selector["family"] not in model_name:
                    return []
                if selector.get("id") and selector["id"] != model_name:
                    return []
            models.append(lm)
        for pid, pinfo in self._lm_providers.items():
            meta = pinfo.get("metadata", {})
            lm = LanguageModelChat(
                id=pid, name=meta.get("name", pid),
                vendor=meta.get("vendor", "extension"),
                family=meta.get("family", ""),
                version=meta.get("version", "1"),
                max_input_tokens=meta.get("maxInputTokens", 128000),
            )
            if selector:
                if selector.get("vendor") and selector["vendor"] != lm.vendor:
                    continue
                if selector.get("id") and selector["id"] != lm.id:
                    continue
            models.append(lm)
        return models

    def _register_lm_tool(self, name: str, tool: Any,
                          extension_id: str = "") -> Disposable:
        record = self._merge_registered_tool(self._lm_tools.get(name), tool)
        if extension_id and not record.get("_extensionId"):
            record["_extensionId"] = extension_id
        self._lm_tools[name] = record
        self._tools_change_emitter.fire({"added": name})
        def _dispose():
            self._lm_tools.pop(name, None)
            self._tools_change_emitter.fire({"removed": name})
        return Disposable(_dispose)

    def _register_tool_definition(self, name: str, schema: Dict,
                                  extension_id: str = "") -> Disposable:
        """Register a schema-only tool definition without pretending it can run."""
        record = self._merge_registered_tool(self._lm_tools.get(name), {
            "schema": schema or {},
            "inputSchema": schema or {},
            "description": "Schema-only tool definition has no invoke handler.",
        })
        if extension_id and not record.get("_extensionId"):
            record["_extensionId"] = extension_id
        if not self._tool_has_runtime_handler(record):
            record["unsupported"] = True
            record["needsExtensionRuntime"] = True
            record["code"] = "needsExtensionRuntime"
        self._lm_tools[name] = record
        self._tools_change_emitter.fire({"added": name})
        def _dispose() -> None:
            self._lm_tools.pop(name, None)
            self._tools_change_emitter.fire({"removed": name})
        return Disposable(_dispose)

    @staticmethod
    def _tool_has_runtime_handler(tool: Any) -> bool:
        if isinstance(tool, dict):
            handler = tool.get("invoke") or tool.get("handler") or tool.get("callback")
            if callable(handler):
                return True
            nested = tool.get("tool")
            return hasattr(nested, "invoke") or callable(nested)
        return hasattr(tool, "invoke") or callable(tool)

    @staticmethod
    def _merge_registered_tool(existing: Any, tool: Any) -> Dict[str, Any]:
        record: Dict[str, Any] = {}

        def _overlay(value: Any) -> None:
            if value is None:
                return
            if isinstance(value, dict):
                record.update(value)
                schema = value.get("inputSchema") or value.get("schema")
                if schema:
                    record["inputSchema"] = schema
                return
            if callable(value) and not hasattr(value, "invoke"):
                record["handler"] = value
            else:
                record["tool"] = value
            desc = getattr(value, "description", "")
            if desc and not record.get("description"):
                record["description"] = desc
            schema = _tool_schema(value)
            if schema:
                record["inputSchema"] = schema
            tags = getattr(value, "tags", None)
            if tags and not record.get("tags"):
                record["tags"] = tags

        _overlay(existing)
        _overlay(tool)
        if VscodeNamespace._tool_has_runtime_handler(record):
            record.pop("unsupported", None)
            record.pop("needsExtensionRuntime", None)
            record.pop("code", None)
        return record

    def _register_mcp_provider(self, provider_id: str, provider: Any) -> Disposable:
        """Register an MCP server definition provider (extension-contributed MCP)."""
        self._mcp_definition_providers[provider_id] = provider

        def _dispose() -> None:
            self._mcp_definition_providers.pop(provider_id, None)

        return Disposable(_dispose)

    def _file_is_ignored(self, uri: Any, token: Any = None) -> bool:
        """Check if a file should be ignored (.gitignore/.copilotignore)."""
        path = uri.fs_path if hasattr(uri, "fs_path") else str(uri)
        # Use path-component matching instead of substring to avoid false
        # positives (e.g. 'my.environment.py' matching '.env').
        parts = set(path.replace("\\", "/").split("/"))
        ignore_patterns = [".git", "__pycache__", "node_modules", ".env"]
        for pat in ignore_patterns:
            if pat in parts:
                return True
        # Consult extension-registered ignore providers
        for prov in list(self._ignored_file_providers):
            try:
                if hasattr(prov, "isFileIgnored") and prov.isFileIgnored(uri):
                    return True
                elif callable(prov) and prov(uri):
                    return True
            except Exception:
                pass
        return False

    def _register_ignored_file_provider(self, provider: Any) -> Disposable:
        """Register a file-ignore provider."""
        self._ignored_file_providers.append(provider)
        def _dispose():
            try:
                self._ignored_file_providers.remove(provider)
            except ValueError:
                pass
        return Disposable(_dispose)

    def _register_lm_provider(self, provider_id: str, provider: Any,
                               metadata: Dict = None) -> Disposable:
        """Register a language model chat provider (extension-contributed model)."""
        self._lm_providers[provider_id] = {
            "provider": provider, "metadata": metadata or {}}
        self._models_change_emitter.fire({"added": provider_id})
        def _dispose():
            self._lm_providers.pop(provider_id, None)
            self._models_change_emitter.fire({"removed": provider_id})
        return Disposable(_dispose)

    def _invoke_tool(self, name: str, input_data: Any = None,
                      token: Any = None) -> Any:
        tool = self._lm_tools.get(name)
        if not tool:
            raise KeyError(f"Tool not found: {name}")
        options = _coerce_tool_options(input_data, token)
        schema = _tool_schema(tool)
        if schema and options.input is not None:
            self._validate_input_schema(name, options.input, schema)
        if isinstance(tool, dict):
            handler = tool.get("invoke") or tool.get("handler") or tool.get("callback")
            if callable(handler):
                return _call_registered_handler(handler, options, options.token)
            nested = tool.get("tool")
            if nested is None:
                if tool.get("needsExtensionRuntime") or tool.get("unsupported"):
                    return _unsupported_tool_result(
                        name, "is schema-only and has no registered invoke handler.")
                return _unsupported_tool_result(
                    name, "is registered without an invoke handler.")
            tool = nested
        # prepareInvocation hook
        if hasattr(tool, "prepareInvocation"):
            try:
                prep = tool.prepareInvocation(
                    options, options.token)
                if isinstance(prep, PreparedToolInvocation) and prep.confirmation_messages:
                    if self._ui_bridge is None:
                        return _tool_confirmation_denied_result(name)
                    confirmed = self._ui_bridge.confirm_tool_invocation(
                        name, prep.confirmation_messages, options.input)
                    if not confirmed:
                        return _tool_confirmation_denied_result(name)
            except Exception:
                return _tool_confirmation_denied_result(name)
        if hasattr(tool, "invoke"):
            return tool.invoke(options, options.token)
        if callable(tool):
            return _call_registered_handler(tool, options, options.token)
        return _unsupported_tool_result(
            name, "is registered without an invoke handler.")

    @staticmethod
    def _validate_input_schema(tool_name: str, input_data: Any,
                               schema: Dict) -> None:
        """Basic type validation of input against a JSON-Schema-like dict.

        Checks top-level ``type`` and ``required`` fields.  This is intentionally
        lightweight -- not a full JSON Schema validator.
        """
        schema_type = schema.get("type")
        if schema_type == "object":
            if not isinstance(input_data, dict):
                raise TypeError(
                    f"Tool '{tool_name}' expects object input, "
                    f"got {type(input_data).__name__}")
            required = schema.get("required", [])
            props = schema.get("properties", {})
            for key in required:
                if key not in input_data:
                    raise ValueError(
                        f"Tool '{tool_name}' missing required field: {key}")
            for key, val in input_data.items():
                if key in props and val is not None:
                    expected = props[key].get("type")
                    if expected and not _check_json_type(val, expected):
                        raise TypeError(
                            f"Tool '{tool_name}' field '{key}': "
                            f"expected {expected}, got {type(val).__name__}")

    # ── chat ──

    def _build_chat(self) -> Dict[str, Any]:
        return {
            "createChatParticipant": self._create_chat_participant,
            "registerVariable": self._register_variable,
            "registerChatWorkspaceContextProvider": (
                lambda provider_id, provider: self._register_chat_context_provider(
                    "workspace", provider_id, provider)),
            "registerChatExplicitContextProvider": (
                lambda provider_id, provider: self._register_chat_context_provider(
                    "explicit", provider_id, provider)),
            "registerChatResourceContextProvider": (
                lambda provider_id, provider: self._register_chat_context_provider(
                    "resource", provider_id, provider)),
            "contextProviders": self._chat_context_providers,
        }

    def _create_chat_participant(self, participant_id: str,
                                  handler: Callable) -> ChatParticipant:
        cp = ChatParticipant(
            participant_id,
            handler,
            on_dispose=lambda pid=participant_id: self._chat_participants.pop(pid, None),
        )
        self._chat_participants[participant_id] = cp
        return cp

    def _register_variable(self, name: str, description: str,
                            resolver: Callable) -> Disposable:
        self._variables[name] = resolver
        return Disposable(lambda: self._variables.pop(name, None))

    def _register_chat_context_provider(self, kind: str,
                                        provider_id: str,
                                        provider: Any) -> Disposable:
        bucket = self._chat_context_providers.setdefault(kind, {})
        bucket[provider_id] = provider

        def _dispose() -> None:
            bucket.pop(provider_id, None)

        return Disposable(_dispose)

    # ── authentication ──

    def _build_auth(self) -> Dict[str, Any]:
        if self._auth_api is None:
            self._auth_api = {
                "getSession": self._get_auth_session,
                "registerAuthenticationProvider": self._register_auth_provider,
                "onDidChangeSessions": self._auth_sessions_emitter.event,
            }
        return self._auth_api

    def _get_auth_session(self, provider_id: str, scopes: List[str] = None,
                           options: Dict = None) -> Optional[AuthenticationSession]:
        sessions = self._auth_sessions.get(provider_id, [])
        if sessions:
            return sessions[0]
        provider_sessions = self._refresh_auth_provider_sessions(
            provider_id, scopes or [], options or {}, emit_change=False)
        if provider_sessions:
            return provider_sessions[0]
        if options and options.get("createIfNone"):
            prov = self._auth_providers.get(provider_id)
            if prov and (hasattr(prov, "create_session") or hasattr(prov, "createSession")):
                session = self._call_provider_method(
                    prov, ("create_session", "createSession"),
                    scopes or [], options)
                if session:
                    auth_session = _coerce_auth_session(
                        session, provider_id, scopes or [])
                    self._auth_sessions.setdefault(provider_id, []).append(auth_session)
                    self._auth_sessions_emitter.fire({
                        "provider": provider_id,
                        "added": [auth_session.id],
                        "removed": [],
                        "changed": [],
                    })
                    return auth_session
        try:
            from ai_editor.auth import get_auth_service
            session = get_auth_service().get_session(
                provider_id, scopes or [], options or {})
            if session:
                return _coerce_auth_session(session, provider_id, scopes or [])
        except Exception:
            pass
        return None

    def _register_auth_provider(self, provider_id: str, label: str,
                                 provider: Any, options: Dict = None) -> Disposable:
        self._auth_providers[provider_id] = provider
        self._refresh_auth_provider_sessions(provider_id, [], options or {}, emit_change=False)
        session_event = (getattr(provider, "on_did_change_sessions", None)
                         or getattr(provider, "onDidChangeSessions", None))
        if callable(session_event):
            try:
                disposable = session_event(
                    lambda evt=None, pid=provider_id: self._handle_auth_provider_change(pid, evt))
                self._auth_provider_listeners[provider_id] = disposable
            except Exception:
                pass
        external_disposable = None
        try:
            from ai_editor.auth import get_auth_service
            external_disposable = get_auth_service().register_provider(
                provider_id, label, provider)
        except Exception:
            external_disposable = None

        def _dispose() -> None:
            self._auth_providers.pop(provider_id, None)
            listener = self._auth_provider_listeners.pop(provider_id, None)
            if listener and hasattr(listener, "dispose"):
                listener.dispose()
            self._auth_sessions.pop(provider_id, None)
            if external_disposable:
                external_disposable.dispose()
        return Disposable(_dispose)

    def _handle_auth_provider_change(self, provider_id: str,
                                     event: Any = None) -> None:
        self._refresh_auth_provider_sessions(provider_id, [], {}, emit_change=True,
                                             preferred_event=event)

    def _refresh_auth_provider_sessions(self, provider_id: str,
                                        scopes: List[str],
                                        options: Dict[str, Any],
                                        emit_change: bool,
                                        preferred_event: Any = None
                                        ) -> List[AuthenticationSession]:
        provider = self._auth_providers.get(provider_id)
        if provider is None:
            return []
        if not (hasattr(provider, "get_sessions") or hasattr(provider, "getSessions")):
            return list(self._auth_sessions.get(provider_id, []))
        try:
            raw_sessions = self._call_provider_method(
                provider, ("get_sessions", "getSessions"),
                scopes or [], options or {}) or []
        except Exception:
            return list(self._auth_sessions.get(provider_id, []))
        sessions = [_coerce_auth_session(item, provider_id, scopes or [])
                    for item in list(raw_sessions)]
        previous = list(self._auth_sessions.get(provider_id, []))
        self._auth_sessions[provider_id] = sessions
        if emit_change:
            payload = preferred_event if isinstance(preferred_event, dict) else None
            if payload is None:
                previous_ids = {item.id for item in previous}
                current_ids = {item.id for item in sessions}
                payload = {
                    "provider": provider_id,
                    "added": sorted(current_ids - previous_ids),
                    "removed": sorted(previous_ids - current_ids),
                    "changed": sorted(previous_ids & current_ids),
                }
            payload.setdefault("provider", provider_id)
            self._auth_sessions_emitter.fire(payload)
        return sessions

    # ── Webview message routing (frontend -> extension) ──

    def deliver_webview_message(self, view_id: str, message: Any) -> bool:
        """Route a message from the webview HTML back to extension listeners.

        Called by the app layer when the frontend iframe executes
        ``acquireVsCodeApi().postMessage(data)``.  Finds the matching
        ``_WebviewView`` or ``_WebviewPanel`` and fires its
        ``onDidReceiveMessage`` emitter so extension code runs.
        """
        view = self._webview_views.get(view_id)
        if view is not None:
            view.webview.receive_message_from_webview(message)
            return True
        panels = self._webview_panels.get(view_id, [])
        if panels:
            panels[-1].webview.receive_message_from_webview(message)
            return True
        return False

    def get_webview_html(self, view_id: str) -> Optional[str]:
        """Return the current HTML for a webview view, or None."""
        view = self._webview_views.get(view_id)
        if view is not None:
            return view.webview.html or None
        panels = self._webview_panels.get(view_id, [])
        if panels:
            return panels[-1].webview.html or None
        return None

    def list_webview_view_ids(self) -> List[str]:
        """Return all registered webview view IDs."""
        return sorted(set(self._webview_views.keys()) | set(self._webview_panels.keys()))

    def list_webview_views(self) -> List[Dict[str, Any]]:
        """Return runtime webview registrations with provider metadata."""
        rows: List[Dict[str, Any]] = []
        for view_id in sorted(self._webview_views.keys()):
            view = self._webview_views.get(view_id)
            entry = self._webview_view_providers.get(view_id, {})
            rows.append({
                "view_id": view_id,
                "kind": "webviewView",
                "extension_id": str(entry.get("_extensionId") or ""),
                "options": dict(entry.get("options") or {}),
                "title": str(getattr(view, "title", "") or view_id),
                "visible": bool(getattr(view, "visible", False)),
                "html_available": bool(getattr(getattr(view, "webview", None), "html", "")),
            })
        for view_type in sorted(self._webview_panels.keys()):
            panels = self._webview_panels.get(view_type, [])
            if not panels:
                continue
            panel = panels[-1]
            rows.append({
                "view_id": view_type,
                "kind": "webviewPanel",
                "extension_id": "",
                "options": dict(getattr(panel, "options", {}) or {}),
                "title": str(getattr(panel, "title", "") or view_type),
                "visible": bool(getattr(panel, "visible", False)),
                "html_available": bool(getattr(getattr(panel, "webview", None), "html", "")),
            })
        return rows

    # ── Accessors for host integration ──

    @property
    def chat_participants(self) -> Dict[str, ChatParticipant]:
        return dict(self._chat_participants)

    @property
    def registered_tools(self) -> Dict[str, Any]:
        return dict(self._lm_tools)

    @property
    def variables(self) -> Dict[str, Callable]:
        return dict(self._variables)


# ---------------------------------------------------------------------------
# Helper compatibility classes
# ---------------------------------------------------------------------------

def _check_json_type(value: Any, expected: str) -> bool:
    """Check if a Python value matches a JSON Schema type string."""
    _MAP = {
        "string": str,
        "integer": int,
        "number": (int, float),
        "boolean": bool,
        "array": list,
        "object": dict,
    }
    py_type = _MAP.get(expected)
    if py_type is None:
        return True  # unknown type, pass
    if expected == "integer" and isinstance(value, bool):
        return False  # bool is subclass of int in Python
    return isinstance(value, py_type)


def _lm_message(role: int, content: str) -> Dict:
    """Legacy helper — use LanguageModelChatMessage class instead."""
    roles = {0: "system", 1: "user", 2: "assistant"}
    return {"role": roles.get(role, "user"), "content": content}


class _OutputChannel:
    def __init__(self, name: str = "",
                 bridge: Optional[UIBridge] = None) -> None:
        self.name = name
        self._value = ""
        self._bridge = bridge

    def append(self, value: str) -> None:
        self._value += str(value)

    def appendLine(self, value: str) -> None:
        self._value += str(value) + "\n"
        print(f"[{self.name}] {value}")

    def clear(self) -> None:
        self._value = ""
        if self._bridge is not None:
            try:
                self._bridge.clear_output(self.name)
            except Exception:
                pass

    def show(self, **kw) -> None:
        if self._bridge is not None:
            try:
                self._bridge.show_output(self.name, self._value)
            except Exception:
                pass

    def dispose(self) -> None:
        self._value = ""
        if self._bridge is not None:
            try:
                self._bridge.dispose_output(self.name)
            except Exception:
                pass


class _StatusBarItem:
    _counter = 0

    def __init__(self, alignment: int = 2, priority: int = 0,
                 bridge: Optional[UIBridge] = None) -> None:
        _StatusBarItem._counter += 1
        self._id = f"sbi-{_StatusBarItem._counter}"
        self.alignment = alignment
        self.priority = priority
        self.text = ""
        self.tooltip = ""
        self.command = ""
        self.color = ""
        self.backgroundColor = ""
        self.visible = False
        self._bridge = bridge

    def show(self) -> None:
        self.visible = True
        if self._bridge is not None:
            try:
                self._bridge.show_status_bar_item(
                    self._id, self.text, self.tooltip, self.command,
                    self.alignment, self.priority,
                    self.color, self.backgroundColor)
            except Exception:
                pass

    def hide(self) -> None:
        self.visible = False
        if self._bridge is not None:
            try:
                self._bridge.hide_status_bar_item(self._id)
            except Exception:
                pass

    def dispose(self) -> None:
        self.visible = False
        if self._bridge is not None:
            try:
                self._bridge.dispose_status_bar_item(self._id)
            except Exception:
                pass


class _Terminal:
    def __init__(self, name: str = "",
                 bridge: Optional[UIBridge] = None) -> None:
        self.name = name
        self.processId = None
        self.creationOptions = {}
        self.exitStatus = None
        self.state = {"isInteractedWith": False}
        self._disposed = False
        self._bridge = bridge
        self._on_dispose: Optional[Callable[[], None]] = None

    def sendText(self, text: str, addNewLine: bool = True) -> None:
        self.state["isInteractedWith"] = True
        if self._bridge is not None:
            try:
                self._bridge.run_terminal_command(self.name, text)
            except Exception:
                suffix = "\n" if addNewLine else ""
                print(f"[terminal:{self.name}] {text}{suffix}", end="")
        else:
            suffix = "\n" if addNewLine else ""
            print(f"[terminal:{self.name}] {text}{suffix}", end="")

    def show(self, preserveFocus: bool = False) -> None:
        if self._bridge is not None:
            try:
                self._bridge.show_terminal(self.name)
            except Exception:
                pass

    def hide(self) -> None:
        if self._bridge is not None:
            try:
                self._bridge.hide_terminal(self.name)
            except Exception:
                pass

    def dispose(self) -> None:
        self._disposed = True
        if self._on_dispose:
            try:
                self._on_dispose()
            except Exception:
                pass


class _UIProgress:
    """Progress reporter that pushes updates to the HTML UI via the bridge.

    Falls back to no-op when no bridge is connected (same behavior as the
    old ``_DummyProgress``).
    """

    def __init__(self, bridge: Optional[UIBridge] = None) -> None:
        self._bridge = bridge

    def report(self, value: Any = None) -> None:
        if self._bridge is not None and value is not None:
            message = None
            increment = None
            if isinstance(value, dict):
                message = value.get("message")
                increment = value.get("increment")
            try:
                self._bridge.show_progress(message, increment)
            except Exception:
                pass


class _WebviewPanel:
    def __init__(self, view_type: str = "", title: str = "",
                 bridge: Optional[UIBridge] = None) -> None:
        self.viewType = view_type          # camelCase (VSCode canonical)
        self.view_type = view_type         # snake_case alias (backward compat)
        self.title = title
        self.viewColumn = 1
        self.options: Dict[str, Any] = {}
        self.iconPath = None
        self.webview = _Webview()
        self.visible = True
        self.active = True
        self._bridge = bridge
        self._disposed = False
        self._dispose_emitter = EventEmitter()
        self._view_state_emitter = EventEmitter()
        self._on_dispose: Optional[Callable[[], None]] = None

        # Wire html change -> bridge
        self.webview._on_html_changed = self._push_html
        self.webview._on_post_message = self._push_message

    def _push_html(self, html: str) -> None:
        if self._bridge is not None:
            try:
                self._bridge.render_webview_panel(self.view_type, html)
            except Exception:
                pass

    def _push_message(self, message: Any) -> None:
        if self._bridge is not None:
            try:
                self._bridge.post_webview_message(self.view_type, message)
            except Exception:
                pass

    @property
    def on_did_dispose(self):
        return self._dispose_emitter.event

    @property
    def onDidDispose(self):
        return self._dispose_emitter.event

    @property
    def onDidChangeViewState(self):
        return self._view_state_emitter.event

    def reveal(self, *a, **kw) -> None:
        self.visible = True
        self.active = True
        self._view_state_emitter.fire({"webviewPanel": self})

    def dispose(self) -> None:
        if self._disposed:
            return
        self._disposed = True
        self.visible = False
        self.active = False
        self._view_state_emitter.fire({"webviewPanel": self})
        # Risk 4: notify bridge of disposal
        if self._bridge is not None:
            try:
                self._bridge.dispose_webview_panel(self.view_type)
            except Exception:
                pass
        # Clear HTML content and unhook callbacks
        self.webview._html = ""
        self.webview._on_html_changed = None
        self.webview._on_post_message = None
        if self._on_dispose is not None:
            try:
                self._on_dispose()
            except Exception:
                pass
        self._dispose_emitter.fire()


class _Webview:
    """Webview that mirrors VSCode's Webview API.

    When ``_on_html_changed`` is set (by the owning panel/view), every
    assignment to ``.html`` pushes the new content to the UI bridge so
    the frontend can render it.

    Bidirectional messaging:
      Extension -> Webview:  ``webview.postMessage(data)``
        Calls ``_on_post_message`` (set by the owner) which relays
        through UIBridge so the frontend iframe receives the data.
      Webview -> Extension:  ``acquireVsCodeApi().postMessage(data)``
        The frontend calls UIBridge, which invokes
        ``webview.receive_message_from_webview(data)`` -- this fires the
        ``onDidReceiveMessage`` emitter so extension listeners run.
    """

    def __init__(self) -> None:
        self._html = ""
        self.options: Dict[str, Any] = {}
        self._message_emitter = EventEmitter()
        # Callbacks set by the owning _WebviewView / _WebviewPanel
        self._on_html_changed: Optional[Callable[[str], None]] = None
        self._on_post_message: Optional[Callable[[Any], None]] = None

    # -- html property with change notification --

    @property
    def html(self) -> str:
        return self._html

    @html.setter
    def html(self, value: str) -> None:
        self._html = value
        if self._on_html_changed is not None:
            try:
                self._on_html_changed(value)
            except Exception:
                pass

    # -- onDidReceiveMessage (webview -> extension) --

    @property
    def on_did_receive_message(self):
        return self._message_emitter.event

    @property
    def onDidReceiveMessage(self):
        return self._message_emitter.event

    def receive_message_from_webview(self, message: Any) -> None:
        """Called by the bridge when the webview HTML posts a message back."""
        self._message_emitter.fire(message)

    # -- postMessage (extension -> webview) --

    def post_message(self, message: Any) -> bool:
        """Send *message* to the webview HTML (extension -> webview direction).
        """
        if self._on_post_message is not None:
            try:
                self._on_post_message(message)
            except Exception:
                pass
        return True

    def postMessage(self, message: Any) -> bool:
        return self.post_message(message)

    # -- URI / CSP helpers --

    @property
    def csp_source(self) -> str:
        return ""

    @property
    def cspSource(self) -> str:
        return self.csp_source

    def as_webview_uri(self, uri: Any) -> str:
        return str(uri)

    def asWebviewUri(self, uri: Any) -> str:
        return self.as_webview_uri(uri)


class _WebviewView:
    def __init__(self, view_id: str,
                 bridge: Optional[UIBridge] = None,
                 token: str = "") -> None:
        self.viewType = view_id
        self.title = view_id
        self.description = ""
        self.badge = None
        self.visible = True
        self.webview = _Webview()
        self._bridge = bridge
        self._token = token
        self._disposed = False
        self._dispose_emitter = EventEmitter()
        self._visibility_emitter = EventEmitter()

        # Wire html change -> bridge
        self.webview._on_html_changed = self._push_html
        self.webview._on_post_message = self._push_message

    def _push_html(self, html: str) -> None:
        if self._bridge is not None:
            try:
                self._bridge.render_webview_panel(
                    self.viewType, html,
                )
            except Exception:
                pass

    def _push_message(self, message: Any) -> None:
        if self._bridge is not None:
            try:
                self._bridge.post_webview_message(self.viewType, message)
            except Exception:
                pass

    @property
    def token(self) -> str:
        return self._token

    @property
    def onDidDispose(self):
        return self._dispose_emitter.event

    @property
    def onDidChangeVisibility(self):
        return self._visibility_emitter.event

    def show(self, preserveFocus: bool = False) -> None:
        was_visible = self.visible
        self.visible = True
        if not was_visible:
            self._visibility_emitter.fire()

    def dispose(self) -> None:
        if self._disposed:
            return
        self._disposed = True
        was_visible = self.visible
        self.visible = False
        # Risk 4: notify bridge of disposal
        if self._bridge is not None:
            try:
                self._bridge.dispose_webview_panel(self.viewType)
            except Exception:
                pass
        # Clear HTML content
        self.webview._html = ""
        self.webview._on_html_changed = None
        self.webview._on_post_message = None
        if was_visible:
            self._visibility_emitter.fire()
        self._dispose_emitter.fire()


class _TextDocument:
    def __init__(self, uri: Uri, content: str, language_id: str = "plaintext") -> None:
        self.uri = uri
        self.fileName = uri.fs_path if getattr(uri, "scheme", "") == "file" else str(uri)
        self.languageId = language_id or "plaintext"
        self.version = 1
        self.isDirty = False
        self.isUntitled = getattr(uri, "scheme", "") == "untitled"
        self._content = content
        self._save_callback: Optional[Callable[["_TextDocument"], bool]] = None

    @property
    def lineCount(self) -> int:
        return self._content.count("\n") + 1

    def getText(self, range: Any = None) -> str:
        return self._content

    def save(self) -> bool:
        if callable(self._save_callback):
            return bool(self._save_callback(self))
        with open(self.fileName, "w", encoding="utf-8") as fh:
            fh.write(self._content)
        self.isDirty = False
        return True


class _TextEditor:
    def __init__(self, document: _TextDocument) -> None:
        self.document = document
        self.selection = Range(Position(0, 0), Position(0, 0))
        self.selections = [self.selection]
        self.visibleRanges = []
        self.options = {}
        self.viewColumn = 1

    def revealRange(self, range: Any, reveal_type: Any = None) -> None:
        self.visibleRanges = [range]

    def edit(self, callback: Callable, options: Dict = None) -> bool:
        builder = _TextEditorEdit(self.document)
        callback(builder)
        return builder.apply()


class _TextEditorEdit:
    def __init__(self, document: Optional[_TextDocument]) -> None:
        self._document = document
        self._edits: List[Dict[str, Any]] = []

    def replace(self, range: Any, text: str) -> None:
        self._edits.append({"kind": "replace", "range": range, "newText": str(text or "")})

    def insert(self, position: Any, text: str) -> None:
        self._edits.append({"kind": "insert", "position": position, "newText": str(text or "")})

    def delete(self, range: Any) -> None:
        self._edits.append({"kind": "delete", "range": range})

    def apply(self) -> bool:
        if self._document is None or not self._edits:
            return False
        updated = _apply_structured_text_edits(self._document.getText(), self._edits)
        if updated is None:
            return False
        self._document._content = updated
        self._document.isDirty = True
        self._document.version += 1
        return True


class _FileSystemWatcher:
    def __init__(self, pattern: Any,
                 root_provider: Callable[[], str]) -> None:
        self.globPattern = pattern
        self.ignoreCreateEvents = False
        self.ignoreChangeEvents = False
        self.ignoreDeleteEvents = False
        self._create = EventEmitter()
        self._change = EventEmitter()
        self._delete = EventEmitter()
        self._root_provider = root_provider
        self._on_dispose: Optional[Callable[[], None]] = None
        self._stop = threading.Event()
        self._known = self._scan_matches()
        self._thread = threading.Thread(target=self._poll_loop, daemon=True)
        self._thread.start()

    @property
    def onDidCreate(self):
        return self._create.event

    @property
    def onDidChange(self):
        return self._change.event

    @property
    def onDidDelete(self):
        return self._delete.event

    def notify_path(self, path: str, event_kind: str) -> None:
        normalized = os.path.normpath(path)
        if not self._matches(normalized):
            return
        uri = Uri.file(normalized)
        if event_kind == "create":
            self._known[normalized] = self._stat_signature(normalized)
            if not self.ignoreCreateEvents:
                self._create.fire(uri)
            return
        if event_kind == "change":
            self._known[normalized] = self._stat_signature(normalized)
            if not self.ignoreChangeEvents:
                self._change.fire(uri)
            return
        if event_kind == "delete":
            self._known.pop(normalized, None)
            if not self.ignoreDeleteEvents:
                self._delete.fire(uri)

    def _poll_loop(self) -> None:
        while not self._stop.wait(0.2):
            current = self._scan_matches()
            current_paths = set(current)
            known_paths = set(self._known)
            for created in sorted(current_paths - known_paths):
                if not self.ignoreCreateEvents:
                    self._create.fire(Uri.file(created))
            for deleted in sorted(known_paths - current_paths):
                if not self.ignoreDeleteEvents:
                    self._delete.fire(Uri.file(deleted))
            for common in sorted(current_paths & known_paths):
                if current[common] != self._known[common] and not self.ignoreChangeEvents:
                    self._change.fire(Uri.file(common))
            self._known = current

    def _scan_matches(self) -> Dict[str, Any]:
        root = self._root_provider() if callable(self._root_provider) else ""
        if not root or not os.path.isdir(root):
            return {}
        matches: Dict[str, Any] = {}
        for current, dirs, files in os.walk(root):
            dirs[:] = [d for d in dirs if d not in {".git", "__pycache__", "node_modules"}]
            for file_name in files:
                full = os.path.normpath(os.path.join(current, file_name))
                if self._matches(full):
                    matches[full] = self._stat_signature(full)
        return matches

    def _matches(self, full_path: str) -> bool:
        pattern = _glob_pattern(self.globPattern)
        if not pattern:
            return True
        normalized = full_path.replace("\\", "/")
        if os.path.isabs(pattern):
            return _glob_matches(normalized, pattern)
        root = self._root_provider() if callable(self._root_provider) else ""
        try:
            rel = os.path.relpath(full_path, root).replace("\\", "/")
        except Exception:
            rel = normalized
        return _glob_matches(rel, pattern)

    @staticmethod
    def _stat_signature(path: str) -> Any:
        try:
            stat = os.stat(path)
        except OSError:
            return None
        return (getattr(stat, "st_mtime_ns", int(stat.st_mtime * 1_000_000_000)), stat.st_size)

    def dispose(self) -> None:
        self._stop.set()
        self._create.clear()
        self._change.clear()
        self._delete.clear()
        if self._on_dispose:
            self._on_dispose()


class _TaskExecution:
    def __init__(self, task: Any, name: str, kind: str,
                 process: Optional[subprocess.Popen] = None) -> None:
        self.id = str(uuid.uuid4())
        self.task = task
        self.name = name
        self.kind = kind
        self.process = process
        self.processId = getattr(process, "pid", None)
        self.exitStatus: Optional[Dict[str, Any]] = None
        self.state = {"isInteractedWith": False}

    def terminate(self) -> None:
        if self.process and self.process.poll() is None:
            self.process.terminate()

    def start_waiter(self, on_finish: Callable[[], None]) -> None:
        def _wait() -> None:
            code = self.process.wait() if self.process else 0
            self.exitStatus = {"code": code}
            on_finish()
        threading.Thread(target=_wait, daemon=True).start()

    def start_callable(self, callback: Callable[[], Any],
                       on_finish: Callable[[], None]) -> None:
        def _run() -> None:
            code = 0
            try:
                callback()
            except Exception:
                code = 1
            self.exitStatus = {"code": code}
            on_finish()
        threading.Thread(target=_run, daemon=True).start()


class _DebugSession:
    def __init__(self, name: str, debug_type: str,
                 configuration: Dict[str, Any],
                 process: subprocess.Popen,
                 parent_session: Any = None) -> None:
        self.id = str(uuid.uuid4())
        self.name = name
        self.type = debug_type
        self.configuration = configuration
        self.parentSession = parent_session
        self.process = process
        self.processId = getattr(process, "pid", None)
        self.exitStatus: Optional[Dict[str, Any]] = None
        self.terminated = False

    def start_waiter(self, on_finish: Callable[[], None]) -> None:
        def _wait() -> None:
            code = self.process.wait()
            self.exitStatus = {"code": code}
            self.terminated = True
            on_finish()
        threading.Thread(target=_wait, daemon=True).start()


class _TreeView:
    def __init__(self, view_id: str, provider: Any = None) -> None:
        self.id = view_id
        self.provider = None
        self.visible = True
        self.selection = []
        self.activeItem = None
        self._message = ""
        self._title = view_id
        self._description = ""
        self._badge = None
        self.dragAndDropController = None
        self.refresh_version = 0
        self.reveal_version = 0
        self._snapshot_counter = 0
        self._handle_elements: Dict[str, Any] = {}
        self._expanded_handles = set()
        self._revealed_element = None
        self._reveal_ancestors: List[Any] = []
        self._focused_element = None
        self._reveal_expand_levels = 0
        self._change_callback: Optional[Callable[[str, str, Dict[str, Any]], None]] = None
        self._provider_change_disposable = None
        self._dispose = EventEmitter()
        self._expand = EventEmitter()
        self._collapse = EventEmitter()
        self._selection_change = EventEmitter()
        self._active_change = EventEmitter()
        self._visibility_change = EventEmitter()
        self._checkbox_change = EventEmitter()
        if provider is not None:
            self.bind_provider(provider)

    @property
    def message(self) -> str:
        return self._message

    @message.setter
    def message(self, value: Any) -> None:
        self.apply_state({"message": value})

    @property
    def title(self) -> str:
        return self._title

    @title.setter
    def title(self, value: Any) -> None:
        self.apply_state({"title": value})

    @property
    def description(self) -> str:
        return self._description

    @description.setter
    def description(self, value: Any) -> None:
        self.apply_state({"description": value})

    @property
    def badge(self) -> Any:
        return dict(self._badge) if isinstance(self._badge, dict) else None

    @badge.setter
    def badge(self, value: Any) -> None:
        self.apply_state({"badge": value})

    @property
    def onDidDispose(self):
        return self._dispose.event

    @property
    def onDidExpandElement(self):
        return self._expand.event

    @property
    def onDidCollapseElement(self):
        return self._collapse.event

    @property
    def onDidChangeSelection(self):
        return self._selection_change.event

    @property
    def onDidChangeActiveItem(self):
        return self._active_change.event

    @property
    def onDidChangeVisibility(self):
        return self._visibility_change.event

    @property
    def onDidChangeCheckboxState(self):
        return self._checkbox_change.event

    def bind_provider(self, provider: Any,
                      change_callback: Optional[
                          Callable[[str, str, Dict[str, Any]], None]
                      ] = None) -> None:
        self._dispose_provider_listener()
        self.provider = provider
        self._change_callback = change_callback
        self._handle_elements.clear()
        self._expanded_handles.clear()
        self._revealed_element = None
        self._reveal_ancestors = []
        self._focused_element = None
        self._reveal_expand_levels = 0
        self.refresh_version += 1
        self._subscribe_provider_refresh()

    @staticmethod
    def _normalize_optional_text(value: Any) -> str:
        if value is None:
            return ""
        return str(value)

    @staticmethod
    def _normalize_badge(value: Any) -> Optional[Dict[str, Any]]:
        if value is None or value == "":
            return None
        if isinstance(value, dict):
            raw_value = value.get("value")
            tooltip = value.get("tooltip")
        else:
            raw_value = getattr(value, "value", None)
            tooltip = getattr(value, "tooltip", None)
        if raw_value is None:
            return None
        try:
            badge_value = int(raw_value)
        except Exception:
            return None
        return {
            "value": badge_value,
            "tooltip": "" if tooltip is None else str(tooltip),
        }

    def state_payload(self) -> Dict[str, Any]:
        return {
            "message": self._message,
            "title": self._title,
            "description": self._description,
            "badge": self.badge,
            "visible": bool(self.visible),
            "dragAndDrop": self.drag_and_drop_payload(),
            "selection": [str(item) for item in list(self.selection or [])],
            "refreshVersion": self.refresh_version,
        }

    @staticmethod
    def _mime_list(value: Any) -> List[str]:
        if isinstance(value, str):
            return [value] if value else []
        try:
            return [str(item) for item in (value or []) if str(item or "")]
        except Exception:
            return []

    def bind_drag_and_drop_controller(self, controller: Any) -> None:
        self.dragAndDropController = controller
        self._notify_changed("state", self.state_payload())

    def drag_and_drop_payload(self) -> Dict[str, Any]:
        controller = self.dragAndDropController
        if controller is None:
            return {
                "enabled": False,
                "canDrag": False,
                "canDrop": False,
                "dragMimeTypes": [],
                "dropMimeTypes": [],
            }
        drag_mime_types = self._mime_list(
            getattr(controller, "dragMimeTypes", None)
            if not isinstance(controller, dict)
            else controller.get("dragMimeTypes"))
        drop_mime_types = self._mime_list(
            getattr(controller, "dropMimeTypes", None)
            if not isinstance(controller, dict)
            else controller.get("dropMimeTypes"))
        handle_drag = (
            controller.get("handleDrag")
            if isinstance(controller, dict)
            else getattr(controller, "handleDrag", None))
        handle_drop = (
            controller.get("handleDrop")
            if isinstance(controller, dict)
            else getattr(controller, "handleDrop", None))
        can_drag = (
            bool(controller.get("canDrag"))
            if isinstance(controller, dict) and "canDrag" in controller
            else bool(getattr(controller, "canDrag", callable(handle_drag))))
        can_drop = (
            bool(controller.get("canDrop"))
            if isinstance(controller, dict) and "canDrop" in controller
            else bool(getattr(controller, "canDrop", callable(handle_drop))))
        return {
            "enabled": can_drag or can_drop,
            "canDrag": can_drag,
            "canDrop": can_drop,
            "dragMimeTypes": drag_mime_types,
            "dropMimeTypes": drop_mime_types,
            "treeMimeType": self.tree_drag_mime_type(self.id),
        }

    @staticmethod
    def tree_drag_mime_type(view_id: Any) -> str:
        normalized = re.sub(
            r"[^a-z0-9_-]+", "",
            str(view_id or "").lower())
        return f"application/vnd.code.tree.{normalized}"

    def perform_drag_and_drop(
            self,
            source_handles: Any,
            target_handle: str = "",
            data_transfer: Any = None) -> Dict[str, Any]:
        controller = self.dragAndDropController
        if controller is None:
            return {"error": f"Tree drag/drop controller not found: {self.id}"}
        handles = [
            str(handle or "")
            for handle in (
                source_handles if isinstance(source_handles, list)
                else [source_handles])
            if str(handle or "")
        ]
        sources: List[Any] = []
        for handle in handles:
            element = self.element_for_handle(handle)
            if element is None:
                return {
                    "error": "Tree source handle is stale or unknown",
                    "view_id": self.id,
                    "handle": handle,
                }
            sources.append(element)
        if not sources:
            return {"error": "At least one tree source handle is required"}
        target = None
        if target_handle:
            target = self.element_for_handle(target_handle)
            if target is None:
                return {
                    "error": "Tree target handle is stale or unknown",
                    "view_id": self.id,
                    "handle": str(target_handle or ""),
                }
        transfer = DataTransfer(data_transfer or {})
        tree_mime = self.tree_drag_mime_type(self.id)
        if not transfer.has(tree_mime):
            transfer.set(tree_mime, sources)
        handle_drag = (
            controller.get("handleDrag")
            if isinstance(controller, dict)
            else getattr(controller, "handleDrag", None))
        handle_drop = (
            controller.get("handleDrop")
            if isinstance(controller, dict)
            else getattr(controller, "handleDrop", None))
        can_drag = (
            bool(controller.get("canDrag"))
            if isinstance(controller, dict) and "canDrag" in controller
            else bool(getattr(controller, "canDrag", callable(handle_drag))))
        can_drop = (
            bool(controller.get("canDrop"))
            if isinstance(controller, dict) and "canDrop" in controller
            else bool(getattr(controller, "canDrop", callable(handle_drop))))
        try:
            if can_drag and callable(handle_drag):
                _resolve_provider_result(_call_with_compatible_args(
                    handle_drag,
                    (sources, transfer, CancellationToken.NONE)),
                    default=None)
            if not can_drop or not callable(handle_drop):
                return {
                    "error": "Tree drag/drop controller does not implement handleDrop",
                    "view_id": self.id,
                }
            _resolve_provider_result(_call_with_compatible_args(
                handle_drop,
                (target, transfer, CancellationToken.NONE)),
                default=None)
        except Exception as exc:
            return {
                "error": str(exc),
                "view_id": self.id,
            }
        return {
            "ok": True,
            "view_id": self.id,
            "sourceHandles": handles,
            "targetHandle": str(target_handle or ""),
            "dataTransfer": transfer.to_payload(),
            "refreshVersion": self.refresh_version,
        }

    def apply_state(self, state: Any, notify: bool = True) -> None:
        if not isinstance(state, dict):
            return
        changed = False
        for key in ("message", "title", "description"):
            if key not in state:
                continue
            next_value = self._normalize_optional_text(state.get(key))
            attr = f"_{key}"
            if getattr(self, attr) != next_value:
                setattr(self, attr, next_value)
                changed = True
        if "badge" in state:
            next_badge = self._normalize_badge(state.get("badge"))
            if self._badge != next_badge:
                self._badge = next_badge
                changed = True
        if "visible" in state:
            next_visible = bool(state.get("visible"))
            if bool(self.visible) != next_visible:
                self.visible = next_visible
                self._visibility_change.fire({"visible": next_visible})
                changed = True
        if changed and notify:
            self._notify_changed("state", self.state_payload())

    def begin_snapshot(self) -> None:
        self._snapshot_counter += 1

    def remember_element(self, element: Any) -> str:
        for handle, existing in self._handle_elements.items():
            if existing is element:
                return handle
            try:
                if existing == element:
                    return handle
            except Exception:
                pass
        handle = f"{self._snapshot_counter}:{len(self._handle_elements) + 1}"
        self._handle_elements[handle] = element
        return handle

    def element_for_handle(self, handle: str) -> Any:
        return self._handle_elements.get(str(handle or ""))

    def set_expanded(self, handle: str, expanded: bool) -> bool:
        normalized_handle = str(handle or "")
        if normalized_handle not in self._handle_elements:
            return False
        was_expanded = normalized_handle in self._expanded_handles
        next_expanded = bool(expanded)
        if was_expanded == next_expanded:
            return True
        if next_expanded:
            self._expanded_handles.add(normalized_handle)
            self._expand.fire({
                "element": self._handle_elements[normalized_handle],
            })
        else:
            self._expanded_handles.discard(normalized_handle)
            self._collapse.fire({
                "element": self._handle_elements[normalized_handle],
            })
        self._notify_changed("expanded", {
            "handle": normalized_handle,
            "expanded": next_expanded,
            "refreshVersion": self.refresh_version,
        })
        return True

    def set_checkbox_state(self, handle: str, state: Any) -> bool:
        normalized_handle = str(handle or "")
        if normalized_handle not in self._handle_elements:
            return False
        try:
            next_state = 1 if int(state or 0) == 1 else 0
        except Exception:
            next_state = 1 if bool(state) else 0
        element = self._handle_elements[normalized_handle]
        self._checkbox_change.fire({
            "items": [(element, next_state)],
        })
        self._notify_changed("checkbox", {
            "handle": normalized_handle,
            "state": next_state,
            "refreshVersion": self.refresh_version,
        })
        return True

    @staticmethod
    def _same_element(left: Any, right: Any) -> bool:
        if left is right:
            return True
        try:
            return left == right
        except Exception:
            return False

    def is_revealed(self, element: Any) -> bool:
        revealed = self._revealed_element
        if revealed is None:
            return False
        return self._same_element(revealed, element)

    def is_reveal_ancestor(self, element: Any) -> bool:
        for ancestor in self._reveal_ancestors:
            if self._same_element(ancestor, element):
                return True
        return False

    def is_focused(self, element: Any) -> bool:
        focused = self._focused_element
        if focused is None:
            return False
        return self._same_element(focused, element)

    def reveal_expand_level(self, element: Any) -> int:
        levels = int(self._reveal_expand_levels or 0)
        revealed = self._revealed_element
        if levels <= 0 or revealed is None:
            return 0
        if self._same_element(element, revealed):
            return levels
        get_parent = self._get_parent_callable()
        if not callable(get_parent):
            return 0
        current = element
        seen = {id(element)}
        remaining = levels
        while remaining > 0:
            try:
                parent = _resolve_provider_result(
                    get_parent(current), default=None)
            except Exception:
                return 0
            if parent is None:
                return 0
            if self._same_element(parent, revealed):
                return remaining - 1
            marker = id(parent)
            if marker in seen:
                return 0
            seen.add(marker)
            current = parent
            remaining -= 1
        return 0

    def is_selected(self, element: Any) -> bool:
        for selected in self.selection:
            if selected is element:
                return True
            try:
                if selected == element:
                    return True
            except Exception:
                pass
        return False

    def select_handle(self, handle: str) -> bool:
        if handle not in self._handle_elements:
            return False
        self.set_selection([self._handle_elements[handle]])
        return True

    def set_selection(self, selection: Any, reveal: bool = False) -> None:
        if selection is None:
            normalized: List[Any] = []
        elif isinstance(selection, list):
            normalized = list(selection)
        else:
            normalized = [selection]
        changed = len(normalized) != len(self.selection)
        if not changed:
            for old, new in zip(self.selection, normalized):
                if old is new:
                    continue
                try:
                    if old == new:
                        continue
                except Exception:
                    pass
                changed = True
                break
        if not reveal:
            self._revealed_element = None
            self._reveal_ancestors = []
            self._focused_element = None
            self._reveal_expand_levels = 0
        self.selection = normalized
        self.activeItem = normalized[0] if normalized else None
        if changed:
            self._selection_change.fire({"selection": list(self.selection)})
            self._active_change.fire({"activeItem": self.activeItem})
            self._notify_changed("selection", {
                "refreshVersion": self.refresh_version,
            })

    def reveal(self, element: Any = None, options: Any = None,
               **kw: Any) -> None:
        normalized_options = self._normalize_reveal_options(options, kw)
        if element is None:
            self._revealed_element = None
            self._reveal_ancestors = []
            self._focused_element = None
            self._reveal_expand_levels = 0
            self.reveal_version += 1
            self._notify_changed("reveal", {
                "refreshVersion": self.refresh_version,
                "revealVersion": self.reveal_version,
            })
            return
        select = bool(normalized_options.get("select", True))
        focus = bool(normalized_options.get("focus", False))
        if select:
            self.set_selection([element], reveal=True)
        elif focus:
            self.activeItem = element
            self._active_change.fire({"activeItem": self.activeItem})
        self._revealed_element = element
        self._reveal_ancestors = self._resolve_reveal_ancestors(element)
        self._focused_element = element if focus else None
        self._reveal_expand_levels = self._normalize_reveal_expand(
            normalized_options.get("expand", False))
        self.reveal_version += 1
        self._notify_changed("reveal", {
            "refreshVersion": self.refresh_version,
            "revealVersion": self.reveal_version,
            "select": select,
            "focus": focus,
            "expand": self._reveal_expand_levels,
        })

    @staticmethod
    def _normalize_reveal_options(options: Any,
                                  kw: Dict[str, Any]) -> Dict[str, Any]:
        result: Dict[str, Any] = {}
        if isinstance(options, dict):
            result.update(options)
        elif options is not None:
            for key in ("select", "focus", "expand"):
                if hasattr(options, key):
                    result[key] = getattr(options, key)
        result.update(kw or {})
        if "select" not in result:
            result["select"] = True
        if "focus" not in result:
            result["focus"] = False
        if "expand" not in result:
            result["expand"] = False
        return result

    @staticmethod
    def _normalize_reveal_expand(value: Any) -> int:
        if value is True:
            return 1
        if value in (False, None):
            return 0
        try:
            levels = int(value)
        except Exception:
            return 0
        return max(0, min(levels, 3))

    def _get_parent_callable(self) -> Optional[Callable[[Any], Any]]:
        provider = self.provider
        return (
            getattr(provider, "getParent", None)
            or getattr(provider, "get_parent", None)
        )

    def _resolve_reveal_ancestors(self, element: Any) -> List[Any]:
        get_parent = self._get_parent_callable()
        if not callable(get_parent):
            return []
        ancestors: List[Any] = []
        seen = {id(element)}
        current = element
        for _ in range(50):
            try:
                parent = _resolve_provider_result(
                    get_parent(current), default=None)
            except Exception:
                break
            if parent is None:
                break
            marker = id(parent)
            if marker in seen:
                break
            ancestors.append(parent)
            seen.add(marker)
            current = parent
        return ancestors

    def dispose(self) -> None:
        self.apply_state({"visible": False}, notify=True)
        self._dispose_provider_listener()
        self._dispose.fire()

    def _subscribe_provider_refresh(self) -> None:
        provider = self.provider
        if provider is None:
            return
        event = (
            getattr(provider, "onDidChangeTreeData", None)
            or getattr(provider, "on_did_change_tree_data", None)
        )
        subscribe = getattr(event, "event", None) if event is not None else None
        if subscribe is None:
            subscribe = event
        if not callable(subscribe):
            return
        try:
            self._provider_change_disposable = subscribe(self._on_provider_changed)
        except Exception:
            self._provider_change_disposable = None

    def _dispose_provider_listener(self) -> None:
        disposable = self._provider_change_disposable
        self._provider_change_disposable = None
        if disposable is None:
            return
        dispose = getattr(disposable, "dispose", None)
        try:
            if callable(dispose):
                dispose()
            elif callable(disposable):
                disposable()
        except Exception:
            pass

    def _on_provider_changed(self, element: Any = None) -> None:
        self.refresh_version += 1
        self._handle_elements.clear()
        self._expanded_handles.clear()
        self._notify_changed("refresh", {
            "refreshVersion": self.refresh_version,
            "element": element,
        })

    def _notify_changed(self, event: str, payload: Dict[str, Any]) -> None:
        callback = self._change_callback
        if callback is None:
            return
        try:
            callback(event, self.id, payload)
        except Exception:
            pass


class _DiagnosticCollection:
    def __init__(self, name: str = "", emitter: EventEmitter = None) -> None:
        self.name = name
        self._items: Dict[str, Any] = {}
        self._emitter = emitter

    def set(self, uri: Any, diagnostics: Any = None) -> None:
        if isinstance(uri, list):
            for item_uri, item_diags in uri:
                self.set(item_uri, item_diags)
            return
        key = str(uri)
        self._items[key] = diagnostics or []
        if self._emitter:
            self._emitter.fire({"uris": [uri]})

    def get(self, uri: Any) -> Any:
        return self._items.get(str(uri), [])

    def delete(self, uri: Any) -> None:
        self._items.pop(str(uri), None)
        if self._emitter:
            self._emitter.fire({"uris": [uri]})

    def clear(self) -> None:
        uris = list(self._items)
        self._items.clear()
        if self._emitter and uris:
            self._emitter.fire({"uris": uris})

    def entries(self) -> List[Any]:
        return list(self._items.items())

    def dispose(self) -> None:
        self.clear()


def _set_document_language(doc: Any, language_id: str) -> Any:
    if isinstance(doc, _TextDocument):
        doc.languageId = str(language_id or "plaintext")
    return doc


def _glob_pattern(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, str):
        return value.replace("\\", "/")
    pattern = getattr(value, "pattern", None)
    if pattern is not None:
        return str(pattern).replace("\\", "/")
    if isinstance(value, dict):
        raw = value.get("pattern") or value.get("globPattern") or ""
        return str(raw).replace("\\", "/")
    return str(value).replace("\\", "/")


def _glob_matches(path: str, pattern: str) -> bool:
    normalized = path.replace("\\", "/")
    normalized_pattern = pattern.replace("\\", "/")
    if fnmatch.fnmatch(normalized, normalized_pattern):
        return True
    if normalized_pattern.startswith("**/"):
        return fnmatch.fnmatch(normalized, normalized_pattern[3:])
    return False


def _coerce_uri(value: Any) -> Optional[Uri]:
    if isinstance(value, Uri):
        return value
    if isinstance(value, str) and value:
        if os.path.isabs(value) or (len(value) >= 2 and value[1] == ":" and value[0].isalpha()):
            return Uri.file(value)
        return Uri.parse(value) if ":" in value else Uri.file(value)
    if isinstance(value, dict):
        raw = value.get("uri") or value.get("path")
        if raw:
            return _coerce_uri(raw)
    return None


def _coerce_position(value: Any) -> Position:
    if isinstance(value, Position):
        return value
    if isinstance(value, dict):
        return Position(int(value.get("line", 0)), int(value.get("character", 0)))
    return Position()


def _coerce_range(value: Any) -> Range:
    if isinstance(value, Range):
        return value
    if isinstance(value, dict):
        return Range(_coerce_position(value.get("start")), _coerce_position(value.get("end")))
    return Range()


def _coerce_inline_value_context(
        value: Any, fallback_range: Any = None) -> InlineValueContext:
    if isinstance(value, InlineValueContext):
        return value
    frame_id = 0
    stopped_location = fallback_range
    if isinstance(value, dict):
        frame_id = value.get("frameId", value.get("frame_id", 0))
        stopped_location = (
            value.get("stoppedLocation")
            or value.get("stopped_location")
            or value.get("range")
            or fallback_range)
    else:
        frame_id = getattr(value, "frameId", 0)
        stopped_location = (
            getattr(value, "stoppedLocation", None)
            or getattr(value, "range", None)
            or fallback_range)
    return InlineValueContext(frame_id, stopped_location)


def _coerce_color(value: Any) -> Color:
    if isinstance(value, Color):
        return value
    if isinstance(value, dict):
        return Color(
            value.get("red", 0.0),
            value.get("green", 0.0),
            value.get("blue", 0.0),
            value.get("alpha", 1.0))
    return Color(
        getattr(value, "red", 0.0),
        getattr(value, "green", 0.0),
        getattr(value, "blue", 0.0),
        getattr(value, "alpha", 1.0))


def _position_to_offset(text: str, position: Position) -> int:
    line = max(0, int(getattr(position, "line", 0)))
    character = max(0, int(getattr(position, "character", 0)))
    current_line = 0
    offset = 0
    lines = text.splitlines(True)
    for segment in lines:
        if current_line == line:
            return min(offset + character, offset + len(segment.rstrip("\r\n")))
        offset += len(segment)
        current_line += 1
    return min(len(text), offset + character)


def _apply_structured_text_edits(text: str, edits: Sequence[Dict[str, Any]]) -> Optional[str]:
    operations: List[Dict[str, Any]] = []
    for edit in edits:
        if not isinstance(edit, dict):
            return None
        kind = str(edit.get("kind") or "replace")
        if kind == "insert":
            position = _coerce_position(edit.get("position"))
            offset = _position_to_offset(text, position)
            operations.append({
                "start": offset,
                "end": offset,
                "newText": str(edit.get("newText", "")),
            })
            continue
        if kind in {"replace", "delete"}:
            range_value = _coerce_range(edit.get("range"))
            start = _position_to_offset(text, range_value.start)
            end = _position_to_offset(text, range_value.end)
            operations.append({
                "start": min(start, end),
                "end": max(start, end),
                "newText": "" if kind == "delete" else str(edit.get("newText", "")),
            })
            continue
        if "range" in edit:
            range_value = _coerce_range(edit.get("range"))
            start = _position_to_offset(text, range_value.start)
            end = _position_to_offset(text, range_value.end)
            operations.append({
                "start": min(start, end),
                "end": max(start, end),
                "newText": str(edit.get("newText", "")),
            })
            continue
        return None
    updated = text
    for operation in sorted(operations, key=lambda item: (item["start"], item["end"]), reverse=True):
        updated = (
            updated[:operation["start"]]
            + operation["newText"]
            + updated[operation["end"]:]
        )
    return updated


def _unsupported_feature(api_name: str, message: str) -> Dict[str, Any]:
    return {
        "ok": False,
        "unsupported": True,
        "code": "unsupportedRuntimeFeature",
        "api": api_name,
        "message": message,
    }


def _coerce_auth_session(raw: Any, provider_id: str,
                         scopes: List[str]) -> AuthenticationSession:
    if isinstance(raw, AuthenticationSession):
        return raw
    account = getattr(raw, "account", None)
    if not isinstance(account, dict):
        account = {
            "id": getattr(raw, "account_id", provider_id),
            "label": getattr(raw, "account_label", provider_id),
        }
    token = getattr(raw, "access_token", "") or getattr(raw, "accessToken", "")
    return AuthenticationSession(
        id=str(getattr(raw, "id", "") or id(raw)),
        access_token=str(token or ""),
        account=account,
        scopes=list(getattr(raw, "scopes", scopes) or scopes),
    )


def _language_id_for_path(path: str) -> str:
    ext = os.path.splitext(path)[1].lower()
    return {
        ".py": "python",
        ".js": "javascript",
        ".jsx": "javascriptreact",
        ".ts": "typescript",
        ".tsx": "typescriptreact",
        ".json": "json",
        ".md": "markdown",
        ".html": "html",
        ".css": "css",
        ".cs": "csharp",
        ".xml": "xml",
        ".yaml": "yaml",
        ".yml": "yaml",
    }.get(ext, "plaintext")


class _FileSystem:
    def __init__(self, namespace: VscodeNamespace) -> None:
        self._namespace = namespace

    def readFile(self, uri: Any) -> bytes:
        uri_obj = _coerce_uri(uri)
        if uri_obj is None:
            raise ValueError("readFile requires a Uri or path")
        return self._namespace._read_document_bytes(uri_obj)

    def writeFile(self, uri: Any, content: bytes) -> None:
        uri_obj = _coerce_uri(uri)
        if uri_obj is None:
            raise ValueError("writeFile requires a Uri or path")
        if uri_obj.scheme == "file":
            path = uri_obj.fs_path
            parent = os.path.dirname(path)
            if parent:
                os.makedirs(parent, exist_ok=True)
            existed = os.path.exists(path)
            with open(path, "wb") as f:
                f.write(content)
            self._namespace._notify_workspace_watchers(path, "change" if existed else "create")
            return
        self._namespace._write_document_bytes(uri_obj, bytes(content))

    def stat(self, uri: Any) -> Dict:
        uri_obj = _coerce_uri(uri)
        if uri_obj is None:
            raise ValueError("stat requires a Uri or path")
        provider_entry = self._namespace._get_file_system_provider(uri_obj.scheme)
        if provider_entry is not None:
            return dict(self._namespace._call_provider_method(
                provider_entry["provider"], ("stat",), uri_obj) or {})
        path = uri_obj.fs_path
        s = os.stat(path)
        return {"type": 1 if os.path.isfile(path) else 2,
                "size": s.st_size, "mtime": int(s.st_mtime * 1000)}

    def readDirectory(self, uri: Any) -> List:
        uri_obj = _coerce_uri(uri)
        if uri_obj is None:
            raise ValueError("readDirectory requires a Uri or path")
        provider_entry = self._namespace._get_file_system_provider(uri_obj.scheme)
        if provider_entry is not None:
            return list(self._namespace._call_provider_method(
                provider_entry["provider"], ("readDirectory", "read_directory"), uri_obj) or [])
        path = uri_obj.fs_path
        return [(n, 1 if os.path.isfile(os.path.join(path, n)) else 2)
                for n in os.listdir(path)]

    def createDirectory(self, uri: Any) -> None:
        uri_obj = _coerce_uri(uri)
        if uri_obj is None:
            raise ValueError("createDirectory requires a Uri or path")
        provider_entry = self._namespace._get_file_system_provider(uri_obj.scheme)
        if provider_entry is not None:
            self._namespace._call_provider_method(
                provider_entry["provider"], ("createDirectory", "create_directory"), uri_obj)
            return
        path = uri_obj.fs_path
        os.makedirs(path, exist_ok=True)

    def delete(self, uri: Any, options: Dict = None) -> None:
        import shutil
        uri_obj = _coerce_uri(uri)
        if uri_obj is None:
            raise ValueError("delete requires a Uri or path")
        provider_entry = self._namespace._get_file_system_provider(uri_obj.scheme)
        if provider_entry is not None:
            self._namespace._call_provider_method(
                provider_entry["provider"], ("delete", "deleteFile", "delete_file"),
                uri_obj, options or {})
            return
        path = uri_obj.fs_path
        if os.path.isdir(path):
            shutil.rmtree(path)
        elif os.path.isfile(path):
            os.remove(path)
        self._namespace._notify_workspace_watchers(path, "delete")

    def rename(self, old_uri: Any, new_uri: Any, options: Dict = None) -> None:
        old_uri_obj = _coerce_uri(old_uri)
        new_uri_obj = _coerce_uri(new_uri)
        if old_uri_obj is None or new_uri_obj is None:
            raise ValueError("rename requires source and target Uri/path")
        provider_entry = self._namespace._get_file_system_provider(old_uri_obj.scheme)
        if provider_entry is not None or new_uri_obj.scheme != "file":
            if old_uri_obj.scheme != new_uri_obj.scheme:
                raise ValueError("rename across filesystem providers is unsupported")
            if provider_entry is None:
                raise ValueError(f"No filesystem provider registered for scheme: {old_uri_obj.scheme}")
            self._namespace._call_provider_method(
                provider_entry["provider"], ("rename", "renameFile", "rename_file"),
                old_uri_obj, new_uri_obj, options or {})
            return
        old_path = old_uri_obj.fs_path
        new_path = new_uri_obj.fs_path
        parent = os.path.dirname(new_path)
        if parent:
            os.makedirs(parent, exist_ok=True)
        os.replace(old_path, new_path)
        self._namespace._notify_workspace_watchers(old_path, "delete")
        self._namespace._notify_workspace_watchers(new_path, "create")
