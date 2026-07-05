# Export the live StringPoolManager localization string table.
#
# Read-only IL2CPP probe.  It resolves the reproducible runtime chain:
#
# ZUtil.ZSingleton<StringPoolManager>.static_fields
# -> instance_
# -> Panda.Module.StringPoolManager.impl_
# -> Panda.Module.StringPoolRuntimeImpl.allLocalizationString_
#
# The full array export is still runtime evidence, but the owner chain gives a
# stable class/static anchor for the current build instead of a naked heap table.

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from typing import Any, Dict, List, Optional, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO = os.path.dirname(os.path.dirname(_HERE))
if _SAO not in sys.path:
    sys.path.insert(0, _SAO)

from plugins.star_resonance_plugin.mem.il2cpp.script_parser import ScriptIndex
from plugins.star_resonance_plugin.mem.process import StarProcess


_NAME_TABLES = os.path.join(_SAO, "assets", "name_tables")
_IL2CPP = os.path.join(_SAO, "mem_probe", "il2cpp")
_DEFAULT_DUMP_ID = "fdc7111b"
_DEFAULT_SCRIPT_JSON = os.path.join(_IL2CPP, "out", _DEFAULT_DUMP_ID, "script.json")
_DEFAULT_POINTER_TABLES = os.path.join(_NAME_TABLES, "live_probe_pointer_tables.json")
_DEFAULT_CROSSREF = os.path.join(_NAME_TABLES, "live_probe_id_candidates.json")
_DEFAULT_OUTPUT = os.path.join(_NAME_TABLES, "live_string_pool_all_localization.json")
_DEFAULT_SUMMARY_OUTPUT = os.path.join(_NAME_TABLES, "live_string_pool_static_anchor.json")

_CLASS_STATIC_FIELDS_OFF = 0xB8
_SINGLETON_INSTANCE_OFF = 0x00
_SINGLETON_LOCK_OFF = 0x08
_STRING_POOL_COMMON_OFF = 0x10
_STRING_POOL_IMPL_OFF = 0x18
_LOCALIZATION_FILE_NAME_OFF = 0x10
_LOCALIZATION_DICT_OFF = 0x18
_RUNTIME_IMPL_ALL_LOCALIZATION_OFF = 0x10
_ARRAY_LENGTH_OFF = 0x18
_ARRAY_ELEMS_OFF = 0x20

_TARGET_TEXTS = ["防护回复流", "光盾回复流", "神圣壁垒", "圣环守护"]


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


def _hex(value: Optional[int]) -> Optional[str]:
    return f"0x{int(value):X}" if value else None


def _safe_int(raw: Any) -> Optional[int]:
    if raw is None:
        return None
    try:
        return int(str(raw), 16) if isinstance(raw, str) else int(raw)
    except (TypeError, ValueError):
        return None


def _find_gameassembly(pm: StarProcess) -> Any:
    return next(m for m in pm.list_modules() if m.name.lower() == "gameassembly.dll")


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


def _runtime_row(by_name: Dict[str, Dict[str, Any]], *names: str) -> Optional[Dict[str, Any]]:
    lowered = {name.lower(): row for name, row in by_name.items()}
    for name in names:
        row = by_name.get(name) or lowered.get(name.lower())
        if row:
            return dict(row)
    return None


def _read_string(pm: StarProcess, obj: int, string_klass: int, *, max_len: int) -> Tuple[Optional[str], Optional[int], str]:
    if not obj:
        return None, None, "null_pointer"
    klass = pm.read_u64(obj)
    if klass != string_klass:
        return None, None, "klass_mismatch"
    length = pm.read_i32(obj + 0x10)
    if length is None:
        return None, None, "length_unreadable"
    if length < 0 or length > max_len:
        return None, int(length), "length_out_of_range"
    text = pm.read_utf16(obj + 0x14, length)
    if text is None:
        return None, int(length), "chars_unreadable"
    return text, int(length), "ok"


