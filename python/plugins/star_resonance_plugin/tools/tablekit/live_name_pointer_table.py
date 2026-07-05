# Expand live Il2CppString pointer tables and attach known TCP/config IDs.
#
# This is a read-only runtime evidence tool.  It starts from pointer locations
# found by ``live_name_owner_refs.py`` and walks adjacent qword slots while they
# look like ``System.String`` object pointers.  Heap addresses in the output are
# volatile and must not be treated as stable anchors until an owner/container
# class root is resolved.

from __future__ import annotations

import argparse
import json
import os
from typing import Any, Dict, Iterable, List, Optional, Tuple

from plugins.star_resonance_plugin.mem.process import StarProcess


_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO = os.path.dirname(os.path.dirname(_HERE))
_NAME_TABLES = os.path.join(_SAO, "assets", "name_tables")
_STRING_LAYOUT = {
    "klass": 0x00,
    "length": 0x10,
    "chars": 0x14,
}


def _load_json(path: str) -> Any:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _safe_int(raw: Any) -> Optional[int]:
    if raw is None:
        return None
    try:
        return int(str(raw), 16) if isinstance(raw, str) else int(raw)
    except (TypeError, ValueError):
        return None


def _read_cstr(pm: StarProcess, addr: int, max_len: int = 256) -> Optional[str]:
    if not addr:
        return None
    buf = pm.read_bytes(addr, max_len)
    if not buf:
        return None
    end = buf.find(b"\x00")
    if end < 0:
        end = len(buf)
    text = buf[:end].decode("utf-8", errors="replace").strip()
    return text or None


def _klass_name(pm: StarProcess, klass: Optional[int]) -> Optional[str]:
    if not klass:
        return None
    name = _read_cstr(pm, pm.read_u64(klass + 0x10) or 0)
    namespace = _read_cstr(pm, pm.read_u64(klass + 0x18) or 0) or ""
    if not name:
        return None
    return f"{namespace}.{name}" if namespace else name


def _read_il2cpp_string(pm: StarProcess, obj: int, string_klass: int) -> Optional[Dict[str, Any]]:
    if not obj:
        return None
    klass = pm.read_u64(obj + _STRING_LAYOUT["klass"])
    if klass != string_klass:
        return None
    length = pm.read_i32(obj + _STRING_LAYOUT["length"])
    if length is None or not (1 <= length <= 128):
        return None
    text = pm.read_utf16(obj + _STRING_LAYOUT["chars"], length)
    if not text:
        return None
    return {
        "obj": f"0x{obj:X}",
        "chars": f"0x{obj + _STRING_LAYOUT['chars']:X}",
        "klass": f"0x{klass:X}",
        "text": text,
        "length": int(length),
    }


def _build_match_index(crossref: Dict[str, Any]) -> Dict[str, List[Dict[str, Any]]]:
    index: Dict[str, List[Dict[str, Any]]] = {}
    for row in crossref.get("unique_candidates") or []:
        text = str(row.get("text") or "")
        matches = row.get("matches") or []
        if text and matches:
            index.setdefault(text, []).extend(matches)
    return index


def _seed_locations(owner_refs: Dict[str, Any]) -> List[int]:
    seeds: List[int] = []
    for row in owner_refs.get("targets") or []:
        for loc in (row.get("reference_scan") or {}).get("ptr_locations") or []:
            raw = loc.get("addr") if isinstance(loc, dict) else loc
            value = _safe_int(raw)
            if value is not None:
                seeds.append(value)
    return sorted(set(seeds))


def _expand_from_seed(pm: StarProcess, seed: int, string_klass: int,
                      *, max_slots: int, max_gap_slots: int) -> Tuple[int, List[Dict[str, Any]], bool]:
    def probe(slot: int) -> Optional[Dict[str, Any]]:
        ptr = pm.read_u64(slot)
        if not ptr:
            return None
        return _read_il2cpp_string(pm, ptr, string_klass)

    # Walk left until too many consecutive non-string qwords are seen.
    left = seed
    gap = 0
    scanned = 0
    while scanned < max_slots:
        prev_slot = left - 8
        if probe(prev_slot):
            left = prev_slot
            gap = 0
        else:
            gap += 1
            if gap > max_gap_slots:
                break
            left = prev_slot
        scanned += 1
    # Trim leading gap slots by advancing to first valid string.
    while left <= seed and not probe(left):
        left += 8

    rows: List[Dict[str, Any]] = []
    slot = left
    gap = 0
    truncated = False
    while len(rows) < max_slots:
        info = probe(slot)
        if info:
            gap = 0
            rows.append({
                "index": len(rows),
                "slot_addr": f"0x{slot:X}",
                "value": info["obj"],
                "string": info,
            })
        else:
            gap += 1
            if gap > max_gap_slots:
                break
            rows.append({
                "index": len(rows),
                "slot_addr": f"0x{slot:X}",
                "value": f"0x{(pm.read_u64(slot) or 0):X}",
                "string": None,
            })
        slot += 8
    else:
        truncated = True
    # Trim trailing non-string rows.
    while rows and rows[-1].get("string") is None:
        rows.pop()
    for idx, row in enumerate(rows):
        row["index"] = idx
    return left, rows, truncated


