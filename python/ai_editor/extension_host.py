"""VSCode-compatible extension host for the SAO AI Editor.

Implements P0 of VSCode extension alignment:
  - ExtensionDescription  parse package.json into internal model
  - ExtensionRegistry     central index, activation event map
  - ExtensionScanner      discover extensions from directories
  - ActivationEvents      *, onCommand, onLanguage, onStartupFinished
  - ExtensionActivator    dependency resolution + activate/deactivate lifecycle
  - CommandService        registerCommand / executeCommand
  - ExtensionContext      subscriptions, extensionPath, globalState, secrets
  - ExtensionPoints       process contributes (commands, chatParticipants, languageModelTools, etc.)
  - Loading Pipeline      scan -> register -> process contributes -> activate

Extensions are loaded in-process (Python): package.json is parsed for
contributes, and Python/JS entry points are called if present. Full
Node.js extension host is NOT implemented — we provide a compatibility
shim that maps VSCode contributes to our native API.
"""

from __future__ import annotations

import json
import os
import threading
import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Set


# ---------------------------------------------------------------------------
# ExtensionDescription — parsed from package.json
# ---------------------------------------------------------------------------

@dataclass
class ExtensionDescription:
    id: str = ""
    name: str = ""
    display_name: str = ""
    publisher: str = ""
    version: str = ""
    description: str = ""
    main: str = ""
    browser: str = ""
    icon: str = ""
    categories: List[str] = field(default_factory=list)
    activation_events: List[str] = field(default_factory=list)
    extension_dependencies: List[str] = field(default_factory=list)
    contributes: Dict[str, Any] = field(default_factory=dict)
    extension_path: str = ""
    extension_kind: str = "workspace"
    is_builtin: bool = False
    enabled: bool = True

    @classmethod
    def from_package_json(cls, pkg: Dict[str, Any],
                          ext_path: str = "") -> ExtensionDescription:
        publisher = pkg.get("publisher", "unknown")
        name = pkg.get("name", "")
        return cls(
            id=f"{publisher}.{name}",
            name=name,
            display_name=pkg.get("displayName", name),
            publisher=publisher,
            version=pkg.get("version", "0.0.0"),
            description=pkg.get("description", ""),
            main=pkg.get("main", ""),
            browser=pkg.get("browser", ""),
            icon=pkg.get("icon", ""),
            categories=list(pkg.get("categories", [])),
            activation_events=list(pkg.get("activationEvents", [])),
            extension_dependencies=list(pkg.get("extensionDependencies", [])),
            contributes=dict(pkg.get("contributes", {})),
            extension_path=ext_path,
            extension_kind=_parse_kind(pkg.get("extensionKind")),
        )

    def to_dict(self) -> Dict[str, Any]:
        return {
            "id": self.id, "name": self.name,
            "displayName": self.display_name,
            "publisher": self.publisher, "version": self.version,
            "description": self.description, "icon": self.icon,
            "categories": self.categories,
            "activationEvents": self.activation_events,
            "extensionPath": self.extension_path,
            "enabled": self.enabled, "isBuiltin": self.is_builtin,
            "contributes_keys": list(self.contributes.keys()),
        }


def _parse_kind(raw: Any) -> str:
    if isinstance(raw, list) and raw:
        return raw[0]
    if isinstance(raw, str):
        return raw
    return "workspace"


class _LMAccessInfo:
    """Compatibility shim for ExtensionContext.languageModelAccessInformation."""

    def __init__(self) -> None:
        self._change_emitter = _LazyEventEmitter()

    @property
    def on_did_change(self):
        return self._change_emitter.event

    def can_send_request(self, chat: Any = None) -> bool:
        return True


class _LazyEventEmitter:
    """Deferred EventEmitter to avoid circular imports."""

    def __init__(self) -> None:
        self._listeners: List[Any] = []

    @property
    def event(self):
        return self._subscribe

    def _subscribe(self, listener) -> Any:
        self._listeners.append(listener)
        class _D:
            def dispose(_self):
                try:
                    self._listeners.remove(listener)
                except ValueError:
                    pass
        return _D()

    def fire(self, data=None):
        for fn in list(self._listeners):
            try:
                fn(data)
            except Exception:
                pass


