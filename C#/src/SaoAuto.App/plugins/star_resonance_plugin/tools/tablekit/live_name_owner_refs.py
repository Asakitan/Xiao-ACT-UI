"""Scan read-only pointer references to live Il2CppString objects.

The output intentionally records pointer locations without pretending to know
owner class names when no current IL2CPP dump/bundle is available.  It is a
safe bridge from runtime string candidates to later table/container owner
analysis.
"""

from __future__ import annotations

import argparse
import json
import os
import time
from collections import defaultdict
from typing import Any, Dict, Iterable, List, Optional

from mem_probe import cy_memscan as _cy
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


def _module_label(pm: StarProcess, addr: int) -> str:
    try:
        for mod in pm.list_modules():
            if mod.base <= addr < mod.base + mod.size:
                return f"{mod.name}+0x{addr - mod.base:X}"
    except Exception:
        pass
    return "heap"


def _read_cstr(pm: StarProcess, addr: int, max_len: int = 256) -> Optional[str]:
    if not addr:
        return None
    buf = pm.read_bytes(addr, max_len)
    if not buf:
        return None
    end = buf.find(b"\x00")
    if end < 0:
        end = len(buf)
    try:
        text = buf[:end].decode("utf-8", errors="replace").strip()
    except Exception:
        return None
    return text or None


def _klass_name(pm: StarProcess, klass: Optional[int]) -> Optional[str]:
    if not klass:
        return None
    name_ptr = pm.read_u64(klass + 0x10)
    ns_ptr = pm.read_u64(klass + 0x18)
    name = _read_cstr(pm, name_ptr or 0)
    namespace = _read_cstr(pm, ns_ptr or 0) if ns_ptr else ""
    if not name:
        return None
    return f"{namespace}.{name}" if namespace else name


def _read_il2cpp_string(pm: StarProcess, obj: int, string_klass: Optional[int]) -> Optional[str]:
    if not obj:
        return None
    if string_klass is not None:
        klass = pm.read_u64(obj + _STRING_LAYOUT["klass"])
        if klass != string_klass:
            return None
    length = pm.read_i32(obj + _STRING_LAYOUT["length"])
    if length is None or not (1 <= length <= 96):
        return None
    return pm.read_utf16(obj + _STRING_LAYOUT["chars"], length)


def _pointer_context(pm: StarProcess, ptr_location: int, string_klass: Optional[int],
                     *, qwords_before: int = 12, qwords_after: int = 24) -> Dict[str, Any]:
    start = ptr_location - qwords_before * 8
    count = qwords_before + qwords_after + 1
    blob = pm.read_bytes(start, count * 8) or b""
    rows: List[Dict[str, Any]] = []
    for idx in range(0, len(blob), 8):
        addr = start + idx
        value = int.from_bytes(blob[idx:idx + 8], "little", signed=False)
        row: Dict[str, Any] = {
            "slot_addr": f"0x{addr:X}",
            "slot_delta": addr - ptr_location,
            "value": f"0x{value:X}",
        }
        text = _read_il2cpp_string(pm, value, string_klass)
        if text:
            row["as_string"] = text
        rows.append(row)
    array_guesses: List[Dict[str, Any]] = []
    for elem_base_delta in range(0, min(qwords_before * 8, 0x80) + 1, 8):
        arr_obj = ptr_location - elem_base_delta - 0x20
        length = pm.read_i32(arr_obj + 0x18)
        klass = pm.read_u64(arr_obj)
        if length is None or not (1 <= length <= 4096):
            continue
        elem_index = elem_base_delta // 8
        if elem_index >= length:
            continue
        array_guesses.append({
            "array_obj": f"0x{arr_obj:X}",
            "array_klass": f"0x{klass:X}" if klass else None,
            "array_klass_name": _klass_name(pm, klass),
            "length": int(length),
            "element_base": f"0x{arr_obj + 0x20:X}",
            "element_index_at_ptr_location": elem_index,
        })
    return {
        "ptr_location": f"0x{ptr_location:X}",
        "qwords": rows,
        "array_guesses": array_guesses,
    }


