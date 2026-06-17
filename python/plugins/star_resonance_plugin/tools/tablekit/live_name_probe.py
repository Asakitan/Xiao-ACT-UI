"""Read-only live probe for Chinese name/localization strings in Star.exe.

This diagnostic intentionally uses only PROCESS_VM_READ via ``StarProcess``.
It searches screenshot/crib terms as UTF-16, reconstructs candidate
Il2CppString objects, and samples nearby string-pool ranges so runtime table
extraction work can start from concrete live addresses instead of guesses.
"""

from __future__ import annotations

import argparse
import json
import os
import time
from typing import Any, Dict, Iterable, List, Optional

from plugins.star_resonance_plugin.mem.process import StarProcess, is_admin
from mem_probe.scanner import scan


DEFAULT_TERMS = [
    "神盾骑士",
    "光盾回复流",
    "生命上限",
    "物理攻击",
]


def _looks_cn(text: str) -> bool:
    if not text or len(text) > 96:
        return False
    cn = sum(1 for ch in text if "\u4e00" <= ch <= "\u9fff")
    return cn >= max(1, min(len(text), 6) // 2)


def _looks_name_entry(text: str) -> bool:
    if not _looks_cn(text):
        return False
    if len(text) > 16:
        return False
    noisy = ("<", ">", "{", "}", "。", "，", "、", "可", "用于", "获得", "打开后", "常用于")
    if any(token in text for token in noisy):
        return False
    return True


def _module_label(pm: StarProcess, addr: int) -> str:
    try:
        for mod in pm.list_modules():
            if mod.base <= addr < mod.base + mod.size:
                return f"{mod.name}+0x{addr - mod.base:X}"
    except Exception:
        pass
    return "heap"


def _read_i32_from(buf: bytes, off: int) -> Optional[int]:
    if off < 0 or off + 4 > len(buf):
        return None
    return int.from_bytes(buf[off:off + 4], "little", signed=True)


def _read_u64_from(buf: bytes, off: int) -> Optional[int]:
    if off < 0 or off + 8 > len(buf):
        return None
    return int.from_bytes(buf[off:off + 8], "little", signed=False)


def _decode_utf16_from(buf: bytes, off: int, chars: int) -> str:
    raw = buf[off:off + chars * 2]
    return raw.decode("utf-16-le", errors="replace")


def _candidate_string_at(pm: StarProcess, chars_addr: int) -> Optional[Dict[str, Any]]:
    obj = chars_addr - 0x14
    length = pm.read_i32(obj + 0x10)
    if length is None or not (1 <= length <= 96):
        return None
    klass = pm.read_ptr(obj)
    if not klass or not (0x10000000 <= klass <= 0x7FFFFFFFFFFF):
        return None
    text = pm.read_utf16(chars_addr, min(length, 96)) or ""
    if not _looks_cn(text):
        return None
    return {
        "obj": f"0x{obj:X}",
        "chars": f"0x{chars_addr:X}",
        "klass": f"0x{klass:X}",
        "length": int(length),
        "text": text,
        "where": _module_label(pm, chars_addr),
    }


def _scan_terms(pm: StarProcess, terms: Iterable[str], max_hits: int) -> Dict[str, Any]:
    out: Dict[str, Any] = {}
    for term in terms:
        t0 = time.time()
        hits = scan(pm, term, "utf16", max_hits=max_hits, max_region_size=512 * 1024 * 1024)
        rows: List[Dict[str, Any]] = []
        for addr in hits[:max_hits]:
            item = _candidate_string_at(pm, addr)
            if item is None:
                item = {
                    "chars": f"0x{addr:X}",
                    "text": pm.read_utf16(addr, 48) or "",
                    "where": _module_label(pm, addr),
                }
            rows.append(item)
        out[term] = {
            "count_capped": len(hits),
            "seconds": round(time.time() - t0, 3),
            "hits": rows,
        }
    return out


def _enumerate_pool(pm: StarProcess, center_chars_addr: int, radius: int,
                    string_klass: Optional[int] = None) -> List[Dict[str, Any]]:
    start = max(0, center_chars_addr - radius)
    end = center_chars_addr + radius
    base = start - 0x20
    size = (end - start) + 0x60
    buf = pm.read_bytes(base, size)
    if not buf:
        return []
    rows: List[Dict[str, Any]] = []
    seen = set()
    for chars_addr in range(start, end, 2):
        obj = chars_addr - 0x14
        obj_off = obj - base
        chars_off = chars_addr - base
        length = _read_i32_from(buf, obj_off + 0x10)
        if length is None or not (1 <= length <= 96):
            continue
        klass = _read_u64_from(buf, obj_off)
        if not klass or not (0x10000000 <= klass <= 0x7FFFFFFFFFFF):
            continue
        if string_klass is not None and klass != string_klass:
            continue
        if chars_off < 0 or chars_off + length * 2 > len(buf):
            continue
        text = _decode_utf16_from(buf, chars_off, length)
        if not _looks_cn(text):
            continue
        key = (obj, text)
        if key in seen:
            continue
        seen.add(key)
        rows.append({
            "obj": f"0x{obj:X}",
            "chars": f"0x{chars_addr:X}",
            "klass": f"0x{klass:X}",
            "length": int(length),
            "text": text,
        })
    rows.sort(key=lambda row: int(str(row["obj"]), 16))
    return rows


def _detect_string_klass(term_hits: Dict[str, Any]) -> Optional[int]:
    for payload in term_hits.values():
        for hit in payload.get("hits") or []:
            klass_raw = hit.get("klass")
            if not klass_raw:
                continue
            try:
                return int(str(klass_raw), 16)
            except (TypeError, ValueError):
                continue
    return None


def _write_candidate_export(result: Dict[str, Any], candidate_output: str) -> None:
    export = {
        "source": "live_name_probe",
        "process": result.get("process"),
        "pid": result.get("pid"),
        "note": "Read-only runtime Il2CppString pool candidates. Not authoritative IDs yet.",
        "terms": result.get("terms") or [],
        "groups": {},
    }
    for key, rows in (result.get("name_candidates") or {}).items():
        export["groups"][key] = [
            {"obj": row.get("obj"), "chars": row.get("chars"), "klass": row.get("klass"), "text": row.get("text")}
            for row in (rows or [])
        ]
    os.makedirs(os.path.dirname(os.path.abspath(candidate_output)), exist_ok=True)
    with open(candidate_output, "w", encoding="utf-8") as f:
        json.dump(export, f, ensure_ascii=False, indent=2)


def run(terms: List[str], output: str, max_hits: int, pool_radius: int,
    string_klass: Optional[int] = None,
        candidate_output: Optional[str] = None) -> Dict[str, Any]:
    pm = StarProcess()
    try:
        result: Dict[str, Any] = {
            "pid": pm.pid,
            "process": pm.name,
            "admin": is_admin(),
            "terms": terms,
        }
        term_hits = _scan_terms(pm, terms, max_hits=max_hits)
        result["term_hits"] = term_hits
        effective_string_klass = string_klass or _detect_string_klass(term_hits)
        if effective_string_klass:
            result["string_klass_filter"] = f"0x{effective_string_klass:X}"
        pools: Dict[str, Any] = {}
        for term, payload in term_hits.items():
            for hit in payload.get("hits") or []:
                chars_raw = hit.get("chars")
                if not chars_raw:
                    continue
                chars_addr = int(str(chars_raw), 16)
                pool_key = f"{term}@0x{chars_addr:X}"
                pools[pool_key] = _enumerate_pool(pm, chars_addr, pool_radius, effective_string_klass)
                break
        result["nearby_pools"] = pools
        candidates: Dict[str, Any] = {}
        for key, rows in pools.items():
            compact = [row for row in (rows or []) if _looks_name_entry(str(row.get("text") or ""))]
            candidates[key] = compact
        result["name_candidates"] = candidates
        os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
        with open(output, "w", encoding="utf-8") as f:
            json.dump(result, f, ensure_ascii=False, indent=2)
        if candidate_output:
            _write_candidate_export(result, candidate_output)
        return result
    finally:
        pm.close()


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Read-only live Chinese name probe for Star.exe")
    parser.add_argument("--term", action="append", dest="terms", help="UTF-16 crib term to search; repeatable")
    parser.add_argument("--output", default=os.path.join("temp", "live_name_probe.json"))
    parser.add_argument("--candidate-output", default="")
    parser.add_argument("--max-hits", type=int, default=40)
    parser.add_argument("--pool-radius", type=lambda s: int(s, 0), default=0x6000)
    parser.add_argument("--string-klass", type=lambda s: int(s, 0), default=0,
                        help="Optional confirmed System.String klass pointer; auto-detected from first valid hit when omitted")
    args = parser.parse_args(argv)
    terms = args.terms or DEFAULT_TERMS
    result = run(terms, args.output, args.max_hits, args.pool_radius,
                 args.string_klass or None, args.candidate_output or None)
    summary = {
        "pid": result.get("pid"),
        "output": args.output,
        "candidate_output": args.candidate_output or None,
        "string_klass_filter": result.get("string_klass_filter"),
        "term_counts": {k: v.get("count_capped", 0) for k, v in result.get("term_hits", {}).items()},
        "pool_counts": {k: len(v or []) for k, v in result.get("nearby_pools", {}).items()},
        "candidate_counts": {k: len(v or []) for k, v in result.get("name_candidates", {}).items()},
    }
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())