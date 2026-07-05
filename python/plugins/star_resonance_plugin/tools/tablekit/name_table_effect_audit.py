# -*- coding: utf-8 -*-
# Audit live name-table mapping effectiveness.
#
# The audit intentionally treats runtime pointers as evidence only.  Consumers
# must use stable fields such as kind/id/text/confidence/index, not heap addresses.
from __future__ import annotations

import argparse
import json
import os
import re
from collections import Counter, defaultdict
from typing import Any, Iterable, Mapping

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO = os.path.dirname(os.path.dirname(_HERE))
_NAME_TABLES = os.path.join(_SAO, "assets", "name_tables")
_DEFAULT_FULL = os.path.join(_NAME_TABLES, "live_string_pool_all_localization.json")
_DEFAULT_MATCHED = os.path.join(_NAME_TABLES, "live_probe_act_matched_rows.json")
_DEFAULT_CACHE = os.path.join(_NAME_TABLES, "tcp_preparse_name_cache.json")
_DEFAULT_CORRESPONDENCE = os.path.join(_NAME_TABLES, "live_name_tcp_correspondence.json")
_RUNTIME_TABLE_KINDS = (
    "skill", "player_skill", "monster_skill", "environment_skill",
    "field_marker", "boss_skill", "ultimate_skill", "roguelike_affix",
    "profession_skill", "scripted_skill", "virtual_skill", "boss_mechanic_skill",
    "dungeon", "monster", "boss", "boss_mechanic",
    "buff", "player_buff", "factor_buff", "profession_skill_buff", "event",
)

_VOLATILE_KEYS = {
    "obj", "chars", "klass", "runtime_klass", "string_klass", "string_obj",
    "string_chars", "slot_addr", "array_obj", "element_base",
    "table_element_base", "allLocalizationString_array_obj",
    "allLocalizationString_element_base",
}
_FALLBACK_RE = re.compile(r"^(技能|玩家技能|怪物技能|环境技能|场地标记|Boss技能|幻想技能|肉鸽词条|职业技能|剧情表演|虚拟体技能|Boss机制技能|怪物|Boss|地牢|Buff|玩家Buff|因子Buff|职业技能Buff|事件|机制|NPC|道具)#\d+$")
_CACHE_FORBIDDEN_KEYS = {
    "endpoints", "player", "sources", "context", "updated_at",
    "first_seen", "last_seen", "seen_count", "player_uid", "player_uuid",
    "scene_guid", "connect_guid", "last_context", "ip", "port",
}
_CACHE_FORBIDDEN_VALUE_RE = re.compile(
    r"(?:\b\d{1,3}(?:\.\d{1,3}){3}\b|[0-9a-fA-F]{8}:\d+|StarResonanceDps/|resonance-logs-cn/)"
)


def _load_json(path: str) -> Any:
    if not path or not os.path.isfile(path):
        return {}
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _rel(path: str) -> str:
    try:
        return os.path.relpath(path, _SAO).replace("\\", "/")
    except ValueError:
        return os.path.abspath(path).replace("\\", "/")


def _rows_from_full(full: Any) -> list[Mapping[str, Any]]:
    if not isinstance(full, Mapping):
        return []
    rows = full.get("rows") or full.get("allLocalizationString") or full.get("strings") or []
    if isinstance(rows, Mapping):
        rows = rows.get("rows") or rows.get("entries") or []
    return [row for row in rows or [] if isinstance(row, Mapping)]


def _iter_dicts(obj: Any) -> Iterable[Mapping[str, Any]]:
    if isinstance(obj, Mapping):
        yield obj
        for value in obj.values():
            yield from _iter_dicts(value)
    elif isinstance(obj, list):
        for value in obj:
            yield from _iter_dicts(value)


def _count_volatile_keys(obj: Any) -> dict[str, int]:
    counts: Counter[str] = Counter()
    for row in _iter_dicts(obj):
        for key, value in row.items():
            if key in _VOLATILE_KEYS and value not in (None, "", 0):
                counts[key] += 1
    return dict(sorted(counts.items()))


def _cache_kind_counts(cache: Any) -> dict[str, int]:
    if not isinstance(cache, Mapping):
        return {}
    by_kind = (((cache.get("names") or {}).get("by_kind") or {}))
    return {str(kind): len(bucket or {}) for kind, bucket in sorted(by_kind.items()) if isinstance(bucket, Mapping)}


def _runtime_table_counts() -> dict[str, int]:
    counts: dict[str, int] = {}
    for kind in _RUNTIME_TABLE_KINDS:
        path = os.path.join(_NAME_TABLES, f"{kind}.json")
        data = _load_json(path)
        counts[kind] = len(data) if isinstance(data, Mapping) else 0
    return counts


def _fallback_labels_in_cache(cache: Any) -> dict[str, int]:
    counts: Counter[str] = Counter()
    if not isinstance(cache, Mapping):
        return {}
    by_kind = (((cache.get("names") or {}).get("by_kind") or {}))
    for kind, bucket in (by_kind.items() if isinstance(by_kind, Mapping) else []):
        if not isinstance(bucket, Mapping):
            continue
        for entry in bucket.values():
            if isinstance(entry, str):
                text = entry.strip()
            elif isinstance(entry, Mapping):
                text = str(entry.get("text") or entry.get("name") or "").strip()
            else:
                text = ""
            if _FALLBACK_RE.match(text):
                counts[str(kind)] += 1
    return dict(sorted(counts.items()))


