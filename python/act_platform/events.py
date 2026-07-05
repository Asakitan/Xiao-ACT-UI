# -*- coding: utf-8 -*-
# Canonical ACT event envelope helpers.
#
# The envelope is intentionally plain ``dict`` data so existing parser, replay,
# WebView, Entity, and plugin code can consume it without taking a dependency on
# UI-specific classes.

from __future__ import annotations

import copy
import time
import uuid
from typing import Any, Mapping, Optional


ACT_EVENT_SCHEMA_VERSION = 1

ACT_EVENT_TOPICS = (
    "raw_packet",
    "parsed_event",
    "damage",
    "heal",
    "skill",
    "dungeon",
    "scene",
    "monster",
    "boss",
    "boss_state",
    "self_state",
    "encounter_started",
    "encounter_updated",
    "encounter_finalized",
    "act_snapshot",
    "trigger_fired",
)


def _clone_payload(payload: Any) -> Any:
    try:
        return copy.deepcopy(payload)
    except Exception:
        return payload


def _safe_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except Exception:
        return float(default)


def make_event(
    topic: str,
    payload: Optional[Mapping[str, Any]] = None,
    *,
    source_name: str = "unknown",
    source_kind: str = "unknown",
    game_id: str = "",
    parser_id: str = "",
    confidence: float = 1.0,
    observed_at: Optional[float] = None,
    event_id: Optional[str] = None,
) -> dict[str, Any]:
    # Build a versioned ACT event envelope.

    src = dict(payload or {})
    ts = observed_at
    if ts is None:
        ts = _safe_float(src.get("timestamp"), 0.0) if isinstance(src, dict) else 0.0
    if not ts:
        ts = time.time()
    return {
        "schema_version": ACT_EVENT_SCHEMA_VERSION,
        "id": str(event_id or uuid.uuid4().hex),
        "topic": str(topic or "parsed_event"),
        "observed_at": float(ts),
        "source": {
            "name": str(source_name or "unknown"),
            "kind": str(source_kind or "unknown"),
            "game_id": str(game_id or ""),
            "parser_id": str(parser_id or ""),
            "confidence": max(0.0, min(1.0, _safe_float(confidence, 1.0))),
        },
        "payload": _clone_payload(src),
    }


def clone_event(event: Mapping[str, Any]) -> dict[str, Any]:
    # Return a defensive copy of an event envelope.

    return _clone_payload(dict(event or {}))


def is_event_envelope(value: Any) -> bool:
    return isinstance(value, dict) and "topic" in value and "payload" in value


__all__ = [
    "ACT_EVENT_SCHEMA_VERSION",
    "ACT_EVENT_TOPICS",
    "clone_event",
    "is_event_envelope",
    "make_event",
]
