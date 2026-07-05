# -*- coding: utf-8 -*-
# Shared ACT trigger/timer evaluator for WebView and Entity modes.
#
# The engine consumes the normalized ``render_spec`` produced by
# ``combat_analytics`` and emits small alert/timer event dictionaries.  It has no
# UI dependency: webview and tkinter/entity overlays can display the same events
# without duplicating trigger logic.

from __future__ import annotations

import copy
import re
import threading
import time
import uuid
from typing import Any, Callable, Dict, Iterable, List, Mapping, Optional, Tuple

_MISSING = object()


def _safe_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except Exception:
        return float(default)


def _safe_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        return int(default)


def _safe_str(value: Any) -> str:
    return str(value or "").strip()


def _first_present(src: Dict[str, Any], *keys: str) -> Any:
    for key in keys:
        if key in src and src.get(key) is not None:
            return src.get(key)
    return None


def _path_get(src: Any, path: str, default: Any = _MISSING) -> Any:
    cur = src
    for part in [item for item in str(path or "").split(".") if item]:
        if isinstance(cur, dict):
            if part not in cur:
                return default
            cur = cur.get(part)
            continue
        if isinstance(cur, list):
            try:
                cur = cur[int(part)]
                continue
            except Exception:
                return default
        return default
    return cur


def _match_root_value(root: Dict[str, Any], render_spec: Dict[str, Any], path: str) -> Any:
    field = _safe_str(path)
    if not field:
        return _MISSING
    if field.startswith("render_spec."):
        return _path_get(root, field, _MISSING)
    value = _path_get(render_spec, field, _MISSING)
    if value is not _MISSING:
        return value
    return _path_get(root, field, _MISSING)


def normalize_trigger_rule(raw: Any, fallback_index: int = 0) -> Dict[str, Any]:
    src = raw if isinstance(raw, dict) else {}
    rule_type = _safe_str(src.get("type") or src.get("trigger_type")).lower()
    if rule_type not in (
        "damage_total",
        "heal_total",
        "elapsed_s",
        "boss_hp_pct_below",
        "skill_kind",
        "boss_event_type",
        "encounter_start",
        "field_match",
        "timer_preset",
        "plugin_trigger",
    ):
        rule_type = "damage_total"
    plugin_trigger_type = _safe_str(_first_present(src, "plugin_trigger_type", "extension_id", "extension"))
    if rule_type == "plugin_trigger" and not plugin_trigger_type:
        plugin_trigger_type = _safe_str(src.get("handler") or src.get("handler_id"))
    threshold = _safe_float(_first_present(src, "threshold", "value", "event_type"), 0.0)
    if rule_type == "boss_hp_pct_below" and threshold > 1.0:
        threshold = threshold / 100.0
    raw_duration = _first_present(src, "duration_s", "timer_s")
    if raw_duration is None and rule_type in ("elapsed_s", "timer_preset"):
        raw_duration = threshold
    duration_s = max(0.0, _safe_float(raw_duration, 0.0))
    match_value = _first_present(src, "match", "event_type", "kind")
    if rule_type == "field_match":
        match_value = _first_present(src, "match", "value", "expected", "kind")
    operator = _safe_str(_first_present(src, "operator", "op")).lower()
    if not operator:
        operator = "eq" if _safe_str(match_value) else ("gte" if threshold else "exists")
    return {
        "id": _safe_str(src.get("id")) or f"act_rule_{fallback_index}",
        "enabled": bool(src.get("enabled", True)),
        "type": rule_type,
        "label": _safe_str(src.get("label")) or rule_type.replace("_", " ").title(),
        "message": _safe_str(src.get("message")),
        "threshold": threshold,
        "match": _safe_str(match_value),
        "field": _safe_str(_first_present(src, "field", "field_path", "path")),
        "plugin_trigger_type": plugin_trigger_type,
        "operator": operator,
        "duration_s": duration_s,
        "time_budget_ms": max(0.0, _safe_float(_first_present(src, "time_budget_ms", "max_runtime_ms"), 25.0)),
        "scope": _safe_str(src.get("scope")) or "encounter",
        "cooldown_s": max(0.0, _safe_float(src.get("cooldown_s"), 5.0)),
        "once_per_encounter": bool(src.get("once_per_encounter", True)),
        "severity": _safe_str(src.get("severity")) or "info",
    }


