"""Probe runtime string-pool owners for the decoded live name table.

Read-only IL2CPP probe.  It uses the current Il2CppDumper script/dump index to
find likely string-pool classes, scans their live instances, and inspects
string[] fields/dictionaries for ACT-relevant Chinese names.
"""

from __future__ import annotations

import argparse
import json
import os
from collections import Counter
from typing import Any, Dict, Iterable, List, Optional, Tuple

from plugins.star_resonance_plugin.mem.il2cpp.dump_cs_parser import DumpCsIndex
from plugins.star_resonance_plugin.mem.il2cpp.script_parser import ScriptIndex
from mem_probe.process import StarProcess
from mem_probe import cy_memscan as _cy

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO = os.path.dirname(os.path.dirname(_HERE))
_NAME_TABLES = os.path.join(_SAO, "assets", "name_tables")
_IL2CPP = os.path.join(_SAO, "mem_probe", "il2cpp")
_DEFAULT_DUMP_ID = "fdc7111b"
_DEFAULT_SCRIPT_JSON = os.path.join(_IL2CPP, "out", _DEFAULT_DUMP_ID, "script.json")
_DEFAULT_DUMP_CS_INDEX = os.path.join(_IL2CPP, "out", _DEFAULT_DUMP_ID, "dump_cs_index.json")
_DEFAULT_POINTER_TABLES = os.path.join(_NAME_TABLES, "live_probe_pointer_tables.json")
_DEFAULT_OUTPUT = os.path.join(_NAME_TABLES, "live_string_pool_probe.json")

_STRING_POOL_CLASSES = [
    "Panda.Module.StringPoolManager",
    "Panda.Module.StringPoolRuntimeImpl",
    "Panda.Utility.Localization.LocalizationStringPool",
    "Panda.Utility.Localization.LocalizationMgr",
    "Panda.MLStringPoolBridge",
    "Bokura.Table.MLStringArray",
    "Bokura.Table.MLStringTable",
]
_TARGET_TEXTS = ["防护回复流", "光盾回复流", "神圣壁垒", "圣环守护"]


def _load_json(path: str) -> Any:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _write_json(path: str, payload: Any) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)


def _safe_int(raw: Any) -> Optional[int]:
    if raw is None:
        return None
    try:
        return int(str(raw), 16) if isinstance(raw, str) else int(raw)
    except (TypeError, ValueError):
        return None


def _find_gameassembly(pm: StarProcess) -> Any:
    return next(m for m in pm.list_modules() if m.name.lower() == "gameassembly.dll")


def _read_string(pm: StarProcess, obj: int, string_klass: Optional[int] = None,
                 max_len: int = 512) -> Optional[str]:
    if not obj:
        return None
    klass = pm.read_u64(obj)
    if string_klass and klass != string_klass:
        return None
    length = pm.read_i32(obj + 0x10)
    if length is None or not (0 <= length <= max_len):
        return None
    return pm.read_utf16(obj + 0x14, length) or ""


def _build_klass_index(pm: StarProcess, ga_base: int, si: ScriptIndex) -> Tuple[Dict[str, Dict[str, Any]], Dict[int, str]]:
    by_name: Dict[str, Dict[str, Any]] = {}
    by_klass: Dict[int, str] = {}
    for name, rva in si.klass_rva.items():
        ptr_addr = ga_base + int(rva)
        klass = pm.read_u64(ptr_addr)
        if not klass:
            continue
        row = {
            "script_name": name,
            "typeinfo_rva": f"0x{int(rva):X}",
            "typeinfo_ptr_addr": f"0x{ptr_addr:X}",
            "runtime_klass": f"0x{int(klass):X}",
        }
        by_name[name] = row
        by_klass[int(klass)] = name
    return by_name, by_klass


