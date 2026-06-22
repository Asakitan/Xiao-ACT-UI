# -*- coding: utf-8 -*-
"""Helpers for wiring ACT platform services into both UI runtimes."""

from __future__ import annotations

import os
import sys
import json
import time
from typing import Any, Callable, Iterable, Mapping, Optional

def _noop_agg(*a, **kw):
    return {}

aggregate_by_actor = _noop_agg
aggregate_by_field = _noop_agg
aggregate_by_topic = _noop_agg
aggregate_damage_by_monster = _noop_agg
aggregate_damage_by_skill = _noop_agg
build_act_aggregate_summary = _noop_agg

try:
    from plugins.star_resonance_plugin.engines.act_aggregate import (
        aggregate_by_actor,
        aggregate_by_field,
        aggregate_by_topic,
        aggregate_damage_by_monster,
        aggregate_damage_by_skill,
        build_act_aggregate_summary,
    )
except Exception:
    pass


# 聚合工作台维度：插件开发者可在界面上切换"按什么聚合"（改聚合规则）。
ACT_AGGREGATE_DIMENSIONS = [
    {"id": "skill", "label": "技能"},
    {"id": "monster", "label": "怪物 / 目标"},
    {"id": "actor", "label": "参与者"},
    {"id": "topic", "label": "事件类型"},
    {"id": "field", "label": "自定义字段"},
]


def _aggregate_dimension(summary: Mapping[str, Any], rows: list[dict[str, Any]],
                         group_by: str, group_field: str, top_n: int) -> list[dict[str, Any]]:
    """Active workbench dimension. Reuses the summary's already-computed
    skill/monster folds; computes actor/topic/custom-field fresh."""
    group_by = str(group_by or "skill").strip().lower()
    if group_by == "monster":
        return list(summary.get("monster_damage") or [])
    if group_by == "actor":
        return aggregate_by_actor(rows, top_n=top_n)
    if group_by == "topic":
        return aggregate_by_topic(rows, top_n=top_n)
    if group_by == "field":
        return aggregate_by_field(rows, group_field, top_n=top_n)
    return list(summary.get("skill_damage") or [])

from . import native_dialog, plugin_deps, plugin_install
from .event_bus import EventBus
from .plugins import PluginManager
from .selective_parsing import normalize_policy, should_record_event


def default_plugin_dirs(base_dir: str) -> list[str]:
    base_dir = os.path.abspath(str(base_dir or ""))
    current_plugins = os.path.abspath(os.path.join(base_dir, "plugins"))
    current_user_plugins = os.path.abspath(os.path.join(base_dir, "user_plugins"))
    root_plugins = _discover_workspace_root_plugins_dir(base_dir)

    out: list[str] = []
    seen: set[str] = set()
    for path in (current_plugins, current_user_plugins, root_plugins):
        if not path:
            continue
        norm = os.path.abspath(path)
        if norm in seen:
            continue
        if os.path.isdir(norm) or norm == root_plugins:
            out.append(norm)
            seen.add(norm)
    return out


def _discover_workspace_root_plugins_dir(base_dir: str) -> str | None:
    """Best-effort upward scan for a workspace-root ``plugins`` directory.

    Keeps the existing ``python/plugins`` + ``python/user_plugins`` roots, then
    adds one extra root-level ``plugins`` directory when the current base dir
    lives inside a larger workspace checkout (e.g. ``E:/VC/SAO-UI/plugins``).
    Missing intermediate parents are ignored; a missing workspace-root plugins
    dir is still returned when the parent clearly looks like the workspace root
    so future drop-in script plugins are discovered without changing code.
    """
    current_plugin_dir = os.path.abspath(os.path.join(base_dir, "plugins"))
    current_user_dir = os.path.abspath(os.path.join(base_dir, "user_plugins"))
    workspace_candidate: str | None = None
    current = os.path.abspath(str(base_dir or ""))

    while current:
        candidate = os.path.abspath(os.path.join(current, "plugins"))
        if candidate not in (current_plugin_dir, current_user_dir):
            if os.path.isdir(candidate):
                return candidate
            if workspace_candidate is None and _looks_like_workspace_root(current):
                workspace_candidate = candidate
        parent = os.path.dirname(current)
        if not parent or parent == current:
            break
        current = parent
    return workspace_candidate


def _looks_like_workspace_root(path: str) -> bool:
    markers = (
        "sao_auto",
        ".git",
        ".github",
        ".vscode",
        "tools",
    )
    try:
        return any(os.path.exists(os.path.join(path, marker)) for marker in markers)
    except Exception:
        return False


def project_base_dir() -> str:
    """插件根目录解析 (打包友好 / onedir-aware)。

    冻结 onedir 下本模块在 ``<exe>/runtime/act_platform/runtime.pyc``, 但 plugins/ 与
    web/assets/proto 一样由 build_release.bat 提升到 exe 顶层 (= ``config.BASE_DIR``)。
    把根目录解析到 BASE_DIR 而非 ``__file__`` 有两个好处:
      1. 用户把自带插件放顶层 ``plugins/`` 或 ``user_plugins/`` 不会被更新覆盖
         (runtime/ 树每次更新都会被整体重写);
      2. 与 config.resource_path / 名字表读取器的 BASE_DIR-first 约定一致。
    非冻结(dev 树)回退到 ``__file__`` 相对 = ``sao_auto`` 项目根。
    """
    if getattr(sys, "frozen", False):
        try:
            from config import BASE_DIR  # = dirname(sys.executable), exe 顶层
            if BASE_DIR:
                return BASE_DIR
        except Exception:
            pass
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def ensure_act_event_bus(owner: Any) -> EventBus:
    bus = getattr(owner, "_act_event_bus", None)
    if isinstance(bus, EventBus):
        return bus
    # Retain a full encounter worth of live events, not just the EventBus
    # default of 200. During a busy boss fight all topics (damage/heal/skill/
    # monster/boss/dungeon) share this one bus, so 200 events roll over in a
    # fraction of a second and every span/elapsed/DPS number derived from the
    # retained slice collapses to "几百毫秒". 4000 covers a typical fight while
    # recent_events() still only clones the last `limit` rows per read.
    try:
        max_recent = int(_settings_get(owner, "act_event_bus_max_recent", 4000) or 4000)
    except Exception:
        max_recent = 4000
    # act_snapshot is a combat-rate live-overlay payload that is always filtered
    # out of the action-log / aggregate views (_HIDDEN_ACTION_TOPICS), so retain
    # it for subscribers but keep it out of the deep-cloned _recent ring.
    bus = EventBus(max_recent=max(200, max_recent), ephemeral_topics={"act_snapshot"})
    try:
        setattr(owner, "_act_event_bus", bus)
    except Exception:
        pass
    return bus


def _owner_settings(owner: Any) -> Any:
    return getattr(owner, "_cfg_settings_ref", None) or getattr(owner, "settings", None)


def _settings_get(owner: Any, key: str, default: Any = None) -> Any:
    settings = _owner_settings(owner)
    try:
        if isinstance(settings, Mapping):
            return settings.get(key, default)
        if settings is not None:
            return settings.get(key, default)
    except Exception:
        pass
    return default


def _settings_set(owner: Any, key: str, value: Any) -> None:
    settings = _owner_settings(owner)
    if settings is None:
        try:
            setattr(owner, key, value)
        except Exception:
            pass
        return
    try:
        if hasattr(settings, "set"):
            settings.set(key, value)
        elif isinstance(settings, dict):
            settings[key] = value
    except Exception:
        pass
    try:
        save = getattr(settings, "save", None)
        if callable(save):
            save()
    except Exception:
        pass


def _dispatch_trigger_action(owner: Any, name: str, *args: Any, **kwargs: Any) -> Any:
    """Forward a game-specific trigger action to the plugin's runtime bridge.

    The platform deliberately owns no trigger engine logic — every
    ``act_trigger_*`` function dispatches to the
    plugin-contributed handler registered via ``register_extension_runtime``.
    If no game plugin is loaded, returns a generic "unavailable" payload so
    upstream UIs degrade gracefully.
    """
    handler = _extension_runtime_handler(name)
    if callable(handler):
        try:
            return handler(owner, *args, **kwargs)
        except Exception as exc:
            return {"ok": False, "message": str(exc), "errors": [str(exc)]}
    return {"ok": False,
            "message": f"trigger action is unavailable: no game plugin loaded (action={name})",
            "errors": [f"trigger action unavailable: {name}"]}


def act_trigger_status(owner: Any, **kwargs: Any) -> dict[str, Any]:
    return _dispatch_trigger_action(owner, "act_trigger_status", **kwargs)


def act_trigger_enable(owner: Any, rule_id: str, **kwargs: Any) -> dict[str, Any]:
    return _dispatch_trigger_action(owner, "act_trigger_enable", rule_id, **kwargs)


def act_trigger_disable(owner: Any, rule_id: str, **kwargs: Any) -> dict[str, Any]:
    return _dispatch_trigger_action(owner, "act_trigger_disable", rule_id, **kwargs)


def act_trigger_reload(owner: Any, **kwargs: Any) -> dict[str, Any]:
    return _dispatch_trigger_action(owner, "act_trigger_reload", **kwargs)


def act_trigger_export_presets(owner: Any, **kwargs: Any) -> dict[str, Any]:
    return _dispatch_trigger_action(owner, "act_trigger_export_presets", **kwargs)


def act_trigger_import_presets(owner: Any, payload: Any, **kwargs: Any) -> dict[str, Any]:
    return _dispatch_trigger_action(owner, "act_trigger_import_presets", payload, **kwargs)


def act_trigger_test(owner: Any, rule_id: str, **kwargs: Any) -> dict[str, Any]:
    return _dispatch_trigger_action(owner, "act_trigger_test", rule_id, **kwargs)


_EXTENSION_RUNTIME_PROVIDER: Callable[[str], Callable[..., Any] | None] | None = None


def register_extension_runtime(provider: Callable[[str], Callable[..., Any] | None]) -> None:
    """Inject an extension-runtime dispatch provider contributed by a plugin.

    Plugins call this from ``on_load`` to expose
    their trigger-engine / DPS-history / name-resolver handlers to the platform.

    The platform never imports plugin code; it only invokes the provider to
    look up a named handler and then calls it with ``(owner, ...)``.
    """
    global _EXTENSION_RUNTIME_PROVIDER
    if callable(provider):
        _EXTENSION_RUNTIME_PROVIDER = provider


def register_webview_extension(owner: Any, extension: Any) -> Any:
    """Attach a plugin-contributed WebView extension to the current host owner.

    The platform stores the extension behind a generic owner attribute and
    dispatches by method name without importing plugin modules.
    """
    if owner is not None:
        try:
            setattr(owner, "_webview_extension", extension)
        except Exception:
            pass
    return extension


def _extension_runtime_handler(name: str) -> Callable[..., Any] | None:
    """Return the plugin-contributed handler for ``name`` or ``None``."""
    provider = _EXTENSION_RUNTIME_PROVIDER
    if not callable(provider):
        return None
    try:
        handler = provider(str(name or ""))
    except Exception:
        return None
    return handler if callable(handler) else None


def _extension_runtime_value(owner: Any, name: str, default: Any = None,
                             *args: Any, **kwargs: Any) -> Any:
    """Call a plugin-contributed value provider without importing plugin code."""
    handler = _extension_runtime_handler(name)
    if not callable(handler):
        return default
    try:
        value = handler(owner, *args, **kwargs)
    except Exception:
        return default
    return default if value is None else value


# Note: plugins own their runtime engine creation and expose it via the
# extension runtime provider. Platform code that needs an engine goes
# through ``act_trigger_*`` dispatch functions only.


def _owner_snapshot_provider(owner: Any) -> Callable[[], Mapping[str, Any]]:
    def _snapshot() -> Mapping[str, Any]:
        snap = _extension_runtime_value(owner, "owner_act_snapshot", {})
        if isinstance(snap, Mapping):
            return snap
        return {}

    return _snapshot


def build_plugin_manager(
    *,
    base_dir: str,
    event_bus: Optional[EventBus] = None,
    snapshot_provider: Optional[Callable[[], Mapping[str, Any]]] = None,
    owner_provider: Optional[Callable[[], Any]] = None,
    settings: Any = None,
    plugin_dirs: Optional[Iterable[str]] = None,
) -> PluginManager:
    manager = PluginManager(
        plugin_dirs=list(plugin_dirs or default_plugin_dirs(base_dir)),
        event_bus=event_bus or EventBus(),
        snapshot_provider=snapshot_provider,
        owner_provider=owner_provider,
        settings=settings,
        user_plugin_dirs=[os.path.join(base_dir, "user_plugins")],
    )
    manager.discover()
    return manager


def _user_plugins_dir() -> str:
    """The update-safe, writable dir where one-click-imported plugins live."""
    return os.path.join(project_base_dir(), "user_plugins")


def ensure_act_plugin_manager(owner: Any, *, load: bool = False,
                              plugin_dirs: Optional[Iterable[str]] = None) -> PluginManager:
    bus = ensure_act_event_bus(owner)
    manager = getattr(owner, "_act_plugin_manager", None)
    if not isinstance(manager, PluginManager):
        manager = build_plugin_manager(
            base_dir=project_base_dir(),
            event_bus=bus,
            snapshot_provider=_owner_snapshot_provider(owner),
            owner_provider=lambda owner=owner: owner,
            settings=_owner_settings(owner),
            plugin_dirs=plugin_dirs,
        )
        try:
            setattr(owner, "_act_plugin_manager", manager)
        except Exception:
            pass
    else:
        manager.event_bus = bus
        manager.snapshot_provider = _owner_snapshot_provider(owner)
        manager.owner_provider = lambda owner=owner: owner
        manager.settings = _owner_settings(owner)
        if plugin_dirs is not None:
            manager.plugin_dirs = [os.path.abspath(path) for path in plugin_dirs if path]
            manager.discover()
    # Idempotent: only load on first request. Without this guard every UI poll
    # (act_plugin_ui_render/menu pass load=True) would unload+reload every
    # plugin, resetting state and killing engines a stateful plugin owns.
    if load and not getattr(manager, "_initial_loaded", False):
        manager.load_all()
    return manager


def shutdown_act_plugin_manager(owner: Any) -> None:
    manager = getattr(owner, "_act_plugin_manager", None)
    if not isinstance(manager, PluginManager):
        return
    for plugin in list(manager.list_plugins()):
        try:
            manager.unload_plugin(plugin.get("id"))
        except Exception:
            pass


def publish_owner_event(owner: Any, topic: str, payload: Optional[Mapping[str, Any]] = None,
                        *, source_name: str, source_kind: str) -> dict[str, Any] | None:
    try:
        return ensure_act_event_bus(owner).publish(
            topic,
            payload or {},
            source_name=source_name,
            source_kind=source_kind,
        )
    except Exception:
        return None


def _self_uid(owner: Any) -> int:
    try:
        value = int(_extension_runtime_value(owner, "self_uid", 0) or 0)
        if value > 0:
            return value
    except Exception:
        pass
    return 0


def _party_ids(owner: Any) -> set[int]:
    raw = _extension_runtime_value(owner, "party_ids", ())
    ids: set[int] = set()
    for item in (raw or ()):
        try:
            ids.add(int(item))
        except Exception:
            pass
    self_uid = _self_uid(owner)
    if self_uid > 0:
        ids.add(self_uid)
    return {uid for uid in ids if uid > 0}


def _read_selective_policy(owner: Any) -> dict[str, Any]:
    raw = _settings_get(owner, "act_selective_parsing", None)
    if raw is None:
        raw = getattr(owner, "act_selective_parsing", None)
    return normalize_policy(raw)


def should_record_owner_combat_event(owner: Any, event: Mapping[str, Any]) -> dict[str, Any]:
    """Return a selective-parsing decision for a live/replay combat event."""
    policy = _read_selective_policy(owner)
    if not policy.get("enabled"):
        result = {"record": True, "reason": "record_all_default", "policy": policy}
        try:
            setattr(owner, "_act_selective_last_decision", result)
        except Exception:
            pass
        return result
    decision = should_record_event(
        event,
        policy,
        self_uid=_self_uid(owner),
        party_ids=_party_ids(owner),
    )
    try:
        setattr(owner, "_act_selective_last_decision", decision.to_dict())
    except Exception:
        pass
    return decision.to_dict()


def act_selective_parsing_status(owner: Any) -> dict[str, Any]:
    policy = _read_selective_policy(owner)
    last = getattr(owner, "_act_selective_last_decision", {})
    return {
        "ok": True,
        "message": "OK",
        "enabled": bool(policy.get("enabled")),
        "mode": str(policy.get("mode") or "all"),
        "policy": policy,
        "filters": {
            "include_ids": list(policy.get("include_ids") or []),
            "include_names": list(policy.get("include_names") or []),
            "exclude_ids": list(policy.get("exclude_ids") or []),
            "exclude_names": list(policy.get("exclude_names") or []),
            "source_kinds": list(policy.get("source_kinds") or []),
            "topics": list(policy.get("topics") or []),
        },
        "self_uid": _self_uid(owner),
        "party_ids": sorted(_party_ids(owner)),
        "last_decision": dict(last) if isinstance(last, Mapping) else {},
        "errors": [],
    }


def act_selective_parsing_update(owner: Any, policy: Mapping[str, Any] | None = None, **updates: Any) -> dict[str, Any]:
    merged = dict(_read_selective_policy(owner))
    if isinstance(policy, Mapping):
        merged.update(dict(policy))
    merged.update({key: value for key, value in updates.items() if value is not None})
    normalized = normalize_policy(merged)
    _settings_set(owner, "act_selective_parsing", normalized)
    return act_selective_parsing_status(owner)


def act_selective_parsing_clear(owner: Any) -> dict[str, Any]:
    normalized = normalize_policy({"enabled": False, "mode": "all"})
    _settings_set(owner, "act_selective_parsing", normalized)
    return act_selective_parsing_status(owner)


def act_plugin_status(owner: Any) -> dict[str, Any]:
    try:
        return ensure_act_plugin_manager(owner).status()
    except Exception as exc:
        return {"ok": False, "message": str(exc), "plugins": []}


def act_plugin_list(owner: Any) -> dict[str, Any]:
    status = act_plugin_status(owner)
    return {"ok": bool(status.get("ok", False)), "plugins": status.get("plugins", [])}


def act_plugin_enable(owner: Any, plugin_id: str) -> dict[str, Any]:
    try:
        manager = ensure_act_plugin_manager(owner)
        ok = manager.enable_plugin(plugin_id)
        return {"ok": bool(ok), "status": manager.status()}
    except Exception as exc:
        return {"ok": False, "message": str(exc)}


def act_plugin_disable(owner: Any, plugin_id: str) -> dict[str, Any]:
    try:
        manager = ensure_act_plugin_manager(owner)
        ok = manager.disable_plugin(plugin_id)
        return {"ok": bool(ok), "status": manager.status()}
    except Exception as exc:
        return {"ok": False, "message": str(exc)}


def act_plugin_reload(owner: Any, plugin_id: str = "") -> dict[str, Any]:
    try:
        manager = ensure_act_plugin_manager(owner)
        if plugin_id:
            ok = manager.reload_plugin(plugin_id)
            return {"ok": bool(ok), "status": manager.status()}
        return manager.reload_all()
    except Exception as exc:
        return {"ok": False, "message": str(exc)}


def act_plugin_import(owner: Any, archive_path: str, *, enable: bool = True) -> dict[str, Any]:
    """一键导入：把一个 ``.zip`` 插件包装进 ``user_plugins/`` 并（默认）启用即用。

    纯 Python 插件无需编译——加载器在 dev 与 onedir 冻结态都直接运行时 import 原始
    ``.py``。带原生扩展(.pyd)或第三方依赖的插件需由**作者**预编译 / vendor 进包。
    流程：安装(防穿越解压→user_plugins/<id>) → 依赖引导 → 单插件 refresh(不动其他
    有状态插件) → 启用加载 → 回状态。
    """
    archive_path = str(archive_path or "").strip()
    if not archive_path:
        return {"ok": False, "message": "未提供插件包路径", "errors": ["no path"]}
    try:
        manager = ensure_act_plugin_manager(owner, load=False)
    except Exception as exc:
        return {"ok": False, "message": str(exc), "errors": [str(exc)]}

    result = plugin_install.install_plugin_archive(archive_path, _user_plugins_dir())
    if not result.get("ok"):
        return result

    plugin_id = str(result.get("id") or "")
    installed_path = str(result.get("path") or "")

    # 首次加载前满足插件声明的依赖(dev: pip→libs；冻结: 仅 vendor/libs)。尽力而为，
    # 依赖缺失只提示不阻断导入(插件可能本就不需要、或主程序已带)。
    deps: dict[str, Any] = {}
    try:
        deps = plugin_deps.ensure_requirements(installed_path, install=True).get("deps") or {}
    except Exception as exc:
        deps = {"_error": str(exc)}

    loaded = False
    load_error = ""
    try:
        manager.refresh_plugin(installed_path)
        if enable:
            manager.enable_plugin(plugin_id)
            for plug in manager.list_plugins():
                if str(plug.get("id")) == plugin_id:
                    loaded = bool(plug.get("active"))
                    load_error = str(plug.get("last_error") or "")
                    break
    except Exception as exc:
        load_error = str(exc)

    try:
        status = manager.status()
    except Exception:
        status = {}

    missing = [k for k, v in deps.items() if v == "missing"] if isinstance(deps, dict) else []
    ok = bool(result.get("ok") and (loaded or not enable) and not load_error)
    message = str(result.get("message") or "")
    if load_error:
        message = f"已导入但加载失败: {load_error}"
    elif missing:
        message = (message + f"  ⚠ 缺少依赖 {', '.join(missing)}(需作者 vendor 进包)").strip()
    return {
        "ok": ok,
        "id": plugin_id,
        "name": result.get("name"),
        "version": result.get("version"),
        "path": installed_path,
        "replaced": bool(result.get("replaced")),
        "enabled": bool(enable),
        "loaded": loaded,
        "deps": deps,
        "load_error": load_error,
        "message": message,
        "status": status,
        "errors": [load_error] if load_error else [],
    }


