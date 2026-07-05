# -*- coding: utf-8 -*-
# Selective parsing policy helpers for ACT-style combat capture.
#
# The default policy records every combat event the parser sees.  This module is
# kept deliberately small and side-effect free so WebView, Entity/Tk, replay, and
# plugins can share the same filtering contract without touching the DPS hot path
# unless the user explicitly enables selective parsing.

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Mapping


_COMBAT_TOPICS = {"damage", "heal", "combat_event"}


def _safe_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        return int(default)


def _safe_str(value: Any) -> str:
    return str(value or "").strip()


def _lower_set(values: Any) -> set[str]:
    if isinstance(values, (str, int, float)):
        values = [values]
    if not isinstance(values, (list, tuple, set)):
        return set()
    return {str(item or "").strip().lower() for item in values if str(item or "").strip()}


def _int_set(values: Any) -> set[int]:
    if isinstance(values, (str, int, float)):
        values = [values]
    if not isinstance(values, (list, tuple, set)):
        return set()
    out: set[int] = set()
    for item in values:
        try:
            out.add(int(item))
        except Exception:
            continue
    return out


def _event_payload(event: Mapping[str, Any]) -> Mapping[str, Any]:
    payload = event.get("payload") if isinstance(event.get("payload"), Mapping) else event
    return payload if isinstance(payload, Mapping) else {}


def _event_topic(event: Mapping[str, Any]) -> str:
    topic = _safe_str(event.get("topic")).lower()
    if topic:
        return topic
    payload = _event_payload(event)
    kind = _safe_str(payload.get("kind") or payload.get("type")).lower()
    if kind == "heal" or payload.get("is_heal"):
        return "heal"
    return kind or "damage"


def _event_source_kind(event: Mapping[str, Any]) -> str:
    source = event.get("source") if isinstance(event.get("source"), Mapping) else {}
    return _safe_str(source.get("kind") if isinstance(source, Mapping) else "").lower()


def _name_values(payload: Mapping[str, Any]) -> set[str]:
    keys = (
        "attacker_name", "actor_name", "actor", "attacker", "source_name",
        "target_name", "target", "victim", "entity_name", "name",
    )
    return {str(payload.get(key) or "").strip().lower() for key in keys if str(payload.get(key) or "").strip()}


def _id_values(payload: Mapping[str, Any]) -> set[int]:
    values = []
    for key in (
        "attacker_uid", "attacker_id", "actor_uid", "actor_id", "uid",
        "target_uid", "target_id", "combatant_id", "entity_id",
    ):
        if payload.get(key) is not None:
            values.append(payload.get(key))
    target_uuid = _safe_int(payload.get("target_uuid"), 0)
    if target_uuid:
        values.append(target_uuid)
        if (target_uuid & 0xFFFF) == 640:
            values.append(target_uuid >> 16)
    return _int_set(values)


def normalize_policy(value: Any = None) -> dict[str, Any]:
    # Return a stable policy dictionary with safe defaults.

    src = dict(value or {}) if isinstance(value, Mapping) else {}
    mode = _safe_str(src.get("mode") or ("selective" if src.get("enabled") else "all")).lower()
    if mode not in {"all", "self", "party", "include", "exclude"}:
        mode = "all"
    enabled = bool(src.get("enabled")) and mode != "all"
    return {
        "enabled": enabled,
        "mode": mode,
        "include_self": bool(src.get("include_self")) or mode in {"self", "party"},
        "include_party": bool(src.get("include_party")) or mode == "party",
        "include_names": sorted(_lower_set(src.get("include_names") or src.get("names"))),
        "include_ids": sorted(_int_set(src.get("include_ids") or src.get("ids"))),
        "exclude_names": sorted(_lower_set(src.get("exclude_names"))),
        "exclude_ids": sorted(_int_set(src.get("exclude_ids"))),
        "source_kinds": sorted(_lower_set(src.get("source_kinds"))),
        "topics": sorted(_lower_set(src.get("topics")) or _COMBAT_TOPICS),
        "reason": _safe_str(src.get("reason")),
    }


@dataclass(frozen=True)
class SelectiveParseDecision:
    record: bool
    reason: str
    policy: Mapping[str, Any] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        return {"record": bool(self.record), "reason": self.reason, "policy": dict(self.policy)}


def should_record_event(
    event: Mapping[str, Any],
    policy: Mapping[str, Any] | None = None,
    *,
    self_uid: int = 0,
    party_ids: set[int] | None = None,
) -> SelectiveParseDecision:
    # Decide whether a canonical combat event should be recorded.

    normalized = normalize_policy(policy)
    if not normalized.get("enabled"):
        return SelectiveParseDecision(True, "record_all_default", normalized)

    topic = _event_topic(event)
    topics = set(normalized.get("topics") or [])
    if topics and topic not in topics:
        return SelectiveParseDecision(True, "non_combat_topic_passthrough", normalized)

    source_kinds = set(normalized.get("source_kinds") or [])
    source_kind = _event_source_kind(event)
    if source_kinds and source_kind and source_kind not in source_kinds:
        return SelectiveParseDecision(False, "source_kind_filtered", normalized)

    payload = _event_payload(event)
    ids = _id_values(payload)
    names = _name_values(payload)
    exclude_ids = set(normalized.get("exclude_ids") or [])
    exclude_names = set(normalized.get("exclude_names") or [])
    if ids & exclude_ids:
        return SelectiveParseDecision(False, "excluded_id", normalized)
    if names & exclude_names:
        return SelectiveParseDecision(False, "excluded_name", normalized)

    include_ids = set(normalized.get("include_ids") or [])
    include_names = set(normalized.get("include_names") or [])
    party_ids = set(party_ids or set())
    if self_uid > 0:
        party_ids.add(int(self_uid))

    if normalized.get("include_self") and self_uid > 0 and self_uid in ids:
        return SelectiveParseDecision(True, "included_self", normalized)
    if normalized.get("include_party") and party_ids and ids & party_ids:
        return SelectiveParseDecision(True, "included_party", normalized)
    if include_ids and ids & include_ids:
        return SelectiveParseDecision(True, "included_id", normalized)
    if include_names and names & include_names:
        return SelectiveParseDecision(True, "included_name", normalized)

    mode = str(normalized.get("mode") or "all")
    if mode == "exclude":
        return SelectiveParseDecision(True, "exclude_mode_passthrough", normalized)
    return SelectiveParseDecision(False, "not_selected", normalized)


__all__ = [
    "SelectiveParseDecision",
    "normalize_policy",
    "should_record_event",
]
