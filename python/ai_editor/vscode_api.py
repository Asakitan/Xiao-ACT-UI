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

import inspect
import json
import os
import fnmatch
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
                r = self._engine.chat_completion_stream(
                    messages=api_msgs, tools=tools_schema,
                    on_delta=lambda d: resp._chunks.append(d.content or "") if d.content else None)
                resp.text = r.content or ""
                if not resp._chunks:
                    resp._chunks = [resp.text] if resp.text else []
                resp._done = True
            except Exception as exc:
                resp.text = f"Error: {exc}"
                resp._chunks = [resp.text]
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
    _chunks: List[str] = field(default_factory=list)
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

    def on_cancellation_requested(self, listener: Callable) -> Disposable:
        if self._source:
            self._source._listeners.append(listener)
            return Disposable(lambda: self._source._listeners.remove(listener)
                              if listener in self._source._listeners else None)
        return Disposable()


CancellationToken.NONE = CancellationToken()


# ---------------------------------------------------------------------------
# Configuration API
# ---------------------------------------------------------------------------

class WorkspaceConfiguration:
    def __init__(self, section: str = "", data: Dict = None) -> None:
        self._section = section
        self._data = data or {}

    def get(self, key: str, default: Any = None) -> Any:
        parts = key.split(".")
        current = self._data
        for p in parts:
            if isinstance(current, dict):
                current = current.get(p)
            else:
                return default
            if current is None:
                return default
        return current

    def has(self, key: str) -> bool:
        return self.get(key) is not None

    def update(self, key: str, value: Any, global_scope: bool = True) -> None:
        self._data[key] = value

    def inspect(self, key: str) -> Dict:
        return {"key": key, "globalValue": self.get(key),
                "workspaceValue": self.get(key)}


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


@dataclass
class Diagnostic:
    range: Range
    message: str = ""
    severity: int = 0
    source: str = ""
    code: Any = None
    tags: List[int] = field(default_factory=list)
    related_information: List[Any] = field(default_factory=list)


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
    def render_webview_panel(self, view_id: str, html: str) -> None: ...
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
        self._webview_view_providers: Dict[str, Any] = {}
        self._webview_views: Dict[str, Any] = {}
        self._webview_view_change_callback: Optional[
            Callable[[Dict[str, Any]], None]
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
        try:
            host.on_did_change(self._on_host_extensions_changed)
        except Exception:
            pass

    def _on_host_extensions_changed(self, event: Any = None) -> None:
        self._sync_extensions_state()
        self._sync_tasks_state()
        self._sync_debug_state()
        self._extensions_change_emitter.fire(event or {})

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
            "Diagnostic": Diagnostic,
            "WorkspaceEdit": WorkspaceEdit,
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
        view = _TreeView(view_id, kw.get("treeDataProvider"))
        self._tree_views[view_id] = view
        return view

    def _register_tree_data_provider(self, view_id: str,
                                     provider: Any) -> Disposable:
        self._tree_data_providers[view_id] = provider
        if view_id in self._tree_views:
            self._tree_views[view_id].provider = provider
        return Disposable(lambda: self._tree_data_providers.pop(view_id, None))

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
                return existing
        self._text_documents.append(document)
        self._sync_workspace_state()
        self._workspace_open_text_document_emitter.fire(document)
        return document

    def _get_configuration(self, section: str = "") -> WorkspaceConfiguration:
        data = {}
        if self._settings_getter:
            raw = self._settings_getter("ai_editor", {}) or {}
            if isinstance(raw, dict):
                data = raw
        return WorkspaceConfiguration(section, data)

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
        self._store_document_content(document, updated)
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

    def _store_document_content(self, document: "_TextDocument", content: str) -> None:
        document._content = content
        document.version += 1
        if getattr(document.uri, "scheme", "") == "file":
            parent = os.path.dirname(document.fileName)
            if parent:
                os.makedirs(parent, exist_ok=True)
            with open(document.fileName, "w", encoding="utf-8") as fh:
                fh.write(content)
            document.isDirty = False
            self._workspace_save_text_document_emitter.fire(document)
            self._notify_workspace_watchers(document.fileName, "change")
        else:
            try:
                self._write_document_bytes(document.uri, content.encode("utf-8"))
                document.isDirty = False
                self._workspace_save_text_document_emitter.fire(document)
            except Exception:
                document.isDirty = True
        self._workspace_change_text_document_emitter.fire({"document": document})

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
            "registerHoverProvider": lambda selector, provider: self._register_language_provider("hover", selector, provider),
            "registerCompletionItemProvider": lambda selector, provider, *trigger: self._register_language_provider("completion", selector, provider, trigger),
            "registerDefinitionProvider": lambda selector, provider: self._register_language_provider("definition", selector, provider),
            "registerDocumentFormattingEditProvider": lambda selector, provider: self._register_language_provider("formatting", selector, provider),
            "registerCodeActionsProvider": lambda selector, provider, metadata=None: self._register_language_provider("codeActions", selector, provider, metadata),
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
        return sorted(set(builtins + contributed))

    def _language_match(self, selector: Any, document: Any) -> int:
        language_id = getattr(document, "languageId", "")
        if isinstance(selector, str):
            return 10 if selector == language_id else 0
        if isinstance(selector, list):
            return max((self._language_match(item, document) for item in selector), default=0)
        if isinstance(selector, dict):
            wanted = selector.get("language")
            return 10 if wanted in (None, language_id) else 0
        return 0

    def _register_language_provider(self, kind: str, selector: Any,
                                    provider: Any, metadata: Any = None) -> Disposable:
        entry = {"kind": kind, "selector": selector,
                 "provider": provider, "metadata": metadata}
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

    @property
    def lineCount(self) -> int:
        return self._content.count("\n") + 1

    def getText(self, range: Any = None) -> str:
        return self._content

    def save(self) -> bool:
        if self.isUntitled:
            return False
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
        self.provider = provider
        self.visible = True
        self.selection = []
        self.message = ""
        self.title = view_id
        self.description = ""
        self._dispose = EventEmitter()

    @property
    def onDidDispose(self):
        return self._dispose.event

    def reveal(self, element: Any, **kw: Any) -> None:
        self.selection = [element]

    def dispose(self) -> None:
        self.visible = False
        self._dispose.fire()


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
