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
class LanguageModelChat:
    id: str = ""
    name: str = ""
    vendor: str = ""
    family: str = ""
    version: str = ""
    max_input_tokens: int = 128000

    def send_request(self, messages: List[Dict], options: Dict = None,
                     token: Any = None) -> "LanguageModelChatResponse":
        resp = LanguageModelChatResponse()
        if self._engine:
            api_msgs = []
            for m in messages:
                role = m.get("role", "user")
                if isinstance(m.get("content"), str):
                    api_msgs.append({"role": role, "content": m["content"]})
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

    def count_tokens(self, text: str, token: Any = None) -> int:
        if self._engine:
            return self._engine.estimate_tokens(text)
        return len(text) // 4

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
    model: Optional[LanguageModelChat] = None


class ChatContext:
    def __init__(self) -> None:
        self.history: List[Dict] = []


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

    def progress(self, value: str) -> None:
        if self._callback:
            self._callback("progress", value)

    def reference(self, uri: Any, location: Any = None) -> None:
        if self._callback:
            self._callback("reference", str(uri))

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
        self._feedback_emitter = EventEmitter()
        self._action_emitter = EventEmitter()
        self._disposed = False

    @property
    def on_did_receive_feedback(self):
        return self._feedback_emitter.event

    @property
    def on_did_perform_action(self):
        return self._action_emitter.event

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
            "LanguageModelChatMessage": _lm_message,
            "ChatResultFeedback": ChatResult,
            "LanguageModelToolResult": LanguageModelToolResult,
            # Enums
            "ChatVariableLevel": {"Short": 1, "Medium": 2, "Full": 3},
            "LanguageModelChatToolMode": {"Auto": 0, "Required": 1},
            "LanguageModelChatMessageRole": {"System": 0, "User": 1, "Assistant": 2},
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
            "showTextDocument": lambda doc, **kw: None,
            "activeTextEditor": None,
            "visibleTextEditors": [],
            "onDidChangeActiveTextEditor": EventEmitter().event,
        }

    # ── workspace ──

    def _build_workspace(self) -> Dict[str, Any]:
        return {
            "getConfiguration": self._get_configuration,
            "onDidChangeConfiguration": self._config_change_emitter.event,
            "workspaceFolders": self._get_workspace_folders(),
            "rootPath": self._get_root_path(),
            "fs": _FileSystem(),
            "openTextDocument": lambda uri, **kw: None,
            "applyEdit": lambda edit: True,
            "onDidOpenTextDocument": EventEmitter().event,
            "onDidCloseTextDocument": EventEmitter().event,
            "onDidChangeTextDocument": EventEmitter().event,
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
            "invokeTool": self._invoke_tool,
            "getTools": lambda: list(self._lm_tools.keys()),
            "onDidChangeChatModels": EventEmitter().event,
            "onDidChangeTools": self._tools_change_emitter.event,
            "tools": self._lm_tools,
        }

    def _select_chat_models(self, selector: Dict = None) -> List[LanguageModelChat]:
        models = []
        if self._engine:
            from ai_editor.llm_engine import get_model_context
            cfg = self._engine.config
            model_name = cfg.effective_model
            ctx = get_model_context(model_name)
            lm = LanguageModelChat(
                id=model_name, name=model_name,
                vendor=cfg.provider, family=cfg.provider,
                version="1", max_input_tokens=ctx["max_input"])
            lm._engine = self._engine
            if selector:
                if selector.get("vendor") and selector["vendor"] != cfg.provider:
                    return []
                if selector.get("family") and selector["family"] not in model_name:
                    return []
            models.append(lm)
        return models

    def _register_lm_tool(self, name: str, tool: Any) -> Disposable:
        self._lm_tools[name] = tool
        self._tools_change_emitter.fire({"added": name})
        def _dispose():
            self._lm_tools.pop(name, None)
            self._tools_change_emitter.fire({"removed": name})
        return Disposable(_dispose)

    def _register_tool_definition(self, name: str, schema: Dict) -> Disposable:
        """Register a tool by JSON schema (no handler yet — stub until invokeTool wires it)."""
        self._lm_tools[name] = {"schema": schema, "stub": True}
        self._tools_change_emitter.fire({"added": name})
        return Disposable(lambda: self._lm_tools.pop(name, None))

    def _register_lm_provider(self, provider_id: str, provider: Any,
                               metadata: Dict = None) -> Disposable:
        """Register a language model chat provider (extension-contributed model)."""
        self._lm_providers[provider_id] = {
            "provider": provider, "metadata": metadata or {}}
        return Disposable(lambda: self._lm_providers.pop(provider_id, None))

    def _invoke_tool(self, name: str, input_data: Any = None,
                      token: Any = None) -> Any:
        tool = self._lm_tools.get(name)
        if not tool:
            raise KeyError(f"Tool not found: {name}")
        if isinstance(tool, dict) and tool.get("stub"):
            schema = tool.get("schema", {})
            if schema and input_data is not None:
                self._validate_input_schema(name, input_data, schema)
            return LanguageModelToolResult.text(f"[stub] {name}")
        # Validate against inputSchema if the tool exposes one
        if hasattr(tool, "inputSchema") and tool.inputSchema and input_data is not None:
            self._validate_input_schema(name, input_data, tool.inputSchema)
        if hasattr(tool, "invoke"):
            opts = LanguageModelToolInvocationOptions(input=input_data)
            return tool.invoke(opts, token)
        if callable(tool):
            return tool(input_data)
        return LanguageModelToolResult.text(f"[no handler] {name}")

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
# Helper stubs
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