class ActTriggerEngine:
    # Thread-safe evaluator for ACT alert/timer rules.

    def __init__(self, rules: Optional[Iterable[Dict[str, Any]]] = None,
                 max_recent: int = 100,
                 plugin_trigger_evaluator: Optional[Callable[..., Any]] = None,
                 plugin_time_budget_ms: float = 25.0) -> None:
        self._lock = threading.RLock()
        self._rules: List[Dict[str, Any]] = []
        self._last_fire_ts: Dict[Tuple[str, str], float] = {}
        self._fired_in_encounter: set[Tuple[str, str]] = set()
        self._recent_events: List[Dict[str, Any]] = []
        self._max_recent = max(1, int(max_recent or 100))
        self._plugin_trigger_evaluator = plugin_trigger_evaluator
        self._plugin_time_budget_ms = max(0.0, _safe_float(plugin_time_budget_ms, 25.0))
        self.set_rules(rules or [])

    def set_plugin_trigger_evaluator(self, evaluator: Optional[Callable[..., Any]],
                                     *, time_budget_ms: float | None = None) -> None:
        with self._lock:
            self._plugin_trigger_evaluator = evaluator if callable(evaluator) else None
            if time_budget_ms is not None:
                self._plugin_time_budget_ms = max(0.0, _safe_float(time_budget_ms, self._plugin_time_budget_ms))

    def set_rules(self, rules: Iterable[Dict[str, Any]]) -> None:
        with self._lock:
            self._rules = [normalize_trigger_rule(rule, i) for i, rule in enumerate(rules or [], 1)]
            self._last_fire_ts.clear()
            self._fired_in_encounter.clear()

    def add_rule(self, rule: Dict[str, Any]) -> Dict[str, Any]:
        with self._lock:
            normalized = normalize_trigger_rule(rule, len(self._rules) + 1)
            self._rules.append(normalized)
            return copy.deepcopy(normalized)

    def evaluate(self, act_snapshot: Dict[str, Any], now: Optional[float] = None) -> List[Dict[str, Any]]:
        now_ts = float(now if now is not None else time.time())
        snapshot_root = dict(act_snapshot or {})
        render_spec = snapshot_root.get("render_spec") or snapshot_root or {}
        snapshot_root.setdefault("render_spec", render_spec)
        emitted: List[Dict[str, Any]] = []
        with self._lock:
            encounter_id = self._encounter_key(render_spec)
            for rule in self._rules:
                if not rule.get("enabled", True):
                    continue
                if not self._matches(rule, render_spec, snapshot_root):
                    continue
                fire_key = (encounter_id, str(rule.get("id") or ""))
                global_key = ("global", str(rule.get("id") or ""))
                if rule.get("once_per_encounter", True) and fire_key in self._fired_in_encounter:
                    continue
                last = self._last_fire_ts.get(global_key, 0.0)
                if last > 0 and now_ts - last < float(rule.get("cooldown_s") or 0.0):
                    continue
                event = self._build_event(rule, render_spec, now_ts, encounter_id)
                self._last_fire_ts[global_key] = now_ts
                self._fired_in_encounter.add(fire_key)
                # event 是 _build_event 产出的扁平 dict（值全为 str/float/int 等
                # 不可变标量），dict() 浅拷贝与 deepcopy 语义等价但更省，
                # 隔离存档副本不被调用方对 emitted 的改键影响。
                self._recent_events.append(dict(event))
                if len(self._recent_events) > self._max_recent:
                    self._recent_events = self._recent_events[-self._max_recent:]
                emitted.append(event)
        return emitted

    def snapshot(self, limit: int = 20) -> Dict[str, Any]:
        with self._lock:
            # 扁平事件 dict（不可变标量值）→ dict() 浅拷贝即可隔离调用方，
            # 比 deepcopy 省（snapshot 由触发器面板按轮询调用）。
            recent = [dict(e) for e in self._recent_events[-max(0, int(limit or 20)):]][::-1]
            return {
                "enabled": bool(self._rules),
                "rule_count": len(self._rules),
                "recent": recent,
            }

    @staticmethod
    def _encounter_key(render_spec: Dict[str, Any]) -> str:
        encounter = render_spec.get("encounter") or {}
        key = _safe_str(encounter.get("id"))
        if key:
            return key
        ctx = render_spec.get("context") or {}
        return "ctx:%s:%s" % (
            _safe_int(ctx.get("dungeon_id"), 0),
            _safe_int(ctx.get("dungeon_scene_id"), 0),
        )

    def _matches(self, rule: Dict[str, Any], render_spec: Dict[str, Any],
                 snapshot_root: Optional[Dict[str, Any]] = None) -> bool:
        totals = render_spec.get("totals") or {}
        encounter = render_spec.get("encounter") or {}
        context = render_spec.get("context") or {}
        root = snapshot_root or {"render_spec": render_spec}
        rule_type = str(rule.get("type") or "")
        threshold = _safe_float(rule.get("threshold"), 0.0)
        if rule_type == "damage_total":
            return _safe_float(totals.get("damage"), 0.0) >= threshold
        if rule_type == "heal_total":
            return _safe_float(totals.get("heal"), 0.0) >= threshold
        if rule_type == "elapsed_s":
            return _safe_float(totals.get("elapsed_s") or encounter.get("duration_s"), 0.0) >= threshold
        if rule_type == "boss_hp_pct_below":
            hp_pct = _safe_float((render_spec.get("boss") or {}).get("hp_pct"), -1.0)
            return hp_pct >= 0.0 and hp_pct <= threshold
        if rule_type == "skill_kind":
            return bool(rule.get("match")) and _safe_str(context.get("last_skill_kind")).lower() == _safe_str(rule.get("match")).lower()
        if rule_type == "boss_event_type":
            event_type = _safe_int(context.get("last_boss_event_type"), 0)
            match = _safe_str(rule.get("match"))
            if match:
                return str(event_type) == match
            return event_type > 0 and event_type == _safe_int(rule.get("threshold"), 0)
        if rule_type == "encounter_start":
            return str(encounter.get("status") or "") in ("active", "pending_reset")
        if rule_type == "timer_preset":
            duration_s = _safe_float(rule.get("duration_s"), threshold)
            return _safe_float(totals.get("elapsed_s") or encounter.get("duration_s"), 0.0) >= duration_s
        if rule_type == "field_match":
            return ActTriggerEngine._match_field_rule(rule, render_spec, root)
        if rule_type == "plugin_trigger":
            return self._match_plugin_rule(rule, render_spec, root)
        return False

    @staticmethod
    def _match_field_rule(rule: Dict[str, Any], render_spec: Dict[str, Any],
                          snapshot_root: Dict[str, Any]) -> bool:
        actual = _match_root_value(snapshot_root, render_spec, str(rule.get("field") or ""))
        operator = _safe_str(rule.get("operator")).lower() or "exists"
        if operator == "exists":
            return actual is not _MISSING and actual not in (None, "")
        if actual is _MISSING:
            return False
        expected_text = _safe_str(rule.get("match"))
        actual_text = _safe_str(actual)
        if operator in ("eq", "equals", "=="):
            return actual_text.lower() == expected_text.lower()
        if operator in ("ne", "not_equals", "!="):
            return actual_text.lower() != expected_text.lower()
        if operator == "contains":
            if not expected_text:
                return False
            return expected_text.lower() in actual_text.lower()
        if operator == "regex":
            if not expected_text:
                return False
            try:
                return bool(re.search(expected_text, actual_text))
            except Exception:
                return False
        if operator in ("gt", "gte", "lt", "lte", ">", ">=", "<", "<="):
            actual_num = _safe_float(actual, 0.0)
            expected_num = _safe_float(rule.get("threshold"), _safe_float(rule.get("match"), 0.0))
            if operator in ("gt", ">"):
                return actual_num > expected_num
            if operator in ("gte", ">="):
                return actual_num >= expected_num
            if operator in ("lt", "<"):
                return actual_num < expected_num
            return actual_num <= expected_num
        return False

    def _match_plugin_rule(self, rule: Dict[str, Any], render_spec: Dict[str, Any],
                           snapshot_root: Dict[str, Any]) -> bool:
        trigger_type = _safe_str(rule.get("plugin_trigger_type"))
        evaluator = self._plugin_trigger_evaluator
        if not trigger_type or not callable(evaluator):
            rule["_plugin_last_result"] = {"ok": False, "message": "plugin trigger evaluator is unavailable"}
            return False
        payload = {
            "rule": {
                key: copy.deepcopy(value)
                for key, value in rule.items()
                if not str(key).startswith("_")
            },
            "render_spec": copy.deepcopy(render_spec),
            "snapshot": copy.deepcopy(snapshot_root),
        }
        try:
            result = evaluator(
                trigger_type,
                payload,
                _safe_float(rule.get("time_budget_ms"), self._plugin_time_budget_ms),
            )
        except Exception as exc:
            rule["_plugin_last_result"] = {"ok": False, "message": str(exc), "errors": [str(exc)]}
            return False
        if isinstance(result, Mapping):
            rule["_plugin_last_result"] = dict(result)
            if not bool(result.get("ok", True)):
                return False
            if "matched" in result:
                return bool(result.get("matched"))
            if "match" in result:
                return bool(result.get("match"))
            if "result" in result:
                return bool(result.get("result"))
            return False
        matched = bool(result)
        rule["_plugin_last_result"] = {"ok": True, "matched": matched}
        return matched

    @staticmethod
    def _build_event(rule: Dict[str, Any], render_spec: Dict[str, Any],
                     now_ts: float, encounter_id: str) -> Dict[str, Any]:
        rule_type = str(rule.get("type") or "")
        plugin_result = rule.get("_plugin_last_result") if isinstance(rule.get("_plugin_last_result"), Mapping) else {}
        label = str(rule.get("label") or rule_type)
        message = str(plugin_result.get("message") or rule.get("message") or label)
        event = {
            "id": uuid.uuid4().hex,
            "rule_id": str(rule.get("id") or ""),
            "type": "timer" if rule_type in ("elapsed_s", "timer_preset") else "alert",
            "trigger_type": rule_type,
            "label": label,
            "message": message,
            "severity": str(plugin_result.get("severity") or rule.get("severity") or "info"),
            "encounter_id": encounter_id,
            "created_at": now_ts,
            "render_mode": str(render_spec.get("mode") or "empty"),
            "field": str(rule.get("field") or ""),
            "match": str(rule.get("match") or ""),
            "duration_s": _safe_float(rule.get("duration_s"), 0.0),
            "scope": str(rule.get("scope") or "encounter"),
        }
        if rule_type == "plugin_trigger":
            event.update({
                "plugin_trigger_type": str(rule.get("plugin_trigger_type") or ""),
                "plugin_id": str(plugin_result.get("plugin_id") or ""),
                "extension_id": str(plugin_result.get("extension_id") or ""),
                "handler_elapsed_ms": _safe_float(plugin_result.get("elapsed_ms"), 0.0),
            })
        return event


__all__ = ["ActTriggerEngine", "normalize_trigger_rule"]