def _needs_extension_runtime(
        contribution: str,
        extension_id: str,
        identifier: str,
        **details: Any) -> Dict[str, Any]:
    result: Dict[str, Any] = {
        "ok": False,
        "unsupported": True,
        "needsExtensionRuntime": True,
        "code": "needsExtensionRuntime",
        "contribution": contribution,
        "extensionId": extension_id,
        "id": identifier,
        "message": (
            f"VSCode {contribution} '{identifier}' from extension "
            f"'{extension_id}' needs a real extension runtime handler."
        ),
    }
    result.update(details)
    return result


def _unsupported_command_handler(extension_id: str,
                                 command_id: str) -> Callable:
    def _handler(*args: Any) -> Dict[str, Any]:
        return _needs_extension_runtime(
            "command", extension_id, command_id, arguments=list(args))

    setattr(_handler, "_needs_extension_activation", True)
    return _handler


def _mark_manifest_only(item: Dict[str, Any], contribution: str,
                        extension_id: str, identifier: str) -> Dict[str, Any]:
    item["_runtimeSupport"] = _needs_extension_runtime(
        contribution, extension_id, identifier)
    return item


# ---------------------------------------------------------------------------
# ExtensionRegistry — central index + activation event map
# ---------------------------------------------------------------------------

class ExtensionRegistry:

    def __init__(self) -> None:
        self._extensions: Dict[str, ExtensionDescription] = {}
        self._activation_map: Dict[str, List[str]] = {}
        self._lock = threading.Lock()
        self._dirty = True

    def register(self, ext: ExtensionDescription) -> None:
        with self._lock:
            self._extensions[ext.id] = ext
            self._dirty = True

    def unregister(self, ext_id: str) -> None:
        with self._lock:
            self._extensions.pop(ext_id, None)
            self._dirty = True

    def get(self, ext_id: str) -> Optional[ExtensionDescription]:
        return self._extensions.get(ext_id)

    def list_all(self) -> List[ExtensionDescription]:
        return list(self._extensions.values())

    def get_for_activation_event(self, event: str) -> List[ExtensionDescription]:
        with self._lock:
            if self._dirty:
                self._rebuild_activation_map()
                self._dirty = False
            ids = self._activation_map.get(event, [])
            # Also check prefix-matched events (e.g., onLanguage:python matches onLanguage:*)
            if ":" in event:
                prefix = event.split(":")[0] + ":*"
                ids = ids + self._activation_map.get(prefix, [])
            star_ids = self._activation_map.get("*", [])
            combined = list(dict.fromkeys(ids + star_ids))
            return [self._extensions[eid] for eid in combined
                    if eid in self._extensions and self._extensions[eid].enabled]

    def _rebuild_activation_map(self) -> None:
        m: Dict[str, List[str]] = {}
        for ext in self._extensions.values():
            if not ext.enabled:
                continue
            events = ext.activation_events or []
            if not events:
                events = self._implicit_events(ext)
            for ev in events:
                m.setdefault(ev, []).append(ext.id)
        self._activation_map = m

    @staticmethod
    def _implicit_events(ext: ExtensionDescription) -> List[str]:
        events = []
        contribs = ext.contributes
        for cmd in contribs.get("commands", []):
            if isinstance(cmd, dict) and cmd.get("command"):
                events.append(f"onCommand:{cmd['command']}")
        for cp in contribs.get("chatParticipants", []):
            if isinstance(cp, dict):
                events.append("*")
                break
        if contribs.get("languageModelTools"):
            events.append("*")
        if contribs.get("authentication"):
            events.append("*")
        for vl in contribs.get("views", {}).values():
            if isinstance(vl, list):
                for v in vl:
                    vid = v.get("id", "") if isinstance(v, dict) else ""
                    if vid:
                        events.append(f"onView:{vid}")
        for lang in contribs.get("languages", []):
            if isinstance(lang, dict) and lang.get("id"):
                events.append(f"onLanguage:{lang['id']}")
        for tp in contribs.get("terminal", []):
            if isinstance(tp, dict) and tp.get("id"):
                events.append(f"onTerminalProfile:{tp['id']}")
        for ce in contribs.get("customEditors", []):
            if isinstance(ce, dict) and ce.get("viewType"):
                events.append(f"onCustomEditor:{ce['viewType']}")
        return events


