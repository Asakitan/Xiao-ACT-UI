"""Scan module image ranges for pointers to runtime table addresses.

``live_name_owner_refs.py`` scans private heap by default.  Static roots for a
runtime pointer table can live in module image/data ranges, so this helper scans
loaded modules directly and reports module-relative pointer locations.  It is
read-only and only records evidence; heap targets remain volatile until the
owning field/static root is proven.
"""

from __future__ import annotations

import argparse
import json
import os
import time
from typing import Any, Dict, Iterable, List, Optional

from mem_probe import cy_memscan as _cy
from plugins.star_resonance_plugin.mem.process import StarProcess


_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO = os.path.dirname(os.path.dirname(_HERE))
_NAME_TABLES = os.path.join(_SAO, "assets", "name_tables")


def _scan_module_for_target(pm: StarProcess, mod: Any, target: int,
                            *, chunk_size: int, max_hits: int) -> Dict[str, Any]:
    hits: List[Dict[str, Any]] = []
    scanned = 0
    failed_chunks = 0
    target_u64 = int(target) & 0xFFFFFFFFFFFFFFFF
    offset = 0
    while offset < mod.size and len(hits) < max_hits:
        n = min(chunk_size, mod.size - offset)
        addr = mod.base + offset
        blob = pm.read_bytes(addr, n)
        if blob is None:
            failed_chunks += 1
            # Some module pages are unreadable.  Fall back to page-sized reads
            # for this chunk instead of losing the whole module range.
            page = 0x1000
            page_off = 0
            while page_off < n and len(hits) < max_hits:
                page_n = min(page, n - page_off)
                page_addr = addr + page_off
                page_blob = pm.read_bytes(page_addr, page_n)
                if page_blob:
                    scanned += len(page_blob)
                    remaining = max_hits - len(hits)
                    for hit_off in _cy.find_aligned_u64(page_blob, target_u64, max_hits=remaining):
                        hit_addr = page_addr + int(hit_off)
                        hits.append({
                            "addr": f"0x{hit_addr:X}",
                            "module": mod.name,
                            "module_offset": f"0x{hit_addr - mod.base:X}",
                        })
                page_off += page_n
        else:
            scanned += len(blob)
            remaining = max_hits - len(hits)
            for hit_off in _cy.find_aligned_u64(blob, target_u64, max_hits=remaining):
                hit_addr = addr + int(hit_off)
                hits.append({
                    "addr": f"0x{hit_addr:X}",
                    "module": mod.name,
                    "module_offset": f"0x{hit_addr - mod.base:X}",
                })
        # Overlap by 7 bytes so a qword straddling chunk boundaries is not missed.
        offset += n if offset + n >= mod.size else max(1, n - 7)
    return {
        "target": f"0x{target:X}",
        "ptr_location_count": len(hits),
        "scanned_bytes": scanned,
        "failed_chunks": failed_chunks,
        "ptr_locations": hits,
    }


def scan_static_refs(targets: Iterable[int], output: str, *, modules: Optional[List[str]],
                     max_module_size: int, chunk_size: int, max_hits: int) -> Dict[str, Any]:
    wanted = [m.lower() for m in (modules or [])]
    pm = StarProcess()
    try:
        loaded = []
        for mod in pm.list_modules():
            if mod.size > max_module_size:
                continue
            if wanted and not any(w in mod.name.lower() for w in wanted):
                continue
            loaded.append(mod)
        rows: List[Dict[str, Any]] = []
        t0 = time.time()
        for target in targets:
            target_rows = []
            for mod in loaded:
                scan = _scan_module_for_target(pm, mod, target, chunk_size=chunk_size, max_hits=max_hits)
                if scan["ptr_location_count"]:
                    target_rows.append(scan | {
                        "module_base": f"0x{mod.base:X}",
                        "module_size": f"0x{mod.size:X}",
                    })
            rows.append({
                "target": f"0x{target:X}",
                "module_hit_count": len(target_rows),
                "ptr_location_count": sum(r["ptr_location_count"] for r in target_rows),
                "modules": target_rows,
            })
        result = {
            "source": "live_name_static_refs",
            "process": pm.name,
            "pid": pm.pid,
            "note": "Read-only module-image scan for static roots pointing at runtime heap/table addresses.",
            "summary": {
                "target_count": len(rows),
                "module_count_scanned": len(loaded),
                "total_ptr_locations": sum(r["ptr_location_count"] for r in rows),
                "seconds": round(time.time() - t0, 3),
            },
            "modules_scanned": [
                {"name": m.name, "base": f"0x{m.base:X}", "size": f"0x{m.size:X}"}
                for m in loaded
            ],
            "targets": rows,
        }
        os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
        with open(output, "w", encoding="utf-8") as f:
            json.dump(result, f, ensure_ascii=False, indent=2)
        return result
    finally:
        pm.close()


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Scan loaded modules for pointers to runtime addresses")
    parser.add_argument("--target", action="append", type=lambda s: int(s, 0), required=True)
    parser.add_argument("--output", default=os.path.join(_NAME_TABLES, "live_probe_pointer_table_static_refs.json"))
    parser.add_argument("--module", action="append", dest="modules",
                        help="Module name substring to scan; repeatable. Default scans all modules under size cap.")
    parser.add_argument("--max-module-size", type=lambda s: int(s, 0), default=512 * 1024 * 1024)
    parser.add_argument("--chunk-size", type=lambda s: int(s, 0), default=4 * 1024 * 1024)
    parser.add_argument("--max-hits", type=int, default=2000)
    args = parser.parse_args(argv)
    result = scan_static_refs(
        args.target,
        args.output,
        modules=args.modules,
        max_module_size=args.max_module_size,
        chunk_size=args.chunk_size,
        max_hits=args.max_hits,
    )
    print(json.dumps({"output": args.output, "summary": result.get("summary")}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
