# -*- coding: utf-8 -*-
"""Audit live name-table mapping effectiveness.

The audit intentionally treats runtime pointers as evidence only.  Consumers
must use stable fields such as kind/id/text/confidence/index, not heap addresses.
"""
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

_VOLATILE_KEYS = {
    "obj", "chars", "klass", "runtime_klass", "string_klass", "string_obj",
    "string_chars", "slot_addr", "array_obj", "element_base",
    "table_element_base", "allLocalizationString_array_obj",
    "allLocalizationString_element_base",
}
_FALLBACK_RE = re.compile(r"^(技能|怪物|地牢|Buff|NPC|道具)#\d+$")


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
            "kind_counts": _cache_kind_counts(cache),
            "fallback_label_counts": _fallback_labels_in_cache(cache),
            "endpoint_count": len((cache.get("endpoints") or {}) if isinstance(cache, Mapping) else {}),
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
    print(json.dumps(audit(args.full_input, args.matched_input, args.cache_input, args.correspondence_input), ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