def act_plugin_import_dialog(owner: Any) -> dict[str, Any]:
    """弹原生文件选择器选 ``.zip`` 插件包并导入(Entity / WebView 两端均可)。"""
    try:
        path = native_dialog.open_plugin_archive(initial_dir=_user_plugins_dir())
    except Exception as exc:
        return {"ok": False, "message": str(exc), "errors": [str(exc)]}
    if not path:
        return {"ok": False, "cancelled": True, "message": "已取消导入", "errors": []}
    return act_plugin_import(owner, path)


def act_open_workshop(owner: Any) -> dict[str, Any]:
    try:
        from workshop.app import launch as ws_launch
        ws_launch(gui_ref=owner)
        return {"ok": True}
    except Exception as exc:
        return {"ok": False, "error": str(exc)}


def act_plugin_uninstall(owner: Any, plugin_id: str) -> dict[str, Any]:
    """卸载一个**用户安装**的插件(卸载并删除其 user_plugins 目录)。内置插件不可删。"""
    plugin_id = str(plugin_id or "").strip()
    if not plugin_id:
        return {"ok": False, "message": "未提供插件 id", "errors": ["no id"]}
    try:
        manager = ensure_act_plugin_manager(owner, load=False)
    except Exception as exc:
        return {"ok": False, "message": str(exc), "errors": [str(exc)]}
    if not manager.is_user_plugin(plugin_id):
        return {"ok": False, "id": plugin_id, "message": "内置插件不可卸载(只能禁用)",
                "errors": ["builtin plugin not removable"]}
    path = ""
    for plug in manager.list_plugins():
        if str(plug.get("id")) == plugin_id:
            path = str(plug.get("path") or "")
            break
    manager.forget_plugin(plugin_id)  # unload + drop record (releases subs/timers)
    result = plugin_install.remove_installed_plugin(path, _user_plugins_dir())
    if not result.get("ok"):
        return result
    try:
        manager.clear_persisted_enabled(plugin_id)
    except Exception:
        pass
    try:
        status = manager.status()
    except Exception:
        status = {}
    return {"ok": True, "id": plugin_id, "message": result.get("message") or "已卸载插件",
            "status": status, "errors": []}


_SCRIPT_MENU_LANGUAGES = {"lua", "csharp", "angelscript", "emma"}
_SCRIPT_MENU_ACTION = "script.overlay.set_enabled"
_SCRIPT_MENU_SETTING = "overlay_enabled"


def _bool_value(value: Any, default: bool = False) -> bool:
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


def _record_setting_default(manager: PluginManager, plugin_id: str,
                            key: str, default: bool = False) -> bool:
    records = getattr(manager, "_records", {})
    record = records.get(str(plugin_id or "")) if isinstance(records, Mapping) else None
    schema = getattr(record, "settings_schema", {}) if record is not None else {}
    if isinstance(schema, Mapping):
        item = schema.get(str(key or ""))
        if isinstance(item, Mapping) and "default" in item:
            return _bool_value(item.get("default"), default)
    return bool(default)


def _script_menu_summary(manager: PluginManager,
                         plug: Mapping[str, Any]) -> dict[str, Any]:
    pid = str(plug.get("id") or "")
    language = str(plug.get("language") or "").lower()
    raw_menu = plug.get("sao_menu")
    sao_menu = raw_menu if isinstance(raw_menu, Mapping) else {}
    if not pid or language not in _SCRIPT_MENU_LANGUAGES or not sao_menu:
        return {}
    if not bool(plug.get("enabled")):
        return {}

    title = str(
        sao_menu.get("name")
        or sao_menu.get("title")
        or sao_menu.get("label")
        or sao_menu.get("script_label")
        or plug.get("label")
        or pid
    ).strip()
    if not title:
        return {}
    icon = str(sao_menu.get("icon_text") or sao_menu.get("icon") or "▣")
    row_icon = str(sao_menu.get("row_icon") or icon or "▣")
    setting_key = str(
        sao_menu.get("setting")
        or sao_menu.get("overlay_setting")
        or _SCRIPT_MENU_SETTING
    ).strip() or _SCRIPT_MENU_SETTING
    default_enabled = _record_setting_default(
        manager,
        pid,
        setting_key,
        _bool_value(sao_menu.get("default_enabled"), False),
    )
    current_enabled = _bool_value(
        manager.get_plugin_setting(pid, setting_key, default_enabled),
        default_enabled,
    )
    try:
        priority = float(sao_menu.get("priority") or 50.0)
    except Exception:
        priority = 0.0
    return {
        "type": "script_menu",
        "id": pid,
        "plugin_id": pid,
        "plugin_label": str(plug.get("label") or pid),
        "language": language,
        "name": title,
        "label": str(sao_menu.get("script_label") or sao_menu.get("toggle_label") or sao_menu.get("label") or title),
        "icon": icon,
        "row_icon": row_icon,
        "enabled": bool(plug.get("enabled")),
        "active": bool(plug.get("active")),
        "loaded": bool(plug.get("loaded")),
        "overlay_enabled": current_enabled,
        "default_enabled": default_enabled,
        "setting": setting_key,
        "action_id": str(sao_menu.get("action_id") or _SCRIPT_MENU_ACTION),
        "surface": str(sao_menu.get("surface") or "unioverlay"),
        "priority": priority,
        "sao_menu": dict(sao_menu),
    }


def act_plugin_menu(owner: Any) -> dict[str, Any]:
    """Data for the dedicated plugin menu (Entity + WebView).

    Plugins are returned pinned-first; each item carries enable/active state,
    whether it ``declares_panel`` (manifest), its registered panel ids and its
    hotkey count so the host can build a rich, toggle-able plugin board.
    """
    try:
        manager = ensure_act_plugin_manager(owner, load=True)
    except Exception as exc:
        return {"ok": False, "message": str(exc), "items": [], "plugins": []}
    status = manager.status()
    panels_by_plugin: dict[str, list[str]] = {}
    for panel in manager.list_ui_panels():
        panels_by_plugin.setdefault(str(panel.get("plugin_id") or ""), []).append(str(panel.get("id") or ""))

    plugins: list[dict[str, Any]] = []
    script_menus: list[dict[str, Any]] = []
    for plug in status.get("plugins", []):
        pid = str(plug.get("id") or "")
        sao_menu = plug.get("sao_menu") if isinstance(plug.get("sao_menu"), Mapping) else {}
        item = {
            "type": "plugin",
            "id": pid,
            "label": str(plug.get("name") or pid),
            "language": str(plug.get("language") or "python"),
            "enabled": bool(plug.get("enabled")),
            "loaded": bool(plug.get("loaded")),
            "active": bool(plug.get("active")),
            "pinned": bool(plug.get("pinned")),
            "declares_panel": bool(plug.get("declares_panel")),
            "panels": panels_by_plugin.get(pid, []),
            "hotkey_count": int(plug.get("hotkey_count") or 0),
            "last_error": str(plug.get("last_error") or ""),
            "sao_menu": dict(sao_menu),
        }
        script_menu = _script_menu_summary(manager, item)
        if script_menu:
            item["script_menu"] = dict(script_menu)
            script_menus.append(script_menu)
        plugins.append(item)
    # Pinned first (preserving pin order), then the rest alphabetically.
    pin_order = {pid: i for i, pid in enumerate(status.get("pinned", []) or [])}
    plugins.sort(key=lambda p: (0, pin_order.get(p["id"], 0), p["label"]) if p["pinned"]
                 else (1, 0, p["label"]))

    items: list[dict[str, Any]] = [
        {"type": "manage", "id": "__manage__", "label": "插件管理面板 Manage"},
        {"type": "panels", "id": "__panels__", "label": "插件面板 Panels"},
        {"type": "reload", "id": "__reload__", "label": "重载全部插件 Reload"},
    ] + plugins
    return {
        "ok": True,
        "count": int(status.get("plugin_count", 0) or 0),
        "active_count": int(status.get("active_count", 0) or 0),
        "pinned": list(status.get("pinned", []) or []),
        "items": items,
        "plugins": plugins,
        "script_menus": sorted(script_menus, key=lambda m: (float(m.get("priority") or 0.0), str(m.get("name") or ""))),
        "hotkeys": status.get("hotkeys", []),
    }


def act_plugin_script_menus(owner: Any) -> dict[str, Any]:
    """Return script-plugin SAO popup descriptors for enabled plugins."""
    try:
        manager = ensure_act_plugin_manager(owner, load=False)
        return {"ok": True, "items": manager.list_script_menu_entries()}
    except Exception as exc:
        return {"ok": False, "message": str(exc), "items": []}


def act_plugin_menu_surfaces(owner: Any, surface_id: str = "") -> dict[str, Any]:
    """Return active plugin-owned descriptors for a generic menu surface."""
    try:
        manager = ensure_act_plugin_manager(owner, load=True)
        return {"ok": True, "surfaces": manager.get_menu_surfaces(str(surface_id or ""))}
    except Exception as exc:
        return {"ok": False, "message": str(exc), "surfaces": []}


def act_plugin_action(owner: Any, action_id: str, payload: Any = None,
                      plugin_id: str = "") -> dict[str, Any]:
    """Dispatch an opaque plugin action without platform-side action knowledge."""
    try:
        manager = ensure_act_plugin_manager(owner, load=True)
        return manager.dispatch_plugin_action(
            str(action_id or ""), _coerce_payload(payload), str(plugin_id or ""))
    except Exception as exc:
        return {"ok": False, "action_id": str(action_id or ""), "message": str(exc), "errors": [str(exc)]}


def act_plugin_pin(owner: Any, plugin_id: str, pinned: bool = True) -> dict[str, Any]:
    """Pin/unpin a plugin so it is promoted to the top of the plugin menu."""
    try:
        manager = ensure_act_plugin_manager(owner)
        return {"ok": True, "pinned": manager.set_pinned(str(plugin_id or ""), bool(pinned))}
    except Exception as exc:
        return {"ok": False, "message": str(exc)}


def act_plugin_hotkeys(owner: Any) -> dict[str, Any]:
    """List plugin-registered hotkeys (for the keybinding editor + menu).

    ``occupied`` 给改键 UI 置灰用: {键: 归属} 含内置动作现值与 active
    插件键现值 (调用方需把动作自身的现值豁免)。
    """
    try:
        manager = ensure_act_plugin_manager(owner, load=True)
        return {"ok": True, "hotkeys": manager.list_hotkeys(),
                "occupied": manager.occupied_hotkeys()}
    except Exception as exc:
        return {"ok": False, "message": str(exc), "hotkeys": [], "occupied": {}}


def act_plugin_hotkey_dispatch(owner: Any, action: str) -> dict[str, Any]:
    """Fire a plugin hotkey action by id (host hotkey-manager entry point)."""
    try:
        manager = ensure_act_plugin_manager(owner)
        return {"ok": bool(manager.dispatch_hotkey(str(action or "")))}
    except Exception as exc:
        return {"ok": False, "message": str(exc)}


def act_plugin_set_hotkey(owner: Any, action: str, key: str = "") -> dict[str, Any]:
    """Rebind a plugin hotkey (shared settings['hotkeys']; '' clears to default)."""
    try:
        manager = ensure_act_plugin_manager(owner)
        ok = manager.set_hotkey(str(action or ""), str(key or ""))
        return {"ok": bool(ok), "hotkeys": manager.list_hotkeys()}
    except Exception as exc:
        return {"ok": False, "message": str(exc), "hotkeys": []}


# ── Plugin UI panels (redrawable declarative panels) ──────────────────────────

def _coerce_payload(payload: Any) -> dict[str, Any]:
    if isinstance(payload, Mapping):
        return dict(payload)
    if isinstance(payload, str) and payload.strip():
        try:
            decoded = json.loads(payload)
        except Exception:
            return {}
        return decoded if isinstance(decoded, dict) else {}
    return {}


def act_plugin_ui_panels(owner: Any) -> dict[str, Any]:
    """List redrawable plugin UI panels registered by active plugins."""
    try:
        manager = ensure_act_plugin_manager(owner, load=True)
        return {"ok": True, "panels": manager.list_ui_panels()}
    except Exception as exc:
        return {"ok": False, "message": str(exc), "panels": []}


def act_plugin_ui_render(owner: Any, panel_id: str, payload: Any = None) -> dict[str, Any]:
    """Render a single plugin UI panel to a normalized spec for the host."""
    try:
        manager = ensure_act_plugin_manager(owner, load=True)
        return manager.render_ui_panel(str(panel_id or ""), _coerce_payload(payload))
    except Exception as exc:
        return {"ok": False, "panel_id": str(panel_id or ""), "message": str(exc),
                "spec": {"version": 1, "title": "", "nodes": []}}


def act_plugin_ui_action(owner: Any, panel_id: str, action_id: str, payload: Any = None) -> dict[str, Any]:
    """Dispatch a button action from a rendered plugin UI panel."""
    try:
        manager = ensure_act_plugin_manager(owner, load=True)
        return manager.invoke_ui_action(str(panel_id or ""), str(action_id or ""), _coerce_payload(payload))
    except Exception as exc:
        return {"ok": False, "panel_id": str(panel_id or ""), "message": str(exc)}


# ── Render hooks + overlays (intercept any UI surface, both renderers) ─────────

def render_surfaces(owner: Any) -> dict[str, Any]:
    """Report which surfaces have plugin hooks/overlays attached."""
    try:
        manager = ensure_act_plugin_manager(owner, load=True)
        return {"ok": True, **manager.render_status()}
    except Exception as exc:
        return {"ok": False, "message": str(exc), "surfaces": {}}


def render_apply_hooks(owner: Any, surface: str, payload: Any = None) -> dict[str, Any]:
    """Run the plugin render-hook chain for ``surface`` over ``payload``.

    Hosts call this just before rendering: the returned ``payload`` may have
    been mutated or replaced by plugins, and ``override`` carries a full
    take-over spec when a hook set ``payload[OVERRIDE_KEY]``.
    """
    from .render_hooks import OVERRIDE_KEY
    from .ui_spec import normalize_ui_spec
    try:
        manager = ensure_act_plugin_manager(owner)
    except Exception as exc:
        return {"ok": False, "message": str(exc), "surface": str(surface or ""),
                "payload": _coerce_payload(payload), "override": None}
    if not manager.render_registry.has_hooks(surface):
        return {"ok": True, "surface": str(surface or ""),
                "payload": _coerce_payload(payload), "override": None, "hooked": False}
    result = manager.apply_render_hooks(surface, _coerce_payload(payload))
    override = None
    if isinstance(result, Mapping) and result.get(OVERRIDE_KEY) is not None:
        override = normalize_ui_spec(result.get(OVERRIDE_KEY))
        result = {k: v for k, v in result.items() if k != OVERRIDE_KEY}
    return {"ok": True, "surface": str(surface or ""), "payload": result,
            "override": override, "hooked": True}


def render_overlays(owner: Any, surface: str) -> dict[str, Any]:
    """Return plugin overlay specs for ``surface`` (drawn over native content)."""
    try:
        manager = ensure_act_plugin_manager(owner)
        return {"ok": True, "surface": str(surface or ""),
                "overlays": manager.surface_overlays(surface)}
    except Exception as exc:
        return {"ok": False, "surface": str(surface or ""), "message": str(exc), "overlays": []}


def render_surface(owner: Any, surface: str, payload: Any = None) -> dict[str, Any]:
    """One-shot helper: apply hooks *and* collect overlays for ``surface``."""
    hooked = render_apply_hooks(owner, surface, payload)
    overlays = render_overlays(owner, surface)
    return {
        "ok": bool(hooked.get("ok") and overlays.get("ok")),
        "surface": str(surface or ""),
        "payload": hooked.get("payload"),
        "override": hooked.get("override"),
        "overlays": overlays.get("overlays", []),
    }

# ``_extension_runtime_handler`` registered via ``register_extension_runtime``).


def _json_safe(value: Any) -> Any:
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    if isinstance(value, Mapping):
        return {str(key): _json_safe(val) for key, val in value.items()}
    if isinstance(value, (list, tuple, set)):
        return [_json_safe(item) for item in value]
    return str(value)


def _safe_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except Exception:
        return float(default)


def _coerce_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        return int(default)


def _owner_act_snapshot(owner: Any, *, history_limit: int = 20) -> dict[str, Any]:
    snap = _extension_runtime_value(owner, "owner_act_snapshot", {}, history_limit=history_limit)
    if isinstance(snap, Mapping):
        return dict(_json_safe(snap))
    return {}


def _owner_dps_report(owner: Any) -> dict[str, Any] | None:
    report = _extension_runtime_value(owner, "owner_dps_report", None)
    if isinstance(report, Mapping):
        return dict(_json_safe(report))
    return None


def _owner_history_store(owner: Any) -> tuple[Any, list[str]]:
    store = _extension_runtime_value(owner, "owner_history_store", None)
    if store is None:
        return None, ["DPS history is not initialized"]
    return store, []


def _history_storage_status(store: Any, encounters: list[Any] | None = None) -> dict[str, Any]:
    items = encounters if isinstance(encounters, list) else []
    archive: dict[str, Any] = {}
    archive_status = getattr(store, "archive_status", None)
    if callable(archive_status):
        try:
            archive = dict(_json_safe(archive_status() or {}))
        except Exception as exc:
            archive = {"available": False, "path": str(getattr(store, "archive_path", "") or ""), "last_error": str(exc)}
    sqlite: dict[str, Any] = {}
    sqlite_status = getattr(store, "sqlite_status", None)
    if callable(sqlite_status):
        try:
            sqlite = dict(_json_safe(sqlite_status() or {}))
        except Exception as exc:
            sqlite = {"available": False, "path": str(getattr(store, "sqlite_path", "") or ""), "last_error": str(exc)}
    return {
        "available": bool(store is not None),
        "count": len(items),
        "path": str(getattr(store, "path", "") or ""),
        "archive": archive,
        "sqlite": sqlite,
    }


def _history_load_report(store: Any, index: int = 0) -> dict[str, Any] | None:
    get_report = getattr(store, "get_report", None)
    if callable(get_report):
        report = get_report(int(index or 0))
        return dict(_json_safe(report)) if isinstance(report, Mapping) else None
    reports = getattr(store, "list_reports", None)
    if callable(reports):
        items = list(reports(max(int(index or 0) + 1, 1)) or [])
        if 0 <= int(index or 0) < len(items) and isinstance(items[int(index or 0)], Mapping):
            return dict(_json_safe(items[int(index or 0)]))
    return None


def _show_plugin_report(owner: Any, report: Mapping[str, Any]) -> bool:
    shown = _extension_runtime_value(owner, "show_dps_last_report", False, report)
    return bool(shown)


def act_history_status(owner: Any, *, limit: int = 20, query: str = "") -> dict[str, Any]:
    """Return ACT history browser payload shared by WebView and Entity/Tk."""
    store, errors = _owner_history_store(owner)
    encounters: list[Any] = []
    if store is not None:
        search_reports = getattr(store, "search_reports", None)
        list_reports = getattr(store, "list_reports", None)
        if callable(search_reports):
            try:
                encounters = list(_json_safe(search_reports(query=str(query or ""), limit=int(limit or 20)) or []))
            except Exception as exc:
                errors.append(str(exc))
        elif callable(list_reports):
            try:
                encounters = list(_json_safe(list_reports(int(limit or 20)) or []))
                for idx, item in enumerate(encounters):
                    if isinstance(item, dict):
                        item.setdefault("_history_index", idx)
            except Exception as exc:
                errors.append(str(exc))
        else:
            errors.append("DPS history list API is unavailable")
    text = str(query or "").strip().lower()
    if text and not callable(getattr(store, "search_reports", None)):
        encounters = [
            item for item in encounters
            if text in json.dumps(item, ensure_ascii=False, default=str).lower()
        ]
    return {
        "ok": bool(store is not None and not errors),
        "message": "OK" if not errors else "; ".join(errors),
        "encounters": encounters,
        "filters": {"query": str(query or ""), "limit": int(limit or 20)},
        "cursor": {"offset": 0, "limit": int(limit or 20), "has_more": False},
        "storage_status": _history_storage_status(store, encounters),
        "errors": errors,
    }


def act_history_load(owner: Any, *, index: int = 0, show: bool = True) -> dict[str, Any]:
    """Load one finalized encounter report from history and optionally show it."""
    store, errors = _owner_history_store(owner)
    if store is None:
        return {
            "ok": False,
            "message": "; ".join(errors),
            "report": None,
            "preview": {},
            "errors": errors,
        }
    try:
        report = _history_load_report(store, int(index or 0))
    except Exception as exc:
        return {"ok": False, "message": str(exc), "report": None, "preview": {}, "errors": [str(exc)]}
    if not isinstance(report, Mapping):
        return {"ok": False, "message": "History report not found", "report": None, "preview": {}, "errors": ["History report not found"]}
    shown = False
    if show:
        try:
            shown = _show_plugin_report(owner, report)
        except Exception as exc:
            errors.append(str(exc))
    preview = _report_preview({}, report)
    return {
        "ok": not errors,
        "message": "Loaded" if not errors else "; ".join(errors),
        "index": int(index or 0),
        "shown": shown,
        "report": dict(_json_safe(report)),
        "preview": preview,
        "errors": errors,
        "status": act_history_status(owner),
    }


def act_history_delete(owner: Any, *, index: int | None = None, clear: bool = False) -> dict[str, Any]:
    """Delete a finalized encounter from history, or clear history when explicit."""
    store, errors = _owner_history_store(owner)
    if store is None:
        return {"ok": False, "message": "; ".join(errors), "deleted": None, "errors": errors}
    deleted: Any = None
    try:
        if clear:
            clear_fn = getattr(store, "clear", None)
            if not callable(clear_fn):
                raise RuntimeError("DPS history clear API is unavailable")
            clear_fn()
            message = "History cleared"
        else:
            if index is None:
                return {"ok": False, "message": "history index is required", "deleted": None, "errors": ["history index is required"]}
            delete_report = getattr(store, "delete_report", None)
            if callable(delete_report):
                deleted = delete_report(int(index or 0))
            else:
                raise RuntimeError("DPS history delete API is unavailable")
            if not isinstance(deleted, Mapping):
                return {"ok": False, "message": "History report not found", "deleted": None, "errors": ["History report not found"]}
            message = "History report deleted"
    except Exception as exc:
        return {"ok": False, "message": str(exc), "deleted": None, "errors": [str(exc)]}
    return {
        "ok": True,
        "message": message,
        "deleted": _json_safe(deleted) if isinstance(deleted, Mapping) else None,
        "errors": [],
        "status": act_history_status(owner),
    }


