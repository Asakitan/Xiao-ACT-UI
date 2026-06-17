"""Resolve managed owner/class evidence for live name pointer tables.

This tool combines runtime pointer-table evidence with the current Il2CppDumper
``script.json``.  It builds a full runtime ``Il2CppClass* -> class name`` index
from ``*_TypeInfo`` RVAs, verifies the live ``System.String`` klass, and searches
near table bases for managed object headers.  It also optionally scans private
heap pointers to candidate table/array objects to find parent owners.

All addresses in the output are runtime evidence.  A persistent anchor requires a
resolved module RVA/static field or a reproducible owner chain.
"""

from __future__ import annotations

import argparse
import json
import os
import time
from collections import Counter, defaultdict
from typing import Any, Dict, Iterable, List, Optional, Set, Tuple

from mem_probe import cy_memscan as _cy
from plugins.star_resonance_plugin.mem.il2cpp.script_parser import ScriptIndex
from plugins.star_resonance_plugin.mem.process import StarProcess


_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO = os.path.dirname(os.path.dirname(_HERE))
_NAME_TABLES = os.path.join(_SAO, "assets", "name_tables")
_IL2CPP = os.path.join(_SAO, "mem_probe", "il2cpp")
_DEFAULT_DUMP_ID = "fdc7111b"
_DEFAULT_SCRIPT_JSON = os.path.join(_IL2CPP, "out", _DEFAULT_DUMP_ID, "script.json")
_DEFAULT_POINTER_TABLES = os.path.join(_NAME_TABLES, "live_probe_pointer_tables.json")
_DEFAULT_OUTPUT = os.path.join(_NAME_TABLES, "live_probe_owner_resolved.json")


def _load_json(path: str) -> Any:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _write_json(path: str, payload: Any) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)


def _find_gameassembly(pm: StarProcess) -> Any:
    return next(m for m in pm.list_modules() if m.name.lower() == "gameassembly.dll")


def _module_label(pm: StarProcess, addr: int) -> str:
    try:
        for mod in pm.list_modules():
            if mod.base <= addr < mod.base + mod.size:
                return f"{mod.name}+0x{addr - mod.base:X}"
    except Exception:
        pass
    return "heap"


def _build_klass_index(pm: StarProcess, ga_base: int, si: ScriptIndex) -> Tuple[Dict[int, str], Dict[str, Dict[str, Any]]]:
    klass_to_name: Dict[int, str] = {}
    name_to_runtime: Dict[str, Dict[str, Any]] = {}
    for name, rva in si.klass_rva.items():
        ptr_addr = ga_base + int(rva)
        klass = pm.read_u64(ptr_addr)
        if not klass:
            continue
        klass_to_name[int(klass)] = name
        name_to_runtime[name] = {
            "typeinfo_rva": f"0x{int(rva):X}",
            "typeinfo_ptr_addr": f"0x{ptr_addr:X}",
            "runtime_klass": f"0x{int(klass):X}",
        }
    return klass_to_name, name_to_runtime


def _first_runtime_class(name_to_runtime: Dict[str, Dict[str, Any]], *names: str) -> Optional[Dict[str, Any]]:
    for name in names:
        row = name_to_runtime.get(name)
        if row:
            return row | {"script_name": name}
    lowered = {name.lower(): row | {"script_name": name} for name, row in name_to_runtime.items()}
    for name in names:
        row = lowered.get(name.lower())
        if row:
            return row
    return None


def _safe_int(raw: Any) -> Optional[int]:
    if raw is None:
        return None
    try:
        return int(str(raw), 16) if isinstance(raw, str) else int(raw)
    except (TypeError, ValueError):
        return None


def _nearby_managed_headers(pm: StarProcess, addr: int, klass_to_name: Dict[int, str],
                            *, before: int, after: int) -> List[Dict[str, Any]]:
    out: List[Dict[str, Any]] = []
    start = addr - before
    end = addr + after
    for cand in range(start & ~0x7, end + 1, 8):
        klass = pm.read_u64(cand)
        if not klass:
            continue
        name = klass_to_name.get(int(klass))
        if not name:
            continue
        out.append({
            "obj": f"0x{cand:X}",
            "klass": f"0x{int(klass):X}",
            "klass_name": name,
            "delta_from_anchor": cand - addr,
        })
    return out


