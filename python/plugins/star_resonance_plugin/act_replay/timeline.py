# -*- coding: utf-8 -*-
# Cursor-based timeline reconstruction for normalized ACT replay events.

from __future__ import annotations

import json
from typing import Any, Iterable, Mapping

from .harness import ActReplayHarness


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


def _event_topic(event: Mapping[str, Any]) -> str:
    kind = str(event.get("kind") or event.get("type") or "").lower()
    if kind == "heal" or bool(event.get("is_heal")):
        return "heal"
    if kind == "damage":
        return "damage"
    if kind in {"client_use", "server_stage_end", "server_end", "skill"}:
        return "skill"
    if kind in {"boss_event", "boss_buff_event", "boss_buff"}:
        return "boss"
    if kind in {"boss_state", "boss_attrs", "boss"}:
        return "boss_state"
    if kind in {"monster_update", "monster", "monster_attrs"}:
        return "monster"
    if kind in {
        "enter_scene",
        "start_dungeon",
        "start_playing_dungeon",
        "sync_dungeon_data",
        "sync_dungeon_dirty_data",
        "dungeon",
        "scene",
    }:
        return "scene" if "scene" in kind else "dungeon"
    return kind or "event"


def _event_label(event: Mapping[str, Any]) -> str:
    return str(
        event.get("message")
        or event.get("label")
        or event.get("skill")
        or event.get("skill_name")
        or event.get("attacker")
        or event.get("name")
        or event.get("kind")
        or event.get("type")
        or "event"
    )


def _event_value(event: Mapping[str, Any]) -> Any:
    return (
        event.get("damage")
        or event.get("damage_total")
        or event.get("heal")
        or event.get("event_type")
        or event.get("boss_current_hp")
        or event.get("skill_id")
        or event.get("skill_uuid")
        or ""
    )


def _json_safe(value: Any) -> Any:
    try:
        json.dumps(value, ensure_ascii=False, default=str)
        return value
    except Exception:
        return str(value)


def _normalize_events(events: Iterable[Mapping[str, Any]]) -> list[dict[str, Any]]:
    normalized: list[dict[str, Any]] = []
    for event in events or []:
        if isinstance(event, Mapping):
            normalized.append(dict(event))
    normalized.sort(key=lambda item: _safe_float(item.get("timestamp") or item.get("observed_at") or 0.0))
    return normalized


def _base_timestamp(events: list[Mapping[str, Any]]) -> float:
    stamps = [
        _safe_float(event.get("timestamp") or event.get("observed_at") or 0.0)
        for event in events
    ]
    stamps = [stamp for stamp in stamps if stamp > 0.0]
    return min(stamps) if stamps else 0.0


def _compact_replay_event(event: Mapping[str, Any], index: int, base_ts: float, cursor_ms: int) -> dict[str, Any]:
    observed_at = _safe_float(event.get("timestamp") or event.get("observed_at") or 0.0)
    time_ms = int(max(0.0, observed_at - base_ts) * 1000.0) if observed_at and base_ts else int(max(0.0, observed_at) * 1000.0)
    return {
        "index": int(index),
        "id": f"replay:{index}",
        "topic": _event_topic(event),
        "kind": str(event.get("kind") or event.get("type") or ""),
        "observed_at": observed_at,
        "time_ms": time_ms,
        "absolute_time_ms": int(max(0.0, observed_at) * 1000.0) if observed_at else 0,
        "label": _event_label(event),
        "value": _event_value(event),
        "source": str(event.get("source") or "replay"),
        "payload": _json_safe(dict(event)),
        "replayed": time_ms <= int(cursor_ms),
        "is_cursor": False,
    }


def replay_timeline_status(events: Iterable[Mapping[str, Any]], *, self_uid: int = 0,
                           cursor_ms: int = 0, limit: int = 80,
                           query: str = "") -> dict[str, Any]:
    # Replay normalized fixture events up to ``cursor_ms`` and return VCR data.
    normalized = _normalize_events(events)
    cursor = max(0, _safe_int(cursor_ms))
    row_limit = max(1, min(_safe_int(limit, 80), 500))
    base_ts = _base_timestamp(normalized)
    compact = [
        _compact_replay_event(event, idx, base_ts, cursor)
        for idx, event in enumerate(normalized)
    ]
    selected = [
        event for event, compact_event in zip(normalized, compact)
        if compact_event.get("replayed")
    ]
    text = str(query or "").strip().lower()
    visible = compact
    if text:
        visible = [
            event for event in visible
            if text in json.dumps(event, ensure_ascii=False, default=str).lower()
        ]
    nearest_id = ""
    if visible:
        nearest = min(visible, key=lambda item: abs(int(item.get("time_ms") or 0) - cursor))
        nearest_id = str(nearest.get("id") or "")
        for item in visible:
            item["is_cursor"] = str(item.get("id") or "") == nearest_id
    harness = ActReplayHarness()
    if self_uid:
        harness.set_self_uid(self_uid)
    snapshot = harness.replay(selected) if selected else harness.snapshot()
    return {
        "ok": True,
        "message": "OK",
        "replay": {
            "enabled": True,
            "source": "act_replay",
            "event_count": len(normalized),
            "replayed_event_count": len(selected),
            "base_timestamp": base_ts,
            "nearest_event_id": nearest_id,
        },
        "events": visible[:row_limit],
        "cursor_ms": cursor,
        "filters": {"query": str(query or ""), "limit": row_limit},
        "replay_snapshot": _json_safe(snapshot),
        "errors": [],
    }


__all__ = ["replay_timeline_status"]
