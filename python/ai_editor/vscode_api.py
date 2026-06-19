"""VSCode API compatibility shim for the SAO AI Editor.

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
import threading
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Sequence

from ai_editor.extension_host import (
    CommandService, ExtensionHost, ExtensionDescription,
    Position, Range, Uri, Disposable, EventEmitter,
)


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
        f"unsupported/needsExtensionRuntime: LanguageModelTool '{name}' {detail}")


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
        self._clipboard_text = ""
        self._active_text_editor: Optional[_TextEditor] = None
        self._visible_text_editors: List[_TextEditor] = []
        self._text_documents: List[_TextDocument] = []
        self._terminals: List[_Terminal] = []
        self._chat_participants: Dict[str, ChatParticipant] = {}
        self._lm_tools: Dict[str, Any] = {}
        self._auth_providers: Dict[str, Any] = {}
        self._auth_sessions: Dict[str, List[AuthenticationSession]] = {}
        self._variables: Dict[str, Callable] = {}
        self._lm_providers: Dict[str, Any] = {}
        self._language_providers: Dict[str, List[Any]] = {}
        self._diagnostic_collections: Dict[str, Any] = {}
        self._diagnostics_change_emitter = EventEmitter()
        self._window_active_text_editor_emitter = EventEmitter()
        self._window_visible_text_editors_emitter = EventEmitter()
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
        self._task_providers: Dict[str, Any] = {}
        self._debug_providers: Dict[str, Any] = {}
        self._config_change_emitter = EventEmitter()
        self._tools_change_emitter = EventEmitter()
        self._models_change_emitter = EventEmitter()
        self._window_api: Optional[Dict[str, Any]] = None
        self._workspace_api: Optional[Dict[str, Any]] = None
        self._auth_api: Optional[Dict[str, Any]] = None

    def build(self, ext: ExtensionDescription = None) -> Dict[str, Any]:
        """Return a dict that serves as the ``vscode`` module for an extension."""
        extension_id = ext.id if ext else ""
        return {
            # Namespaces
            "commands": self._build_commands(),
            "window": self._build_window(),
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
                "showQuickPick": lambda items, **kw: items[0] if items else None,
                "showInputBox": lambda **kw: kw.get("value", ""),
                "createOutputChannel": lambda name, **kw: _OutputChannel(name),
                "createStatusBarItem": lambda *a, **kw: _StatusBarItem(),
                "createWebviewPanel": lambda vt, title, col, **kw: _WebviewPanel(vt, title),
                "showTextDocument": self._show_text_document,
                "createTreeView": self._create_tree_view,
                "registerTreeDataProvider": self._register_tree_data_provider,
                "registerWebviewViewProvider": self._register_webview_view_provider,
                "createTerminal": self._create_terminal,
                "withProgress": lambda opts, task: task(_DummyProgress(), CancellationToken.NONE),
                "activeTextEditor": None,
                "visibleTextEditors": [],
                "terminals": [],
                "onDidChangeActiveTextEditor": self._window_active_text_editor_emitter.event,
                "onDidChangeVisibleTextEditors": self._window_visible_text_editors_emitter.event,
                "onDidChangeActiveTerminal": EventEmitter().event,
                "onDidOpenTerminal": self._window_open_terminal_emitter.event,
                "onDidCloseTerminal": self._window_close_terminal_emitter.event,
                "tabGroups": {"all": [], "activeTabGroup": None,
                              "onDidChangeTabGroups": EventEmitter().event,
                              "onDidChangeTabs": EventEmitter().event},
            }
        self._sync_window_state()
        return self._window_api

    def _sync_window_state(self) -> None:
        if self._window_api is None:
            return
        self._window_api["activeTextEditor"] = self._active_text_editor
        self._window_api["visibleTextEditors"] = list(self._visible_text_editors)
        self._window_api["terminals"] = list(self._terminals)

    def _show_message(self, level: str, message: Any,
                      items: Sequence[Any]) -> Any:
        print(f"[{level}] {message}")
        return items[0] if items else None

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
                                        options: Dict[str, Any] = None) -> Disposable:
        self._webview_view_providers[view_id] = {
            "provider": provider,
            "options": dict(options or {}),
        }
        view = _WebviewView(view_id)
        self._webview_views[view_id] = view
        if hasattr(provider, "resolveWebviewView"):
            try:
                provider.resolveWebviewView(view, None, CancellationToken.NONE)
            except TypeError:
                provider.resolveWebviewView(view)
        return Disposable(lambda: (self._webview_view_providers.pop(view_id, None),
                                   self._webview_views.pop(view_id, None)))

    def _create_terminal(self, *args: Any, **kw: Any) -> Any:
        name = ""
        if args and isinstance(args[0], str):
            name = args[0]
        elif args and isinstance(args[0], dict):
            name = str(args[0].get("name", ""))
        name = str(kw.get("name") or name or "SAO Terminal")
        terminal = _Terminal(name)
        terminal._on_dispose = lambda t=terminal: self._on_terminal_disposed(t)
        self._terminals.append(terminal)
        self._sync_window_state()
        self._window_open_terminal_emitter.fire(terminal)
        return terminal

    def _on_terminal_disposed(self, terminal: "_Terminal") -> None:
        if terminal in self._terminals:
            self._terminals.remove(terminal)
            self._sync_window_state()
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
                "fs": _FileSystem(),
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
        uri_obj = raw_uri if isinstance(raw_uri, Uri) else Uri.file(path)
        if isinstance(uri_obj, Uri) and uri_obj.scheme != "file":
            return self._remember_text_document(
                _TextDocument(uri_obj, "", str(kw.get("language", "plaintext"))))
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            content = fh.read()
        return self._remember_text_document(
            _TextDocument(Uri.file(path), content, _language_id_for_path(path)))

    def _apply_workspace_edit(self, edit: Any) -> bool:
        if not edit:
            return True
        if isinstance(edit, WorkspaceEdit) and not edit.entries():
            return True
        return False

    def _create_file_system_watcher(self, pattern: Any, *args: Any,
                                    **kw: Any) -> Any:
        watcher = _FileSystemWatcher(pattern)
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
        return watcher

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
        return Disposable()

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
        builtins = ["plaintext", "python", "javascript", "typescript", "json", "markdown"]
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
        return {
            "registerTaskProvider": self._register_task_provider,
            "fetchTasks": lambda filter=None: [],
            "executeTask": lambda task: _unsupported_feature(
                "tasks.executeTask", "Task execution requires a real VSCode task service."),
            "taskExecutions": [],
            "onDidStartTask": EventEmitter().event,
            "onDidEndTask": EventEmitter().event,
        }

    def _register_task_provider(self, task_type: str, provider: Any) -> Disposable:
        self._task_providers[task_type] = provider
        return Disposable(lambda: self._task_providers.pop(task_type, None))

    def _build_debug(self) -> Dict[str, Any]:
        return {
            "registerDebugConfigurationProvider": self._register_debug_provider,
            "startDebugging": lambda folder, name_or_config, parent=None: False,
            "activeDebugSession": None,
            "breakpoints": [],
            "onDidStartDebugSession": EventEmitter().event,
            "onDidTerminateDebugSession": EventEmitter().event,
            "onDidChangeBreakpoints": EventEmitter().event,
        }

    def _register_debug_provider(self, debug_type: str, provider: Any,
                                 trigger_kind: Any = None) -> Disposable:
        self._debug_providers[debug_type] = {
            "provider": provider, "triggerKind": trigger_kind,
        }
        return Disposable(lambda: self._debug_providers.pop(debug_type, None))

    def _build_notebooks(self) -> Dict[str, Any]:
        return {"registerNotebookSerializer": lambda *a, **kw: Disposable()}

    # ── extensions ──

    def _build_extensions(self) -> Dict[str, Any]:
        return {
            "getExtension": lambda ext_id: self._host.registry.get(ext_id),
            "all": self._host.registry.list_all(),
            "onDidChange": EventEmitter().event,
        }

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
        return Disposable(lambda: self._lm_tools.pop(name, None))

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
        self._lm_tools[f"mcp:{provider_id}"] = {
            "provider": provider, "mcp": True}
        return Disposable(lambda: self._lm_tools.pop(f"mcp:{provider_id}", None))

    def _file_is_ignored(self, uri: Any, token: Any = None) -> bool:
        """Check if a file should be ignored (.gitignore/.copilotignore)."""
        path = uri.fs_path if hasattr(uri, "fs_path") else str(uri)
        ignore_patterns = [".git", "__pycache__", "node_modules", ".env"]
        for pat in ignore_patterns:
            if pat in path:
                return True
        return False

    def _register_ignored_file_provider(self, provider: Any) -> Disposable:
        """Register a file-ignore provider."""
        return Disposable()

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
                    pass  # UI would show confirmation — we auto-approve in shim
            except Exception:
                pass
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
            "registerChatWorkspaceContextProvider": lambda id, p: Disposable(),
            "registerChatExplicitContextProvider": lambda id, p: Disposable(),
            "registerChatResourceContextProvider": lambda id, p: Disposable(),
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
        if options and options.get("createIfNone"):
            prov = self._auth_providers.get(provider_id)
            if prov and hasattr(prov, "create_session"):
                session = prov.create_session(scopes or [], options)
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
        external_disposable = None
        try:
            from ai_editor.auth import get_auth_service
            external_disposable = get_auth_service().register_provider(
                provider_id, label, provider)
        except Exception:
            external_disposable = None

        def _dispose() -> None:
            self._auth_providers.pop(provider_id, None)
            if external_disposable:
                external_disposable.dispose()
        return Disposable(_dispose)

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
    def __init__(self, name: str = "") -> None:
        self.name = name
        self._value = ""
    def append(self, value: str) -> None:
        self._value += str(value)
    def appendLine(self, value: str) -> None:
        self._value += str(value) + "\n"
        print(f"[{self.name}] {value}")
    def clear(self) -> None:
        self._value = ""
    def show(self, **kw) -> None:
        pass
    def dispose(self) -> None:
        pass


class _StatusBarItem:
    def __init__(self) -> None:
        self.text = ""
        self.tooltip = ""
        self.command = ""
        self.visible = False
    def show(self) -> None:
        self.visible = True
    def hide(self) -> None:
        self.visible = False
    def dispose(self) -> None:
        pass


class _Terminal:
    def __init__(self, name: str = "") -> None:
        self.name = name
        self.processId = None
        self.creationOptions = {}
        self.exitStatus = None
        self.state = {"isInteractedWith": False}
        self._disposed = False
        self._on_dispose: Optional[Callable[[], None]] = None

    def sendText(self, text: str, addNewLine: bool = True) -> None:
        self.state["isInteractedWith"] = True
        suffix = "\n" if addNewLine else ""
        print(f"[terminal:{self.name}] {text}{suffix}", end="")

    def show(self, preserveFocus: bool = False) -> None:
        pass

    def hide(self) -> None:
        pass

    def dispose(self) -> None:
        self._disposed = True
        if self._on_dispose:
            try:
                self._on_dispose()
            except Exception:
                pass


class _DummyProgress:
    def report(self, value: Any = None) -> None:
        pass


class _WebviewPanel:
    def __init__(self, view_type: str = "", title: str = "") -> None:
        self.view_type = view_type
        self.title = title
        self.viewColumn = 1
        self.options: Dict[str, Any] = {}
        self.webview = _Webview()
        self.visible = True
        self.active = True
        self._dispose_emitter = EventEmitter()

    @property
    def on_did_dispose(self):
        return self._dispose_emitter.event

    @property
    def onDidDispose(self):
        return self._dispose_emitter.event

    def reveal(self, *a, **kw) -> None:
        self.visible = True
        self.active = True

    def dispose(self) -> None:
        self.visible = False
        self.active = False
        self._dispose_emitter.fire()


class _Webview:
    def __init__(self) -> None:
        self.html = ""
        self.options = {}
        self._message_emitter = EventEmitter()

    @property
    def on_did_receive_message(self):
        return self._message_emitter.event

    @property
    def onDidReceiveMessage(self):
        return self._message_emitter.event

    def post_message(self, message: Any) -> bool:
        self._message_emitter.fire(message)
        return True

    def postMessage(self, message: Any) -> bool:
        return self.post_message(message)

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
    def __init__(self, view_id: str) -> None:
        self.viewType = view_id
        self.title = view_id
        self.description = ""
        self.badge = None
        self.visible = True
        self.webview = _Webview()
        self._dispose_emitter = EventEmitter()

    @property
    def onDidDispose(self):
        return self._dispose_emitter.event

    def show(self, preserveFocus: bool = False) -> None:
        self.visible = True

    def dispose(self) -> None:
        self.visible = False
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
        self._replacement: Optional[str] = None

    def replace(self, range: Any, text: str) -> None:
        self._replacement = str(text or "")

    def insert(self, position: Any, text: str) -> None:
        if self._document is None:
            self._replacement = str(text or "")
        else:
            self._replacement = self._document.getText() + str(text or "")

    def delete(self, range: Any) -> None:
        self._replacement = ""

    def apply(self) -> bool:
        if self._document is None or self._replacement is None:
            return False
        self._document._content = self._replacement
        self._document.isDirty = True
        self._document.version += 1
        return True


class _FileSystemWatcher:
    def __init__(self, pattern: Any) -> None:
        self.globPattern = pattern
        self.ignoreCreateEvents = False
        self.ignoreChangeEvents = False
        self.ignoreDeleteEvents = False
        self._create = EventEmitter()
        self._change = EventEmitter()
        self._delete = EventEmitter()

    @property
    def onDidCreate(self):
        return self._create.event

    @property
    def onDidChange(self):
        return self._change.event

    @property
    def onDidDelete(self):
        return self._delete.event

    def dispose(self) -> None:
        self._create._listeners.clear()
        self._change._listeners.clear()
        self._delete._listeners.clear()


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
    def readFile(self, uri: Any) -> bytes:
        path = uri.fs_path if hasattr(uri, "fs_path") else str(uri)
        with open(path, "rb") as f:
            return f.read()
    def writeFile(self, uri: Any, content: bytes) -> None:
        path = uri.fs_path if hasattr(uri, "fs_path") else str(uri)
        with open(path, "wb") as f:
            f.write(content)
    def stat(self, uri: Any) -> Dict:
        path = uri.fs_path if hasattr(uri, "fs_path") else str(uri)
        s = os.stat(path)
        return {"type": 1 if os.path.isfile(path) else 2,
                "size": s.st_size, "mtime": int(s.st_mtime * 1000)}
    def readDirectory(self, uri: Any) -> List:
        path = uri.fs_path if hasattr(uri, "fs_path") else str(uri)
        return [(n, 1 if os.path.isfile(os.path.join(path, n)) else 2)
                for n in os.listdir(path)]
    def createDirectory(self, uri: Any) -> None:
        path = uri.fs_path if hasattr(uri, "fs_path") else str(uri)
        os.makedirs(path, exist_ok=True)
    def delete(self, uri: Any, options: Dict = None) -> None:
        import shutil
        path = uri.fs_path if hasattr(uri, "fs_path") else str(uri)
        if os.path.isdir(path):
            shutil.rmtree(path)
        elif os.path.isfile(path):
            os.remove(path)
