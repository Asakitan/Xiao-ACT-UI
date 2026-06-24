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
contributes. Full Node.js extension activation is not implemented yet, but the
host keeps manifest contributions executable where deterministic fallbacks exist
and reports runtime requirements with contribution metadata.
"""

from __future__ import annotations

import json
import os
import threading
import time
import uuid
from urllib.parse import unquote, urlparse
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
    """Compatibility object for ExtensionContext.languageModelAccessInformation."""

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
            f"'{extension_id}' has no registered runtime callback yet."
        ),
    }
    result.update(details)
    return result


def _activation_metadata(ext: ExtensionDescription,
                         contribution: str,
                         identifier: str) -> Dict[str, Any]:
    declared_events = [
        str(event).strip()
        for event in ext.activation_events
        if str(event or "").strip()
    ]
    implicit_events = ExtensionRegistry._implicit_events(ext)
    matching_events: List[str] = []
    if contribution == "command" and identifier:
        matching_events.append(f"onCommand:{identifier}")
    elif contribution == "view" and identifier:
        matching_events.append(f"onView:{identifier}")
    elif contribution == "language" and identifier:
        matching_events.append(f"onLanguage:{identifier}")
    elif contribution in {
            "chatParticipant", "languageModelTool", "authentication"}:
        matching_events.append("*")
    active_events = declared_events or implicit_events
    effective_matches = [
        event for event in active_events
        if not matching_events or event == "*" or event in matching_events
    ]
    if not effective_matches and matching_events:
        effective_matches = list(matching_events)
    return {
        "declaredEvents": declared_events,
        "implicitEvents": implicit_events,
        "effectiveEvents": effective_matches,
        "usesImplicitEvents": not bool(declared_events),
        "startup": "*" in active_events or "onStartupFinished" in active_events,
    }


def _manifest_command_handler(command_id: str,
                              resolver: Callable[[str, List[Any]], Dict[str, Any]]) -> Callable:
    def _handler(*args: Any) -> Dict[str, Any]:
        return resolver(command_id, list(args))

    setattr(_handler, "_needs_extension_activation", True)
    return _handler


def _mark_manifest_only(item: Dict[str, Any], contribution: str,
                        ext: ExtensionDescription, identifier: str) -> Dict[str, Any]:
    item["_runtimeSupport"] = _needs_extension_runtime(
        contribution, ext.id, identifier)
    item["_activation"] = _activation_metadata(ext, contribution, identifier)
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
        self._change_emitter = _LazyEventEmitter()

    @property
    def on_did_change(self):
        return self._change_emitter.event

    def register(self, ext: ExtensionDescription) -> None:
        previous = None
        with self._lock:
            previous = self._extensions.get(ext.id)
            self._extensions[ext.id] = ext
            self._dirty = True
        self._change_emitter.fire({
            "added": [] if previous else [ext.id],
            "removed": [],
            "changed": [ext.id] if previous else [],
        })

    def unregister(self, ext_id: str) -> None:
        removed = None
        with self._lock:
            removed = self._extensions.pop(ext_id, None)
            self._dirty = True
        if removed is not None:
            self._change_emitter.fire({
                "added": [],
                "removed": [ext_id],
                "changed": [],
            })

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

    def delete(self, key: str) -> None:
        if key in self._store:
            del self._store[key]
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
        self._command_fallback_resolver: Optional[
            Callable[[Dict[str, Any], Dict[str, Any], List[Any]], Optional[Dict[str, Any]]]
        ] = None
        self._command_contributions: List[Dict[str, Any]] = []
        self.configuration_contributions: List[Dict[str, Any]] = []
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
        self._json_validation: List[Dict[str, Any]] = []
        self._yaml_validation: List[Dict[str, Any]] = []
        self._views_welcome: List[Dict[str, Any]] = []
        self._submenus: List[Dict[str, Any]] = []
        self._breakpoints: List[Dict[str, Any]] = []
        self._problem_matchers: List[Dict[str, Any]] = []
        self._problem_patterns: List[Dict[str, Any]] = []
        self._icons: List[Dict[str, Any]] = []
        self._semantic_token_types: List[Dict[str, Any]] = []
        self._semantic_token_modifiers: List[Dict[str, Any]] = []
        self._semantic_token_scopes: List[Dict[str, Any]] = []
        self._resource_label_formatters: List[Dict[str, Any]] = []
        self._typescript_server_plugins: List[Dict[str, Any]] = []
        self._capabilities: List[Dict[str, Any]] = []
        self._custom_editors: List[Dict[str, Any]] = []
        self._walkthroughs: List[Dict[str, Any]] = []
        self._debuggers: List[Dict[str, Any]] = []
        self._notebooks: List[Dict[str, Any]] = []
        self._task_definitions: List[Dict[str, Any]] = []
        self._command_index: Dict[str, Dict[str, Any]] = {}
        self._contributions_by_extension: Dict[str, Dict[str, List[Dict[str, Any]]]] = {}

    def set_command_fallback_resolver(
            self,
            resolver: Optional[
                Callable[[Dict[str, Any], Dict[str, Any], List[Any]], Optional[Dict[str, Any]]]
            ]) -> None:
        self._command_fallback_resolver = resolver

    def _bucket(self, extension_id: str, key: str) -> List[Dict[str, Any]]:
        ext_bucket = self._contributions_by_extension.setdefault(extension_id, {})
        return ext_bucket.setdefault(key, [])

    def _command_fallback_targets(self, extension_id: str) -> List[Dict[str, Any]]:
        bucket = self._contributions_by_extension.get(extension_id, {})
        targets: List[Dict[str, Any]] = []
        for participant in bucket.get("chatParticipants", []):
            participant_id = participant.get("id") or participant.get("name", "")
            if participant_id:
                targets.append({
                    "kind": "chatParticipant",
                    "id": participant_id,
                    "label": participant.get("fullName") or participant.get("name") or participant_id,
                })
        for tool in bucket.get("languageModelTools", []):
            tool_name = tool.get("name", "")
            if tool_name:
                targets.append({
                    "kind": "languageModelTool",
                    "id": tool_name,
                    "label": tool.get("displayName") or tool.get("name") or tool_name,
                })
        for view in bucket.get("views", []):
            view_id = view.get("id", "")
            if view_id:
                view_kind = "view"
                if str(view.get("type") or "").lower() == "webview":
                    view_kind = "webviewView"
                targets.append({
                    "kind": view_kind,
                    "id": view_id,
                    "label": view.get("name") or view_id,
                    "location": view.get("_viewLocation", ""),
                })
        return targets

    def _build_manifest_command_result(
            self,
            command_id: str,
            arguments: List[Any],
            resolve_runtime: bool = True) -> Dict[str, Any]:
        command_record = self._command_index.get(command_id)
        if not command_record:
            return {
                "ok": False,
                "code": "commandNotFound",
                "command": command_id,
                "arguments": list(arguments),
                "message": f"Command not found: {command_id}",
            }
        extension_id = str(command_record.get("_extensionId", ""))
        fallbacks = self._command_fallback_targets(extension_id)
        selected = dict(fallbacks[0]) if len(fallbacks) == 1 else {}
        payload: Dict[str, Any] = {
            "ok": bool(selected),
            "handledBy": "manifestFallback",
            "contribution": "command",
            "extensionId": extension_id,
            "id": command_id,
            "command": dict(command_record),
            "activation": dict(command_record.get("_activation", {})),
            "arguments": list(arguments),
            "availableFallbacks": fallbacks,
            "selectedFallback": selected,
            "needsExtensionRuntime": not bool(selected),
            "code": "manifestCommandFallback" if selected else "needsExtensionRuntime",
            "message": (
                f"Command '{command_id}' resolved to a deterministic manifest fallback."
                if selected else
                f"Command '{command_id}' has no deterministic manifest fallback."
            ),
        }
        if selected and resolve_runtime and self._command_fallback_resolver:
            resolved = self._command_fallback_resolver(
                dict(command_record), dict(selected), list(arguments))
            if isinstance(resolved, dict):
                merged = dict(payload)
                merged.update(resolved)
                merged.setdefault("ok", True)
                merged.setdefault("handledBy", "runtimeFallback")
                merged.setdefault("selectedFallback", selected)
                merged.setdefault("availableFallbacks", fallbacks)
                merged.setdefault("command", dict(command_record))
                merged.setdefault("activation", dict(command_record.get("_activation", {})))
                merged.setdefault("arguments", list(arguments))
                merged.setdefault("extensionId", extension_id)
                merged.setdefault("id", command_id)
                return merged
        if not selected:
            payload.update(_needs_extension_runtime(
                "command", extension_id, command_id,
                availableFallbacks=fallbacks,
                arguments=list(arguments),
                activation=dict(command_record.get("_activation", {})),
                command=dict(command_record),
            ))
            payload["handledBy"] = "manifestFallback"
        return payload

    def resolve_manifest_command(self, command_id: str,
                                 arguments: List[Any]) -> Dict[str, Any]:
        return self._build_manifest_command_result(
            command_id, arguments, resolve_runtime=True)

    def describe_manifest_command(self, command_id: str) -> Dict[str, Any]:
        return self._build_manifest_command_result(
            command_id, [], resolve_runtime=False)

    def get_command_contribution(self, command_id: str) -> Dict[str, Any]:
        return dict(self._command_index.get(command_id, {}))

    @staticmethod
    def _as_contribution_items(raw: Any) -> List[Dict[str, Any]]:
        items = raw if isinstance(raw, list) else [raw]
        return [dict(item) for item in items if isinstance(item, dict)]

    @staticmethod
    def _tag_items(raw: Any, extension_id: str) -> List[Dict[str, Any]]:
        result = []
        for item in ExtensionPoints._as_contribution_items(raw):
            item["_extensionId"] = extension_id
            result.append(item)
        return result

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
                    cmd_record, "command", ext,
                    cmd_record.get("command", ""))
                self._command_contributions.append(cmd_record)
                self._command_index[str(cmd_record.get("command", ""))] = cmd_record
                self._bucket(eid, "commands").append(cmd_record)
                if not self._commands.has(cmd["command"]):
                    self._commands.register(
                        cmd["command"],
                        _manifest_command_handler(
                            cmd["command"], self.resolve_manifest_command))

        for cp in (c.get("chatParticipants", []) if _enabled("chatParticipants") else []):
            if isinstance(cp, dict):
                cp = dict(cp)
                cp["_extensionId"] = eid
                _mark_manifest_only(
                    cp, "chatParticipant", ext,
                    cp.get("id") or cp.get("name", ""))
                self._chat_participants.append(cp)
                self._bucket(eid, "chatParticipants").append(cp)

        for tool in (c.get("languageModelTools", []) if _enabled("languageModelTools") else []):
            if isinstance(tool, dict):
                tool = dict(tool)
                tool["_extensionId"] = eid
                _mark_manifest_only(
                    tool, "languageModelTool", ext,
                    tool.get("name", ""))
                self._lm_tools.append(tool)
                self._bucket(eid, "languageModelTools").append(tool)

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
            self._configurations.extend(self._tag_items(cfg, eid))
            # Build structured configuration_contributions
            cfg_items = cfg if isinstance(cfg, list) else [cfg]
            for ci in cfg_items:
                if not isinstance(ci, dict):
                    continue
                props = ci.get("properties")
                if not isinstance(props, dict) or not props:
                    continue
                self.configuration_contributions.append({
                    "title": ci.get("title", ""),
                    "extension_id": eid,
                    "properties": {
                        k: {sk: sv for sk, sv in v.items()
                             if sk != "_extensionId"}
                        for k, v in props.items()
                        if isinstance(v, dict)
                    },
                })

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
                        v["_viewLocation"] = loc
                        _mark_manifest_only(
                            v, "view", ext,
                            v.get("id") or v.get("name", ""))
                        self._views.setdefault(loc, []).append(v)
                        self._bucket(eid, "views").append(v)

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
            self._config_defaults.extend(self._tag_items(cfg_defaults, eid))

        self._json_validation.extend(self._tag_items(c.get("jsonValidation", []), eid))
        self._yaml_validation.extend(self._tag_items(c.get("yamlValidation", []), eid))
        self._views_welcome.extend(self._tag_items(c.get("viewsWelcome", []), eid))
        self._submenus.extend(self._tag_items(c.get("submenus", []), eid))
        self._breakpoints.extend(self._tag_items(c.get("breakpoints", []), eid))
        self._problem_matchers.extend(self._tag_items(c.get("problemMatchers", []), eid))
        self._problem_patterns.extend(self._tag_items(c.get("problemPatterns", []), eid))
        self._icons.extend(self._tag_items(c.get("icons", []), eid))
        self._semantic_token_types.extend(self._tag_items(c.get("semanticTokenTypes", []), eid))
        self._semantic_token_modifiers.extend(self._tag_items(c.get("semanticTokenModifiers", []), eid))
        self._semantic_token_scopes.extend(self._tag_items(c.get("semanticTokenScopes", []), eid))
        self._resource_label_formatters.extend(self._tag_items(c.get("resourceLabelFormatters", []), eid))
        self._typescript_server_plugins.extend(self._tag_items(c.get("typescriptServerPlugins", []), eid))
        self._capabilities.extend(self._tag_items(c.get("capabilities", []), eid))

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
                auth["_activation"] = _activation_metadata(
                    ext, "authentication",
                    auth.get("id") or auth.get("provider") or auth.get("label", ""))
                auth["_runtimeSupport"] = _needs_extension_runtime(
                    "authentication", ext.id,
                    auth.get("id") or auth.get("provider") or auth.get("label", ""))
                self._authentication.append(auth)

        for lang in c.get("languages", []):
            if isinstance(lang, dict):
                lang = dict(lang)
                lang["_extensionId"] = eid
                lang["_activation"] = _activation_metadata(
                    ext, "language", lang.get("id", ""))
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
                theme["_themeType"] = "color"
                self._themes.append(theme)
        for theme in c.get("iconThemes", []):
            if isinstance(theme, dict):
                theme = dict(theme)
                theme["_extensionId"] = eid
                theme["_themeType"] = "icon"
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
                dbg["_activation"] = _activation_metadata(
                    ext, "debugger", dbg.get("type") or dbg.get("label", ""))
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
                td["_activation"] = _activation_metadata(
                    ext, "taskDefinition", td.get("type") or td.get("taskType", ""))
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

    @property
    def all_contributions(self) -> Dict[str, Any]:
        return {
            "commands": list(self._command_contributions),
            "chatParticipants": list(self._chat_participants),
            "languageModelTools": list(self._lm_tools),
            "languageModelToolSets": list(self._lm_tool_sets),
            "menus": {k: list(v) for k, v in self._menus.items()},
            "keybindings": list(self._keybindings),
            "configuration": list(self._configurations),
            "configurationDefaults": list(self._config_defaults),
            "views": {k: list(v) for k, v in self._views.items()},
            "viewsContainers": {k: list(v) for k, v in self._view_containers.items()},
            "viewsWelcome": list(self._views_welcome),
            "submenus": list(self._submenus),
            "chatSessions": list(self._chat_sessions),
            "languageModelChatProviders": list(self._lm_providers),
            "chatPromptFiles": list(self._chat_prompt_files),
            "chatSkills": list(self._chat_skills),
            "mcpServerDefinitionProviders": list(self._mcp_providers),
            "terminal": list(self._terminal_profiles),
            "interactiveSession": list(self._interactive_sessions),
            "authentication": list(self._authentication),
            "languages": list(self._languages),
            "grammars": list(self._grammars),
            "themes": list(self._themes),
            "snippets": list(self._snippets),
            "jsonValidation": list(self._json_validation),
            "yamlValidation": list(self._yaml_validation),
            "breakpoints": list(self._breakpoints),
            "problemMatchers": list(self._problem_matchers),
            "problemPatterns": list(self._problem_patterns),
            "icons": list(self._icons),
            "semanticTokenTypes": list(self._semantic_token_types),
            "semanticTokenModifiers": list(self._semantic_token_modifiers),
            "semanticTokenScopes": list(self._semantic_token_scopes),
            "resourceLabelFormatters": list(self._resource_label_formatters),
            "typescriptServerPlugins": list(self._typescript_server_plugins),
            "capabilities": list(self._capabilities),
            "customEditors": list(self._custom_editors),
            "walkthroughs": list(self._walkthroughs),
            "debuggers": list(self._debuggers),
            "notebooks": list(self._notebooks),
            "taskDefinitions": list(self._task_definitions),
        }

    @property
    def activity_bar_items(self) -> List[Dict[str, Any]]:
        """Return viewContainers contributed to the activitybar location."""
        return list(self._view_containers.get("activitybar", []))

    @property
    def editor_title_actions(self) -> List[Dict[str, Any]]:
        """Return menu items contributed to editor/title."""
        menus = self.all_contributions.get("menus", {})
        if isinstance(menus, dict):
            return list(menus.get("editor/title", []))
        return []

    def to_summary(self) -> Dict[str, Any]:
        manifest_fallbacks = sum(
            1 for command_id in self._command_index
            if bool(self.describe_manifest_command(command_id).get("selectedFallback"))
        )
        return {
            "commands": self._commands.list_commands(),
            "commandsContributed": len(self._command_contributions),
            "manifestCommandFallbacks": manifest_fallbacks,
            "chatParticipants": len(self._chat_participants),
            "languageModelTools": len(self._lm_tools),
            "languageModelToolSets": len(self._lm_tool_sets),
            "activityBarItems": len(self.activity_bar_items),
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
                "views": sum(
                    1 for views in self._views.values() for view in views
                    if view.get("_runtimeSupport", {}).get("needsExtensionRuntime")),
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
            "viewsWelcome": len(self._views_welcome),
            "submenus": len(self._submenus),
            "interactiveSessions": len(self._interactive_sessions),
            "authentication": len(self._authentication),
            "languages": len(self._languages),
            "grammars": len(self._grammars),
            "themes": len(self._themes),
            "snippets": len(self._snippets),
            "jsonValidation": len(self._json_validation),
            "yamlValidation": len(self._yaml_validation),
            "breakpoints": len(self._breakpoints),
            "problemMatchers": len(self._problem_matchers),
            "problemPatterns": len(self._problem_patterns),
            "icons": len(self._icons),
            "semanticTokenTypes": len(self._semantic_token_types),
            "semanticTokenModifiers": len(self._semantic_token_modifiers),
            "semanticTokenScopes": len(self._semantic_token_scopes),
            "resourceLabelFormatters": len(self._resource_label_formatters),
            "typescriptServerPlugins": len(self._typescript_server_plugins),
            "capabilities": len(self._capabilities),
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

    def get_context(self, ext_id: str) -> Optional[ExtensionContext]:
        """Return the ExtensionContext for an activated extension, or None."""
        act = self._activated.get(ext_id)
        return act.context if act else None


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
        parsed = urlparse(value)
        if parsed.scheme:
            path = unquote(parsed.path or parsed.netloc or "")
            authority = parsed.netloc if parsed.path else ""
            if parsed.scheme == "file" and path.startswith("/") and len(path) >= 3 and path[2] == ":":
                path = path[1:]
            return Uri(
                scheme=parsed.scheme,
                authority=authority,
                path=path,
                query=parsed.query,
                fragment=parsed.fragment,
            )
        return Uri.file(value)

    def to_string(self) -> str:
        if self.scheme == "file":
            path = self.path or ""
            if path and not path.startswith("/"):
                path = "/" + path
            return f"file://{self.authority}{path}"
        if self.authority:
            return f"{self.scheme}://{self.authority}{self.path}"
        if self.path:
            suffix = self.path
            if self.query:
                suffix += f"?{self.query}"
            if self.fragment:
                suffix += f"#{self.fragment}"
            return f"{self.scheme}:{suffix}"
        return f"{self.scheme}://{self.authority}{self.path}"

    def __str__(self) -> str:
        return self.to_string()

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

    def clear(self) -> None:
        """Remove all listeners."""
        self._listeners.clear()


class NodeTreeElement(dict):
    """Serializable handle for an element owned by the Node extension host."""

    def __str__(self) -> str:
        return str(
            self.get("label")
            or self.get("_nodeTreeHandle")
            or self.get("handle")
            or "")

    @property
    def view_id(self) -> str:
        return str(self.get("_nodeTreeViewId") or "")

    @property
    def handle(self) -> str:
        return str(self.get("_nodeTreeHandle") or "")


class NodeTreeDataProvider:
    """TreeDataProvider adapter that proxies calls to ``node_ext_host.js``."""

    def __init__(self, host: Any, view_id: str) -> None:
        self._host = host
        self.view_id = str(view_id or "")
        self._emitter = EventEmitter()
        self.last_error = ""

    @property
    def onDidChangeTreeData(self):
        return self._emitter.event

    def refresh(self, element: Any = None) -> None:
        self._emitter.fire(element)

    def getChildren(self, element: Any = None) -> List[NodeTreeElement]:
        result = self._host.request_tree_data_result(
            self.view_id, "getChildren",
            self._element_handle(element), default=[])
        self._remember_result_error(result)
        value = result.get("value", [])
        if not isinstance(value, list):
            return []
        return [
            self._coerce_element(item)
            for item in value
            if isinstance(item, dict)
        ]

    def getTreeItem(self, element: Any) -> Dict[str, Any]:
        result = self._host.request_tree_data_result(
            self.view_id, "getTreeItem",
            self._element_handle(element), default={})
        self._remember_result_error(result)
        value = result.get("value", {})
        return value if isinstance(value, dict) else {}

    def getParent(self, element: Any) -> Optional[NodeTreeElement]:
        result = self._host.request_tree_data_result(
            self.view_id, "getParent",
            self._element_handle(element), default=None)
        self._remember_result_error(result)
        value = result.get("value")
        return self._coerce_element(value) if isinstance(value, dict) else None

    def clear_error(self) -> None:
        self.last_error = ""

    def _remember_result_error(self, result: Dict[str, Any]) -> None:
        if result.get("ok"):
            self.last_error = ""
            return
        self.last_error = str(result.get("error") or "Node tree provider failed")

    @staticmethod
    def _coerce_element(value: Dict[str, Any]) -> NodeTreeElement:
        return NodeTreeElement(value)

    @staticmethod
    def _element_handle(element: Any) -> str:
        if isinstance(element, dict):
            return str(element.get("_nodeTreeHandle") or element.get("handle") or "")
        return str(getattr(element, "handle", "") or "")


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
        self._activation_event_callbacks: List[
            Callable[[str, List[ActivatedExtension]], None]
        ] = []
        self._change_emitter = _LazyEventEmitter()
        self.registry.on_did_change(self._relay_registry_change)

    @property
    def on_did_change(self):
        return self._change_emitter.event

    def _emit_change(self, payload: Optional[Dict[str, Any]] = None) -> None:
        self._change_emitter.fire({
            "extensions": self.list_extensions(),
            "change": payload or {"added": [], "removed": [], "changed": []},
        })

    def _relay_registry_change(self, payload: Optional[Dict[str, Any]] = None) -> None:
        self._emit_change(payload)

    def _activate_command_extension(self, command_id: str) -> None:
        self.activate_event(f"onCommand:{command_id}")

    def on_activation_event(
            self,
            callback: Callable[[str, List[ActivatedExtension]], None]
    ) -> Callable[[], None]:
        self._activation_event_callbacks.append(callback)

        def dispose() -> None:
            try:
                self._activation_event_callbacks.remove(callback)
            except ValueError:
                pass

        return dispose

    def set_policy(self, policy: Optional[Callable[[ExtensionDescription], bool]] = None,
                   enabled_contributions: Optional[Set[str]] = None) -> None:
        self._policy = policy
        self.ext_points.enabled_contributions = enabled_contributions

    def set_command_fallback_resolver(
            self,
            resolver: Optional[
                Callable[[Dict[str, Any], Dict[str, Any], List[Any]], Optional[Dict[str, Any]]]
            ]) -> None:
        self.ext_points.set_command_fallback_resolver(resolver)

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
                self._emit_change({"activated": [desc.id]})
        return desc

    def start(self) -> List[ActivatedExtension]:
        self._started = True
        results = self.activator.activate_by_event("*")
        results += self.activator.activate_by_event("onStartupFinished")
        if results:
            self._emit_change({"activated": [item.ext.id for item in results]})
        return results

    def activate_event(self, event: str) -> List[ActivatedExtension]:
        results = self.activator.activate_by_event(event)
        for callback in list(self._activation_event_callbacks):
            try:
                callback(event, results)
            except Exception:
                pass
        if results:
            self._emit_change({
                "event": event,
                "activated": [item.ext.id for item in results],
            })
        return results

    def shutdown(self) -> None:
        had_active = bool(self.activator.list_activated())
        self.activator.deactivate_all()
        self._started = False
        if had_active:
            self._emit_change({"deactivatedAll": True})

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
# NodeExtensionHost — subprocess bridge for Node.js extensions with "main"
# ---------------------------------------------------------------------------

import logging
import shutil
import subprocess

_log = logging.getLogger(__name__)


class NodeExtensionHost:
    """Manages a Node.js child process that runs real VS Code extension code.

    Extensions whose ``package.json`` declares a ``"main"`` entry point
    require a JavaScript runtime.  This class spawns a Node.js subprocess
    running *script_path* (the extension host bootstrap script), then
    communicates over a newline-delimited JSON protocol on stdin/stdout.

    The class **complements** the existing Python ``ExtensionHost`` which
    continues to handle manifest-only (no ``main``) extensions.

    Protocol (Python -> Node, one JSON object per line on stdin):
        {"type": "activate",   "extensionPath": "...", "extensionId": "...",
         "manifest": {...}, "storageRoot": "..."}
        {"type": "deactivate", "extensionId": "..."}
        {"type": "webviewMessage", "viewId": "...", "message": {...}}
        {"type": "quick_input_action", "id": "...", "action": "..."}
        {"type": "deserialize_webview_panel", "viewType": "...",
         "state": {...}}
        {"type": "settings_sync",    "settings": {...}}
        {"type": "settings_changed", "section": "...", "key": "...", "value": ...}
        {"type": "settings_changed", "section": "...", "key": "...",
         "overrideIdentifier": "python", "value": ...}
        {"type": "shutdown"}

    Protocol (Node -> Python, one JSON object per line on stdout):
        {"type": "activated",           "extensionId": "..."}
        {"type": "error",               "extensionId": "...", "message": "..."}
        {"type": "execute_command",     "requestId": "...", "commandId": "...", "args": [...]}
        {"type": "webview_html",        "viewId": "...", "html": "...",
         "localResourceRoots": [...]}
        {"type": "webview_post_message","viewId": "...", "message": {...}}
        {"type": "webview_dispose",     "viewId": "..."}
        {"type": "webview_panel_deserialized", "requestId": "...",
         "ok": true, "viewId": "..."}
        {"type": "command_registered",  "commandId": "...", "extensionId": "..."}
        {"type": "command_response",    "requestId": "...", "ok": true, "value": ...}
        {"type": "config_set",          "section": "...", "key": "...", "value": ...}
        {"type": "config_set",          "section": "...", "key": "...",
         "overrideIdentifier": "python", "value": ...}
        {"type": "output",              "channel": "...", "text": "..."}
        {"type": "show_message",        "level": "info|warn|error", "message": "..."}
        {"type": "quick_input",         "event": "show|update|hide|dispose",
         "id": "...", "kind": "quickPick|inputBox", "state": {...}}
        {"type": "progress_start",      "message": "..."}
        {"type": "progress_report",     "message": "...", "increment": 10}
        {"type": "progress_done",       "ok": true}
    """

    def __init__(self,
                 node_path: Optional[str] = None,
                 script_path: str = "",
                 ui_bridge: Any = None,
                 storage_root: str = "") -> None:
        if node_path is None:
            from ai_editor.node_runtime import get_node_path as _get_node
            node_path = _get_node() or ""
        self._node_path = node_path
        self._script_path = script_path
        self._ui_bridge = ui_bridge
        self._storage_root = storage_root
        self._proc: Optional[subprocess.Popen] = None
        self._reader: Optional[threading.Thread] = None
        self._lock = threading.Lock()
        self._activated_ids: Set[str] = set()
        self._activation_sent_ids: Set[str] = set()
        self._output_channels: Dict[str, List[str]] = {}
        self._command_service: Optional[CommandService] = None
        self._command_request_lock = threading.Lock()
        self._command_requests: Dict[str, Dict[str, Any]] = {}
        self._diagnostics_enabled = False
        self._diagnostics_lock = threading.Lock()
        self._diagnostics: Dict[str, Dict[str, Any]] = {}
        self._registered_extensions: Dict[str, Dict[str, Any]] = {}
        self._on_activated_callbacks: List[Callable[[str], None]] = []
        self._on_error_callbacks: List[Callable[[str, str], None]] = []
        self._on_config_set_callbacks: List[
            Callable[[str, str, Any, bool, str], None]
        ] = []
        self._on_tree_callbacks: List[
            Callable[[str, str, Dict[str, Any]], None]
        ] = []
        self._on_lm_tool_callbacks: List[
            Callable[[str, str, Dict[str, Any]], None]
        ] = []
        self._on_chat_participant_callbacks: List[
            Callable[[str, str, Dict[str, Any]], None]
        ] = []
        self._lm_model_request_callback: Optional[
            Callable[[Dict[str, Any]], Dict[str, Any]]
        ] = None
        self._lm_model_cancel_callback: Optional[
            Callable[[Dict[str, Any]], Any]
        ] = None
        self._tree_request_lock = threading.Lock()
        self._tree_requests: Dict[str, Dict[str, Any]] = {}
        self._lm_tool_request_lock = threading.Lock()
        self._lm_tool_requests: Dict[str, Dict[str, Any]] = {}
        self._chat_participant_request_lock = threading.Lock()
        self._chat_participant_requests: Dict[str, Dict[str, Any]] = {}
        self._language_request_lock = threading.Lock()
        self._language_requests: Dict[str, Dict[str, Any]] = {}
        self._language_providers: List[Dict[str, Any]] = []
        self._custom_editor_request_lock = threading.Lock()
        self._custom_editor_requests: Dict[str, Dict[str, Any]] = {}
        self._custom_editor_lifecycle_lock = threading.Lock()
        self._custom_editor_lifecycle_requests: Dict[str, Dict[str, Any]] = {}
        self._custom_editor_state_lock = threading.Lock()
        self._custom_editor_states: Dict[str, Dict[str, Any]] = {}
        self._webview_serializer_request_lock = threading.Lock()
        self._webview_serializer_requests: Dict[str, Dict[str, Any]] = {}
        self._shutting_down = False

    # -- Lifecycle -----------------------------------------------------------

    def start(self) -> bool:
        """Spawn the Node subprocess and start the reader thread.

        Returns True on success, False if node is unavailable or the
        process fails to start.
        """
        with self._lock:
            if self._proc is not None and self._proc.poll() is None:
                return True  # already running

        node = self._resolve_node()
        if not node:
            _log.warning("[NodeExtHost] Node.js not found at %r; "
                         "JS extensions will not be activated.", self._node_path)
            return False

        if not os.path.isfile(self._script_path):
            _log.warning("[NodeExtHost] Extension host script not found: %s",
                         self._script_path)
            return False

        try:
            self._shutting_down = False
            self._proc = subprocess.Popen(
                [node, self._script_path],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                bufsize=0,
                creationflags=(
                    getattr(subprocess, "CREATE_NO_WINDOW", 0)
                    if os.name == "nt" else 0
                ),
            )
        except FileNotFoundError:
            _log.error("[NodeExtHost] Failed to spawn Node: executable %r "
                       "not found.", node)
            return False
        except OSError as exc:
            _log.error("[NodeExtHost] Failed to spawn Node: %s", exc)
            return False

        self._reader = threading.Thread(
            target=self._reader_thread, daemon=True,
            name="NodeExtHost-reader")
        self._reader.start()

        # Start a stderr logger too
        threading.Thread(
            target=self._stderr_thread, daemon=True,
            name="NodeExtHost-stderr").start()

        _log.info("[NodeExtHost] Started (pid=%d)", self._proc.pid)
        return True

    def stop(self, timeout: float = 5.0) -> None:
        """Gracefully shut down the Node subprocess."""
        self._shutting_down = True
        proc = self._proc
        if proc is None:
            return
        # Try graceful shutdown first
        try:
            self._send({"type": "shutdown"})
        except Exception:
            pass
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            _log.warning("[NodeExtHost] Graceful shutdown timed out; killing.")
            try:
                proc.kill()
            except OSError:
                pass
            try:
                proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                pass
        self._proc = None
        self._activated_ids.clear()
        self._activation_sent_ids.clear()
        _log.info("[NodeExtHost] Stopped.")

    @property
    def is_running(self) -> bool:
        proc = self._proc
        return proc is not None and proc.poll() is None

    # -- Extension activation -----------------------------------------------

    @staticmethod
    def extension_manifest(ext: ExtensionDescription) -> Dict[str, Any]:
        return {
            "name": ext.name,
            "displayName": ext.display_name,
            "publisher": ext.publisher,
            "version": ext.version,
            "description": ext.description,
            "main": ext.main,
            "browser": ext.browser,
            "icon": ext.icon,
            "categories": list(ext.categories),
            "activationEvents": list(ext.activation_events),
            "extensionDependencies": list(ext.extension_dependencies),
            "extensionKind": ext.extension_kind,
            "contributes": ext.contributes,
        }

    def register_extensions(self, extensions: List[ExtensionDescription]) -> int:
        """Sync known Node extension descriptions without activating them."""
        records: List[Dict[str, Any]] = []
        for ext in extensions:
            if not ext.main:
                continue
            manifest = self.extension_manifest(ext)
            record = {
                "extensionId": ext.id,
                "extensionPath": ext.extension_path,
                "manifest": manifest,
                "extensionKind": ext.extension_kind,
            }
            if self._storage_root:
                record["storageRoot"] = self._storage_root
            self._registered_extensions[ext.id] = record
            records.append(record)
        if records and self.is_running:
            self._send({"type": "register_extensions", "extensions": records})
        return len(records)

    def activate(self, extension_path: str, extension_id: str,
                 manifest: Dict[str, Any]) -> bool:
        """Send an activate message for a single extension.

        Returns True if the message was sent successfully.
        """
        if not self.is_running:
            _log.warning("[NodeExtHost] Cannot activate %s: host not running.",
                         extension_id)
            return False
        if extension_id in self._activated_ids:
            return True
        if extension_id in self._activation_sent_ids:
            return True
        record = {
            "extensionId": extension_id,
            "extensionPath": extension_path,
            "manifest": dict(manifest or {}),
        }
        if self._storage_root:
            record["storageRoot"] = self._storage_root
        self._registered_extensions[extension_id] = record
        msg = {
            "type": "activate",
            "extensionPath": extension_path,
            "extensionId": extension_id,
            "manifest": manifest,
        }
        if self._storage_root:
            msg["storageRoot"] = self._storage_root
        sent = self._send(msg)
        if sent:
            self._activation_sent_ids.add(extension_id)
        return sent

    def deactivate(self, extension_id: str) -> bool:
        """Send a deactivate message for a single extension."""
        if not self.is_running:
            return False
        self._activated_ids.discard(extension_id)
        self._activation_sent_ids.discard(extension_id)
        return self._send({
            "type": "deactivate",
            "extensionId": extension_id,
        })

    def wait_for_activation(
            self, extension_ids: List[str], timeout: float = 5.0) -> bool:
        """Wait until all extension ids are activated in the Node host."""
        pending = [str(ext_id or "") for ext_id in extension_ids if ext_id]
        if not pending:
            return True
        deadline = time.time() + max(0.0, timeout)
        while time.time() < deadline:
            if all(ext_id in self._activated_ids for ext_id in pending):
                return True
            failed = [
                ext_id for ext_id in pending
                if ext_id not in self._activated_ids
                and ext_id not in self._activation_sent_ids
            ]
            if failed:
                return False
            time.sleep(0.025)
        return all(ext_id in self._activated_ids for ext_id in pending)

    def activate_all(
            self, extensions: List[ExtensionDescription],
            wait: bool = False,
            timeout: float = 5.0) -> int:
        """Activate a list of extensions. Returns the number successfully sent."""
        count = 0
        waiting: List[str] = []
        for ext in extensions:
            if not ext.main:
                continue
            manifest = self.extension_manifest(ext)
            was_active = ext.id in self._activated_ids
            if self.activate(ext.extension_path, ext.id, manifest):
                count += 1
                if not was_active:
                    waiting.append(ext.id)
        if wait and waiting:
            self.wait_for_activation(waiting, timeout=timeout)
        return count

    # -- Communication: send -------------------------------------------------

    def _send(self, msg: Dict[str, Any]) -> bool:
        """Write a JSON line to the subprocess stdin.

        Returns True if the write succeeded.
        """
        proc = self._proc
        if proc is None or proc.stdin is None:
            return False
        try:
            line = json.dumps(msg, ensure_ascii=False, default=str) + "\n"
            with self._lock:
                proc.stdin.write(line.encode("utf-8"))
                proc.stdin.flush()
            return True
        except (BrokenPipeError, OSError) as exc:
            if not self._shutting_down:
                _log.error("[NodeExtHost] Write failed: %s", exc)
            return False

    # -- Communication: receive ----------------------------------------------

    def _reader_thread(self) -> None:
        """Read JSON lines from the subprocess stdout and dispatch."""
        proc = self._proc
        if proc is None or proc.stdout is None:
            return
        try:
            for raw_line in proc.stdout:
                if self._shutting_down:
                    break
                line = raw_line.decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    _log.warning("[NodeExtHost] Malformed JSON from Node: %s",
                                 line[:200])
                    continue
                if isinstance(msg, dict):
                    try:
                        self._on_message(msg)
                    except Exception:
                        _log.exception("[NodeExtHost] Error handling message: "
                                       "%s", msg.get("type", "?"))
        except Exception:
            if not self._shutting_down:
                _log.exception("[NodeExtHost] Reader thread crashed.")
        finally:
            # If the process exited unexpectedly, log it
            if not self._shutting_down and proc.poll() is not None:
                _log.warning("[NodeExtHost] Node subprocess exited with "
                             "code %d.", proc.returncode or -1)

    def _stderr_thread(self) -> None:
        """Drain stderr from the Node subprocess and log it."""
        proc = self._proc
        if proc is None or proc.stderr is None:
            return
        try:
            for raw_line in proc.stderr:
                if self._shutting_down:
                    break
                line = raw_line.decode("utf-8", errors="replace").rstrip()
                if line:
                    _log.info("[NodeExtHost:stderr] %s", line)
        except Exception:
            pass

    def _custom_editor_state_key(self, payload: Dict[str, Any]) -> str:
        view_id = str(payload.get("viewId") or payload.get("view_id") or "")
        if view_id:
            return f"view:{view_id}"
        view_type = str(payload.get("viewType") or payload.get("view_type") or "")
        uri = str(payload.get("uri") or payload.get("resource") or payload.get("path") or "")
        return f"doc:{view_type}|{uri}"

    def _normalize_custom_editor_state(
            self, payload: Dict[str, Any]) -> Dict[str, Any]:
        view_id = str(payload.get("viewId") or payload.get("view_id") or "")
        view_type = str(payload.get("viewType") or payload.get("view_type") or "")
        state = dict(payload)
        state["viewId"] = view_id
        state["view_id"] = view_id
        state["viewType"] = view_type
        state["view_type"] = view_type
        state["uri"] = str(payload.get("uri") or "")
        state["dirty"] = bool(payload.get("dirty", False))
        state["editable"] = bool(payload.get("editable", False))
        state["textEditor"] = bool(payload.get("textEditor", False))
        state["text_editor"] = state["textEditor"]
        state["kind"] = str(payload.get("kind") or "")
        state["label"] = str(payload.get("label") or "")
        state["backupId"] = str(payload.get("backupId") or payload.get("backup_id") or "")
        state["backup_id"] = state["backupId"]
        for key in ("supportsSave", "supportsSaveAs", "supportsRevert", "supportsBackup"):
            if key in payload:
                state[key] = bool(payload.get(key))
        for key in ("canUndo", "canRedo"):
            if key in payload:
                state[key] = bool(payload.get(key))
        if "currentEditIndex" in payload:
            try:
                state["currentEditIndex"] = int(payload.get("currentEditIndex"))
            except (TypeError, ValueError):
                state["currentEditIndex"] = -1
        return state

    def _store_custom_editor_state(
            self, payload: Dict[str, Any]) -> Dict[str, Any]:
        state = self._normalize_custom_editor_state(payload)
        key = self._custom_editor_state_key(state)
        with self._custom_editor_state_lock:
            previous = self._custom_editor_states.get(key, {})
            merged = {**previous, **state}
            self._custom_editor_states[key] = merged
            return dict(merged)

    def _find_custom_editor_state(
            self, view_id: str = "", view_type: str = "",
            uri: str = "") -> Optional[Dict[str, Any]]:
        normalized_view = str(view_id or "")
        normalized_type = str(view_type or "")
        normalized_uri = str(uri or "")
        with self._custom_editor_state_lock:
            states = [dict(item) for item in self._custom_editor_states.values()]
        for state in states:
            if normalized_view and state.get("viewId") == normalized_view:
                return state
            if normalized_type and normalized_uri:
                if (state.get("viewType") == normalized_type
                        and state.get("uri") == normalized_uri):
                    return state
            if normalized_uri and state.get("uri") == normalized_uri:
                return state
        return None

    def _on_message(self, msg: Dict[str, Any]) -> None:
        """Dispatch an incoming message from the Node subprocess."""
        msg_type = str(msg.get("type", ""))

        if msg_type == "activated":
            ext_id = str(msg.get("extensionId", ""))
            if bool(msg.get("ok", True)):
                self._activated_ids.add(ext_id)
                self._activation_sent_ids.add(ext_id)
                _log.info("[NodeExtHost] Extension activated: %s", ext_id)
                for cb in self._on_activated_callbacks:
                    try:
                        cb(ext_id)
                    except Exception:
                        _log.exception(
                            "[NodeExtHost] on_activated callback error")
            else:
                self._activated_ids.discard(ext_id)
                self._activation_sent_ids.discard(ext_id)
                _log.warning(
                    "[NodeExtHost] Extension activation failed: %s (%s)",
                    ext_id,
                    msg.get("error") or msg.get("message") or "Unknown error")

        elif msg_type == "error":
            ext_id = str(msg.get("extensionId", ""))
            message = str(
                msg.get("message") or msg.get("error") or "Unknown error")
            if ext_id and ext_id not in self._activated_ids:
                self._activation_sent_ids.discard(ext_id)
            _log.error("[NodeExtHost] Extension error (%s): %s",
                       ext_id, message)
            for cb in self._on_error_callbacks:
                try:
                    cb(ext_id, message)
                except Exception:
                    pass

        elif msg_type == "webview_html":
            view_id = str(msg.get("viewId", ""))
            html = str(msg.get("html", ""))
            local_roots = msg.get("localResourceRoots", None)
            state = msg.get("state", None)
            title = str(msg.get("title", ""))
            if self._ui_bridge and view_id:
                try:
                    self._ui_bridge.render_webview_panel(
                        view_id, html, local_roots, state, title)
                except TypeError:
                    try:
                        self._ui_bridge.render_webview_panel(
                            view_id, html, local_roots, state)
                    except TypeError:
                        try:
                            self._ui_bridge.render_webview_panel(
                                view_id, html, local_roots)
                        except TypeError:
                            try:
                                self._ui_bridge.render_webview_panel(
                                    view_id, html)
                            except Exception:
                                _log.exception(
                                    "[NodeExtHost] render_webview_panel failed "
                                    "for %s", view_id)
                        except Exception:
                            _log.exception(
                                "[NodeExtHost] render_webview_panel failed "
                                "for %s", view_id)
                    except Exception:
                        _log.exception(
                            "[NodeExtHost] render_webview_panel failed "
                            "for %s", view_id)
                except Exception:
                    _log.exception("[NodeExtHost] render_webview_panel failed "
                                   "for %s", view_id)

        elif msg_type == "webview_title":
            view_id = str(msg.get("viewId", ""))
            title = str(msg.get("title", ""))
            view_type = str(msg.get("viewType", ""))
            if self._ui_bridge and view_id:
                try:
                    updater = getattr(
                        self._ui_bridge, "update_webview_panel_title", None)
                    if callable(updater):
                        updater(view_id, title, view_type)
                except Exception:
                    _log.exception(
                        "[NodeExtHost] update_webview_panel_title failed "
                        "for %s", view_id)

        elif msg_type == "webview_post_message":
            view_id = str(msg.get("viewId", ""))
            message = msg.get("message")
            if self._ui_bridge and view_id:
                try:
                    self._ui_bridge.post_webview_message(view_id, message)
                except Exception:
                    _log.exception("[NodeExtHost] post_webview_message failed "
                                   "for %s", view_id)

        elif msg_type == "webview_dispose":
            view_id = str(msg.get("viewId", ""))
            if self._ui_bridge and view_id:
                try:
                    self._ui_bridge.dispose_webview_panel(view_id)
                except Exception:
                    _log.exception("[NodeExtHost] dispose_webview_panel failed "
                                   "for %s", view_id)

        elif msg_type == "command_registered":
            command_id = str(msg.get("commandId", ""))
            ext_id = str(msg.get("extensionId", ""))
            if command_id and self._command_service:
                # Register a proxy command that forwards execution to Node
                def _node_command_proxy(*args, _cid=command_id):
                    result = self.request_command_result(_cid, list(args))
                    if result.get("ok"):
                        return result.get("value")
                    raise RuntimeError(
                        result.get("error")
                        or f"Node command failed: {_cid}")
                self._command_service.register(command_id, _node_command_proxy)
                _log.info("[NodeExtHost] Command registered: %s (from %s)",
                          command_id, ext_id)

        elif msg_type == "command_response":
            request_id = str(msg.get("requestId", ""))
            with self._command_request_lock:
                pending = self._command_requests.get(request_id)
            if pending:
                pending["response"] = msg
                event = pending.get("event")
                if isinstance(event, threading.Event):
                    event.set()

        elif msg_type == "execute_command":
            request_id = str(msg.get("requestId", ""))
            command_id = str(msg.get("commandId", ""))
            args = msg.get("args") or []
            if not isinstance(args, list):
                args = [args]
            started = time.perf_counter()
            ok = False
            timed_out = False
            error = ""
            if not self._command_service:
                error = "Command service is not available"
                if request_id:
                    self._send({
                        "type": "execute_command_response",
                        "requestId": request_id,
                        "ok": False,
                        "error": error,
                    })
            else:
                try:
                    value = self._command_service.execute(command_id, *args)
                    ok = True
                    if request_id:
                        self._send({
                            "type": "execute_command_response",
                            "requestId": request_id,
                            "ok": True,
                            "value": value,
                        })
                except Exception as exc:
                    error = str(exc)
                    if request_id:
                        self._send({
                            "type": "execute_command_response",
                            "requestId": request_id,
                            "ok": False,
                            "error": error,
                        })
            self._record_diagnostic(
                "python_command",
                (time.perf_counter() - started) * 1000,
                ok=ok,
                timeout=timed_out,
                detail=command_id,
                error=error,
            )

        elif msg_type == "lm_model_request":
            request_msg = dict(msg)
            request_id = str(request_msg.get("requestId", ""))

            def _run_lm_model_request() -> None:
                ok = False
                value: Any = None
                error = ""
                try:
                    if self._lm_model_request_callback is None:
                        if str(request_msg.get("action") or "") == "selectChatModels":
                            ok = True
                            value = []
                        else:
                            raise RuntimeError("LM model provider is not available")
                    else:
                        result = self._lm_model_request_callback(request_msg)
                        if isinstance(result, dict) and result.get("ok") is False:
                            error = str(
                                result.get("error") or "LM model request failed")
                            value = result.get("value")
                        else:
                            ok = True
                            value = (
                                result.get("value")
                                if isinstance(result, dict) and "value" in result
                                else result)
                except Exception as exc:
                    error = str(exc)
                if request_id:
                    self._send({
                        "type": "lm_model_response",
                        "requestId": request_id,
                        "ok": ok,
                        "value": value,
                        "error": error,
                    })

            threading.Thread(
                target=_run_lm_model_request,
                daemon=True,
                name=f"NodeExtHost-lm-model-{request_id[:8]}",
            ).start()

        elif msg_type == "lm_model_cancel":
            if self._lm_model_cancel_callback is not None:
                try:
                    self._lm_model_cancel_callback(dict(msg))
                except Exception:
                    pass

        elif msg_type == "activate_extension":
            request_id = str(msg.get("requestId", ""))
            ext_id = str(msg.get("extensionId", ""))
            record = self._registered_extensions.get(ext_id)
            ok = False
            error = ""
            if not ext_id:
                error = "Extension id is required"
            elif not record:
                error = f"Extension is not registered: {ext_id}"
            else:
                ok = self.activate(
                    str(record.get("extensionPath") or ""),
                    ext_id,
                    dict(record.get("manifest") or {}),
                )
                if not ok:
                    error = f"Extension activation could not be sent: {ext_id}"
            if request_id:
                self._send({
                    "type": "activate_extension_response",
                    "requestId": request_id,
                    "extensionId": ext_id,
                    "ok": ok,
                    "error": error,
                })

        elif msg_type == "diagnostic_event":
            self._record_diagnostic(
                str(msg.get("category", "node")),
                float(msg.get("elapsedMs") or 0),
                ok=bool(msg.get("ok", True)),
                timeout=bool(msg.get("timeout", False)),
                detail=str(msg.get("detail", "")),
                error=str(msg.get("error", "")),
            )

        elif msg_type == "output":
            channel = str(
                msg.get("channel")
                or msg.get("channelName")
                or "Extension Output")
            text = str(msg.get("text", ""))
            self._output_channels.setdefault(channel, []).append(text)
            if self._ui_bridge:
                try:
                    self._ui_bridge.show_output(channel, text)
                except Exception:
                    pass

        elif msg_type == "show_message":
            level = str(msg.get("level", "info"))
            message = str(msg.get("message", ""))
            if self._ui_bridge:
                try:
                    self._ui_bridge.show_message(level, message)
                except Exception:
                    pass
            else:
                _log.info("[NodeExtHost] %s: %s", level, message)

        elif msg_type == "quick_input":
            if self._ui_bridge:
                try:
                    handler = getattr(self._ui_bridge, "quick_input_changed", None)
                    if callable(handler):
                        handler(dict(msg))
                except Exception:
                    _log.exception("[NodeExtHost] quick_input bridge failed")

        elif msg_type == "window_dialog_request":
            request_id = str(msg.get("requestId", ""))
            kind = str(msg.get("kind", ""))
            options = msg.get("options")
            value: Any = {"cancelled": True}
            ok = True
            error = ""
            try:
                handler = (
                    getattr(self._ui_bridge, "show_window_dialog", None)
                    if self._ui_bridge else None)
                if callable(handler):
                    value = handler(kind, options if isinstance(options, dict) else {})
                else:
                    value = {"cancelled": True}
            except Exception as exc:
                ok = False
                error = str(exc)
                _log.exception("[NodeExtHost] window dialog bridge failed")
            if request_id:
                self._send({
                    "type": "window_dialog_response",
                    "requestId": request_id,
                    "ok": ok,
                    "value": value,
                    "error": error,
                })

        elif msg_type == "env_clipboard_request":
            request_id = str(msg.get("requestId", ""))
            action = str(msg.get("action", ""))
            value: Any = {}
            ok = True
            error = ""
            try:
                if action == "read":
                    handler = (
                        getattr(self._ui_bridge, "read_clipboard_text", None)
                        if self._ui_bridge else None)
                    value = {
                        "text": str(handler() if callable(handler) else "")
                    }
                elif action == "write":
                    handler = (
                        getattr(self._ui_bridge, "write_clipboard_text", None)
                        if self._ui_bridge else None)
                    if callable(handler):
                        handler(str(msg.get("text", "")))
                    value = {}
                else:
                    ok = False
                    error = f"Unknown clipboard action: {action}"
            except Exception as exc:
                ok = False
                error = str(exc)
                _log.exception("[NodeExtHost] clipboard bridge failed")
            if request_id:
                self._send({
                    "type": "env_clipboard_response",
                    "requestId": request_id,
                    "ok": ok,
                    "value": value,
                    "error": error,
                })

        elif msg_type == "language_provider_registered":
            kind = str(msg.get("kind", ""))
            selector = msg.get("selector")
            self._language_providers.append({
                "handle": msg.get("handle"),
                "extensionId": str(msg.get("extensionId", "")),
                "kind": kind,
                "selector": selector,
                "triggers": list(msg.get("triggers") or []),
                "metadata": msg.get("metadata"),
            })
            _log.info("[NodeExtHost] Language provider registered: %s "
                      "selector=%s", kind, selector)

        elif msg_type == "language_provider_disposed":
            handle = msg.get("handle")
            self._language_providers = [
                item for item in self._language_providers
                if item.get("handle") != handle
            ]

        elif msg_type == "language_provider_response":
            request_id = str(msg.get("requestId", ""))
            with self._language_request_lock:
                pending = self._language_requests.get(request_id)
            if pending:
                pending["response"] = msg
                event = pending.get("event")
                if isinstance(event, threading.Event):
                    event.set()

        elif msg_type in {"lm_tool_registered", "lm_tool_disposed"}:
            name = str(msg.get("name") or msg.get("toolName") or "")
            payload = dict(msg)
            payload.pop("type", None)
            for cb in self._on_lm_tool_callbacks:
                try:
                    cb(msg_type, name, payload)
                except Exception:
                    _log.exception("[NodeExtHost] on_lm_tool callback error")

        elif msg_type == "lm_tool_response":
            request_id = str(msg.get("requestId", ""))
            with self._lm_tool_request_lock:
                pending = self._lm_tool_requests.get(request_id)
            if pending:
                pending["response"] = msg
                event = pending.get("event")
                if isinstance(event, threading.Event):
                    event.set()

        elif msg_type in {
                "chat_participant_registered",
                "chat_participant_disposed"}:
            participant_id = str(msg.get("id") or msg.get("participantId") or "")
            payload = dict(msg)
            payload.pop("type", None)
            for cb in self._on_chat_participant_callbacks:
                try:
                    cb(msg_type, participant_id, payload)
                except Exception:
                    _log.exception(
                        "[NodeExtHost] on_chat_participant callback error")

        elif msg_type == "chat_participant_response":
            request_id = str(msg.get("requestId", ""))
            with self._chat_participant_request_lock:
                pending = self._chat_participant_requests.get(request_id)
            if pending:
                pending["response"] = msg
                event = pending.get("event")
                if isinstance(event, threading.Event):
                    event.set()

        elif msg_type == "tree_response":
            request_id = str(msg.get("requestId", ""))
            with self._tree_request_lock:
                pending = self._tree_requests.get(request_id)
            if pending:
                pending["response"] = msg
                event = pending.get("event")
                if isinstance(event, threading.Event):
                    event.set()

        elif msg_type == "custom_editor_resolved":
            if msg.get("ok"):
                self._store_custom_editor_state(msg)
            request_id = str(msg.get("requestId", ""))
            with self._custom_editor_request_lock:
                pending = self._custom_editor_requests.get(request_id)
            if pending:
                pending["response"] = msg
                event = pending.get("event")
                if isinstance(event, threading.Event):
                    event.set()

        elif msg_type == "webview_panel_deserialized":
            request_id = str(msg.get("requestId", ""))
            with self._webview_serializer_request_lock:
                pending = self._webview_serializer_requests.get(request_id)
            if pending:
                pending["response"] = msg
                event = pending.get("event")
                if isinstance(event, threading.Event):
                    event.set()

        elif msg_type == "custom_editor_changed":
            state = self._store_custom_editor_state(msg)
            if self._ui_bridge:
                try:
                    handler = getattr(self._ui_bridge, "custom_editor_changed", None)
                    if callable(handler):
                        handler(state)
                except Exception:
                    _log.exception("[NodeExtHost] custom_editor_changed bridge failed")

        elif msg_type == "custom_editor_lifecycle_response":
            if msg.get("ok"):
                state_payload = msg.get("state")
                if isinstance(state_payload, dict):
                    self._store_custom_editor_state(state_payload)
                else:
                    self._store_custom_editor_state(msg)
            request_id = str(msg.get("requestId", ""))
            with self._custom_editor_lifecycle_lock:
                pending = self._custom_editor_lifecycle_requests.get(request_id)
            if pending:
                pending["response"] = msg
                event = pending.get("event")
                if isinstance(event, threading.Event):
                    event.set()

        elif msg_type in {
                "tree_data_provider_registered",
                "tree_data_provider_disposed",
                "tree_data_changed",
                "tree_view_reveal"}:
            view_id = str(msg.get("viewId", ""))
            payload = dict(msg)
            payload.pop("type", None)
            for cb in self._on_tree_callbacks:
                try:
                    cb(msg_type, view_id, payload)
                except Exception:
                    _log.exception("[NodeExtHost] on_tree callback error")

        elif msg_type == "config_set":
            section = str(msg.get("section", ""))
            key = str(msg.get("key", ""))
            value = msg.get("value")
            remove = bool(msg.get("remove"))
            override_identifier = str(msg.get("overrideIdentifier", ""))
            if key:
                full_key = f"{section}.{key}" if section else key
                _log.info("[NodeExtHost] config_set: %s", full_key)
                for cb in self._on_config_set_callbacks:
                    try:
                        cb(section, key, value, remove, override_identifier)
                    except Exception:
                        _log.exception("[NodeExtHost] on_config_set callback "
                                       "error for %s", full_key)

        elif msg_type == "status_bar_show":
            if self._ui_bridge:
                try:
                    self._ui_bridge.show_status_bar_item(
                        str(msg.get("id", "")), str(msg.get("text", "")),
                        str(msg.get("tooltip", "")), str(msg.get("command", "")),
                        int(msg.get("alignment", 2)), int(msg.get("priority", 0)),
                        str(msg.get("color", "")), str(msg.get("backgroundColor", "")))
                except Exception:
                    pass

        elif msg_type == "status_bar_hide":
            if self._ui_bridge:
                try:
                    self._ui_bridge.hide_status_bar_item(str(msg.get("id", "")))
                except Exception:
                    pass

        elif msg_type == "status_bar_dispose":
            if self._ui_bridge:
                try:
                    self._ui_bridge.dispose_status_bar_item(str(msg.get("id", "")))
                except Exception:
                    pass

        elif msg_type == "terminal_show":
            if self._ui_bridge:
                try:
                    self._ui_bridge.show_terminal(str(msg.get("name", "")))
                except Exception:
                    pass

        elif msg_type == "terminal_hide":
            if self._ui_bridge:
                try:
                    self._ui_bridge.hide_terminal(str(msg.get("name", "")))
                except Exception:
                    pass

        elif msg_type == "terminal_dispose":
            if self._ui_bridge:
                try:
                    self._ui_bridge.hide_terminal(str(msg.get("name", "")))
                except Exception:
                    pass

        elif msg_type == "terminal_command":
            if self._ui_bridge and bool(msg.get("shouldExecute", True)):
                try:
                    self._ui_bridge.run_terminal_command(
                        str(msg.get("name", "")), str(msg.get("text", "")))
                except Exception:
                    pass

        elif msg_type == "terminal_write":
            if self._ui_bridge:
                try:
                    writer = getattr(
                        self._ui_bridge, "write_terminal_data", None)
                    if callable(writer):
                        writer(str(msg.get("name", "")),
                               str(msg.get("text", "")))
                except Exception:
                    pass

        elif msg_type in ("progress_start", "progress_report", "progress_done"):
            progress_handler = (
                getattr(self._ui_bridge, "show_progress", None)
                if self._ui_bridge else None)
            if callable(progress_handler):
                try:
                    message = msg.get("message")
                    if message is None and msg_type != "progress_done":
                        options = msg.get("options")
                        if isinstance(options, dict):
                            message = options.get("title")
                    increment = msg.get("increment")
                    if isinstance(increment, bool):
                        increment = None
                    elif increment is not None:
                        try:
                            increment = float(increment)
                        except (TypeError, ValueError):
                            increment = None
                    if message is not None or increment is not None:
                        progress_handler(
                            str(message) if message is not None else None,
                            increment)
                except Exception:
                    _log.exception("[NodeExtHost] show_progress failed")

        else:
            _log.debug("[NodeExtHost] Unknown message type: %s", msg_type)

    # -- Settings sync -------------------------------------------------------

    def send_settings_sync(self, settings: Dict[str, Any]) -> bool:
        """Push the full settings dictionary to the Node subprocess.

        Called once after activation so that ``workspace.getConfiguration``
        returns real values instead of empty defaults.
        """
        return self._send({"type": "settings_sync", "settings": settings})

    def send_settings_changed(self, section: str, key: str,
                              value: Any = None,
                              remove: bool = False) -> bool:
        """Notify Node of an incremental settings change."""
        msg = {
            "type": "settings_changed",
            "section": section,
            "key": key,
        }
        if remove:
            msg["remove"] = True
        else:
            msg["value"] = value
        return self._send(msg)

    def on_config_set(
            self,
            callback: Callable[[str, str, Any, bool, str], None]) -> None:
        """Register a callback invoked when Node sends a config_set message.

        The callback receives ``(section, key, value, remove,
        override_identifier)`` and is responsible for persisting the change on
        the Python side.
        """
        self._on_config_set_callbacks.append(callback)

    def on_tree_event(
            self,
            callback: Callable[[str, str, Dict[str, Any]], None]) -> None:
        """Register a callback for Node TreeDataProvider lifecycle events."""
        self._on_tree_callbacks.append(callback)

    def on_lm_tool_event(
            self,
            callback: Callable[[str, str, Dict[str, Any]], None]) -> None:
        """Register a callback for Node language model tool lifecycle events."""
        self._on_lm_tool_callbacks.append(callback)

    def on_chat_participant_event(
            self,
            callback: Callable[[str, str, Dict[str, Any]], None]) -> None:
        """Register a callback for Node chat participant lifecycle events."""
        self._on_chat_participant_callbacks.append(callback)

    def request_command_result(
            self, command_id: str, args: Optional[List[Any]] = None,
            default: Any = None, timeout: float = 3.0) -> Dict[str, Any]:
        """Synchronously execute a Node-registered command and return its result."""
        started = time.perf_counter()
        ok = False
        timed_out = False
        error = ""
        if not self.is_running:
            error = "Node extension host is not running"
            self._record_diagnostic(
                "command", 0, ok=False, detail=str(command_id or ""),
                error=error)
            return {
                "ok": False,
                "value": default,
                "error": error,
            }
        request_id = str(uuid.uuid4())
        event = threading.Event()
        with self._command_request_lock:
            self._command_requests[request_id] = {"event": event}
        try:
            sent = self._send({
                "type": "executeCommand",
                "requestId": request_id,
                "commandId": str(command_id or ""),
                "args": list(args or []),
            })
            if not sent:
                error = "Node command request could not be sent"
                return {
                    "ok": False,
                    "value": default,
                    "error": error,
                }
            if not event.wait(timeout):
                timed_out = True
                error = "Node command request timed out"
                return {
                    "ok": False,
                    "value": default,
                    "error": error,
                    "timeout": True,
                }
            with self._command_request_lock:
                pending = self._command_requests.get(request_id, {})
            response = pending.get("response", {})
            if isinstance(response, dict) and response.get("ok"):
                ok = True
                return {
                    "ok": True,
                    "value": response.get("value", default),
                }
            error = (
                response.get("error")
                if isinstance(response, dict) else "Node command failed")
            return {
                "ok": False,
                "value": default,
                "error": error,
            }
        finally:
            with self._command_request_lock:
                self._command_requests.pop(request_id, None)
            self._record_diagnostic(
                "command",
                (time.perf_counter() - started) * 1000,
                ok=ok,
                timeout=timed_out,
                detail=str(command_id or ""),
                error=error,
            )

    def request_tree_data(
            self, view_id: str, op: str, element_handle: str = "",
            default: Any = None, timeout: float = 0.85) -> Any:
        """Synchronously request TreeDataProvider data from the Node host."""
        result = self.request_tree_data_result(
            view_id, op, element_handle, default=default, timeout=timeout)
        return result.get("value", default) if result.get("ok") else default

    def request_tree_data_result(
            self, view_id: str, op: str, element_handle: str = "",
            default: Any = None, timeout: float = 0.85) -> Dict[str, Any]:
        """Return a structured Node TreeDataProvider request result."""
        started = time.perf_counter()
        ok = False
        timed_out = False
        error = ""
        detail = f"{view_id}:{op}"
        if not self.is_running:
            error = "Node extension host is not running"
            self._record_diagnostic(
                "tree", 0, ok=False, detail=detail, error=error)
            return {
                "ok": False,
                "value": default,
                "error": error,
            }
        request_id = str(uuid.uuid4())
        event = threading.Event()
        with self._tree_request_lock:
            self._tree_requests[request_id] = {"event": event}
        try:
            sent = self._send({
                "type": "tree_request",
                "requestId": request_id,
                "viewId": str(view_id or ""),
                "op": str(op or ""),
                "elementHandle": str(element_handle or ""),
            })
            if not sent:
                error = "Node tree provider request could not be sent"
                return {
                    "ok": False,
                    "value": default,
                    "error": error,
                }
            if not event.wait(timeout):
                timed_out = True
                error = "Node tree provider request timed out"
                return {
                    "ok": False,
                    "value": default,
                    "error": error,
                    "timeout": True,
                }
            with self._tree_request_lock:
                pending = self._tree_requests.get(request_id, {})
            response = pending.get("response", {})
            if isinstance(response, dict) and response.get("ok"):
                ok = True
                return {
                    "ok": True,
                    "value": response.get("value", default),
                }
            error = (
                response.get("error")
                if isinstance(response, dict) else "Node tree provider failed")
            return {
                "ok": False,
                "value": default,
                "error": error,
            }
        finally:
            with self._tree_request_lock:
                self._tree_requests.pop(request_id, None)
            self._record_diagnostic(
                "tree",
                (time.perf_counter() - started) * 1000,
                ok=ok,
                timeout=timed_out,
                detail=detail,
                error=error,
            )

    def request_language_provider(
            self, payload: Dict[str, Any], default: Any = None,
            timeout: float = 0.85) -> Any:
        """Synchronously request language-provider data from the Node host."""
        result = self.request_language_provider_result(
            payload, default=default, timeout=timeout)
        return result.get("value", default) if result.get("ok") else default

    def request_language_provider_result(
            self, payload: Dict[str, Any], default: Any = None,
            timeout: float = 0.85) -> Dict[str, Any]:
        """Return a structured Node language-provider request result."""
        started = time.perf_counter()
        ok = False
        timed_out = False
        error = ""
        kind = str((payload or {}).get("kind", ""))
        if not self.is_running:
            error = "Node extension host is not running"
            self._record_diagnostic(
                "language", 0, ok=False, detail=kind, error=error)
            return {
                "ok": False,
                "value": default,
                "error": error,
            }
        request_id = str(uuid.uuid4())
        event = threading.Event()
        with self._language_request_lock:
            self._language_requests[request_id] = {"event": event}
        try:
            msg = {
                "type": "language_provider_request",
                "requestId": request_id,
            }
            msg.update(dict(payload or {}))
            sent = self._send(msg)
            if not sent:
                error = "Node language provider request could not be sent"
                return {
                    "ok": False,
                    "value": default,
                    "error": error,
                }
            if not event.wait(timeout):
                timed_out = True
                error = "Node language provider request timed out"
                return {
                    "ok": False,
                    "value": default,
                    "error": error,
                    "timeout": True,
                }
            with self._language_request_lock:
                pending = self._language_requests.get(request_id, {})
            response = pending.get("response", {})
            if isinstance(response, dict) and response.get("ok"):
                ok = True
                return {
                    "ok": True,
                    "value": response.get("value", default),
                }
            error = (
                response.get("error")
                if isinstance(response, dict)
                else "Node language provider failed")
            return {
                "ok": False,
                "value": default,
                "error": error,
            }
        finally:
            with self._language_request_lock:
                self._language_requests.pop(request_id, None)
            self._record_diagnostic(
                "language",
                (time.perf_counter() - started) * 1000,
                ok=ok,
                timeout=timed_out,
                detail=kind,
                error=error,
            )

    def request_lm_tool_result(
            self, name: str, input_data: Any = None,
            default: Any = None, timeout: float = 3.0) -> Dict[str, Any]:
        """Synchronously invoke a Node-registered language model tool."""
        started = time.perf_counter()
        ok = False
        timed_out = False
        error = ""
        detail = str(name or "")
        if not self.is_running:
            error = "Node extension host is not running"
            self._record_diagnostic(
                "lm_tool", 0, ok=False, detail=detail, error=error)
            return {"ok": False, "value": default, "error": error}
        request_id = str(uuid.uuid4())
        event = threading.Event()
        with self._lm_tool_request_lock:
            self._lm_tool_requests[request_id] = {"event": event}
        try:
            sent = self._send({
                "type": "lm_tool_request",
                "requestId": request_id,
                "name": detail,
                "input": input_data,
            })
            if not sent:
                error = "Node language model tool request could not be sent"
                return {"ok": False, "value": default, "error": error}
            if not event.wait(timeout):
                timed_out = True
                error = "Node language model tool request timed out"
                return {
                    "ok": False,
                    "value": default,
                    "error": error,
                    "timeout": True,
                }
            with self._lm_tool_request_lock:
                pending = self._lm_tool_requests.get(request_id, {})
            response = pending.get("response", {})
            if isinstance(response, dict) and response.get("ok"):
                ok = True
                return {
                    "ok": True,
                    "value": response.get("value", default),
                }
            error = (
                response.get("error")
                if isinstance(response, dict)
                else "Node language model tool failed")
            return {"ok": False, "value": default, "error": error}
        finally:
            with self._lm_tool_request_lock:
                self._lm_tool_requests.pop(request_id, None)
            self._record_diagnostic(
                "lm_tool",
                (time.perf_counter() - started) * 1000,
                ok=ok,
                timeout=timed_out,
                detail=detail,
                error=error,
            )

    def request_chat_participant_result(
            self, participant_id: str, prompt: str = "",
            request: Optional[Dict[str, Any]] = None,
            default: Any = None, timeout: float = 3.0) -> Dict[str, Any]:
        """Synchronously invoke a Node-registered chat participant."""
        started = time.perf_counter()
        ok = False
        timed_out = False
        error = ""
        detail = str(participant_id or "")
        if not self.is_running:
            error = "Node extension host is not running"
            self._record_diagnostic(
                "chat_participant", 0, ok=False, detail=detail,
                error=error)
            return {"ok": False, "value": default, "error": error}
        request_id = str(uuid.uuid4())
        event = threading.Event()
        with self._chat_participant_request_lock:
            self._chat_participant_requests[request_id] = {"event": event}
        try:
            sent = self._send({
                "type": "chat_participant_request",
                "requestId": request_id,
                "participantId": detail,
                "prompt": str(prompt or ""),
                "request": dict(request or {}),
            })
            if not sent:
                error = "Node chat participant request could not be sent"
                return {"ok": False, "value": default, "error": error}
            if not event.wait(timeout):
                timed_out = True
                error = "Node chat participant request timed out"
                return {
                    "ok": False,
                    "value": default,
                    "error": error,
                    "timeout": True,
                }
            with self._chat_participant_request_lock:
                pending = self._chat_participant_requests.get(request_id, {})
            response = pending.get("response", {})
            if isinstance(response, dict) and response.get("ok"):
                ok = True
                return {
                    "ok": True,
                    "value": response.get("value", default),
                }
            error = (
                response.get("error")
                if isinstance(response, dict)
                else "Node chat participant failed")
            return {"ok": False, "value": default, "error": error}
        finally:
            with self._chat_participant_request_lock:
                self._chat_participant_requests.pop(request_id, None)
            self._record_diagnostic(
                "chat_participant",
                (time.perf_counter() - started) * 1000,
                ok=ok,
                timeout=timed_out,
                detail=detail,
                error=error,
            )

    def send_tree_view_event(
            self, view_id: str, event: str, element: Any = None,
            selection: Optional[List[Any]] = None) -> bool:
        """Notify the Node TreeView object about frontend selection/expand."""
        element_handle = NodeTreeDataProvider._element_handle(element)
        selection_handles = [
            NodeTreeDataProvider._element_handle(item)
            for item in (selection or [])
        ]
        return self._send({
            "type": "tree_view_event",
            "viewId": str(view_id or ""),
            "event": str(event or ""),
            "elementHandle": element_handle,
            "selectionHandles": selection_handles,
        })

    def request_custom_editor_result(
            self, view_type: str, uri: str, title: str = "",
            view_id: str = "", timeout: float = 2.0) -> Dict[str, Any]:
        """Ask Node to resolve a registered CustomEditorProvider."""
        started = time.perf_counter()
        ok = False
        timed_out = False
        error = ""
        detail = str(view_type or "")
        if not self.is_running:
            error = "Node extension host is not running"
            self._record_diagnostic(
                "custom_editor", 0, ok=False, detail=detail, error=error)
            return {"ok": False, "error": error}
        request_id = str(uuid.uuid4())
        event = threading.Event()
        with self._custom_editor_request_lock:
            self._custom_editor_requests[request_id] = {"event": event}
        try:
            sent = self._send({
                "type": "resolve_custom_editor",
                "requestId": request_id,
                "viewType": str(view_type or ""),
                "uri": str(uri or ""),
                "title": str(title or ""),
                "viewId": str(view_id or ""),
            })
            if not sent:
                error = "Node custom editor request could not be sent"
                return {"ok": False, "error": error}
            if not event.wait(timeout):
                timed_out = True
                error = "Node custom editor request timed out"
                return {"ok": False, "error": error, "timeout": True}
            with self._custom_editor_request_lock:
                pending = self._custom_editor_requests.get(request_id, {})
            response = pending.get("response", {})
            if isinstance(response, dict) and response.get("ok"):
                ok = True
                return dict(response)
            error = (
                response.get("error")
                if isinstance(response, dict) else "Node custom editor failed")
            return {"ok": False, "error": error}
        finally:
            with self._custom_editor_request_lock:
                self._custom_editor_requests.pop(request_id, None)
            self._record_diagnostic(
                "custom_editor",
                (time.perf_counter() - started) * 1000,
                ok=ok,
                timeout=timed_out,
                detail=detail,
                error=error,
            )

    def request_webview_panel_deserialization(
            self, view_type: str, state: Any = None, title: str = "",
            view_id: str = "", timeout: float = 2.0) -> Dict[str, Any]:
        """Ask Node to revive a WebviewPanel via a registered serializer."""
        started = time.perf_counter()
        ok = False
        timed_out = False
        error = ""
        detail = str(view_type or "")
        if not self.is_running:
            error = "Node extension host is not running"
            self._record_diagnostic(
                "webview_serializer", 0, ok=False, detail=detail,
                error=error)
            return {"ok": False, "error": error}
        request_id = str(uuid.uuid4())
        event = threading.Event()
        with self._webview_serializer_request_lock:
            self._webview_serializer_requests[request_id] = {"event": event}
        try:
            resolved_state = state
            if resolved_state is None and view_id and self._ui_bridge:
                try:
                    getter = getattr(self._ui_bridge, "get_webview_state", None)
                    if callable(getter):
                        resolved_state = getter(str(view_id or ""))
                except Exception:
                    resolved_state = state
            sent = self._send({
                "type": "deserialize_webview_panel",
                "requestId": request_id,
                "viewType": str(view_type or ""),
                "state": resolved_state,
                "title": str(title or ""),
                "viewId": str(view_id or ""),
            })
            if not sent:
                error = "Node webview panel serializer request could not be sent"
                return {"ok": False, "error": error}
            if not event.wait(timeout):
                timed_out = True
                error = "Node webview panel serializer request timed out"
                return {"ok": False, "error": error, "timeout": True}
            with self._webview_serializer_request_lock:
                pending = self._webview_serializer_requests.get(
                    request_id, {})
            response = pending.get("response", {})
            if isinstance(response, dict) and response.get("ok"):
                ok = True
                return dict(response)
            error = (
                response.get("error")
                if isinstance(response, dict)
                else "Node webview panel serializer failed")
            return {"ok": False, "error": error}
        finally:
            with self._webview_serializer_request_lock:
                self._webview_serializer_requests.pop(request_id, None)
            self._record_diagnostic(
                "webview_serializer",
                (time.perf_counter() - started) * 1000,
                ok=ok,
                timeout=timed_out,
                detail=detail,
                error=error,
            )

    def request_custom_editor_lifecycle(
            self, action: str, view_type: str = "", uri: str = "",
            view_id: str = "", target: str = "",
            timeout: float = 2.0) -> Dict[str, Any]:
        """Ask Node to run a custom editor save/revert/backup lifecycle hook."""
        started = time.perf_counter()
        ok = False
        timed_out = False
        error = ""
        detail = str(action or "")
        if not self.is_running:
            error = "Node extension host is not running"
            self._record_diagnostic(
                "custom_editor_lifecycle", 0, ok=False,
                detail=detail, error=error)
            return {"ok": False, "error": error}
        request_id = str(uuid.uuid4())
        event = threading.Event()
        with self._custom_editor_lifecycle_lock:
            self._custom_editor_lifecycle_requests[request_id] = {"event": event}
        try:
            sent = self._send({
                "type": "custom_editor_lifecycle",
                "requestId": request_id,
                "action": str(action or ""),
                "viewType": str(view_type or ""),
                "uri": str(uri or ""),
                "viewId": str(view_id or ""),
                "target": str(target or ""),
            })
            if not sent:
                error = "Node custom editor lifecycle request could not be sent"
                return {"ok": False, "error": error}
            if not event.wait(timeout):
                timed_out = True
                error = "Node custom editor lifecycle request timed out"
                return {"ok": False, "error": error, "timeout": True}
            with self._custom_editor_lifecycle_lock:
                pending = self._custom_editor_lifecycle_requests.get(
                    request_id, {})
            response = pending.get("response", {})
            if isinstance(response, dict) and response.get("ok"):
                ok = True
                return dict(response)
            error = (
                response.get("error")
                if isinstance(response, dict)
                else "Node custom editor lifecycle failed")
            return {"ok": False, "error": error}
        finally:
            with self._custom_editor_lifecycle_lock:
                self._custom_editor_lifecycle_requests.pop(request_id, None)
            self._record_diagnostic(
                "custom_editor_lifecycle",
                (time.perf_counter() - started) * 1000,
                ok=ok,
                timeout=timed_out,
                detail=detail,
                error=error,
            )

    def list_custom_editor_states(self) -> List[Dict[str, Any]]:
        """Return resolved custom editor dirty/lifecycle state snapshots."""
        with self._custom_editor_state_lock:
            return [dict(item) for item in self._custom_editor_states.values()]

    def custom_editor_state(
            self, view_id: str = "", view_type: str = "",
            uri: str = "") -> Dict[str, Any]:
        """Return one custom editor state snapshot when known."""
        state = self._find_custom_editor_state(view_id, view_type, uri)
        return dict(state or {})

    # -- Webview message relay -----------------------------------------------

    def relay_webview_message(self, view_id: str, message: Any) -> bool:
        """Relay a message from the webview UI to the Node subprocess.

        The Node side fires the extension's ``onDidReceiveMessage`` handler.
        """
        return self._send({
            "type": "webviewMessage",
            "viewId": view_id,
            "message": message,
        })

    def update_webview_panel_view_state(
            self, view_id: str, state: Optional[Dict[str, Any]] = None) -> bool:
        """Relay frontend webview panel visibility/focus state to Node."""
        payload = {
            "type": "webview_panel_view_state",
            "viewId": str(view_id or ""),
        }
        if isinstance(state, dict):
            for key in ("active", "visible", "viewColumn"):
                if key in state:
                    payload[key] = state.get(key)
        return self._send(payload)

    def send_quick_input_action(
            self, input_id: str, action: str,
            payload: Optional[Dict[str, Any]] = None) -> bool:
        """Relay a frontend QuickInput interaction to the Node subprocess."""
        msg = {
            "type": "quick_input_action",
            "id": str(input_id or ""),
            "action": str(action or ""),
        }
        if isinstance(payload, dict):
            msg.update(payload)
        return self._send(msg)

    # -- Integration helpers -------------------------------------------------

    def set_command_service(self, commands: CommandService) -> None:
        """Wire the CommandService so Node-registered commands are usable."""
        self._command_service = commands

    def set_ui_bridge(self, bridge: Any) -> None:
        """Update the UI bridge after construction."""
        self._ui_bridge = bridge

    def set_lm_model_request_callback(
            self,
            callback: Optional[
                Callable[[Dict[str, Any]], Dict[str, Any]]]) -> None:
        """Wire Python-backed language model selection/count/request APIs."""
        self._lm_model_request_callback = callback

    def set_lm_model_cancel_callback(
            self,
            callback: Optional[Callable[[Dict[str, Any]], Any]]) -> None:
        """Wire cancellation for Python-backed language model requests."""
        self._lm_model_cancel_callback = callback

    def on_activated(self, callback: Callable[[str], None]) -> None:
        """Register a callback invoked when a Node extension confirms activation."""
        self._on_activated_callbacks.append(callback)

    def on_error(self, callback: Callable[[str, str], None]) -> None:
        """Register a callback invoked on Node extension errors."""
        self._on_error_callbacks.append(callback)

    def is_extension_activated(self, extension_id: str) -> bool:
        return extension_id in self._activated_ids

    def is_extension_activation_pending(self, extension_id: str) -> bool:
        return (
            extension_id in self._activation_sent_ids
            and extension_id not in self._activated_ids
        )

    def list_activated(self) -> List[str]:
        return sorted(self._activated_ids)

    def list_language_providers(self) -> List[Dict[str, Any]]:
        """Return registered language provider capabilities from Node."""
        return list(self._language_providers)

    def set_diagnostics_enabled(self, enabled: bool) -> None:
        """Enable lightweight in-memory request diagnostics."""
        self._diagnostics_enabled = bool(enabled)

    def diagnostics_enabled(self) -> bool:
        return self._diagnostics_enabled

    def reset_diagnostics(self) -> None:
        with self._diagnostics_lock:
            self._diagnostics.clear()

    def diagnostics_snapshot(self) -> Dict[str, Any]:
        with self._diagnostics_lock:
            categories = {
                name: self._diagnostic_snapshot_for(data)
                for name, data in self._diagnostics.items()
            }
        return {
            "enabled": self._diagnostics_enabled,
            "running": self.is_running,
            "activated": len(self._activated_ids),
            "pending": {
                "commands": len(self._command_requests),
                "tree": len(self._tree_requests),
                "language": len(self._language_requests),
                "customEditors": len(self._custom_editor_requests),
                "customEditorLifecycle": len(
                    self._custom_editor_lifecycle_requests),
                "webviewSerializers": len(
                    self._webview_serializer_requests),
            },
            "categories": categories,
        }

    # -- Private helpers -----------------------------------------------------

    def _record_diagnostic(
            self, category: str, elapsed_ms: float, ok: bool = True,
            timeout: bool = False, detail: str = "",
            error: str = "") -> None:
        if not self._diagnostics_enabled:
            return
        name = str(category or "node")
        try:
            elapsed = float(elapsed_ms)
        except Exception:
            elapsed = 0.0
        with self._diagnostics_lock:
            data = self._diagnostics.setdefault(name, {
                "count": 0,
                "ok": 0,
                "errors": 0,
                "timeouts": 0,
                "total_ms": 0.0,
                "max_ms": 0.0,
                "last_ms": 0.0,
                "last_detail": "",
                "last_error": "",
            })
            data["count"] += 1
            if ok:
                data["ok"] += 1
            else:
                data["errors"] += 1
            if timeout:
                data["timeouts"] += 1
            data["total_ms"] += max(0.0, elapsed)
            data["last_ms"] = max(0.0, elapsed)
            data["max_ms"] = max(float(data.get("max_ms") or 0), elapsed)
            data["last_detail"] = str(detail or "")[:200]
            data["last_error"] = str(error or "")[:300]

    @staticmethod
    def _diagnostic_snapshot_for(data: Dict[str, Any]) -> Dict[str, Any]:
        count = int(data.get("count") or 0)
        total = float(data.get("total_ms") or 0.0)
        avg = total / count if count else 0.0
        return {
            "count": count,
            "ok": int(data.get("ok") or 0),
            "errors": int(data.get("errors") or 0),
            "timeouts": int(data.get("timeouts") or 0),
            "avg_ms": round(avg, 3),
            "max_ms": round(float(data.get("max_ms") or 0.0), 3),
            "last_ms": round(float(data.get("last_ms") or 0.0), 3),
            "last_detail": data.get("last_detail", ""),
            "last_error": data.get("last_error", ""),
        }

    def _resolve_node(self) -> Optional[str]:
        """Find the node executable: explicit path, then PATH lookup."""
        if self._node_path:
            expanded = os.path.expandvars(os.path.expanduser(self._node_path))
            if os.path.isfile(expanded):
                return expanded
            found = shutil.which(expanded)
            if found:
                return found
        # Fallback: try "node" on PATH
        return shutil.which("node")


def find_node_path() -> Optional[str]:
    """Locate a usable Node.js executable on the system.

    Delegates to :func:`ai_editor.node_runtime.get_node_path` which checks
    the bundled binary, user PATH, and common install locations.

    Returns the absolute path or None if node is not found.
    """
    try:
        from ai_editor.node_runtime import get_node_path
        return get_node_path()
    except Exception:
        return shutil.which("node")


# ---------------------------------------------------------------------------
# Singleton
# ---------------------------------------------------------------------------

_host: Optional[ExtensionHost] = None


def get_extension_host() -> ExtensionHost:
    global _host
    if _host is None:
        _host = ExtensionHost()
    return _host
