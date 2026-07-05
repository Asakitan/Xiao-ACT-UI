# Export live StringPool by aligning a current pointer-table slice to a prior full table.
#
# This is a fallback for sessions where the IL2CPP TypeInfo/static singleton chain
# is stale but the decoded runtime pointer table matches a known allLocalization
# text slice exactly. It reads the inferred allLocalizationString_ array from the
# live process and writes the same full-table schema shape as static export, while
# marking the anchor as text-aligned fallback instead of a static singleton proof.

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from collections import Counter, defaultdict
from typing import Any, Dict, List, Optional, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO = os.path.dirname(os.path.dirname(_HERE))
if _SAO not in sys.path:
    sys.path.insert(0, _SAO)

from plugins.star_resonance_plugin.mem.process import StarProcess

_NAME_TABLES = os.path.join(_SAO, "assets", "name_tables")
_DEFAULT_POINTER_TABLES = os.path.join(_NAME_TABLES, "live_probe_pointer_tables.json")
_DEFAULT_REFERENCE_FULL = os.path.join(_NAME_TABLES, "live_string_pool_all_localization.json")
_DEFAULT_OUTPUT = os.path.join(_NAME_TABLES, "live_string_pool_all_localization.json")
_DEFAULT_SUMMARY_OUTPUT = os.path.join(_NAME_TABLES, "live_string_pool_static_anchor.json")
_STRING_LAYOUT = {"klass": 0x00, "length": 0x10, "chars": 0x14}
_ARRAY_LENGTH_OFF = 0x18
_ARRAY_ELEMS_OFF = 0x20


def _load_json(path: str) -> Any:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _write_json(path: str, payload: Any, *, indent: Optional[int] = None) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=indent)


def _rel(path: str) -> str:
    try:
        return os.path.relpath(path, _SAO).replace("\\", "/")
    except ValueError:
        return path.replace("\\", "/")


def _safe_int(raw: Any) -> Optional[int]:
    if raw is None:
        return None
    try:
        return int(str(raw), 16) if isinstance(raw, str) else int(raw)
    except (TypeError, ValueError):
        return None


def _hex(value: Optional[int]) -> Optional[str]:
    return f"0x{int(value):X}" if value else None


def _extract_reference_rows(obj: Dict[str, Any]) -> List[Dict[str, Any]]:
    rows = obj.get("rows") or obj.get("allLocalizationString") or obj.get("strings") or []
    if isinstance(rows, dict):
        rows = rows.get("rows") or rows.get("entries") or []
    return [dict(row) for row in rows if isinstance(row, dict)]


def _infer_alignment(pointer_tables: Dict[str, Any], reference_rows: List[Dict[str, Any]]) -> Tuple[Dict[str, Any], int]:
    by_text: Dict[str, List[int]] = defaultdict(list)
    for fallback_idx, row in enumerate(reference_rows):
        text = str(row.get("text") or "")
        if not text:
            continue
        try:
            idx = int(row.get("index"))
        except Exception:
            idx = fallback_idx
        by_text[text].append(idx)
    best_table: Optional[Dict[str, Any]] = None
    best_offset = 0
    best_count = -1
    for table in pointer_tables.get("tables") or []:
        counts: Counter[int] = Counter()
        for row in table.get("rows") or []:
            string_info = row.get("string") or {}
            text = str(string_info.get("text") or "")
            if not text:
                continue
            row_index = int(row.get("index") or 0)
            for ref_index in by_text.get(text, []):
                counts[ref_index - row_index] += 1
        if not counts:
            continue
        offset, count = counts.most_common(1)[0]
        if count > best_count:
            best_table = table
            best_offset = int(offset)
            best_count = int(count)
    if best_table is None or best_count <= 0:
        raise RuntimeError("Unable to align pointer table rows to reference full table")
    if best_count < int(best_table.get("row_count") or 0):
        raise RuntimeError(
            f"Best text alignment is partial: {best_count}/{best_table.get('row_count')} rows"
        )
    return dict(best_table), best_offset


def _read_pointer_array(pm: StarProcess, elem_base: int, length: int) -> List[int]:
    blob = pm.read_bytes(elem_base, length * 8)
    if blob and len(blob) == length * 8:
        return list(struct.unpack(f"<{length}Q", blob))
    out: List[int] = []
    for idx in range(length):
        out.append(pm.read_u64(elem_base + idx * 8) or 0)
    return out


def _read_string(pm: StarProcess, obj: int, string_klass: int, *, max_len: int) -> Tuple[Optional[str], Optional[int], str]:
    if not obj:
        return None, None, "null_pointer"
    klass = pm.read_u64(obj + _STRING_LAYOUT["klass"])
    if klass != string_klass:
        return None, None, "klass_mismatch"
    length = pm.read_i32(obj + _STRING_LAYOUT["length"])
    if length is None:
        return None, None, "length_unreadable"
    if length < 0 or length > max_len:
        return None, int(length), "length_out_of_range"
    text = pm.read_utf16(obj + _STRING_LAYOUT["chars"], length)
    if text is None:
        return None, int(length), "chars_unreadable"
    return text, int(length), "ok"


