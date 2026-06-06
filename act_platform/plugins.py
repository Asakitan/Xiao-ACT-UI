# -*- coding: utf-8 -*-
"""In-process Python plugin manager for ACT platform extensions."""

from __future__ import annotations

import copy
import importlib
import importlib.util
import json
import os
import sys
import threading
import time
import traceback
from dataclasses import dataclass, field
from types import ModuleType
from typing import Any, Callable, Dict, Iterable, Mapping, Optional

from .event_bus import EventBus
from .render_hooks import RenderHookRegistry
from .ui_spec import UI, normalize_ui_spec


MANIFEST_FILE = "plugin.json"
EXTENSION_KINDS = (
    "parser_adapters",
    "exporters",
    "formatters",
    "trigger_types",
    "report_views",
    "timers",
    "ui_panels",
)
# Render hooks are NOT extension-registry entries — they live in
# ``PluginManager.render_registry`` and surface via ``render_status()``.


_ENGINE_HANDLE_ALIASES: dict[str, tuple[str, ...]] = {
    "owner": (),
    "event_bus": (),
    "plugin_manager": (),
    "settings": (),
    "game_state": ("_game_state", "game_state"),
    "state_manager": ("_state_mgr", "state_mgr", "state_manager"),
    "dps_tracker": ("_dps_tracker", "dps_tracker", "tracker", "dps"),
    "history_store": ("_dps_history_store", "dps_history_store", "history", "store"),
    "encounter_manager": ("_encounter_mgr", "encounter_mgr", "encounter_manager"),
    "trigger_engine": ("_act_trigger_engine", "act_trigger_engine", "trigger_engine"),
    "packet_bridge": ("_packet_engine", "_packet_bridge", "packet_engine", "packet_bridge"),
    "memory_bridge": ("_mem_bridge", "mem_bridge", "memory_bridge"),
    "auto_key_engine": ("_auto_key_engine", "auto_key_engine"),
    "boss_raid_engine": ("_boss_raid_engine", "boss_raid_engine"),
    "window_locator": ("_locator", "locator", "window_locator"),
}


_RUNTIME_ACTION_ALIASES: dict[str, str] = {
    "plugin_status": "act_plugin_status",
    "plugin_list": "act_plugin_list",
    "history_status": "act_history_status",
    "history_load": "act_history_load",
    "history_delete": "act_history_delete",
    "report_status": "act_report_status",
    "report_export": "act_report_export",
    "report_copy": "act_report_copy",
    "mini_parse_status": "act_mini_parse_status",
    "mini_parse_preview": "act_mini_parse_preview",
    "mini_parse_copy": "act_mini_parse_copy",
    "selective_parsing_status": "act_selective_parsing_status",
    "selective_parsing_update": "act_selective_parsing_update",
    "selective_parsing_clear": "act_selective_parsing_clear",
    "offline_import_status": "act_offline_import_status",
    "offline_import_file": "act_offline_import_file",
    "trigger_status": "act_trigger_status",
    "trigger_enable": "act_trigger_enable",
    "trigger_disable": "act_trigger_disable",
    "trigger_reload": "act_trigger_reload",
    "trigger_test": "act_trigger_test",
    "trigger_export_presets": "act_trigger_export_presets",
    "trigger_import_presets": "act_trigger_import_presets",
    "timeline_status": "act_timeline_status",
    "timeline_play": "act_timeline_play",
    "timeline_pause": "act_timeline_pause",
    "timeline_seek": "act_timeline_seek",
    "timeline_step": "act_timeline_step",
    "action_log_status": "act_action_log_status",
    "action_log_copy": "act_action_log_copy",
    "death_recap_status": "act_death_recap_status",
    "death_recap_copy": "act_death_recap_copy",
    "graph_timeseries_status": "act_graph_timeseries_status",
    "graph_timeseries_export": "act_graph_timeseries_export",
    "combatant_drilldown_status": "act_combatant_drilldown_status",
    "skill_drilldown_status": "act_skill_drilldown_status",
    "data_source_health": "act_data_source_health",
    "data_source_diagnose": "act_data_source_diagnose",
    "ui_panels": "act_plugin_ui_panels",
    "ui_render": "act_plugin_ui_render",
    "ui_action": "act_plugin_ui_action",
    "render_surfaces": "act_render_surfaces",
    "render_apply_hooks": "act_render_apply_hooks",
    "render_overlays": "act_render_overlays",
}


_MISSING = object()


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


def _normalize_engine_name(value: Any) -> str:
    return str(value or "").strip().lower().replace("-", "_").replace(".", "_")