# ---------------------------------------------------------------------------
# ExtensionScanner — discover extensions from directories
# ---------------------------------------------------------------------------

class ExtensionScanner:

    @staticmethod
    def scan_directory(directory: str,
                       is_builtin: bool = False) -> List[ExtensionDescription]:
        results: List[ExtensionDescription] = []
        if not os.path.isdir(directory):
            return results
        for entry in sorted(os.listdir(directory)):
            ext_dir = os.path.join(directory, entry)
            pkg_path = os.path.join(ext_dir, "package.json")
            if not os.path.isfile(pkg_path):
                continue
            try:
                with open(pkg_path, "r", encoding="utf-8") as f:
                    pkg = json.load(f)
                desc = ExtensionDescription.from_package_json(pkg, ext_dir)
                desc.is_builtin = is_builtin
                results.append(desc)
            except Exception:
                continue
        return results

    @staticmethod
    def scan_vsix_extracted(ext_dir: str) -> Optional[ExtensionDescription]:
        pkg_path = os.path.join(ext_dir, "package.json")
        if not os.path.isfile(pkg_path):
            pkg_path = os.path.join(ext_dir, "extension", "package.json")
        if not os.path.isfile(pkg_path):
            return None
        try:
            with open(pkg_path, "r", encoding="utf-8") as f:
                pkg = json.load(f)
            return ExtensionDescription.from_package_json(
                pkg, os.path.dirname(pkg_path))
        except Exception:
            return None


# ---------------------------------------------------------------------------
# CommandService — registerCommand / executeCommand
# ---------------------------------------------------------------------------

class CommandService:

    def __init__(self) -> None:
        self._commands: Dict[str, Callable] = {}
        self._before_execute: Optional[Callable[[str], None]] = None
        self._lock = threading.Lock()

    def set_before_execute(self,
                           callback: Optional[Callable[[str], None]]) -> None:
        self._before_execute = callback

    def register(self, command_id: str, handler: Callable) -> Callable:
        with self._lock:
            self._commands[command_id] = handler

        def dispose():
            with self._lock:
                self._commands.pop(command_id, None)
        return dispose

    def execute(self, command_id: str, *args: Any) -> Any:
        handler = self._commands.get(command_id)
        if self._before_execute and (
                handler is None or
                getattr(handler, "_needs_extension_activation", False)):
            self._before_execute(command_id)
            handler = self._commands.get(command_id)
        if not handler:
            raise KeyError(f"Command not found: {command_id}")
        return handler(*args)

    def list_commands(self) -> List[str]:
        return sorted(self._commands.keys())

    def has(self, command_id: str) -> bool:
        return command_id in self._commands


# ---------------------------------------------------------------------------
# ExtensionContext — passed to activate(context)
# ---------------------------------------------------------------------------

class ExtensionContext:
    """VSCode-compatible ExtensionContext object."""

    def __init__(self, ext: ExtensionDescription,
                 global_storage_path: str = "") -> None:
        self.extension = ext
        self.extension_path = ext.extension_path
        self.extension_uri = Uri.file(ext.extension_path) if ext.extension_path else None
        self.extension_mode = 1  # Production=1, Development=2, Test=3
        self.subscriptions: List[Any] = []
        self._storage_path = global_storage_path or os.path.join(
            ext.extension_path, ".storage")
        self.global_storage_path = self._storage_path
        self.global_storage_uri = Uri.file(self._storage_path)
        self.storage_path = os.path.join(self._storage_path, "workspace")
        self.storage_uri = Uri.file(self.storage_path)
        self.log_path = os.path.join(self._storage_path, "logs")
        self.log_uri = Uri.file(self.log_path)
        self.environment_variable_collection: Dict[str, str] = {}
        self.language_model_access_information = _LMAccessInfo()
        self._global_state: Dict[str, Any] = {}
        self._workspace_state: Dict[str, Any] = {}
        self._secrets: Dict[str, str] = {}
        self._load_state()

    @property
    def global_state(self) -> "_MemStore":
        return _MemStore(self._global_state, self._save_state)

    @property
    def workspace_state(self) -> "_MemStore":
        return _MemStore(self._workspace_state, self._save_state)

    @property
    def secrets(self) -> "_SecretStore":
        return _SecretStore(self._secrets, self._save_state)

    def as_absolute_path(self, relative: str) -> str:
        return os.path.join(self.extension_path, relative)

    def _state_file(self) -> str:
        os.makedirs(self._storage_path, exist_ok=True)
        return os.path.join(self._storage_path, "state.json")

    def _load_state(self) -> None:
        try:
            with open(self._state_file(), "r", encoding="utf-8") as f:
                data = json.load(f)
            self._global_state = data.get("global", {})
            self._workspace_state = data.get("workspace", {})
            self._secrets = data.get("secrets", {})
        except Exception:
            pass

    def _save_state(self) -> None:
        try:
            with open(self._state_file(), "w", encoding="utf-8") as f:
                json.dump({"global": self._global_state,
                           "workspace": self._workspace_state,
                           "secrets": self._secrets},
                          f, ensure_ascii=False)
        except Exception:
            pass


