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
                 handler: Callable) -> None:
        self.id = participant_id
        self.request_handler = handler
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


class AuthenticationProviderBase:
    def __init__(self) -> None:
        self._sessions_changed = EventEmitter()

    @property
    def on_did_change_sessions(self):
        return self._sessions_changed.event

    def get_sessions(self, scopes=None, options=None):
        return []

    def create_session(self, scopes=None, options=None):
        raise NotImplementedError

    def remove_session(self, session_id: str):
        pass


# ---------------------------------------------------------------------------
# Chat Variable types
# ---------------------------------------------------------------------------

@dataclass
class ChatVariableValue:
    level: int = 2  # 1=Short, 2=Medium, 3=Full
    value: str = ""
    description: str = ""


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
        self._chat_participants: Dict[str, ChatParticipant] = {}
        self._lm_tools: Dict[str, Any] = {}
        self._auth_providers: Dict[str, Any] = {}
        self._auth_sessions: Dict[str, List[AuthenticationSession]] = {}
        self._variables: Dict[str, Callable] = {}
        self._lm_providers: Dict[str, Any] = {}
        self._config_change_emitter = EventEmitter()
        self._tools_change_emitter = EventEmitter()
        self._models_change_emitter = EventEmitter()

    def build(self, ext: ExtensionDescription = None) -> Dict[str, Any]:
        """Return a dict that serves as the ``vscode`` module for an extension."""
        return {
            # Namespaces
            "commands": self._build_commands(),
            "window": self._build_window(),
            "workspace": self._build_workspace(),
            "env": self._build_env(),
            "extensions": self._build_extensions(),
            "lm": self._build_lm(),
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
        }

    # ── commands ──

    def _build_commands(self) -> Dict[str, Callable]:
        cmds = self._host.commands
        return {
            "registerCommand": cmds.register,
            "executeCommand": cmds.execute,
            "getCommands": lambda filter_internal=False: cmds.list_commands(),
        }

    # ── window ──

    def _build_window(self) -> Dict[str, Any]:
        return {
            "showInformationMessage": lambda msg, *items: print(f"[info] {msg}"),
            "showWarningMessage": lambda msg, *items: print(f"[warn] {msg}"),
            "showErrorMessage": lambda msg, *items: print(f"[error] {msg}"),
            "showQuickPick": lambda items, **kw: items[0] if items else None,
            "showInputBox": lambda **kw: kw.get("value", ""),
            "createOutputChannel": lambda name, **kw: _OutputChannel(name),
            "createStatusBarItem": lambda *a, **kw: _StatusBarItem(),
            "createWebviewPanel": lambda vt, title, col, **kw: _WebviewPanel(vt, title),
            "showTextDocument": lambda doc, **kw: None,
            "withProgress": lambda opts, task: task(_DummyProgress(), CancellationToken.NONE),
            "activeTextEditor": None,
            "visibleTextEditors": [],
            "terminals": [],
            "onDidChangeActiveTextEditor": EventEmitter().event,
            "onDidChangeVisibleTextEditors": EventEmitter().event,
            "onDidChangeActiveTerminal": EventEmitter().event,
            "onDidOpenTerminal": EventEmitter().event,
            "onDidCloseTerminal": EventEmitter().event,
            "tabGroups": {"all": [], "activeTabGroup": None,
                          "onDidChangeTabGroups": EventEmitter().event,
                          "onDidChangeTabs": EventEmitter().event},
        }

    # ── workspace ──

    def _build_workspace(self) -> Dict[str, Any]:
        return {
            "getConfiguration": self._get_configuration,
            "onDidChangeConfiguration": self._config_change_emitter.event,
            "workspaceFolders": self._get_workspace_folders(),
            "rootPath": self._get_root_path(),
            "name": "SAO Workspace",
            "fs": _FileSystem(),
            "openTextDocument": lambda uri, **kw: None,
            "applyEdit": lambda edit: True,
            "findFiles": lambda include, exclude=None, max_results=None, token=None: [],
            "saveAll": lambda include_untitled=False: True,
            "onDidOpenTextDocument": EventEmitter().event,
            "onDidCloseTextDocument": EventEmitter().event,
            "onDidChangeTextDocument": EventEmitter().event,
            "onDidSaveTextDocument": EventEmitter().event,
            "onDidCreateFiles": EventEmitter().event,
            "onDidDeleteFiles": EventEmitter().event,
            "onDidRenameFiles": EventEmitter().event,
            "onDidChangeWorkspaceFolders": EventEmitter().event,
            "textDocuments": [],
        }

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

    # ── env ──

    def _build_env(self) -> Dict[str, Any]:
        return {
            "appName": "SAO AI Editor",
            "appRoot": self._get_root_path(),
            "language": "zh-cn",
            "uriScheme": "sao-editor",
            "clipboard": {"readText": lambda: "", "writeText": lambda t: None},
            "machineId": "sao-" + os.environ.get("COMPUTERNAME", "local"),
            "sessionId": "",
            "isNewAppInstall": False,
            "isTelemetryEnabled": False,
        }

    # ── extensions ──

    def _build_extensions(self) -> Dict[str, Any]:
        return {
            "getExtension": lambda ext_id: self._host.registry.get(ext_id),
            "all": self._host.registry.list_all(),
            "onDidChange": EventEmitter().event,
        }

    # ── lm (Language Model) ──

    def _build_lm(self) -> Dict[str, Any]:
        return {
            "selectChatModels": self._select_chat_models,
            "registerTool": self._register_lm_tool,
            "registerToolDefinition": self._register_tool_definition,
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

    def _register_lm_tool(self, name: str, tool: Any) -> Disposable:
        self._lm_tools[name] = self._merge_registered_tool(
            self._lm_tools.get(name), tool)
        self._tools_change_emitter.fire({"added": name})
        def _dispose():
            self._lm_tools.pop(name, None)
            self._tools_change_emitter.fire({"removed": name})
        return Disposable(_dispose)

    def _register_tool_definition(self, name: str, schema: Dict) -> Disposable:
        """Register a schema-only tool definition without pretending it can run."""
        record = self._merge_registered_tool(self._lm_tools.get(name), {
            "schema": schema or {},
            "inputSchema": schema or {},
            "description": "Schema-only tool definition has no invoke handler.",
        })
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
        cp = ChatParticipant(participant_id, handler)
        self._chat_participants[participant_id] = cp
        return cp

    def _register_variable(self, name: str, description: str,
                            resolver: Callable) -> Disposable:
        self._variables[name] = resolver
        return Disposable(lambda: self._variables.pop(name, None))

    # ── authentication ──

    def _build_auth(self) -> Dict[str, Any]:
        return {
            "getSession": self._get_auth_session,
            "registerAuthenticationProvider": self._register_auth_provider,
            "onDidChangeSessions": EventEmitter().event,
        }

    def _get_auth_session(self, provider_id: str, scopes: List[str] = None,
                           options: Dict = None) -> Optional[AuthenticationSession]:
        sessions = self._auth_sessions.get(provider_id, [])
        if sessions:
            return sessions[0]
        if options and options.get("createIfNone"):
            prov = self._auth_providers.get(provider_id)
            if prov and hasattr(prov, "create_session"):
                session = prov.create_session(scopes or [], options)
                self._auth_sessions.setdefault(provider_id, []).append(session)
                return session
        return None

    def _register_auth_provider(self, provider_id: str, label: str,
                                 provider: Any, options: Dict = None) -> Disposable:
        self._auth_providers[provider_id] = provider
        return Disposable(lambda: self._auth_providers.pop(provider_id, None))

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
    def append(self, value: str) -> None:
        pass
    def appendLine(self, value: str) -> None:
        print(f"[{self.name}] {value}")
    def clear(self) -> None:
        pass
    def show(self, **kw) -> None:
        pass
    def dispose(self) -> None:
        pass


class _StatusBarItem:
    def __init__(self) -> None:
        self.text = ""
        self.tooltip = ""
        self.command = ""
    def show(self) -> None:
        pass
    def hide(self) -> None:
        pass
    def dispose(self) -> None:
        pass


class _DummyProgress:
    def report(self, value: Any = None) -> None:
        pass


class _WebviewPanel:
    def __init__(self, view_type: str = "", title: str = "") -> None:
        self.view_type = view_type
        self.title = title
        self.webview = _Webview()
        self.visible = True
        self.active = True
        self._dispose_emitter = EventEmitter()

    @property
    def on_did_dispose(self):
        return self._dispose_emitter.event

    def reveal(self, *a, **kw) -> None:
        self.visible = True

    def dispose(self) -> None:
        self.visible = False
        self._dispose_emitter.fire()


class _Webview:
    def __init__(self) -> None:
        self.html = ""
        self.options = {}
        self._message_emitter = EventEmitter()

    @property
    def on_did_receive_message(self):
        return self._message_emitter.event

    def post_message(self, message: Any) -> bool:
        return True

    @property
    def csp_source(self) -> str:
        return ""

    def as_webview_uri(self, uri: Any) -> str:
        return str(uri)


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
