# -*- coding: utf-8 -*-
"""In-process Python plugin manager for ACT platform extensions."""

from __future__ import annotations

import copy
import importlib.util
import json
import os
import sys
import time
import traceback
from dataclasses import dataclass, field
from types import ModuleType
from typing import Any, Callable, Dict, Iterable, Mapping, Optional

from .event_bus import EventBus


MANIFEST_FILE = "plugin.json"
EXTENSION_KINDS = (
    "parser_adapters",
    "exporters",
    "trigger_types",
    "report_views",
    "timers",
)


def _safe_id(value: Any) -> str:
    text = str(value or "").strip().replace(" ", "_").lower()
    out = []
    for ch in text:
        if ch.isalnum() or ch in ("_", "-", "."):
            out.append(ch)
    return "".join(out).strip("._-")


def _normalize_capability(item: Any) -> Optional[dict[str, Any]]:
    if isinstance(item, str):
        raw_id = str(item or "").strip()
        cap_id = _safe_id(raw_id)
        if cap_id != raw_id:
            return None
        return {"id": cap_id} if cap_id else None
    if not isinstance(item, Mapping):
        return None
    raw_id = str(item.get("id") or item.get("capability_id") or "").strip()
    cap_id = _safe_id(raw_id)
    if cap_id != raw_id:
        return None
    if not cap_id:
        return None
    normalized: dict[str, Any] = {"id": cap_id}
    for key in ("title", "description", "route", "render_hint"):
        value = item.get(key)
        if value is not None:
            normalized[key] = str(value)
    for key in ("actions", "payload_fields"):
        value = item.get(key)
        if isinstance(value, (list, tuple)):
            normalized[key] = [str(x) for x in value if str(x or "").strip()]
    return normalized


def _normalize_capabilities(value: Any) -> tuple[dict[str, Any], ...]:
    if not isinstance(value, (list, tuple)):
        return ()
    out: list[dict[str, Any]] = []
    seen: set[str] = set()
    for item in value:
        normalized = _normalize_capability(item)
        if not normalized:
            continue
        cap_id = str(normalized["id"])
        if cap_id in seen:
            continue
        seen.add(cap_id)
        out.append(normalized)
    return tuple(out)


def _json_safe(value: Any) -> Any:
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    if isinstance(value, Mapping):
        return {str(key): _json_safe(val) for key, val in value.items() if not callable(val)}
    if isinstance(value, (list, tuple, set)):
        return [_json_safe(item) for item in value if not callable(item)]
    return str(value)


def _normalize_extension(kind: str, plugin_id: str, extension_id: Any,
                         metadata: Optional[Mapping[str, Any]] = None) -> dict[str, Any]:
    kind = str(kind or "")
    if kind not in EXTENSION_KINDS:
        raise ValueError(f"unsupported extension kind: {kind}")
    raw_id = str(extension_id or "").strip()
    ext_id = _safe_id(raw_id)
    if not ext_id or ext_id != raw_id:
        raise ValueError(f"invalid extension id: {raw_id!r}")
    src = dict(metadata or {}) if isinstance(metadata, Mapping) else {}
    normalized: dict[str, Any] = {
        "id": ext_id,
        "kind": kind,
        "plugin_id": str(plugin_id or ""),
    }
    for key in (
        "title",
        "description",
        "version",
        "route",
        "render_hint",
        "format",
        "scope",
        "label",
        "display_name",
        "game_id",
        "isolation",
        "isolation_mode",
    ):
        value = src.get(key)
        if value is not None:
            normalized[key] = str(value)
    for key in (
        "game_ids",
        "source_kinds",
        "actions",
        "payload_fields",
        "formats",
        "permissions",
        "supported_locales",
        "locales",
    ):
        value = src.get(key)
        if isinstance(value, (list, tuple, set)):
            normalized[key] = [str(item) for item in value if str(item or "").strip()]
    for key in ("priority", "duration_s", "cooldown_s", "time_budget_ms", "max_runtime_ms"):
        value = src.get(key)
        if value is not None:
            try:
                normalized[key] = float(value)
            except Exception:
                pass
    for key in ("schema", "settings_schema"):
        value = src.get(key)
        if isinstance(value, Mapping):
            normalized[key] = _json_safe(value)
    return normalized