def _cache_privacy_violations(cache: Any) -> dict[str, Any]:
    key_counts: Counter[str] = Counter()
    value_hits: list[str] = []
    if not isinstance(cache, Mapping):
        return {"ok": True, "forbidden_key_counts": {}, "forbidden_value_samples": []}
    for row in _iter_dicts(cache):
        for key, value in row.items():
            key_text = str(key)
            if key_text in _CACHE_FORBIDDEN_KEYS:
                key_counts[key_text] += 1
            if isinstance(value, str) and _CACHE_FORBIDDEN_VALUE_RE.search(value):
                if len(value_hits) < 16:
                    value_hits.append(value)
    ok = not key_counts and not value_hits
    return {
        "ok": ok,
        "forbidden_key_counts": dict(sorted(key_counts.items())),
        "forbidden_value_samples": value_hits,
    }


def _matched_counts(matched: Any) -> dict[str, Any]:
    rows = matched.get("rows") if isinstance(matched, Mapping) else []
    by_kind: Counter[str] = Counter()
    confidence: Counter[str] = Counter()
    anchored = 0
    for row in rows or []:
        if not isinstance(row, Mapping):
            continue
        confidence[str(row.get("confidence") or "none")] += 1
        match = row.get("primary_match") if isinstance(row.get("primary_match"), Mapping) else {}
        by_kind[str(match.get("id_space") or "unknown")] += 1
        runtime = row.get("runtime") if isinstance(row.get("runtime"), Mapping) else {}
        if runtime.get("anchor_status") not in {None, "", "runtime_heap_pointer_table_only", "unresolved"}:
            anchored += 1
    return {
        "row_count": len(rows or []),
        "anchored_row_count": anchored,
        "by_id_space": dict(sorted(by_kind.items())),
        "by_confidence": dict(sorted(confidence.items())),
        "summary": (matched.get("summary") if isinstance(matched, Mapping) else {}) or {},
    }


def audit(full_input: str = _DEFAULT_FULL,
          matched_input: str = _DEFAULT_MATCHED,
          cache_input: str = _DEFAULT_CACHE,
          correspondence_input: str = _DEFAULT_CORRESPONDENCE) -> dict[str, Any]:
    full = _load_json(full_input)
    matched = _load_json(matched_input)
    cache = _load_json(cache_input)
    correspondence = _load_json(correspondence_input)
    full_rows = _rows_from_full(full)
    nonempty = sum(1 for row in full_rows if str(row.get("text") or "").strip())
    anchor = (full.get("anchor") if isinstance(full, Mapping) else {}) or {}
    corr_summary = correspondence.get("summary") if isinstance(correspondence, Mapping) else {}
    cache_kind_counts = _cache_kind_counts(cache)
    if not corr_summary and (full_rows or cache_kind_counts):
        corr_summary = {
            "status": "not_generated_runtime_optional",
            "total_entries": len(full_rows),
            "nonempty_text_count": nonempty,
            "cache_kind_counts": cache_kind_counts,
        }
    return {
        "inputs": {
            "full": _rel(full_input),
            "matched": _rel(matched_input),
            "cache": _rel(cache_input),
            "correspondence": _rel(correspondence_input),
        },
        "anchor": {
            "status": anchor.get("status") or ((matched.get("static_anchor") or {}).get("status") if isinstance(matched, Mapping) else ""),
            "chain": anchor.get("chain"),
        },
        "full_string_pool": {
            "total_rows": len(full_rows),
            "nonempty_text_count": nonempty,
        },
        "matched_rows": _matched_counts(matched if isinstance(matched, Mapping) else {}),
        "tcp_preparse_cache": {
            "kind_counts": cache_kind_counts,
            "fallback_label_counts": _fallback_labels_in_cache(cache),
            "endpoint_count": len((cache.get("endpoints") or {}) if isinstance(cache, Mapping) else {}),
            "privacy": _cache_privacy_violations(cache),
        },
        "runtime_tables": {
            "kind_counts": _runtime_table_counts(),
        },
        "correspondence": corr_summary or {},
        "volatile_address_keys": {
            "full": _count_volatile_keys(full),
            "matched": _count_volatile_keys(matched),
            "cache": _count_volatile_keys(cache),
            "correspondence": _count_volatile_keys(correspondence),
        },
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Audit live name-table mapping effects")
    parser.add_argument("--full-input", default=_DEFAULT_FULL)
    parser.add_argument("--matched-input", default=_DEFAULT_MATCHED)
    parser.add_argument("--cache-input", default=_DEFAULT_CACHE)
    parser.add_argument("--correspondence-input", default=_DEFAULT_CORRESPONDENCE)
    args = parser.parse_args(argv)
    result = audit(args.full_input, args.matched_input, args.cache_input, args.correspondence_input)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    privacy = ((result.get("tcp_preparse_cache") or {}).get("privacy") or {})
    return 0 if privacy.get("ok", True) else 1


if __name__ == "__main__":
    raise SystemExit(main())