def _offline_import_source_probe(source_path: str, fmt: str, event_count: int) -> dict[str, Any]:
    return {
        "data_source": "offline_import",
        "mode": "offline_import",
        "active": True,
        "source_path": source_path,
        "format": fmt,
        "event_count": int(event_count or 0),
    }


def _split_import_meta_events(events: Iterable[Any], self_uid: int = 0) -> tuple[int, list[dict[str, Any]]]:
    out: list[dict[str, Any]] = []
    uid = _coerce_int(self_uid, 0)
    for event in events or []:
        if not isinstance(event, Mapping):
            raise ValueError("normalized import events must be objects")
        item = dict(_json_safe(event))
        if str(item.get("kind") or "").strip().lower() == "meta":
            uid = _coerce_int(item.get("self_uid"), uid)
            continue
        out.append(item)
    return uid, out


def _load_normalized_import(path: str) -> tuple[int, list[dict[str, Any]]]:
    suffix = os.path.splitext(str(path or ""))[1].lower()
    if suffix in (".jsonl", ".ndjson"):
        events: list[dict[str, Any]] = []
        with open(path, "r", encoding="utf-8") as fp:
            for line in fp:
                text = line.strip()
                if not text:
                    continue
                value = json.loads(text)
                if not isinstance(value, Mapping):
                    raise ValueError("normalized JSONL rows must be objects")
                events.append(dict(value))
        return _split_import_meta_events(events)
    if suffix != ".json":
        raise ValueError(f"unsupported normalized import format: {suffix.lstrip('.') or os.path.basename(path)}")
    with open(path, "r", encoding="utf-8") as fp:
        data = json.load(fp)
    if isinstance(data, list):
        return _split_import_meta_events(data)
    if not isinstance(data, Mapping):
        raise ValueError("normalized import JSON must be an object or event array")
    meta = data.get("metadata") if isinstance(data.get("metadata"), Mapping) else {}
    self_uid = _coerce_int(data.get("self_uid") or meta.get("self_uid"), 0)
    raw_events = data.get("events") or data.get("items") or []
    if not isinstance(raw_events, list):
        raise ValueError("normalized import JSON events must be an array")
    return _split_import_meta_events(raw_events, self_uid)


def _import_normalized_event_file(path: str) -> dict[str, Any]:
    suffix = os.path.splitext(str(path or ""))[1].lower().lstrip(".") or "unknown"
    try:
        self_uid, events = _load_normalized_import(path)
        return {
            "ok": True,
            "format": suffix,
            "source_path": str(path or ""),
            "self_uid": int(self_uid or 0),
            "event_count": len(events),
            "events": events,
            "errors": [],
            "importer": "normalized_event_file",
        }
    except Exception as exc:
        return {
            "ok": False,
            "format": suffix,
            "source_path": str(path or ""),
            "self_uid": 0,
            "event_count": 0,
            "events": [],
            "errors": [str(exc)],
            "message": str(exc),
            "importer": "normalized_event_file",
        }


def _topic_from_import_event(event: Mapping[str, Any]) -> str:
    topic = str(event.get("topic") or "").strip()
    if topic:
        return topic
    kind = str(event.get("kind") or event.get("type") or "").strip().lower()
    if kind in {"heal", "damage", "skill", "dungeon", "scene", "monster", "boss", "boss_state"}:
        return kind
    if kind in {"server_end", "server_stage_end", "client_use"}:
        return "skill"
    if "damage" in event:
        return "damage"
    if "heal" in event:
        return "heal"
    return kind or "parsed_event"


def _import_event_amount(event: Mapping[str, Any], *keys: str) -> int:
    for key in keys:
        try:
            value = int(event.get(key) or 0)
        except Exception:
            value = 0
        if value:
            return value
    return 0


def _generic_import_snapshot(events: Iterable[Mapping[str, Any]], *, self_uid: int = 0) -> dict[str, Any]:
    rows = [dict(event) for event in events if isinstance(event, Mapping)]
    topics: dict[str, int] = {}
    total_damage = 0
    total_heal = 0
    for event in rows:
        topic = _topic_from_import_event(event)
        topics[topic] = topics.get(topic, 0) + 1
        total_damage += _import_event_amount(event, "damage", "damage_total")
        total_heal += _import_event_amount(event, "heal", "heal_total")
    return {
        "event_count": len(rows),
        "topics": topics,
        "self_uid": int(self_uid or 0),
        "live": {
            "total_damage": total_damage,
            "total_heal": total_heal,
        },
        "render_spec": {
            "title": "Offline Import",
            "sources": {
                "summary": {
                    "data_source": "offline_import",
                    "mode": "offline_import",
                    "active": True,
                },
            },
            "totals": {
                "damage": total_damage,
                "heal": total_heal,
                "dps": 0,
                "hps": 0,
                "elapsed_s": 0.0,
            },
        },
    }


def _generic_import_report(snapshot: Mapping[str, Any], *, source_path: str,
                           fmt: str, event_count: int) -> dict[str, Any]:
    live = snapshot.get("live") if isinstance(snapshot.get("live"), Mapping) else {}
    total_damage = int(live.get("total_damage") or 0)
    total_heal = int(live.get("total_heal") or 0)
    return {
        "encounter_id": f"offline:{os.path.basename(str(source_path or 'import'))}",
        "report_reason": "offline_import",
        "source_kind": "offline_import",
        "source_path": str(source_path or ""),
        "import_format": str(fmt or ""),
        "import_event_count": int(event_count or 0),
        "elapsed_s": 0.0,
        "total_damage": total_damage,
        "total_heal": total_heal,
        "total_dps": 0,
        "total_hps": 0,
        "entities": [],
    }


def _publish_import_events(owner: Any, events: Iterable[Mapping[str, Any]]) -> list[str]:
    errors: list[str] = []
    try:
        bus = ensure_act_event_bus(owner)
    except Exception as exc:
        return [str(exc)]
    for event in events or []:
        if not isinstance(event, Mapping):
            continue
        topic = _topic_from_import_event(event)
        try:
            if "payload" in event and "source" in event:
                bus.publish(topic, event=event)
            else:
                bus.publish(topic, dict(event), source_name="offline_import", source_kind="offline_import")
        except Exception as exc:
            errors.append(str(exc))
    return errors


def _finalize_offline_import_report(harness: Any, *, source_path: str, fmt: str,
                                    event_count: int) -> tuple[dict[str, Any] | None, list[str]]:
    errors: list[str] = []
    tracker = getattr(harness, "dps_tracker", None)
    if tracker is None:
        return None, ["Replay harness DPS tracker is unavailable"]
    reset = getattr(tracker, "reset", None)
    if callable(reset):
        try:
            reset()
        except Exception as exc:
            errors.append(str(exc))
    get_last = getattr(tracker, "get_last_report", None)
    report: Any = None
    if callable(get_last):
        try:
            report = get_last()
        except Exception as exc:
            errors.append(str(exc))
    if not isinstance(report, Mapping):
        return None, errors or ["No DPS report was produced from imported events"]
    out = dict(_json_safe(report))
    out["report_reason"] = "offline_import"
    out["source_kind"] = "offline_import"
    out["source_path"] = str(source_path)
    out["import_format"] = str(fmt or "")
    out["import_event_count"] = int(event_count or 0)
    return out, errors


def _persist_offline_import_report(store: Any, report: Mapping[str, Any]) -> tuple[dict[str, Any] | None, list[str]]:
    add_report = getattr(store, "add_report", None)
    if not callable(add_report):
        return None, ["DPS history add API is unavailable"]
    try:
        item = add_report(dict(report))
    except Exception as exc:
        return None, [str(exc)]
    if not isinstance(item, Mapping):
        return None, ["DPS history did not accept imported report"]
    return dict(_json_safe(item)), []


def _import_exported_report_file(owner: Any, path: str, *, persist: bool,
                                 show: bool, history_limit: int,
                                 initial_errors: Iterable[Any] = ()) -> dict[str, Any] | None:
    """Dispatch shipped-report import to the active game plugin's runtime bridge.

    The platform deliberately owns no game-specific report loader. The plugin
    that owns ``DpsHistoryStore.load_exported_report_file`` registers its importer
    here via ``register_extension_runtime('_import_exported_report_file', ...)``.
    """
    handler = _extension_runtime_handler("_import_exported_report_file")
    if not callable(handler):
        return None
    try:
        return handler(owner, path, persist=persist, show=show,
                       history_limit=history_limit, initial_errors=initial_errors)
    except Exception:
        return None


def _plugin_offline_import_summary(owner: Any, path: str, initial_errors: Iterable[Any] = ()) -> tuple[dict[str, Any] | None, list[str]]:
    try:
        from .adapters import create_plugin_parser_adapter, plugin_parser_adapters
    except Exception as exc:
        return None, [str(exc)]
    errors = [str(item) for item in (initial_errors or []) if str(item or "")]
    try:
        manager = ensure_act_plugin_manager(owner, load=True)
    except Exception as exc:
        errors.append(str(exc))
        return None, errors
    adapters = plugin_parser_adapters(manager)
    if not adapters:
        errors.append("No plugin parser adapters are available")
        return None, errors
    for meta in sorted(adapters, key=lambda item: _safe_float(item.get("priority"), 0.0), reverse=True):
        adapter_id = str(meta.get("adapter_id") or meta.get("id") or "").strip()
        if not adapter_id:
            continue
        try:
            adapter = create_plugin_parser_adapter(manager, adapter_id)
            invoked = adapter.import_file(path)
        except Exception as exc:
            errors.append(f"{adapter_id}: {exc}")
            continue
        invoked_map = invoked if isinstance(invoked, Mapping) else {}
        if not bool(invoked_map.get("ok")):
            msg = str(invoked_map.get("message") or "; ".join(invoked_map.get("errors") or []) or "plugin import failed")
            errors.append(f"{adapter_id}: {msg}")
            continue
        result = invoked_map.get("result")
        if not isinstance(result, Mapping):
            errors.append(f"{adapter_id}: import_file did not return an object")
            continue
        if result.get("ok") is False:
            msg = str(result.get("message") or result.get("reason") or "plugin import did not match")
            errors.append(f"{adapter_id}: {msg}")
            continue
        raw_events = result.get("events") or result.get("items") or []
        if not isinstance(raw_events, list):
            errors.append(f"{adapter_id}: import_file events must be an array")
            continue
        events = [dict(_json_safe(event)) for event in raw_events if isinstance(event, Mapping)]
        if len(events) != len(raw_events):
            errors.append(f"{adapter_id}: import_file returned non-object events")
            continue
        meta_obj = result.get("metadata") if isinstance(result.get("metadata"), Mapping) else {}
        suffix = os.path.splitext(str(path or ""))[1].lower().lstrip(".")
        fmt = str(result.get("format") or result.get("source_format") or suffix or "plugin")
        return {
            "ok": True,
            "format": fmt,
            "source_path": str(result.get("source_path") or result.get("path") or path or ""),
            "self_uid": _coerce_int(result.get("self_uid") or meta_obj.get("self_uid"), 0),
            "event_count": len(events),
            "events": events,
            "snapshot": dict(_json_safe(result.get("snapshot"))) if isinstance(result.get("snapshot"), Mapping) else {},
            "report": dict(_json_safe(result.get("report"))) if isinstance(result.get("report"), Mapping) else None,
            "errors": [],
            "warnings": errors,
            "message": str(result.get("message") or ""),
            "importer": "plugin_parser_adapter",
            "parser_adapter_id": adapter_id,
            "plugin_id": str(getattr(adapter, "plugin_id", "") or meta.get("plugin_id") or ""),
        }, []
    return None, errors


_OFFLINE_IMPORT_ACCEPTED_FORMATS = ["json", "jsonl", "ndjson", "xml", "xml.gz", "xml.zip", "zip", "plugin"]


def _offline_import_state(owner: Any) -> dict[str, Any]:
    state = getattr(owner, "_act_offline_import_state", None)
    if not isinstance(state, dict):
        state = {"selected_file": "", "progress": 0.0, "status": "idle", "last_result": {}}
        try:
            setattr(owner, "_act_offline_import_state", state)
        except Exception:
            pass
    state.setdefault("selected_file", "")
    state.setdefault("progress", 0.0)
    state.setdefault("status", "idle")
    state.setdefault("last_result", {})
    return state


def _set_offline_import_state(owner: Any, *, selected_file: str | None = None,
                              progress: float | None = None, status: str | None = None,
                              last_result: Mapping[str, Any] | None = None) -> dict[str, Any]:
    state = _offline_import_state(owner)
    if selected_file is not None:
        state["selected_file"] = str(selected_file or "")
    if progress is not None:
        state["progress"] = max(0.0, min(float(progress or 0.0), 1.0))
    if status is not None:
        state["status"] = str(status or "idle")
    if last_result is not None:
        state["last_result"] = dict(_json_safe(last_result))
        state["progress"] = 1.0
        state["status"] = "imported" if bool(last_result.get("ok")) else "error"
        state["selected_file"] = str(last_result.get("source_path") or selected_file or state.get("selected_file") or "")
    return state


def act_offline_import_status(owner: Any, *, history_limit: int = 20) -> dict[str, Any]:
    """Return standalone offline-import wizard state shared by WebView and Entity/Tk."""
    state = _offline_import_state(owner)
    last_result = state.get("last_result") if isinstance(state.get("last_result"), Mapping) else {}
    errors = list(last_result.get("errors") or []) if isinstance(last_result, Mapping) else []
    try:
        history = act_history_status(owner, limit=int(history_limit or 20))
    except Exception as exc:
        history = {"ok": False, "message": str(exc), "encounters": [], "errors": [str(exc)]}
        errors.append(str(exc))
    return {
        "ok": not errors,
        "message": str(last_result.get("message") or "Ready") if isinstance(last_result, Mapping) else "Ready",
        "accepted_formats": list(_OFFLINE_IMPORT_ACCEPTED_FORMATS),
        "selected_file": str(state.get("selected_file") or ""),
        "progress": float(state.get("progress") or 0.0),
        "status": str(state.get("status") or "idle"),
        "last_result": dict(_json_safe(last_result)) if isinstance(last_result, Mapping) else {},
        "history": history,
        "errors": errors,
    }


def act_offline_import_file(owner: Any, path: str, *, persist: bool = True,
                            show: bool = False, history_limit: int = 20) -> dict[str, Any]:
    """Replay a normalized ACT import file and optionally persist it to history."""

    _set_offline_import_state(owner, selected_file=str(path or ""), progress=0.0, status="importing")

    def _finish(result: Mapping[str, Any]) -> dict[str, Any]:
        state = _set_offline_import_state(
            owner,
            selected_file=str(result.get("source_path") or path or ""),
            last_result=result,
        )
        last_result = state.get("last_result") if isinstance(state.get("last_result"), Mapping) else result
        return dict(last_result)

    report_import = _import_exported_report_file(
        owner,
        path,
        persist=persist,
        show=show,
        history_limit=history_limit,
        initial_errors=(),
    )
    if isinstance(report_import, Mapping):
        return _finish(dict(report_import))

    summary = _import_normalized_event_file(str(path or ""))
    if not bool(summary.get("ok")):
        errors = list(summary.get("errors") or [])
        fallback, fallback_errors = _plugin_offline_import_summary(owner, path, errors)
        if isinstance(fallback, Mapping) and bool(fallback.get("ok")):
            summary = fallback
        else:
            merged_errors = fallback_errors or errors
            message = str(summary.get("message") or "; ".join(merged_errors) or "Offline import failed")
            return _finish({
                "ok": False,
                "message": message,
                "format": str(summary.get("format") or ""),
                "source_path": str(summary.get("source_path") or path or ""),
                "self_uid": int(summary.get("self_uid") or 0),
                "event_count": int(summary.get("event_count") or 0),
                "persist_requested": bool(persist),
                "persisted": False,
                "history_item": None,
                "preview": {},
                "snapshot": {},
                "errors": merged_errors or [message],
                "status": {},
                "importer": str(summary.get("importer") or "normalized_event_file"),
                "parser_adapter_id": "",
                "plugin_id": "",
            })

    source_path = str(summary.get("source_path") or path or "")
    fmt = str(summary.get("format") or "")
    event_count = int(summary.get("event_count") or 0)
    events = list(summary.get("events") or [])
    importer = str(summary.get("importer") or "normalized")
    parser_adapter_id = str(summary.get("parser_adapter_id") or "")
    plugin_id = str(summary.get("plugin_id") or "")
    events = [dict(event) for event in events if isinstance(event, Mapping)]
    store = _extension_runtime_value(owner, "owner_history_store", None) if persist else None
    errors: list[str] = []
    errors.extend(_publish_import_events(owner, events))

    self_uid = int(summary.get("self_uid") or 0)
    raw_snapshot = summary.get("snapshot") if isinstance(summary.get("snapshot"), Mapping) else {}
    snapshot_payload = dict(_json_safe(raw_snapshot if raw_snapshot else _generic_import_snapshot(events, self_uid=self_uid)))
    report = summary.get("report") if isinstance(summary.get("report"), Mapping) else None
    if report is not None:
        report = dict(_json_safe(report))
        report.setdefault("report_reason", "offline_import")
        report.setdefault("source_kind", "offline_import")
        report.setdefault("source_path", source_path)
        report.setdefault("import_format", fmt)
        report.setdefault("import_event_count", event_count)
    elif events:
        report = _generic_import_report(
            snapshot_payload,
            source_path=source_path,
            fmt=fmt,
            event_count=event_count,
        )
    if persist and report is not None and store is None:
        errors.append("DPS history is not initialized")

    history_item: dict[str, Any] | None = None
    if persist and report is not None and store is not None:
        history_item, persist_errors = _persist_offline_import_report(store, report)
        errors.extend(persist_errors)

    shown = False
    if show and report is not None:
        try:
            shown = _show_plugin_report(owner, report)
        except Exception as exc:
            errors.append(str(exc))

    preview = _report_preview(snapshot_payload, report)
    persisted = bool(history_item)
    status = act_history_status(owner, limit=history_limit) if persist else {}
    ok = bool((not persist or (report is not None and persisted)) and not errors)
    return _finish({
        "ok": ok,
        "message": "Imported" if ok else "; ".join(errors) or "Offline import did not produce a report",
        "format": fmt,
        "source_path": source_path,
        "self_uid": self_uid,
        "event_count": event_count,
        "importer": importer,
        "parser_adapter_id": parser_adapter_id,
        "plugin_id": plugin_id,
        "persist_requested": bool(persist),
        "persisted": persisted,
        "shown": shown,
        "history_item": history_item,
        "report": dict(_json_safe(report)) if isinstance(report, Mapping) else None,
        "preview": preview,
        "snapshot": snapshot_payload,
        "errors": errors,
        "status": status,
    })


def _timeline_state(owner: Any) -> dict[str, Any]:
    state = getattr(owner, "_act_timeline_state", None)
    if not isinstance(state, dict):
        state = {"playing": False, "cursor_ms": 0, "speed": 1.0, "filters": {"query": ""}}
        try:
            setattr(owner, "_act_timeline_state", state)
        except Exception:
            pass
    state.setdefault("playing", False)
    state.setdefault("cursor_ms", 0)
    state.setdefault("speed", 1.0)
    filters = state.get("filters")
    if not isinstance(filters, dict):
        filters = {"query": ""}
        state["filters"] = filters
    filters.setdefault("query", "")
    return state


def _compact_timeline_event(event: Mapping[str, Any], index: int = 0) -> dict[str, Any]:
    payload = event.get("payload") if isinstance(event.get("payload"), Mapping) else {}
    source = event.get("source") if isinstance(event.get("source"), Mapping) else {}
    topic = str(event.get("topic") or "")
    observed_at = _safe_float(event.get("observed_at") or (payload or {}).get("timestamp") or 0.0)
    display = _action_log_display_fields(payload, topic=topic)
    label = str(
        display.get("label")
        or (payload or {}).get("message")
        or topic
    )
    value = (
        (payload or {}).get("damage")
        or (payload or {}).get("damage_total")
        or (payload or {}).get("heal")
        or (payload or {}).get("event_type")
        or ""
    )
    return {
        "index": int(index),
        "id": str(event.get("id") or ""),
        "topic": topic,
        "observed_at": observed_at,
        "time_ms": int(max(0.0, observed_at) * 1000.0) if observed_at else 0,
        "label": label,
        "value": value,
        "source": str(source.get("name") or source.get("kind") or ""),
        "source_kind": str(source.get("kind") or ""),
        "payload": _json_safe(payload),
    }


def _timeline_encounter_id(owner: Any, state: Mapping[str, Any]) -> str:
    value = _extension_runtime_value(owner, "encounter_id", "")
    if value:
        return str(value)
    return str(state.get("encounter_id") or "live")


def _timeline_replay_events(owner: Any) -> tuple[list[Mapping[str, Any]], int]:
    provider = (
        getattr(owner, "_act_timeline_replay_events", None)
        or getattr(owner, "act_timeline_replay_events", None)
    )
    events = provider() if callable(provider) else provider
    if not isinstance(events, list):
        return [], 0
    self_uid = (
        getattr(owner, "_act_timeline_replay_self_uid", 0)
        or getattr(owner, "act_timeline_replay_self_uid", 0)
    )
    try:
        uid = int(self_uid or 0)
    except Exception:
        uid = 0
    return [event for event in events if isinstance(event, Mapping)], uid


def _replay_base_timestamp(events: list[Mapping[str, Any]]) -> float:
    stamps = [
        _safe_float(event.get("timestamp") or event.get("observed_at") or 0.0)
        for event in events
    ]
    stamps = [stamp for stamp in stamps if stamp > 0.0]
    return min(stamps) if stamps else 0.0


