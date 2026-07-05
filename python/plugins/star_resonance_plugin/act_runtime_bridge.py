# -*- coding: utf-8 -*-
# Star Resonance plugin's ACT runtime bridge.
#
# These trigger-engine + DPS history importer + name-resolver runtime handlers
# used to live in ``act_platform/runtime.py``. They were moved here in 5.0.0 so
# the platform can stay strictly game-agnostic: the platform no longer imports
# the plugin's trigger engine, no longer hard-codes ``owner._act_trigger_engine``
# / ``owner._act_trigger_normalizer`` / ``owner._dps_report_loader`` lookups, and
# no longer holds a module-level ``_NAME_RESOLVER`` callback.
#
# The plugin exposes these handlers to the platform through two SDK surfaces:
#
# 1. ``ctx.engine.set_owner_attr('_act_extension_runtime', extension_runtime_provider)``
# registers a ``name -> callable`` provider so ``ctx.engine.runtime(name, **kw)``
# dispatches plugin-defined runtime actions (``act_trigger_*``,
# ``_import_exported_report_file``) without the platform importing anything.
# 2. ``ctx.register_formatter('act_resolve_name', ...)`` exposes the game's
# ``skill/monster/dungeon`` name resolver through the standard extension
# registry; the platform looks it up via ``manager.invoke_extension`` only
# when a renderer needs a name, and never imports the plugin's name tables.

from __future__ import annotations

import os
import time
from typing import Any, Callable, Iterable, Mapping

from plugins.star_resonance_plugin.engines.act_trigger_engine import (
    ActTriggerEngine,
    normalize_trigger_rule,
)


# ── settings helpers (mirrored from act_platform.runtime for self-containment) ──

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


def owner_history_store(owner: Any) -> Any:
    return getattr(owner, "_dps_history_store", None)


def owner_packet_bridge(owner: Any) -> Any:
    return (getattr(owner, "_packet_engine", None)
            or getattr(owner, "_packet_bridge", None)
            or getattr(owner, "_bridge", None))


def owner_act_snapshot(owner: Any, *, history_limit: int = 20) -> dict[str, Any]:
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
                return dict(snap)
    return {}


def owner_dps_report(owner: Any) -> dict[str, Any] | None:
    tracker = getattr(owner, "_dps_tracker", None)
    if tracker is not None:
        get_last = getattr(tracker, "get_last_report", None)
        if callable(get_last):
            try:
                report = get_last()
                if isinstance(report, Mapping):
                    return dict(report)
            except Exception:
                pass
    store = owner_history_store(owner)
    latest = getattr(store, "latest_report", None)
    if callable(latest):
        try:
            report = latest()
            if isinstance(report, Mapping):
                return dict(report)
        except Exception:
            pass
    return None


def show_dps_last_report(owner: Any, report: Mapping[str, Any]) -> bool:
    show_report = getattr(owner, "_show_dps_last_report", None)
    if callable(show_report):
        return bool(show_report(report))
    return False


def self_uid(owner: Any) -> int:
    tracker = getattr(owner, "_dps_tracker", None)
    for attr in ("_self_uid", "self_uid"):
        try:
            value = int(getattr(tracker, attr) or 0)
            if value > 0:
                return value
        except Exception:
            pass
    state = getattr(owner, "_state_mgr", None)
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


def party_ids(owner: Any) -> list[int]:
    ids: set[int] = set()
    bridge = owner_packet_bridge(owner)
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
    uid = self_uid(owner)
    if uid > 0:
        ids.add(uid)
    ids.discard(0)
    return sorted(ids)


def encounter_id(owner: Any) -> str:
    for attr in ("_encounter_id", "encounter_id"):
        value = getattr(owner, attr, "")
        if value:
            return str(value)
    mgr = getattr(owner, "_encounter_mgr", None)
    for attr in ("encounter_id", "current_encounter_id", "id"):
        value = getattr(mgr, attr, "") if mgr is not None else ""
        if value:
            return str(value)
    return ""