def _type_name(value: Any) -> str:
    if value is None:
        return ""
    cls = type(value)
    return f"{cls.__module__}.{cls.__name__}"


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
        "formatter",
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
        "examples",
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
    loaded_local_modules: list[str] = field(default_factory=list)

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
            # A plugin only "has a panel" if it DECLARES one — the manifest must
            # list a ``ui_panels`` capability. Lets the menu/manager know even
            # while the plugin is disabled (and so not yet runtime-registered).
            "declares_panel": any(str(cap.get("id")) == "ui_panels" for cap in self.capabilities),
            "failures": self.failures,
            "event_failures": self.event_failures,
            "last_error": self.last_error,
            "last_loaded_at": self.last_loaded_at,
            "subscription_count": len(self.subscriptions),
            "extensions": {kind: list(ids) for kind, ids in sorted(self.extensions.items()) if ids},
            "extension_count": sum(len(ids) for ids in self.extensions.values()),
            "logs": list(self.logs[-20:]),
        }


class EngineAccess:
    """Trusted in-process plugin bridge to SAO Auto owner/runtime engines."""

    def __init__(self, manager: "PluginManager", record: Optional[PluginRecord] = None) -> None:
        self._manager = manager
        self._record = record

    @property
    def owner(self) -> Any:
        provider = getattr(self._manager, "owner_provider", None)
        if callable(provider):
            try:
                return provider()
            except Exception:
                return None
        return None

    @property
    def plugin_manager(self) -> "PluginManager":
        return self._manager

    @property
    def event_bus(self) -> EventBus:
        return self._manager.event_bus

    @property
    def settings(self) -> Any:
        return self._manager.settings

    def handles(self) -> dict[str, dict[str, Any]]:
        out: dict[str, dict[str, Any]] = {}
        for name in _ENGINE_HANDLE_ALIASES:
            handle = self.get(name, None)
            out[name] = {
                "available": handle is not None,
                "type": _type_name(handle),
            }
        return out

    def available(self) -> list[str]:
        return [name for name, meta in self.handles().items() if meta.get("available")]

    def get(self, name: str, default: Any = None) -> Any:
        key = _normalize_engine_name(name)
        if key == "owner":
            owner = self.owner
            return owner if owner is not None else default
        if key == "event_bus":
            return self.event_bus
        if key == "plugin_manager":
            return self.plugin_manager
        if key == "settings":
            return self.settings if self.settings is not None else default
        owner = self.owner
        if owner is None:
            return default
        for canonical, aliases in _ENGINE_HANDLE_ALIASES.items():
            alias_keys = {_normalize_engine_name(alias) for alias in aliases}
            if key != canonical and key not in alias_keys:
                continue
            for attr in aliases:
                try:
                    value = getattr(owner, attr)
                except Exception:
                    continue
                if value is not None:
                    return value
            return default
        try:
            value = getattr(owner, str(name))
        except Exception:
            return default
        return value if value is not None else default

    def require(self, name: str) -> Any:
        value = self.get(name, _MISSING)
        if value is _MISSING or value is None:
            raise RuntimeError(f"engine handle is unavailable: {name}")
        return value

    def owner_attr(self, name: str, default: Any = None) -> Any:
        owner = self.owner
        if owner is None:
            return default
        try:
            return getattr(owner, str(name))
        except Exception:
            return default

    def set_owner_attr(self, name: str, value: Any) -> None:
        owner = self.require("owner")
        setattr(owner, str(name), value)

    def call_owner(self, method: str, *args: Any, **kwargs: Any) -> Any:
        owner = self.require("owner")
        fn = getattr(owner, str(method or ""), None)
        if not callable(fn):
            raise AttributeError(f"owner method is unavailable: {method}")
        return fn(*args, **kwargs)

    def call(self, engine_name: str, method: str, *args: Any, **kwargs: Any) -> Any:
        handle = self.require(engine_name)
        fn = getattr(handle, str(method or ""), None)
        if not callable(fn):
            raise AttributeError(f"{engine_name}.{method} is unavailable")
        return fn(*args, **kwargs)

    def runtime(self, action: str, *args: Any, **kwargs: Any) -> Any:
        owner = self.require("owner")
        action_key = _normalize_engine_name(action)
        fn_name = _RUNTIME_ACTION_ALIASES.get(action_key) or str(action or "").strip()
        if not fn_name.startswith("act_") and not fn_name.startswith("ensure_act_"):
            fn_name = f"act_{fn_name}"
        module = importlib.import_module("act_platform.runtime")
        fn = getattr(module, fn_name, None)
        if not callable(fn):
            raise AttributeError(f"runtime action is unavailable: {action}")
        return fn(owner, *args, **kwargs)

    invoke_runtime = runtime
    call_runtime = runtime

    def import_module(self, module_name: str) -> Any:
        return importlib.import_module(str(module_name or ""))

    def snapshot(self) -> dict[str, Any]:
        provider = self._manager.snapshot_provider
        if callable(provider):
            result = provider() or {}
            return dict(result) if isinstance(result, Mapping) else {}
        return {}


