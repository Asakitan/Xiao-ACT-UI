# Compose a live all-name + TCP/config correspondence audit table.
#
# Inputs:
# - live StringPool full export (all localization rows)
# - ACT matched runtime rows (TCP/config/community matches)
# - TcpNameCache compact/runtime endpoint cache
#
# The output keeps all localization names while annotating entries that have known
# TCP/config ID matches or runtime endpoint observations.

from __future__ import annotations

import argparse
import hashlib
import json
import os
import time
from collections import defaultdict
from typing import Any, Dict, Iterable, List, Optional

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO = os.path.dirname(os.path.dirname(_HERE))
_NAME_TABLES = os.path.join(_SAO, "assets", "name_tables")
_DEFAULT_FULL = os.path.join(_NAME_TABLES, "live_string_pool_all_localization.json")
_DEFAULT_MATCHED = os.path.join(_NAME_TABLES, "live_probe_act_matched_rows.json")
_DEFAULT_CACHE = os.path.join(_NAME_TABLES, "tcp_preparse_name_cache.json")
_DEFAULT_OUTPUT = os.path.join(_NAME_TABLES, "live_name_tcp_correspondence.json")

_ID_SPACE_KIND = {
    "skill_id": "skill",
    "skill_id_or_legacy_skill_name_index": "skill",
    "sub_profession_skill_id": "skill",
    "buff_id": "buff",
    "monster_id": "monster",
    "dungeon_id": "dungeon",
    "scene_id": "dungeon",
    "npc_id": "npc",
    "item_id": "item",
}


def _load_json(path: str) -> Any:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _write_json(path: str, payload: Any, *, indent: Optional[int] = 2) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=indent)


def _rel(path: str) -> str:
    try:
        return os.path.relpath(path, _SAO).replace("\\", "/")
    except ValueError:
        return path.replace("\\", "/")


def _iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def _text_hash(text: str) -> str:
    return hashlib.sha1(text.encode("utf-8", errors="ignore")).hexdigest()[:16]


def _full_rows(full: Dict[str, Any]) -> List[Dict[str, Any]]:
    rows = full.get("rows") or full.get("allLocalizationString") or full.get("strings") or []
    if isinstance(rows, dict):
        rows = rows.get("rows") or rows.get("entries") or []
    out: List[Dict[str, Any]] = []
    for fallback_idx, row in enumerate(rows):
        if not isinstance(row, dict):
            continue
        item = dict(row)
        try:
            item["index"] = int(item.get("index"))
        except Exception:
            item["index"] = fallback_idx
        out.append(item)
    return out


def _match_sort_key(row: Dict[str, Any]) -> tuple:
    conf = {"high": 0, "medium": 1, "low": 2}.get(str(row.get("confidence") or ""), 9)
    status = 0 if str(row.get("tcp_status") or "") in {"parsed_by_tcp", "parsed_by_tcp_alias"} else 1
    return (conf, status, str(row.get("id_space") or ""), int(row.get("id") or 0), str(row.get("source") or ""))


def _cache_index(cache: Dict[str, Any]) -> Dict[str, Dict[str, Any]]:
    out: Dict[str, Dict[str, Any]] = {}
    by_kind = (((cache.get("names") or {}).get("by_kind") or {}))
    for kind, bucket in by_kind.items():
        if not isinstance(bucket, dict) or kind == "player":
            continue
        for raw_id, entry in bucket.items():
            if not isinstance(entry, dict):
                continue
            out[f"{kind}:{raw_id}"] = dict(entry)
    return out


def _endpoint_rows(cache: Dict[str, Any], endpoint_ids: Iterable[str]) -> List[Dict[str, Any]]:
    endpoints = cache.get("endpoints") if isinstance(cache, dict) else {}
    out = []
    for endpoint in endpoint_ids or []:
        row = dict((endpoints or {}).get(str(endpoint)) or {})
        row.setdefault("endpoint_hex", str(endpoint))
        out.append(row)
    return out


