# -*- coding: utf-8 -*-
"""Pure ACT semantic aggregation helpers.

The functions in this module intentionally know nothing about Tk/WebView.  They
turn recent ACT event/action rows into readable semantic groups so UI surfaces do
not have to render one raw event per line by default.
"""

from __future__ import annotations

from collections import Counter, defaultdict
from typing import Any, Iterable, Mapping


_NUMERIC_FIELDS = (
    "value", "damage", "damage_total", "total_damage", "amount", "heal",
    "heal_total", "total_heal", "shield", "absorbed", "hp_delta",
)


_ID_KEYS = {
    "skill_id": ("skill_id", "base_skill_id", "semantic_skill_id", "semantic_base_skill_id", "source_skill_id", "raw_skill_key", "skill_key", "stats_key", "skill_level_id"),
    "monster_id": ("monster_id", "template_id", "target_template_id", "boss_id", "config_id"),
    "dungeon_id": ("dungeon_id", "dungeon", "cur_map_id", "map_id", "scene_id", "dungeon_scene_id"),
    "actor_uid": ("actor_uid", "attacker_uid", "source_uid", "caster_uid", "player_uid", "uid"),
    "target_uid": ("target_uid", "target_uuid", "uuid", "victim_uid", "boss_uid", "boss_uuid", "combatant_id"),
}


_NAME_KEYS = {
    "skill_name": ("skill_name", "skill_display", "skillName", "action_name", "skill"),
    "monster_name": ("monster_name", "target_name", "boss_name", "target", "victim", "boss"),
    "dungeon_name": ("dungeon_name", "dungeon_display", "scene_name", "map_name"),
    "actor": ("actor", "actor_display", "actor_name", "attacker_name", "attacker", "source_name", "caster_name", "player_name", "name"),
    "target": ("target", "target_display", "target_name", "victim_name", "victim", "boss_name", "boss"),
}


def _text(value: Any) -> str:
    try:
        text = str(value or "").strip()
    except Exception:
        return ""
    return text if text and text not in {"-", "0", "None", "none", "null"} else ""


def _safe_int(value: Any, default: int = 0) -> int:
    if isinstance(value, bool):
        return int(value)
    try:
        if isinstance(value, str):
            value = value.replace(",", "").strip()
            if not value:
                return default
        return int(float(value))
    except Exception:
        return default


def _safe_float(value: Any, default: float = 0.0) -> float:
    if isinstance(value, bool):
        return float(value)
    try:
        if isinstance(value, str):
            value = value.replace(",", "").strip()
            if not value:
                return default
        return float(value)
    except Exception:
        return default


def _mapping(value: Any) -> Mapping[str, Any]:
    return value if isinstance(value, Mapping) else {}


def _first_text(payload: Mapping[str, Any], keys: Iterable[str]) -> str:
    for key in keys:
        value = _text(payload.get(key))
        if value:
            return value
    return ""


def _first_int(payload: Mapping[str, Any], keys: Iterable[str]) -> int:
    for key in keys:
        value = _safe_int(payload.get(key), 0)
        if value:
            return value
    return 0


def _event_time_ms(item: Mapping[str, Any], payload: Mapping[str, Any]) -> int:
    for key in ("time_ms", "timestamp_ms", "elapsed_ms"):
        value = _safe_int(item.get(key) if item.get(key) is not None else payload.get(key), 0)
        if value:
            return value
    for key in ("timestamp", "observed_at", "time"):
        value = item.get(key) if item.get(key) is not None else payload.get(key)
        number = _safe_float(value, 0.0)
        if number:
            return int(number * 1000 if number < 10_000_000 else number)
    return 0


def _event_source(item: Mapping[str, Any], payload: Mapping[str, Any]) -> str:
    source = item.get("source")
    if isinstance(source, Mapping):
        return _first_text(source, ("name", "kind", "parser_id")) or "unknown"
    return _text(source) or _first_text(item, ("source_name", "source_kind")) or _first_text(payload, ("source_name", "source_kind")) or "unknown"


def _numeric_value(item: Mapping[str, Any], payload: Mapping[str, Any]) -> float:
    for src in (item, payload):
        for key in _NUMERIC_FIELDS:
            if src.get(key) is not None:
                value = _safe_float(src.get(key), 0.0)
                if value:
                    return value
    return 0.0