def _runtime_class(by_name: Dict[str, Dict[str, Any]], *names: str) -> Optional[Dict[str, Any]]:
    lowered = {name.lower(): row for name, row in by_name.items()}
    for name in names:
        row = by_name.get(name) or lowered.get(name.lower())
        if row:
            return row
    return None


def _find_instances(pm: StarProcess, klass: int, *, max_region: int, max_hits: int) -> Tuple[List[int], int]:
    hits: List[int] = []
    scanned = 0
    needle = int(klass) & 0xFFFFFFFFFFFFFFFF
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
                return hits, scanned
            for hit_off in _cy.find_aligned_u64(blob, needle, max_hits=remaining):
                hits.append(region.base + off + int(hit_off))
            if len(hits) >= max_hits:
                return hits, scanned
            off += n
    return hits, scanned


def _read_string_array(pm: StarProcess, arr_obj: int, string_klass: Optional[int], *, max_items: int = 20000) -> Optional[Dict[str, Any]]:
    if not arr_obj:
        return None
    length = pm.read_i32(arr_obj + 0x18)
    if length is None or not (0 <= length <= max_items):
        return None
    rows: List[Dict[str, Any]] = []
    matched_targets: List[Dict[str, Any]] = []
    sample_nonempty: List[Dict[str, Any]] = []
    for i in range(length):
        ptr = pm.read_u64(arr_obj + 0x20 + i * 8)
        if not ptr:
            continue
        text = _read_string(pm, ptr, string_klass)
        if text is None:
            continue
        row = {"index": i, "obj": f"0x{ptr:X}", "text": text}
        if len(sample_nonempty) < 20:
            sample_nonempty.append(row)
        if text in _TARGET_TEXTS:
            matched_targets.append(row)
    return {
        "array_obj": f"0x{arr_obj:X}",
        "length": int(length),
        "sample_nonempty": sample_nonempty,
        "matched_targets": matched_targets,
    }


def _extract_table_texts(pointer_tables: Dict[str, Any]) -> Dict[str, List[Dict[str, Any]]]:
    out: Dict[str, List[Dict[str, Any]]] = {}
    for table_index, table in enumerate(pointer_tables.get("tables") or []):
        for row in table.get("rows") or []:
            text = ((row.get("string") or {}).get("text") or "")
            if text in _TARGET_TEXTS:
                out.setdefault(text, []).append({
                    "table_index": table_index,
                    "row_index": row.get("index"),
                    "slot_addr": row.get("slot_addr"),
                    "obj": (row.get("string") or {}).get("obj"),
                })
    return out


def _field_rows(dci: DumpCsIndex, class_name: str) -> List[Dict[str, Any]]:
    c = dci.find_class(class_name)
    if not c:
        return []
    return [dict(f) for f in c.get("fields") or []]


def _probe_instances(pm: StarProcess, dci: DumpCsIndex, class_name: str, class_row: Dict[str, Any],
                     string_klass: Optional[int], *, max_region: int, max_hits: int) -> Dict[str, Any]:
    klass = _safe_int(class_row.get("runtime_klass"))
    fields = _field_rows(dci, class_name)
    string_array_fields = [f for f in fields if not f.get("is_static") and str(f.get("type")) == "string[]"]
    ptr_fields = [f for f in fields if not f.get("is_static") and str(f.get("type")) in {"IStringPool", "IStringPoolNew"}]
    instances, scanned = _find_instances(pm, klass or 0, max_region=max_region, max_hits=max_hits)
    instance_rows: List[Dict[str, Any]] = []
    for obj in instances[:64]:
        row: Dict[str, Any] = {"obj": f"0x{obj:X}", "string_array_fields": [], "ptr_fields": []}
        for f in string_array_fields:
            arr = pm.read_u64(obj + int(f.get("offset") or 0))
            arr_info = _read_string_array(pm, arr or 0, string_klass) if arr else None
            row["string_array_fields"].append({
                "field": f.get("name"),
                "offset": f"0x{int(f.get('offset') or 0):X}",
                "array": arr_info,
            })
        for f in ptr_fields:
            ptr = pm.read_u64(obj + int(f.get("offset") or 0))
            row["ptr_fields"].append({
                "field": f.get("name"),
                "offset": f"0x{int(f.get('offset') or 0):X}",
                "ptr": f"0x{ptr:X}" if ptr else None,
            })
        instance_rows.append(row)
    return {
        "class": class_name,
        "runtime": class_row,
        "fields_of_interest": {
            "string_arrays": string_array_fields,
            "pool_refs": ptr_fields,
        },
        "instance_count_scanned": len(instances),
        "instance_scan_bytes": scanned,
        "instances": instance_rows,
    }