class PluginContext:
    """Small SDK object passed to in-process plugins."""

    def __init__(self, manager: "PluginManager", record: PluginRecord) -> None:
        self._manager = manager
        self._record = record
        self.engine = EngineAccess(manager, record)
        #: Declarative UI builder (see :mod:`act_platform.ui_spec`).
        self.ui = UI

    @property
    def plugin_id(self) -> str:
        return self._record.plugin_id

    @property
    def event_bus(self) -> EventBus:
        return self._manager.event_bus

    @property
    def owner(self) -> Any:
        return self.engine.owner

    def get_engine(self, name: str, default: Any = None) -> Any:
        return self.engine.get(name, default)

    def require_engine(self, name: str) -> Any:
        return self.engine.require(name)

    def call_engine(self, engine_name: str, method: str, *args: Any, **kwargs: Any) -> Any:
        return self.engine.call(engine_name, method, *args, **kwargs)

    def call_runtime(self, action: str, *args: Any, **kwargs: Any) -> Any:
        return self.engine.runtime(action, *args, **kwargs)

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

    def register_formatter(self, formatter_id: str,
                           metadata: Optional[Mapping[str, Any]] = None,
                           handler: Optional[Callable[..., Any]] = None) -> dict[str, Any]:
        return self._manager._register_extension("formatters", self._record.plugin_id, formatter_id, metadata, handler)

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

    # ── UI panels / render hooks / overlays ───────────────────────────────
    def register_ui_panel(self, panel_id: str,
                          metadata: Optional[Mapping[str, Any]] = None,
                          render: Optional[Callable[..., Any]] = None,
                          on_action: Optional[Callable[..., Any]] = None) -> dict[str, Any]:
        """Register a redrawable plugin UI panel.

        ``render(payload) -> ui_spec`` is called whenever the host needs the
        panel's content; ``on_action(action_id, payload) -> result`` handles
        button presses from the rendered spec.  Call :meth:`request_redraw` to
        ask the host to re-fetch and repaint.
        """
        return self._manager._register_ui_panel(
            self._record.plugin_id, panel_id, metadata, render, on_action)

    def register_render_hook(self, surface: str, callback: Callable[[str, dict], Any],
                             priority: float = 0.0) -> str:
        """Intercept a surface's render payload before the host draws it.

        ``callback(surface, payload) -> payload | None`` may mutate the payload,
        return a replacement, or take the surface over entirely by setting
        ``payload[OVERRIDE_KEY] = ctx.ui.panel(...)``.
        """
        return self._manager._register_render_hook(
            self._record.plugin_id, surface, callback, priority)

    def set_overlay(self, surface: str, spec: Any) -> dict[str, Any]:
        """Draw a declarative spec in ``surface``'s plugin overlay layer."""
        return self._manager._set_overlay(self._record.plugin_id, surface, spec)

    def clear_overlay(self, surface: Optional[str] = None) -> None:
        self._manager._clear_overlay(self._record.plugin_id, surface)

    def register_hotkey(self, hotkey_id: str, callback: Callable[[], Any],
                        default_key: str = "", label: str = "") -> str:
        """Register a customizable global hotkey for this plugin.

        Returns the action id ``plugin.<plugin_id>.<hotkey_id>``. The default key
        (e.g. ``"F6"``) seeds the shared hotkey settings; the user can rebind it
        in the normal keybinding editor. ``callback()`` fires when pressed.
        """
        return self._manager._register_hotkey(
            self._record.plugin_id, hotkey_id, callback, default_key, label)

    def request_redraw(self, surface: str = "", reason: str = "") -> dict[str, Any]:
        """Ask all host surfaces (or one) to repaint plugin content."""
        return self.emit("plugin_ui_invalidate", {
            "plugin_id": self._record.plugin_id,
            "surface": str(surface or ""),
            "reason": str(reason or ""),
        })

    # ── schedulers / loops (timing primitives for heavy plugins) ──────────
    def set_interval(self, callback: Callable[[], Any], seconds: float) -> str:
        """Call ``callback`` every ``seconds`` on a daemon thread until cleared."""
        return self._manager._add_timer(self._record.plugin_id, callback, seconds, True)

    def set_timeout(self, callback: Callable[[], Any], seconds: float) -> str:
        """Call ``callback`` once after ``seconds`` on a daemon thread."""
        return self._manager._add_timer(self._record.plugin_id, callback, seconds, False)

    def clear_timer(self, token: str) -> bool:
        return self._manager._clear_timer(self._record.plugin_id, str(token or ""))

    def run_on_ui(self, callback: Callable[[], Any]) -> None:
        """Marshal ``callback`` onto the host UI thread (Tk root.after if present)."""
        owner = self.owner
        root = getattr(owner, "root", None)
        after = getattr(root, "after", None)
        if callable(after):
            try:
                after(0, callback)
                return
            except Exception:
                pass
        try:
            callback()
        except Exception as exc:
            self._manager._record_failure(self._record.plugin_id, exc)

    # ── owner-agnostic notifications (Entity + WebView alert bridge) ───────
    def notify(self, title: str, message: str, duration_s: float = 60.0,
               kind: str = "plugin") -> bool:
        """Show a persistent SAO alert via whichever alert API the host owns.

        Works on both the Entity (``_show_entity_alert``) and WebView
        (``_show_identity_alert_window``) owners, marshaled to the UI thread.
        """
        owner = self.owner
        if owner is None:
            return False
        entity = getattr(owner, "_show_entity_alert", None)
        if callable(entity):
            self.run_on_ui(lambda: entity(str(title), str(message), display_time=float(duration_s)))
            return True
        webview = getattr(owner, "_show_identity_alert_window", None)
        if callable(webview):
            try:
                webview(str(title), str(message), duration_ms=int(float(duration_s) * 1000),
                        alert_kind=str(kind or "plugin"))
                return True
            except Exception as exc:
                self._manager._record_failure(self._record.plugin_id, exc)
        return False

    def dismiss_notify(self) -> bool:
        """Dismiss a persistent WebView alert (Entity alerts auto-expire)."""
        owner = self.owner
        fn = getattr(owner, "_hide_identity_alert_window", None)
        if callable(fn):
            def _do():
                # Must pass force=True to bypass the persistent-alert guard; a
                # non-forcing call would leave the alert stuck. If the host's
                # signature differs, try a positional force before giving up
                # (never fall back to a non-forcing call, which sticks the alert).
                try:
                    fn(force=True)
                except TypeError:
                    fn(None, True)
            try:
                self.run_on_ui(_do)
                return True
            except Exception:
                return False
        return False

    def toast(self, message: str) -> bool:
        """Best-effort transient toast (WebView menu toast, else an entity alert)."""
        owner = self.owner
        eval_menu = getattr(owner, "_eval_menu", None)
        if callable(eval_menu):
            try:
                safe = json.dumps(str(message))
                eval_menu(f'window.SAO&&SAO.showToast&&SAO.showToast({safe})')
                return True
            except Exception:
                pass
        return self.notify("PLUGIN", str(message), duration_s=3.0)

    def load_local(self, relative_path: str) -> ModuleType:
        """Load a Python module bundled inside this plugin's own directory.

        Lets a plugin ship its own engine/helper ``.py`` files alongside
        ``plugin.py`` (sandboxed to the plugin dir, loaded under a unique module
        name).  The loaded module still imports main-program packages
        (``cv2``, ``config``, ``utils.*`` …) from the shared process normally.
        """
        base = os.path.abspath(self._record.path)
        target = os.path.abspath(os.path.join(base, str(relative_path or "")))
        if os.path.commonpath([base, target]) != base:
            raise ValueError("load_local path must stay inside the plugin directory")
        if not os.path.isfile(target):
            raise FileNotFoundError(relative_path)
        stem = os.path.splitext(os.path.basename(target))[0]
        mod_name = f"act_plugin_{self._record.plugin_id.replace('-', '_').replace('.', '_')}__{stem}"
        spec = importlib.util.spec_from_file_location(mod_name, target)
        if spec is None or spec.loader is None:
            raise ImportError(f"cannot load local module: {relative_path}")
        module = importlib.util.module_from_spec(spec)
        sys.modules[mod_name] = module
        # Tracked so unload removes it from sys.modules (no leak across reloads).
        if mod_name not in self._record.loaded_local_modules:
            self._record.loaded_local_modules.append(mod_name)
        spec.loader.exec_module(module)
        return module


