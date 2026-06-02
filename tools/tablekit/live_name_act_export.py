"""Export ACT-focused rows from live runtime name pointer tables.

This tool reads the runtime evidence produced by ``live_name_pointer_table.py``
and keeps only rows that already have known TCP/config/community matches.  The
export is intentionally marked as volatile: heap addresses and table indices are
runtime evidence until an owner/static root is resolved for the current game
build.
"""

from __future__ import annotations

import argparse
import json
import os
from collections import defaultdict
from typing import Any, Dict, Iterable, List, Optional, Tuple


_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO = os.path.dirname(os.path.dirname(_HERE))
_NAME_TABLES = os.path.join(_SAO, "assets", "name_tables")
_DEFAULT_STATIC_ANCHOR = os.path.join(_NAME_TABLES, "live_string_pool_static_anchor.json")
_ID_PRIORITY = {
    "sub_profession_skill_id": 0,
    "skill_id": 1,
    "buff_id": 2,
    "skill_effect_id": 3,
    "temp_attr_id": 4,
    "profession_id": 5,
}
_CONF_PRIORITY = {"high": 0, "medium": 1, "low": 2}


def _load_json(path: str) -> Any:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _write_json(path: str, payload: Any) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)


def _rel(path: str) -> str:
    return path.replace("\\", "/")


def _safe_int(raw: Any) -> Optional[int]:
    if raw is None:
        return None
    try:
        return int(str(raw), 16) if isinstance(raw, str) else int(raw)
    except (TypeError, ValueError):
        return None


def _match_sort_key(match: Dict[str, Any]) -> Tuple[int, int, int, str]:
    return (
        _CONF_PRIORITY.get(str(match.get("confidence")), 9),
        _ID_PRIORITY.get(str(match.get("id_space")), 99),
        int(match.get("id") or 0),
        str(match.get("source_kind") or ""),
    )


def _dedupe_matches(matches: Iterable[Dict[str, Any]]) -> List[Dict[str, Any]]:
    seen = set()
    out: List[Dict[str, Any]] = []
    for match in matches:
        key = (
            match.get("id_space"),
            match.get("id"),
            match.get("source"),
            match.get("source_kind"),
            match.get("field"),
            match.get("match"),
            match.get("alias_of"),
        )
        if key in seen:
            continue
        seen.add(key)
        out.append(dict(match))
    out.sort(key=_match_sort_key)
    return out


def _primary_match(matches: List[Dict[str, Any]]) -> Optional[Dict[str, Any]]:
    if not matches:
        return None
    return sorted(matches, key=_match_sort_key)[0]


def _confidence(matches: List[Dict[str, Any]]) -> str:
    if any(m.get("confidence") == "high" and m.get("match") == "exact" for m in matches):
        return "high"
    if any(m.get("tcp_status") in {"parsed_by_tcp", "parsed_by_tcp_alias"} for m in matches):
        return "medium"
    if matches:
        return "medium"
    return "none"


def _load_static_anchor(anchor_input: Optional[str]) -> Optional[Dict[str, Any]]:
    if not anchor_input or not os.path.isfile(anchor_input):
        return None
    return _load_json(anchor_input)


def _projection_by_table(anchor: Optional[Dict[str, Any]]) -> Dict[int, Dict[str, Any]]:
    if not anchor:
        return {}
    out: Dict[int, Dict[str, Any]] = {}
    for projection in anchor.get("pointer_table_projections") or []:
        if projection.get("array_slice_status") != "matches_all_rows":
            continue
        table_index = projection.get("table_index")
        if isinstance(table_index, int):
            out[table_index] = dict(projection)
    return out