@dataclass
class PluginRecord:
    plugin_id: str
    name: str
    version: str
    path: str
    entry: str
    enabled: bool = True
    game_ids: tuple[str, ...] = ("star_resonance",)
    permissions: tuple[str, ...] = ()
    capabilities: tuple[dict[str, Any], ...] = ()
    settings_schema: Mapping[str, Any] = field(default_factory=dict)
    module: Optional[ModuleType] = None
    context: Optional["PluginContext"] = None
    loaded: bool = False
    active: bool = False
    failures: int = 0
    event_failures: int = 0
    last_error: str = ""
    last_loaded_at: float = 0.0
    subscriptions: list[str] = field(default_factory=list)
    extensions: dict[str, list[str]] = field(default_factory=dict)
    logs: list[str] = field(default_factory=list)

    def to_status(self) -> dict[str, Any]:
        return {
            "id": self.plugin_id,
            "name": self.name,
            "version": self.version,
            "path": self.path,
            "entry": self.entry,
            "enabled": self.enabled,
            "loaded": self.loaded,
            "active": self.active,
            "game_ids": list(self.game_ids),
            "permissions": list(self.permissions),
            "capabilities": [dict(cap) for cap in self.capabilities],
            "capability_ids": [str(cap.get("id")) for cap in self.capabilities if cap.get("id")],
            "failures": self.failures,
            "event_failures": self.event_failures,
            "last_error": self.last_error,
            "last_loaded_at": self.last_loaded_at,
            "subscription_count": len(self.subscriptions),
            "extensions": {kind: list(ids) for kind, ids in sorted(self.extensions.items()) if ids},
            "extension_count": sum(len(ids) for ids in self.extensions.values()),
            "logs": list(self.logs[-20:]),
        }