class _MemStore:
    def __init__(self, store: Dict[str, Any], save: Callable) -> None:
        self._store = store
        self._save = save

    def get(self, key: str, default: Any = None) -> Any:
        return self._store.get(key, default)

    def update(self, key: str, value: Any) -> None:
        self._store[key] = value
        self._save()

    def keys(self) -> List[str]:
        return list(self._store.keys())


class _SecretStore:
    def __init__(self, store: Dict[str, str], save: Callable) -> None:
        self._store = store
        self._save = save

    def get(self, key: str) -> Optional[str]:
        return self._store.get(key)

    def store(self, key: str, value: str) -> None:
        self._store[key] = value
        self._save()

    def delete(self, key: str) -> None:
        self._store.pop(key, None)
        self._save()


# ---------------------------------------------------------------------------
# Extension Points — process contributes section
# ---------------------------------------------------------------------------

class ExtensionPoints:
    """Processes the contributes section of package.json and registers
    commands, chat participants, language model tools, etc."""

    def __init__(self, commands: CommandService) -> None:
        self._commands = commands
        self.enabled_contributions: Optional[Set[str]] = None
        self._command_contributions: List[Dict[str, Any]] = []
        self._chat_participants: List[Dict[str, Any]] = []
        self._lm_tools: List[Dict[str, Any]] = []
        self._lm_tool_sets: List[Dict[str, Any]] = []
        self._menus: Dict[str, List[Dict[str, Any]]] = {}
        self._keybindings: List[Dict[str, Any]] = []
        self._configurations: List[Dict[str, Any]] = []
        self._views: Dict[str, List[Dict[str, Any]]] = {}
        self._view_containers: Dict[str, List[Dict[str, Any]]] = {}
        self._chat_sessions: List[Dict[str, Any]] = []
        self._lm_providers: List[Dict[str, Any]] = []
        self._chat_prompt_files: List[Dict[str, Any]] = []
        self._chat_skills: List[Dict[str, Any]] = []
        self._mcp_providers: List[Dict[str, Any]] = []
        self._terminal_profiles: List[Dict[str, Any]] = []
        self._config_defaults: List[Dict[str, Any]] = []
        self._chat_welcome: List[Dict[str, Any]] = []
        self._interactive_sessions: List[Dict[str, Any]] = []
        self._authentication: List[Dict[str, Any]] = []
        self._languages: List[Dict[str, Any]] = []
        self._grammars: List[Dict[str, Any]] = []
        self._themes: List[Dict[str, Any]] = []
        self._snippets: List[Dict[str, Any]] = []
        self._custom_editors: List[Dict[str, Any]] = []
        self._walkthroughs: List[Dict[str, Any]] = []
        self._debuggers: List[Dict[str, Any]] = []
        self._notebooks: List[Dict[str, Any]] = []
        self._task_definitions: List[Dict[str, Any]] = []

    def process(self, ext: ExtensionDescription) -> None:
        c = ext.contributes
        if not c:
            return
        eid = ext.id
        enabled = self.enabled_contributions

        def _enabled(name: str) -> bool:
            return enabled is None or name in enabled

        for cmd in (c.get("commands", []) if _enabled("commands") else []):
            if isinstance(cmd, dict) and cmd.get("command"):
                cmd_record = dict(cmd)
                cmd_record["_extensionId"] = eid
                _mark_manifest_only(
                    cmd_record, "command", eid,
                    cmd_record.get("command", ""))
                self._command_contributions.append(cmd_record)
                if not self._commands.has(cmd["command"]):
                    self._commands.register(
                        cmd["command"],
                        _unsupported_command_handler(eid, cmd["command"]))

        for cp in (c.get("chatParticipants", []) if _enabled("chatParticipants") else []):
            if isinstance(cp, dict):
                cp = dict(cp)
                cp["_extensionId"] = eid
                _mark_manifest_only(
                    cp, "chatParticipant", eid,
                    cp.get("id") or cp.get("name", ""))
                self._chat_participants.append(cp)

        for tool in (c.get("languageModelTools", []) if _enabled("languageModelTools") else []):
            if isinstance(tool, dict):
                tool = dict(tool)
                tool["_extensionId"] = eid
                _mark_manifest_only(
                    tool, "languageModelTool", eid,
                    tool.get("name", ""))
                self._lm_tools.append(tool)

        for ts in c.get("languageModelToolSets", []):
            if isinstance(ts, dict):
                ts = dict(ts)
                ts["_extensionId"] = eid
                self._lm_tool_sets.append(ts)

        for kb in c.get("keybindings", []):
            if isinstance(kb, dict):
                kb = dict(kb)
                kb["_extensionId"] = eid
                self._keybindings.append(kb)

        cfg = c.get("configuration")
        if cfg:
            items = cfg if isinstance(cfg, list) else [cfg]
            for item in items:
                if isinstance(item, dict):
                    item = dict(item)
                    item["_extensionId"] = eid
                    self._configurations.append(item)

        for loc, vcs in c.get("viewsContainers", {}).items():
            if isinstance(vcs, list):
                for vc in vcs:
                    if isinstance(vc, dict):
                        vc = dict(vc)
                        vc["_extensionId"] = eid
                        self._view_containers.setdefault(loc, []).append(vc)

        for loc, vs in c.get("views", {}).items():
            if isinstance(vs, list):
                for v in vs:
                    if isinstance(v, dict):
                        v = dict(v)
                        v["_extensionId"] = eid
                        self._views.setdefault(loc, []).append(v)

        for cs in c.get("chatSessions", []):
            if isinstance(cs, dict):
                cs = dict(cs)
                cs["_extensionId"] = eid
                self._chat_sessions.append(cs)

        for lmp in c.get("languageModelChatProviders", []):
            if isinstance(lmp, dict):
                lmp = dict(lmp)
                lmp["_extensionId"] = eid
                self._lm_providers.append(lmp)

        for cpf in c.get("chatPromptFiles", []):
            if isinstance(cpf, dict):
                cpf = dict(cpf)
                cpf["_extensionId"] = eid
                self._chat_prompt_files.append(cpf)

        for cs in c.get("chatSkills", []):
            if isinstance(cs, dict):
                cs = dict(cs)
                cs["_extensionId"] = eid
                self._chat_skills.append(cs)

        for mp in c.get("mcpServerDefinitionProviders", []):
            if isinstance(mp, dict):
                mp = dict(mp)
                mp["_extensionId"] = eid
                self._mcp_providers.append(mp)

        for tp in c.get("terminal", []):
            if isinstance(tp, dict):
                tp = dict(tp)
                tp["_extensionId"] = eid
                self._terminal_profiles.append(tp)

        cfg_defaults = c.get("configurationDefaults")
        if cfg_defaults:
            items = cfg_defaults if isinstance(cfg_defaults, list) else [cfg_defaults]
            for item in items:
                if isinstance(item, dict):
                    item = dict(item)
                    item["_extensionId"] = eid
                    self._config_defaults.append(item)

        for cw in c.get("chatViewsWelcome", []):
            if isinstance(cw, dict):
                cw = dict(cw)
                cw["_extensionId"] = eid
                self._chat_welcome.append(cw)

        for isess in c.get("interactiveSession", []):
            if isinstance(isess, dict):
                isess = dict(isess)
                isess["_extensionId"] = eid
                self._interactive_sessions.append(isess)

        for auth in c.get("authentication", []):
            if isinstance(auth, dict):
                auth = dict(auth)
                auth["_extensionId"] = eid
                self._authentication.append(auth)

        for lang in c.get("languages", []):
            if isinstance(lang, dict):
                lang = dict(lang)
                lang["_extensionId"] = eid
                self._languages.append(lang)

        for gram in c.get("grammars", []):
            if isinstance(gram, dict):
                gram = dict(gram)
                gram["_extensionId"] = eid
                self._grammars.append(gram)

        for theme in c.get("themes", []):
            if isinstance(theme, dict):
                theme = dict(theme)
                theme["_extensionId"] = eid
                self._themes.append(theme)
        for theme in c.get("iconThemes", []):
            if isinstance(theme, dict):
                theme = dict(theme)
                theme["_extensionId"] = eid
                self._themes.append(theme)

        for snip in c.get("snippets", []):
            if isinstance(snip, dict):
                snip = dict(snip)
                snip["_extensionId"] = eid
                self._snippets.append(snip)

        for ce in c.get("customEditors", []):
            if isinstance(ce, dict):
                ce = dict(ce)
                ce["_extensionId"] = eid
                self._custom_editors.append(ce)

        for wt in c.get("walkthroughs", []):
            if isinstance(wt, dict):
                wt = dict(wt)
                wt["_extensionId"] = eid
                self._walkthroughs.append(wt)

        for dbg in c.get("debuggers", []):
            if isinstance(dbg, dict):
                dbg = dict(dbg)
                dbg["_extensionId"] = eid
                self._debuggers.append(dbg)

        for nb in c.get("notebooks", []):
            if isinstance(nb, dict):
                nb = dict(nb)
                nb["_extensionId"] = eid
                self._notebooks.append(nb)

        for td in c.get("taskDefinitions", []):
            if isinstance(td, dict):
                td = dict(td)
                td["_extensionId"] = eid
                self._task_definitions.append(td)

        menus = c.get("menus", {})
        if isinstance(menus, dict):
            for ctx, items in menus.items():
                if isinstance(items, list):
                    for m in items:
                        if isinstance(m, dict):
                            m = dict(m)
                            m["_extensionId"] = eid
                            self._menus.setdefault(ctx, []).append(m)

    @property
    def command_contributions(self) -> List[Dict[str, Any]]:
        return list(self._command_contributions)

    @property
    def chat_participants(self) -> List[Dict[str, Any]]:
        return list(self._chat_participants)

    @property
    def language_model_tools(self) -> List[Dict[str, Any]]:
        return list(self._lm_tools)

    def to_summary(self) -> Dict[str, Any]:
        return {
            "commands": self._commands.list_commands(),
            "commandsContributed": len(self._command_contributions),
            "chatParticipants": len(self._chat_participants),
            "languageModelTools": len(self._lm_tools),
            "languageModelToolSets": len(self._lm_tool_sets),
            "needsExtensionRuntime": {
                "commands": sum(
                    1 for cmd in self._command_contributions
                    if cmd.get("_runtimeSupport", {}).get("needsExtensionRuntime")),
                "chatParticipants": sum(
                    1 for cp in self._chat_participants
                    if cp.get("_runtimeSupport", {}).get("needsExtensionRuntime")),
                "languageModelTools": sum(
                    1 for tool in self._lm_tools
                    if tool.get("_runtimeSupport", {}).get("needsExtensionRuntime")),
            },
            "chatSessions": len(self._chat_sessions),
            "languageModelChatProviders": len(self._lm_providers),
            "keybindings": len(self._keybindings),
            "configurations": len(self._configurations),
            "views": sum(len(v) for v in self._views.values()),
            "viewContainers": sum(len(v) for v in self._view_containers.values()),
            "menus": sum(len(v) for v in self._menus.values()),
            "chatPromptFiles": len(self._chat_prompt_files),
            "chatSkills": len(self._chat_skills),
            "mcpServerDefinitionProviders": len(self._mcp_providers),
            "terminalProfiles": len(self._terminal_profiles),
            "configurationDefaults": len(self._config_defaults),
            "chatViewsWelcome": len(self._chat_welcome),
            "interactiveSessions": len(self._interactive_sessions),
            "authentication": len(self._authentication),
            "languages": len(self._languages),
            "grammars": len(self._grammars),
            "themes": len(self._themes),
            "snippets": len(self._snippets),
            "customEditors": len(self._custom_editors),
            "walkthroughs": len(self._walkthroughs),
            "debuggers": len(self._debuggers),
            "notebooks": len(self._notebooks),
            "taskDefinitions": len(self._task_definitions),
        }