def _anchored_runtime(row_runtime: Dict[str, Any], row_index: int,
                      projection: Optional[Dict[str, Any]], anchor: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    if not projection or not anchor:
        row_runtime["anchor_status"] = "runtime_heap_pointer_table_only"
        return row_runtime
    global_start = projection.get("global_start_index")
    if not isinstance(global_start, int):
        row_runtime["anchor_status"] = "runtime_heap_pointer_table_only"
        return row_runtime
    all_loc = (((anchor.get("anchor") or {}).get("allLocalizationString_") or {}))
    row_runtime.update({
        "anchor_status": "resolved_static_singleton_string_pool",
        "static_anchor_chain": ((anchor.get("anchor") or {}).get("chain")),
        "allLocalizationString_array_obj": all_loc.get("array_obj"),
        "allLocalizationString_element_base": all_loc.get("element_base"),
        "allLocalizationString_index": global_start + row_index,
    })
    return row_runtime


def export_act_rows(pointer_tables_input: str, output: str, *, anchor_input: Optional[str] = None) -> Dict[str, Any]:
    data = _load_json(pointer_tables_input)
    anchor = _load_static_anchor(anchor_input)
    projections = _projection_by_table(anchor)
    rows: List[Dict[str, Any]] = []
    by_id_space: Dict[str, int] = defaultdict(int)
    by_text: Dict[str, int] = defaultdict(int)
    by_table: Dict[int, int] = defaultdict(int)
    anchored_row_count = 0

    for table_index, table in enumerate(data.get("tables") or []):
        element_base = table.get("element_base")
        projection = projections.get(table_index)
        for row in table.get("rows") or []:
            matches = _dedupe_matches(row.get("matches") or [])
            if not matches:
                continue
            string_info = row.get("string") or {}
            text = str(string_info.get("text") or "")
            primary = _primary_match(matches)
            if primary:
                by_id_space[str(primary.get("id_space") or "unknown")] += 1
            by_text[text] += 1
            by_table[table_index] += 1
            runtime = _anchored_runtime({
                "table_index": table_index,
                "table_element_base": element_base,
                "row_index": row.get("index"),
                "slot_addr": row.get("slot_addr"),
                "string_obj": string_info.get("obj"),
                "string_chars": string_info.get("chars"),
                "string_klass": string_info.get("klass"),
                "string_klass_name": "System.String",
            }, int(row.get("index") or 0), projection, anchor)
            if runtime.get("anchor_status") == "resolved_static_singleton_string_pool":
                anchored_row_count += 1
            rows.append({
                "text": text,
                "classification": "tcp_or_config_matched_runtime_name",
                "confidence": _confidence(matches),
                "primary_match": primary,
                "matches": matches,
                "runtime": runtime,
            })

    rows.sort(key=lambda item: (
        int((item.get("runtime") or {}).get("table_index") or 0),
        int((item.get("runtime") or {}).get("row_index") or 0),
        item.get("text") or "",
    ))
    duplicate_texts = {text: count for text, count in by_text.items() if count > 1}
    result = {
        "source": "live_name_act_export",
        "pointer_tables_input": _rel(pointer_tables_input),
        "process": data.get("process"),
        "pid": data.get("pid"),
        "known_runtime_klass": data.get("known_runtime_klass"),
        "note": (
            "ACT-focused matched subset from live runtime pointer tables. "
            "TCP/config matches are useful for display/cross-reference, but runtime "
            "heap addresses and row indices are volatile until a persistent owner/static "
            "root is resolved for the current build."
        ),
        "summary": {
            "source_table_count": len(data.get("tables") or []),
            "source_row_count": (data.get("summary") or {}).get("row_count"),
            "matched_row_count": len(rows),
            "unique_text_count": len(by_text),
            "duplicate_text_count": len(duplicate_texts),
            "matched_rows_by_table": dict(sorted(by_table.items())),
            "primary_id_space_counts": dict(sorted(by_id_space.items())),
            "anchored_row_count": anchored_row_count,
            "persistent_anchor_status": (
                "resolved_static_singleton_string_pool"
                if anchored_row_count else "unresolved"
            ),
        },
        "static_anchor": {
            "input": _rel(anchor_input) if anchor_input else None,
            "status": ((anchor or {}).get("anchor") or {}).get("status") if anchor else "not_available",
            "chain": ((anchor or {}).get("anchor") or {}).get("chain") if anchor else None,
        },
        "duplicate_texts": duplicate_texts,
        "rows": rows,
    }
    _write_json(output, result)
    return result


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Export ACT-focused matched live name rows")
    parser.add_argument("--pointer-tables-input", default=os.path.join(_NAME_TABLES, "live_probe_pointer_tables.json"))
    parser.add_argument("--output", default=os.path.join(_NAME_TABLES, "live_probe_act_matched_rows.json"))
    parser.add_argument("--anchor-input", default=_DEFAULT_STATIC_ANCHOR,
                        help="Optional compact static anchor summary from live_string_pool_static_export.py")
    args = parser.parse_args(argv)
    result = export_act_rows(args.pointer_tables_input, args.output, anchor_input=args.anchor_input)
    print(json.dumps({"output": args.output, "summary": result.get("summary")}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