def _array_guess(pm: StarProcess, element_base: int) -> List[Dict[str, Any]]:
    guesses: List[Dict[str, Any]] = []
    for prefix in range(0x20, 0x100, 8):
        arr_obj = element_base - prefix
        length = pm.read_i32(arr_obj + 0x18)
        if length is None or not (1 <= length <= 100000):
            continue
        klass = pm.read_u64(arr_obj)
        guesses.append({
            "array_obj": f"0x{arr_obj:X}",
            "array_klass": f"0x{klass:X}" if klass else None,
            "array_klass_name": _klass_name(pm, klass),
            "length": int(length),
            "element_base": f"0x{element_base:X}",
        })
    return guesses


def _merge_runs(runs: Iterable[Dict[str, Any]]) -> List[Dict[str, Any]]:
    by_base: Dict[str, Dict[str, Any]] = {}
    for run in runs:
        key = run["element_base"]
        existing = by_base.get(key)
        if existing is None or len(run.get("rows") or []) > len(existing.get("rows") or []):
            by_base[key] = run
    return sorted(by_base.values(), key=lambda r: int(r["element_base"], 16))


def decode_pointer_tables(owner_refs_input: str, crossref_input: str, output: str,
                          *, string_klass: Optional[int], max_slots: int,
                          max_gap_slots: int) -> Dict[str, Any]:
    owner_refs = _load_json(owner_refs_input)
    crossref = _load_json(crossref_input)
    if string_klass is None:
        string_klass = _safe_int(((crossref.get("known_runtime_klass") or {}).get("System.String") or {}).get("runtime_klass"))
    if string_klass is None:
        for row in owner_refs.get("targets") or []:
            string_klass = _safe_int((row.get("runtime") or {}).get("klass"))
            if string_klass is not None:
                break
    if string_klass is None:
        raise RuntimeError("Unable to determine System.String runtime klass; pass --string-klass")

    match_index = _build_match_index(crossref)
    seeds = _seed_locations(owner_refs)
    pm = StarProcess()
    try:
        runs: List[Dict[str, Any]] = []
        for seed in seeds:
            base, rows, truncated = _expand_from_seed(pm, seed, string_klass, max_slots=max_slots, max_gap_slots=max_gap_slots)
            for row in rows:
                info = row.get("string") or {}
                text = info.get("text")
                row["matches"] = match_index.get(text, []) if text else []
            runs.append({
                "seed_ptr_location": f"0x{seed:X}",
                "element_base": f"0x{base:X}",
                "row_count": len([r for r in rows if r.get("string")]),
                "max_slots": max_slots,
                "truncated": truncated,
                "rows": rows,
                "array_guesses": _array_guess(pm, base),
            })
    finally:
        pm.close()

    tables = _merge_runs(runs)
    result = {
        "source": "live_name_pointer_table",
        "owner_refs_input": owner_refs_input.replace("\\", "/"),
        "crossref_input": crossref_input.replace("\\", "/"),
        "process": owner_refs.get("process"),
        "pid": owner_refs.get("pid"),
        "note": "Read-only runtime pointer-table expansion. Heap addresses and indices are volatile evidence until owner/static roots are resolved.",
        "known_runtime_klass": {
            "System.String": {
                "runtime_klass": f"0x{string_klass:X}",
            },
        },
        "summary": {
            "seed_count": len(seeds),
            "table_count": len(tables),
            "row_count": sum(t.get("row_count", 0) for t in tables),
            "matched_row_count": sum(1 for t in tables for r in t.get("rows", []) if r.get("matches")),
            "truncated_table_count": sum(1 for t in tables if t.get("truncated")),
        },
        "tables": tables,
    }
    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    with open(output, "w", encoding="utf-8") as f:
        json.dump(result, f, ensure_ascii=False, indent=2)
    return result


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Expand live Il2CppString pointer tables")
    parser.add_argument("--owner-refs-input", default=os.path.join(_NAME_TABLES, "live_probe_owner_refs.json"))
    parser.add_argument("--crossref-input", default=os.path.join(_NAME_TABLES, "live_probe_id_candidates.json"))
    parser.add_argument("--output", default=os.path.join(_NAME_TABLES, "live_probe_pointer_tables.json"))
    parser.add_argument("--string-klass", type=lambda s: int(s, 0), default=None)
    parser.add_argument("--max-slots", type=int, default=512)
    parser.add_argument("--max-gap-slots", type=int, default=0)
    args = parser.parse_args(argv)
    result = decode_pointer_tables(
        args.owner_refs_input,
        args.crossref_input,
        args.output,
        string_klass=args.string_klass,
        max_slots=args.max_slots,
        max_gap_slots=args.max_gap_slots,
    )
    print(json.dumps({"output": args.output, "summary": result.get("summary")}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