class PluginContext:
    """Small SDK object passed to in-process plugins."""

    def __init__(self, manager: "PluginManager", record: PluginRecord) -> None:
        self._manager = manager
        self._record = record

    @property
    def plugin_id(self) -> str:
        return self._record.plugin_id

    @property
    def event_bus(self) -> EventBus:
        return self._manager.event_bus

    def log(self, message: Any) -> None:
        self._manager._append_log(self._record.plugin_id, str(message))

    def on(self, topic: str, callback: Optional[Callable[[dict[str, Any]], None]] = None):
        """Subscribe to an ACT topic, or use as ``@ctx.on('damage')``."""
        if callback is None:
            def _decorator(func: Callable[[dict[str, Any]], None]):
                self.subscribe(topic, func)
                return func
            return _decorator
        return self.subscribe(topic, callback)

    def subscribe(self, topic: str, callback: Callable[[dict[str, Any]], None]) -> str:
        def _wrapped(event: dict[str, Any]) -> None:
            try:
                callback(event)
            except Exception as exc:
                self._manager._record_event_failure(self._record.plugin_id, exc)
                raise

        token = self._manager.event_bus.subscribe(topic, _wrapped, owner_id=self._record.plugin_id)
        self._record.subscriptions.append(token)
        return token

    def subscribe_once(self, topic: str, callback: Callable[[dict[str, Any]], None]) -> str:
        token_box = {"token": ""}

        def _once(event: dict[str, Any]) -> None:
            token = token_box.get("token") or ""
            if token:
                self.unsubscribe(token)
            callback(event)

        token_box["token"] = self.subscribe(topic, _once)
        return token_box["token"]

    def unsubscribe(self, token: str) -> bool:
        token = str(token or "")
        if not token:
            return False
        ok = self._manager.event_bus.unsubscribe(token)
        if ok:
            self._record.subscriptions = [item for item in self._record.subscriptions if item != token]
        return ok

    def on_damage(self, callback: Callable[[dict[str, Any]], None]) -> str:
        return self.subscribe("damage", callback)

    def on_heal(self, callback: Callable[[dict[str, Any]], None]) -> str:
        return self.subscribe("heal", callback)

    def on_skill(self, callback: Callable[[dict[str, Any]], None]) -> str:
        return self.subscribe("skill", callback)

    def on_boss(self, callback: Callable[[dict[str, Any]], None]) -> str:
        return self.subscribe("boss", callback)

    def on_snapshot(self, callback: Callable[[dict[str, Any]], None]) -> str:
        return self.subscribe("act_snapshot", callback)

    def on_encounter_finalized(self, callback: Callable[[dict[str, Any]], None]) -> str:
        return self.subscribe("encounter_finalized", callback)

    def emit(self, topic: str, payload: Optional[Mapping[str, Any]] = None) -> dict[str, Any]:
        return self._manager.event_bus.publish(
            topic,
            payload or {},
            source_name=self._record.plugin_id,
            source_kind="plugin",
        )

    def get_snapshot(self) -> dict[str, Any]:
        provider = self._manager.snapshot_provider
        if callable(provider):
            try:
                return dict(provider() or {})
            except Exception as exc:
                self._manager._record_failure(self._record.plugin_id, exc)
        return {}

    def snapshot_value(self, path: str, default: Any = None) -> Any:
        value: Any = self.get_snapshot()
        for part in str(path or "").split("."):
            if not part:
                continue
            if isinstance(value, Mapping) and part in value:
                value = value.get(part)
            else:
                return default
        return value

    def recent_events(self, limit: int = 20, topic: str = "") -> list[dict[str, Any]]:
        events = self._manager.event_bus.recent_events(limit)
        topic = str(topic or "")
        if topic:
            events = [event for event in events if str(event.get("topic") or "") == topic]
        return events

    def get_setting(self, key: str, default: Any = None) -> Any:
        return self._manager.get_plugin_setting(self._record.plugin_id, key, default)

    def setting(self, key: str, default: Any = None) -> Any:
        return self.get_setting(key, default)

    def set_setting(self, key: str, value: Any) -> None:
        self._manager.set_plugin_setting(self._record.plugin_id, key, value)

    def set_defaults(self, defaults: Mapping[str, Any]) -> None:
        if not isinstance(defaults, Mapping):
            return
        for key, value in defaults.items():
            key_text = str(key or "")
            if not key_text:
                continue
            sentinel = object()
            if self.get_setting(key_text, sentinel) is sentinel:
                self.set_setting(key_text, value)

    def register_parser_adapter(self, adapter_id: str,
                                metadata: Optional[Mapping[str, Any]] = None,
                                handler: Optional[Callable[..., Any]] = None) -> dict[str, Any]:
        return self._manager._register_extension("parser_adapters", self._record.plugin_id, adapter_id, metadata, handler)

    def register_exporter(self, exporter_id: str,
                          metadata: Optional[Mapping[str, Any]] = None,
                          handler: Optional[Callable[..., Any]] = None) -> dict[str, Any]:
        return self._manager._register_extension("exporters", self._record.plugin_id, exporter_id, metadata, handler)

    def register_trigger_type(self, trigger_type: str,
                              metadata: Optional[Mapping[str, Any]] = None,
                              handler: Optional[Callable[..., Any]] = None) -> dict[str, Any]:
        return self._manager._register_extension("trigger_types", self._record.plugin_id, trigger_type, metadata, handler)

    def register_report_view(self, view_id: str,
                             metadata: Optional[Mapping[str, Any]] = None,
                             handler: Optional[Callable[..., Any]] = None) -> dict[str, Any]:
        return self._manager._register_extension("report_views", self._record.plugin_id, view_id, metadata, handler)

    def register_timer(self, timer_id: str,
                       metadata: Optional[Mapping[str, Any]] = None,
                       handler: Optional[Callable[..., Any]] = None) -> dict[str, Any]:
        return self._manager._register_extension("timers", self._record.plugin_id, timer_id, metadata, handler)