def _scan_pointers_to(pm: StarProcess, target: int, *, max_region: int,
                      max_hits: int, only_private: bool = True) -> Dict[str, Any]:
    hits: List[int] = []
    target_u64 = int(target) & 0xFFFFFFFFFFFFFFFF
    scanned = 0
    t0 = time.time()
    for region in pm.iter_regions(only_readable=True, only_private=only_private):
        if region.size > max_region:
            continue
        chunk = 16 * 1024 * 1024
        off = 0
        while off < region.size:
            n = min(chunk, region.size - off)
            blob = pm.read_bytes(region.base + off, n)
            if blob is None:
                break
            scanned += len(blob)
            remaining = max_hits - len(hits)
            if remaining <= 0:
                return {"hits": hits, "scanned_bytes": scanned, "seconds": time.time() - t0}
            for idx in _cy.find_aligned_u64(blob, target_u64, max_hits=remaining):
                hits.append(region.base + off + idx)
            if len(hits) >= max_hits:
                return {"hits": hits, "scanned_bytes": scanned, "seconds": time.time() - t0}
            off += n
    return {"hits": hits, "scanned_bytes": scanned, "seconds": time.time() - t0}


def _iter_unique_targets(crossref: Dict[str, Any], names: Optional[Iterable[str]]) -> Iterable[Dict[str, Any]]:
    wanted = set(names or [])
    for row in crossref.get("unique_candidates") or []:
        text = str(row.get("text") or "")
        if wanted and text not in wanted:
            continue
        runtime = row.get("runtime") or {}
        obj = runtime.get("obj")
        if not obj:
            continue
        yield {"text": text, "runtime": runtime, "classification": row.get("classification"), "matches": row.get("matches") or []}


def scan_owner_refs(crossref_input: str, output: str, *, names: Optional[List[str]],
                    target_objs: Optional[List[int]], max_region: int, max_hits: int,
                    only_private: bool = True) -> Dict[str, Any]:
    crossref = _load_json(crossref_input)
    targets = [] if target_objs and not names else list(_iter_unique_targets(crossref, names))
    for obj in target_objs or []:
        targets.append({
            "text": f"target_obj_0x{obj:X}",
            "runtime": {"obj": f"0x{obj:X}", "anchor_status": "manual_target_obj"},
            "classification": "manual_target",
            "matches": [],
        })
    pm = StarProcess()
    try:
        rows: List[Dict[str, Any]] = []
        for target in targets:
            obj = int(str(target["runtime"]["obj"]), 16)
            print(f"[scan] {target['text']} obj=0x{obj:X}", flush=True)
            refs = _scan_pointers_to(pm, obj, max_region=max_region, max_hits=max_hits,
                                     only_private=only_private)
            ptr_locations = refs["hits"]
            print(f"[scan] {target['text']} refs={len(ptr_locations)} scanned={refs['scanned_bytes']/1e9:.2f}GB seconds={refs['seconds']:.1f}", flush=True)
            rows.append({
                "text": target["text"],
                "runtime": target["runtime"],
                "classification": target.get("classification"),
                "match_ids": [
                    {
                        "id_space": match.get("id_space"),
                        "id": match.get("id"),
                        "source_kind": match.get("source_kind"),
                        "confidence": match.get("confidence"),
                        "match": match.get("match"),
                        "alias_of": match.get("alias_of"),
                    }
                    for match in (target.get("matches") or [])
                ],
                "reference_scan": {
                    "ptr_location_count": len(ptr_locations),
                    "scanned_bytes": refs["scanned_bytes"],
                    "seconds": round(refs["seconds"], 3),
                    "ptr_locations": [
                        {"addr": f"0x{addr:X}", "where": _module_label(pm, addr)}
                        for addr in ptr_locations
                    ],
                    "ptr_contexts": [
                        _pointer_context(pm, addr, _safe_runtime_klass(target.get("runtime")))
                        for addr in ptr_locations[:8]
                    ],
                    "owner_status": "ptr_locations_only_no_current_script_json",
                    "scan_scope": "private_readable" if only_private else "all_readable_commit_under_max_region",
                },
            })
        summary_by_text = defaultdict(int)
        for row in rows:
            summary_by_text[row["text"]] += row["reference_scan"]["ptr_location_count"]
        result = {
            "source": "live_name_owner_refs",
            "input": crossref_input.replace("\\", "/"),
            "process": crossref.get("process"),
            "pid": crossref.get("pid"),
            "note": (
                "Read-only pointer reference scan for live Il2CppString objects. "
                "Pointer locations are runtime-only evidence; owner class names require "
                "a current script.json/bundle for this game build."
            ),
            "summary": {
                "target_count": len(rows),
                "total_ptr_locations": sum(summary_by_text.values()),
                "ptr_locations_by_text": dict(summary_by_text),
            },
            "targets": rows,
        }
        os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
        with open(output, "w", encoding="utf-8") as f:
            json.dump(result, f, ensure_ascii=False, indent=2)
        return result
    finally:
        pm.close()