class _PluginTimer:
    """A cancellable one-shot or repeating daemon timer owned by a plugin.

    Callbacks run on a background daemon thread; exceptions are isolated and
    recorded against the plugin.  All of a plugin's timers are cancelled when it
    unloads, so a plugin cannot leak a thread after being disabled.
    """

    __slots__ = ("_manager", "plugin_id", "token", "_fn", "_seconds", "_repeat",
                 "_cancelled", "_timer")

    def __init__(self, manager: "PluginManager", plugin_id: str, token: str,
                 fn: Callable[[], Any], seconds: float, repeat: bool) -> None:
        self._manager = manager
        self.plugin_id = plugin_id
        self.token = token
        self._fn = fn
        self._seconds = max(0.02, float(seconds or 0.0))
        self._repeat = bool(repeat)
        self._cancelled = False
        self._timer: Optional[threading.Timer] = None

    def start(self) -> "_PluginTimer":
        # Armed once at creation, before the timer is shared — no lock needed.
        self._arm()
        return self

    def _arm(self) -> None:
        if self._cancelled:
            return
        self._timer = threading.Timer(self._seconds, self._fire)
        self._timer.daemon = True
        self._timer.start()

    def _fire(self) -> None:
        if self._cancelled:
            return
        try:
            self._fn()
        except Exception as exc:  # noqa: BLE001 - isolation is intentional
            self._manager._record_failure(self.plugin_id, exc)
        # Decide rearm under the shared lock so a concurrent cancel() (from
        # unload) cannot slip between the _cancelled check and re-arming, which
        # would otherwise orphan a daemon timer that survives unload.
        forget = False
        with self._manager._timers_lock:
            if self._cancelled:
                pass
            elif self._repeat:
                self._arm()
            else:
                forget = True
        if forget:
            self._manager._forget_timer(self.plugin_id, self.token)

    def cancel(self) -> None:
        with self._manager._timers_lock:
            self._cancelled = True
            timer = self._timer
        if timer is not None:
            try:
                timer.cancel()
            except Exception:
                pass