def _scan_pointers_to(pm: StarProcess, target: int, *, max_region: int, max_hits: int) -> Tuple[List[int], int, float]:
    hits: List[int] = []
    scanned = 0
    t0 = time.time()
    target_u64 = int(target) & 0xFFFFFFFFFFFFFFFF
    for region in pm.iter_regions(only_readable=True, only_private=True):
        if region.size > max_region:
            continue
        off = 0
        chunk = 16 * 1024 * 1024
        while off < region.size:
            n = min(chunk, region.size - off)
            blob = pm.read_bytes(region.base + off, n)
            if blob is None:
                break
            scanned += len(blob)
            remaining = max_hits - len(hits)
            if remaining <= 0:
                return hits, scanned, time.time() - t0
            for hit_off in _cy.find_aligned_u64(blob, target_u64, max_hits=remaining):
                hits.append(region.base + off + int(hit_off))
            if len(hits) >= max_hits:
                return hits, scanned, time.time() - t0
            off += n
    return hits, scanned, time.time() - t0


def _scan_pointers_to_set(pm: StarProcess, targets: Iterable[int], *, max_region: int,
                          max_hits: int) -> Tuple[Dict[int, List[int]], int, float]:
    target_values = sorted({int(t) & 0xFFFFFFFFFFFFFFFF for t in targets})
    hits: Dict[int, List[int]] = defaultdict(list)
    if not target_values:
        return {}, 0, 0.0
    scanned = 0
    t0 = time.time()
    total_hits = 0
    for region in pm.iter_regions(only_readable=True, only_private=True):
        if region.size > max_region:
            continue
        off = 0
        chunk = 16 * 1024 * 1024
        while off < region.size:
            n = min(chunk, region.size - off)
            blob = pm.read_bytes(region.base + off, n)
            if blob is None:
                break
            scanned += len(blob)
            remaining = max_hits - total_hits
            if remaining <= 0:
                return dict(hits), scanned, time.time() - t0
            for hit_off, matched in _cy.find_aligned_u64_in_set(blob, target_values, max_hits=remaining):
                hits[int(matched)].append(region.base + off + int(hit_off))
                total_hits += 1
            if total_hits >= max_hits:
                return dict(hits), scanned, time.time() - t0
            off += n
    return dict(hits), scanned, time.time() - t0


def _resolve_ptr_owner(pm: StarProcess, ptr_location: int, klass_to_name: Dict[int, str],
                       *, max_back: int = 0x300) -> Optional[Dict[str, Any]]:
    for off in range(0, max_back + 1, 8):
        obj = ptr_location - off
        klass = pm.read_u64(obj)
        if not klass:
            continue
        name = klass_to_name.get(int(klass))
        if not name:
            continue
        return {
            "obj": f"0x{obj:X}",
            "field_offset_guess": f"0x{off:X}",
            "klass": f"0x{int(klass):X}",
            "klass_name": name,
        }
    return None


def _array_guess_from_element_base(pm: StarProcess, element_base: int,
                                   klass_to_name: Dict[int, str]) -> List[Dict[str, Any]]:
    out: List[Dict[str, Any]] = []
    for prefix in range(0x20, 0x100, 8):
        obj = element_base - prefix
        length = pm.read_i32(obj + 0x18)
        if length is None or not (1 <= length <= 100000):
            continue
        klass = pm.read_u64(obj)
        name = klass_to_name.get(int(klass or 0))
        if not name:
            continue
        out.append({
            "array_obj": f"0x{obj:X}",
            "array_klass": f"0x{int(klass):X}",
            "array_klass_name": name,
            "length": int(length),
            "element_base": f"0x{element_base:X}",
            "element_base_delta": prefix,
        })
    return out