def _generic_replay_timeline_status(events: list[Mapping[str, Any]], *,
                                    self_uid: int = 0, cursor_ms: int = 0,
                                    limit: int = 80, query: str = "") -> dict[str, Any]:
    normalized = [dict(event) for event in events if isinstance(event, Mapping)]
    normalized.sort(key=lambda item: _safe_float(item.get("timestamp") or item.get("observed_at") or 0.0))
    cursor = max(0, _coerce_int(cursor_ms, 0))
    row_limit = max(1, min(_coerce_int(limit, 80), 500))
    base_ts = _replay_base_timestamp(normalized)
    compact: list[dict[str, Any]] = []
    selected: list[dict[str, Any]] = []
    for index, event in enumerate(normalized):
        observed_at = _safe_float(event.get("timestamp") or event.get("observed_at") or 0.0)
        if observed_at and base_ts:
            time_ms = int(max(0.0, observed_at - base_ts) * 1000.0)
        else:
            time_ms = int(max(0.0, observed_at) * 1000.0)
        replayed = time_ms <= cursor
        if replayed:
            selected.append(event)
        compact.append({
            "index": index,
            "id": str(event.get("id") or f"replay:{index}"),
            "topic": _topic_from_import_event(event),
            "kind": str(event.get("kind") or event.get("type") or ""),
            "observed_at": observed_at,
            "time_ms": time_ms,
            "absolute_time_ms": int(max(0.0, observed_at) * 1000.0) if observed_at else 0,
            "label": str(event.get("message") or event.get("label") or event.get("skill_name") or event.get("name") or event.get("kind") or event.get("type") or "event"),
            "value": event.get("damage") or event.get("heal") or event.get("value") or "",
            "source": str(event.get("source") or "replay"),
            "payload": _json_safe(dict(event)),
            "replayed": replayed,
            "is_cursor": False,
        })
    text = str(query or "").strip().lower()
    visible = compact
    if text:
        visible = [
            event for event in compact
            if text in json.dumps(event, ensure_ascii=False, default=str).lower()
        ]
    nearest_id = ""
    if visible:
        nearest = min(visible, key=lambda item: abs(int(item.get("time_ms") or 0) - cursor))
        nearest_id = str(nearest.get("id") or "")
        for item in visible:
            item["is_cursor"] = str(item.get("id") or "") == nearest_id
    return {
        "ok": True,
        "message": "OK",
        "replay": {
            "enabled": True,
            "source": "owner_events",
            "event_count": len(normalized),
            "replayed_event_count": len(selected),
            "base_timestamp": base_ts,
            "nearest_event_id": nearest_id,
        },
        "events": visible[:row_limit],
        "cursor_ms": cursor,
        "filters": {"query": str(query or ""), "limit": row_limit},
        "replay_snapshot": _generic_import_snapshot(selected, self_uid=self_uid),
        "errors": [],
    }


def _timeline_replay_status(owner: Any, *, state: Mapping[str, Any], limit: int, query: str) -> Optional[dict[str, Any]]:
    replay_events, self_uid = _timeline_replay_events(owner)
    if not replay_events:
        return None
    provider = getattr(owner, "_act_timeline_replay_status", None) or getattr(owner, "act_timeline_replay_status", None)
    if callable(provider):
        result = provider(
            replay_events,
            self_uid=self_uid,
            cursor_ms=int(state.get("cursor_ms") or 0),
            limit=limit,
            query=query,
        )
        if isinstance(result, Mapping):
            return dict(result)
    return _generic_replay_timeline_status(
        replay_events,
        self_uid=self_uid,
        cursor_ms=int(state.get("cursor_ms") or 0),
        limit=limit,
        query=query,
    )


def act_timeline_status(owner: Any, *, limit: int = 80, query: str = "") -> dict[str, Any]:
    """Return compact ACT timeline/VCR state shared by WebView and Entity/Tk."""
    state = _timeline_state(owner)
    errors: list[str] = []
    encounter_id = _timeline_encounter_id(owner, state)
    filters = dict(state.get("filters") or {})
    query_text = str(query or filters.get("query") or "")
    row_limit = max(1, min(int(limit or 80), 500))
    try:
        replay_status = _timeline_replay_status(owner, state=state, limit=row_limit, query=query_text)
    except Exception as exc:
        replay_status = None
        errors.append(str(exc))
    if replay_status is not None:
        replay_errors = list(replay_status.get("errors") or [])
        all_errors = errors + replay_errors
        replay_filters = dict(replay_status.get("filters") or {})
        replay_filters.update({"query": query_text, "limit": row_limit})
        replay_status.update({
            "ok": not all_errors,
            "message": "OK" if not all_errors else "; ".join(all_errors),
            "encounter_id": encounter_id,
            "speed": float(state.get("speed") or 1.0),
            "playing": bool(state.get("playing")),
            "filters": replay_filters,
            "errors": all_errors,
        })
        return replay_status
    # 事件列表是 (retained 计数, query, limit) 的纯函数 — 同 aggregate 的
    # 新鲜度缓存: 无新事件时跳过 recent_events 深拷贝 + 逐事件 compact +
    # query 过滤(每行一次 json.dumps)。cursor/speed/playing 是播放态,
    # 不进缓存, 每次现读保持实时。
    text = query_text.strip().lower()
    _bus = None
    _ev_key = None
    try:
        _bus = ensure_act_event_bus(owner)
        _ev_key = (_bus.retained, text, row_limit)
    except Exception as exc:
        errors.append(str(exc))
    events: Optional[list] = None
    if _ev_key is not None:
        _cached = getattr(owner, "_act_timeline_events_cache", None)
        if _cached is not None and _cached[0] == _ev_key:
            events = _cached[1]
    if events is None:
        raw_events: list = []
        if _bus is not None:
            try:
                raw_events = _bus.recent_events(row_limit)
            except Exception as exc:
                errors.append(str(exc))
        events = [_compact_timeline_event(event, idx) for idx, event in enumerate(raw_events) if isinstance(event, Mapping)]
        if text:
            events = [
                event for event in events
                if text in json.dumps(event, ensure_ascii=False, default=str).lower()
            ]
        if _ev_key is not None and not errors:
            try:
                setattr(owner, "_act_timeline_events_cache", (_ev_key, events))
            except Exception:
                pass
    filters.update({"query": query_text, "limit": row_limit})
    return {
        "ok": not errors,
        "message": "OK" if not errors else "; ".join(errors),
        "encounter_id": encounter_id,
        "events": events,
        "cursor_ms": int(state.get("cursor_ms") or 0),
        "speed": float(state.get("speed") or 1.0),
        "playing": bool(state.get("playing")),
        "filters": filters,
        "replay": {"enabled": False},
        "errors": errors,
    }


def act_timeline_play(owner: Any, *, speed: float | None = None) -> dict[str, Any]:
    state = _timeline_state(owner)
    if speed is not None:
        state["speed"] = max(0.1, min(float(speed or 1.0), 8.0))
    state["playing"] = True
    return act_timeline_status(owner)


def act_timeline_pause(owner: Any) -> dict[str, Any]:
    state = _timeline_state(owner)
    state["playing"] = False
    return act_timeline_status(owner)


def act_timeline_seek(owner: Any, *, cursor_ms: int = 0) -> dict[str, Any]:
    state = _timeline_state(owner)
    state["cursor_ms"] = max(0, int(cursor_ms or 0))
    return act_timeline_status(owner)


def act_timeline_step(owner: Any, *, delta_ms: int = 1000) -> dict[str, Any]:
    state = _timeline_state(owner)
    state["cursor_ms"] = max(0, int(state.get("cursor_ms") or 0) + int(delta_ms or 0))
    state["playing"] = False
    return act_timeline_status(owner)


def act_timeline_set_speed(owner: Any, *, speed: float = 1.0) -> dict[str, Any]:
    state = _timeline_state(owner)
    state["speed"] = max(0.1, min(float(speed or 1.0), 8.0))
    return act_timeline_status(owner)


def act_timeline_filter(owner: Any, *, query: str = "") -> dict[str, Any]:
    state = _timeline_state(owner)
    state["filters"] = {"query": str(query or "")}
    return act_timeline_status(owner, query=query)


_ACTION_LOG_COLUMNS = [
    {"key": "time_ms", "label": "Time", "width": 90},
    {"key": "topic", "label": "Topic", "width": 110},
    {"key": "label", "label": "Action", "width": 220},
    {"key": "value", "label": "Value", "width": 110},
    {"key": "source", "label": "Source", "width": 120},
]


def _action_log_state(owner: Any) -> dict[str, Any]:
    state = getattr(owner, "_act_action_log_state", None)
    if not isinstance(state, dict):
        state = {"filters": {"query": "", "topic": "", "source": "live", "encounter_id": ""}, "cursor": {"time_ms": 0, "offset": 0, "limit": 80}}
        try:
            setattr(owner, "_act_action_log_state", state)
        except Exception:
            pass
    filters = state.get("filters")
    if not isinstance(filters, dict):
        filters = {"query": "", "topic": "", "source": "live", "encounter_id": ""}
        state["filters"] = filters
    filters.setdefault("query", "")
    filters.setdefault("topic", "")
    filters.setdefault("source", "live")
    filters.setdefault("encounter_id", "")
    cursor = state.get("cursor")
    if not isinstance(cursor, dict):
        cursor = {"time_ms": 0, "offset": 0, "limit": 80}
        state["cursor"] = cursor
    cursor.setdefault("time_ms", 0)
    cursor.setdefault("offset", 0)
    cursor.setdefault("limit", 80)
    return state


def _normalize_action_log_source(source: Any) -> str:
    text = str(source or "live").strip().lower()
    if text in {"history", "sqlite", "archive", "db"}:
        return "history"
    return "live"


def _action_log_row(event: Mapping[str, Any], index: int = 0) -> dict[str, Any]:
    compact = _compact_timeline_event(event, index)
    payload = compact.get("payload") if isinstance(compact.get("payload"), Mapping) else {}
    display = _action_log_display_fields(payload, topic=str(compact.get("topic") or ""))
    actor = str(display.get("actor") or "")
    target = str(display.get("target") or "")
    row_id = compact.get("id") or f"{compact.get('topic')}:{compact.get('time_ms')}:{index}"
    row = {
        "index": int(index),
        "id": str(row_id),
        "time_ms": int(compact.get("time_ms") or 0),
        "topic": str(compact.get("topic") or ""),
        "label": str(display.get("label") or compact.get("label") or ""),
        "value": compact.get("value") or "",
        "source": str(compact.get("source") or ""),
        "source_kind": str(compact.get("source_kind") or ""),
        "actor": actor,
        "target": target,
        "payload": _json_safe(payload),
        "source_mode": "live",
        "is_cursor": False,
    }
    row.update(_action_log_group_metadata(row, payload, display))
    return row


def _action_log_history_row(action: Mapping[str, Any], index: int = 0) -> dict[str, Any]:
    payload = action.get("payload") if isinstance(action.get("payload"), Mapping) else {}
    actor = str(action.get("actor_name") or payload.get("actor_name") or payload.get("source_name") or action.get("actor_uid") or "")
    target = str(action.get("target_name") or payload.get("target_name") or action.get("target_uid") or "")
    label = str(
        action.get("skill_name")
        or payload.get("skill_name")
        or action.get("action_type")
        or payload.get("action_type")
        or action.get("topic")
        or ""
    )
    source = str(action.get("source_name") or action.get("source_kind") or payload.get("source_name") or "history")
    encounter_id = str(action.get("encounter_id") or "")
    seq = int(action.get("seq") or index)
    time_ms = int(action.get("time_ms") or 0)
    merged_payload = dict(payload)
    for key in ("actor_uid", "target_uid", "skill_id", "dungeon_id", "encounter_id", "topic"):
        if action.get(key) is not None and merged_payload.get(key) is None:
            merged_payload[key] = action.get(key)
    if actor and not merged_payload.get("actor"):
        merged_payload["actor"] = actor
    if target and not merged_payload.get("target"):
        merged_payload["target"] = target
    if label and not merged_payload.get("skill_name"):
        merged_payload["skill_name"] = label
    display = _action_log_display_fields(merged_payload, topic=str(action.get("topic") or payload.get("topic") or ""))
    row = {
        "index": int(index),
        "id": f"history:{encounter_id}:{seq}:{time_ms}",
        "encounter_id": encounter_id,
        "time_ms": time_ms,
        "topic": str(action.get("topic") or payload.get("topic") or ""),
        "label": str(display.get("label") or label),
        "value": action.get("value") if action.get("value") is not None else "",
        "source": source,
        "actor": str(display.get("actor") or actor),
        "target": str(display.get("target") or target),
        "payload": _json_safe(merged_payload),
        "source_mode": "history",
        "is_cursor": False,
    }
    row.update(_action_log_group_metadata(row, merged_payload, display))
    return row


# UI-internal book-keeping topics that carry no user-facing combat content
# (act_snapshot is the DPS render-spec push emitted on every damage/state tick).
# They are kept on the event bus for plugins/triggers/overlay but hidden from
# the Action Log and Aggregate views so they don't show up as empty "0 · 38x"
# groups.
_HIDDEN_ACTION_TOPICS = {"act_snapshot"}

# Plugins (and UI snapshots) publish with these source kinds. Their events are
# echoes / internal book-keeping (plugin_ui_invalidate from request_redraw, the
# star_basic_report_plugin re-emitting skills, etc.). They are KEPT in the Action
# Log but grouped under a separate 系统 category, and EXCLUDED from the combat
# aggregate + graph so they don't double-count skills or flatten the curve.
_SYSTEM_SOURCE_KINDS = {"plugin", "ui"}


def _is_system_event(row: Mapping[str, Any]) -> bool:
    return str(row.get("source_kind") or "").strip().lower() in _SYSTEM_SOURCE_KINDS