def combatant_detail(owner: Any, uid: Any) -> dict[str, Any] | None:
    tracker = getattr(owner, "_dps_tracker", None)
    get_detail = getattr(tracker, "get_entity_detail", None)
    if callable(get_detail):
        try:
            detail = get_detail(int(uid or 0))
            if isinstance(detail, Mapping):
                return dict(detail)
        except Exception:
            pass
    return None


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
    return [normalize_trigger_rule(rule, idx) for idx, rule in enumerate(rules or [], 1)]


# ── plugin manager adapter ─────────────────────────────────────────────────────

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
    # Lazy import to avoid a circular import with act_platform.runtime at module load.
    from act_platform.runtime import ensure_act_plugin_manager

    def _evaluate(trigger_type: str, payload: Mapping[str, Any], time_budget_ms: float = 25.0) -> dict[str, Any]:
        try:
            manager = ensure_act_plugin_manager(owner)
        except Exception as exc:
            return {"ok": False, "matched": False, "message": str(exc)}
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


def ensure_act_trigger_engine(owner: Any) -> ActTriggerEngine:
    # Return the trigger engine contributed by the Star Resonance plugin.
    #
    # Plugin-private helper: this module lives in the plugin tree, so reading an
    # owner attribute set by the same plugin is fine — the platform itself never
    # reads ``owner._act_trigger_engine`` and never imports ``ActTriggerEngine``.
    engine = getattr(owner, "_act_trigger_engine", None)
    if isinstance(engine, ActTriggerEngine):
        _configure_trigger_engine_plugins(owner, engine)
        return engine
    rules = _read_trigger_rules(owner)
    engine = ActTriggerEngine(rules, plugin_trigger_evaluator=_plugin_trigger_evaluator(owner))
    try:
        setattr(owner, "_act_trigger_engine", engine)
    except Exception:
        pass
    _configure_trigger_engine_plugins(owner, engine)
    return engine


# ── trigger status / rule admin ───────────────────────────────────────────────

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
                               rule_ids: Iterable[str] | None = None) -> dict[str, Any]:
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


# ── synthetic snapshot for act_trigger_test ────────────────────────────────────

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
        "encounter": {"id": "act-trigger-test", "status": "active",
                      "duration_s": max(1.0, threshold + 1.0)},
        "totals": {"damage": 0.0, "heal": 0.0, "elapsed_s": max(1.0, threshold + 1.0)},
        "context": {},
        "target": {},
    }
    if rule_type == "damage_total":
        render_spec["totals"]["damage"] = max(1.0, threshold + 1.0)
    elif rule_type == "heal_total":
        render_spec["totals"]["heal"] = max(1.0, threshold + 1.0)
    elif rule_type == "target_hp_pct_below":
        render_spec["target"]["hp_pct"] = min(max(threshold, 0.0), 1.0)
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
        rules = _normalize_trigger_rules(_read_trigger_rules(owner))
        rule = next((item for item in rules if str(item.get("id") or "") == target), None)
        if rule is None:
            return {"ok": False, "message": f"trigger rule not found: {target}", "events": []}
        enabled_rule = dict(rule)
        enabled_rule["enabled"] = True
        enabled_rule["cooldown_s"] = 0.0
        enabled_rule["once_per_encounter"] = False
        engine = ActTriggerEngine([enabled_rule], plugin_trigger_evaluator=_plugin_trigger_evaluator(owner))
        events = engine.evaluate(_synthetic_snapshot_for_rule(enabled_rule), now=time.time())
        return {
            "ok": bool(events),
            "message": "Test event emitted" if events else "Rule did not match synthetic snapshot",
            "events": events,
            "status": act_trigger_status(owner),
        }
    except Exception as exc:
        return {"ok": False, "message": str(exc), "events": [], "errors": [str(exc)]}


