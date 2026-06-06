# -*- coding: utf-8 -*-
"""Helpers for wiring ACT platform services into both UI runtimes."""

from __future__ import annotations

import os
import sys
import json
import time
from typing import Any, Callable, Iterable, Mapping, Optional

from engines.act_aggregate import build_act_aggregate_summary

from .event_bus import EventBus
from .plugins import PluginManager
from .selective_parsing import normalize_policy, should_record_event


def default_plugin_dirs(base_dir: str) -> list[str]:
    return [
        os.path.join(base_dir, "plugins"),
        os.path.join(base_dir, "user_plugins"),
    ]


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


def _read_trigger_rules(owner: Any) -> list[dict[str, Any]]:
    settings = _owner_settings(owner)
    try:
        raw = settings.get("act_trigger_rules", []) if settings is not None else []
    except Exception:
        raw = []
    if isinstance(raw, Mapping):
        raw = raw.get("act_trigger_rules") or raw.get("rules") or []
    if not isinstance(raw, list):
        return []
    return [dict(rule) for rule in raw if isinstance(rule, Mapping)]


def _write_trigger_rules(owner: Any, rules: list[dict[str, Any]]) -> None:
    _settings_set(owner, "act_trigger_rules", rules)
    try:
        setattr(owner, "_act_trigger_last_reload_ms", int(time.time() * 1000))
    except Exception:
        pass
    try:
        engine = ensure_act_trigger_engine(owner)
        engine.set_rules(rules)
    except Exception:
        pass


def _normalize_trigger_rules(rules: Iterable[Mapping[str, Any]]) -> list[dict[str, Any]]:
    from engines.act_trigger_engine import normalize_trigger_rule

    return [normalize_trigger_rule(rule, idx) for idx, rule in enumerate(rules or [], 1)]


def _plugin_trigger_result(invoked: Mapping[str, Any]) -> dict[str, Any]:
    out = dict(invoked or {})
    result = out.get("result")
    if isinstance(result, Mapping):
        for key in ("message", "severity", "label"):
            if result.get(key) is not None:
                out[key] = result.get(key)
        if "matched" in result:
            out["matched"] = bool(result.get("matched"))
        elif "match" in result:
            out["matched"] = bool(result.get("match"))
        elif "result" in result:
            out["matched"] = bool(result.get("result"))
        else:
            out.setdefault("matched", False)
    else:
        out["matched"] = bool(result)
    return out


def _plugin_trigger_evaluator(owner: Any):
    def _evaluate(trigger_type: str, payload: Mapping[str, Any], time_budget_ms: float = 25.0) -> dict[str, Any]:
        manager = ensure_act_plugin_manager(owner)
        invoke = getattr(manager, "invoke_extension", None)
        if not callable(invoke):
            return {"ok": False, "matched": False, "message": "plugin extension invocation is unavailable"}
        invoked = invoke(
            "trigger_types",
            str(trigger_type or ""),
            payload,
            time_budget_ms=float(time_budget_ms or 25.0),
        )
        return _plugin_trigger_result(invoked if isinstance(invoked, Mapping) else {})

    return _evaluate


def _configure_trigger_engine_plugins(owner: Any, engine: Any) -> None:
    setter = getattr(engine, "set_plugin_trigger_evaluator", None)
    if callable(setter):
        try:
            setter(_plugin_trigger_evaluator(owner))
        except Exception:
            pass


def ensure_act_trigger_engine(owner: Any):
    from engines.act_trigger_engine import ActTriggerEngine

    engine = getattr(owner, "_act_trigger_engine", None)
    if isinstance(engine, ActTriggerEngine):
        _configure_trigger_engine_plugins(owner, engine)
        return engine
    engine = ActTriggerEngine(
        _read_trigger_rules(owner),
        plugin_trigger_evaluator=_plugin_trigger_evaluator(owner),
    )
    try:
        setattr(owner, "_act_trigger_engine", engine)
    except Exception:
        pass
    _configure_trigger_engine_plugins(owner, engine)
    return engine