def probe_string_pools(pointer_tables_input: str, script_json: str, dump_cs_index: str, output: str,
                       *, class_names: List[str], max_region: int, max_hits: int) -> Dict[str, Any]:
    pointer_tables = _load_json(pointer_tables_input)
    si = ScriptIndex.load(script_json)
    dci = DumpCsIndex.load_from_json(dump_cs_index)
    pm = StarProcess()
    try:
        ga = _find_gameassembly(pm)
        by_name, by_klass = _build_klass_index(pm, ga.base, si)
        string_runtime = _runtime_class(by_name, "System.String", "string")
        string_array_runtime = _runtime_class(by_name, "System.String[]", "string[]")
        string_klass = _safe_int((string_runtime or {}).get("runtime_klass"))
        classes: List[Dict[str, Any]] = []
        for name in class_names or _STRING_POOL_CLASSES:
            row = by_name.get(name)
            if not row:
                continue
            classes.append(_probe_instances(pm, dci, name, row, string_klass, max_region=max_region, max_hits=max_hits))
        result = {
            "source": "live_string_pool_probe",
            "pointer_tables_input": pointer_tables_input.replace("\\", "/"),
            "script_json": script_json.replace("\\", "/"),
            "dump_cs_index": dump_cs_index.replace("\\", "/"),
            "process": pm.name,
            "pid": pm.pid,
            "gameassembly": {"base": f"0x{ga.base:X}", "size": f"0x{ga.size:X}"},
            "verified_runtime_classes": {
                "System.String": string_runtime,
                "System.String[]": string_array_runtime,
            },
            "decoded_table_targets": _extract_table_texts(pointer_tables),
            "summary": {
                "class_count": len(classes),
                "target_texts": _TARGET_TEXTS,
                "matched_class_field_count": sum(
                    1
                    for c in classes
                    for inst in c.get("instances", [])
                    for f in inst.get("string_array_fields", [])
                    if ((f.get("array") or {}).get("matched_targets") or [])
                ),
            },
            "classes": classes,
        }
        _write_json(output, result)
        return result
    finally:
        pm.close()


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Probe runtime string-pool owner instances")
    parser.add_argument("--pointer-tables-input", default=_DEFAULT_POINTER_TABLES)
    parser.add_argument("--script-json", default=_DEFAULT_SCRIPT_JSON)
    parser.add_argument("--dump-cs-index", default=_DEFAULT_DUMP_CS_INDEX)
    parser.add_argument("--output", default=_DEFAULT_OUTPUT)
    parser.add_argument("--class-name", action="append", default=[],
                        help="Only scan this fully qualified class name. Repeat for multiple classes.")
    parser.add_argument("--max-region", type=lambda s: int(s, 0), default=512 * 1024 * 1024)
    parser.add_argument("--max-hits", type=int, default=128)
    args = parser.parse_args(argv)
    result = probe_string_pools(
        args.pointer_tables_input,
        args.script_json,
        args.dump_cs_index,
        args.output,
        class_names=args.class_name,
        max_region=args.max_region,
        max_hits=args.max_hits,
    )
    print(json.dumps({"output": args.output, "summary": result.get("summary"), "verified_runtime_classes": result.get("verified_runtime_classes")}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
