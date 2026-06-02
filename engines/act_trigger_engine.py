# -*- coding: utf-8 -*-
"""Shared ACT trigger/timer evaluator for WebView and Entity modes.

The engine consumes the normalized ``render_spec`` produced by
``combat_analytics`` and emits small alert/timer event dictionaries.  It has no
UI dependency: webview and tkinter/entity overlays can display the same events
without duplicating trigger logic.
"""

from __future__ import annotations

import copy
import threading
import time
import uuid
from typing import Any, Dict, Iterable, List, Optional, Tuple


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
    ):
        rule_type = "damage_total"
    return {
        "id": _safe_str(src.get("id")) or f"act_rule_{fallback_index}",
        "enabled": bool(src.get("enabled", True)),
        "type": rule_type,
        "label": _safe_str(src.get("label")) or rule_type.replace("_", " ").title(),
        "message": _safe_str(src.get("message")),
        "threshold": _safe_float(src.get("threshold") or src.get("value"), 0.0),
        "match": _safe_str(src.get("match")),
        "cooldown_s": max(0.0, _safe_float(src.get("cooldown_s"), 5.0)),
        "once_per_encounter": bool(src.get("once_per_encounter", True)),
        "severity": _safe_str(src.get("severity")) or "info",
    }


class ActTriggerEngine:
    """Thread-safe evaluator for ACT alert/timer rules."""

    def __init__(self, rules: Optional[Iterable[Dict[str, Any]]] = None,
                 max_recent: int = 100) -> None:
        self._lock = threading.RLock()
        self._rules: List[Dict[str, Any]] = []
        self._last_fire_ts: Dict[Tuple[str, str], float] = {}
        self._fired_in_encounter: set[Tuple[str, str]] = set()
        self._recent_events: List[Dict[str, Any]] = []
        self._max_recent = max(1, int(max_recent or 100))
        self.set_rules(rules or [])

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
        render_spec = (act_snapshot or {}).get("render_spec") or act_snapshot or {}
        emitted: List[Dict[str, Any]] = []
        with self._lock:
            encounter_id = self._encounter_key(render_spec)
            for rule in self._rules:
                if not rule.get("enabled", True):
                    continue
                if not self._matches(rule, render_spec):
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
                self._recent_events.append(copy.deepcopy(event))
                if len(self._recent_events) > self._max_recent:
                    self._recent_events = self._recent_events[-self._max_recent:]
                emitted.append(event)
        return emitted

    def snapshot(self, limit: int = 20) -> Dict[str, Any]:
        with self._lock:
            recent = [copy.deepcopy(e) for e in self._recent_events[-max(0, int(limit or 20)):]][::-1]
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

    @staticmethod
    def _matches(rule: Dict[str, Any], render_spec: Dict[str, Any]) -> bool:
        totals = render_spec.get("totals") or {}
        encounter = render_spec.get("encounter") or {}
        context = render_spec.get("context") or {}
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
        return False

    @staticmethod
    def _build_event(rule: Dict[str, Any], render_spec: Dict[str, Any],
                     now_ts: float, encounter_id: str) -> Dict[str, Any]:
        rule_type = str(rule.get("type") or "")
        label = str(rule.get("label") or rule_type)
        message = str(rule.get("message") or label)
        return {
            "id": uuid.uuid4().hex,
            "rule_id": str(rule.get("id") or ""),
            "type": "timer" if rule_type == "elapsed_s" else "alert",
            "trigger_type": rule_type,
            "label": label,
            "message": message,
            "severity": str(rule.get("severity") or "info"),
            "encounter_id": encounter_id,
            "created_at": now_ts,
            "render_mode": str(render_spec.get("mode") or "empty"),
        }


__all__ = ["ActTriggerEngine", "normalize_trigger_rule"]