def _topic(item: Mapping[str, Any], payload: Mapping[str, Any]) -> str:
    return _text(item.get("topic")) or _text(payload.get("topic")) or _text(payload.get("kind")) or "event"


def _key(kind: str, ident: Any, name: str, fallback: str) -> str:
    ident_text = _text(ident)
    if ident_text:
        return f"{kind}:{ident_text}"
    name_text = _text(name)
    if name_text:
        return f"{kind}:name:{name_text.lower()}"
    return f"{kind}:{fallback}"


def _append_unique(values: list[Any], value: Any, *, cap: int = 24) -> None:
    text = _text(value)
    if not text:
        return
    if text in {_text(item) for item in values}:
        return
    if len(values) < cap:
        values.append(value)


def _counter_top(counter: Counter[str], *, limit: int = 5) -> list[dict[str, Any]]:
    return [
        {"key": key, "count": count}
        for key, count in sorted(counter.items(), key=lambda item: (-item[1], item[0]))[:max(1, int(limit or 5))]
        if key
    ]


def normalize_act_event_row(event_or_row: Mapping[str, Any], *, index: int = 0) -> dict[str, Any]:
    """Normalize a live event envelope, action-log row, or history row."""
    item = event_or_row if isinstance(event_or_row, Mapping) else {}
    payload = _mapping(item.get("payload"))
    combat_fact = _mapping(payload.get("combat_fact")) or _mapping(item.get("combat_fact"))
    if not payload and any(key in item for key in ("skill_id", "target_uid", "dungeon_id", "damage", "heal")):
        payload = item

    topic = _topic(item, payload)
    time_ms = _event_time_ms(item, payload)
    source = _event_source(item, payload)
    value = _numeric_value(item, payload)

    skill_id = _first_int(payload, _ID_KEYS["skill_id"]) or _first_int(combat_fact, _ID_KEYS["skill_id"])
    monster_id = _first_int(payload, _ID_KEYS["monster_id"]) or _first_int(combat_fact, _ID_KEYS["monster_id"])
    dungeon_id = _first_int(payload, _ID_KEYS["dungeon_id"]) or _first_int(combat_fact, _ID_KEYS["dungeon_id"])
    actor_uid = _first_int(payload, _ID_KEYS["actor_uid"]) or _safe_int(item.get("actor_uid"), 0)
    target_uid = _first_int(payload, _ID_KEYS["target_uid"]) or _safe_int(item.get("target_uid"), 0)

    skill_name = _first_text(payload, _NAME_KEYS["skill_name"]) or _first_text(combat_fact, _NAME_KEYS["skill_name"]) or _text(item.get("skill"))
    monster_name = _first_text(payload, _NAME_KEYS["monster_name"]) or _first_text(combat_fact, _NAME_KEYS["monster_name"]) or _text(item.get("monster"))
    dungeon_name = _first_text(payload, _NAME_KEYS["dungeon_name"]) or _first_text(combat_fact, _NAME_KEYS["dungeon_name"]) or _text(item.get("dungeon"))
    actor = _text(item.get("actor")) or _first_text(payload, _NAME_KEYS["actor"])
    target = _text(item.get("target")) or _first_text(payload, _NAME_KEYS["target"]) or monster_name
    label = _text(item.get("label")) or skill_name or _first_text(payload, ("display_label", "message", "action_name", "name")) or topic

    kind = "heal" if topic == "heal" or _safe_float(payload.get("heal") or payload.get("total_heal"), 0.0) else ("damage" if topic == "damage" or _safe_float(payload.get("damage") or payload.get("total_damage"), 0.0) else topic)
    row_id = _text(item.get("id")) or f"{topic}:{time_ms}:{index}"
    return {
        "index": int(item.get("index") or index),
        "id": row_id,
        "time_ms": time_ms,
        "topic": topic,
        "kind": kind,
        "source": source,
        "source_mode": _text(item.get("source_mode")) or ("history" if str(row_id).startswith("history:") else "live"),
        "label": label,
        "value": round(value, 3),
        "damage": round(value if kind == "damage" else _safe_float(payload.get("damage") or payload.get("total_damage"), 0.0), 3),
        "heal": round(value if kind == "heal" else _safe_float(payload.get("heal") or payload.get("total_heal"), 0.0), 3),
        "actor": actor,
        "target": target,
        "actor_uid": actor_uid,
        "target_uid": target_uid,
        "skill_id": skill_id,
        "skill_name": skill_name,
        "monster_id": monster_id,
        "monster_name": monster_name,
        "dungeon_id": dungeon_id,
        "dungeon_name": dungeon_name,
        "group_key": _text(item.get("group_key")),
        "group_kind": _text(item.get("group_kind")) or kind,
        "group_name": _text(item.get("group_name")) or label,
        "payload": dict(payload),
        "combat_fact": dict(combat_fact),
    }