def _owner_snapshot_provider(owner: Any) -> Callable[[], Mapping[str, Any]]:
    def _snapshot() -> Mapping[str, Any]:
        for name in ("_build_dps_act_snapshot", "_get_dps_act_snapshot"):
            fn = getattr(owner, name, None)
            if callable(fn):
                try:
                    return fn() or {}
                except Exception:
                    return {}
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
    )
    manager.discover()
    return manager


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
    tracker = getattr(owner, "_dps_tracker", None) or getattr(owner, "dps_tracker", None)
    for attr in ("_self_uid", "self_uid"):
        try:
            value = int(getattr(tracker, attr) or 0)
            if value > 0:
                return value
        except Exception:
            pass
    state = getattr(owner, "_state_mgr", None) or getattr(owner, "state_mgr", None)
    gs = None
    getter = getattr(state, "get", None)
    if callable(getter):
        try:
            gs = getter()
        except Exception:
            gs = None
    if gs is None:
        gs = getattr(state, "state", None)
    for attr in ("player_id", "uid", "self_uid"):
        try:
            value = int(getattr(gs, attr) or 0)
            if value > 0:
                return value
        except Exception:
            pass
    return 0


def _party_ids(owner: Any) -> set[int]:
    ids: set[int] = set()
    bridge = getattr(owner, "_packet_engine", None) or getattr(owner, "_packet_bridge", None)
    get_players = getattr(bridge, "get_players", None)
    if callable(get_players):
        try:
            players = get_players() or {}
        except Exception:
            players = {}
        if isinstance(players, Mapping):
            for key, value in players.items():
                try:
                    ids.add(int(key))
                except Exception:
                    pass
                if isinstance(value, Mapping):
                    for field in ("uid", "id", "player_id"):
                        try:
                            ids.add(int(value.get(field) or 0))
                        except Exception:
                            pass
                    continue
                for attr in ("uid", "id", "player_id"):
                    try:
                        ids.add(int(getattr(value, attr) or 0))
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
    decision = should_record_event(
        event,
        _read_selective_policy(owner),
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


def act_plugin_menu(owner: Any) -> dict[str, Any]:
    """Data for the plugin popup menu (Entity + WebView): manager + plugin list."""
    try:
        manager = ensure_act_plugin_manager(owner, load=True)
    except Exception as exc:
        return {"ok": False, "message": str(exc), "items": []}
    status = manager.status()
    panels_by_plugin: dict[str, list[str]] = {}
    for panel in manager.list_ui_panels():
        panels_by_plugin.setdefault(str(panel.get("plugin_id") or ""), []).append(str(panel.get("id") or ""))
    items: list[dict[str, Any]] = [
        {"type": "manage", "id": "__manage__", "label": "插件管理面板 Plugin Manager"},
    ]
    for plug in status.get("plugins", []):
        pid = str(plug.get("id") or "")
        items.append({
            "type": "plugin",
            "id": pid,
            "label": str(plug.get("name") or pid),
            "enabled": bool(plug.get("enabled")),
            "active": bool(plug.get("active")),
            "panels": panels_by_plugin.get(pid, []),
        })
    return {
        "ok": True,
        "count": int(status.get("plugin_count", 0) or 0),
        "active_count": int(status.get("active_count", 0) or 0),
        "items": items,
    }


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

def act_render_surfaces(owner: Any) -> dict[str, Any]:
    """Report which surfaces have plugin hooks/overlays attached."""
    try:
        manager = ensure_act_plugin_manager(owner, load=True)
        return {"ok": True, **manager.render_status()}
    except Exception as exc:
        return {"ok": False, "message": str(exc), "surfaces": {}}


def act_render_apply_hooks(owner: Any, surface: str, payload: Any = None) -> dict[str, Any]:
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


def act_render_overlays(owner: Any, surface: str) -> dict[str, Any]:
    """Return plugin overlay specs for ``surface`` (drawn over native content)."""
    try:
        manager = ensure_act_plugin_manager(owner)
        return {"ok": True, "surface": str(surface or ""),
                "overlays": manager.surface_overlays(surface)}
    except Exception as exc:
        return {"ok": False, "surface": str(surface or ""), "message": str(exc), "overlays": []}


def act_render_surface(owner: Any, surface: str, payload: Any = None) -> dict[str, Any]:
    """One-shot helper: apply hooks *and* collect overlays for ``surface``."""
    hooked = act_render_apply_hooks(owner, surface, payload)
    overlays = act_render_overlays(owner, surface)
    return {
        "ok": bool(hooked.get("ok") and overlays.get("ok")),
        "surface": str(surface or ""),
        "payload": hooked.get("payload"),
        "override": hooked.get("override"),
        "overlays": overlays.get("overlays", []),
    }


def _trigger_status_from_rules(owner: Any, rules: list[dict[str, Any]], *,
                               ok: bool = True, message: str = "") -> dict[str, Any]:
    normalized = _normalize_trigger_rules(rules)
    timers = [rule for rule in normalized if rule.get("type") in ("elapsed_s", "timer_preset")]
    errors: list[str] = []
    engine_snapshot: dict[str, Any] = {}
    try:
        engine_snapshot = ensure_act_trigger_engine(owner).snapshot(limit=12)
    except Exception as exc:
        errors.append(str(exc))
    return {
        "ok": bool(ok) and not errors,
        "message": message,
        "enabled": bool(normalized),
        "rule_count": len(normalized),
        "trigger_count": len(normalized),
        "timer_count": len(timers),
        "triggers": normalized,
        "timers": timers,
        "recent": list(engine_snapshot.get("recent") or []),
        "last_reload_ms": int(getattr(owner, "_act_trigger_last_reload_ms", 0) or 0),
        "errors": errors,
    }


def act_trigger_status(owner: Any) -> dict[str, Any]:
    try:
        return _trigger_status_from_rules(owner, _read_trigger_rules(owner), message="OK")
    except Exception as exc:
        return {
            "ok": False,
            "message": str(exc),
            "enabled": False,
            "rule_count": 0,
            "trigger_count": 0,
            "timer_count": 0,
            "triggers": [],
            "timers": [],
            "recent": [],
            "last_reload_ms": 0,
            "errors": [str(exc)],
        }


def _set_trigger_rule_enabled(owner: Any, rule_id: str, enabled: bool) -> dict[str, Any]:
    target = str(rule_id or "").strip()
    if not target:
        return {"ok": False, "message": "rule id is required"}
    rules = _read_trigger_rules(owner)
    found = False
    normalized = _normalize_trigger_rules(rules)
    for index, rule in enumerate(rules):
        current_id = str((normalized[index] if index < len(normalized) else rule).get("id") or "")
        if current_id == target:
            rule["enabled"] = bool(enabled)
            found = True
            break
    if not found:
        return {"ok": False, "message": f"trigger rule not found: {target}"}
    _write_trigger_rules(owner, rules)
    action = "enabled" if enabled else "disabled"
    status = _trigger_status_from_rules(owner, rules, message=f"{target} {action}")
    status["ok"] = True
    return status


def act_trigger_enable(owner: Any, rule_id: str) -> dict[str, Any]:
    return _set_trigger_rule_enabled(owner, rule_id, True)


def act_trigger_disable(owner: Any, rule_id: str) -> dict[str, Any]:
    return _set_trigger_rule_enabled(owner, rule_id, False)


def act_trigger_reload(owner: Any) -> dict[str, Any]:
    try:
        rules = _read_trigger_rules(owner)
        try:
            ensure_act_trigger_engine(owner).set_rules(rules)
        except Exception:
            pass
        try:
            setattr(owner, "_act_trigger_last_reload_ms", int(time.time() * 1000))
        except Exception:
            pass
        return _trigger_status_from_rules(owner, rules, message="Reloaded")
    except Exception as exc:
        return {"ok": False, "message": str(exc), "errors": [str(exc)], "triggers": [], "timers": []}


def _rules_from_preset_payload(payload: Any) -> list[dict[str, Any]]:
    raw = payload
    if isinstance(raw, Mapping):
        raw = raw.get("act_trigger_rules") or raw.get("rules") or raw.get("presets") or []
    if not isinstance(raw, list):
        return []
    return [dict(rule) for rule in raw if isinstance(rule, Mapping)]


def act_trigger_export_presets(owner: Any,
                               rule_ids: Optional[Iterable[str]] = None) -> dict[str, Any]:
    try:
        wanted = {str(item or "").strip() for item in (rule_ids or []) if str(item or "").strip()}
        rules = _normalize_trigger_rules(_read_trigger_rules(owner))
        if wanted:
            rules = [rule for rule in rules if str(rule.get("id") or "") in wanted]
        return {
            "ok": True,
            "kind": "act_trigger_presets",
            "schema_version": 1,
            "count": len(rules),
            "rules": rules,
        }
    except Exception as exc:
        return {"ok": False, "message": str(exc), "rules": [], "errors": [str(exc)]}


def act_trigger_import_presets(owner: Any, payload: Any, *,
                               replace: bool = False) -> dict[str, Any]:
    try:
        incoming = _normalize_trigger_rules(_rules_from_preset_payload(payload))
        if not incoming:
            return {"ok": False, "message": "no trigger presets found", "imported_count": 0}
        existing = _normalize_trigger_rules(_read_trigger_rules(owner))
        if replace:
            combined = incoming
        else:
            by_id = {str(rule.get("id") or ""): dict(rule) for rule in existing}
            order = [str(rule.get("id") or "") for rule in existing if str(rule.get("id") or "")]
            for rule in incoming:
                rule_id = str(rule.get("id") or "")
                if rule_id and rule_id not in by_id:
                    order.append(rule_id)
                by_id[rule_id] = dict(rule)
            combined = [by_id[rule_id] for rule_id in order if rule_id in by_id]
        _write_trigger_rules(owner, combined)
        status = _trigger_status_from_rules(owner, combined, message="Imported trigger presets")
        status["ok"] = True
        status["imported_count"] = len(incoming)
        status["replace"] = bool(replace)
        return status
    except Exception as exc:
        return {"ok": False, "message": str(exc), "imported_count": 0, "errors": [str(exc)]}


def _assign_path(src: dict[str, Any], path: str, value: Any) -> None:
    parts = [part for part in str(path or "").split(".") if part]
    if not parts:
        return
    cur = src
    for part in parts[:-1]:
        nxt = cur.get(part)
        if not isinstance(nxt, dict):
            nxt = {}
            cur[part] = nxt
        cur = nxt
    cur[parts[-1]] = value


def _synthetic_snapshot_for_rule(rule: Mapping[str, Any]) -> dict[str, Any]:
    rule_type = str(rule.get("type") or "")
    threshold = float(rule.get("threshold") or 0.0)
    match = str(rule.get("match") or "")
    render_spec: dict[str, Any] = {
        "mode": "synthetic",
        "encounter": {"id": "act-trigger-test", "status": "active", "duration_s": max(1.0, threshold + 1.0)},
        "totals": {"damage": 0.0, "heal": 0.0, "elapsed_s": max(1.0, threshold + 1.0)},
        "context": {},
        "boss": {},
    }
    if rule_type == "damage_total":
        render_spec["totals"]["damage"] = max(1.0, threshold + 1.0)
    elif rule_type == "heal_total":
        render_spec["totals"]["heal"] = max(1.0, threshold + 1.0)
    elif rule_type == "boss_hp_pct_below":
        render_spec["boss"]["hp_pct"] = min(max(threshold, 0.0), 1.0)
    elif rule_type == "skill_kind":
        render_spec["context"]["last_skill_kind"] = match or "server_end"
    elif rule_type == "boss_event_type":
        try:
            event_type = int(match or threshold or 1)
        except Exception:
            event_type = 1
        render_spec["context"]["last_boss_event_type"] = event_type
    elif rule_type == "timer_preset":
        duration_s = float(rule.get("duration_s") or threshold or 1.0)
        render_spec["totals"]["elapsed_s"] = max(1.0, duration_s + 1.0)
        render_spec["encounter"]["duration_s"] = max(1.0, duration_s + 1.0)
    elif rule_type == "plugin_trigger":
        render_spec["totals"]["damage"] = max(1.0, threshold + 1.0)
    snapshot = {"render_spec": render_spec}
    if rule_type == "field_match":
        field = str(rule.get("field") or "context.last_skill_kind")
        value: Any = match or "server_end"
        operator = str(rule.get("operator") or "")
        if operator in ("lt", "lte", "<", "<="):
            value = float(rule.get("threshold") or 0.0) - 1.0
        elif operator in ("gt", "gte", ">", ">="):
            value = float(rule.get("threshold") or 0.0) + 1.0
        if field.startswith("render_spec."):
            _assign_path(snapshot, field, value)
        elif field.startswith("source."):
            _assign_path(snapshot, field, value)
        else:
            _assign_path(render_spec, field, value)
    return snapshot



def act_trigger_test(owner: Any, rule_id: str) -> dict[str, Any]:
    target = str(rule_id or "").strip()
    if not target:
        return {"ok": False, "message": "rule id is required", "events": []}
    try:
        from engines.act_trigger_engine import ActTriggerEngine

        rules = _normalize_trigger_rules(_read_trigger_rules(owner))
        rule = next((item for item in rules if str(item.get("id") or "") == target), None)
        if rule is None:
            return {"ok": False, "message": f"trigger rule not found: {target}", "events": []}
        enabled_rule = dict(rule)
        enabled_rule["enabled"] = True
        enabled_rule["cooldown_s"] = 0.0
        enabled_rule["once_per_encounter"] = False
        engine = ActTriggerEngine(
            [enabled_rule],
            plugin_trigger_evaluator=_plugin_trigger_evaluator(owner),
        )
        events = engine.evaluate(_synthetic_snapshot_for_rule(enabled_rule), now=time.time())
        return {
            "ok": bool(events),
            "message": "Test event emitted" if events else "Rule did not match synthetic snapshot",
            "events": events,
            "status": act_trigger_status(owner),
        }
    except Exception as exc:
        return {"ok": False, "message": str(exc), "events": [], "errors": [str(exc)]}


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
    for name in ("_build_dps_act_snapshot", "_get_dps_act_snapshot"):
        fn = getattr(owner, name, None)
        if callable(fn):
            try:
                snap = fn(history_limit=history_limit)
            except TypeError:
                try:
                    snap = fn()
                except Exception:
                    snap = {}
            except Exception:
                snap = {}
            if isinstance(snap, Mapping):
                return dict(_json_safe(snap))
    return {}


def _owner_dps_report(owner: Any) -> dict[str, Any] | None:
    tracker = getattr(owner, "_dps_tracker", None)
    if tracker is not None:
        get_last = getattr(tracker, "get_last_report", None)
        if callable(get_last):
            try:
                report = get_last()
                if isinstance(report, Mapping):
                    return dict(_json_safe(report))
            except Exception:
                pass
    store = getattr(owner, "_dps_history_store", None)
    latest = getattr(store, "latest_report", None)
    if callable(latest):
        try:
            report = latest()
            if isinstance(report, Mapping):
                return dict(_json_safe(report))
        except Exception:
            pass
    return None


def _owner_history_store(owner: Any) -> tuple[Any, list[str]]:
    store = getattr(owner, "_dps_history_store", None)
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
        show_report = getattr(owner, "_show_dps_last_report", None)
        if callable(show_report):
            try:
                shown = bool(show_report(report))
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
    try:
        from engines.dps_history import load_exported_report_file
    except Exception:
        return None
    try:
        report = load_exported_report_file(path)
    except Exception:
        return None
    if not isinstance(report, Mapping):
        return None
    source_path = str(path or "")
    lower_path = source_path.lower()
    if lower_path.endswith(".xml.gz"):
        fmt = "xml.gz"
    elif lower_path.endswith(".xml.zip") or lower_path.endswith(".zip"):
        fmt = "xml.zip"
    else:
        fmt = os.path.splitext(source_path)[1].lower().lstrip(".") or "report"
    imported_report = dict(_json_safe(report))
    imported_report["report_reason"] = str(imported_report.get("report_reason") or "offline_report_import")
    imported_report["source_kind"] = "offline_report_import"
    imported_report["source_path"] = source_path
    imported_report["import_format"] = fmt
    imported_report["import_event_count"] = 0
    errors = [str(item) for item in (initial_errors or []) if str(item or "")]
    store = getattr(owner, "_dps_history_store", None) if persist else None
    if persist and store is None:
        errors.append("DPS history is not initialized")
    history_item: dict[str, Any] | None = None
    if persist and store is not None:
        history_item, persist_errors = _persist_offline_import_report(store, imported_report)
        errors.extend(persist_errors)
    shown = False
    if show:
        show_report = getattr(owner, "_show_dps_last_report", None)
        if callable(show_report):
            try:
                shown = bool(show_report(imported_report))
            except Exception as exc:
                errors.append(str(exc))
    preview = _report_preview({}, imported_report)
    persisted = bool(history_item)
    status = act_history_status(owner, limit=history_limit) if persist else {}
    ok = bool((not persist or persisted) and not errors)
    return {
        "ok": ok,
        "message": "Imported" if ok else "; ".join(errors) or "Offline report import failed",
        "format": fmt,
        "source_path": source_path,
        "self_uid": 0,
        "event_count": 0,
        "importer": "exported_report",
        "parser_adapter_id": "",
        "plugin_id": "",
        "persist_requested": bool(persist),
        "persisted": persisted,
        "shown": shown,
        "history_item": history_item,
        "report": imported_report,
        "preview": preview,
        "snapshot": {},
        "errors": errors,
        "status": status,
    }


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
        raw_events = result.get("events") or result.get("act_replay_events") or result.get("items") or []
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
            "errors": [],
            "warnings": errors,
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

    try:
        from act_replay.harness import ActReplayHarness
        from act_replay.importer import import_normalized_file
    except Exception as exc:
        message = str(exc)
        return _finish({
            "ok": False,
            "message": message,
            "format": "",
            "source_path": str(path or ""),
            "self_uid": 0,
            "event_count": 0,
            "persist_requested": bool(persist),
            "persisted": False,
            "history_item": None,
            "preview": {},
            "snapshot": {},
            "errors": [message],
            "status": {},
            "importer": "normalized",
            "parser_adapter_id": "",
            "plugin_id": "",
        })

    try:
        summary = import_normalized_file(path)
    except Exception as exc:
        message = str(exc)
        return _finish({
            "ok": False,
            "message": message,
            "format": "",
            "source_path": str(path or ""),
            "self_uid": 0,
            "event_count": 0,
            "persist_requested": bool(persist),
            "persisted": False,
            "history_item": None,
            "preview": {},
            "snapshot": {},
            "errors": [message],
            "status": {},
            "importer": "normalized",
            "parser_adapter_id": "",
            "plugin_id": "",
        })
    if not bool(summary.get("ok")):
        errors = list(summary.get("errors") or [])
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
                "importer": "normalized",
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
    store = getattr(owner, "_dps_history_store", None) if persist else None
    errors: list[str] = []

    if persist and store is None:
        errors.append("DPS history is not initialized")

    source_probe = _offline_import_source_probe(source_path, fmt, event_count)
    harness = ActReplayHarness(history_store=store, source_probe=source_probe)
    self_uid = int(summary.get("self_uid") or 0)
    if self_uid:
        try:
            harness.set_self_uid(self_uid)
        except Exception as exc:
            errors.append(str(exc))
    try:
        snapshot = harness.replay(events)
    except Exception as exc:
        message = str(exc)
        return _finish({
            "ok": False,
            "message": message,
            "format": fmt,
            "source_path": source_path,
            "self_uid": self_uid,
            "event_count": event_count,
            "persist_requested": bool(persist),
            "persisted": False,
            "history_item": None,
            "preview": {},
            "snapshot": {},
            "errors": errors + [message],
            "status": {},
            "importer": importer,
            "parser_adapter_id": parser_adapter_id,
            "plugin_id": plugin_id,
        })

    report, finalize_errors = _finalize_offline_import_report(
        harness,
        source_path=source_path,
        fmt=fmt,
        event_count=event_count,
    )
    errors.extend(finalize_errors)

    history_item: dict[str, Any] | None = None
    if persist and report is not None and store is not None:
        history_item, persist_errors = _persist_offline_import_report(store, report)
        errors.extend(persist_errors)

    shown = False
    if show and report is not None:
        show_report = getattr(owner, "_show_dps_last_report", None)
        if callable(show_report):
            try:
                shown = bool(show_report(report))
            except Exception as exc:
                errors.append(str(exc))

    snapshot_payload = dict(_json_safe(snapshot if isinstance(snapshot, Mapping) else {}))
    preview = _report_preview(snapshot_payload, report)
    persisted = bool(history_item)
    status = act_history_status(owner, limit=history_limit) if persist else {}
    ok = bool(report is not None and (not persist or persisted) and not errors)
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
        "payload": _json_safe(payload),
    }