def compose(full_input: str, matched_input: str, cache_input: str, output: str) -> Dict[str, Any]:
    full = _load_json(full_input)
    matched = _load_json(matched_input)
    cache = _load_json(cache_input) if os.path.isfile(cache_input) else {}
    cache_by_kind_id = _cache_index(cache)

    rows = _full_rows(full)
    by_index: Dict[int, List[Dict[str, Any]]] = defaultdict(list)
    for row in matched.get("rows") or []:
        runtime = row.get("runtime") or {}
        idx = runtime.get("allLocalizationString_index")
        if isinstance(idx, int):
            by_index[idx].append(dict(row))

    entries: List[Dict[str, Any]] = []
    by_text: Dict[str, List[int]] = defaultdict(list)
    by_id_space: Dict[str, Dict[str, List[int]]] = defaultdict(lambda: defaultdict(list))
    matched_count = 0
    endpoint_count = 0
    for row in rows:
        idx = int(row.get("index") or 0)
        text = str(row.get("text") or "")
        matches = by_index.get(idx, [])
        tcp_matches: List[Dict[str, Any]] = []
        runtime = {
            "allLocalizationString_index": idx,
            "string_obj": row.get("string_obj") or row.get("obj"),
            "string_chars": row.get("string_chars") or row.get("chars"),
            "string_klass": row.get("string_klass") or row.get("klass"),
            "decode_status": row.get("decode_status"),
            "length": row.get("length"),
        }
        primary = None
        endpoints: List[Dict[str, Any]] = []
        for match_row in matches:
            for match in match_row.get("matches") or []:
                tcp_matches.append(dict(match))
            if primary is None and match_row.get("primary_match"):
                primary = dict(match_row.get("primary_match"))
            mruntime = match_row.get("runtime") or {}
            runtime.update({k: v for k, v in mruntime.items() if v is not None})
        tcp_matches = sorted(tcp_matches, key=_match_sort_key)
        if primary is None and tcp_matches:
            primary = tcp_matches[0]
        cache_entry = None
        if primary:
            id_space = str(primary.get("id_space") or "")
            kind = _ID_SPACE_KIND.get(id_space, "")
            raw_id = primary.get("id")
            if kind and raw_id is not None:
                cache_entry = cache_by_kind_id.get(f"{kind}:{raw_id}")
        if cache_entry:
            endpoints = _endpoint_rows(cache, cache_entry.get("endpoints") or [])
            endpoint_count += len(endpoints)
        if tcp_matches:
            matched_count += 1
        entry_index = len(entries)
        if text:
            by_text[text].append(entry_index)
        if primary:
            by_id_space[str(primary.get("id_space") or "unknown")][str(primary.get("id") or "")].append(entry_index)
        entries.append({
            "index": idx,
            "text": text,
            "text_hash": _text_hash(text) if text else "",
            "primary_match": primary,
            "tcp_matches": tcp_matches,
            "cache_entry": cache_entry,
            "endpoints": endpoints,
            "runtime_provenance": runtime,
            "classification": "tcp_or_config_matched_runtime_name" if tcp_matches else "runtime_localization_only",
            "confidence": (matches[0].get("confidence") if matches else "none"),
        })

    duplicate_texts = {text: len(indexes) for text, indexes in by_text.items() if len(indexes) > 1}
    result = {
        "source": "build_live_name_tcp_correspondence",
        "generated_at": _iso(),
        "inputs": {
            "full": _rel(full_input),
            "matched": _rel(matched_input),
            "cache": _rel(cache_input),
        },
        "process": full.get("process"),
        "pid": full.get("pid"),
        "anchor": full.get("anchor"),
        "summary": {
            "total_entries": len(entries),
            "nonempty_text_count": sum(1 for item in entries if item.get("text")),
            "tcp_matched_entry_count": matched_count,
            "endpoint_annotation_count": endpoint_count,
            "cache_endpoint_count": len((cache.get("endpoints") or {}) if isinstance(cache, dict) else {}),
            "duplicate_text_count": len(duplicate_texts),
            "cache_kind_counts": {
                kind: len(bucket or {})
                for kind, bucket in (((cache.get("names") or {}).get("by_kind") or {})).items()
            },
        },
        "indices": {
            "by_text": dict(by_text),
            "by_id_space": {space: dict(ids) for space, ids in by_id_space.items()},
        },
        "diagnostics": {
            "duplicate_texts": duplicate_texts,
            "anchor_status": ((full.get("anchor") or {}).get("status")),
        },
        "names": entries,
    }
    _write_json(output, result, indent=2)
    return result


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Build all-name + TCP/config correspondence audit table")
    parser.add_argument("--full-input", default=_DEFAULT_FULL)
    parser.add_argument("--matched-input", default=_DEFAULT_MATCHED)
    parser.add_argument("--cache-input", default=_DEFAULT_CACHE)
    parser.add_argument("--output", default=_DEFAULT_OUTPUT)
    args = parser.parse_args(argv)
    result = compose(args.full_input, args.matched_input, args.cache_input, args.output)
    print(json.dumps({"output": args.output, "summary": result.get("summary"), "anchor_status": (result.get("diagnostics") or {}).get("anchor_status")}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