def resolve_owner(pointer_tables_input: str, script_json: str, output: str,
                  *, scan_parent_refs: bool, table_indices: Set[int], max_region: int, max_hits: int) -> Dict[str, Any]:
    pointer_tables = _load_json(pointer_tables_input)
    si = ScriptIndex.load(script_json)
    pm = StarProcess()
    try:
        ga = _find_gameassembly(pm)
        klass_to_name, name_to_runtime = _build_klass_index(pm, ga.base, si)
        string_runtime = _first_runtime_class(name_to_runtime, "System.String", "string")
        string_array_runtime = _first_runtime_class(name_to_runtime, "System.String[]", "string[]")
        prepared_tables: List[Dict[str, Any]] = []
        for table_index, table in enumerate(pointer_tables.get("tables") or []):
            if table_indices and table_index not in table_indices:
                continue
            element_base = _safe_int(table.get("element_base"))
            if element_base is None:
                continue
            seed = _safe_int(table.get("seed_ptr_location"))
            managed_headers = _nearby_managed_headers(
                pm, element_base, klass_to_name, before=0x400, after=0x80
            )
            array_guesses = _array_guess_from_element_base(pm, element_base, klass_to_name)
            scan_targets: List[Tuple[str, int]] = []
            scan_targets.append(("element_base", element_base))
            if seed is not None:
                scan_targets.append(("seed_ptr_location", seed))
            for guess in array_guesses:
                arr_obj = _safe_int(guess.get("array_obj"))
                if arr_obj is not None:
                    scan_targets.append(("array_obj", arr_obj))
            prepared_tables.append({
                "table_index": table_index,
                "seed_ptr_location": table.get("seed_ptr_location"),
                "element_base": table.get("element_base"),
                "row_count": table.get("row_count"),
                "managed_header_candidates_near_element_base": managed_headers,
                "managed_array_guesses": array_guesses,
                "_scan_targets": scan_targets,
                "owner_status": "managed_array_or_header_found" if (managed_headers or array_guesses) else "no_managed_header_near_base",
            })
        parent_hits: Dict[int, List[int]] = {}
        parent_scanned = 0
        parent_seconds = 0.0
        if scan_parent_refs:
            all_targets = [target for table in prepared_tables for _, target in table.get("_scan_targets", [])]
            parent_hits, parent_scanned, parent_seconds = _scan_pointers_to_set(
                pm, all_targets, max_region=max_region, max_hits=max_hits
            )
        output_tables: List[Dict[str, Any]] = []
        for table in prepared_tables:
            parent_scans: List[Dict[str, Any]] = []
            if scan_parent_refs:
                seen_targets = set()
                for label, target in table.get("_scan_targets", []):
                    if target in seen_targets:
                        continue
                    seen_targets.add(target)
                    hits = parent_hits.get(int(target) & 0xFFFFFFFFFFFFFFFF, [])
                    owners = []
                    for loc in hits[:128]:
                        owners.append({
                            "ptr_location": f"0x{loc:X}",
                            "where": _module_label(pm, loc),
                            "owner_guess": _resolve_ptr_owner(pm, loc, klass_to_name),
                        })
                    parent_scans.append({
                        "label": label,
                        "target": f"0x{target:X}",
                        "ptr_location_count": len(hits),
                        "scanned_bytes": parent_scanned,
                        "seconds": round(parent_seconds, 3),
                        "owner_class_counts": dict(Counter(
                            (o.get("owner_guess") or {}).get("klass_name") or "unknown"
                            for o in owners
                        )),
                        "ptr_locations": owners,
                    })
            table = dict(table)
            table.pop("_scan_targets", None)
            table["parent_reference_scans"] = parent_scans
            output_tables.append(table)
        result = {
            "source": "live_name_owner_resolve",
            "pointer_tables_input": pointer_tables_input.replace("\\", "/"),
            "script_json": script_json.replace("\\", "/"),
            "process": pm.name,
            "pid": pm.pid,
            "gameassembly": {
                "base": f"0x{ga.base:X}",
                "size": f"0x{ga.size:X}",
            },
            "summary": {
                "klass_index_count": len(klass_to_name),
                "table_count": len(output_tables),
                "requested_table_indices": sorted(table_indices) if table_indices else "all",
                "tables_with_managed_header_or_array": sum(
                    1 for t in output_tables
                    if t.get("managed_header_candidates_near_element_base") or t.get("managed_array_guesses")
                ),
                "parent_ref_scan_enabled": scan_parent_refs,
            },
            "verified_runtime_classes": {
                "System.String": string_runtime,
                "System.String[]": string_array_runtime,
            },
            "note": (
                "Runtime owner/class evidence resolved using current script.json. "
                "Heap addresses remain volatile; persistent anchors require a module RVA/static root or reproducible owner chain."
            ),
            "tables": output_tables,
        }
        _write_json(output, result)
        return result
    finally:
        pm.close()


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Resolve owner/class evidence for live name pointer tables")
    parser.add_argument("--pointer-tables-input", default=_DEFAULT_POINTER_TABLES)
    parser.add_argument("--script-json", default=_DEFAULT_SCRIPT_JSON)
    parser.add_argument("--output", default=_DEFAULT_OUTPUT)
    parser.add_argument("--scan-parent-refs", action="store_true",
                        help="Scan private heap for pointers to table/array objects and resolve parent owners")
    parser.add_argument("--table-index", action="append", type=int, default=[],
                        help="Only process a specific decoded table index. Repeat for multiple indices.")
    parser.add_argument("--max-region", type=lambda s: int(s, 0), default=512 * 1024 * 1024)
    parser.add_argument("--max-hits", type=int, default=20000)
    args = parser.parse_args(argv)
    result = resolve_owner(
        args.pointer_tables_input,
        args.script_json,
        args.output,
        scan_parent_refs=args.scan_parent_refs,
        table_indices=set(args.table_index),
        max_region=args.max_region,
        max_hits=args.max_hits,
    )
    print(json.dumps({"output": args.output, "summary": result.get("summary"), "verified_runtime_classes": result.get("verified_runtime_classes")}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
