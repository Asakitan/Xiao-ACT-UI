# -*- coding: utf-8 -*-
"""Helpers for wiring ACT platform services into both UI runtimes."""

from __future__ import annotations

import os
import json
import time
from typing import Any, Callable, Iterable, Mapping, Optional

from .event_bus import EventBus
from .plugins import PluginManager


def default_plugin_dirs(base_dir: str) -> list[str]:
    return [
        os.path.join(base_dir, "plugins"),
        os.path.join(base_dir, "user_plugins"),
    ]


def project_base_dir() -> str:
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def ensure_act_event_bus(owner: Any) -> EventBus:
    bus = getattr(owner, "_act_event_bus", None)
    if isinstance(bus, EventBus):
        return bus
    bus = EventBus()
    try:
        setattr(owner, "_act_event_bus", bus)
    except Exception:
        pass
    return bus


def _owner_settings(owner: Any) -> Any:
    return getattr(owner, "_cfg_settings_ref", None) or getattr(owner, "settings", None)


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
    settings = _owner_settings(owner)
    if settings is not None:
        try:
            settings.set("act_trigger_rules", rules)
        except Exception:
            pass
        try:
            settings.save()
        except Exception:
            pass
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


def ensure_act_trigger_engine(owner: Any):
    from engines.act_trigger_engine import ActTriggerEngine

    engine = getattr(owner, "_act_trigger_engine", None)
    if isinstance(engine, ActTriggerEngine):
        return engine
    engine = ActTriggerEngine(_read_trigger_rules(owner))
    try:
        setattr(owner, "_act_trigger_engine", engine)
    except Exception:
        pass
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
    settings: Any = None,
    plugin_dirs: Optional[Iterable[str]] = None,
) -> PluginManager:
    manager = PluginManager(
        plugin_dirs=list(plugin_dirs or default_plugin_dirs(base_dir)),
        event_bus=event_bus or EventBus(),
        snapshot_provider=snapshot_provider,
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
        manager.settings = _owner_settings(owner)
        if plugin_dirs is not None:
            manager.plugin_dirs = [os.path.abspath(path) for path in plugin_dirs if path]
            manager.discover()
    if load:
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


def _trigger_status_from_rules(owner: Any, rules: list[dict[str, Any]], *,
                               ok: bool = True, message: str = "") -> dict[str, Any]:
    normalized = _normalize_trigger_rules(rules)
    timers = [rule for rule in normalized if rule.get("type") == "elapsed_s"]
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
    return {"render_spec": render_spec}


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
        engine = ActTriggerEngine([enabled_rule])
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
    return {
        "available": bool(store is not None),
        "count": len(items),
        "path": str(getattr(store, "path", "") or ""),
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
        list_reports = getattr(store, "list_reports", None)
        if callable(list_reports):
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
    if text:
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
    return value if value in {"json", "csv"} else "json"


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
        "formats": ["json", "csv"],
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
    if int(payload.get("last_event_ms") or 0) > 30000:
        diagnostics.append({"level": "warn", "message": "No player update for more than 30s"})
    out = dict(payload)
    out["diagnostics"] = diagnostics
    return out


__all__ = [
    "act_history_delete",
    "act_history_load",
    "act_history_status",
    "act_report_copy",
    "act_report_export",
    "act_report_status",
    "act_plugin_disable",
    "act_plugin_enable",
    "act_plugin_list",
    "act_plugin_reload",
    "act_plugin_status",
    "act_data_source_diagnose",
    "act_data_source_health",
    "act_trigger_disable",
    "act_trigger_enable",
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
    "shutdown_act_plugin_manager",
]