# ---------------------------------------------------------------------------
# ActivatedExtension — runtime state of an activated extension
# ---------------------------------------------------------------------------

@dataclass
class ActivatedExtension:
    ext: ExtensionDescription
    context: ExtensionContext
    exports: Any = None
    activation_time_ms: float = 0.0
    failed: bool = False
    error: Optional[str] = None


# ---------------------------------------------------------------------------
# ExtensionActivator — dependency resolution + lifecycle
# ---------------------------------------------------------------------------

class ExtensionActivator:

    def __init__(self, registry: ExtensionRegistry,
                 commands: CommandService,
                 ext_points: ExtensionPoints) -> None:
        self._registry = registry
        self._commands = commands
        self._ext_points = ext_points
        self._activated: Dict[str, ActivatedExtension] = {}
        self._activating: Set[str] = set()
        self._lock = threading.Lock()

    def activate_by_event(self, event: str) -> List[ActivatedExtension]:
        targets = self._registry.get_for_activation_event(event)
        results = []
        for ext in targets:
            act = self.activate(ext.id)
            if act:
                results.append(act)
        return results

    def activate(self, ext_id: str) -> Optional[ActivatedExtension]:
        with self._lock:
            if ext_id in self._activated:
                return self._activated[ext_id]
            if ext_id in self._activating:
                return None
            self._activating.add(ext_id)

        ext = self._registry.get(ext_id)
        if not ext or not ext.enabled:
            self._activating.discard(ext_id)
            return None

        for dep_id in ext.extension_dependencies:
            if dep_id not in self._activated:
                self.activate(dep_id)

        t0 = time.monotonic()
        ctx = ExtensionContext(ext)
        self._ext_points.process(ext)

        activated = ActivatedExtension(
            ext=ext, context=ctx,
            activation_time_ms=(time.monotonic() - t0) * 1000)

        with self._lock:
            self._activated[ext_id] = activated
            self._activating.discard(ext_id)

        return activated

    def deactivate(self, ext_id: str) -> None:
        with self._lock:
            act = self._activated.pop(ext_id, None)
        if act:
            for sub in act.context.subscriptions:
                if callable(sub):
                    try:
                        sub()
                    except Exception:
                        pass

    def deactivate_all(self) -> None:
        ids = list(self._activated.keys())
        for eid in reversed(ids):
            self.deactivate(eid)

    def is_activated(self, ext_id: str) -> bool:
        return ext_id in self._activated

    def get_activated(self, ext_id: str) -> Optional[ActivatedExtension]:
        return self._activated.get(ext_id)

    def list_activated(self) -> List[ActivatedExtension]:
        return list(self._activated.values())