def _build_match_index(crossref_input: str) -> Dict[str, List[Dict[str, Any]]]:
    if not os.path.isfile(crossref_input):
        return {}
    crossref = _load_json(crossref_input)
    out: Dict[str, List[Dict[str, Any]]] = {}
    for row in crossref.get("unique_candidates") or []:
        text = str(row.get("text") or "")
        matches = row.get("matches") or []
        if text and matches:
            out.setdefault(text, []).extend(matches)
    return out


def _read_pointer_array(pm: StarProcess, elem_base: int, length: int) -> List[int]:
    blob = pm.read_bytes(elem_base, length * 8)
    if not blob or len(blob) != length * 8:
        out: List[int] = []
        for i in range(length):
            out.append(pm.read_u64(elem_base + i * 8) or 0)
        return out
    return list(struct.unpack(f"<{length}Q", blob))


def _project_pointer_tables(pointer_tables: Dict[str, Any], pointer_values: List[int], elem_base: int) -> List[Dict[str, Any]]:
    projections: List[Dict[str, Any]] = []
    length = len(pointer_values)
    for table_index, table in enumerate(pointer_tables.get("tables") or []):
        table_base = _safe_int(table.get("element_base"))
        if table_base is None:
            continue
        delta = table_base - elem_base
        start_index: Optional[int] = None
        rows = table.get("rows") or []
        compared = 0
        matched = 0
        if delta >= 0 and delta % 8 == 0:
            candidate_start = delta // 8
            if candidate_start < length:
                start_index = candidate_start
        if start_index is not None:
            for row in rows:
                row_index = int(row.get("index") or 0)
                global_index = start_index + row_index
                string_obj = _safe_int((row.get("string") or {}).get("obj"))
                if string_obj is None or global_index < 0 or global_index >= length:
                    continue
                compared += 1
                if pointer_values[global_index] == string_obj:
                    matched += 1
        status = "no_overlap"
        if start_index is not None:
            if compared and matched == compared == len(rows):
                status = "matches_all_rows"
            elif compared:
                status = "partial_or_mismatch"
        projections.append({
            "table_index": table_index,
            "table_element_base": table.get("element_base"),
            "row_count": table.get("row_count"),
            "global_start_index": start_index,
            "global_end_index_exclusive": (start_index + len(rows)) if start_index is not None else None,
            "array_slice_status": status,
            "compared_row_count": compared,
            "matched_row_count": matched,
        })
    return projections


