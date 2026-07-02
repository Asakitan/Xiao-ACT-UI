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
    "menu_categories",
    "data_sources",
)
# Render hooks are NOT extension-registry entries — they live in
# ``PluginManager.render_registry`` and surface via ``render_status()``.


_RUNTIME_ACTION_ALIASES: dict[str, str] = {
    "plugin_status": "act_plugin_status",
    "plugin_list": "act_plugin_list",
    "plugin_import": "act_plugin_import",
    "plugin_import_dialog": "act_plugin_import_dialog",
    "plugin_uninstall": "act_plugin_uninstall",
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
    "plugin_action": "act_plugin_action",
    "menu_surfaces": "act_plugin_menu_surfaces",
    "render_surfaces": "render_surfaces",
    "render_apply_hooks": "render_apply_hooks",
    "render_overlays": "render_overlays",
}


_MISSING = object()


def _safe_exception_text(exc: BaseException) -> str:
    try:
        text = "".join(traceback.format_exception_only(type(exc), exc)).strip()
        if text:
            return text
    except Exception:
        pass
    name = type(exc).__name__
    try:
        message = str(exc)
    except Exception:
        message = ""
    return f"{name}: {message}" if message else name


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


def _normalize_locale(value: Any) -> str:
    if value is _MISSING:
        return ""
    text = str(value or "").strip().replace("_", "-")
    if not text:
        return ""
    if text.lower() in {"c", "c.utf-8", "posix"}:
        return ""
    parts = [part for part in text.split("-") if part]
    if not parts:
        return ""
    out = [parts[0].lower()]
    for part in parts[1:]:
        if len(part) == 2 and part.isalpha():
            out.append(part.upper())
        elif len(part) == 4 and part.isalpha():
            out.append(part.title())
        else:
            out.append(part)
    return "-".join(out)


def _normalize_manifest_locales(value: Any) -> dict[str, Any]:
    if not isinstance(value, Mapping):
        return {}
    out: dict[str, Any] = {}
    for raw_locale, table in value.items():
        locale = _normalize_locale(raw_locale)
        if not locale or not isinstance(table, Mapping):
            continue
        safe = _json_safe(table)
        if isinstance(safe, Mapping):
            out[locale] = dict(safe)
    return out


def _locale_overlay(locales: Mapping[str, Any], locale: Any) -> dict[str, Any]:
    requested = _normalize_locale(locale)
    if not requested or not isinstance(locales, Mapping):
        return {}
    language = requested.split("-", 1)[0]
    candidates: list[str] = []
    if language and language != requested:
        candidates.append(language)
    if language == "en" and requested != "en-US" and "en-US" not in candidates:
        candidates.append("en-US")
    if language == "zh" and requested != "zh-CN" and "zh-CN" not in candidates:
        candidates.append("zh-CN")
    candidates.append(requested)
    overlay: dict[str, Any] = {}
    for candidate in candidates:
        section = locales.get(candidate)
        if isinstance(section, Mapping):
            overlay = _merge_localized(overlay, section)
    return overlay


def _merge_localized(base: Any, override: Any) -> Any:
    if override is None:
        return copy.deepcopy(base)
    if isinstance(base, Mapping) and isinstance(override, Mapping):
        out = {str(k): copy.deepcopy(v) for k, v in base.items()}
        for key, value in override.items():
            text_key = str(key)
            out[text_key] = _merge_localized(out.get(text_key), value)
        return out
    if isinstance(base, list) and isinstance(override, Mapping):
        return _merge_list_by_id(base, override)
    if isinstance(base, tuple) and isinstance(override, Mapping):
        return tuple(_merge_list_by_id(list(base), override))
    if isinstance(base, (list, tuple)) and isinstance(override, (list, tuple)):
        return _merge_list_by_id(list(base), override)
    return copy.deepcopy(override)


def _merge_list_by_id(base: list[Any], override: Any) -> list[Any]:
    out = [copy.deepcopy(item) for item in base]
    if isinstance(override, Mapping):
        override_items = [
            dict(value, id=key) if isinstance(value, Mapping) and "id" not in value else value
            for key, value in override.items()
        ]
    elif isinstance(override, (list, tuple)):
        override_items = list(override)
    else:
        return copy.deepcopy(override)

    index_by_id = {
        str(item.get("id") or ""): idx
        for idx, item in enumerate(out)
        if isinstance(item, Mapping) and item.get("id")
    }
    for item in override_items:
        if not isinstance(item, Mapping):
            out.append(copy.deepcopy(item))
            continue
        item_id = str(item.get("id") or "")
        if item_id and item_id in index_by_id:
            idx = index_by_id[item_id]
            out[idx] = _merge_localized(out[idx], item)
        else:
            out.append(copy.deepcopy(item))
            if item_id:
                index_by_id[item_id] = len(out) - 1
    return out


def _localized_value(locales: Mapping[str, Any], locale: Any,
                     key: str, fallback: Any = "") -> Any:
    overlay = _locale_overlay(locales, locale)
    if key in overlay:
        return copy.deepcopy(overlay.get(key))
    return copy.deepcopy(fallback)


_LOCALIZED_MENU_TEXT_KEYS = {
    "name",
    "category",
    "title",
    "label",
    "script_label",
    "toggle_label",
    "enable_label",
    "disable_label",
}
_LOCALIZED_ACTION_TEXT_KEYS = {"label", "title", "description", "tooltip"}
_LOCALIZED_CAPABILITY_TEXT_KEYS = {"label", "title", "description", "display_name", "render_hint"}
_LOCALIZED_SETTING_TEXT_KEYS = {
    "label",
    "title",
    "description",
    "help",
    "tooltip",
    "placeholder",
    "unit",
    "prefix",
    "suffix",
}
_LOCALIZED_SETTING_OPTION_TEXT_KEYS = {
    "label",
    "title",
    "description",
    "help",
    "tooltip",
}
_SETTING_OPTION_LIST_KEYS = ("options", "choices")
_SETTING_OPTION_LABEL_MAP_KEYS = ("enum_labels", "value_labels")


def _localized_sao_menu(base: Mapping[str, Any], override: Any) -> dict[str, Any]:
    out = copy.deepcopy(dict(base or {}))
    if not isinstance(override, Mapping):
        return out
    for key in _LOCALIZED_MENU_TEXT_KEYS:
        if key in override:
            out[key] = copy.deepcopy(override.get(key))
    if isinstance(out.get("actions"), list) and isinstance(override.get("actions"), (Mapping, list, tuple)):
        out["actions"] = _localized_actions(out.get("actions") or [], override.get("actions"))
    return out


def _localized_actions(base: list[Any], override: Any) -> list[Any]:
    out = [copy.deepcopy(item) for item in base]
    if isinstance(override, Mapping):
        override_items = [
            dict(value, id=key) if isinstance(value, Mapping) and "id" not in value else value
            for key, value in override.items()
        ]
    elif isinstance(override, (list, tuple)):
        override_items = list(override)
    else:
        return out
    index_by_id = {
        str(item.get("id") or ""): idx
        for idx, item in enumerate(out)
        if isinstance(item, Mapping) and item.get("id")
    }
    for item in override_items:
        if not isinstance(item, Mapping):
            continue
        item_id = str(item.get("id") or "")
        if not item_id or item_id not in index_by_id:
            continue
        target = out[index_by_id[item_id]]
        if not isinstance(target, Mapping):
            continue
        merged = dict(target)
        for key in _LOCALIZED_ACTION_TEXT_KEYS:
            if key in item:
                merged[key] = copy.deepcopy(item.get(key))
        out[index_by_id[item_id]] = merged
    return out


def _localized_capabilities(base: tuple[dict[str, Any], ...], override: Any) -> tuple[dict[str, Any], ...]:
    out = [copy.deepcopy(dict(item)) for item in base if isinstance(item, Mapping)]
    if isinstance(override, Mapping):
        override_items = [
            dict(value, id=key) if isinstance(value, Mapping) and "id" not in value else value
            for key, value in override.items()
        ]
    elif isinstance(override, (list, tuple)):
        override_items = list(override)
    else:
        return tuple(out)
    index_by_id = {
        str(item.get("id") or ""): idx
        for idx, item in enumerate(out)
        if item.get("id")
    }
    for item in override_items:
        if not isinstance(item, Mapping):
            continue
        item_id = str(item.get("id") or "")
        if not item_id or item_id not in index_by_id:
            continue
        merged = dict(out[index_by_id[item_id]])
        for key in _LOCALIZED_CAPABILITY_TEXT_KEYS:
            if key in item:
                merged[key] = copy.deepcopy(item.get(key))
        out[index_by_id[item_id]] = merged
    return tuple(out)


def _localized_settings_schema(base: Mapping[str, Any], override: Any) -> dict[str, Any]:
    out = copy.deepcopy(dict(base or {}))
    if not isinstance(override, Mapping):
        return out
    for setting_key, setting_override in override.items():
        if not isinstance(setting_override, Mapping):
            continue
        target = out.get(str(setting_key))
        if not isinstance(target, Mapping):
            continue
        merged = dict(target)
        for key in _LOCALIZED_SETTING_TEXT_KEYS:
            if key in setting_override:
                merged[key] = copy.deepcopy(setting_override.get(key))
        _localized_setting_options(merged, setting_override)
        out[str(setting_key)] = merged
    return out


def _localized_setting_options(target: dict[str, Any], override: Mapping[str, Any]) -> None:
    for key in _SETTING_OPTION_LIST_KEYS:
        if key in target and key in override:
            target[key] = _localized_option_list(target.get(key), override.get(key))
    for key in _SETTING_OPTION_LABEL_MAP_KEYS:
        _localized_label_map(target, override, key)