def _combat_rows(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Authoritative combat events only (drops plugin/ui echoes) — for the
    aggregate workbench and the damage graph."""
    return [row for row in rows if not _is_system_event(row)]


def _filter_action_log_rows(rows: list[dict[str, Any]], *, query: str = "", topic: str = "") -> list[dict[str, Any]]:
    text = str(query or "").strip().lower()
    topic_text = str(topic or "").strip().lower()
    out = [row for row in rows if str(row.get("topic") or "").lower() not in _HIDDEN_ACTION_TOPICS]
    if topic_text:
        out = [row for row in out if str(row.get("topic") or "").lower() == topic_text]
    if text:
        out = [row for row in out if text in json.dumps(row, ensure_ascii=False, default=str).lower()]
    return out


def _action_log_numeric_value(row: Mapping[str, Any]) -> float:
    value = row.get("value")
    if isinstance(value, (int, float)):
        return float(value)
    text = str(value or "").replace(",", "").strip()
    if not text:
        return 0.0
    try:
        return float(text)
    except Exception:
        return 0.0


def _truthy_text(value: Any) -> str:
    text = str(value or "").strip()
    return text if text and text not in {"-", "0", "None", "none", "null"} else ""


def _payload_first_text(payload: Mapping[str, Any], keys: Iterable[str]) -> str:
    for key in keys:
        text = _truthy_text(payload.get(key))
        if text:
            return text
    return ""


def _payload_first_int(payload: Mapping[str, Any], keys: Iterable[str]) -> int:
    for key in keys:
        value = payload.get(key)
        try:
            if value is not None and str(value or "").strip():
                ivalue = int(value)
                if ivalue:
                    return ivalue
        except Exception:
            continue
    return 0


def _friendly_unknown(kind_label: str, uid: Any) -> str:
    """Stable fallback for unresolved entities.

    Action Log groups must keep the full UID visible so users can copy/search
    the exact entity and so generated labels never masquerade as resolved names.
    """
    text = str(uid or "").strip()
    if not text:
        return kind_label
    label = {
        "未知怪物": "怪物",
        "未知目标": "目标",
        "未知参与者": "参与者",
    }.get(kind_label, kind_label)
    return f"{label}#{text}"


def _is_generated_entity_label(text: Any) -> bool:
    value = _truthy_text(text)
    return bool(value and (value.startswith("怪物#") or value.startswith("目标#") or value.startswith("技能#") or value.startswith("地牢#") or value.startswith("未知")))


def _is_unresolved_entity_label(text: Any) -> bool:
    value = _truthy_text(text)
    if not value:
        return True
    if _is_generated_entity_label(value):
        return True
    return value.lstrip("+-").isdigit()


def _payload_first_name_text(payload: Mapping[str, Any], keys: Iterable[str]) -> str:
    for key in keys:
        text = _truthy_text(payload.get(key))
        if text and not _is_unresolved_entity_label(text):
            return text
    return ""


# Game name resolver is contributed by the active game plugin through
# ``register_extension_runtime('resolve_name')``. Callers go via
# ``_resolve_name`` below. Platform code never imports the plugin's name tables.


def _resolve_name(kind: str, value: Any) -> str:
    """Resolve a game entity name (skill/monster/dungeon) through the plugin.

    Returns "" when no game plugin is loaded (callers fall back to bare ids).
    The plugin runtime provider's name resolver is the registered
    handler; the plugin owns its own tablekit/tables.
    """
    if value is None or not str(value or "").strip():
        return ""
    handler = _extension_runtime_handler("resolve_name")
    if not callable(handler):
        return ""
    try:
        return _truthy_text(handler(kind=kind, id=value, default=""))
    except Exception:
        return ""


def _resolve_skill_name_from_detail(skill: Mapping[str, Any]) -> str:
    """Best-effort skill name for a DPS-tracker skill detail row.

    Tries every id form the tracker carries (base/semantic/level/raw) through
    the shared name resolver, retrying with //100 so composite/level ids resolve
    against the base-keyed tables. Returns "" when nothing resolves so callers
    keep their existing bare-id fallback.
    """
    name = _truthy_text(skill.get("skill_name")) or _truthy_text(skill.get("name"))
    if name:
        return name
    seen: set[int] = set()
    for key in ("base_skill_id", "semantic_skill_id", "semantic_base_skill_id",
                "skill_id", "skill_level_id", "source_skill_id"):
        sid = _coerce_int(skill.get(key), 0)
        if sid <= 0 or sid in seen:
            continue
        seen.add(sid)
        resolved = _resolve_name("skill", sid) or (_resolve_name("skill", sid // 100) if sid >= 100 else "")
        if resolved:
            return resolved
    return ""


def _owner_packet_bridge(owner: Any) -> Any:
    return _extension_runtime_value(owner, "owner_packet_bridge", None)


def _monster_snapshot_from_owner(owner: Any, uid: int) -> Mapping[str, Any]:
    if owner is None or not uid:
        return {}
    bridge = _owner_packet_bridge(owner)
    getter = getattr(bridge, "get_monster", None)
    if not callable(getter):
        return {}
    try:
        monster = getter(int(uid))
    except Exception:
        monster = None
    if monster is None:
        return {}
    to_dict = getattr(monster, "to_dict", None)
    if callable(to_dict):
        try:
            snap = to_dict()
            return snap if isinstance(snap, Mapping) else {}
        except Exception:
            return {}
    out: dict[str, Any] = {}
    for attr in ("uuid", "uid", "name", "template_id", "hp", "max_hp"):
        try:
            value = getattr(monster, attr)
        except Exception:
            continue
        if value is not None:
            out[attr] = value
    return out


def _owner_monster_identity_cache(owner: Any) -> dict[str, dict[str, Any]]:
    if owner is None:
        return {"by_uuid": {}, "by_template": {}}
    cache = getattr(owner, "_act_monster_identity_cache", None)
    if not isinstance(cache, dict):
        cache = {"by_uuid": {}, "by_template": {}}
        try:
            setattr(owner, "_act_monster_identity_cache", cache)
        except Exception:
            return {"by_uuid": {}, "by_template": {}}
    by_uuid = cache.get("by_uuid")
    if not isinstance(by_uuid, dict):
        by_uuid = {}
        cache["by_uuid"] = by_uuid
    by_template = cache.get("by_template")
    if not isinstance(by_template, dict):
        by_template = {}
        cache["by_template"] = by_template
    return cache


def _remember_monster_identity(owner: Any, payload: Mapping[str, Any]) -> None:
    if owner is None or not isinstance(payload, Mapping):
        return
    name = _payload_first_name_text(payload, ("monster_name", "target_name", "boss_name", "name"))
    if not name:
        return
    uuid = _payload_first_int(payload, ("target_uuid", "target_uid", "uuid", "boss_uuid", "boss_uid", "combatant_id"))
    template_id = _payload_first_int(payload, ("monster_id", "template_id", "target_template_id", "boss_id", "config_id"))
    cache = _owner_monster_identity_cache(owner)
    item = {"name": name, "monster_id": template_id, "uuid": uuid}
    if uuid:
        cache["by_uuid"][str(uuid)] = item
    if template_id:
        cache["by_template"][str(template_id)] = item


def _lookup_monster_identity(owner: Any, *, uuid: int = 0, template_id: int = 0) -> Mapping[str, Any]:
    if owner is None:
        return {}
    cache = _owner_monster_identity_cache(owner)
    if uuid:
        item = cache.get("by_uuid", {}).get(str(uuid))
        if isinstance(item, Mapping) and _truthy_text(item.get("name")):
            return item
    if template_id:
        item = cache.get("by_template", {}).get(str(template_id))
        if isinstance(item, Mapping) and _truthy_text(item.get("name")):
            return item
    return {}


def enrich_action_log_event(event: Mapping[str, Any] | None, *, owner: Any = None,
                            topic: str = "") -> dict[str, Any]:
    """Add best-effort display names/ids for ACT action-log consumers.

    The helper is intentionally additive: existing parser payload fields are
    preserved, and UI/runtime code can still fall back to raw IDs when a name
    is not available yet.
    """
    payload = dict(event or {})
    topic_text = str(topic or payload.get("topic") or "").strip().lower()
    if topic_text in {"monster", "boss", "boss_state"}:
        if _payload_first_int(payload, ("uuid",)) and not _payload_first_int(payload, ("target_uuid", "target_uid")):
            payload["target_uuid"] = payload.get("uuid")
        if _payload_first_int(payload, ("template_id",)) and not _payload_first_int(payload, ("monster_id", "target_template_id")):
            payload["monster_id"] = payload.get("template_id")
        name = _payload_first_name_text(payload, ("monster_name", "target_name", "boss_name", "name"))
        if name:
            payload.setdefault("monster_name", name)
            payload.setdefault("target_name", name)
    _remember_monster_identity(owner, payload)
    target_uid = _payload_first_int(payload, ("target_uuid", "target_uid", "uuid", "boss_uuid", "boss_uid", "combatant_id"))
    monster = _monster_snapshot_from_owner(owner, target_uid)
    if monster:
        name = _truthy_text(monster.get("name"))
        template_id = _payload_first_int(monster, ("template_id", "monster_id"))
        if name and not _is_unresolved_entity_label(name) and not _payload_first_name_text(payload, ("target_name", "monster_name", "target_display")):
            payload["target_name"] = name
            payload["monster_name"] = name
        if template_id and not _payload_first_int(payload, ("monster_id", "template_id", "target_template_id")):
            payload["monster_id"] = template_id
            payload["target_template_id"] = template_id
    cached = _lookup_monster_identity(
        owner,
        uuid=target_uid,
        template_id=_payload_first_int(payload, ("monster_id", "template_id", "target_template_id")),
    )
    if cached:
        cached_name = _truthy_text(cached.get("name"))
        cached_monster_id = _payload_first_int(cached, ("monster_id", "template_id"))
        if cached_name and not _is_unresolved_entity_label(cached_name) and not _payload_first_name_text(payload, ("target_name", "monster_name", "target_display")):
            payload["target_name"] = cached_name
            payload["monster_name"] = cached_name
        if cached_monster_id and not _payload_first_int(payload, ("monster_id", "template_id", "target_template_id")):
            payload["monster_id"] = cached_monster_id
            payload["target_template_id"] = cached_monster_id
    display = _action_log_display_fields(payload, topic=topic_text)
    if display.get("skill_name") and not payload.get("skill_name"):
        payload["skill_name"] = display.get("skill_name")
    if display.get("monster_name") and not payload.get("monster_name"):
        payload["monster_name"] = display.get("monster_name")
    if display.get("dungeon_name") and not payload.get("dungeon_name"):
        payload["dungeon_name"] = display.get("dungeon_name")
    if display.get("label"):
        payload.setdefault("display_label", display.get("label"))
    if display.get("actor"):
        payload.setdefault("actor_display", display.get("actor"))
    if display.get("target"):
        payload.setdefault("target_display", display.get("target"))
    payload.setdefault("display_kind", display.get("group_kind") or topic_text or "event")
    evidence: list[dict[str, Any]] = []
    for kind, id_key, name_key in (
        ("skill", "skill_id", "skill_name"),
        ("monster", "monster_id", "monster_name"),
        ("dungeon", "dungeon_id", "dungeon_name"),
    ):
        iid = _payload_first_int(payload, (id_key,))
        text = _truthy_text(payload.get(name_key))
        if iid and text and not _is_unresolved_entity_label(text):
            evidence.append({"kind": kind, "id": iid, "name": text, "source": "act_name_resolver"})
    if monster:
        monster_text = _truthy_text(monster.get("name"))
        if monster_text and not _is_unresolved_entity_label(monster_text):
            evidence.append({"kind": "monster", "id": target_uid, "name": monster_text, "source": "packet_bridge_monster_cache"})
    if evidence:
        payload["name_resolution"] = evidence
    return payload


def _action_log_display_fields(payload: Mapping[str, Any], *, topic: str = "") -> dict[str, Any]:
    payload = payload if isinstance(payload, Mapping) else {}
    topic_text = str(topic or payload.get("topic") or "").strip().lower()
    skill_id = _payload_first_int(payload, ("skill_id", "skill_key", "skill", "skill_level_id", "skillLevelId"))
    monster_id = _payload_first_int(payload, ("monster_id", "template_id", "target_template_id", "boss_id", "config_id"))
    dungeon_id = _payload_first_int(payload, ("dungeon_id", "dungeon", "cur_map_id", "map_id"))
    actor_uid = _payload_first_int(payload, ("actor_uid", "attacker_uid", "source_uid", "caster_uid", "player_uid"))
    target_uid = _payload_first_int(payload, ("target_uid", "target_uuid", "uuid", "victim_uid", "boss_uid", "boss_uuid", "combatant_id"))
    skill_name = (
        _payload_first_text(payload, ("skill_display", "skill_name", "skillName", "action_name"))
        or _resolve_name("skill", skill_id)
        # Direct-hit damage carries a composite/level id (e.g. 120101) while the
        # name tables are keyed by the base skill id (1201). Retry with //100
        # like packet_bridge._get_skill_name does, so most skills resolve a name
        # instead of only the DoT/debuff ticks whose id is a verbatim buff key.
        or (_resolve_name("skill", skill_id // 100) if skill_id >= 100 else "")
        or _payload_first_text(payload, ("skill",))
    )
    monster_name = _payload_first_name_text(payload, ("monster_name", "target_name", "boss_name", "target", "victim", "boss")) or _resolve_name("monster", monster_id)
    if _is_unresolved_entity_label(monster_name):
        monster_name = ""
    dungeon_name = (
        _payload_first_text(payload, ("dungeon_display", "dungeon_name", "scene_name", "map_name"))
        or _resolve_name("dungeon", dungeon_id)
    )
    actor_name = _payload_first_text(payload, ("actor_display", "actor_name", "attacker_name", "attacker", "actor", "source_name", "source", "caster_name", "player_name", "name"))
    target_name = _payload_first_name_text(payload, ("target_name", "victim_name", "target", "victim", "boss_name", "boss")) or monster_name
    if _is_unresolved_entity_label(target_name):
        target_name = ""
    if topic_text in {"skill"}:
        label = skill_name or _payload_first_text(payload, ("message", "event_type", "name")) or topic_text
        group_kind = "actor_skill" if actor_name and skill_name else "skill"
        group_name = f"{actor_name} · {skill_name}" if actor_name and skill_name else (skill_name or label)
    elif topic_text in {"dungeon", "scene"}:
        label = dungeon_name or _payload_first_text(payload, ("message", "event_type", "name")) or topic_text
        group_kind = "dungeon"
        group_name = dungeon_name or label
    elif topic_text in {"monster", "boss", "boss_state"}:
        fallback = _friendly_unknown("未知怪物", target_uid or monster_id) if (target_uid or monster_id) else topic_text
        label = monster_name or target_name or _payload_first_text(payload, ("message", "event_type", "name")) or fallback
        group_kind = "monster"
        group_name = monster_name or target_name or label
    elif topic_text in {"damage", "heal"}:
        label = skill_name or _payload_first_text(payload, ("message", "action_type", "event_type")) or topic_text
        target_is_player = bool(payload.get("target_is_player"))
        target_is_monster = bool(payload.get("target_is_monster")) or bool(monster_name or monster_id)
        group_kind = "monster" if target_is_monster and not target_is_player else "target"
        fallback = _friendly_unknown("未知怪物" if group_kind == "monster" else "未知目标", target_uid) if target_uid else label
        group_name = target_name or monster_name or fallback
    else:
        label = _payload_first_text(payload, ("display_label", "message", "action_name", "skill_name", "name")) or topic_text
        group_kind = topic_text or "event"
        group_name = label
    return {
        "label": label,
        "actor": actor_name or (_friendly_unknown("未知参与者", actor_uid) if actor_uid else ""),
        "target": target_name or (_friendly_unknown("未知怪物" if group_kind == "monster" else "未知目标", target_uid) if target_uid else ""),
        "skill_name": skill_name,
        "monster_name": monster_name,
        "dungeon_name": dungeon_name,
        "skill_id": skill_id,
        "monster_id": monster_id,
        "dungeon_id": dungeon_id,
        "actor_uid": actor_uid,
        "target_uid": target_uid,
        "group_kind": group_kind,
        "group_name": group_name,
    }


def _action_log_group_metadata(row: Mapping[str, Any], payload: Mapping[str, Any], display: Mapping[str, Any]) -> dict[str, Any]:
    topic = str(row.get("topic") or "")
    if _is_system_event(row):
        # Internal / plugin events: kept in the Action Log but bucketed under a
        # separate 系统 kind (one group per internal topic) so they don't mix
        # with combat actions.
        group_kind = "system"
        group_name = topic or "system"
        group_key = f"system:{(topic or 'event').lower()}"
    else:
        group_kind = _truthy_text(display.get("group_kind")) or topic or "event"
        group_name = _truthy_text(display.get("group_name")) or _truthy_text(row.get("label")) or "-"
        safe_name = group_name.lower()
        group_key = f"{group_kind}:{safe_name}"
    return {
        "group_key": group_key,
        "group_kind": group_kind,
        "group_name": group_name,
        "actor_uid": _payload_first_int(payload, ("actor_uid", "attacker_uid", "source_uid", "caster_uid", "player_uid")) or int(display.get("actor_uid") or 0),
        "target_uid": _payload_first_int(payload, ("target_uid", "target_uuid", "uuid", "victim_uid", "boss_uid", "boss_uuid", "combatant_id")) or int(display.get("target_uid") or 0),
        "skill_id": _payload_first_int(payload, ("skill_id", "skill_key", "skill", "skill_level_id", "skillLevelId")) or int(display.get("skill_id") or 0),
        "monster_id": _payload_first_int(payload, ("monster_id", "template_id", "target_template_id", "boss_id", "config_id")) or int(display.get("monster_id") or 0),
        "dungeon_id": _payload_first_int(payload, ("dungeon_id", "dungeon", "cur_map_id", "map_id")) or int(display.get("dungeon_id") or 0),
        "dungeon": _truthy_text(display.get("dungeon_name")) or _payload_first_text(payload, ("dungeon_name", "scene_name", "map_name")),
        "skill": _truthy_text(display.get("skill_name")) or _payload_first_text(payload, ("skill_name", "skill_display")),
        "monster": _truthy_text(display.get("monster_name")) or _payload_first_text(payload, ("monster_name", "target_name", "boss_name")),
    }


def _action_log_group_summary(rows: list[dict[str, Any]], key: str, *, limit: int = 6) -> list[dict[str, Any]]:
    groups: dict[str, dict[str, Any]] = {}
    for row in rows:
        label = str(row.get(key) or "-").strip() or "-"
        item = groups.setdefault(label, {
            "key": label,
            "count": 0,
            "total_value": 0.0,
            "first_time_ms": 0,
            "last_time_ms": 0,
        })
        time_ms = int(row.get("time_ms") or 0)
        item["count"] = int(item.get("count") or 0) + 1
        item["total_value"] = float(item.get("total_value") or 0.0) + _action_log_numeric_value(row)
        if time_ms:
            first = int(item.get("first_time_ms") or 0)
            item["first_time_ms"] = time_ms if not first else min(first, time_ms)
            item["last_time_ms"] = max(int(item.get("last_time_ms") or 0), time_ms)
    ordered = sorted(
        groups.values(),
        key=lambda item: (-int(item.get("count") or 0), -float(item.get("total_value") or 0.0), str(item.get("key") or "")),
    )
    out: list[dict[str, Any]] = []
    for item in ordered[:max(1, int(limit or 6))]:
        copied = dict(item)
        copied["total_value"] = round(float(copied.get("total_value") or 0.0), 3)
        out.append(copied)
    return out


def _append_unique(values: list[Any], value: Any, *, cap: int = 24) -> None:
    if value is None:
        return
    text = str(value or "").strip()
    if not text or text == "0" or text in {str(item) for item in values}:
        return
    if len(values) < cap:
        values.append(value)


def _action_log_detailed_groups(rows: list[dict[str, Any]], *, limit: int = 80,
                                details_per_group: int = 80) -> list[dict[str, Any]]:
    groups: dict[str, dict[str, Any]] = {}
    order: list[str] = []
    for row in rows:
        key = _truthy_text(row.get("group_key")) or f"{row.get('topic') or 'event'}:{row.get('label') or '-'}".lower()
        if key not in groups:
            groups[key] = {
                "key": key,
                "kind": _truthy_text(row.get("group_kind")) or _truthy_text(row.get("topic")) or "event",
                "name": _truthy_text(row.get("group_name")) or _truthy_text(row.get("label")) or "-",
                "count": 0,
                "total_value": 0.0,
                "first_time_ms": 0,
                "last_time_ms": 0,
                "uids": [],
                "uid_count": 0,
                "actor_uids": [],
                "target_uids": [],
                "skill_ids": [],
                "monster_ids": [],
                "dungeon_ids": [],
                "dungeons": [],
                "sources": [],
                "topics": [],
                "row_ids": [],
                "rows": [],
            }
            order.append(key)
        item = groups[key]
        time_ms = int(row.get("time_ms") or 0)
        item["count"] = int(item.get("count") or 0) + 1
        item["total_value"] = float(item.get("total_value") or 0.0) + _action_log_numeric_value(row)
        if time_ms:
            first = int(item.get("first_time_ms") or 0)
            item["first_time_ms"] = time_ms if not first else min(first, time_ms)
            item["last_time_ms"] = max(int(item.get("last_time_ms") or 0), time_ms)
        for field, out_key in (("actor_uid", "actor_uids"), ("target_uid", "target_uids"), ("skill_id", "skill_ids"), ("monster_id", "monster_ids"), ("dungeon_id", "dungeon_ids")):
            _append_unique(item[out_key], row.get(field))
        for uid_field in ("actor_uid", "target_uid"):
            _append_unique(item["uids"], row.get(uid_field))
        _append_unique(item["dungeons"], row.get("dungeon"))
        _append_unique(item["sources"], row.get("source"))
        _append_unique(item["topics"], row.get("topic"))
        _append_unique(item["row_ids"], row.get("id"), cap=details_per_group)
        if len(item["rows"]) < max(1, int(details_per_group or 80)):
            item["rows"].append(row)
    detailed = [groups[key] for key in order]
    for item in detailed:
        item["total_value"] = round(float(item.get("total_value") or 0.0), 3)
        if str(item.get("kind") or "") in {"monster", "target"}:
            item["uid_count"] = len(item.get("target_uids") or [])
        elif str(item.get("kind") or "") in {"actor", "actor_skill"}:
            item["uid_count"] = len(item.get("actor_uids") or [])
        else:
            item["uid_count"] = len(item.get("uids") or [])
        item["has_more_rows"] = int(item.get("count") or 0) > len(item.get("rows") or [])
    detailed.sort(
        key=lambda item: (-float(item.get("total_value") or 0.0), -int(item.get("count") or 0), str(item.get("name") or "")),
    )
    return detailed[:max(1, int(limit or 80))]


def _action_log_analytics(all_rows: list[dict[str, Any]], page_rows: list[dict[str, Any]],
                          *, offset: int, limit: int) -> dict[str, Any]:
    times = [int(row.get("time_ms") or 0) for row in all_rows if int(row.get("time_ms") or 0) >= 0]
    first_time = min(times) if times else 0
    last_time = max(times) if times else 0
    total_rows = len(all_rows)
    page_count = (total_rows + max(1, int(limit or 1)) - 1) // max(1, int(limit or 1)) if total_rows else 0
    page_index = (max(0, int(offset or 0)) // max(1, int(limit or 1))) + 1 if total_rows else 0
    return {
        "total_rows": total_rows,
        "page": {
            "offset": max(0, int(offset or 0)),
            "limit": max(1, int(limit or 1)),
            "row_count": len(page_rows),
            "page_index": page_index,
            "page_count": page_count,
            "has_previous": max(0, int(offset or 0)) > 0,
            "has_next": max(0, int(offset or 0)) + len(page_rows) < total_rows,
        },
        "time_range_ms": {
            "first": first_time,
            "last": last_time,
            "span": max(0, last_time - first_time),
        },
        "totals": {
            "value": round(sum(_action_log_numeric_value(row) for row in all_rows), 3),
        },
        "groups": {
            "topics": _action_log_group_summary(all_rows, "topic"),
            "actors": _action_log_group_summary(all_rows, "actor"),
            "targets": _action_log_group_summary(all_rows, "target"),
            "actions": _action_log_group_summary(all_rows, "label"),
            "detailed": _action_log_detailed_groups(all_rows),
        },
    }


def _mark_action_log_cursor(rows: list[dict[str, Any]], cursor_ms: int) -> tuple[list[dict[str, Any]], str]:
    if not rows:
        return rows, ""
    nearest = min(rows, key=lambda row: abs(int(row.get("time_ms") or 0) - int(cursor_ms or 0)))
    nearest_id = str(nearest.get("id") or "")
    for row in rows:
        row["is_cursor"] = str(row.get("id") or "") == nearest_id
    return rows, nearest_id


def act_action_log_status(owner: Any, *, limit: int = 80, query: str | None = None,
                          topic: str | None = None, cursor_ms: int | None = None,
                          source: str | None = None, encounter_id: str | None = None,
                          offset: int | None = None) -> dict[str, Any]:
    """Return searchable ACT action-log rows shared by WebView and Entity/Tk."""
    state = _action_log_state(owner)
    filters = dict(state.get("filters") or {})
    if query is not None:
        filters["query"] = str(query or "")
    if topic is not None:
        filters["topic"] = str(topic or "")
    if source is not None:
        filters["source"] = _normalize_action_log_source(source)
    else:
        filters["source"] = _normalize_action_log_source(filters.get("source"))
    if encounter_id is not None:
        filters["encounter_id"] = str(encounter_id or "")
    cursor = dict(state.get("cursor") or {})
    if cursor_ms is not None:
        cursor["time_ms"] = max(0, int(cursor_ms or 0))
    if offset is not None:
        cursor["offset"] = max(0, int(offset or 0))
    row_limit = max(1, min(int(limit or cursor.get("limit") or 80), 500))
    row_offset = max(0, int(cursor.get("offset") or 0))
    cursor["limit"] = row_limit
    cursor["offset"] = row_offset
    state["filters"] = filters
    state["cursor"] = cursor
    errors: list[str] = []
    storage_status: dict[str, Any] = {}
    source_mode = _normalize_action_log_source(filters.get("source"))
    if source_mode == "history":
        store, store_errors = _owner_history_store(owner)
        errors.extend(store_errors)
        raw_actions: list[Any] = []
        if store is not None:
            storage_status = _history_storage_status(store)
            list_actions = getattr(store, "list_sqlite_actions", None)
            if callable(list_actions):
                try:
                    fetch_limit = min(max(row_offset + row_limit * 4, row_limit), 5000)
                    raw_actions = list(_json_safe(list_actions(
                        limit=fetch_limit,
                        encounter_id=str(filters.get("encounter_id") or ""),
                    ) or []))
                except Exception as exc:
                    errors.append(str(exc))
            else:
                errors.append("SQLite action history query API is unavailable")
        rows = [_action_log_history_row(action, idx) for idx, action in enumerate(raw_actions) if isinstance(action, Mapping)]
        rows = _filter_action_log_rows(rows, query=str(filters.get("query") or ""), topic=str(filters.get("topic") or ""))
    else:
        # live 行是 (retained, 取段跨度, 过滤参数) 的纯函数 — 同 aggregate/
        # timeline 的新鲜度缓存; 无新事件时跳过 recent_events 深拷贝 +
        # 逐事件 compact + 过滤(query 非空时每行一次 json.dumps)。
        # is_cursor 由 _mark_action_log_cursor 对当前页逐行全量重写, 共享
        # dict 自纠正, 缓存安全。
        text_q = str(filters.get("query") or "")
        text_t = str(filters.get("topic") or "")
        span = min(row_offset + row_limit, 1000)
        _bus = None
        _rows_key = None
        try:
            _bus = ensure_act_event_bus(owner)
            _rows_key = (_bus.retained, span, text_q.strip().lower(), text_t.strip().lower())
        except Exception as exc:
            errors.append(str(exc))
        rows = None
        if _rows_key is not None:
            _cached = getattr(owner, "_act_action_log_rows_cache", None)
            if _cached is not None and _cached[0] == _rows_key:
                rows = _cached[1]
        if rows is None:
            raw_events: list = []
            if _bus is not None:
                try:
                    raw_events = _bus.recent_events(span)
                except Exception as exc:
                    errors.append(str(exc))
            rows = [_action_log_row(event, idx) for idx, event in enumerate(raw_events) if isinstance(event, Mapping)]
            rows = _filter_action_log_rows(rows, query=text_q, topic=text_t)
            if _rows_key is not None and not errors:
                try:
                    setattr(owner, "_act_action_log_rows_cache", (_rows_key, rows))
                except Exception:
                    pass
    total_rows = len(rows)
    if total_rows and row_offset >= total_rows:
        row_offset = ((total_rows - 1) // row_limit) * row_limit
        cursor["offset"] = row_offset
    page_rows = rows[row_offset:row_offset + row_limit]
    page_rows, nearest_id = _mark_action_log_cursor(page_rows, int(cursor.get("time_ms") or 0))
    page_count = (total_rows + row_limit - 1) // row_limit if total_rows else 0
    cursor.update({
        "nearest_row_id": nearest_id,
        "row_count": len(page_rows),
        "total_row_count": total_rows,
        "page_index": (row_offset // row_limit) + 1 if total_rows else 0,
        "page_count": page_count,
        "has_previous": row_offset > 0,
        "has_next": row_offset + len(page_rows) < total_rows,
    })
    analytics = _action_log_analytics(rows, page_rows, offset=row_offset, limit=row_limit)
    selected_encounter = str(filters.get("encounter_id") or "")
    if not selected_encounter and page_rows:
        selected_encounter = str(page_rows[0].get("encounter_id") or "")
    return {
        "ok": not errors,
        "message": "OK" if not errors else "; ".join(errors),
        "encounter_id": selected_encounter if source_mode == "history" else _timeline_encounter_id(owner, state),
        "source": source_mode,
        "rows": page_rows,
        "columns": list(_ACTION_LOG_COLUMNS),
        "filters": filters,
        "cursor": cursor,
        "analytics": analytics,
        "grouped_rows": _action_log_detailed_groups(rows, details_per_group=row_limit),
        "storage_status": storage_status,
        "errors": errors,
    }


def _act_aggregate_snapshot(owner: Any) -> dict[str, Any]:
    snap = _extension_runtime_value(owner, "owner_act_snapshot", {}, history_limit=20)
    if isinstance(snap, Mapping):
        return dict(_json_safe(snap))
    return {}


def act_aggregate_status(owner: Any, *, limit: int = 1000, query: str | None = "",
                         source: str | None = "live", window_ms: int = 1000,
                         top_n: int = 20, encounter_id: str | None = None,
                         group_by: str | None = "skill", group_field: str | None = "") -> dict[str, Any]:
    """Return semantic ACT aggregate groups for the primary cockpit panel.

    group_by selects the active workbench dimension (skill/monster/actor/topic/
    field) so plugin debuggers can re-group the same events without re-querying;
    group_field is the payload field name when group_by == 'field'.
    """
    errors: list[str] = []
    row_limit = max(1, min(int(limit or 1000), 5000))
    source_mode = _normalize_action_log_source(source)
    selected_encounter = str(encounter_id or "")
    group_by = str(group_by or "skill").strip().lower()
    group_field = str(group_field or "").strip()
    # Live-source cache: the aggregate is a pure function of the retained event
    # slice, so key it on the bus publish counter + params. This collapses the
    # menu's double call (refresh-signature + children-build) into one fold and
    # makes re-opening the menu after combat O(1) until new events arrive — the
    # full O(rows) pure-Python fold (~36ms over 240 events on the Tk thread) was
    # the cause of the "menu lags after combat / instant on empty cache" report.
    _cache_key = None
    if source_mode != "history":
        try:
            _bus = ensure_act_event_bus(owner)
            _cache_key = (_bus.retained, source_mode, str(query or ""),
                          int(window_ms or 1000), int(top_n or 20), row_limit,
                          selected_encounter, group_by, group_field)
            _cached = getattr(owner, "_act_aggregate_status_cache", None)
            if _cached is not None and _cached[0] == _cache_key:
                return _cached[1]
        except Exception:
            _cache_key = None
    rows: list[dict[str, Any]] = []
    storage_status: dict[str, Any] = {}
    if source_mode == "history":
        store, store_errors = _owner_history_store(owner)
        errors.extend(store_errors)
        raw_actions: list[Any] = []
        if store is not None:
            storage_status = _history_storage_status(store)
            list_actions = getattr(store, "list_sqlite_actions", None)
            if callable(list_actions):
                try:
                    raw_actions = list(_json_safe(list_actions(
                        limit=row_limit,
                        encounter_id=selected_encounter,
                    ) or []))
                except Exception as exc:
                    errors.append(str(exc))
            else:
                errors.append("SQLite action history query API is unavailable")
        rows = [_action_log_history_row(action, idx) for idx, action in enumerate(raw_actions) if isinstance(action, Mapping)]
    else:
        try:
            raw_events = ensure_act_event_bus(owner).recent_events(row_limit)
        except Exception as exc:
            raw_events = []
            errors.append(str(exc))
        rows = [_action_log_row(event, idx) for idx, event in enumerate(raw_events) if isinstance(event, Mapping)]
    rows = _filter_action_log_rows(rows, query=str(query or ""), topic="")
    # The aggregate is the COMBAT workbench — drop plugin/ui echoes so the same
    # skill isn't counted once under 实体 and again under a plugin source.
    rows = _combat_rows(rows)
    snapshot = _act_aggregate_snapshot(owner)
    render_spec = snapshot.get("render_spec") if isinstance(snapshot.get("render_spec"), Mapping) else {}
    try:
        summary = build_act_aggregate_summary(
            rows,
            render_spec=render_spec,
            window_ms=max(100, int(window_ms or 1000)),
            top_n=max(1, min(int(top_n or 20), 80)),
        )
    except Exception as exc:
        errors.append(str(exc))
        summary = build_act_aggregate_summary([], render_spec=render_spec)
    raw_counts = dict(summary.get("raw_counts") or {})
    raw_counts.update({"action_rows": len(rows), "limit": row_limit})
    try:
        active_groups = _aggregate_dimension(summary, rows, group_by, group_field,
                                             max(1, min(int(top_n or 20), 80)))
    except Exception as exc:
        errors.append(str(exc))
        active_groups = []
    result = {
        "ok": not errors,
        "message": "OK" if not errors else "; ".join(errors),
        "source": source_mode,
        "encounter_id": selected_encounter or str(snapshot.get("encounter_id") or (render_spec.get("encounter") or {}).get("id") or ""),
        "overview": summary.get("overview") or {},
        # Workbench dimension: the active group-by view + the selectable dimensions.
        "group_by": group_by,
        "group_field": group_field,
        "dimensions": list(ACT_AGGREGATE_DIMENSIONS),
        "groups": active_groups,
        "timeline_clusters": summary.get("timeline_clusters") or [],
        "skill_damage": summary.get("skill_damage") or [],
        "monster_damage": summary.get("monster_damage") or [],
        "dungeon_damage": summary.get("dungeon_damage") or [],
        "log_groups": summary.get("log_groups") or [],
        "source_mix": summary.get("source_mix") or [],
        "raw_counts": raw_counts,
        "filters": {
            "query": str(query or ""),
            "source": source_mode,
            "window_ms": max(100, int(window_ms or 1000)),
            "top_n": max(1, min(int(top_n or 20), 80)),
            "encounter_id": selected_encounter,
            "group_by": group_by,
            "group_field": group_field,
        },
        "storage_status": storage_status,
        "snapshot": snapshot,
        "errors": errors,
    }
    # Cache the live result (read-only by all callers) keyed on the bus counter.
    if _cache_key is not None and not errors:
        try:
            setattr(owner, "_act_aggregate_status_cache", (_cache_key, result))
        except Exception:
            pass
    return result


def act_action_log_search(owner: Any, *, query: str = "", limit: int = 80,
                          source: str | None = None, encounter_id: str | None = None,
                          offset: int = 0) -> dict[str, Any]:
    state = _action_log_state(owner)
    filters = dict(state.get("filters") or {})
    filters["query"] = str(query or "")
    if source is not None:
        filters["source"] = _normalize_action_log_source(source)
    if encounter_id is not None:
        filters["encounter_id"] = str(encounter_id or "")
    state["filters"] = filters
    cursor = dict(state.get("cursor") or {})
    cursor["offset"] = max(0, int(offset or 0))
    state["cursor"] = cursor
    return act_action_log_status(owner, limit=limit)


def act_action_log_filter(owner: Any, *, topic: str = "", query: str | None = None,
                          limit: int = 80, source: str | None = None,
                          encounter_id: str | None = None, offset: int = 0) -> dict[str, Any]:
    state = _action_log_state(owner)
    filters = dict(state.get("filters") or {})
    filters["topic"] = str(topic or "")
    if query is not None:
        filters["query"] = str(query or "")
    if source is not None:
        filters["source"] = _normalize_action_log_source(source)
    if encounter_id is not None:
        filters["encounter_id"] = str(encounter_id or "")
    state["filters"] = filters
    cursor = dict(state.get("cursor") or {})
    cursor["offset"] = max(0, int(offset or 0))
    state["cursor"] = cursor
    return act_action_log_status(owner, limit=limit)


def act_action_log_jump_to_time(owner: Any, *, cursor_ms: int = 0, limit: int = 80,
                                source: str | None = None, encounter_id: str | None = None,
                                offset: int | None = None,
                                topic: str | None = None) -> dict[str, Any]:
    state = _action_log_state(owner)
    cursor = dict(state.get("cursor") or {})
    cursor["time_ms"] = max(0, int(cursor_ms or 0))
    if offset is not None:
        cursor["offset"] = max(0, int(offset or 0))
    state["cursor"] = cursor
    return act_action_log_status(owner, limit=limit, source=source, encounter_id=encounter_id, topic=topic)


def act_action_log_copy(owner: Any, *, limit: int = 80, query: str = "",
                        topic: str = "", source: str | None = None,
                        encounter_id: str | None = None, offset: int | None = None) -> dict[str, Any]:
    status = act_action_log_status(owner, limit=limit, query=query, topic=topic, source=source, encounter_id=encounter_id, offset=offset)
    payload = {
        "encounter_id": status.get("encounter_id"),
        "source": status.get("source"),
        "rows": status.get("rows") or [],
        "columns": status.get("columns") or [],
        "filters": status.get("filters") or {},
        "cursor": status.get("cursor") or {},
        "analytics": status.get("analytics") or {},
        "grouped_rows": status.get("grouped_rows") or [],
        "storage_status": status.get("storage_status") or {},
    }
    try:
        text = json.dumps(payload, ensure_ascii=False, indent=2)
    except Exception:
        text = str(payload)
    return {
        "ok": bool(status.get("ok")),
        "message": status.get("message") or "OK",
        "encounter_id": payload["encounter_id"],
        "source": payload["source"],
        "text": text,
        "rows": payload["rows"],
        "columns": payload["columns"],
        "filters": payload["filters"],
        "cursor": payload["cursor"],
        "analytics": payload["analytics"],
        "grouped_rows": payload["grouped_rows"],
        "storage_status": payload["storage_status"],
        "errors": list(status.get("errors") or []),
    }


_DEATH_RECAP_COLUMNS = [
    {"key": "relative_ms", "label": "Delta", "width": 90},
    {"key": "time_ms", "label": "Time", "width": 90},
    {"key": "kind", "label": "Kind", "width": 140},
    {"key": "actor", "label": "Actor", "width": 160},
    {"key": "target", "label": "Target", "width": 160},
    {"key": "amount", "label": "Amount", "width": 110},
]


def _first_text(payload: Mapping[str, Any], keys: Iterable[str]) -> str:
    for key in keys:
        value = payload.get(key)
        if value is not None and str(value or "").strip():
            return str(value)
    return ""


def _first_int(payload: Mapping[str, Any], keys: Iterable[str]) -> int:
    for key in keys:
        value = payload.get(key)
        try:
            if value is not None and str(value or "").strip():
                return int(value)
        except Exception:
            continue
    return 0


def _death_target_id(payload: Mapping[str, Any]) -> int:
    return _first_int(payload, (
        "target_uid", "victim_uid", "combatant_id", "player_uid", "self_uid",
        "uid", "target_id", "victim_id",
    ))


def _death_target_name(payload: Mapping[str, Any]) -> str:
    return _first_text(payload, ("target", "victim", "combatant", "player", "name"))


def _is_death_signal(event: Mapping[str, Any]) -> bool:
    payload = event.get("payload") if isinstance(event.get("payload"), Mapping) else {}
    topic = str(event.get("topic") or "").lower()
    if "death" in topic or topic in {"dead", "defeat", "defeated", "player_dead"}:
        return True
    for key in ("is_dead", "dead", "death", "defeated"):
        if bool(payload.get(key)):
            return True
    hp = _first_int(payload, ("hp", "current_hp", "target_hp", "player_hp"))
    max_hp = _first_int(payload, ("max_hp", "target_max_hp", "player_max_hp"))
    return bool(max_hp > 0 and hp <= 0 and topic in {"self_state", "damage", "monster", "boss"})


def _matches_death_target(payload: Mapping[str, Any], target_id: int, target_name: str = "") -> bool:
    if not target_id and not target_name:
        return True
    if bool(payload.get("target_is_self") or payload.get("victim_is_self") or payload.get("is_self")):
        return True
    candidates = (
        "target_uid", "victim_uid", "combatant_id", "player_uid", "self_uid",
        "target_id", "victim_id", "uid",
    )
    if target_id and any(_first_int(payload, (key,)) == target_id for key in candidates):
        return True
    if target_name:
        text = _death_target_name(payload).lower()
        if text and text == target_name.lower():
            return True
    return False


def _death_recap_row(event: Mapping[str, Any], index: int, death_time_ms: int, target_id: int, target_name: str) -> dict[str, Any]:
    row = _action_log_row(event, index)
    payload = row.get("payload") if isinstance(row.get("payload"), Mapping) else {}
    topic = str(row.get("topic") or "").lower()
    amount = _first_int(payload, ("damage", "damage_total", "heal", "heal_total", "shield", "mitigation", "absorbed"))
    kind = topic or "event"
    if _is_death_signal(event):
        kind = "death"
    elif topic == "damage" and _matches_death_target(payload, target_id, target_name):
        kind = "incoming_damage"
    elif topic == "heal" and _matches_death_target(payload, target_id, target_name):
        kind = "healing"
    elif (
        ("shield" in topic or payload.get("shield") is not None or payload.get("absorbed") is not None)
        and _matches_death_target(payload, target_id, target_name)
    ):
        kind = "shield"
    elif (
        ("mitigation" in topic or payload.get("mitigation") is not None)
        and _matches_death_target(payload, target_id, target_name)
    ):
        kind = "mitigation"
    row.update({
        "relative_ms": int(row.get("time_ms") or 0) - int(death_time_ms or 0),
        "kind": kind,
        "amount": amount,
        "is_death": kind == "death",
    })
    return row


def _death_recap_summary(rows: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "event_count": len(rows),
        "incoming_damage": sum(int(row.get("amount") or 0) for row in rows if row.get("kind") == "incoming_damage"),
        "healing": sum(int(row.get("amount") or 0) for row in rows if row.get("kind") == "healing"),
        "shield": sum(int(row.get("amount") or 0) for row in rows if row.get("kind") == "shield"),
        "mitigation": sum(int(row.get("amount") or 0) for row in rows if row.get("kind") == "mitigation"),
        "death_events": sum(1 for row in rows if row.get("kind") == "death"),
    }


def act_death_recap_status(owner: Any, *, limit: int = 80, window_s: float = 8.0,
                           entity_id: Any = None) -> dict[str, Any]:
    """Return a compact death-recap window from recent ACT events."""
    row_limit = max(1, min(int(limit or 80), 500))
    window_ms = max(1000, int(float(window_s or 8.0) * 1000.0))
    requested_id = _coerce_int(entity_id, 0) if entity_id is not None else 0
    errors: list[str] = []
    try:
        raw_events = ensure_act_event_bus(owner).recent_events(max(row_limit * 4, 120))
    except Exception as exc:
        raw_events = []
        errors.append(str(exc))
    deaths: list[Mapping[str, Any]] = []
    for event in raw_events:
        if not isinstance(event, Mapping) or not _is_death_signal(event):
            continue
        payload = event.get("payload") if isinstance(event.get("payload"), Mapping) else {}
        if requested_id and _death_target_id(payload) != requested_id:
            continue
        deaths.append(event)
    if not deaths:
        return {
            "ok": not errors,
            "message": "No death event found" if not errors else "; ".join(errors),
            "encounter_id": _timeline_encounter_id(owner, {}),
            "death": None,
            "rows": [],
            "columns": list(_DEATH_RECAP_COLUMNS),
            "summary": _death_recap_summary([]),
            "window": {"before_ms": window_ms, "after_ms": min(2000, window_ms), "center_ms": 0},
            "filters": {"entity_id": str(entity_id or ""), "limit": row_limit, "window_s": float(window_s or 8.0)},
            "errors": errors,
        }
    death_event = deaths[0]
    death_payload = death_event.get("payload") if isinstance(death_event.get("payload"), Mapping) else {}
    death_compact = _compact_timeline_event(death_event, 0)
    death_time_ms = int(death_compact.get("time_ms") or 0)
    target_id = _death_target_id(death_payload) or requested_id
    target_name = _death_target_name(death_payload)
    start_ms = max(0, death_time_ms - window_ms)
    end_ms = death_time_ms + min(2000, window_ms)
    rows: list[dict[str, Any]] = []
    for idx, event in enumerate(raw_events):
        if not isinstance(event, Mapping):
            continue
        compact = _compact_timeline_event(event, idx)
        time_ms = int(compact.get("time_ms") or 0)
        if time_ms < start_ms or time_ms > end_ms:
            continue
        row = _death_recap_row(event, idx, death_time_ms, target_id, target_name)
        rows.append(row)
    rows.sort(key=lambda row: (int(row.get("time_ms") or 0), int(row.get("index") or 0)))
    rows = rows[-row_limit:]
    death = {
        "time_ms": death_time_ms,
        "entity_id": str(target_id or ""),
        "name": target_name,
        "topic": str(death_event.get("topic") or ""),
        "payload": _json_safe(death_payload),
    }
    return {
        "ok": not errors,
        "message": "OK" if not errors else "; ".join(errors),
        "encounter_id": _timeline_encounter_id(owner, {}),
        "death": death,
        "rows": rows,
        "columns": list(_DEATH_RECAP_COLUMNS),
        "summary": _death_recap_summary(rows),
        "window": {"before_ms": window_ms, "after_ms": min(2000, window_ms), "center_ms": death_time_ms},
        "filters": {"entity_id": str(entity_id or ""), "limit": row_limit, "window_s": float(window_s or 8.0)},
        "errors": errors,
    }


def act_death_recap_copy(owner: Any, *, limit: int = 80, window_s: float = 8.0,
                         entity_id: Any = None) -> dict[str, Any]:
    status = act_death_recap_status(owner, limit=limit, window_s=window_s, entity_id=entity_id)
    payload = {
        "encounter_id": status.get("encounter_id"),
        "death": status.get("death"),
        "summary": status.get("summary") or {},
        "rows": status.get("rows") or [],
        "window": status.get("window") or {},
        "filters": status.get("filters") or {},
    }
    try:
        text = json.dumps(payload, ensure_ascii=False, indent=2)
    except Exception:
        text = str(payload)
    return {
        "ok": bool(status.get("ok")),
        "message": status.get("message") or "OK",
        "encounter_id": payload["encounter_id"],
        "text": text,
        "death": payload["death"],
        "summary": payload["summary"],
        "rows": payload["rows"],
        "window": payload["window"],
        "filters": payload["filters"],
        "errors": list(status.get("errors") or []),
    }


_GRAPH_METRICS = [
    {"id": "damage", "label": "Damage", "kind": "cumulative", "unit": "damage"},
    {"id": "heal", "label": "Heal", "kind": "cumulative", "unit": "heal"},
    {"id": "event_count", "label": "Events", "kind": "cumulative", "unit": "events"},
    {"id": "target_hp_pct", "label": "Target HP %", "kind": "gauge", "unit": "pct"},
]


def _graph_timeseries_state(owner: Any) -> dict[str, Any]:
    state = getattr(owner, "_act_graph_timeseries_state", None)
    if not isinstance(state, dict):
        state = {"metric": "damage", "filters": {"query": "", "topic": ""}, "time_range_ms": 0}
        try:
            setattr(owner, "_act_graph_timeseries_state", state)
        except Exception:
            pass
    filters = state.get("filters")
    if not isinstance(filters, dict):
        filters = {"query": "", "topic": ""}
        state["filters"] = filters
    filters.setdefault("query", "")
    filters.setdefault("topic", "")
    state.setdefault("metric", "damage")
    state.setdefault("time_range_ms", 0)
    return state


def _normalize_graph_metric(metric: str | None) -> str:
    value = str(metric or "damage").strip().lower()
    allowed = {str(item["id"]) for item in _GRAPH_METRICS}
    return value if value in allowed else "damage"


def _event_target_hp_pct(payload: Mapping[str, Any]) -> float | None:
    for key in ("target_hp_pct", "hp_pct", "target_hp_est_pct"):
        if key in payload:
            value = _safe_float(payload.get(key), -1.0)
            if value < 0:
                continue
            return max(0.0, min(100.0, value * 100.0 if value <= 1.0 else value))
    hp = _safe_float(payload.get("hp") or payload.get("target_current_hp"), -1.0)
    total = _safe_float(payload.get("max_hp") or payload.get("target_total_hp"), -1.0)
    if hp >= 0 and total > 0:
        return max(0.0, min(100.0, hp * 100.0 / total))
    return None


def _graph_rows_from_events(raw_events: list[dict[str, Any]], *, query: str = "", topic: str = "") -> list[dict[str, Any]]:
    rows = [_action_log_row(event, idx) for idx, event in enumerate(raw_events) if isinstance(event, Mapping)]
    rows = _filter_action_log_rows(rows, query=query, topic=topic)
    # Drop plugin/ui echoes: otherwise the last-120-event window fills up with
    # plugin_ui_invalidate noise and the cumulative damage curve flatlines.
    rows = _combat_rows(rows)
    rows.sort(key=lambda row: (int(row.get("time_ms") or 0), int(row.get("index") or 0)))
    return rows


def _build_graph_series(rows: list[dict[str, Any]], selected_metric: str) -> dict[str, Any]:
    damage_total = 0.0
    heal_total = 0.0
    event_total = 0.0
    last_target_hp: float | None = None
    series: dict[str, Any] = {
        "damage": {"metric": "damage", "selected": selected_metric == "damage", "points": []},
        "heal": {"metric": "heal", "selected": selected_metric == "heal", "points": []},
        "event_count": {"metric": "event_count", "selected": selected_metric == "event_count", "points": []},
        "target_hp_pct": {"metric": "target_hp_pct", "selected": selected_metric == "target_hp_pct", "points": []},
    }
    for row in rows:
        payload = row.get("payload") if isinstance(row.get("payload"), Mapping) else {}
        topic = str(row.get("topic") or "")
        if topic == "damage" or "damage" in payload or "damage_total" in payload:
            damage_total += _safe_float(payload.get("damage") or payload.get("damage_total") or row.get("value"), 0.0)
        if topic == "heal" or "heal" in payload or "heal_total" in payload:
            heal_total += _safe_float(payload.get("heal") or payload.get("heal_total") or row.get("value"), 0.0)
        event_total += 1.0
        target_hp = _event_target_hp_pct(payload)
        if target_hp is not None:
            last_target_hp = target_hp
        point_base = {"time_ms": int(row.get("time_ms") or 0), "row_id": str(row.get("id") or ""), "topic": topic}
        series["damage"]["points"].append({**point_base, "value": float(damage_total)})
        series["heal"]["points"].append({**point_base, "value": float(heal_total)})
        series["event_count"]["points"].append({**point_base, "value": float(event_total)})
        if last_target_hp is not None:
            series["target_hp_pct"]["points"].append({**point_base, "value": float(last_target_hp)})
    return series


def act_graph_timeseries_status(owner: Any, *, metric: str | None = None, limit: int = 120, query: str | None = None, topic: str | None = None, time_range_ms: int | None = None) -> dict[str, Any]:
    """Return compact ACT graph/timeseries data shared by WebView and Entity/Tk."""
    state = _graph_timeseries_state(owner)
    if metric is not None:
        state["metric"] = _normalize_graph_metric(metric)
    selected_metric = _normalize_graph_metric(str(state.get("metric") or "damage"))
    filters = dict(state.get("filters") or {})
    if query is not None:
        filters["query"] = str(query or "")
    if topic is not None:
        filters["topic"] = str(topic or "")
    if time_range_ms is not None:
        state["time_range_ms"] = max(0, int(time_range_ms or 0))
    row_limit = max(1, min(int(limit or 120), 500))
    errors: list[str] = []
    # series/observed_range 是 (retained 计数 + 参数) 的纯函数 — 同 aggregate
    # 的新鲜度缓存: 无新事件时跳过 recent_events 深拷贝 + 整套 series 重建。
    # encounter_id 等轻字段不进缓存, 每次现算。
    _bus = None
    _cache_key = None
    try:
        _bus = ensure_act_event_bus(owner)
        _cache_key = (_bus.retained, selected_metric,
                      str(filters.get("query") or ""), str(filters.get("topic") or ""),
                      int(state.get("time_range_ms") or 0), row_limit)
    except Exception as exc:
        errors.append(str(exc))
    _hit = None
    if _cache_key is not None:
        _cached = getattr(owner, "_act_graph_series_cache", None)
        if _cached is not None and _cached[0] == _cache_key:
            _hit = _cached[1]
    if _hit is not None:
        series, row_count, observed_range = _hit
    else:
        raw_events: list = []
        if _bus is not None:
            try:
                raw_events = _bus.recent_events(row_limit)
            except Exception as exc:
                errors.append(str(exc))
        rows = _graph_rows_from_events(raw_events, query=str(filters.get("query") or ""), topic=str(filters.get("topic") or ""))
        if int(state.get("time_range_ms") or 0) > 0 and rows:
            end_ms = max(int(row.get("time_ms") or 0) for row in rows)
            start_ms = max(0, end_ms - int(state.get("time_range_ms") or 0))
            rows = [row for row in rows if int(row.get("time_ms") or 0) >= start_ms]
        series = _build_graph_series(rows, selected_metric)
        observed_range = 0
        if rows:
            observed_range = max(0, max(int(row.get("time_ms") or 0) for row in rows) - min(int(row.get("time_ms") or 0) for row in rows))
        row_count = len(rows)
        if _cache_key is not None and not errors:
            try:
                setattr(owner, "_act_graph_series_cache", (_cache_key, (series, row_count, observed_range)))
            except Exception:
                pass
    effective_range = int(state.get("time_range_ms") or 0) or observed_range
    state["filters"] = filters
    return {
        "ok": not errors,
        "message": "OK" if not errors else "; ".join(errors),
        "encounter_id": _timeline_encounter_id(owner, state),
        "selected_metric": selected_metric,
        "series": series,
        "metrics": list(_GRAPH_METRICS),
        "time_range_ms": int(effective_range),
        "filters": filters,
        "row_count": row_count,
        "errors": errors,
    }


def act_graph_timeseries_select_metric(owner: Any, *, metric: str = "damage", limit: int = 120) -> dict[str, Any]:
    state = _graph_timeseries_state(owner)
    state["metric"] = _normalize_graph_metric(metric)
    return act_graph_timeseries_status(owner, limit=limit)


def act_graph_timeseries_zoom(owner: Any, *, time_range_ms: int = 0, limit: int = 120) -> dict[str, Any]:
    state = _graph_timeseries_state(owner)
    state["time_range_ms"] = max(0, int(time_range_ms or 0))
    return act_graph_timeseries_status(owner, limit=limit)


def act_graph_timeseries_filter(owner: Any, *, query: str | None = None, topic: str | None = None, limit: int = 120) -> dict[str, Any]:
    state = _graph_timeseries_state(owner)
    filters = dict(state.get("filters") or {})
    if query is not None:
        filters["query"] = str(query or "")
    if topic is not None:
        filters["topic"] = str(topic or "")
    state["filters"] = filters
    return act_graph_timeseries_status(owner, limit=limit)


def act_graph_timeseries_export(owner: Any, *, metric: str | None = None, limit: int = 120, query: str | None = None, topic: str | None = None) -> dict[str, Any]:
    status = act_graph_timeseries_status(owner, metric=metric, limit=limit, query=query, topic=topic)
    payload = {
        "encounter_id": status.get("encounter_id"),
        "selected_metric": status.get("selected_metric"),
        "series": status.get("series") or {},
        "metrics": status.get("metrics") or [],
        "time_range_ms": status.get("time_range_ms") or 0,
        "filters": status.get("filters") or {},
    }
    try:
        text = json.dumps(payload, ensure_ascii=False, indent=2)
    except Exception:
        text = str(payload)
    return {
        "ok": bool(status.get("ok")),
        "message": status.get("message") or "OK",
        "text": text,
        "selected_metric": payload["selected_metric"],
        "series": payload["series"],
        "metrics": payload["metrics"],
        "time_range_ms": payload["time_range_ms"],
        "filters": payload["filters"],
        "errors": list(status.get("errors") or []),
    }


def _combatant_drilldown_state(owner: Any) -> dict[str, Any]:
    state = getattr(owner, "_act_combatant_drilldown_state", None)
    if not isinstance(state, dict):
        state = {"combatant_id": "", "filters": {"query": "", "focus_target": ""}}
        try:
            setattr(owner, "_act_combatant_drilldown_state", state)
        except Exception:
            pass
    filters = state.get("filters")
    if not isinstance(filters, dict):
        filters = {"query": "", "focus_target": ""}
        state["filters"] = filters
    filters.setdefault("query", "")
    filters.setdefault("focus_target", "")
    state.setdefault("combatant_id", "")
    return state


def _find_combatant_detail(owner: Any, combatant_id: str) -> tuple[dict[str, Any] | None, list[str]]:
    errors: list[str] = []
    try:
        uid = int(str(combatant_id or "0"), 0)
    except Exception:
        uid = 0
    if uid:
        detail = _extension_runtime_value(owner, "combatant_detail", None, uid)
        if isinstance(detail, Mapping):
            return dict(_json_safe(detail)), errors
    report = _owner_dps_report(owner)
    for row in list((report or {}).get("entities") or []):
        if isinstance(row, Mapping) and str(row.get("uid") or row.get("id") or "") == str(combatant_id):
            return dict(_json_safe(row)), errors
    return None, errors or [f"combatant not found: {combatant_id}"]


def _combatant_summary(detail: Mapping[str, Any]) -> dict[str, Any]:
    damage = int(detail.get("damage") or detail.get("damage_total") or 0)
    heal = int(detail.get("heal") or detail.get("heal_total") or 0)
    return {
        "uid": str(detail.get("uid") or detail.get("id") or ""),
        "name": str(detail.get("name") or detail.get("display_name") or "Unknown"),
        "profession": str(detail.get("profession") or detail.get("profession_name") or ""),
        "damage": damage,
        "heal": heal,
        "dps": int(detail.get("dps") or 0),
        "hps": int(detail.get("hps") or 0),
        "damage_pct": float(detail.get("damage_pct") or 0.0),
        "crit_rate": float(detail.get("crit_rate") or 0.0),
        "is_self": bool(detail.get("is_self")),
    }


def _combatant_skill_rows(detail: Mapping[str, Any], *, query: str = "") -> list[dict[str, Any]]:
    text = str(query or "").strip().lower()
    rows: list[dict[str, Any]] = []
    for index, skill in enumerate(list(detail.get("skills") or []), 1):
        if not isinstance(skill, Mapping):
            continue
        damage = int(skill.get("total") or skill.get("damage") or skill.get("damage_total") or 0)
        heal = int(skill.get("heal_total") or skill.get("heal") or 0)
        is_heal = heal > damage
        amount = heal if is_heal else damage
        row = {
            "rank": index,
            "skill_id": str(skill.get("skill_id") or skill.get("id") or ""),
            "source_skill_id": str(skill.get("source_skill_id") or skill.get("raw_skill_key") or skill.get("skill_key") or skill.get("stats_key") or skill.get("id") or ""),
            "base_skill_id": str(skill.get("base_skill_id") or skill.get("semantic_base_skill_id") or skill.get("semantic_skill_id") or ""),
            "semantic_skill_id": str(skill.get("semantic_skill_id") or skill.get("base_skill_id") or skill.get("semantic_base_skill_id") or ""),
            "skill_level_id": str(skill.get("skill_level_id") or ""),
            "skill_uuid": str(skill.get("skill_uuid") or ""),
            "name": _resolve_skill_name_from_detail(skill) or str(skill.get("skill_name") or skill.get("name") or skill.get("skill_id") or "Unknown Skill"),
            "kind": "heal" if is_heal else "damage",
            "amount": int(amount),
            "damage": damage,
            "heal": heal,
            "hits": int(skill.get("heal_hits" if is_heal else "hits") or skill.get("hits") or 0),
            "crit_rate": float(skill.get("crit_rate") or 0.0),
        }
        if text and text not in json.dumps(row, ensure_ascii=False, default=str).lower():
            continue
        rows.append(row)
    rows.sort(key=lambda item: int(item.get("amount") or 0), reverse=True)
    for rank, row in enumerate(rows, 1):
        row["rank"] = rank
    return rows


def act_combatant_drilldown_status(owner: Any, *, combatant_id: str | int | None = None, query: str | None = None, focus_target: str | int | None = None) -> dict[str, Any]:
    """Return one combatant drilldown payload shared by WebView and Entity/Tk."""
    state = _combatant_drilldown_state(owner)
    if combatant_id is not None:
        state["combatant_id"] = str(combatant_id or "")
    filters = dict(state.get("filters") or {})
    if query is not None:
        filters["query"] = str(query or "")
    if focus_target is not None:
        filters["focus_target"] = str(focus_target or "")
    state["filters"] = filters
    cid = str(state.get("combatant_id") or "")
    if not cid:
        return {
            "ok": True,
            "message": "No combatant selected",
            "encounter_id": _timeline_encounter_id(owner, state),
            "combatant_id": "",
            "summary": {},
            "skills": [],
            "incoming": [],
            "outgoing": [],
            "filters": filters,
            "errors": [],
        }
    detail, errors = _find_combatant_detail(owner, cid)
    if detail is None:
        return {
            "ok": False,
            "message": "; ".join(errors),
            "encounter_id": _timeline_encounter_id(owner, state),
            "combatant_id": cid,
            "summary": {},
            "skills": [],
            "incoming": [],
            "outgoing": [],
            "filters": filters,
            "errors": errors,
        }
    skills = _combatant_skill_rows(detail, query=str(filters.get("query") or ""))
    outgoing = [{"kind": row.get("kind"), "name": row.get("name"), "amount": row.get("amount"), "hits": row.get("hits")} for row in skills[:12]]
    incoming = list(_json_safe(detail.get("incoming") or [])) if isinstance(detail.get("incoming"), list) else []
    return {
        "ok": True,
        "message": "OK",
        "encounter_id": _timeline_encounter_id(owner, state),
        "combatant_id": cid,
        "summary": _combatant_summary(detail),
        "skills": skills,
        "incoming": incoming,
        "outgoing": outgoing,
        "filters": filters,
        "errors": errors,
    }


def act_combatant_drilldown_filter(owner: Any, *, combatant_id: str | int | None = None, query: str = "") -> dict[str, Any]:
    return act_combatant_drilldown_status(owner, combatant_id=combatant_id, query=query)


def act_combatant_drilldown_focus_target(owner: Any, *, combatant_id: str | int | None = None, target_id: str | int = "") -> dict[str, Any]:
    return act_combatant_drilldown_status(owner, combatant_id=combatant_id, focus_target=target_id)


def act_combatant_drilldown_back(owner: Any) -> dict[str, Any]:
    state = _combatant_drilldown_state(owner)
    state["combatant_id"] = ""
    return act_combatant_drilldown_status(owner)


def _skill_drilldown_state(owner: Any) -> dict[str, Any]:
    state = getattr(owner, "_act_skill_drilldown_state", None)
    if not isinstance(state, dict):
        state = {"combatant_id": "", "skill_id": "", "filters": {"query": ""}}
        try:
            setattr(owner, "_act_skill_drilldown_state", state)
        except Exception:
            pass
    filters = state.get("filters")
    if not isinstance(filters, dict):
        filters = {"query": ""}
        state["filters"] = filters
    filters.setdefault("query", "")
    state.setdefault("combatant_id", "")
    state.setdefault("skill_id", "")
    return state


def _normalize_skill_id(value: Any) -> str:
    return str(value or "").strip()


def _skill_id_candidates(*values: Any) -> set[str]:
    candidates: set[str] = set()
    for value in values:
        if isinstance(value, Mapping):
            for key in (
                "skill_id", "id", "source_skill_id", "raw_skill_key", "skill_key",
                "stats_key", "base_skill_id", "semantic_base_skill_id", "semantic_skill_id",
                "skill_level_id", "skill_uuid",
            ):
                candidates.update(_skill_id_candidates(value.get(key)))
            fact = value.get("combat_fact")
            if isinstance(fact, Mapping):
                candidates.update(_skill_id_candidates(fact))
            continue
        text = _normalize_skill_id(value)
        if not text:
            continue
        candidates.add(text)
        try:
            num = int(text, 0)
        except Exception:
            num = None
        if num is None or num <= 0:
            continue
        dec = str(num)
        candidates.add(dec)
        if len(dec) > 8:
            # DPS/Cython can expose a composite damage key such as
            # 110048200100. Keep the raw key, but also match embedded
            # base ids like 1004820 and common leveled/effect suffix forms.
            for start in range(0, max(0, len(dec) - 5)):
                for width in (8, 7, 6):
                    if start + width <= len(dec):
                        part = dec[start:start + width]
                        if part and not part.startswith("0"):
                            candidates.add(str(int(part)))
            for trim in (2, 3, 4):
                if len(dec) > trim:
                    base = dec[:-trim]
                    if base and not base.startswith("0"):
                        candidates.add(str(int(base)))
        elif len(dec) > 4:
            for trim in (1, 2, 3):
                if len(dec) > trim + 3:
                    base = dec[:-trim]
                    if base and not base.startswith("0"):
                        candidates.add(str(int(base)))
    return {item for item in candidates if item}


def _find_skill_detail(detail: Mapping[str, Any], skill_id: str) -> dict[str, Any] | None:
    target_ids = _skill_id_candidates(skill_id)
    for skill in list(detail.get("skills") or []):
        if not isinstance(skill, Mapping):
            continue
        current_ids = _skill_id_candidates(skill)
        if target_ids and current_ids and target_ids.intersection(current_ids):
            return dict(_json_safe(skill))
    return None


def _skill_summary(skill: Mapping[str, Any]) -> dict[str, Any]:
    damage = int(skill.get("total") or skill.get("damage") or skill.get("damage_total") or 0)
    heal = int(skill.get("heal_total") or skill.get("heal") or 0)
    is_heal = heal > damage
    amount = heal if is_heal else damage
    sid = _normalize_skill_id(skill.get("skill_id") or skill.get("id"))
    source_sid = _normalize_skill_id(skill.get("source_skill_id") or skill.get("raw_skill_key") or skill.get("skill_key") or skill.get("stats_key"))
    base_sid = _normalize_skill_id(skill.get("base_skill_id") or skill.get("semantic_base_skill_id") or skill.get("semantic_skill_id"))
    return {
        "skill_id": sid,
        "source_skill_id": source_sid,
        "base_skill_id": base_sid,
        "semantic_skill_id": _normalize_skill_id(skill.get("semantic_skill_id") or base_sid),
        "skill_level_id": _normalize_skill_id(skill.get("skill_level_id")),
        "skill_uuid": _normalize_skill_id(skill.get("skill_uuid")),
        "candidate_skill_ids": sorted(_skill_id_candidates(skill)),
        "name": _resolve_skill_name_from_detail(skill) or str(skill.get("skill_name") or skill.get("name") or skill.get("skill_id") or "Unknown Skill"),
        "kind": "heal" if is_heal else "damage",
        "amount": int(amount),
        "damage": damage,
        "heal": heal,
    }


def _skill_timeline_refs(owner: Any, *, skill_id: str, query: str = "", limit: int = 80) -> list[dict[str, Any]]:
    text = str(query or "").strip().lower()
    target_ids = _skill_id_candidates(skill_id)
    try:
        raw_events = ensure_act_event_bus(owner).recent_events(limit)
    except Exception:
        raw_events = []
    refs: list[dict[str, Any]] = []
    for idx, event in enumerate(raw_events):
        if not isinstance(event, Mapping):
            continue
        row = _action_log_row(event, idx)
        payload = row.get("payload") if isinstance(row.get("payload"), Mapping) else {}
        current_ids = _skill_id_candidates(payload)
        matched_ids = target_ids.intersection(current_ids)
        if not target_ids or not current_ids or not matched_ids:
            continue
        ref = {
            "id": str(row.get("id") or ""),
            "time_ms": int(row.get("time_ms") or 0),
            "topic": str(row.get("topic") or ""),
            "label": str(row.get("label") or ""),
            "value": row.get("value") or "",
            "matched_skill_ids": sorted(matched_ids),
            "payload": _json_safe(payload),
        }
        if text and text not in json.dumps(ref, ensure_ascii=False, default=str).lower():
            continue
        refs.append(ref)
    refs.sort(key=lambda item: int(item.get("time_ms") or 0))
    return refs


def act_skill_drilldown_status(owner: Any, *, combatant_id: str | int | None = None, skill_id: str | int | None = None, query: str | None = None, limit: int = 80) -> dict[str, Any]:
    """Return one skill drilldown payload shared by WebView and Entity/Tk."""
    state = _skill_drilldown_state(owner)
    if combatant_id is not None:
        state["combatant_id"] = str(combatant_id or "")
    if skill_id is not None:
        state["skill_id"] = _normalize_skill_id(skill_id)
    filters = dict(state.get("filters") or {})
    if query is not None:
        filters["query"] = str(query or "")
    state["filters"] = filters
    cid = str(state.get("combatant_id") or "")
    sid = _normalize_skill_id(state.get("skill_id"))
    if not cid or not sid:
        return {
            "ok": True,
            "message": "No skill selected",
            "encounter_id": _timeline_encounter_id(owner, state),
            "combatant_id": cid,
            "skill_id": sid,
            "summary": {},
            "casts": 0,
            "hits": 0,
            "crit_rate": 0.0,
            "timeline_refs": [],
            "filters": filters,
            "errors": [],
        }
    detail, errors = _find_combatant_detail(owner, cid)
    skill = _find_skill_detail(detail or {}, sid) if detail else None
    if skill is None:
        msg = f"skill not found: {sid}"
        errors = errors or [msg]
        if msg not in errors:
            errors.append(msg)
        return {
            "ok": False,
            "message": "; ".join(errors),
            "encounter_id": _timeline_encounter_id(owner, state),
            "combatant_id": cid,
            "skill_id": sid,
            "summary": {},
            "casts": 0,
            "hits": 0,
            "crit_rate": 0.0,
            "timeline_refs": [],
            "filters": filters,
            "errors": errors,
        }
    summary = _skill_summary(skill)
    hits = int(skill.get("heal_hits" if summary.get("kind") == "heal" else "hits") or skill.get("hits") or 0)
    timeline_refs = list(_json_safe(skill.get("timeline_refs") or [])) if isinstance(skill.get("timeline_refs"), list) else []
    query_text = str(filters.get("query") or "").strip().lower()
    if query_text:
        timeline_refs = [
            ref for ref in timeline_refs
            if query_text in json.dumps(ref, ensure_ascii=False, default=str).lower()
        ]
    live_refs = _skill_timeline_refs(owner, skill_id=sid, query=str(filters.get("query") or ""), limit=limit)
    timeline_refs.extend(live_refs)
    timeline_refs.sort(key=lambda item: int(item.get("time_ms") or 0) if isinstance(item, Mapping) else 0)
    return {
        "ok": True,
        "message": "OK",
        "encounter_id": _timeline_encounter_id(owner, state),
        "combatant_id": cid,
        "skill_id": sid,
        "summary": summary,
        "casts": int(skill.get("casts") or skill.get("cast_count") or max(1, hits if hits else 0)),
        "hits": hits,
        "crit_rate": float(skill.get("crit_rate") or 0.0),
        "timeline_refs": timeline_refs,
        "filters": filters,
        "errors": errors,
    }


def act_skill_drilldown_filter(owner: Any, *, combatant_id: str | int | None = None, skill_id: str | int | None = None, query: str = "", limit: int = 80) -> dict[str, Any]:
    return act_skill_drilldown_status(owner, combatant_id=combatant_id, skill_id=skill_id, query=query, limit=limit)


def act_skill_drilldown_copy(owner: Any, *, combatant_id: str | int | None = None, skill_id: str | int | None = None, query: str | None = None, limit: int = 80) -> dict[str, Any]:
    status = act_skill_drilldown_status(owner, combatant_id=combatant_id, skill_id=skill_id, query=query, limit=limit)
    payload = {
        "encounter_id": status.get("encounter_id"),
        "combatant_id": status.get("combatant_id"),
        "skill_id": status.get("skill_id"),
        "summary": status.get("summary") or {},
        "casts": status.get("casts") or 0,
        "hits": status.get("hits") or 0,
        "crit_rate": status.get("crit_rate") or 0.0,
        "timeline_refs": status.get("timeline_refs") or [],
        "filters": status.get("filters") or {},
    }
    try:
        text = json.dumps(payload, ensure_ascii=False, indent=2)
    except Exception:
        text = str(payload)
    return {"ok": bool(status.get("ok")), "message": status.get("message") or "OK", "text": text, **payload, "errors": list(status.get("errors") or [])}


def act_skill_drilldown_back(owner: Any) -> dict[str, Any]:
    state = _skill_drilldown_state(owner)
    state["combatant_id"] = ""
    state["skill_id"] = ""
    return act_skill_drilldown_status(owner)


def _report_rows_from_snapshot(snapshot: Mapping[str, Any], report: Mapping[str, Any] | None) -> list[dict[str, Any]]:
    render_spec = snapshot.get("render_spec") if isinstance(snapshot, Mapping) else {}
    rows = render_spec.get("rows") if isinstance(render_spec, Mapping) else []
    if not rows and isinstance(report, Mapping):
        rows = report.get("entities") or []
    out: list[dict[str, Any]] = []
    for index, row in enumerate(rows or [], 1):
        if not isinstance(row, Mapping):
            continue
        out.append({
            "rank": int(row.get("rank") or index),
            "uid": int(row.get("uid") or 0),
            "name": str(row.get("name") or ""),
            "profession": str(row.get("profession") or ""),
            "damage": int(row.get("damage") or row.get("damage_total") or 0),
            "heal": int(row.get("heal") or row.get("heal_total") or 0),
            "dps": int(row.get("dps") or 0),
            "hps": int(row.get("hps") or 0),
            "damage_pct": float(row.get("damage_pct") or 0.0),
            "is_self": bool(row.get("is_self")),
        })
    return out


def _report_preview(snapshot: Mapping[str, Any], report: Mapping[str, Any] | None) -> dict[str, Any]:
    render_spec = snapshot.get("render_spec") if isinstance(snapshot, Mapping) else {}
    if not isinstance(render_spec, Mapping):
        render_spec = {}
    encounter = render_spec.get("encounter") if isinstance(render_spec.get("encounter"), Mapping) else {}
    totals = render_spec.get("totals") if isinstance(render_spec.get("totals"), Mapping) else {}
    live = snapshot.get("live") if isinstance(snapshot.get("live"), Mapping) else {}
    report = report if isinstance(report, Mapping) else {}
    rows = _report_rows_from_snapshot(snapshot, report)
    total_damage = int(totals.get("damage") or live.get("total_damage") or report.get("total_damage") or 0)
    total_heal = int(totals.get("heal") or live.get("total_heal") or report.get("total_heal") or 0)
    elapsed_s = float(totals.get("elapsed_s") or encounter.get("duration_s") or report.get("elapsed_s") or 0.0)
    return {
        "title": str(render_spec.get("title") or report.get("report_reason") or "Last Encounter"),
        "encounter_id": str(encounter.get("id") or report.get("encounter_id") or ""),
        "status": str(encounter.get("status") or ("report" if report else "empty")),
        "elapsed_s": elapsed_s,
        "total_damage": total_damage,
        "total_heal": total_heal,
        "total_dps": int(totals.get("dps") or report.get("total_dps") or (total_damage / elapsed_s if elapsed_s > 0 else 0)),
        "total_hps": int(totals.get("hps") or report.get("total_hps") or (total_heal / elapsed_s if elapsed_s > 0 else 0)),
        "combatant_count": len(rows),
        "top_rows": rows[:8],
    }


def _normalize_export_format(fmt: str | None) -> str:
    value = str(fmt or "json").strip().lower()
    return value if value in {"json", "csv", "html", "xml", "xml.gz", "xml.zip"} else "json"


def act_report_status(owner: Any, *, limit: int = 20, fmt: str = "json") -> dict[str, Any]:
    """Return export-ready report status and preview for both ACT UIs."""
    selected = _normalize_export_format(fmt)
    errors: list[str] = []
    store = _extension_runtime_value(owner, "owner_history_store", None)
    if store is None:
        errors.append("DPS history is not initialized")
    snapshot = _owner_act_snapshot(owner, history_limit=limit)
    report = _owner_dps_report(owner)
    history: list[Any] = []
    if store is not None:
        list_reports = getattr(store, "list_reports", None)
        if callable(list_reports):
            try:
                history = list(_json_safe(list_reports(int(limit or 20)) or []))
            except Exception as exc:
                errors.append(str(exc))
    if not report and not history:
        errors.append("No DPS report is available")
    preview = _report_preview(snapshot, report)
    encounter_id = str(preview.get("encounter_id") or "latest")
    history_payload = act_history_status(owner, limit=limit)
    if history_payload.get("storage_status", {}).get("available"):
        history = list(history_payload.get("encounters") or history)
    return {
        "ok": bool(report or history) and not errors,
        "message": "OK" if not errors else "; ".join(errors),
        "encounter_id": encounter_id,
        "formats": ["json", "csv", "html", "xml", "xml.gz", "xml.zip"],
        "selected_format": selected,
        "preview": preview,
        "history": history,
        "storage_status": {
            "available": bool(store is not None),
            "count": len(history),
            "path": str(getattr(store, "path", "") or ""),
        },
        "errors": errors,
    }


def act_report_export(owner: Any, *, fmt: str = "json") -> dict[str, Any]:
    """Save the latest ACT/DPS report using the existing DpsHistoryStore exporter."""
    selected = _normalize_export_format(fmt)
    store = _extension_runtime_value(owner, "owner_history_store", None)
    if store is None:
        return {"ok": False, "message": "DPS history is not initialized.", "errors": ["DPS history is not initialized"], "selected_format": selected}
    export = getattr(store, "export_report", None)
    if not callable(export):
        return {"ok": False, "message": "DPS history exporter is unavailable.", "errors": ["DPS history exporter is unavailable"], "selected_format": selected}
    report = _owner_dps_report(owner)
    try:
        path = export(report=report, fmt=selected)
    except Exception as exc:
        return {"ok": False, "message": str(exc), "errors": [str(exc)], "selected_format": selected}
    if not path:
        return {"ok": False, "message": "No report to export.", "errors": ["No report to export"], "selected_format": selected}
    status = act_report_status(owner, fmt=selected)
    status.update({"ok": True, "message": "Exported", "path": str(path), "selected_format": selected, "errors": []})
    return status


def act_report_copy(owner: Any, *, fmt: str = "json") -> dict[str, Any]:
    """Return a clipboard-friendly JSON report payload without writing a file."""
    selected = _normalize_export_format(fmt)
    status = act_report_status(owner, fmt=selected)
    payload = {
        "encounter_id": status.get("encounter_id"),
        "format": selected,
        "preview": status.get("preview") or {},
        "history": status.get("history") or [],
    }
    try:
        text = json.dumps(payload, ensure_ascii=False, indent=2)
    except Exception:
        text = str(payload)
    return {
        "ok": bool(status.get("ok")),
        "message": status.get("message") or "OK",
        "text": text,
        "selected_format": selected,
        "preview": status.get("preview") or {},
        "errors": list(status.get("errors") or []),
    }


def _mini_parse_registry(owner: Any):
    from .mini_parse import build_default_registry

    registry = getattr(owner, "_act_mini_parse_registry", None)
    if registry is None:
        registry = build_default_registry()
        try:
            setattr(owner, "_act_mini_parse_registry", registry)
        except Exception:
            pass
    return registry


def _mini_parse_payload(owner: Any, *, limit: int = 20) -> dict[str, Any]:
    status = act_report_status(owner, limit=limit, fmt="json")
    return {
        "encounter_id": status.get("encounter_id"),
        "preview": status.get("preview") or {},
        "history": status.get("history") or [],
        "report": _owner_dps_report(owner),
        "status": status,
    }


def _invoke_plugin_formatter(owner: Any, formatter_id: str, payload: Mapping[str, Any]) -> dict[str, Any] | None:
    try:
        manager = ensure_act_plugin_manager(owner, load=True)
    except Exception:
        return None
    invoke = getattr(manager, "invoke_extension", None)
    if not callable(invoke):
        return None
    try:
        result = invoke("formatters", formatter_id, dict(payload), time_budget_ms=25.0)
    except Exception as exc:
        return {"ok": False, "message": str(exc), "errors": [str(exc)]}
    if not isinstance(result, Mapping) or not bool(result.get("ok")):
        return dict(result) if isinstance(result, Mapping) else None
    formatted = result.get("result")
    if isinstance(formatted, Mapping):
        text = str(formatted.get("text") or formatted.get("value") or "")
    else:
        text = str(formatted or "")
    return {"ok": True, "text": text, "plugin": dict(result)}


def _formatter_status_rows(owner: Any) -> list[dict[str, Any]]:
    rows = _mini_parse_registry(owner).list_formatters()
    try:
        manager = ensure_act_plugin_manager(owner, load=True)
        status = manager.status()
        for item in status.get("extensions", {}).get("formatters", []) or []:
            if isinstance(item, Mapping):
                rows.append({
                    "id": str(item.get("id") or ""),
                    "title": str(item.get("title") or item.get("display_name") or item.get("id") or ""),
                    "description": str(item.get("description") or ""),
                    "plugin_id": str(item.get("plugin_id") or ""),
                })
    except Exception:
        pass
    seen: set[str] = set()
    out: list[dict[str, Any]] = []
    for row in rows:
        fmt_id = str(row.get("id") or "")
        if not fmt_id or fmt_id in seen:
            continue
        seen.add(fmt_id)
        out.append(row)
    return out


def act_mini_parse_status(owner: Any, *, formatter_id: str = "summary_table", limit: int = 20) -> dict[str, Any]:
    formatters = _formatter_status_rows(owner)
    ids = {str(row.get("id") or "") for row in formatters}
    selected = str(formatter_id or "summary_table").strip().lower().replace(" ", "_")
    if selected not in ids:
        selected = "summary_table"
    auto_copy = bool(_settings_get(owner, "act_mini_parse_auto_copy", False))
    payload = _mini_parse_payload(owner, limit=limit)
    return {
        "ok": True,
        "message": "OK",
        "formatter_id": selected,
        "formatters": formatters,
        "preview": payload.get("preview") or {},
        "auto_copy": auto_copy,
        "last_text": str(getattr(owner, "_act_mini_parse_last_text", "") or ""),
        "errors": [],
    }


def act_mini_parse_preview(owner: Any, *, formatter_id: str = "summary_table", limit: int = 20) -> dict[str, Any]:
    payload = _mini_parse_payload(owner, limit=limit)
    selected = str(formatter_id or "summary_table").strip().lower().replace(" ", "_")
    builtin_ids = {str(row.get("id") or "") for row in _mini_parse_registry(owner).list_formatters()}
    plugin_result = None if selected in builtin_ids else _invoke_plugin_formatter(owner, selected, payload)
    errors: list[str] = []
    if isinstance(plugin_result, Mapping) and plugin_result.get("ok"):
        text = str(plugin_result.get("text") or "")
    elif isinstance(plugin_result, Mapping) and plugin_result.get("errors"):
        errors.extend(str(err) for err in plugin_result.get("errors") or [])
        text = _mini_parse_registry(owner).format(selected, payload)
    else:
        text = _mini_parse_registry(owner).format(selected, payload)
    try:
        setattr(owner, "_act_mini_parse_last_text", text)
    except Exception:
        pass
    return {
        "ok": not errors,
        "message": "OK" if not errors else "; ".join(errors),
        "formatter_id": selected if selected else "summary_table",
        "text": text,
        "preview": payload.get("preview") or {},
        "errors": errors,
        "status": act_mini_parse_status(owner, formatter_id=selected, limit=limit),
    }


def act_mini_parse_copy(owner: Any, *, formatter_id: str = "summary_table", limit: int = 20) -> dict[str, Any]:
    result = act_mini_parse_preview(owner, formatter_id=formatter_id, limit=limit)
    result["copied"] = False
    return result


def _source_active(source: Mapping[str, Any]) -> bool:
    if not source:
        return False
    for key in ("running", "alive", "active", "started", "is_memory_active"):
        if key in source:
            return bool(source.get(key))
    status = str(source.get("status") or source.get("mode") or "").lower()
    return bool(status and status not in ("error", "missing", "stopped", "disabled", "off"))


def _owner_data_source(owner: Any) -> str:
    settings = _owner_settings(owner)
    default = ""
    try:
        if isinstance(settings, Mapping):
            return str(settings.get("data_source", default) or default).lower()
        if settings is not None:
            return str(settings.get("data_source", default) or default).lower()
    except Exception:
        pass
    return default


def act_data_source_health(owner: Any, *, now: float | None = None) -> dict[str, Any]:
    """Return a parity-friendly data source health payload for both UIs."""
    now_ts = float(now if now is not None else time.time())
    engine = _extension_runtime_value(owner, "owner_packet_bridge", None)
    errors: list[str] = []
    raw: dict[str, Any] = {}
    if engine is None:
        errors.append("no packet engine")
    else:
        health = getattr(engine, "health", None)
        if callable(health):
            try:
                raw = dict(health() or {})
            except Exception as exc:
                errors.append(str(exc))
        else:
            errors.append("packet engine has no health()")
    raw = _json_safe(raw if isinstance(raw, Mapping) else {})
    packet = dict(raw)
    memory = {}
    if isinstance(packet.get("mem"), Mapping):
        memory = dict(packet.pop("mem") or {})
    packet.setdefault("data_source", str(packet.get("data_source") or _owner_data_source(owner) or "unknown"))
    parser_selection = packet.get("parser_adapter_selection") if isinstance(packet.get("parser_adapter_selection"), Mapping) else {}

    packet_active = _source_active(packet)
    memory_active = _source_active(memory)
    sources: dict[str, Any] = {}
    if packet or engine is not None:
        sources["packet"] = packet
    if memory:
        memory.setdefault("data_source", str(memory.get("data_source") or "memory"))
        sources["memory"] = memory
    sources["summary"] = {
        "data_source": packet.get("data_source") or _owner_data_source(owner),
        "primary": "packet" if packet else ("memory" if memory else "none"),
        "hybrid": bool(packet and memory),
        "packet_active": packet_active,
        "memory_active": memory_active,
        "fallbacks": ["memory"] if packet and memory else [],
        "parser_adapter_id": str(parser_selection.get("selected_id") or ""),
        "parser_adapter_requested_id": str(parser_selection.get("requested_id") or ""),
        "parser_adapter_mode": str(parser_selection.get("mode") or ""),
        "parser_adapter_plugin_id": str(parser_selection.get("plugin_id") or ""),
        "parser_adapter_fallback_reason": str(parser_selection.get("fallback_reason") or ""),
    }

    last_update = float(getattr(engine, "_last_update_t", 0.0) or 0.0) if engine is not None else 0.0
    last_raw = float(getattr(engine, "_last_capture_raw_seen_ts", 0.0) or 0.0) if engine is not None else 0.0
    latency_ms = int(max(0.0, (now_ts - last_raw) * 1000.0)) if last_raw else 0
    last_event_ms = int(max(0.0, (now_ts - last_update) * 1000.0)) if last_update else 0

    error_msg = str(packet.get("error_msg") or packet.get("last_error") or "").strip()
    if error_msg:
        errors.append(error_msg)
    if engine is None:
        status = "missing"
    elif errors:
        status = "error"
    elif packet_active or memory_active:
        status = "running"
    else:
        status = "stopped"
    return {
        "ok": bool(engine is not None and not errors),
        "available": bool(engine is not None),
        "status": status,
        "sources": sources,
        "latency_ms": latency_ms,
        "last_event_ms": last_event_ms,
        "errors": errors,
        "requested_mode": _owner_data_source(owner),
        "recognition_active": bool(getattr(owner, "_recognition_active", False)),
        "generated_at": now_ts,
    }


def act_data_source_diagnose(owner: Any, *, now: float | None = None) -> dict[str, Any]:
    """Return health plus user-facing diagnostic hints."""
    payload = act_data_source_health(owner, now=now)
    diagnostics: list[dict[str, str]] = []
    if not payload.get("available"):
        diagnostics.append({"level": "error", "message": "PacketBridge is not started"})
    elif payload.get("errors"):
        for err in payload.get("errors") or []:
            diagnostics.append({"level": "error", "message": str(err)})
    else:
        diagnostics.append({"level": "info", "message": "Data source health is nominal"})
    sources = payload.get("sources") or {}
    summary = sources.get("summary") or {}
    if summary.get("hybrid") and not summary.get("memory_active"):
        diagnostics.append({"level": "warn", "message": "Hybrid mode has memory fallback configured but inactive"})
    if summary.get("parser_adapter_fallback_reason"):
        diagnostics.append({"level": "warn", "message": f"Parser adapter fallback: {summary.get('parser_adapter_fallback_reason')}"})
    if int(payload.get("last_event_ms") or 0) > 30000:
        diagnostics.append({"level": "warn", "message": "No player update for more than 30s"})
    out = dict(payload)
    out["diagnostics"] = diagnostics
    return out


# ── Memory-scan access (read-only facade for plugins + the Mem Scope panel) ────

class _NullMemAccess:
    """Fallback when game plugin hasn't registered mem_access."""
    def __getattr__(self, name):
        def _noop(**kw):
            return {"ok": False, "reason": "no_plugin", "hint": "游戏插件未加载"}
        return _noop


def _mem_access(owner: Any):
    try:
        from mem_probe.mem_access import MemAccess
        return MemAccess(owner)
    except ImportError:
        return _NullMemAccess()


def act_mem_status(owner: Any, **_: Any) -> dict[str, Any]:
    return _mem_access(owner).status()


def act_mem_catalog(owner: Any, **_: Any) -> dict[str, Any]:
    return _mem_access(owner).catalog()


def act_mem_self(owner: Any, **_: Any) -> dict[str, Any]:
    return _mem_access(owner).self_state()


def act_mem_entities(owner: Any, *, include_monsters: bool = True,
                     include_npcs: bool = False, max_per_dict: int = 128, **_: Any) -> dict[str, Any]:
    return _mem_access(owner).entities(include_monsters=bool(include_monsters),
                                       include_npcs=bool(include_npcs),
                                       max_per_dict=int(max_per_dict or 128))


def act_mem_boss(owner: Any, **_: Any) -> dict[str, Any]:
    return _mem_access(owner).boss()


def act_mem_boss_actions(owner: Any, **_: Any) -> dict[str, Any]:
    return _mem_access(owner).boss_actions()


def act_mem_boss_action(owner: Any, *, uuid: Any = 0, **_: Any) -> dict[str, Any]:
    return _mem_access(owner).boss_action(uuid)


def act_mem_damage(owner: Any, *, total_type: int = 1, **_: Any) -> dict[str, Any]:
    return _mem_access(owner).damage_totals(total_type=int(total_type or 1))


def act_mem_skill_damage(owner: Any, *, uuid: Any = 0, **_: Any) -> dict[str, Any]:
    return _mem_access(owner).skill_damage(uuid)


def act_mem_attr_map(owner: Any, *, ent_addr: Any = 0, **_: Any) -> dict[str, Any]:
    return _mem_access(owner).attr_map(ent_addr)


def act_mem_resolve_name(owner: Any, *, kind: str = "monster", id: Any = 0, **_: Any) -> dict[str, Any]:
    return _mem_access(owner).resolve_name(str(kind or "monster"), id)


def act_mem_read(owner: Any, *, addr: Any = 0, dtype: str = "u64", **_: Any) -> dict[str, Any]:
    return _mem_access(owner).read_at(addr, str(dtype or "u64"))


def act_mem_read_many(owner: Any, *, addrs: Any = (), dtype: str = "u64", **_: Any) -> dict[str, Any]:
    return _mem_access(owner).read_many(addrs if isinstance(addrs, (list, tuple)) else (), str(dtype or "u64"))


def act_mem_search(owner: Any, *, value: Any = None, dtype: str = "i32", align: int = 0, **_: Any) -> dict[str, Any]:
    return _mem_access(owner).search(value, str(dtype or "i32"), align=int(align or 0))


def act_mem_search_status(owner: Any, *, job_id: str = "", **_: Any) -> dict[str, Any]:
    return _mem_access(owner).search_status(str(job_id or ""))


def act_mem_narrow(owner: Any, *, job_id: str = "", value: Any = None, **_: Any) -> dict[str, Any]:
    return _mem_access(owner).narrow(str(job_id or ""), value)


def act_mem_search_cancel(owner: Any, *, job_id: str = "", **_: Any) -> dict[str, Any]:
    return _mem_access(owner).search_cancel(str(job_id or ""))


def act_mem_search_list(owner: Any, **_: Any) -> dict[str, Any]:
    return _mem_access(owner).search_list()


def act_mem_scope_status(owner: Any, *, query: str = "", dtype: str = "i32",
                         job_id: str = "", **_: Any) -> dict[str, Any]:
    """One-shot Mem Scope panel refresh: status + catalog + live data (+ search job)."""
    ma = _mem_access(owner)
    status = ma.status()
    out: dict[str, Any] = {
        "ok": True, "status": status, "catalog": ma.catalog(),
        "query": str(query or ""), "dtype": str(dtype or "i32"), "job_id": str(job_id or ""),
    }
    if status.get("active"):
        out["self"] = ma.self_state()
        out["entities"] = ma.entities()
        out["damage"] = ma.damage_totals()
        if job_id:
            out["search"] = ma.search_status(str(job_id))
    return out


__all__ = [
    "act_mem_status",
    "act_mem_catalog",
    "act_mem_self",
    "act_mem_entities",
    "act_mem_boss",
    "act_mem_boss_actions",
    "act_mem_boss_action",
    "act_mem_damage",
    "act_mem_skill_damage",
    "act_mem_attr_map",
    "act_mem_resolve_name",
    "act_mem_read",
    "act_mem_read_many",
    "act_mem_search",
    "act_mem_search_status",
    "act_mem_narrow",
    "act_mem_search_cancel",
    "act_mem_search_list",
    "act_mem_scope_status",
    "act_combatant_drilldown_back",
    "act_combatant_drilldown_filter",
    "act_combatant_drilldown_focus_target",
    "act_combatant_drilldown_status",
    "act_skill_drilldown_back",
    "act_skill_drilldown_copy",
    "act_skill_drilldown_filter",
    "act_skill_drilldown_status",
    "act_action_log_copy",
    "act_action_log_filter",
    "act_action_log_jump_to_time",
    "act_action_log_search",
    "act_action_log_status",
    "act_death_recap_copy",
    "act_death_recap_status",
    "act_graph_timeseries_export",
    "act_graph_timeseries_filter",
    "act_graph_timeseries_select_metric",
    "act_graph_timeseries_status",
    "act_graph_timeseries_zoom",
    "act_history_delete",
    "act_history_load",
    "act_history_status",
    "act_mini_parse_copy",
    "act_mini_parse_preview",
    "act_mini_parse_status",
    "act_offline_import_file",
    "act_offline_import_status",
    "act_report_copy",
    "act_report_export",
    "act_report_status",
    "act_timeline_filter",
    "act_timeline_pause",
    "act_timeline_play",
    "act_timeline_seek",
    "act_timeline_set_speed",
    "act_timeline_status",
    "act_timeline_step",
    "act_plugin_disable",
    "act_plugin_enable",
    "act_plugin_list",
    "act_plugin_reload",
    "act_plugin_status",
    "act_plugin_import",
    "act_plugin_import_dialog",
    "act_plugin_uninstall",
    "act_open_workshop",
    "act_plugin_menu",
    "act_plugin_script_menus",
    "act_plugin_menu_surfaces",
    "act_plugin_action",
    "act_plugin_pin",
    "act_plugin_hotkeys",
    "act_plugin_hotkey_dispatch",
    "act_plugin_set_hotkey",
    "act_plugin_ui_panels",
    "act_plugin_ui_render",
    "act_plugin_ui_action",
    "render_surfaces",
    "render_apply_hooks",
    "render_overlays",
    "render_surface",
    "act_selective_parsing_clear",
    "act_selective_parsing_status",
    "act_selective_parsing_update",
    "act_data_source_diagnose",
    "act_data_source_health",
    "act_trigger_disable",
    "act_trigger_enable",
    "act_trigger_export_presets",
    "act_trigger_import_presets",
    "act_trigger_reload",
    "act_trigger_status",
    "act_trigger_test",
    "build_plugin_manager",
    "default_plugin_dirs",
    "ensure_act_event_bus",
    "ensure_act_plugin_manager",
    "project_base_dir",
    "publish_owner_event",
    "register_extension_runtime",
    "register_webview_extension",
    "should_record_owner_combat_event",
    "shutdown_act_plugin_manager",
]