def export_text_aligned_string_pool(pointer_tables_input: str, reference_full_input: str,
                                    output: str, summary_output: str, *, max_string_len: int,
                                    pretty_full: bool) -> Dict[str, Any]:
    pointer_tables = _load_json(pointer_tables_input)
    reference_full = _load_json(reference_full_input)
    reference_rows = _extract_reference_rows(reference_full)
    if not reference_rows:
        raise RuntimeError("Reference full table has no rows/texts to align against")
    table, global_start_index = _infer_alignment(pointer_tables, reference_rows)
    table_base = _safe_int(table.get("element_base"))
    if table_base is None:
        raise RuntimeError("Aligned pointer table has no element_base")
    elem_base = table_base - global_start_index * 8
    array_obj = elem_base - _ARRAY_ELEMS_OFF
    first_string = next((row.get("string") for row in table.get("rows") or [] if row.get("string")), None) or {}
    string_klass = _safe_int(first_string.get("klass"))
    if string_klass is None:
        raise RuntimeError("Unable to infer System.String klass from aligned pointer table")

    pm = StarProcess()
    try:
        arr_klass = pm.read_u64(array_obj)
        length = pm.read_i32(array_obj + _ARRAY_LENGTH_OFF)
        if length is None or length <= 0 or length > 500000:
            raise RuntimeError(f"Unexpected inferred allLocalizationString length: {length}")
        pointer_values = _read_pointer_array(pm, elem_base, int(length))
        rows: List[Dict[str, Any]] = []
        nonempty = 0
        decode_status_counts: Dict[str, int] = {}
        matched_reference = 0
        mismatch_samples: List[Dict[str, Any]] = []
        for index, string_obj in enumerate(pointer_values):
            text, text_length, status = _read_string(pm, string_obj, string_klass, max_len=max_string_len)
            decode_status_counts[status] = decode_status_counts.get(status, 0) + 1
            ref_text = str(reference_rows[index].get("text") or "") if index < len(reference_rows) else ""
            if text:
                nonempty += 1
            live_text_for_compare = text or ""
            if live_text_for_compare == ref_text:
                matched_reference += 1
            elif len(mismatch_samples) < 20:
                mismatch_samples.append({"index": index, "live_text": text, "reference_text": ref_text})
            row = {
                "index": index,
                "string_obj": _hex(string_obj),
                "string_chars": _hex((string_obj or 0) + _STRING_LAYOUT["chars"]),
                "string_klass": _hex(pm.read_u64(string_obj or 0) or 0),
                "text": text or "",
                "length": text_length,
                "decode_status": status,
            }
            rows.append(row)
        projection = {
            "table_index": (pointer_tables.get("tables") or []).index(table),
            "table_element_base": table.get("element_base"),
            "row_count": table.get("row_count"),
            "global_start_index": global_start_index,
            "global_end_index_exclusive": global_start_index + int(table.get("row_count") or 0),
            "array_slice_status": "matches_all_rows",
            "compared_row_count": int(table.get("row_count") or 0),
            "matched_row_count": int(table.get("row_count") or 0),
        }
        anchor = {
            "status": "text_aligned_pointer_table_fallback",
            "chain": "pointer_table_text_alignment -> inferred allLocalizationString_ array",
            "reason": "IL2CPP static singleton TypeInfo chain did not validate for current process; pointer table rows matched reference full table texts exactly.",
            "allLocalizationString_": {
                "array_obj": _hex(array_obj),
                "array_klass": _hex(arr_klass),
                "element_base": _hex(elem_base),
                "length": int(length),
                "string_klass": _hex(string_klass),
            },
        }
        result = {
            "source": "live_string_pool_text_align_export",
            "reference_full_input": _rel(reference_full_input),
            "pointer_tables_input": _rel(pointer_tables_input),
            "full_table_output": _rel(output),
            "process": pm.name,
            "pid": pm.pid,
            "anchor": anchor,
            "pointer_table_projections": [projection],
            "summary": {
                "allLocalizationString_length": int(length),
                "row_count": len(rows),
                "nonempty_text_count": nonempty,
                "decode_status_counts": decode_status_counts,
                "matched_reference_text_count": matched_reference,
                "reference_text_mismatch_count": max(0, len(rows) - matched_reference),
                "mismatch_samples": mismatch_samples,
                "persistent_anchor_status": "text_aligned_pointer_table_fallback",
            },
            "rows": rows,
        }
        summary = dict(result)
        summary.pop("rows", None)
        _write_json(summary_output, summary, indent=2)
        _write_json(output, result, indent=2 if pretty_full else None)
        return summary
    finally:
        pm.close()


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Export live StringPool by text-aligning pointer table to a reference full table")
    parser.add_argument("--pointer-tables-input", default=_DEFAULT_POINTER_TABLES)
    parser.add_argument("--reference-full-input", default=_DEFAULT_REFERENCE_FULL)
    parser.add_argument("--output", default=_DEFAULT_OUTPUT)
    parser.add_argument("--summary-output", default=_DEFAULT_SUMMARY_OUTPUT)
    parser.add_argument("--max-string-len", type=int, default=4096)
    parser.add_argument("--pretty-full", action="store_true")
    args = parser.parse_args(argv)
    result = export_text_aligned_string_pool(
        args.pointer_tables_input,
        args.reference_full_input,
        args.output,
        args.summary_output,
        max_string_len=args.max_string_len,
        pretty_full=bool(args.pretty_full),
    )
    print(json.dumps({"output": args.output, "summary_output": args.summary_output, "summary": result.get("summary")}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