def normalize_rows(rows: Iterable[Mapping[str, Any]]) -> list[dict[str, Any]]:
    normalized = [normalize_act_event_row(row, index=idx) for idx, row in enumerate(rows or []) if isinstance(row, Mapping)]
    normalized.sort(key=lambda row: (int(row.get("time_ms") or 0), int(row.get("index") or 0), str(row.get("id") or "")))
    return normalized


def _new_group(key: str, kind: str, name: str) -> dict[str, Any]:
    return {
        "key": key,
        "kind": kind,
        "name": name,
        "count": 0,
        "total_value": 0.0,
        "damage": 0.0,
        "heal": 0.0,
        "first_time_ms": 0,
        "last_time_ms": 0,
        "actors": [],
        "actor_uids": [],
        "targets": [],
        "target_uids": [],
        "skills": [],
        "skill_ids": [],
        "monsters": [],
        "monster_ids": [],
        "dungeons": [],
        "dungeon_ids": [],
        "sources": [],
        "topics": [],
        "rows": [],
        "has_more_rows": False,
    }


def _add_common(group: dict[str, Any], row: Mapping[str, Any], *, sample_limit: int = 8) -> None:
    time_ms = int(row.get("time_ms") or 0)
    group["count"] = int(group.get("count") or 0) + 1
    group["total_value"] = round(float(group.get("total_value") or 0.0) + float(row.get("value") or 0.0), 3)
    group["damage"] = round(float(group.get("damage") or 0.0) + float(row.get("damage") or 0.0), 3)
    group["heal"] = round(float(group.get("heal") or 0.0) + float(row.get("heal") or 0.0), 3)
    if time_ms:
        first = int(group.get("first_time_ms") or 0)
        group["first_time_ms"] = time_ms if not first else min(first, time_ms)
        group["last_time_ms"] = max(int(group.get("last_time_ms") or 0), time_ms)
    for field, out_key in (
        ("actor", "actors"), ("actor_uid", "actor_uids"), ("target", "targets"),
        ("target_uid", "target_uids"), ("skill_name", "skills"), ("skill_id", "skill_ids"),
        ("monster_name", "monsters"), ("monster_id", "monster_ids"),
        ("dungeon_name", "dungeons"), ("dungeon_id", "dungeon_ids"),
        ("source", "sources"), ("topic", "topics"),
    ):
        _append_unique(group[out_key], row.get(field))
    if len(group["rows"]) < max(1, int(sample_limit or 8)):
        group["rows"].append(dict(row))
    else:
        group["has_more_rows"] = True


def _finalize_groups(groups: Iterable[dict[str, Any]], *, top_n: int = 20) -> list[dict[str, Any]]:
    out = []
    for group in groups:
        item = dict(group)
        item["total_value"] = round(float(item.get("total_value") or 0.0), 3)
        item["damage"] = round(float(item.get("damage") or 0.0), 3)
        item["heal"] = round(float(item.get("heal") or 0.0), 3)
        item["duration_ms"] = max(0, int(item.get("last_time_ms") or 0) - int(item.get("first_time_ms") or 0))
        out.append(item)
    out.sort(key=lambda item: (-float(item.get("damage") or item.get("total_value") or 0.0), -int(item.get("count") or 0), str(item.get("name") or ""), str(item.get("key") or "")))
    return out[:max(1, int(top_n or 20))]