def _localized_option_list(base: Any, override: Any) -> Any:
    if not isinstance(base, list):
        return copy.deepcopy(base)
    out = [copy.deepcopy(item) for item in base]
    if isinstance(override, Mapping):
        override_items = []
        for key, value in override.items():
            if isinstance(value, Mapping):
                item = dict(value)
                item["_locale_target_id"] = str(key)
                override_items.append(item)
            else:
                override_items.append(value)
    elif isinstance(override, (list, tuple)):
        override_items = list(override)
    else:
        return out
    index_by_id: dict[str, int] = {}
    for idx, item in enumerate(out):
        if not isinstance(item, Mapping):
            continue
        option_id = _setting_option_id(item)
        if option_id:
            index_by_id[option_id] = idx
    for item in override_items:
        if not isinstance(item, Mapping):
            continue
        option_id = _setting_option_id(item)
        if not option_id or option_id not in index_by_id:
            continue
        target = out[index_by_id[option_id]]
        if not isinstance(target, Mapping):
            continue
        merged = dict(target)
        for key in _LOCALIZED_SETTING_OPTION_TEXT_KEYS:
            if key in item:
                merged[key] = copy.deepcopy(item.get(key))
        out[index_by_id[option_id]] = merged
    return out


def _setting_option_id(item: Mapping[str, Any]) -> str:
    for key in ("_locale_target_id", "id", "value"):
        if key in item:
            return str(item.get(key) or "")
    return ""


def _localized_label_map(target: dict[str, Any], override: Mapping[str, Any], key: str) -> None:
    raw = override.get(key)
    if not isinstance(raw, Mapping):
        return
    current = target.get(key)
    label_map = dict(current) if isinstance(current, Mapping) else {}
    allowed = _setting_value_ids(target)
    for label_key, label_value in raw.items():
        text_key = str(label_key or "")
        if not text_key:
            continue
        if allowed and text_key not in allowed and text_key not in label_map:
            continue
        label_map[text_key] = copy.deepcopy(label_value)
    if label_map:
        target[key] = label_map


def _setting_value_ids(setting: Mapping[str, Any]) -> set[str]:
    allowed: set[str] = set()
    enum_values = setting.get("enum")
    if isinstance(enum_values, (list, tuple)):
        allowed.update(str(item) for item in enum_values if item is not None)
    for key in _SETTING_OPTION_LIST_KEYS:
        value = setting.get(key)
        if not isinstance(value, list):
            continue
        for item in value:
            if isinstance(item, Mapping):
                option_id = _setting_option_id(item)
                if option_id:
                    allowed.add(option_id)
            elif item is not None:
                allowed.add(str(item))
    return allowed


def _normalize_sao_menu(value: Any) -> dict[str, Any]:
    """Normalize optional manifest-declared SAO menu metadata."""
    if not isinstance(value, Mapping):
        return {}
    safe = _json_safe(value)
    if not isinstance(safe, Mapping):
        return {}
    out: dict[str, Any] = {}
    for key, val in safe.items():
        text_key = str(key or "").strip()
        if not text_key or val is None:
            continue
        out[text_key] = val
    for key in (
        "name",
        "category",
        "title",
        "label",
        "icon",
        "icon_text",
        "row_icon",
        "script_label",
        "toggle_label",
        "enable_label",
        "disable_label",
        "action_id",
        "setting",
        "overlay_setting",
        "surface",
    ):
        if key in out:
            out[key] = str(out.get(key) or "")
    if out.get("icon_text") and not out.get("icon"):
        out["icon"] = str(out.get("icon_text") or "")
    if out.get("icon") and not out.get("icon_text"):
        out["icon_text"] = str(out.get("icon") or "")
    if out.get("category") and not out.get("name"):
        out["name"] = str(out.get("category") or "")
    if not out.get("script_label"):
        label = out.get("toggle_label") or out.get("label") or out.get("name") or out.get("title")
        if label:
            out["script_label"] = str(label)

    def _as_bool(raw: Any, default: bool = False) -> bool:
        if isinstance(raw, bool):
            return raw
        if isinstance(raw, (int, float)):
            return bool(raw)
        if isinstance(raw, str):
            text = raw.strip().lower()
            if text in {"1", "true", "yes", "on", "enabled"}:
                return True
            if text in {"0", "false", "no", "off", "disabled"}:
                return False
        return bool(default)

    for key in ("default_enabled", "keep_menu_open"):
        if key in out:
            out[key] = _as_bool(out.get(key), False)
    if "priority" in out:
        try:
            out["priority"] = float(out.get("priority") or 0.0)
        except Exception:
            out["priority"] = 50.0
    else:
        out["priority"] = 50.0
    return out