# ---------------------------------------------------------------------------
# VscodeTypes — minimal type exports for compatibility
# ---------------------------------------------------------------------------

class Position:
    __slots__ = ('line', 'character')
    def __init__(self, line: int = 0, character: int = 0):
        self.line = line
        self.character = character

class Range:
    __slots__ = ('start', 'end')
    def __init__(self, start: Position = None, end: Position = None):
        self.start = start or Position()
        self.end = end or Position()

class Uri:
    __slots__ = ('scheme', 'authority', 'path', 'query', 'fragment')
    def __init__(self, scheme: str = "file", path: str = "",
                 authority: str = "", query: str = "", fragment: str = ""):
        self.scheme = scheme
        self.authority = authority
        self.path = path
        self.query = query
        self.fragment = fragment

    @staticmethod
    def file(path: str) -> "Uri":
        return Uri(scheme="file", path=path.replace("\\", "/"))

    @staticmethod
    def parse(value: str) -> "Uri":
        if "://" in value:
            scheme, rest = value.split("://", 1)
            return Uri(scheme=scheme, path=rest)
        return Uri.file(value)

    def to_string(self) -> str:
        return f"{self.scheme}://{self.authority}{self.path}"

    @property
    def fs_path(self) -> str:
        return self.path.replace("/", os.sep)