def _timeline_encounter_id(owner: Any, state: Mapping[str, Any]) -> str:
    for attr in ("_encounter_id", "encounter_id"):
        value = getattr(owner, attr, "")
        if value:
            return str(value)
    mgr = getattr(owner, "_encounter_mgr", None)
    for attr in ("encounter_id", "current_encounter_id", "id"):
        value = getattr(mgr, attr, "") if mgr is not None else ""
        if value:
            return str(value)
    return str(state.get("encounter_id") or "live")


def _timeline_replay_events(owner: Any) -> tuple[list[Mapping[str, Any]], int]:
    provider = (
        getattr(owner, "_act_timeline_replay_events", None)
        or getattr(owner, "act_timeline_replay_events", None)
        or getattr(owner, "_act_replay_events", None)
    )
    events = provider() if callable(provider) else provider
    if not isinstance(events, list):
        return [], 0
    self_uid = (
        getattr(owner, "_act_timeline_replay_self_uid", 0)
        or getattr(owner, "act_timeline_replay_self_uid", 0)
        or getattr(owner, "_act_replay_self_uid", 0)
    )
    try:
        uid = int(self_uid or 0)
    except Exception:
        uid = 0
    return [event for event in events if isinstance(event, Mapping)], uid


def _timeline_replay_status(owner: Any, *, state: Mapping[str, Any], limit: int, query: str) -> Optional[dict[str, Any]]:
    replay_events, self_uid = _timeline_replay_events(owner)
    if not replay_events:
        return None
    from act_replay.timeline import replay_timeline_status

    return replay_timeline_status(
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
    try:
        raw_events = ensure_act_event_bus(owner).recent_events(row_limit)
    except Exception as exc:
        raw_events = []
        errors.append(str(exc))
    events = [_compact_timeline_event(event, idx) for idx, event in enumerate(raw_events) if isinstance(event, Mapping)]
    text = query_text.strip().lower()
    if text:
        events = [
            event for event in events
            if text in json.dumps(event, ensure_ascii=False, default=str).lower()
        ]
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


def _is_generated_entity_label(text: Any) -> bool:
    value = _truthy_text(text)
    return bool(value and (value.startswith("怪物#") or value.startswith("目标#") or value.startswith("技能#") or value.startswith("地牢#")))


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


def _resolve_name(kind: str, value: Any) -> str:
    if value is None or not str(value or "").strip():
        return ""
    try:
        from tools.tablekit.name_tables import names
        return _truthy_text(names.resolve(kind, value, default=""))
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
    return getattr(owner, "_packet_engine", None) or getattr(owner, "packet_bridge", None) or getattr(owner, "_bridge", None)


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
        fallback = f"怪物#{target_uid}" if target_uid else (f"怪物#{monster_id}" if monster_id else topic_text)
        label = monster_name or target_name or _payload_first_text(payload, ("message", "event_type", "name")) or fallback
        group_kind = "monster"
        group_name = monster_name or target_name or label
    elif topic_text in {"damage", "heal"}:
        label = skill_name or _payload_first_text(payload, ("message", "action_type", "event_type")) or topic_text
        target_is_player = bool(payload.get("target_is_player"))
        target_is_monster = bool(payload.get("target_is_monster")) or bool(monster_name or monster_id)
        group_kind = "monster" if target_is_monster and not target_is_player else "target"
        fallback = f"怪物#{target_uid}" if group_kind == "monster" and target_uid else (f"目标#{target_uid}" if target_uid else label)
        group_name = target_name or monster_name or fallback
    else:
        label = _payload_first_text(payload, ("display_label", "message", "action_name", "skill_name", "name")) or topic_text
        group_kind = topic_text or "event"
        group_name = label
    return {
        "label": label,
        "actor": actor_name or (str(actor_uid) if actor_uid else ""),
        "target": target_name or (("怪物#" if group_kind == "monster" else "目标#") + str(target_uid) if target_uid else ""),
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
    else:
        try:
            raw_events = ensure_act_event_bus(owner).recent_events(min(row_offset + row_limit, 1000))
        except Exception as exc:
            raw_events = []
            errors.append(str(exc))
        rows = [_action_log_row(event, idx) for idx, event in enumerate(raw_events) if isinstance(event, Mapping)]
    rows = _filter_action_log_rows(rows, query=str(filters.get("query") or ""), topic=str(filters.get("topic") or ""))
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
    for name in ("_get_dps_act_snapshot", "_build_dps_act_snapshot"):
        fn = getattr(owner, name, None)
        if callable(fn):
            try:
                snap = fn(history_limit=20)
            except TypeError:
                try:
                    snap = fn()
                except Exception:
                    snap = {}
            except Exception:
                snap = {}
            if isinstance(snap, Mapping):
                return dict(_json_safe(snap))
    return {}


def act_aggregate_status(owner: Any, *, limit: int = 1000, query: str | None = "",
                         source: str | None = "live", window_ms: int = 1000,
                         top_n: int = 20, encounter_id: str | None = None) -> dict[str, Any]:
    """Return semantic ACT aggregate groups for the primary cockpit panel."""
    errors: list[str] = []
    row_limit = max(1, min(int(limit or 1000), 5000))
    source_mode = _normalize_action_log_source(source)
    selected_encounter = str(encounter_id or "")
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
                          selected_encounter)
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
    result = {
        "ok": not errors,
        "message": "OK" if not errors else "; ".join(errors),
        "source": source_mode,
        "encounter_id": selected_encounter or str(snapshot.get("encounter_id") or (render_spec.get("encounter") or {}).get("id") or ""),
        "overview": summary.get("overview") or {},
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
                                offset: int | None = None) -> dict[str, Any]:
    state = _action_log_state(owner)
    cursor = dict(state.get("cursor") or {})
    cursor["time_ms"] = max(0, int(cursor_ms or 0))
    if offset is not None:
        cursor["offset"] = max(0, int(offset or 0))
    state["cursor"] = cursor
    return act_action_log_status(owner, limit=limit, source=source, encounter_id=encounter_id)


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
    {"id": "boss_hp_pct", "label": "Boss HP %", "kind": "gauge", "unit": "pct"},
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


def _event_boss_hp_pct(payload: Mapping[str, Any]) -> float | None:
    for key in ("boss_hp_pct", "hp_pct", "boss_hp_est_pct"):
        if key in payload:
            value = _safe_float(payload.get(key), -1.0)
            if value < 0:
                continue
            return max(0.0, min(100.0, value * 100.0 if value <= 1.0 else value))
    hp = _safe_float(payload.get("hp") or payload.get("boss_current_hp"), -1.0)
    total = _safe_float(payload.get("max_hp") or payload.get("boss_total_hp"), -1.0)
    if hp >= 0 and total > 0:
        return max(0.0, min(100.0, hp * 100.0 / total))
    return None


def _graph_rows_from_events(raw_events: list[dict[str, Any]], *, query: str = "", topic: str = "") -> list[dict[str, Any]]:
    rows = [_action_log_row(event, idx) for idx, event in enumerate(raw_events) if isinstance(event, Mapping)]
    rows = _filter_action_log_rows(rows, query=query, topic=topic)
    rows.sort(key=lambda row: (int(row.get("time_ms") or 0), int(row.get("index") or 0)))
    return rows


def _build_graph_series(rows: list[dict[str, Any]], selected_metric: str) -> dict[str, Any]:
    damage_total = 0.0
    heal_total = 0.0
    event_total = 0.0
    last_boss_hp: float | None = None
    series: dict[str, Any] = {
        "damage": {"metric": "damage", "selected": selected_metric == "damage", "points": []},
        "heal": {"metric": "heal", "selected": selected_metric == "heal", "points": []},
        "event_count": {"metric": "event_count", "selected": selected_metric == "event_count", "points": []},
        "boss_hp_pct": {"metric": "boss_hp_pct", "selected": selected_metric == "boss_hp_pct", "points": []},
    }
    for row in rows:
        payload = row.get("payload") if isinstance(row.get("payload"), Mapping) else {}
        topic = str(row.get("topic") or "")
        if topic == "damage" or "damage" in payload or "damage_total" in payload:
            damage_total += _safe_float(payload.get("damage") or payload.get("damage_total") or row.get("value"), 0.0)
        if topic == "heal" or "heal" in payload or "heal_total" in payload:
            heal_total += _safe_float(payload.get("heal") or payload.get("heal_total") or row.get("value"), 0.0)
        event_total += 1.0
        boss_hp = _event_boss_hp_pct(payload)
        if boss_hp is not None:
            last_boss_hp = boss_hp
        point_base = {"time_ms": int(row.get("time_ms") or 0), "row_id": str(row.get("id") or ""), "topic": topic}
        series["damage"]["points"].append({**point_base, "value": float(damage_total)})
        series["heal"]["points"].append({**point_base, "value": float(heal_total)})
        series["event_count"]["points"].append({**point_base, "value": float(event_total)})
        if last_boss_hp is not None:
            series["boss_hp_pct"]["points"].append({**point_base, "value": float(last_boss_hp)})
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
    try:
        raw_events = ensure_act_event_bus(owner).recent_events(row_limit)
    except Exception as exc:
        raw_events = []
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
        "row_count": len(rows),
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
    tracker = getattr(owner, "_dps_tracker", None)
    get_detail = getattr(tracker, "get_entity_detail", None)
    if uid and callable(get_detail):
        try:
            detail = get_detail(uid)
            if isinstance(detail, Mapping):
                return dict(_json_safe(detail)), errors
        except Exception as exc:
            errors.append(str(exc))
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
    report = report if isinstance(report, Mapping) else {}
    rows = _report_rows_from_snapshot(snapshot, report)
    total_damage = int(totals.get("damage") or report.get("total_damage") or 0)
    total_heal = int(totals.get("heal") or report.get("total_heal") or 0)
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
    store = getattr(owner, "_dps_history_store", None)
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
    store = getattr(owner, "_dps_history_store", None)
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


def _owner_mem_data_source(owner: Any) -> str:
    settings = _owner_settings(owner)
    default = "tcp"
    try:
        if isinstance(settings, Mapping):
            return str(settings.get("mem_data_source", default) or default).lower()
        if settings is not None:
            return str(settings.get("mem_data_source", default) or default).lower()
    except Exception:
        pass
    return default


def act_data_source_health(owner: Any, *, now: float | None = None) -> dict[str, Any]:
    """Return a parity-friendly data source health payload for both UIs."""
    now_ts = float(now if now is not None else time.time())
    engine = getattr(owner, "_packet_engine", None)
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
    packet.setdefault("data_source", str(packet.get("data_source") or _owner_mem_data_source(owner) or "tcp"))
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
        "data_source": packet.get("data_source") or _owner_mem_data_source(owner),
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
        "requested_mode": _owner_mem_data_source(owner),
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


__all__ = [
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
    "act_plugin_menu",
    "act_plugin_ui_panels",
    "act_plugin_ui_render",
    "act_plugin_ui_action",
    "act_render_surfaces",
    "act_render_apply_hooks",
    "act_render_overlays",
    "act_render_surface",
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
    "ensure_act_trigger_engine",
    "project_base_dir",
    "publish_owner_event",
    "should_record_owner_combat_event",
    "shutdown_act_plugin_manager",
]