def export_static_string_pool(script_json: str, pointer_tables_input: str, crossref_input: str,
                              output: str, summary_output: str, *, max_string_len: int,
                              compact_full: bool) -> Dict[str, Any]:
    si = ScriptIndex.load(script_json)
    pointer_tables = _load_json(pointer_tables_input)
    match_index = _build_match_index(crossref_input)
    pm = StarProcess()
    try:
        ga = _find_gameassembly(pm)
        by_name, by_klass = _build_klass_index(pm, ga.base, si)
        string_runtime = _runtime_row(by_name, "string", "System.String")
        string_array_runtime = _runtime_row(by_name, "string[]", "System.String[]")
        singleton_runtime = _runtime_row(by_name, "ZUtil.ZSingleton<StringPoolManager>")
        runtime_impl_runtime = _runtime_row(by_name, "Panda.Module.StringPoolRuntimeImpl")
        localization_pool_runtime = _runtime_row(by_name, "Panda.Utility.Localization.LocalizationStringPool")
        string_klass = _safe_int((string_runtime or {}).get("runtime_klass"))
        string_array_klass = _safe_int((string_array_runtime or {}).get("runtime_klass"))
        singleton_klass = _safe_int((singleton_runtime or {}).get("runtime_klass"))
        runtime_impl_klass = _safe_int((runtime_impl_runtime or {}).get("runtime_klass"))
        localization_pool_klass = _safe_int((localization_pool_runtime or {}).get("runtime_klass"))
        if not all((string_klass, string_array_klass, singleton_klass, runtime_impl_klass, localization_pool_klass)):
            raise RuntimeError("Missing required runtime classes for StringPoolManager static export")

        static_fields = pm.read_u64(singleton_klass + _CLASS_STATIC_FIELDS_OFF)
        singleton_instance = pm.read_u64((static_fields or 0) + _SINGLETON_INSTANCE_OFF)
        singleton_lock = pm.read_u64((static_fields or 0) + _SINGLETON_LOCK_OFF)
        manager_klass = pm.read_u64(singleton_instance or 0)
        common_pool = pm.read_u64((singleton_instance or 0) + _STRING_POOL_COMMON_OFF)
        runtime_impl = pm.read_u64((singleton_instance or 0) + _STRING_POOL_IMPL_OFF)
        common_pool_klass = pm.read_u64(common_pool or 0)
        runtime_impl_obj_klass = pm.read_u64(runtime_impl or 0)
        if common_pool_klass != localization_pool_klass:
            raise RuntimeError(f"StringPoolManager +0x10 did not validate as LocalizationStringPool: {_hex(common_pool_klass)}")
        if runtime_impl_obj_klass != runtime_impl_klass:
            raise RuntimeError(f"StringPoolManager +0x18 did not validate as StringPoolRuntimeImpl: {_hex(runtime_impl_obj_klass)}")

        file_name_obj = pm.read_u64(common_pool + _LOCALIZATION_FILE_NAME_OFF)
        file_name, _, file_name_status = _read_string(pm, file_name_obj or 0, string_klass, max_len=max_string_len)
        common_dict = pm.read_u64(common_pool + _LOCALIZATION_DICT_OFF)
        all_array = pm.read_u64(runtime_impl + _RUNTIME_IMPL_ALL_LOCALIZATION_OFF)
        all_array_klass = pm.read_u64(all_array or 0)
        if all_array_klass != string_array_klass:
            raise RuntimeError(f"allLocalizationString_ did not validate as string[]: {_hex(all_array_klass)}")
        length = pm.read_i32(all_array + _ARRAY_LENGTH_OFF)
        if length is None or length < 0 or length > 500000:
            raise RuntimeError(f"Unexpected allLocalizationString_ length: {length}")
        elem_base = all_array + _ARRAY_ELEMS_OFF
        pointer_values = _read_pointer_array(pm, elem_base, int(length))
        rows: List[Dict[str, Any]] = []
        target_matches: List[Dict[str, Any]] = []
        matched_external_rows: List[Dict[str, Any]] = []
        nonempty = 0
        decode_status_counts: Dict[str, int] = {}
        for index, string_obj in enumerate(pointer_values):
            text, text_length, status = _read_string(pm, string_obj, string_klass, max_len=max_string_len)
            decode_status_counts[status] = decode_status_counts.get(status, 0) + 1
            if text:
                nonempty += 1
            row: Dict[str, Any] = {
                "index": index,
                "slot_addr": f"0x{elem_base + index * 8:X}",
                "string_obj": _hex(string_obj),
                "text": text,
                "length": text_length,
                "decode_status": status,
            }
            matches = match_index.get(text or "", [])
            if matches:
                row["matches"] = matches
                matched_external_rows.append({
                    "index": index,
                    "slot_addr": row["slot_addr"],
                    "string_obj": row["string_obj"],
                    "text": text,
                    "matches": matches,
                })
            if text in _TARGET_TEXTS:
                target_matches.append({
                    "index": index,
                    "slot_addr": row["slot_addr"],
                    "string_obj": row["string_obj"],
                    "text": text,
                    "matches": matches,
                })
            rows.append(row)

        projections = _project_pointer_tables(pointer_tables, pointer_values, elem_base)
        anchor = {
            "status": "resolved_static_singleton_string_pool",
            "chain": "ZUtil.ZSingleton<StringPoolManager>.static_fields.instance_ -> StringPoolManager.impl_ -> StringPoolRuntimeImpl.allLocalizationString_",
            "class_static_fields_offset": f"0x{_CLASS_STATIC_FIELDS_OFF:X}",
            "singleton": {
                "runtime": singleton_runtime,
                "static_fields": _hex(static_fields),
                "instance_slot_offset": f"0x{_SINGLETON_INSTANCE_OFF:X}",
                "lock_slot_offset": f"0x{_SINGLETON_LOCK_OFF:X}",
                "instance": _hex(singleton_instance),
                "lock": _hex(singleton_lock),
                "instance_klass": _hex(manager_klass),
                "instance_klass_script_name": by_klass.get(int(manager_klass or 0)),
                "instance_klass_status": "unmapped_but_field_layout_validated",
            },
            "string_pool_manager_fields": {
                "mCommonCfgEnPoolLocalization_": {
                    "offset": f"0x{_STRING_POOL_COMMON_OFF:X}",
                    "obj": _hex(common_pool),
                    "klass": _hex(common_pool_klass),
                    "klass_name": by_klass.get(int(common_pool_klass or 0)),
                    "fileName_": file_name,
                    "fileName_status": file_name_status,
                    "currLanguageStringPool_": _hex(common_dict),
                },
                "impl_": {
                    "offset": f"0x{_STRING_POOL_IMPL_OFF:X}",
                    "obj": _hex(runtime_impl),
                    "klass": _hex(runtime_impl_obj_klass),
                    "klass_name": by_klass.get(int(runtime_impl_obj_klass or 0)),
                },
            },
            "allLocalizationString_": {
                "field_offset": f"0x{_RUNTIME_IMPL_ALL_LOCALIZATION_OFF:X}",
                "array_obj": _hex(all_array),
                "array_klass": _hex(all_array_klass),
                "array_klass_name": by_klass.get(int(all_array_klass or 0)),
                "length": int(length),
                "element_base": _hex(elem_base),
            },
        }
        summary = {
            "source": "live_string_pool_static_export",
            "script_json": _rel(script_json),
            "pointer_tables_input": _rel(pointer_tables_input),
            "crossref_input": _rel(crossref_input),
            "full_table_output": _rel(output),
            "process": pm.name,
            "pid": pm.pid,
            "gameassembly": {"base": _hex(ga.base), "size": _hex(ga.size)},
            "verified_runtime_classes": {
                "System.String": string_runtime,
                "System.String[]": string_array_runtime,
                "ZUtil.ZSingleton<StringPoolManager>": singleton_runtime,
                "Panda.Module.StringPoolRuntimeImpl": runtime_impl_runtime,
                "Panda.Utility.Localization.LocalizationStringPool": localization_pool_runtime,
            },
            "anchor": anchor,
            "pointer_table_projections": projections,
            "target_matches": target_matches,
            "matched_external_row_count": len(matched_external_rows),
            "matched_external_rows_preview": matched_external_rows[:512],
            "summary": {
                "array_length": int(length),
                "nonempty_text_count": nonempty,
                "decode_status_counts": dict(sorted(decode_status_counts.items())),
                "target_match_count": len(target_matches),
                "pointer_tables_resolved_count": sum(1 for p in projections if p.get("array_slice_status") == "matches_all_rows"),
            },
        }
        full = dict(summary)
        full["rows"] = rows
        _write_json(summary_output, summary)
        _write_json(output, full, indent=None if compact_full else 2)
        return summary
    finally:
        pm.close()


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Export live StringPoolManager allLocalizationString_ table")
    parser.add_argument("--script-json", default=_DEFAULT_SCRIPT_JSON)
    parser.add_argument("--pointer-tables-input", default=_DEFAULT_POINTER_TABLES)
    parser.add_argument("--crossref-input", default=_DEFAULT_CROSSREF)
    parser.add_argument("--output", default=_DEFAULT_OUTPUT)
    parser.add_argument("--summary-output", default=_DEFAULT_SUMMARY_OUTPUT)
    parser.add_argument("--max-string-len", type=int, default=4096)
    parser.add_argument("--pretty-full", action="store_true", help="Pretty-print the large full-table export")
    args = parser.parse_args(argv)
    result = export_static_string_pool(
        args.script_json,
        args.pointer_tables_input,
        args.crossref_input,
        args.output,
        args.summary_output,
        max_string_len=args.max_string_len,
        compact_full=not args.pretty_full,
    )
    print(json.dumps({
        "output": args.output,
        "summary_output": args.summary_output,
        "summary": result.get("summary"),
        "anchor_status": ((result.get("anchor") or {}).get("status")),
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())