class PluginManager:
    """Discover and manage in-process Python ACT plugins."""

    def __init__(self, plugin_dirs: Optional[Iterable[str]] = None,
                 event_bus: Optional[EventBus] = None,
                 snapshot_provider: Optional[Callable[[], Mapping[str, Any]]] = None,
                 owner_provider: Optional[Callable[[], Any]] = None,
                 settings: Any = None,
                 max_failures: int = 3) -> None:
        self.plugin_dirs = [os.path.abspath(path) for path in (plugin_dirs or []) if path]
        self.event_bus = event_bus or EventBus()
        self.snapshot_provider = snapshot_provider
        self.owner_provider = owner_provider
        self.settings = settings
        self.max_failures = max(1, int(max_failures or 3))
        self._records: Dict[str, PluginRecord] = {}
        self._settings_cache: Dict[str, Dict[str, Any]] = {}
        self._extensions: Dict[str, Dict[str, dict[str, Any]]] = {kind: {} for kind in EXTENSION_KINDS}
        self._extension_handlers: Dict[tuple[str, str], Callable[..., Any]] = {}
        #: panel_id -> on_action callable (UI panel button dispatch).
        self._ui_action_handlers: Dict[str, Callable[..., Any]] = {}
        #: Shared render-hook + overlay registry for every UI surface.
        self.render_registry = RenderHookRegistry()
        #: plugin_id -> {token: _PluginTimer} for plugin schedulers/loops.
        self._timers: Dict[str, Dict[str, "_PluginTimer"]] = {}
        self._timer_seq = 0
        #: Guards all _timers mutations + the timer rearm/cancel decision so a
        #: repeating timer cannot rearm itself past a concurrent unload.
        self._timers_lock = threading.RLock()
        #: action ("plugin.<id>.<hotkey>") -> {plugin_id, hotkey_id, callback, default_key, label}
        self._hotkeys: Dict[str, dict[str, Any]] = {}
        #: Guards _hotkeys — registered/cleared on the main thread, read from the
        #: pynput hotkey-listener thread (snapshot under lock to avoid races).
        self._hotkeys_lock = threading.RLock()
        #: pinned plugin ids fallback when no settings object is attached.
        self._pinned_cache: list[str] = []
        #: True once load_all() has run — lets ensure_act_plugin_manager(load=True)
        #: be idempotent instead of reloading every plugin on every call.
        self._initial_loaded = False

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
        self._initial_loaded = True
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
        pinned = set(self.pinned_plugins())
        hotkeys_by_plugin: dict[str, int] = {}
        with self._hotkeys_lock:
            metas = list(self._hotkeys.values())
        for meta in metas:
            pid = str(meta.get("plugin_id") or "")
            hotkeys_by_plugin[pid] = hotkeys_by_plugin.get(pid, 0) + 1
        out: list[dict[str, Any]] = []
        for key in sorted(self._records):
            status = self._records[key].to_status()
            status["pinned"] = key in pinned
            status["hotkey_count"] = hotkeys_by_plugin.get(key, 0)
            out.append(status)
        return out

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

    # ── UI panels ─────────────────────────────────────────────────────────
    def _register_ui_panel(self, plugin_id: str, panel_id: str,
                           metadata: Optional[Mapping[str, Any]] = None,
                           render: Optional[Callable[..., Any]] = None,
                           on_action: Optional[Callable[..., Any]] = None) -> dict[str, Any]:
        normalized = self._register_extension("ui_panels", plugin_id, panel_id, metadata, render)
        if callable(on_action):
            self._ui_action_handlers[normalized["id"]] = on_action
        else:
            self._ui_action_handlers.pop(normalized["id"], None)
        return normalized

    def list_ui_panels(self) -> list[dict[str, Any]]:
        out: list[dict[str, Any]] = []
        for meta in self.list_extensions("ui_panels"):
            item = dict(meta)
            plugin_id = str(item.get("plugin_id") or "")
            record = self._records.get(plugin_id)
            item["available"] = bool(record and record.loaded and record.active and record.enabled)
            item["has_actions"] = item["id"] in self._ui_action_handlers
            out.append(item)
        return out

    def render_ui_panel(self, panel_id: str, payload: Optional[Mapping[str, Any]] = None, *,
                        time_budget_ms: float = 60.0) -> dict[str, Any]:
        invoked = self.invoke_extension("ui_panels", panel_id, payload, time_budget_ms=time_budget_ms)
        if not invoked.get("ok"):
            return {"ok": False, "panel_id": str(panel_id or ""),
                    "message": invoked.get("message") or "render failed",
                    "spec": {"version": 1, "title": "", "nodes": []},
                    "errors": invoked.get("errors") or [invoked.get("message") or "render failed"]}
        spec = normalize_ui_spec(invoked.get("result"))
        return {
            "ok": True,
            "panel_id": str(panel_id or ""),
            "plugin_id": invoked.get("plugin_id"),
            "elapsed_ms": invoked.get("elapsed_ms"),
            "spec": spec,
            "errors": [],
        }

    def invoke_ui_action(self, panel_id: str, action_id: str,
                         payload: Optional[Mapping[str, Any]] = None, *,
                         time_budget_ms: float = 800.0) -> dict[str, Any]:
        panel_id = str(panel_id or "")
        meta = self._extensions.get("ui_panels", {}).get(panel_id)
        if not isinstance(meta, Mapping):
            return {"ok": False, "message": f"ui panel not found: {panel_id}", "errors": ["panel not found"]}
        plugin_id = str(meta.get("plugin_id") or "")
        record = self._records.get(plugin_id)
        if record is None or not record.loaded or not record.active or not record.enabled:
            return {"ok": False, "plugin_id": plugin_id, "message": "plugin is not active", "errors": ["plugin is not active"]}
        handler = self._ui_action_handlers.get(panel_id)
        if not callable(handler):
            return {"ok": False, "plugin_id": plugin_id, "message": "panel has no action handler", "errors": ["no action handler"]}
        started = time.perf_counter()
        try:
            result = handler(str(action_id or ""), copy.deepcopy(dict(payload or {})))
        except Exception as exc:
            self._record_failure(plugin_id, exc)
            return {"ok": False, "plugin_id": plugin_id, "panel_id": panel_id,
                    "action_id": str(action_id or ""), "message": str(exc), "errors": [str(exc)]}
        elapsed_ms = (time.perf_counter() - started) * 1000.0
        budget = float(time_budget_ms or 0.0)
        if budget > 0 and elapsed_ms > budget:
            exc = TimeoutError(f"ui action exceeded budget: {elapsed_ms:.1f}ms > {budget:.1f}ms")
            self._record_failure(plugin_id, exc)
            return {
                "ok": False,
                "plugin_id": plugin_id,
                "panel_id": panel_id,
                "action_id": str(action_id or ""),
                "elapsed_ms": elapsed_ms,
                "time_budget_ms": budget,
                "timed_out": True,
                "message": str(exc),
                "errors": [str(exc)],
            }
        out: dict[str, Any] = {
            "ok": True,
            "plugin_id": plugin_id,
            "panel_id": panel_id,
            "action_id": str(action_id or ""),
            "elapsed_ms": elapsed_ms,
            "time_budget_ms": budget,
            "result": _json_safe(result),
            "errors": [],
        }
        # An action handler may return a fresh spec to repaint immediately.
        if isinstance(result, Mapping) and ("nodes" in result or "children" in result or result.get("type")):
            out["spec"] = normalize_ui_spec(result)
        return out

    # ── render hooks + overlays ────────────────────────────────────────────
    def _register_render_hook(self, plugin_id: str, surface: str,
                              callback: Callable[[str, dict], Any], priority: float = 0.0) -> str:
        return self.render_registry.register_hook(plugin_id, surface, callback, priority)

    def _set_overlay(self, plugin_id: str, surface: str, spec: Any) -> dict[str, Any]:
        return self.render_registry.set_overlay(plugin_id, surface, spec)

    def _clear_overlay(self, plugin_id: str, surface: Optional[str] = None) -> None:
        self.render_registry.clear_overlay(plugin_id, surface)

    # ── plugin schedulers / loops ──────────────────────────────────────────
    def _add_timer(self, plugin_id: str, fn: Callable[[], Any], seconds: float,
                   repeat: bool) -> str:
        if not callable(fn):
            raise TypeError("timer callback must be callable")
        with self._timers_lock:
            self._timer_seq += 1
            token = f"timer_{self._timer_seq}"
            timer = _PluginTimer(self, plugin_id, token, fn, seconds, repeat)
            self._timers.setdefault(plugin_id, {})[token] = timer
        timer.start()  # arm after registration, outside the lock
        return token

    def _clear_timer(self, plugin_id: str, token: str) -> bool:
        with self._timers_lock:
            timer = (self._timers.get(plugin_id) or {}).pop(token, None)
        if timer is not None:
            timer.cancel()
            return True
        return False

    def _forget_timer(self, plugin_id: str, token: str) -> None:
        with self._timers_lock:
            bucket = self._timers.get(plugin_id)
            if bucket is not None:
                bucket.pop(token, None)

    def _clear_plugin_timers(self, plugin_id: str) -> None:
        with self._timers_lock:
            bucket = self._timers.pop(plugin_id, {}) or {}
        for timer in list(bucket.values()):
            timer.cancel()

    # ── plugin hotkeys (customizable) ──────────────────────────────────────
    def _register_hotkey(self, plugin_id: str, hotkey_id: str,
                         callback: Callable[[], Any], default_key: str = "",
                         label: str = "") -> str:
        if not callable(callback):
            raise TypeError("hotkey callback must be callable")
        hid = _safe_id(hotkey_id)
        if not hid:
            raise ValueError(f"invalid hotkey id: {hotkey_id!r}")
        action = f"plugin.{plugin_id}.{hid}"
        with self._hotkeys_lock:
            self._hotkeys[action] = {
                "action": action,
                "plugin_id": str(plugin_id or ""),
                "hotkey_id": hid,
                "callback": callback,
                "default_key": str(default_key or "").upper(),
                "label": str(label or hid),
            }
        return action

    def list_hotkeys(self) -> list[dict[str, Any]]:
        out: list[dict[str, Any]] = []
        # Snapshot under the lock: read from the pynput listener thread while
        # load/unload mutates _hotkeys on the main thread.
        with self._hotkeys_lock:
            items = sorted(self._hotkeys.items())
        for action, meta in items:
            record = self._records.get(str(meta.get("plugin_id") or ""))
            out.append({
                "action": action,
                "plugin_id": meta.get("plugin_id"),
                "hotkey_id": meta.get("hotkey_id"),
                "label": meta.get("label"),
                "default_key": meta.get("default_key"),
                "current_key": self._current_hotkey(action, str(meta.get("default_key") or "")),
                "active": bool(record and record.active and record.enabled),
            })
        return out

    def _current_hotkey(self, action: str, default_key: str) -> str:
        if self.settings is not None and hasattr(self.settings, "get"):
            raw = self.settings.get("hotkeys", {}) or {}
            if isinstance(raw, Mapping) and action in raw:
                value = raw.get(action)
                if isinstance(value, Mapping):
                    return str(value.get("key") or value.get("name") or default_key)
                return str(value or default_key)
        return default_key

    def hotkey_actions(self) -> dict[str, Callable[[], Any]]:
        """{action: dispatch} for the host hotkey manager (active plugins only)."""
        out: dict[str, Callable[[], Any]] = {}
        with self._hotkeys_lock:
            items = list(self._hotkeys.items())
        for action, meta in items:
            record = self._records.get(str(meta.get("plugin_id") or ""))
            if record and record.active and record.enabled:
                out[action] = lambda a=action: self.dispatch_hotkey(a)
        return out

    def dispatch_hotkey(self, action: str) -> bool:
        with self._hotkeys_lock:
            meta = self._hotkeys.get(str(action or ""))
        if not meta:
            return False
        record = self._records.get(str(meta.get("plugin_id") or ""))
        if not (record and record.active and record.enabled):
            return False
        callback = meta.get("callback")
        if not callable(callback):
            return False
        try:
            # Callback runs OUTSIDE the lock (it may re-enter the manager).
            callback()
            return True
        except Exception as exc:
            self._record_failure(str(meta.get("plugin_id") or ""), exc)
            return False

    def _clear_plugin_hotkeys(self, plugin_id: str) -> None:
        with self._hotkeys_lock:
            stale = [a for a, m in self._hotkeys.items() if str(m.get("plugin_id") or "") == str(plugin_id or "")]
            for action in stale:
                self._hotkeys.pop(action, None)

    def set_hotkey(self, action: str, key: str) -> bool:
        """Rebind a plugin hotkey through the shared ``settings['hotkeys']``.

        Only registered plugin hotkeys may be rebound (so a plugin panel cannot
        clobber a built-in binding). An empty / 'default' key clears the override
        so the plugin's declared default applies again. The key is stored as a
        plain ``"F8"`` string, which ``SAOHotkeyManager`` resolves to a VK — the
        same mechanism the built-in hotkeys use, avoiding a separate conflict set.
        """
        # Hotkey actions are stored lowercase (_safe_id); normalize callers'
        # input so a mixed-case action id still matches.
        action = str(action or "").lower()
        with self._hotkeys_lock:
            if action not in self._hotkeys:
                return False
        if self.settings is None or not (hasattr(self.settings, "get") and hasattr(self.settings, "set")):
            return False
        raw = self.settings.get("hotkeys", {}) or {}
        raw = dict(raw) if isinstance(raw, dict) else {}
        key_norm = str(key or "").strip().upper()
        if not key_norm or key_norm in ("DEFAULT", "(DEFAULT)", "默认", "NONE", "无"):
            raw.pop(action, None)
        else:
            raw[action] = key_norm
        self.settings.set("hotkeys", raw)
        save = getattr(self.settings, "save", None)
        if callable(save):
            try:
                save()
            except Exception:
                pass
        return True

    # ── pinned plugins (promoted to the top of the plugin menu) ────────────
    def pinned_plugins(self) -> list[str]:
        if self.settings is not None and hasattr(self.settings, "get"):
            raw = self.settings.get("act_pinned_plugins", []) or []
            if isinstance(raw, (list, tuple)):
                return [str(x) for x in raw]
        return list(self._pinned_cache)

    def set_pinned(self, plugin_id: str, pinned: bool = True) -> list[str]:
        current = list(self.pinned_plugins())
        pid = str(plugin_id or "")
        if pinned and pid not in current:
            current.append(pid)
        elif not pinned and pid in current:
            current = [x for x in current if x != pid]
        if self.settings is not None and hasattr(self.settings, "set"):
            self.settings.set("act_pinned_plugins", current)
            save = getattr(self.settings, "save", None)
            if callable(save):
                try:
                    save()
                except Exception:
                    pass
        else:
            self._pinned_cache = current
        return current

    def apply_render_hooks(self, surface: str, payload: Optional[Mapping[str, Any]] = None) -> dict[str, Any]:
        def _on_error(plugin_id: str, exc: BaseException) -> None:
            self._record_failure(plugin_id, exc)
        return self.render_registry.apply(surface, dict(payload or {}), on_error=_on_error)

    def surface_overlays(self, surface: str) -> list[dict[str, Any]]:
        return self.render_registry.overlays(surface)

    def render_status(self) -> dict[str, Any]:
        return self.render_registry.status()

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
            "engine_access": self.engine_status(),
            "event_bus": self.event_bus.snapshot(),
            "ui_panels": self.list_ui_panels(),
            "render": self.render_registry.status(),
            "hotkeys": self.list_hotkeys(),
            "pinned": self.pinned_plugins(),
        }

    def engine_status(self) -> dict[str, Any]:
        access = EngineAccess(self)
        return {
            "trusted_in_process": True,
            "owner_available": access.owner is not None,
            "available": access.available(),
            "handles": access.handles(),
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
                if kind == "ui_panels":
                    self._ui_action_handlers.pop(ext_id, None)
        # Drop every render hook + overlay this plugin registered.
        self.render_registry.unregister_plugin(str(plugin_id or ""))
        # Cancel any scheduler/loop timers the plugin started.
        self._clear_plugin_timers(str(plugin_id or ""))
        # Drop the plugin's registered hotkeys.
        self._clear_plugin_hotkeys(str(plugin_id or ""))
        if record is not None:
            # Remove modules the plugin loaded via ctx.load_local (no sys.modules leak).
            for mod_name in list(record.loaded_local_modules):
                sys.modules.pop(mod_name, None)
            record.loaded_local_modules.clear()
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


__all__ = ["EngineAccess", "PluginContext", "PluginManager", "PluginRecord"]