def _normalize_requires(value: Any) -> tuple[str, ...]:
    """Normalize manifest requirements into stable string tokens.

    Older built-in plugin manifests use a plain list such as
    ``["star_resonance"]``. Workspace-root script plugins may use a mapping
    with a platform version and runtime features:
    ``{"act_platform": ">=1.0", "runtime_features": ["rgba_frame"]}``.
    Keep both forms discoverable without loading the plugin.
    """
    out: list[str] = []

    def add(raw: Any) -> None:
        text = str(raw or "").strip()
        if text:
            out.append(text)

    if isinstance(value, Mapping):
        for key, raw in value.items():
            name = str(key or "").strip()
            if not name:
                continue
            if name == "runtime_features" and isinstance(raw, (list, tuple, set)):
                for feature in raw:
                    add(f"runtime_feature:{feature}")
            elif raw is True:
                add(name)
            elif raw not in (None, False):
                add(f"{name}{raw}")
    elif isinstance(value, (list, tuple, set)):
        for item in value:
            add(item)
    elif value not in (None, ""):
        add(value)
    return tuple(out)


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
    # Window-size hints for panels opened in their own host window (ui_panels).
    # A plugin may declare these on register_ui_panel; the host uses them and
    # falls back to its default when absent (see PluginContext.open_window).
    for key in ("width", "height", "min_width", "min_height"):
        value = src.get(key)
        if value is not None:
            try:
                normalized[key] = max(0, int(value))
            except Exception:
                pass
    # ``hidden`` panels are registered (renderable + summonable via
    # ctx.open_window) but NOT listed as separate entries — a plugin's
    # sub-panels live under the plugin, summoned by its own primary panel.
    # ``primary`` marks the panel a host opens by default for the whole plugin.
    for key in ("hidden", "primary"):
        if src.get(key) is not None:
            normalized[key] = bool(src.get(key))
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
    description: str = ""
    enabled: bool = False
    #: Scripting language: "python" (default), "lua", "csharp", "angelscript", "emma".
    language: str = "python"
    game_ids: tuple[str, ...] = ()
    requires: tuple[str, ...] = ()
    permissions: tuple[str, ...] = ()
    capabilities: tuple[dict[str, Any], ...] = ()
    settings_schema: Mapping[str, Any] = field(default_factory=dict)
    sao_menu: Mapping[str, Any] = field(default_factory=dict)
    locales: Mapping[str, Any] = field(default_factory=dict)
    module: Optional[ModuleType] = None
    context: Optional["PluginContext"] = None
    loaded: bool = False
    active: bool = False
    #: True once a non-Python ScriptRuntime has begun loading this record.
    script_runtime_active: bool = False
    failures: int = 0
    event_failures: int = 0
    last_error: str = ""
    last_loaded_at: float = 0.0
    subscriptions: list[str] = field(default_factory=list)
    extensions: dict[str, list[str]] = field(default_factory=dict)
    logs: list[str] = field(default_factory=list)
    loaded_local_modules: list[str] = field(default_factory=list)
    #: sys.path entries this plugin added (its own vendor/ + libs/, plus any
    #: ctx.ensure_requirements paths). Restored on unload so deps don't leak.
    added_sys_paths: list[str] = field(default_factory=list)
    #: {dist: 'libs'|'vendor'|'pip→libs'|'site(fallback)'|'missing'} after deps bootstrap.
    deps_summary: dict[str, str] = field(default_factory=dict)

    def localized_name(self, locale: Any = "") -> str:
        value = _localized_value(self.locales, locale, "name", self.name)
        return str(value or self.name)

    def localized_description(self, locale: Any = "") -> str:
        value = _localized_value(self.locales, locale, "description", self.description)
        return str(value or self.description or "")

    def localized_settings_schema(self, locale: Any = "") -> Mapping[str, Any]:
        overlay = _locale_overlay(self.locales, locale)
        return _localized_settings_schema(
            self.settings_schema,
            overlay.get("settings_schema") if isinstance(overlay.get("settings_schema"), Mapping) else None,
        )

    def localized_sao_menu(self, locale: Any = "") -> Mapping[str, Any]:
        overlay = _locale_overlay(self.locales, locale)
        menu = _localized_sao_menu(
            self.sao_menu,
            overlay.get("sao_menu") if isinstance(overlay.get("sao_menu"), Mapping) else None,
        )
        return _normalize_sao_menu(menu)

    def localized_capabilities(self, locale: Any = "") -> tuple[dict[str, Any], ...]:
        overlay = _locale_overlay(self.locales, locale)
        return _localized_capabilities(
            self.capabilities,
            overlay.get("capabilities") if overlay.get("capabilities") is not None else None,
        )

    def to_status(self, locale: Any = "") -> dict[str, Any]:
        locale_id = _normalize_locale(locale)
        name = self.localized_name(locale_id)
        description = self.localized_description(locale_id)
        capabilities = self.localized_capabilities(locale_id)
        sao_menu = self.localized_sao_menu(locale_id)
        settings_schema = self.localized_settings_schema(locale_id)
        return {
            "id": self.plugin_id,
            "name": name,
            "default_name": self.name,
            "description": description,
            "default_description": self.description,
            "version": self.version,
            "path": self.path,
            "entry": self.entry,
            "language": self.language,
            "locale": locale_id,
            "locales": sorted(str(key) for key in self.locales),
            "enabled": self.enabled,
            "loaded": self.loaded,
            "active": self.active,
            "game_ids": list(self.game_ids),
            "requires": list(self.requires),
            "permissions": list(self.permissions),
            "capabilities": [dict(cap) for cap in capabilities],
            "capability_ids": [str(cap.get("id")) for cap in capabilities if cap.get("id")],
            "settings_schema": _json_safe(settings_schema) if isinstance(settings_schema, Mapping) else {},
            "sao_menu": _json_safe(sao_menu) if isinstance(sao_menu, Mapping) else {},
            # A plugin only "has a panel" if it DECLARES one — the manifest must
            # list a ``ui_panels`` capability. Lets the menu/manager know even
            # while the plugin is disabled (and so not yet runtime-registered).
            "declares_panel": any(str(cap.get("id")) == "ui_panels" for cap in capabilities),
            "failures": self.failures,
            "event_failures": self.event_failures,
            "last_error": self.last_error,
            "last_loaded_at": self.last_loaded_at,
            "subscription_count": len(self.subscriptions),
            "extensions": {kind: list(ids) for kind, ids in sorted(self.extensions.items()) if ids},
            "extension_count": sum(len(ids) for ids in self.extensions.values()),
            "deps": dict(self.deps_summary),
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
        for name, handle in (
            ("owner", self.owner),
            ("event_bus", self.event_bus),
            ("plugin_manager", self.plugin_manager),
            ("settings", self.settings),
        ):
            out[name] = {"available": handle is not None, "type": _type_name(handle)}
        for name, engine in self._manager._plugin_engines.items():
            if name not in out:
                out[name] = {
                    "available": engine is not None,
                    "type": _type_name(engine),
                    "source": "plugin",
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
        # Check plugin-contributed engines before falling through to owner attrs.
        plugin_engine = self._manager._plugin_engines.get(key)
        if plugin_engine is not None:
            return plugin_engine
        owner = self.owner
        if owner is None:
            return default
        if key == "memory_bridge":
            # Memory access remains a platform facade; plugin-specific bridge
            # discovery is delegated to mem_probe without hard-coded engine attrs.
            try:
                from mem_probe.mem_access import resolve_bridge
                bridge = resolve_bridge(owner)
            except Exception:
                bridge = None
            if bridge is not None:
                return bridge
            # else fall through to the flat alias lookup (covers memory-only adapters)
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
        self.path = record.path
        #: Declarative UI builder (see :mod:`act_platform.ui_spec`).
        self.ui = UI
        self._mem_access: Any = None
        self._stop_event = threading.Event()
        self._registered_threads: list[threading.Thread] = []

    @property
    def plugin_id(self) -> str:
        return self._record.plugin_id

    @property
    def should_stop(self) -> bool:
        return self._stop_event.is_set()

    def register_thread(self, thread: threading.Thread) -> None:
        """Register a worker thread so it gets joined on plugin unload."""
        if isinstance(thread, threading.Thread):
            self._registered_threads.append(thread)

    def _signal_stop_and_join(self, timeout: float = 2.0) -> None:
        self._stop_event.set()
        for t in self._registered_threads:
            try:
                if t.is_alive():
                    t.join(timeout=timeout)
            except Exception:
                pass
        self._registered_threads.clear()

    @property
    def mem(self) -> Any:
        """Read-only memory-scan facade (see :mod:`mem_probe.mem_access`).

        Hybrid-gated: in TCP-only mode every method returns
        ``{"ok": False, "reason": "mode_tcp", ...}``.  In hybrid/auto/memory it
        exposes ``self_state/entities/boss/damage_totals/skill_damage/attr_map/
        resolve_name/read_at/search/...`` plus ``catalog()`` and ``status()``.
        """
        facade = self._mem_access
        if facade is None:
            try:
                from mem_probe.mem_access import MemAccess
                facade = MemAccess(self.owner)
            except ImportError:
                return None
            self._mem_access = facade
        return facade

    @property
    def web_path(self) -> str:
        """Absolute path to this plugin's ``web/`` directory (may not exist)."""
        return os.path.join(self._record.path, "web")

    @property
    def assets_path(self) -> str:
        """Absolute path to this plugin's ``assets/`` directory (may not exist)."""
        return os.path.join(self._record.path, "assets")

    def resolve_web(self, filename: str) -> Optional[str]:
        """Resolve a web resource file inside this plugin's ``web/`` dir.

        Returns the absolute path if the file exists, else None.
        The host's webview can use this to load plugin HTML panels.
        """
        target = os.path.abspath(os.path.join(self.web_path, str(filename or "")))
        base = os.path.abspath(self.web_path)
        if os.path.commonpath([base, target]) != base:
            return None
        return target if os.path.isfile(target) else None

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

    def time(self) -> float:
        return time.time()

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

    # -- direct compositor layer API (bypass base64 rgba_frame path) --------

    def _get_compositor_overlay(self) -> Any:
        """Get the singleton compositor overlay instance."""
        try:
            from render.overlay_compositor import get_unified_overlay
            return get_unified_overlay()
        except ImportError:
            return None

    def create_compositor_layer(
        self, name: str, width: int, height: int,
        x: int = 0, y: int = 0, z: int = 140,
        click_through: bool = True,
        high_fps: bool = False,
        target_fps: int = 0,
    ) -> dict[str, Any]:
        """Create a compositor layer owned by this plugin for direct BGRA frame upload."""
        import sys
        overlay = self._get_compositor_overlay()
        if overlay is None:
            print("[CuteGirl-PY] compositor overlay unavailable", file=sys.stderr)
            return {"ok": False, "reason": "overlay_unavailable"}
        full_name = f"{self._record.plugin_id}_{name}"
        print(f"[CuteGirl-PY] creating layer '{full_name}' {width}x{height} at ({x},{y}) z={z} "
              f"overlay_id={id(overlay)} existing_layers={list(overlay._layers.keys())}", file=sys.stderr)
        layer = overlay.create_layer(
            full_name, int(width), int(height),
            x=int(x), y=int(y), z=int(z),
            click_through=bool(click_through),
            high_fps=bool(high_fps),
            target_fps=int(target_fps),
        )
        print(f"[CuteGirl-PY] layer created: {layer} visible={getattr(layer, 'visible', '?')}", file=sys.stderr)
        if not hasattr(self, "_compositor_layers"):
            self._compositor_layers: dict[str, Any] = {}
        self._compositor_layers[str(name)] = layer
        return {"ok": True, "name": full_name}

    def set_compositor_layer_mmf_source(
        self, name: str, mmf_name: str,
    ) -> None:
        """Attach an MMF zero-copy source to a compositor layer.

        The compositor render thread will read frames directly from
        the named shared memory, bypassing upload_compositor_frame().
        """
        layers = getattr(self, "_compositor_layers", None)
        if not layers:
            return
        layer = layers.get(str(name))
        if layer is None:
            return
        layer.set_mmf_source(str(mmf_name) if mmf_name else None)
        if not layer.visible:
            layer.show()

    def set_compositor_layer_shared_texture_source(
        self, name: str, handle: int, width: int, height: int,
    ) -> None:
        """Attach a GPU-shared D3D11 texture as a compositor layer's
        color source (zero-copy, GPU-to-GPU — no CPU pixel touch).

        *handle* is a legacy D3D11 shared handle value from the
        producer process's own ``IDXGIResource::GetSharedHandle()``.
        Requires ``compositor_gpu_interop_available()`` to be True —
        callers should check that first and keep using
        ``set_compositor_layer_mmf_source`` if it's False. Generic:
        any plugin can call this, not tied to any particular producer.
        """
        layers = getattr(self, "_compositor_layers", None)
        if not layers:
            return
        layer = layers.get(str(name))
        if layer is None:
            return
        layer.set_shared_texture_source(int(handle), int(width), int(height))
        if not layer.visible:
            layer.show()

    def compositor_gpu_interop_available(self) -> bool:
        """Whether the compositor's GPU interop path (WGL_NV_DX_interop2)
        is active. Callers must check this before ever attempting
        ``set_compositor_layer_shared_texture_source`` — if False, the
        platform/driver doesn't support it and the caller should keep
        using the MMF/upload path instead."""
        overlay = self._get_compositor_overlay()
        if overlay is None:
            return False
        dc = getattr(overlay, "_dcomp", None)
        return bool(dc is not None and getattr(dc, "gl_interop_active", False))

    def compositor_layer_shared_texture_active(self, name: str) -> bool:
        """Whether *name*'s layer is actually drawing from a registered
        GPU shared texture right now. Setting a handle can still fail
        later on the render thread (driver refuses the cross-process
        registration, interop disabled after a device loss, ...) —
        producers should poll this for a while after handing over a
        handle and, if it stays False, clear the source (handle=0) and
        resume their CPU/MMF publishing path."""
        layers = getattr(self, "_compositor_layers", None)
        if not layers:
            return False
        layer = layers.get(str(name))
        if layer is None:
            return False
        return bool(getattr(layer, "shared_texture_active", False))

    _upload_log_count: int = 0

    def upload_compositor_frame(
        self, name: str, bgra_bytes: Any, width: int, height: int,
        x: Any = None, y: Any = None,
    ) -> None:
        """Upload premultiplied BGRA frame bytes directly to a compositor layer."""
        import sys
        layers = getattr(self, "_compositor_layers", None)
        if not layers:
            if self._upload_log_count < 3:
                print(f"[CuteGirl-PY] upload: no _compositor_layers dict", file=sys.stderr)
                self._upload_log_count += 1
            return
        layer = layers.get(str(name))
        if layer is None:
            if self._upload_log_count < 3:
                print(f"[CuteGirl-PY] upload: layer '{name}' not found in {list(layers.keys())}", file=sys.stderr)
                self._upload_log_count += 1
            return
        if x is not None and y is not None:
            layer.set_position(int(x), int(y))
        raw = bytes(bgra_bytes) if not isinstance(bgra_bytes, bytes) else bgra_bytes
        layer.upload_bgra(raw, int(width), int(height))
        if not layer.visible:
            layer.show()
        if self._upload_log_count < 3:
            print(f"[CuteGirl-PY] upload OK: layer={layer.name} {width}x{height} visible={layer.visible} bytes={len(raw)}", file=sys.stderr)
            self._upload_log_count += 1

    def set_compositor_layer_position(self, name: str, x: int, y: int) -> None:
        """Update the screen position of a compositor layer (no frame data)."""
        layers = getattr(self, "_compositor_layers", None)
        if not layers:
            return
        layer = layers.get(str(name))
        if layer is not None:
            layer.set_position(int(x), int(y))

    def destroy_compositor_layer(self, name: str) -> None:
        """Destroy a compositor layer owned by this plugin."""
        layers = getattr(self, "_compositor_layers", None)
        if not layers:
            return
        layer = layers.pop(str(name), None)
        if layer is None:
            return
        try:
            overlay = self._get_compositor_overlay()
            if overlay is not None:
                overlay.destroy_layer(layer.name)
        except Exception:
            pass

    def set_compositor_layer_visible(self, name: str, visible: bool) -> None:
        layers = getattr(self, "_compositor_layers", None)
        if not layers:
            return
        layer = layers.get(str(name))
        if layer is None:
            return
        if visible:
            layer.show()
        else:
            layer.hide()

    def set_compositor_layer_input(
        self, name: str,
        cursor_pos_fn: Any = None, mouse_button_fn: Any = None,
        cursor_leave_fn: Any = None, scroll_fn: Any = None,
    ) -> None:
        """Register mouse callbacks on a compositor layer."""
        layers = getattr(self, "_compositor_layers", None)
        if not layers:
            return
        layer = layers.get(str(name))
        if layer is None:
            return
        layer.set_input_callbacks(
            cursor_pos_fn, cursor_leave_fn, mouse_button_fn, scroll_fn,
        )

    def _cleanup_compositor_layers(self) -> None:
        """Destroy all compositor layers owned by this plugin."""
        layers = getattr(self, "_compositor_layers", None)
        if not layers:
            return
        overlay = self._get_compositor_overlay()
        for _name, layer in list(layers.items()):
            try:
                if overlay is not None:
                    overlay.destroy_layer(layer.name)
            except Exception:
                pass
        layers.clear()

    def register_hotkey(self, hotkey_id: str, callback: Callable[[], Any],
                        default_key: str = "", label: str = "") -> str:
        """Register a customizable global hotkey for this plugin.

        Returns the action id ``plugin.<plugin_id>.<hotkey_id>``. The default key
        (e.g. ``"F6"``) seeds the shared hotkey settings; the user can rebind it
        in the normal keybinding editor. ``callback()`` fires when pressed.
        """
        return self._manager._register_hotkey(
            self._record.plugin_id, hotkey_id, callback, default_key, label)

    def register_menu_category(self, name: str, icon: str,
                               builder: Callable[[], list],
                               priority: float = 0.0) -> str:
        """Register a named menu category populated by a callback.

        ``builder() -> list[dict]`` returns items in the same format as
        ``_build_menu_children`` entries (icon/label/command dicts). The
        platform merges plugin-contributed categories into the SAO menu
        alongside the built-in ones. Returns the extension id.
        """
        ext_id = _safe_id(name) or _safe_id(f"{self._record.plugin_id}_{name}")
        if not ext_id:
            raise ValueError(f"invalid menu category name: {name!r}")
        meta: dict[str, Any] = {
            "title": str(name),
            "description": f"Menu category from {self._record.name}",
        }
        normalized = self._manager._register_extension(
            "menu_categories", self._record.plugin_id, ext_id, meta, builder)
        self._manager._menu_categories[ext_id] = {
            "plugin_id": self._record.plugin_id,
            "name": str(name),
            "icon": str(icon or ""),
            "builder": builder,
            "priority": float(priority or 0.0),
        }
        return ext_id

    def register_menu_surface(self, surface_id: str,
                              descriptor: Mapping[str, Any],
                              priority: float = 0.0) -> str:
        """Register plugin-owned content for a generic host menu surface.

        ``descriptor`` may contain callbacks such as ``header_provider``,
        ``left_widget_factory``, ``on_open`` and ``on_close``. The platform
        stores the descriptor opaquely and invokes callbacks without knowing
        the plugin's domain schema.
        """
        return self._manager._register_menu_surface(
            self._record.plugin_id, surface_id, descriptor, priority)

    def register_action_handler(self, handler: Callable[[str, Mapping[str, Any]], Any]) -> str:
        """Register an opaque action dispatcher for this plugin."""
        return self._manager._register_action_handler(self._record.plugin_id, handler)

    def register_engine(self, name: str, engine: Any) -> None:
        """Register a named engine that the platform and other plugins can query.

        The engine becomes available via ``ctx.get_engine(name)`` for all
        plugins and via ``EngineAccess.get(name)`` for the platform.
        """
        key = _normalize_engine_name(name)
        if not key:
            raise ValueError(f"invalid engine name: {name!r}")
        self._manager._plugin_engines[key] = engine
        self._manager._plugin_engine_owners[key] = self._record.plugin_id

    def register_data_source(self, source_id: str,
                             metadata: Optional[Mapping[str, Any]] = None,
                             start: Optional[Callable[[], Any]] = None,
                             stop: Optional[Callable[[], Any]] = None) -> dict[str, Any]:
        """Register a data source (packet capture, memory reader, etc.).

        ``start()`` / ``stop()`` control the source lifecycle. The platform's
        data-source-health panel auto-discovers registered sources.
        """
        meta = dict(metadata or {}) if isinstance(metadata, Mapping) else {}
        meta.setdefault("title", str(source_id))
        meta.setdefault("description", f"Data source from {self._record.name}")
        normalized = self._manager._register_extension(
            "data_sources", self._record.plugin_id, source_id, meta, start)
        self._manager._data_sources[normalized["id"]] = {
            "plugin_id": self._record.plugin_id,
            "metadata": meta,
            "start": start,
            "stop": stop,
            "running": False,
        }
        return normalized

    def request_redraw(self, surface: str = "", reason: str = "") -> dict[str, Any]:
        """Ask all host surfaces (or one) to repaint plugin content."""
        return self.emit("plugin_ui_invalidate", {
            "plugin_id": self._record.plugin_id,
            "surface": str(surface or ""),
            "reason": str(reason or ""),
        })

    def open_window(self, panel_id: str = "", width: int = 0, height: int = 0) -> dict[str, Any]:
        """Ask the host to open one of this plugin's panels in its own window.

        ``panel_id`` selects which registered ``ui_panel`` to show (defaults to
        the plugin's primary panel); ``width``/``height`` override the panel's
        declared size for this window (0 = use the panel meta size, else the host
        default). The host opens a real, freely-movable window — not an in-place
        view. Entity mode opens a detached Tk window; both renderers listen for
        the emitted ``plugin_open_window`` event.
        """
        return self.emit("plugin_open_window", {
            "plugin_id": self._record.plugin_id,
            "panel_id": str(panel_id or ""),
            "width": max(0, int(width or 0)),
            "height": max(0, int(height or 0)),
        })

    def open_file(self, filters: Any = None, title: str = "选择文件",
                  initial_dir: str = "", hwnd_owner: int = 0) -> str:
        """Open a platform-native file picker and return the selected path.

        ``filters`` accepts ``[(label, pattern), ...]`` or small mappings with
        ``label``/``pattern`` keys. An empty string means cancelled or failed.
        Pass ``hwnd_owner < 0`` to force an unowned dialog; this is useful when
        opening after transient GPU/overlay UI that may still be foreground.
        """
        try:
            from . import native_dialog
        except Exception as exc:
            self._manager._record_failure(self._record.plugin_id, exc)
            return ""

        norm_filters: list[tuple[str, str]] = []
        try:
            source = filters
            if isinstance(source, Mapping):
                source = source.items()
            if source is not None and not isinstance(source, (str, bytes)):
                for item in source:
                    label: Any = ""
                    pattern: Any = ""
                    if isinstance(item, Mapping):
                        label = item.get("label") or item.get("name") or item.get("title")
                        pattern = item.get("pattern") or item.get("glob") or item.get("filter")
                    else:
                        try:
                            seq = list(item)
                        except Exception:
                            seq = []
                        if len(seq) >= 2:
                            label, pattern = seq[0], seq[1]
                    if str(label or "").strip() and str(pattern or "").strip():
                        norm_filters.append((str(label), str(pattern)))
        except Exception:
            norm_filters = []

        dialog_owner = None
        try:
            owner = self.owner
            dialog_owner = owner
            if owner is not None:
                try:
                    setattr(owner, "_sao_native_dialog_active", True)
                except Exception:
                    pass
            menu = getattr(owner, "_sao_menu", None)
            if menu is not None and getattr(menu, "visible", False):
                close_for_dialog = getattr(owner, "_close_sao_menu_for_external_command", None)
                if callable(close_for_dialog):
                    try:
                        close_for_dialog()
                    except Exception:
                        pass
            requested_owner = int(hwnd_owner or 0)
            owner_hwnd = 0 if requested_owner < 0 else (requested_owner or native_dialog.foreground_hwnd())
            path = native_dialog.open_file(
                filters=norm_filters or None,
                title=str(title or "选择文件"),
                initial_dir=str(initial_dir or ""),
                hwnd_owner=owner_hwnd,
            )
            return str(path or "")
        except Exception as exc:
            self._manager._record_failure(self._record.plugin_id, exc)
            return ""
        finally:
            owner = dialog_owner
            if owner is not None:
                try:
                    setattr(owner, "_sao_native_dialog_active", False)
                except Exception:
                    pass
                try:
                    setattr(owner, "_fisheye_close_suppress_until", time.time() + 0.35)
                except Exception:
                    pass

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

    def ensure_requirements(self, install: bool = True) -> dict[str, Any]:
        """Satisfy this plugin's ``requirements.txt`` from its own ``vendor/``/``libs/``.

        Generalized dependency bootstrap (see :mod:`act_platform.plugin_deps`):
        prepends the plugin's ``engine/`` ``libs/`` ``vendor/`` to ``sys.path`` so
        a bundled (vendored) pure-Python dep imports without touching the global
        site-packages; in a non-frozen dev tree it can ``pip install --target``
        into ``libs/`` first. Paths are restored on unload. ``install=False``
        skips the pip step (validate/path-prepend only).
        """
        from . import plugin_deps
        rec = plugin_deps.ensure_requirements(self._record.path, log=self.log, install=bool(install))
        for path in rec.get("added", []) or []:
            if path not in self._record.added_sys_paths:
                self._record.added_sys_paths.append(path)
        deps = rec.get("deps")
        if isinstance(deps, Mapping):
            self._record.deps_summary = {str(k): str(v) for k, v in deps.items()}
        return rec

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
    """Discover and manage in-process Python plugins."""

    def __init__(self, plugin_dirs: Optional[Iterable[str]] = None,
                 event_bus: Optional[EventBus] = None,
                 snapshot_provider: Optional[Callable[[], Mapping[str, Any]]] = None,
                 owner_provider: Optional[Callable[[], Any]] = None,
                 settings: Any = None,
                 max_failures: int = 3,
                 user_plugin_dirs: Optional[Iterable[str]] = None) -> None:
        self.plugin_dirs = [os.path.abspath(path) for path in (plugin_dirs or []) if path]
        #: Writable dirs where one-click-imported plugins live (eligible for
        #: uninstall). Built-in ``plugins/`` is never user-writable. Defaults to
        #: any ``plugin_dirs`` entry literally named ``user_plugins``.
        if user_plugin_dirs is not None:
            self.user_plugin_dirs = [os.path.abspath(path) for path in user_plugin_dirs if path]
        else:
            self.user_plugin_dirs = [d for d in self.plugin_dirs
                                     if os.path.basename(d).lower() == "user_plugins"]
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
        #: {plugin_id: bool} persisted enable/disable choices (settings fallback).
        self._enabled_cache: dict[str, bool] = {}
        #: True once load_all() has run — lets ensure_act_plugin_manager(load=True)
        #: be idempotent instead of reloading every plugin on every call.
        self._initial_loaded = False
        #: Plugin-contributed menu categories: ext_id -> {plugin_id, name, icon, builder, priority}.
        self._menu_categories: Dict[str, dict[str, Any]] = {}
        #: Plugin-contributed generic menu surfaces.
        self._menu_surfaces: Dict[str, dict[str, Any]] = {}
        #: plugin_id -> opaque action handler.
        self._action_handlers: Dict[str, Callable[[str, Mapping[str, Any]], Any]] = {}
        #: Plugin-contributed named engines: engine_name -> engine_instance.
        self._plugin_engines: Dict[str, Any] = {}
        #: Reverse map engine_name -> plugin_id for cleanup on unload.
        self._plugin_engine_owners: Dict[str, str] = {}
        #: Plugin-contributed data sources: source_id -> {plugin_id, metadata, start_fn, stop_fn, running}.
        self._data_sources: Dict[str, dict[str, Any]] = {}
        #: Lightweight filesystem signature for manifest hot-add/remove/change.
        self._discovery_signature_cache: tuple[Any, ...] | None = None

    def _discovery_signature(self) -> tuple[Any, ...]:
        """Return a cheap signature of plugin manifests under all roots."""
        rows: list[Any] = []
        for root in self.plugin_dirs:
            root_abs = os.path.abspath(str(root or ""))
            try:
                root_stat = os.stat(root_abs)
                rows.append(("root", root_abs, int(root_stat.st_mtime_ns), int(root_stat.st_size)))
                names = sorted(os.listdir(root_abs))
            except OSError:
                rows.append(("missing", root_abs))
                continue
            for name in names:
                plug_dir = os.path.join(root_abs, name)
                manifest_path = os.path.join(plug_dir, MANIFEST_FILE)
                if not os.path.isdir(plug_dir):
                    continue
                try:
                    dir_stat = os.stat(plug_dir)
                except OSError:
                    rows.append(("dir_gone", plug_dir))
                    continue
                if not os.path.isfile(manifest_path):
                    rows.append(("dir", plug_dir, int(dir_stat.st_mtime_ns), 0, 0))
                    continue
                try:
                    manifest_stat = os.stat(manifest_path)
                    rows.append((
                        "plugin",
                        plug_dir,
                        int(dir_stat.st_mtime_ns),
                        int(manifest_stat.st_mtime_ns),
                        int(manifest_stat.st_size),
                    ))
                except OSError:
                    rows.append(("manifest_gone", plug_dir))
        return tuple(rows)

    def sync_discovery(self, *, force: bool = False) -> list[PluginRecord]:
        """Refresh manifest records when plugin roots changed, without loading plugins."""
        try:
            signature = self._discovery_signature()
        except Exception:
            signature = None
        if force or self._discovery_signature_cache != signature:
            records = self.discover()
            try:
                self._discovery_signature_cache = self._discovery_signature()
            except Exception:
                self._discovery_signature_cache = signature
            return records
        return list(self._records.values())

    def discover(self) -> list[PluginRecord]:
        old_records = dict(self._records)
        records: Dict[str, PluginRecord] = {}
        changed_ids: set[str] = set()
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
        # Apply the user's persisted enable/disable choices over the manifest
        # default so a plugin the user turned off (or on) stays that way across
        # restarts. Only for validly-parsed plugins.
        persisted = self._persisted_enabled()
        if persisted:
            for pid, rec in records.items():
                if pid in persisted and not rec.last_error:
                    rec.enabled = bool(persisted[pid])
        for pid, old_record in old_records.items():
            new_record = records.get(pid)
            changed = (
                new_record is None
                or old_record.path != new_record.path
                or old_record.entry != new_record.entry
                or old_record.name != new_record.name
                or old_record.description != getattr(new_record, "description", "")
                or old_record.version != new_record.version
                or old_record.language != new_record.language
                or old_record.sao_menu != getattr(new_record, "sao_menu", {})
                or old_record.settings_schema != getattr(new_record, "settings_schema", {})
                or old_record.locales != getattr(new_record, "locales", {})
                or old_record.capabilities != getattr(new_record, "capabilities", ())
                or old_record.requires != getattr(new_record, "requires", ())
                or old_record.permissions != getattr(new_record, "permissions", ())
                or old_record.enabled != getattr(new_record, "enabled", old_record.enabled)
            )
            if changed:
                changed_ids.add(pid)
            if changed and bool(old_record.loaded or old_record.active or old_record.module is not None):
                try:
                    self.unload_plugin(pid)
                except Exception:
                    pass
            elif not changed and bool(old_record.loaded or old_record.active or old_record.module is not None):
                old_record.enabled = bool(getattr(new_record, "enabled", old_record.enabled))
                records[pid] = old_record
        self._records = records
        try:
            self._discovery_signature_cache = self._discovery_signature()
        except Exception:
            self._discovery_signature_cache = None
        for pid, old_record in old_records.items():
            if pid not in self._records:
                self._publish_plugin_lifecycle(old_record, "forgotten")
        for pid, record in self._records.items():
            old_record = old_records.get(pid)
            if old_record is None:
                self._publish_plugin_lifecycle(record, "discovered")
            elif pid in changed_ids:
                if old_record.path != record.path or old_record.version != record.version:
                    self._publish_plugin_lifecycle(record, "discovered")
                else:
                    self._publish_plugin_lifecycle(record, "manifest_changed")
        return list(self._records.values())

    def refresh_plugin(self, plug_dir: str) -> Optional[PluginRecord]:
        """(Re)read a single plugin directory and register/replace its record.

        Used by one-click import to add a freshly installed plugin **without** a
        full :meth:`reload_all` (which would restart every other stateful
        plugin). Raises if the manifest is missing/invalid so the caller can
        surface the error. Upgrading an already-loaded plugin unloads the old
        copy first (running its ``on_unload``/releasing subscriptions+timers).
        """
        plug_dir = os.path.abspath(str(plug_dir or ""))
        manifest_path = os.path.join(plug_dir, MANIFEST_FILE)
        if not os.path.isfile(manifest_path):
            raise FileNotFoundError(MANIFEST_FILE)
        record = self._read_manifest(plug_dir, manifest_path)
        persisted = self._persisted_enabled()
        if record.plugin_id in persisted and not record.last_error:
            record.enabled = bool(persisted[record.plugin_id])
        existing = self._records.get(record.plugin_id)
        if existing is not None and existing.loaded:
            self.unload_plugin(record.plugin_id)
        self._records[record.plugin_id] = record
        self._publish_plugin_lifecycle(record, "discovered")
        try:
            self._discovery_signature_cache = self._discovery_signature()
        except Exception:
            self._discovery_signature_cache = None
        return record

    def is_user_plugin(self, plugin_id: str) -> bool:
        record = self._records.get(str(plugin_id or ""))
        return bool(record and self._is_user_path(record.path))

    def _is_user_path(self, path: str) -> bool:
        path = os.path.abspath(str(path or ""))
        for root in getattr(self, "user_plugin_dirs", []) or []:
            try:
                if os.path.commonpath([root, path]) == root and path != root:
                    return True
            except ValueError:
                continue
        return False

    def _topo_sorted_ids(self) -> list[str]:
        """Return plugin ids sorted so that ``requires`` dependencies load first."""
        ids = sorted(self._records)
        loaded: set[str] = set()
        ordered: list[str] = []
        visited: set[str] = set()

        def _visit(pid: str) -> None:
            if pid in visited:
                return
            visited.add(pid)
            rec = self._records.get(pid)
            if rec:
                for dep in rec.requires:
                    if dep in self._records:
                        _visit(dep)
            ordered.append(pid)

        for pid in ids:
            _visit(pid)
        return ordered

    def load_all(self) -> dict[str, Any]:
        if not self._records:
            self.discover()
        for plugin_id in self._topo_sorted_ids():
            record = self._records[plugin_id]
            if record.enabled:
                if record.requires:
                    missing = [r for r in record.requires
                               if r not in self._records or not self._records[r].loaded]
                    if missing:
                        record.last_error = f"missing prerequisites: {', '.join(missing)}"
                        continue
                self.load_plugin(plugin_id)
        self._initial_loaded = True
        return self.status()

    def load_pending_enabled(self) -> dict[str, Any]:
        """Load enabled plugins that are not currently active.

        Used after hot discovery so newly dropped or manifest-changed enabled
        plugins become available without restarting already active plugins.
        """
        if not self._records:
            self.discover()
        for plugin_id in self._topo_sorted_ids():
            record = self._records[plugin_id]
            if not record.enabled:
                continue
            if record.loaded and record.active and record.module is not None:
                continue
            if record.requires:
                missing = [r for r in record.requires
                           if r not in self._records or not self._records[r].loaded]
                if missing:
                    record.last_error = f"missing prerequisites: {', '.join(missing)}"
                    continue
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
            # Make the plugin's own bundled deps (vendor/ + libs/) importable
            # before its entry module runs, so a vendored pure-Python dependency
            # resolves without the plugin having to call ctx.ensure_requirements.
            self._prepare_plugin_sys_path(record)
            if record.language == "python":
                module = self._load_module(record)
                record.module = module
                record.context = PluginContext(self, record)
                self._call_hook(record, "on_load", record.context)
            else:
                record.context = PluginContext(self, record)
                module = self._load_script_module(record)
                record.module = module
                self._call_hook(record, "on_load", record.context)
            self._call_hook(record, "on_enable")
            record.loaded = True
            record.active = True
            record.last_loaded_at = time.time()
            record.last_error = ""
            self._publish_plugin_lifecycle(record, "loaded")
            return True
        except Exception as exc:
            self._record_failure(record.plugin_id, exc)
            for token in list(record.subscriptions):
                self.event_bus.unsubscribe(token)
            record.subscriptions.clear()
            self._unregister_plugin_extensions(record.plugin_id)
            self._unload_script_runtime(record)
            record.module = None
            record.context = None
            record.loaded = False
            record.active = False
            self._publish_plugin_lifecycle(record, "load_failed", message=str(exc))
            return False

    def unload_plugin(self, plugin_id: str) -> bool:
        record = self._records.get(str(plugin_id or ""))
        if record is None:
            return False
        was_loaded = bool(record.loaded or record.active or record.module is not None)
        if record.context is not None:
            try:
                record.context._stop_event.set()
            except Exception:
                pass
        if record.module is not None:
            try:
                self._call_hook(record, "on_disable")
            except Exception:
                pass
            try:
                self._call_hook(record, "on_unload")
            except Exception:
                pass
        if record.context is not None:
            try:
                record.context._signal_stop_and_join(timeout=2.0)
            except Exception:
                pass
        self._unload_script_runtime(record)
        for token in list(record.subscriptions):
            self.event_bus.unsubscribe(token)
        record.subscriptions.clear()
        if record.context is not None:
            try:
                record.context._cleanup_compositor_layers()
            except Exception:
                pass
        self._unregister_plugin_extensions(record.plugin_id)
        record.module = None
        record.context = None
        record.loaded = False
        record.active = False
        if was_loaded:
            self._publish_plugin_lifecycle(record, "unloaded")
        return True

    def forget_plugin(self, plugin_id: str) -> bool:
        """Unload and drop a plugin's record entirely (used after uninstall)."""
        plugin_id = str(plugin_id or "")
        record = self._records.get(plugin_id)
        if record is None:
            return False
        self.unload_plugin(plugin_id)
        self._records.pop(plugin_id, None)
        self._settings_cache.pop(plugin_id, None)
        self._publish_plugin_lifecycle(record, "forgotten")
        try:
            self._discovery_signature_cache = self._discovery_signature()
        except Exception:
            self._discovery_signature_cache = None
        return True

    def enable_plugin(self, plugin_id: str) -> bool:
        record = self._records.get(str(plugin_id or ""))
        if record is None:
            return False
        record.enabled = True
        self._set_persisted_enabled(record.plugin_id, True)
        return self.load_plugin(record.plugin_id)

    def disable_plugin(self, plugin_id: str) -> bool:
        record = self._records.get(str(plugin_id or ""))
        if record is None:
            return False
        record.enabled = False
        self._set_persisted_enabled(record.plugin_id, False)
        was_loaded = bool(record.loaded or record.active or record.module is not None)
        self.unload_plugin(record.plugin_id)
        if not was_loaded:
            self._publish_plugin_lifecycle(record, "disabled")
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

    def _owner(self) -> Any:
        if callable(self.owner_provider):
            try:
                return self.owner_provider()
            except Exception:
                return None
        return None

    def current_locale(self) -> str:
        for key in ("act_plugin_locale", "plugin_locale", "ui_locale", "locale", "ui_language", "language"):
            value = _MISSING
            if self.settings is not None and hasattr(self.settings, "get"):
                try:
                    value = self.settings.get(key, _MISSING)
                except Exception:
                    value = _MISSING
            locale = _normalize_locale(value)
            if locale:
                return locale
        owner = self._owner()
        for attr in ("act_plugin_locale", "plugin_locale", "ui_locale", "locale", "ui_language"):
            locale = _normalize_locale(getattr(owner, attr, ""))
            if locale:
                return locale
        locale = _normalize_locale(os.environ.get("SAO_UI_LOCALE") or os.environ.get("LANG"))
        return locale or "zh-CN"

    def list_plugins(self, locale: Any = "") -> list[dict[str, Any]]:
        locale_id = _normalize_locale(locale) or self.current_locale()
        pinned = set(self.pinned_plugins())
        hotkeys_by_plugin: dict[str, int] = {}
        with self._hotkeys_lock:
            metas = list(self._hotkeys.values())
        for meta in metas:
            pid = str(meta.get("plugin_id") or "")
            hotkeys_by_plugin[pid] = hotkeys_by_plugin.get(pid, 0) + 1
        out: list[dict[str, Any]] = []
        for key in sorted(self._records):
            status = self._records[key].to_status(locale_id)
            status["pinned"] = key in pinned
            status["hotkey_count"] = hotkeys_by_plugin.get(key, 0)
            status["user_installed"] = self._is_user_path(self._records[key].path)
            out.append(status)
        return out

    def list_script_menu_entries(self, locale: Any = "") -> list[dict[str, Any]]:
        """Return manifest-declared SAO popup buttons for script-like plugins.

        Only enabled plugins are exposed here. Disabled scripts stay
        discoverable/manageable without being launchable from the popup.
        """
        locale_id = _normalize_locale(locale) or self.current_locale()
        entries: list[dict[str, Any]] = []
        script_languages = {"lua", "csharp", "angelscript", "emma", "python"}

        def _boolish(value: Any, default: bool = False) -> bool:
            if isinstance(value, bool):
                return value
            if isinstance(value, (int, float)):
                return bool(value)
            if isinstance(value, str):
                text = value.strip().lower()
                if text in {"1", "true", "yes", "on", "enabled"}:
                    return True
                if text in {"0", "false", "no", "off", "disabled"}:
                    return False
            return bool(default)

        for key in sorted(self._records):
            record = self._records[key]
            if str(record.language or "").lower() not in script_languages:
                continue
            if not bool(record.enabled):
                continue
            meta = dict(record.localized_sao_menu(locale_id) or {})
            if not meta:
                continue
            schema = record.localized_settings_schema(locale_id)
            if not isinstance(schema, Mapping):
                schema = {}
            setting_key = str(meta.get("setting") or meta.get("overlay_setting") or "overlay_enabled")
            overlay_schema = schema.get(setting_key)
            default_enabled = _boolish(meta.get("default_enabled"), False)
            if isinstance(overlay_schema, Mapping):
                default_enabled = _boolish(overlay_schema.get("default"), default_enabled)
            overlay_enabled = _boolish(
                self.get_plugin_setting(record.plugin_id, setting_key, default_enabled),
                default_enabled,
            )
            entries.append({
                "id": record.plugin_id,
                "plugin_id": record.plugin_id,
                "name": record.localized_name(locale_id),
                "plugin_label": record.localized_name(locale_id),
                "language": record.language,
                "locale": locale_id,
                "enabled": bool(record.enabled),
                "loaded": bool(record.loaded),
                "active": bool(record.active and record.enabled),
                "overlay_enabled": overlay_enabled,
                "default_enabled": default_enabled,
                "setting": setting_key,
                "action_id": str(meta.get("action_id") or "script.overlay.set_enabled"),
                "surface": str(meta.get("surface") or "unioverlay"),
                "menu": meta,
            })
        entries.sort(key=lambda item: (
            float((item.get("menu") or {}).get("priority") or 50.0),
            str((item.get("menu") or {}).get("name") or item.get("id") or ""),
        ))
        return entries

    def list_extensions(self, kind: str = "") -> dict[str, list[dict[str, Any]]] | list[dict[str, Any]]:
        if kind:
            bucket = self._extensions.get(str(kind or ""), {})
            return [dict(bucket[key]) for key in sorted(bucket)]
        return {
            ext_kind: [dict(bucket[key]) for key in sorted(bucket)]
            for ext_kind, bucket in self._extensions.items()
        }

    def resolve_plugin_web(self, plugin_id: str, filename: str) -> Optional[str]:
        """Resolve a web resource from a specific plugin's ``web/`` dir."""
        record = self._records.get(str(plugin_id or ""))
        if record is None:
            return None
        target = os.path.abspath(os.path.join(record.path, "web", str(filename or "")))
        base = os.path.abspath(os.path.join(record.path, "web"))
        if os.path.commonpath([base, target]) != base:
            return None
        return target if os.path.isfile(target) else None

    def get_menu_categories(self) -> dict[str, dict[str, Any]]:
        """Return all plugin-contributed menu categories sorted by priority.

        Each value: {plugin_id, name, icon, builder, priority}. The ``builder``
        is a callable ``() -> list[dict]`` that returns items in the same
        format as ``_build_menu_children`` entries.
        """
        active: dict[str, dict[str, Any]] = {}
        for ext_id, cat in self._menu_categories.items():
            pid = str(cat.get("plugin_id") or "")
            rec = self._records.get(pid)
            if rec and rec.loaded and rec.active and rec.enabled:
                active[ext_id] = dict(cat)
        return dict(sorted(active.items(), key=lambda kv: float(kv[1].get("priority") or 0)))

    def _register_menu_surface(self, plugin_id: str, surface_id: str,
                               descriptor: Mapping[str, Any],
                               priority: float = 0.0) -> str:
        surface_key = _safe_id(surface_id) or _safe_id(f"{plugin_id}_surface")
        if not surface_key:
            raise ValueError(f"invalid menu surface id: {surface_id!r}")
        item = dict(descriptor or {}) if isinstance(descriptor, Mapping) else {}
        item["plugin_id"] = str(plugin_id or "")
        item["surface_id"] = surface_key
        item["priority"] = float(priority or item.get("priority") or 0.0)
        self._menu_surfaces[f"{plugin_id}:{surface_key}"] = item
        return surface_key

    def get_menu_surfaces(self, surface_id: str = "") -> list[dict[str, Any]]:
        surface_key = _safe_id(surface_id) if surface_id else ""
        active: list[dict[str, Any]] = []
        for item in self._menu_surfaces.values():
            pid = str(item.get("plugin_id") or "")
            rec = self._records.get(pid)
            if rec is None or not rec.loaded or not rec.active or not rec.enabled:
                continue
            if surface_key and str(item.get("surface_id") or "") != surface_key:
                continue
            active.append(dict(item))
        active.sort(key=lambda item: float(item.get("priority") or 0.0))
        return active

    def _register_action_handler(self, plugin_id: str,
                                 handler: Callable[[str, Mapping[str, Any]], Any]) -> str:
        if not callable(handler):
            raise TypeError("action handler must be callable")
        self._action_handlers[str(plugin_id or "")] = handler
        return str(plugin_id or "")

    def dispatch_plugin_action(self, action_id: str,
                               payload: Optional[Mapping[str, Any]] = None,
                               plugin_id: str = "") -> dict[str, Any]:
        action = str(action_id or "")
        if not action:
            return {"ok": False, "message": "action id is required", "errors": ["action id is required"]}
        target_plugin = str(plugin_id or "")
        if target_plugin:
            record = self._records.get(target_plugin)
            if record is not None and record.enabled and not (
                    record.loaded and record.active and record.module is not None):
                self.load_plugin(target_plugin)
        candidates = [target_plugin] if target_plugin else list(self._action_handlers)
        errors: list[str] = []
        for pid in candidates:
            handler = self._action_handlers.get(pid)
            rec = self._records.get(pid)
            if not callable(handler) or rec is None or not rec.loaded or not rec.active or not rec.enabled:
                continue
            try:
                result = handler(action, copy.deepcopy(dict(payload or {})))
            except Exception as exc:
                self._record_failure(pid, exc)
                errors.append(_safe_exception_text(exc))
                continue
            if result is None:
                continue
            return {"ok": True, "plugin_id": pid, "action_id": action,
                    "result": _json_safe(result), "errors": []}
        return {"ok": False, "action_id": action, "message": "no plugin handled action", "errors": errors}

    def list_data_sources(self) -> list[dict[str, Any]]:
        """Return all plugin-contributed data sources with their status."""
        out: list[dict[str, Any]] = []
        for src_id, src in self._data_sources.items():
            pid = str(src.get("plugin_id") or "")
            rec = self._records.get(pid)
            out.append({
                "id": src_id,
                "plugin_id": pid,
                "available": bool(rec and rec.loaded and rec.active),
                "running": bool(src.get("running")),
                "metadata": dict(src.get("metadata") or {}),
            })
        return out

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
        token = self.render_registry.register_hook(plugin_id, surface, callback, priority)
        self._publish_plugin_ui_invalidate(plugin_id, surface, "render_hook_registered")
        return token

    def _set_overlay(self, plugin_id: str, surface: str, spec: Any) -> dict[str, Any]:
        normalized, changed = self.render_registry.set_overlay_with_status(plugin_id, surface, spec)
        if changed:
            self._publish_plugin_ui_invalidate(plugin_id, surface, "overlay_set")
        return normalized

    def _clear_overlay(self, plugin_id: str, surface: Optional[str] = None) -> None:
        self.render_registry.clear_overlay(plugin_id, surface)
        self._publish_plugin_ui_invalidate(plugin_id, surface or "", "overlay_cleared")

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
        from config import normalize_hotkey
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
                # 规范化拼写 ('Control+F8'→'CTRL+F8'), 占用表才能按字符串比较
                "default_key": (normalize_hotkey(default_key)
                                or str(default_key or "").upper()),
                "label": str(label or hid),
            }
        # 一次性迁移: 旧版改键 UI 允许把插件键设成与内置键同键 (运行时被
        # 内置永久遮蔽且无任何提示)。注册时发现存量覆盖与内置现值同键就
        # 丢弃, 让新的不冲突默认键生效。
        self._drop_shadowed_override(action)
        return action

    def _drop_shadowed_override(self, action: str) -> None:
        from config import DEFAULT_HOTKEYS, normalize_hotkey
        if self.settings is None or not (hasattr(self.settings, "get")
                                         and hasattr(self.settings, "set")):
            return
        raw = self.settings.get("hotkeys", {}) or {}
        if not isinstance(raw, dict) or action not in raw:
            return

        def _canon(value: Any) -> Optional[str]:
            if isinstance(value, Mapping):
                value = value.get("key") or value.get("name") or ""
            return normalize_hotkey(str(value or ""))

        override = _canon(raw.get(action))
        if override is None:
            return
        builtin_current = {_canon(raw.get(b_action, default))
                           for b_action, default in DEFAULT_HOTKEYS.items()}
        if override not in builtin_current:
            return
        raw = dict(raw)
        raw.pop(action, None)
        self.settings.set("hotkeys", raw)
        save = getattr(self.settings, "save", None)
        if callable(save):
            try:
                save()
            except Exception:
                pass

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

    def occupied_hotkeys(self, exclude_action: str = "") -> dict[str, str]:
        """当前已占用的快捷键 {键: 归属} — 改键 UI 置灰 + set_hotkey 拒冲突。

        内置动作取 settings 覆盖后的现值 (归属为动作 id), 插件取
        list_hotkeys 现值且仅计 active 的 (归属为 label)。键经
        normalize_hotkey 规范化 ('Control+F8'→'CTRL+F8'), 与改键 UI 的
        选项字符串可直接比较; 不可规范化的键按原样大写记录。
        ``exclude_action`` 把正在改键的动作自身排除在外。
        """
        from config import DEFAULT_HOTKEYS, normalize_hotkey
        exclude_action = str(exclude_action or "").lower()
        raw: Mapping = {}
        if self.settings is not None and hasattr(self.settings, "get"):
            candidate = self.settings.get("hotkeys", {}) or {}
            if isinstance(candidate, Mapping):
                raw = candidate

        def _canon(value: Any) -> str:
            if isinstance(value, Mapping):
                value = value.get("key") or value.get("name") or ""
            text = str(value or "").strip().upper()
            return normalize_hotkey(text) or text

        occupied: dict[str, str] = {}
        for action, default in DEFAULT_HOTKEYS.items():
            if action == exclude_action:
                continue
            key = _canon(raw.get(action, default))
            if key:
                occupied.setdefault(key, action)
        for hk in self.list_hotkeys():
            action = str(hk.get("action") or "")
            if not hk.get("active") or action == exclude_action:
                continue
            key = _canon(hk.get("current_key"))
            if key:
                occupied.setdefault(key, str(hk.get("label") or action))
        return occupied

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
        so the plugin's declared default applies again — refused when that
        default is meanwhile occupied by another action. Keys are stored in the
        canonical ``"F8"`` / ``"CTRL+F8"`` spelling (``config.normalize_hotkey``)
        that the listeners parse via ``config.parse_hotkey``. Unparseable keys
        and keys already bound elsewhere (built-in action or another active
        plugin hotkey) are rejected with ``False``.
        """
        from config import normalize_hotkey
        # Hotkey actions are stored lowercase (_safe_id); normalize callers'
        # input so a mixed-case action id still matches.
        action = str(action or "").lower()
        with self._hotkeys_lock:
            meta = self._hotkeys.get(action)
        if meta is None:
            return False
        if self.settings is None or not (hasattr(self.settings, "get") and hasattr(self.settings, "set")):
            return False
        raw = self.settings.get("hotkeys", {}) or {}
        raw = dict(raw) if isinstance(raw, dict) else {}
        key_norm = str(key or "").strip().upper()
        if not key_norm or key_norm in ("DEFAULT", "(DEFAULT)", "默认", "NONE", "无"):
            if action in raw:
                # 清除覆盖会让声明默认键重新生效 — 默认键已被别的动作占走
                # 时拒绝, 否则会悄悄造出双绑定 (运行时只响一个)。
                default_canon = normalize_hotkey(str(meta.get("default_key") or ""))
                if default_canon and default_canon in self.occupied_hotkeys(
                        exclude_action=action):
                    return False
            raw.pop(action, None)
        else:
            canon = normalize_hotkey(key_norm)
            if canon is None:
                return False
            if canon in self.occupied_hotkeys(exclude_action=action):
                return False
            raw[action] = canon
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

    # ── persisted enable/disable state (remembered across restarts) ────────
    def _persisted_enabled(self) -> dict[str, bool]:
        if self.settings is not None and hasattr(self.settings, "get"):
            raw = self.settings.get("act_plugin_enabled", {}) or {}
            if isinstance(raw, Mapping):
                return {str(k): bool(v) for k, v in raw.items()}
        return dict(self._enabled_cache)

    def _set_persisted_enabled(self, plugin_id: str, enabled: bool) -> None:
        current = self._persisted_enabled()
        current[str(plugin_id or "")] = bool(enabled)
        self._store_persisted_enabled(current)

    def clear_persisted_enabled(self, plugin_id: str) -> None:
        """卸载后清掉该插件的持久化启用标记, 避免同名插件复用旧状态."""
        current = self._persisted_enabled()
        if str(plugin_id or "") not in current:
            return
        current.pop(str(plugin_id or ""), None)
        self._store_persisted_enabled(current)

    def _store_persisted_enabled(self, current: dict[str, bool]) -> None:
        if self.settings is not None and hasattr(self.settings, "set"):
            self.settings.set("act_plugin_enabled", current)
            save = getattr(self.settings, "save", None)
            if callable(save):
                try:
                    save()
                except Exception:
                    pass
        else:
            self._enabled_cache = current

    def _publish_plugin_lifecycle(self, record: PluginRecord, action: str,
                                  **extra: Any) -> None:
        """Broadcast plugin lifecycle changes to menus and overlay hosts."""
        payload: dict[str, Any] = {
            "plugin_id": record.plugin_id,
            "action": str(action or ""),
            "enabled": bool(record.enabled),
            "loaded": bool(record.loaded),
            "active": bool(record.active),
            "language": str(record.language or ""),
        }
        payload.update(extra)
        try:
            self.event_bus.publish(
                "plugin_lifecycle",
                payload,
                source_name="plugin_manager",
                source_kind="platform",
            )
        except Exception:
            pass
        self._publish_plugin_ui_invalidate(
            record.plugin_id, "", f"plugin_{payload['action']}")

    def _publish_plugin_ui_invalidate(self, plugin_id: str, surface: str = "",
                                      reason: str = "") -> None:
        try:
            self.event_bus.publish(
                "plugin_ui_invalidate",
                {
                    "plugin_id": str(plugin_id or ""),
                    "surface": str(surface or ""),
                    "reason": str(reason or ""),
                },
                source_name="plugin_manager",
                source_kind="platform",
            )
        except Exception:
            pass

    def apply_render_hooks(self, surface: str, payload: Optional[Mapping[str, Any]] = None) -> dict[str, Any]:
        def _on_error(plugin_id: str, exc: BaseException) -> None:
            self._record_failure(plugin_id, exc)
        return self.render_registry.apply(surface, dict(payload or {}), on_error=_on_error)

    def surface_overlays(self, surface: str) -> list[dict[str, Any]]:
        return self.render_registry.overlays(surface)

    def render_status(self) -> dict[str, Any]:
        return self.render_registry.status()

    def status(self) -> dict[str, Any]:
        locale_id = self.current_locale()
        capabilities: dict[str, list[str]] = {}
        for record in self._records.values():
            for cap in record.localized_capabilities(locale_id):
                cap_id = str(cap.get("id") or "")
                if cap_id:
                    capabilities.setdefault(cap_id, []).append(record.plugin_id)
        for cap_id in list(capabilities):
            capabilities[cap_id] = sorted(set(capabilities[cap_id]))
        try:
            from .scripting import list_runtimes
            script_runtimes = list_runtimes()
        except Exception:
            script_runtimes = {}
        return {
            "ok": True,
            "locale": locale_id,
            "plugin_count": len(self._records),
            "active_count": sum(1 for record in self._records.values() if record.active),
            "plugins": self.list_plugins(locale_id),
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
            "script_runtimes": script_runtimes,
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
        key_text = str(key or "")
        safe_value = _json_safe(value)
        if self.settings is not None and hasattr(self.settings, "get") and hasattr(self.settings, "set"):
            raw = self.settings.get("act_plugin_settings", {}) or {}
            if not isinstance(raw, dict):
                raw = {}
            plug = raw.get(plugin_id) if isinstance(raw.get(plugin_id), dict) else {}
            plug[key_text] = safe_value
            raw[plugin_id] = plug
            self.settings.set("act_plugin_settings", raw)
            save = getattr(self.settings, "save", None)
            if callable(save):
                try:
                    save()
                except Exception:
                    pass
            return
        self._settings_cache.setdefault(plugin_id, {})[key_text] = safe_value

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
        # Drop plugin-contributed menu categories.
        stale_cats = [k for k, v in self._menu_categories.items() if str(v.get("plugin_id") or "") == str(plugin_id or "")]
        for k in stale_cats:
            self._menu_categories.pop(k, None)
        # Drop plugin-contributed menu surfaces and action dispatchers.
        stale_surfaces = [k for k, v in self._menu_surfaces.items() if str(v.get("plugin_id") or "") == str(plugin_id or "")]
        for k in stale_surfaces:
            self._menu_surfaces.pop(k, None)
        self._action_handlers.pop(str(plugin_id or ""), None)
        # Drop plugin-contributed engines.
        stale_eng = [k for k, pid in self._plugin_engine_owners.items() if pid == str(plugin_id or "")]
        for k in stale_eng:
            self._plugin_engines.pop(k, None)
            self._plugin_engine_owners.pop(k, None)
        # Stop and drop plugin-contributed data sources.
        stale_ds = [k for k, v in self._data_sources.items() if str(v.get("plugin_id") or "") == str(plugin_id or "")]
        for k in stale_ds:
            ds = self._data_sources.pop(k, {})
            stop_fn = ds.get("stop")
            if callable(stop_fn):
                try:
                    stop_fn()
                except Exception:
                    pass
        if record is not None:
            # Remove the plugin's main module AND all submodules from
            # sys.modules so .pyd/.dll handles are released and the plugin
            # directory can be deleted or replaced on Windows.
            main_mod = f"act_plugin_{record.plugin_id.replace('-', '_').replace('.', '_')}"
            prefix_dot = main_mod + "."
            stale_mods = [
                name for name in list(sys.modules)
                if name == main_mod or name.startswith(prefix_dot)
            ]
            for mod_name in stale_mods:
                sys.modules.pop(mod_name, None)
            # Remove modules the plugin loaded via ctx.load_local (no sys.modules leak).
            for mod_name in list(record.loaded_local_modules):
                sys.modules.pop(mod_name, None)
            record.loaded_local_modules.clear()
            # Restore sys.path: drop the plugin's own vendor/libs (+ensure_requirements)
            # entries so a disabled plugin's bundled deps stop shadowing globals.
            for path in list(record.added_sys_paths):
                try:
                    while path in sys.path:
                        sys.path.remove(path)
                except Exception:
                    pass
            record.added_sys_paths.clear()
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
        from .scripting import detect_language
        language = detect_language(entry, str(manifest.get("language") or ""))
        locales = _normalize_manifest_locales(
            manifest.get("locales")
            or manifest.get("i18n")
            or manifest.get("translations")
        )
        return PluginRecord(
            plugin_id=plugin_id,
            name=str(manifest.get("name") or plugin_id),
            description=str(manifest.get("description") or ""),
            version=str(manifest.get("version") or "0.1.0"),
            path=plug_dir,
            entry=entry,
            enabled=bool(manifest.get("enabled", False)),
            language=language,
            game_ids=tuple(str(x) for x in manifest.get("game_ids", [])),
            requires=_normalize_requires(manifest.get("requires", [])),
            permissions=tuple(str(x) for x in manifest.get("permissions", [])),
            capabilities=_normalize_capabilities(manifest.get("capabilities", [])),
            settings_schema=manifest.get("settings_schema") if isinstance(manifest.get("settings_schema"), dict) else {},
            sao_menu=_normalize_sao_menu(manifest.get("sao_menu")),
            locales=locales,
        )

    def _prepare_plugin_sys_path(self, record: PluginRecord) -> None:
        """Prepend the plugin's own ``vendor/`` + ``libs/`` so bundled deps win.

        Lets an author ship a pure-Python dependency in ``vendor/`` (or fetched
        into ``libs/`` via ``ctx.ensure_requirements``) and just ``import`` it —
        no global install. Added entries are tracked on the record and removed on
        unload (see :meth:`_unregister_plugin_extensions`).

        Packages already loaded in the host (e.g. PIL, numpy) are protected:
        if the plugin ships a copy of such a package, the loader would create
        a second, incompatible module object.  The insertion index is chosen
        so that sys.path entries that already provide those packages stay
        ahead.
        """
        from act_platform.plugin_deps import _safe_plugin_insert_index
        for sub in ("vendor", "libs"):
            path = os.path.abspath(os.path.join(record.path, sub))
            if not os.path.isdir(path):
                continue
            while path in sys.path:
                sys.path.remove(path)
            idx = _safe_plugin_insert_index(path)
            sys.path.insert(idx, path)
            if path not in record.added_sys_paths:
                record.added_sys_paths.append(path)

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

    def _load_script_module(self, record: PluginRecord) -> ModuleType:
        """Load a non-Python plugin via its language's ScriptRuntime."""
        from .scripting import get_runtime
        runtime = get_runtime(record.language)
        entry_path = os.path.abspath(os.path.join(record.path, record.entry))
        record.script_runtime_active = True
        module = runtime.load_script(entry_path, record, record.context)
        return module

    def _unload_script_runtime(self, record: PluginRecord) -> None:
        """Release a script runtime even after a partial or failed load."""
        language = str(record.language or "").lower()
        if language == "python":
            return
        if not bool(getattr(record, "script_runtime_active", False)):
            return
        try:
            from .scripting import get_cached_runtime
            runtime = get_cached_runtime(record.language)
            unload = getattr(runtime, "unload_script", None)
            if callable(unload):
                unload(record)
        except Exception:
            pass
        finally:
            record.script_runtime_active = False
            if not any(
                other is not record
                and str(getattr(other, "language", "") or "").lower() == language
                and bool(getattr(other, "script_runtime_active", False))
                for other in self._records.values()
            ):
                try:
                    from .scripting import release_runtime
                    release_runtime(language)
                except Exception:
                    pass

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
        record.last_error = _safe_exception_text(exc)
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