# ── exported DPS history report file importer (replaces _import_exported_report_file) ──

def import_exported_report_file(owner: Any, path: str, *, persist: bool,
                                 show: bool, history_limit: int,
                                 initial_errors: Iterable[Any] = ()) -> dict[str, Any] | None:
    # Import one game-specific exported ACT report (XML/XML.GZ/XML.ZIP).
    #
    # The platform's ``act_offline_import_file`` calls this via the extension
    # runtime provider instead of importing the plugin's ``dps_history`` loader
    # directly. ``None`` means "this importer did not match the file"; the
    # platform falls back to the generic normalized-event importer.
    try:
        from plugins.star_resonance_plugin.engines.dps_history import load_exported_report_file
    except Exception:
        return None
    try:
        report = load_exported_report_file(path)
    except Exception:
        return None
    if not isinstance(report, Mapping):
        return None
    from act_platform import runtime as _plat
    _json_safe = _plat._json_safe
    _report_preview = _plat._report_preview
    act_history_status = _plat.act_history_status
    _persist_offline_import_report = _plat._persist_offline_import_report

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


# ── name resolver formatter (registered as a plugin formatter extension) ──────

# (The name resolver is registered through the ``register_extension_runtime``
# dispatch table as ``resolve_name`` — see ``resolve_name_handler`` below.
# The platform calls it via ``_extension_runtime_handler`` so it never imports
# the plugin's name tables directly.)


def resolve_name_handler(*, kind: str = "monster", id: Any = 0, default: str = "", **_: Any) -> str:
    # Game name resolver exposed via ``register_extension_runtime('resolve_name')``.
    #
    # Platform render helpers (``act_platform.runtime._resolve_name``) call this
    # through ``_extension_runtime_handler`` so they never touch the plugin's
    # name tables directly. The handler is a strict wrapper around the plugin's
    # tablekit ``names.resolve`` and returns "" on miss so callers fall back to
    # bare ids.
    try:
        from plugins.star_resonance_plugin.tools.tablekit.name_tables import names
    except Exception:
        return ""
    try:
        return str(names.resolve(str(kind or "monster"), id, default=default) or "")
    except Exception:
        return ""


# ── extension runtime dispatch table ───────────────────────────────────────────

from plugins.star_resonance_plugin import cli as _cli  # noqa: E402

EXTENSION_RUNTIME_HANDLERS: dict[str, Callable[..., Any]] = {
    "owner_history_store": owner_history_store,
    "owner_packet_bridge": owner_packet_bridge,
    "owner_act_snapshot": owner_act_snapshot,
    "owner_dps_report": owner_dps_report,
    "show_dps_last_report": show_dps_last_report,
    "self_uid": self_uid,
    "party_ids": party_ids,
    "encounter_id": encounter_id,
    "combatant_detail": combatant_detail,
    "act_trigger_status": act_trigger_status,
    "act_trigger_reload": act_trigger_reload,
    "act_trigger_enable": act_trigger_enable,
    "act_trigger_disable": act_trigger_disable,
    "act_trigger_test": act_trigger_test,
    "act_trigger_export_presets": act_trigger_export_presets,
    "act_trigger_import_presets": act_trigger_import_presets,
    "ensure_act_trigger_engine": ensure_act_trigger_engine,
    "_import_exported_report_file": import_exported_report_file,
    "resolve_name": resolve_name_handler,
    "cli_test": _cli.run_test,
    "cli_headless": _cli.run_headless,
}


def extension_runtime_provider(name: str) -> Callable[..., Any] | None:
    # Return the plugin-contributed handler for the given platform runtime name.
    #
    # The platform dispatcher (``ctx.engine.runtime``) calls this before falling
    # back to ``act_platform.runtime.<act_*>``. Returning ``None`` lets the
    # platform's own (game-agnostic) handler run, so the platform can stay tiny.
    return EXTENSION_RUNTIME_HANDLERS.get(str(name or ""))