class Disposable:
    def __init__(self, fn: Callable = None):
        self._fn = fn
    def dispose(self):
        if self._fn:
            self._fn()
            self._fn = None

class EventEmitter:
    def __init__(self):
        self._listeners: List[Callable] = []
    @property
    def event(self):
        return self._subscribe
    def _subscribe(self, listener: Callable) -> Disposable:
        self._listeners.append(listener)
        return Disposable(lambda: self._listeners.remove(listener)
                          if listener in self._listeners else None)
    def fire(self, data: Any = None):
        for fn in list(self._listeners):
            try:
                fn(data)
            except Exception:
                pass


# ---------------------------------------------------------------------------
# ExtensionHost — top-level orchestrator (Loading Pipeline)
# ---------------------------------------------------------------------------

class ExtensionHost:
    """Manages the full extension lifecycle.

    Usage::

        host = ExtensionHost()
        host.scan(["/path/to/extensions", "/another/path"])
        host.start()  # activate * and onStartupFinished
        host.activate_event("onCommand:myExt.run")
        ...
        host.shutdown()
    """

    def __init__(self) -> None:
        self.registry = ExtensionRegistry()
        self.commands = CommandService()
        self.commands.set_before_execute(self._activate_command_extension)
        self.ext_points = ExtensionPoints(self.commands)
        self.activator = ExtensionActivator(
            self.registry, self.commands, self.ext_points)
        self._started = False
        self._policy: Optional[Callable[[ExtensionDescription], bool]] = None

    def _activate_command_extension(self, command_id: str) -> None:
        self.activate_event(f"onCommand:{command_id}")

    def set_policy(self, policy: Optional[Callable[[ExtensionDescription], bool]] = None,
                   enabled_contributions: Optional[Set[str]] = None) -> None:
        self._policy = policy
        self.ext_points.enabled_contributions = enabled_contributions

    def _allowed_by_policy(self, ext: ExtensionDescription) -> bool:
        return True if self._policy is None else bool(self._policy(ext))

    def scan(self, directories: List[str],
             builtin_dirs: Optional[List[str]] = None) -> int:
        count = 0
        for d in (builtin_dirs or []):
            for ext in ExtensionScanner.scan_directory(d, is_builtin=True):
                if not self._allowed_by_policy(ext):
                    continue
                self.registry.register(ext)
                count += 1
        for d in directories:
            for ext in ExtensionScanner.scan_directory(d, is_builtin=False):
                if not self._allowed_by_policy(ext):
                    continue
                self.registry.register(ext)
                count += 1
        return count

    def install_from_dir(self, ext_dir: str) -> Optional[ExtensionDescription]:
        desc = ExtensionScanner.scan_vsix_extracted(ext_dir)
        if desc and self._allowed_by_policy(desc):
            self.registry.register(desc)
            if self._started:
                self.activator.activate(desc.id)
        return desc

    def start(self) -> List[ActivatedExtension]:
        self._started = True
        results = self.activator.activate_by_event("*")
        results += self.activator.activate_by_event("onStartupFinished")
        return results

    def activate_event(self, event: str) -> List[ActivatedExtension]:
        return self.activator.activate_by_event(event)

    def shutdown(self) -> None:
        self.activator.deactivate_all()
        self._started = False

    def list_extensions(self) -> List[Dict[str, Any]]:
        result = []
        for ext in self.registry.list_all():
            d = ext.to_dict()
            act = self.activator.get_activated(ext.id)
            d["activated"] = act is not None
            d["activationTimeMs"] = act.activation_time_ms if act else 0
            result.append(d)
        return result

    def get_contributes_summary(self) -> Dict[str, Any]:
        return self.ext_points.to_summary()


# ---------------------------------------------------------------------------
# Singleton
# ---------------------------------------------------------------------------

_host: Optional[ExtensionHost] = None


def get_extension_host() -> ExtensionHost:
    global _host
    if _host is None:
        _host = ExtensionHost()
    return _host
