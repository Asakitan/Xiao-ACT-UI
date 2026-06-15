# -*- coding: utf-8 -*-
"""Ingest the in-memory localization string pool into our runtime name tables.

The MEM dump ``live_string_pool_all_localization.json`` is a raw ``index -> text``
pool (≈108k strings) with **no game ids**. To place a name into an id->name
table we need an *id <-> string-index* bridge. Two bridges are supported:

  1. route-2 ``live_id_name_index.json`` — produced by the extended C# mem probe
     that walks the IL2CPP SkillTable/BuffTable arrays (id + NameStringIndex).
     Shape: ``{"<id_space>": {"<id>": <string_index>, ...}, ...}``.
     This is the only way to mint *new* ids purely from memory (see
     ``.vscode/handoff`` route-2 design).

  2. ``live_probe_act_matched_rows.json`` — the existing matched rows that already
     carry ``text`` + ``primary_match{id_space,id}`` (anchored historically). Used
     as a fallback / demo bridge.

Both are funnelled through the SAME compact builders the runtime already uses
(``build_index_from_live_rows`` + ``sanitize_shared_cache``) and folded onto the
on-disk tables via ``hybrid_name_tables.overlay_cache_into_existing_tables`` — so
classification and placement reuse the self-contained classifier; nothing here
re-implements an id->kind path.

Usage:
    python -m tools.tablekit.mem_name_ingest --dry-run
    python -m tools.tablekit.mem_name_ingest --apply
    python -m tools.tablekit.mem_name_ingest --pool <pool.json> --idmap <id_name_index.json> --apply
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Any

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from net.tcp_name_cache import build_index_from_live_rows, sanitize_shared_cache  # noqa: E402

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO = os.path.dirname(os.path.dirname(_HERE))
_NAME_TABLES = os.path.join(_SAO, "assets", "name_tables")
# Derived/volatile: ``live_`` prefix is already gitignored and excluded from the
# runtime resolver's asset set.
_DEFAULT_MEM_CACHE = os.path.join(_NAME_TABLES, "live_mem_name_cache.json")

# Where the MEM dumps may live (Python-side first, then the C# build outputs).
_POOL_NAMES = ("live_string_pool_all_localization.json",)
_IDMAP_NAMES = ("live_id_name_index.json",)
_MATCHED_NAMES = ("live_probe_act_matched_rows.json",)


def _load_json(path: str) -> Any:
    if not path or not os.path.isfile(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def _discover(names: tuple[str, ...]) -> str:
    """Find a dump by name under assets/name_tables or any C# bin output."""
    for name in names:
        local = os.path.join(_NAME_TABLES, name)
        if os.path.isfile(local):
            return local
    # Search C# build outputs (read-only) as a fallback.
    import glob
    for name in names:
        hits = glob.glob(os.path.join(_SAO, "C#", "**", "bin", "**", name), recursive=True)
        if hits:
            return sorted(hits)[0]
    return ""


def _pool_index_to_text(pool: Any) -> dict[int, str]:
    rows = pool.get("rows") if isinstance(pool, dict) else None
    out: dict[int, str] = {}
    for row in rows or []:
        if not isinstance(row, dict):
            continue
        try:
            idx = int(row.get("index"))
        except (TypeError, ValueError):
            continue
        text = str(row.get("text") or "").strip()
        if text:
            out[idx] = text
    return out


def _rows_from_idmap(idmap: Any, index_to_text: dict[int, str]) -> list[dict[str, Any]]:
    """Build synthetic live-rows from a route-2 id->string_index map."""
    rows: list[dict[str, Any]] = []
    if not isinstance(idmap, dict):
        return rows
    for id_space, table in idmap.items():
        if not isinstance(table, dict):
            continue
        for sid, sidx in table.items():
            try:
                text = index_to_text.get(int(sidx), "")
            except (TypeError, ValueError):
                text = ""
            if not text:
                continue
            rows.append({
                "text": text,
                "confidence": "mem",
                "primary_match": {"id_space": str(id_space), "id": int(sid) if str(sid).lstrip("-").isdigit() else sid},
            })
    return rows


def build_mem_cache(pool_path: str = "", idmap_path: str = "", matched_path: str = "") -> dict[str, Any]:
    """Build a compact by-kind cache (tcp_preparse schema) from MEM dumps."""
    pool_path = pool_path or _discover(_POOL_NAMES)
    idmap_path = idmap_path or _discover(_IDMAP_NAMES)
    matched_path = matched_path or _discover(_MATCHED_NAMES)

    bridge = "none"
    rows: list[dict[str, Any]] = []
    idmap = _load_json(idmap_path)
    if isinstance(idmap, dict) and idmap:
        pool = _load_json(pool_path) or {}
        index_to_text = _pool_index_to_text(pool)
        rows = _rows_from_idmap(idmap, index_to_text)
        bridge = "id_name_index+string_pool"
    else:
        matched = _load_json(matched_path)
        if matched is not None:
            # build_index_from_live_rows understands matched_rows directly.
            index = sanitize_shared_cache(build_index_from_live_rows(matched))
            return {
                "bridge": "matched_rows",
                "pool": os.path.normpath(pool_path) if pool_path else "",
                "source": os.path.normpath(matched_path) if matched_path else "",
                "index": index,
            }

    # MEM-anchored rows are authoritative; accept the "mem" confidence too.
    index = sanitize_shared_cache(build_index_from_live_rows(
        {"rows": rows}, confidence={"high", "medium", "mem", "tcp", "static"}))
    return {
        "bridge": bridge,
        "pool": os.path.normpath(pool_path) if pool_path else "",
        "source": os.path.normpath(idmap_path) if idmap_path else "",
        "index": index,
    }


def _summary(index: dict[str, Any]) -> dict[str, int]:
    by_kind = ((index.get("names") or {}).get("by_kind") or {})
    return {k: len(v or {}) for k, v in sorted(by_kind.items())}


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--pool", default="", help="live_string_pool_all_localization.json")
    p.add_argument("--idmap", default="", help="route-2 live_id_name_index.json (id_space->{id:string_index})")
    p.add_argument("--matched", default="", help="live_probe_act_matched_rows.json (fallback bridge)")
    p.add_argument("--out", default=_DEFAULT_MEM_CACHE, help="compact mem cache output path")
    p.add_argument("--apply", action="store_true", help="overlay the mem names onto the on-disk tables")
    p.add_argument("--dry-run", action="store_true", help="report only; write nothing")
    args = p.parse_args(argv)

    built = build_mem_cache(args.pool, args.idmap, args.matched)
    index = built["index"]
    report = {
        "bridge": built["bridge"],
        "pool": built["pool"],
        "source": built["source"],
        "by_kind_counts": _summary(index),
        "total_ids": sum(_summary(index).values()),
    }

    if not args.dry_run:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(index, f, ensure_ascii=False, indent=2, sort_keys=True)
            f.write("\n")
        report["written_cache"] = os.path.normpath(args.out)
        if args.apply:
            from tools.tablekit.hybrid_name_tables import overlay_cache_into_existing_tables
            ov = overlay_cache_into_existing_tables(args.out, write=True)
            report["overlay_changed"] = {k: v["changed"] for k, v in ov["kinds"].items() if v["changed"]}
            report["overlay_total_changed"] = sum(v["changed"] for v in ov["kinds"].values())

    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