def build_timeline_clusters(rows: Iterable[Mapping[str, Any]], *, window_ms: int = 1000, top_n: int = 80) -> list[dict[str, Any]]:
    normalized = normalize_rows(rows)
    if not normalized:
        return []
    bucket_ms = max(100, int(window_ms or 1000))
    first_ms = int(normalized[0].get("time_ms") or 0)
    groups: dict[int, dict[str, Any]] = {}
    topic_counters: dict[int, Counter[str]] = defaultdict(Counter)
    label_counters: dict[int, Counter[str]] = defaultdict(Counter)
    for row in normalized:
        time_ms = int(row.get("time_ms") or 0)
        bucket = ((time_ms - first_ms) // bucket_ms) if first_ms else (time_ms // bucket_ms)
        start = first_ms + bucket * bucket_ms if first_ms else bucket * bucket_ms
        key = f"time:{start}:{start + bucket_ms}"
        group = groups.setdefault(bucket, _new_group(key, "timeline", f"{start}-{start + bucket_ms}ms"))
        group["start_ms"] = start
        group["end_ms"] = start + bucket_ms
        _add_common(group, row, sample_limit=10)
        topic_counters[bucket][str(row.get("topic") or "event")] += 1
        label_counters[bucket][str(row.get("label") or "-")] += 1
    ordered = [groups[key] for key in sorted(groups)]
    for idx, group in enumerate(ordered):
        group["topics_top"] = _counter_top(topic_counters[idx], limit=5)
        group["labels_top"] = _counter_top(label_counters[idx], limit=5)
    return ordered[:max(1, int(top_n or 80))]


def aggregate_damage_by_skill(rows: Iterable[Mapping[str, Any]], *, top_n: int = 20) -> list[dict[str, Any]]:
    groups: dict[str, dict[str, Any]] = {}
    for row in normalize_rows(rows):
        if not (float(row.get("damage") or 0.0) or float(row.get("heal") or 0.0) or row.get("skill_id") or row.get("skill_name")):
            continue
        key = _key("skill", row.get("skill_id"), str(row.get("skill_name") or ""), str(row.get("label") or row.get("id") or "unknown"))
        name = str(row.get("skill_name") or row.get("label") or row.get("skill_id") or "Unknown Skill")
        group = groups.setdefault(key, _new_group(key, "skill", name))
        _add_common(group, row)
    return _finalize_groups(groups.values(), top_n=top_n)


def aggregate_damage_by_monster(rows: Iterable[Mapping[str, Any]], *, top_n: int = 20) -> list[dict[str, Any]]:
    groups: dict[str, dict[str, Any]] = {}
    for row in normalize_rows(rows):
        if not (float(row.get("damage") or 0.0) or row.get("monster_id") or row.get("monster_name") or row.get("target_uid")):
            continue
        key = _key("monster", row.get("monster_id") or row.get("target_uid"), str(row.get("monster_name") or row.get("target") or ""), str(row.get("id") or "unknown"))
        name = str(row.get("monster_name") or row.get("target") or row.get("monster_id") or row.get("target_uid") or "Unknown Target")
        group = groups.setdefault(key, _new_group(key, "monster", name))
        _add_common(group, row)
    return _finalize_groups(groups.values(), top_n=top_n)


def aggregate_damage_by_dungeon(rows: Iterable[Mapping[str, Any]], *, top_n: int = 12) -> list[dict[str, Any]]:
    groups: dict[str, dict[str, Any]] = {}
    for row in normalize_rows(rows):
        dungeon_id = row.get("dungeon_id")
        dungeon_name = str(row.get("dungeon_name") or "")
        if not (dungeon_id or dungeon_name or float(row.get("damage") or 0.0) or float(row.get("heal") or 0.0)):
            continue
        key = _key("dungeon", dungeon_id, dungeon_name, "live")
        name = dungeon_name or (str(dungeon_id) if dungeon_id else "Live Encounter")
        group = groups.setdefault(key, _new_group(key, "dungeon", name))
        _add_common(group, row)
    return _finalize_groups(groups.values(), top_n=top_n)


def aggregate_action_log(rows: Iterable[Mapping[str, Any]], *, top_n: int = 20) -> list[dict[str, Any]]:
    groups: dict[str, dict[str, Any]] = {}
    for row in normalize_rows(rows):
        key = str(row.get("group_key") or "")
        if not key:
            topic = str(row.get("topic") or "event")
            label = str(row.get("group_name") or row.get("label") or "event")
            key = f"log:{topic}:{label.lower()}"
        name = str(row.get("group_name") or row.get("label") or row.get("topic") or "Event")
        group = groups.setdefault(key, _new_group(key, str(row.get("group_kind") or row.get("topic") or "log"), name))
        _add_common(group, row)
    return _finalize_groups(groups.values(), top_n=top_n)


def _source_mix(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    counter: Counter[str] = Counter(str(row.get("source") or "unknown") for row in rows)
    total = max(1, sum(counter.values()))
    return [
        {"source": key, "count": count, "ratio": round(count / total, 4)}
        for key, count in sorted(counter.items(), key=lambda item: (-item[1], item[0]))
    ]


def _overview(rows: list[dict[str, Any]], render_spec: Mapping[str, Any] | None = None) -> dict[str, Any]:
    render_spec = render_spec if isinstance(render_spec, Mapping) else {}
    totals = render_spec.get("totals") if isinstance(render_spec.get("totals"), Mapping) else {}
    encounter = render_spec.get("encounter") if isinstance(render_spec.get("encounter"), Mapping) else {}
    context = render_spec.get("context") if isinstance(render_spec.get("context"), Mapping) else {}
    first = min((int(row.get("time_ms") or 0) for row in rows if int(row.get("time_ms") or 0)), default=0)
    last = max((int(row.get("time_ms") or 0) for row in rows if int(row.get("time_ms") or 0)), default=0)
    damage = sum(float(row.get("damage") or 0.0) for row in rows)
    heal = sum(float(row.get("heal") or 0.0) for row in rows)
    render_damage = _safe_float(totals.get("damage"), 0.0)
    render_heal = _safe_float(totals.get("heal"), 0.0)
    elapsed_s = _safe_float(totals.get("elapsed_s") or encounter.get("duration_s"), 0.0)
    if not elapsed_s and first and last:
        elapsed_s = max(0.0, (last - first) / 1000.0)
    final_damage = render_damage or damage
    final_heal = render_heal or heal
    return {
        "event_count": len(rows),
        "damage": round(final_damage, 3),
        "heal": round(final_heal, 3),
        "dps": round(_safe_float(totals.get("dps"), 0.0) or (final_damage / elapsed_s if elapsed_s > 0 else 0.0), 3),
        "hps": round(_safe_float(totals.get("hps"), 0.0) or (final_heal / elapsed_s if elapsed_s > 0 else 0.0), 3),
        "elapsed_s": round(elapsed_s, 3),
        "first_time_ms": first,
        "last_time_ms": last,
        "span_ms": max(0, last - first),
        "skill_count": len({row.get("skill_id") or row.get("skill_name") for row in rows if row.get("skill_id") or row.get("skill_name")}),
        "monster_count": len({row.get("monster_id") or row.get("monster_name") or row.get("target_uid") for row in rows if row.get("monster_id") or row.get("monster_name") or row.get("target_uid")}),
        "dungeon_count": len({row.get("dungeon_id") or row.get("dungeon_name") for row in rows if row.get("dungeon_id") or row.get("dungeon_name")}),
        "dungeon_name": _text(context.get("dungeon_name")) or _text(render_spec.get("title")),
        "encounter_id": _text(encounter.get("id")),
        "mode": _text(render_spec.get("mode")) or "live",
    }


def build_act_aggregate_summary(rows: Iterable[Mapping[str, Any]], *, render_spec: Mapping[str, Any] | None = None,
                                window_ms: int = 1000, top_n: int = 20) -> dict[str, Any]:
    normalized = normalize_rows(rows)
    return {
        "overview": _overview(normalized, render_spec),
        "timeline_clusters": build_timeline_clusters(normalized, window_ms=window_ms, top_n=max(top_n, 24)),
        "skill_damage": aggregate_damage_by_skill(normalized, top_n=top_n),
        "monster_damage": aggregate_damage_by_monster(normalized, top_n=top_n),
        "dungeon_damage": aggregate_damage_by_dungeon(normalized, top_n=min(top_n, 12)),
        "log_groups": aggregate_action_log(normalized, top_n=top_n),
        "source_mix": _source_mix(normalized),
        "raw_counts": {
            "rows": len(normalized),
            "timeline_clusters": len(build_timeline_clusters(normalized, window_ms=window_ms, top_n=max(top_n, 24))),
            "skills": len(aggregate_damage_by_skill(normalized, top_n=10_000)),
            "monsters": len(aggregate_damage_by_monster(normalized, top_n=10_000)),
            "dungeons": len(aggregate_damage_by_dungeon(normalized, top_n=10_000)),
            "logs": len(aggregate_action_log(normalized, top_n=10_000)),
        },
    }


__all__ = [
    "normalize_act_event_row",
    "normalize_rows",
    "build_timeline_clusters",
    "aggregate_damage_by_skill",
    "aggregate_damage_by_monster",
    "aggregate_damage_by_dungeon",
    "aggregate_action_log",
    "build_act_aggregate_summary",
]