def augment_existing(output: str) -> Dict[str, Any]:
    result = _load_json(output)
    pm = StarProcess()
    try:
        for row in result.get("targets") or []:
            runtime = row.get("runtime") or {}
            string_klass = _safe_runtime_klass(runtime)
            scan = row.get("reference_scan") or {}
            contexts = []
            for loc in scan.get("ptr_locations") or []:
                raw = loc.get("addr") if isinstance(loc, dict) else loc
                if not raw:
                    continue
                try:
                    addr = int(str(raw), 16)
                except ValueError:
                    continue
                contexts.append(_pointer_context(pm, addr, string_klass))
            scan["ptr_contexts"] = contexts
            row["reference_scan"] = scan
        with open(output, "w", encoding="utf-8") as f:
            json.dump(result, f, ensure_ascii=False, indent=2)
        return result
    finally:
        pm.close()


def _safe_runtime_klass(runtime: Any) -> Optional[int]:
    try:
        raw = (runtime or {}).get("klass")
        return int(str(raw), 16) if raw else None
    except (TypeError, ValueError):
        return None


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Scan pointer references to live runtime name strings")
    parser.add_argument("--crossref-input", default=os.path.join(_NAME_TABLES, "live_probe_id_candidates.json"))
    parser.add_argument("--output", default=os.path.join(_NAME_TABLES, "live_probe_owner_refs.json"))
    parser.add_argument("--name", action="append", dest="names", help="Only scan this text; repeatable")
    parser.add_argument("--target-obj", action="append", type=lambda s: int(s, 0), default=[],
                        help="Manually scan references to this runtime object pointer; repeatable")
    parser.add_argument("--max-region", type=lambda s: int(s, 0), default=512 * 1024 * 1024)
    parser.add_argument("--max-hits", type=int, default=20000)
    parser.add_argument("--include-non-private", action="store_true",
                        help="Also scan readable image/mapped regions; useful for static roots in modules")
    parser.add_argument("--augment-existing", action="store_true",
                        help="Only enrich the existing output file with local pointer contexts; no full memory scan")
    args = parser.parse_args(argv)
    if args.augment_existing:
        result = augment_existing(args.output)
        print(json.dumps({"output": args.output, "summary": result.get("summary")}, ensure_ascii=False, indent=2))
        return 0
    result = scan_owner_refs(
        args.crossref_input,
        args.output,
        names=args.names,
        target_objs=args.target_obj,
        max_region=args.max_region,
        max_hits=args.max_hits,
        only_private=not args.include_non_private,
    )
    print(json.dumps({"output": args.output, "summary": result.get("summary")}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