class PluginManager:
    """Discover and manage in-process Python ACT plugins."""

    def __init__(self, plugin_dirs: Optional[Iterable[str]] = None,
                 event_bus: Optional[EventBus] = None,
                 snapshot_provider: Optional[Callable[[], Mapping[str, Any]]] = None,
                 settings: Any = None,
                 max_failures: int = 3) -> None:
        self.plugin_dirs = [os.path.abspath(path) for path in (plugin_dirs or []) if path]
        self.event_bus = event_bus or EventBus()
        self.snapshot_provider = snapshot_provider
        self.settings = settings
        self.max_failures = max(1, int(max_failures or 3))
        self._records: Dict[str, PluginRecord] = {}
        self._settings_cache: Dict[str, Dict[str, Any]] = {}
        self._extensions: Dict[str, Dict[str, dict[str, Any]]] = {kind: {} for kind in EXTENSION_KINDS}
        self._extension_handlers: Dict[tuple[str, str], Callable[..., Any]] = {}

    def discover(self) -> list[PluginRecord]:
        self._extensions = {kind: {} for kind in EXTENSION_KINDS}
        self._extension_handlers.clear()
        records: Dict[str, PluginRecord] = {}
        for root in self.plugin_dirs:
            if not os.path.isdir(root):
                continue
            for name in sorted(os.listdir(root)):
                plug_dir = os.path.join(root, name)
                manifest_path = os.path.join(plug_dir, MANIFEST_FILE)
                if not os.path.isdir(plug_dir) or not os.path.isfile(manifest_path):
                    continue
                try:
                    record = self._read_manifest(plug_dir, manifest_path)
                except Exception as exc:
                    plugin_id = _safe_id(name) or f"invalid_{len(records) + 1}"
                    record = PluginRecord(
                        plugin_id=plugin_id,
                        name=name,
                        version="0",
                        path=plug_dir,
                        entry="",
                        enabled=False,
                        loaded=False,
                        active=False,
                        failures=1,
                        last_error=f"manifest: {exc}",
                    )
                records[record.plugin_id] = record
        self._records = records
        return list(self._records.values())

    def load_all(self) -> dict[str, Any]:
        if not self._records:
            self.discover()
        for plugin_id in sorted(self._records):
            record = self._records[plugin_id]
            if record.enabled:
                self.load_plugin(plugin_id)
        return self.status()

    def load_plugin(self, plugin_id: str) -> bool:
        record = self._records.get(str(plugin_id or ""))
        if record is None:
            return False
        self.unload_plugin(record.plugin_id)
        if not record.enabled:
            return False
        try:
            module = self._load_module(record)
            record.module = module
            record.context = PluginContext(self, record)
            self._call_hook(record, "on_load", record.context)
            self._call_hook(record, "on_enable")
            record.loaded = True
            record.active = True
            record.last_loaded_at = time.time()
            record.last_error = ""
            return True
        except Exception as exc:
            self._record_failure(record.plugin_id, exc)
            for token in list(record.subscriptions):
                self.event_bus.unsubscribe(token)
            record.subscriptions.clear()
            self._unregister_plugin_extensions(record.plugin_id)
            record.module = None
            record.context = None
            record.loaded = False
            record.active = False
            return False

    def unload_plugin(self, plugin_id: str) -> bool:
        record = self._records.get(str(plugin_id or ""))
        if record is None:
            return False
        if record.module is not None:
            try:
                self._call_hook(record, "on_disable")
            except Exception:
                pass
            try:
                self._call_hook(record, "on_unload")
            except Exception:
                pass
        for token in list(record.subscriptions):
            self.event_bus.unsubscribe(token)
        record.subscriptions.clear()
        self._unregister_plugin_extensions(record.plugin_id)
        record.module = None
        record.context = None
        record.loaded = False
        record.active = False
        return True

    def enable_plugin(self, plugin_id: str) -> bool:
        record = self._records.get(str(plugin_id or ""))
        if record is None:
            return False
        record.enabled = True
        return self.load_plugin(record.plugin_id)

    def disable_plugin(self, plugin_id: str) -> bool:
        record = self._records.get(str(plugin_id or ""))
        if record is None:
            return False
        record.enabled = False
        self.unload_plugin(record.plugin_id)
        return True

    def reload_plugin(self, plugin_id: str) -> bool:
        record = self._records.get(str(plugin_id or ""))
        if record is None:
            return False
        return self.load_plugin(record.plugin_id) if record.enabled else False

    def reload_all(self) -> dict[str, Any]:
        for plugin_id in list(self._records):
            self.unload_plugin(plugin_id)
        self.discover()
        return self.load_all()

    def list_plugins(self) -> list[dict[str, Any]]:
        return [self._records[key].to_status() for key in sorted(self._records)]

    def list_extensions(self, kind: str = "") -> dict[str, list[dict[str, Any]]] | list[dict[str, Any]]:
        if kind:
            bucket = self._extensions.get(str(kind or ""), {})
            return [dict(bucket[key]) for key in sorted(bucket)]
        return {
            ext_kind: [dict(bucket[key]) for key in sorted(bucket)]
            for ext_kind, bucket in self._extensions.items()
        }

    def invoke_extension(self, kind: str, extension_id: str,
                         payload: Optional[Mapping[str, Any]] = None, *,
                         time_budget_ms: float = 25.0) -> dict[str, Any]:
        kind = str(kind or "")
        ext_id = str(extension_id or "").strip()
        meta = self._extensions.get(kind, {}).get(ext_id)
        if not isinstance(meta, Mapping):
            return {"ok": False, "message": f"extension not found: {kind}/{ext_id}", "errors": ["extension not found"]}
        plugin_id = str(meta.get("plugin_id") or "")
        record = self._records.get(plugin_id)
        if record is None or not record.loaded or not record.active or not record.enabled:
            return {"ok": False, "plugin_id": plugin_id, "extension_id": ext_id, "message": "plugin is not active", "errors": ["plugin is not active"]}
        handler = self._extension_handlers.get((kind, ext_id))
        if not callable(handler):
            return {"ok": False, "plugin_id": plugin_id, "extension_id": ext_id, "message": "extension handler is unavailable", "errors": ["extension handler is unavailable"]}
        budget_value = meta.get("time_budget_ms")
        if budget_value is None:
            budget_value = meta.get("max_runtime_ms")
        if budget_value is None:
            budget_value = time_budget_ms
        if budget_value is None:
            budget_value = 25.0
        budget = float(budget_value)
        started = time.perf_counter()
        try:
            result = handler(copy.deepcopy(dict(payload or {})))
        except Exception as exc:
            elapsed_ms = (time.perf_counter() - started) * 1000.0
            self._record_failure(plugin_id, exc)
            return {
                "ok": False,
                "plugin_id": plugin_id,
                "extension_id": ext_id,
                "elapsed_ms": elapsed_ms,
                "time_budget_ms": budget,
                "message": str(exc),
                "errors": [str(exc)],
            }
        elapsed_ms = (time.perf_counter() - started) * 1000.0
        if budget > 0 and elapsed_ms > budget:
            exc = TimeoutError(f"extension handler exceeded budget: {elapsed_ms:.1f}ms > {budget:.1f}ms")
            self._record_failure(plugin_id, exc)
            return {
                "ok": False,
                "plugin_id": plugin_id,
                "extension_id": ext_id,
                "elapsed_ms": elapsed_ms,
                "time_budget_ms": budget,
                "timed_out": True,
                "message": str(exc),
                "errors": [str(exc)],
            }
        return {
            "ok": True,
            "plugin_id": plugin_id,
            "extension_id": ext_id,
            "elapsed_ms": elapsed_ms,
            "time_budget_ms": budget,
            "result": _json_safe(result),
            "errors": [],
        }

    def status(self) -> dict[str, Any]:
        capabilities: dict[str, list[str]] = {}
        for record in self._records.values():
            for cap in record.capabilities:
                cap_id = str(cap.get("id") or "")
                if cap_id:
                    capabilities.setdefault(cap_id, []).append(record.plugin_id)
        for cap_id in list(capabilities):
            capabilities[cap_id] = sorted(set(capabilities[cap_id]))
        return {
            "ok": True,
            "plugin_count": len(self._records),
            "active_count": sum(1 for record in self._records.values() if record.active),
            "plugins": self.list_plugins(),
            "capabilities": dict(sorted(capabilities.items())),
            "extensions": self.list_extensions(),
            "extension_counts": {
                kind: len(bucket)
                for kind, bucket in self._extensions.items()
            },
            "event_bus": self.event_bus.snapshot(),
        }

    def get_plugin_setting(self, plugin_id: str, key: str, default: Any = None) -> Any:
        if self.settings is not None and hasattr(self.settings, "get"):
            raw = self.settings.get("act_plugin_settings", {}) or {}
            if isinstance(raw, dict):
                return (raw.get(plugin_id) or {}).get(key, default) if isinstance(raw.get(plugin_id), dict) else default
        return self._settings_cache.get(plugin_id, {}).get(key, default)

    def set_plugin_setting(self, plugin_id: str, key: str, value: Any) -> None:
        if self.settings is not None and hasattr(self.settings, "get") and hasattr(self.settings, "set"):
            raw = self.settings.get("act_plugin_settings", {}) or {}
            if not isinstance(raw, dict):
                raw = {}
            plug = raw.get(plugin_id) if isinstance(raw.get(plugin_id), dict) else {}
            plug[key] = value
            raw[plugin_id] = plug
            self.settings.set("act_plugin_settings", raw)
            save = getattr(self.settings, "save", None)
            if callable(save):
                try:
                    save()
                except Exception:
                    pass
            return
        self._settings_cache.setdefault(plugin_id, {})[key] = value

    def _register_extension(self, kind: str, plugin_id: str, extension_id: str,
                            metadata: Optional[Mapping[str, Any]] = None,
                            handler: Optional[Callable[..., Any]] = None) -> dict[str, Any]:
        record = self._records.get(str(plugin_id or ""))
        if record is None:
            raise ValueError(f"plugin is not loaded: {plugin_id}")
        normalized = _normalize_extension(kind, record.plugin_id, extension_id, metadata)
        bucket = self._extensions.setdefault(kind, {})
        existing = bucket.get(normalized["id"])
        if existing and str(existing.get("plugin_id") or "") != record.plugin_id:
            raise ValueError(f"extension id already registered: {kind}/{normalized['id']}")
        bucket[normalized["id"]] = normalized
        ids = record.extensions.setdefault(kind, [])
        if normalized["id"] not in ids:
            ids.append(normalized["id"])
            ids.sort()
        if callable(handler):
            self._extension_handlers[(kind, normalized["id"])] = handler
        else:
            self._extension_handlers.pop((kind, normalized["id"]), None)
        return dict(normalized)

    def _unregister_plugin_extensions(self, plugin_id: str) -> None:
        record = self._records.get(str(plugin_id or ""))
        for kind, bucket in self._extensions.items():
            stale = [ext_id for ext_id, item in bucket.items() if str(item.get("plugin_id") or "") == str(plugin_id or "")]
            for ext_id in stale:
                bucket.pop(ext_id, None)
                self._extension_handlers.pop((kind, ext_id), None)
        if record is not None:
            record.extensions.clear()

    def _read_manifest(self, plug_dir: str, manifest_path: str) -> PluginRecord:
        with open(manifest_path, "r", encoding="utf-8") as fp:
            manifest = json.load(fp)
        plugin_id = _safe_id(manifest.get("id") or os.path.basename(plug_dir))
        if not plugin_id:
            raise ValueError("plugin id is required")
        entry = str(manifest.get("entry") or "plugin.py").strip()
        if not entry:
            raise ValueError("plugin entry is required")
        entry_path = os.path.abspath(os.path.join(plug_dir, entry))
        if os.path.commonpath([os.path.abspath(plug_dir), entry_path]) != os.path.abspath(plug_dir):
            raise ValueError("plugin entry must stay inside plugin directory")
        if not os.path.isfile(entry_path):
            raise FileNotFoundError(entry)
        return PluginRecord(
            plugin_id=plugin_id,
            name=str(manifest.get("name") or plugin_id),
            version=str(manifest.get("version") or "0.1.0"),
            path=plug_dir,
            entry=entry,
            enabled=bool(manifest.get("enabled", True)),
            game_ids=tuple(str(x) for x in manifest.get("game_ids", ["star_resonance"])),
            permissions=tuple(str(x) for x in manifest.get("permissions", [])),
            capabilities=_normalize_capabilities(manifest.get("capabilities", [])),
            settings_schema=manifest.get("settings_schema") if isinstance(manifest.get("settings_schema"), dict) else {},
        )

    def _load_module(self, record: PluginRecord) -> ModuleType:
        entry_path = os.path.abspath(os.path.join(record.path, record.entry))
        module_name = f"act_plugin_{record.plugin_id.replace('-', '_').replace('.', '_')}"
        if module_name in sys.modules:
            del sys.modules[module_name]
        spec = importlib.util.spec_from_file_location(module_name, entry_path)
        if spec is None or spec.loader is None:
            raise ImportError(f"cannot load plugin entry: {entry_path}")
        module = importlib.util.module_from_spec(spec)
        sys.modules[module_name] = module
        spec.loader.exec_module(module)
        return module

    def _call_hook(self, record: PluginRecord, name: str, *args: Any) -> Any:
        module = record.module
        hook = getattr(module, name, None) if module is not None else None
        if not callable(hook):
            return None
        try:
            return hook(*args)
        except Exception as exc:
            self._record_failure(record.plugin_id, exc)
            raise

    def _append_log(self, plugin_id: str, message: str) -> None:
        record = self._records.get(plugin_id)
        if record is None:
            return
        record.logs.append(f"{time.strftime('%H:%M:%S')} {message}")
        record.logs = record.logs[-50:]

    def _record_failure(self, plugin_id: str, exc: BaseException) -> None:
        record = self._records.get(plugin_id)
        if record is None:
            return
        record.failures += 1
        record.last_error = "".join(traceback.format_exception_only(type(exc), exc)).strip()
        self._append_log(plugin_id, f"ERROR {record.last_error}")
        if record.failures >= self.max_failures:
            record.enabled = False
            record.active = False

    def _record_event_failure(self, plugin_id: str, exc: BaseException) -> None:
        record = self._records.get(plugin_id)
        if record is None:
            return
        record.event_failures += 1
        self._record_failure(plugin_id, exc)


__all__ = ["PluginContext", "PluginManager", "PluginRecord"]
