# -*- coding: utf-8 -*-
"""Promote stable live name cross-reference matches into compact runtime cache.

This tool consumes ``live_probe_id_candidates.json`` and writes only stable
``id_space -> id -> text`` matches into ``tcp_preparse_name_cache.json``.  Raw
runtime pointers and heap addresses are intentionally discarded.
"""
from __future__ import annotations

import argparse
import json
import os
import tempfile
import time
from typing import Any, Mapping

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO = os.path.dirname(os.path.dirname(_HERE))
_NAME_TABLES = os.path.join(_SAO, "assets", "name_tables")
_DEFAULT_INPUT = os.path.join(_NAME_TABLES, "live_probe_id_candidates.json")
_DEFAULT_CACHE = os.path.join(_NAME_TABLES, "tcp_preparse_name_cache.json")

_ID_SPACE_KIND = {
    "skill_id": "skill",
    "skill_id_or_legacy_skill_name_index": "skill",
    "sub_profession_skill_id": "skill",
    "buff_id": "buff",
    "monster_id": "monster",
    "boss_id": "boss",
    "dungeon_id": "dungeon",
    "scene_id": "dungeon",
    "boss_event_type": "boss_mechanic",
}
_ALLOWED_CONTEXT_KEYS = ("source", "source_kind", "field", "match", "id_space", "tcp_status", "alias_of", "alias_reason")
_CONFIDENCE_RANK = {"static": 50, "curated": 45, "mem": 40, "tcp": 35, "high": 30, "medium": 20, "low": 10}
_FALLBACK_PREFIXES = ("技能#", "怪物#", "Boss#", "地牢#", "Buff#", "机制#", "NPC#", "道具#")


def _iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def _load_json(path: str) -> Any:
    if not os.path.isfile(path):
        return {}
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _write_json_atomic(path: str, payload: Any) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=os.path.basename(path) + ".", suffix=".tmp", dir=os.path.dirname(path))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(payload, f, ensure_ascii=False, indent=2, sort_keys=True)
            f.write("\n")
        os.replace(tmp, path)
    finally:
        try:
            if os.path.exists(tmp):
                os.unlink(tmp)
        except Exception:
            pass


def _clean_text(value: Any) -> str:
    text = str(value or "").strip()
    if not text:
        return ""
    if any(text.startswith(prefix) for prefix in _FALLBACK_PREFIXES):
        return ""
    return text


def _rank(confidence: str) -> int:
    return _CONFIDENCE_RANK.get(str(confidence or "").strip().lower(), 0)


def _empty_cache() -> dict[str, Any]:
    return {"schema_version": 1, "updated_at": "", "endpoints": {}, "names": {"by_kind": {}}}


def _candidate_rows(obj: Any):
    if not isinstance(obj, Mapping):
        return []
    rows = obj.get("unique_candidates")
    if isinstance(rows, list):
        return rows
    out = []
    groups = obj.get("groups")
    if isinstance(groups, Mapping):
        for group in groups.values():
            if isinstance(group, Mapping):
                out.extend(group.get("unique_candidates") or group.get("candidates") or [])
            elif isinstance(group, list):
                out.extend(group)
    return out


def _best_matches(row: Mapping[str, Any], *, confidence: set[str], exact_only: bool) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for match in row.get("matches") or []:
        if not isinstance(match, Mapping):
            continue
        conf = str(match.get("confidence") or "").strip().lower()
        if conf not in confidence:
            continue
        if exact_only and match.get("match") != "exact":
            continue
        kind = _ID_SPACE_KIND.get(str(match.get("id_space") or ""), "")
        try:
            iid = int(match.get("id") or 0)
        except Exception:
            iid = 0
        if not kind or iid <= 0:
            continue
        out.append(dict(match))
    out.sort(key=lambda m: (-_rank(str(m.get("confidence") or "")), str(m.get("id_space") or ""), int(m.get("id") or 0)))
    return out


def promote_crossref(input_path: str = _DEFAULT_INPUT, cache_path: str = _DEFAULT_CACHE, *, write: bool = True,
                     confidence: set[str] | None = None, exact_only: bool = True,
                     fill_missing_only: bool = True) -> dict[str, Any]:
    accepted = confidence or {"high"}
    crossref = _load_json(input_path)
    cache = _load_json(cache_path)
    if not isinstance(cache, dict):
        cache = _empty_cache()
    cache.setdefault("schema_version", 1)
    cache.setdefault("endpoints", {})
    by_kind = cache.setdefault("names", {}).setdefault("by_kind", {})
    stats: dict[str, Any] = {
        "input": os.path.normpath(input_path),
        "cache": os.path.normpath(cache_path),
        "candidate_count": 0,
        "promoted": {},
        "skipped_existing": {},
        "skipped_unmatched": 0,
        "write": bool(write),
    }
    for row in _candidate_rows(crossref):
        if not isinstance(row, Mapping):
            continue
        stats["candidate_count"] += 1
        text = _clean_text(row.get("text"))
        if not text:
            stats["skipped_unmatched"] += 1
            continue
        matches = _best_matches(row, confidence=accepted, exact_only=exact_only)
        if not matches:
            stats["skipped_unmatched"] += 1
            continue
        for match in matches:
            id_space = str(match.get("id_space") or "")
            kind = _ID_SPACE_KIND[id_space]
            iid = str(int(match.get("id") or 0))
            bucket = by_kind.setdefault(kind, {})
            old = bucket.get(iid)
            old_conf = ""
            if isinstance(old, Mapping):
                old_conf = str(old.get("confidence") or "")
            if old and (fill_missing_only or _rank(old_conf) >= _rank(str(match.get("confidence") or ""))):
                stats["skipped_existing"][kind] = stats["skipped_existing"].get(kind, 0) + 1
                continue
            bucket[iid] = {
                "text": text,
                "confidence": str(match.get("confidence") or "high"),
                "sources": [str(match.get("source") or match.get("source_kind") or "live_crossref")],
                "endpoints": [],
                "updated_at": _iso(),
                "context": {key: match.get(key) for key in _ALLOWED_CONTEXT_KEYS if match.get(key) is not None},
            }
            stats["promoted"][kind] = stats["promoted"].get(kind, 0) + 1
    cache["updated_at"] = _iso()
    if write:
        _write_json_atomic(cache_path, cache)
    return stats


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Promote stable live name crossref matches into compact runtime cache")
    parser.add_argument("--input", default=_DEFAULT_INPUT)
    parser.add_argument("--cache", default=_DEFAULT_CACHE)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--allow-medium", action="store_true", help="Accept medium confidence matches in addition to high")
    parser.add_argument("--allow-alias", action="store_true", help="Accept non-exact alias matches")
    parser.add_argument("--allow-overwrite", action="store_true", help="Allow higher-confidence live entries to replace lower-confidence cache entries")
    args = parser.parse_args(argv)
    confidence = {"high", "medium"} if args.allow_medium else {"high"}
    result = promote_crossref(
        args.input,
        args.cache,
        write=not args.dry_run,
        confidence=confidence,
        exact_only=not args.allow_alias,
        fill_missing_only=not args.allow_overwrite,
    )
    print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
