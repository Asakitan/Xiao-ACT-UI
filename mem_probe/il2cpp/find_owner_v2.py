"""find_owner_v2 — 输出写文件,stdout 仅汇总."""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from collections import Counter
from typing import Dict, List, Tuple

_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from mem_probe.process import StarProcess
from mem_probe.il2cpp.script_parser import ScriptIndex


def scan_pointers_to(pm, target, max_region=512 * 1024 * 1024, max_hits=20000):
    from mem_probe import cy_memscan as _cy
    hits = []
    t0 = time.time()
    scanned = 0
    target_u64 = int(target) & 0xFFFFFFFFFFFFFFFF
    for r in pm.iter_regions(only_readable=True, only_private=True):
        if r.size > max_region:
            continue
        chunk = 16 * 1024 * 1024
        off = 0
        while off < r.size:
            n = min(chunk, r.size - off)
            blob = pm.read_bytes(r.base + off, n)
            if blob is None:
                break
            scanned += len(blob)
            remaining = max_hits - len(hits)
            if remaining <= 0:
                return hits, scanned, time.time() - t0
            for o in _cy.find_aligned_u64(blob, target_u64, max_hits=remaining):
                hits.append(r.base + off + o)
            if len(hits) >= max_hits:
                return hits, scanned, time.time() - t0
            off += n
    return hits, scanned, time.time() - t0


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--target", type=lambda s: int(s, 16), default=0x13FBE4F660)
    p.add_argument("--dump-id", default="ef9ef95a")
    p.add_argument("--out", default="owner_results.json")
    args = p.parse_args()

    sj_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "out", args.dump_id, "dumper_out", "script.json")
    print(f"[load] {sj_path}")
    si = ScriptIndex.load(sj_path)

    pm = StarProcess()
    try:
        ga = next(m for m in pm.list_modules() if m.name.lower() == "gameassembly.dll")
        print(f"[ga] base=0x{ga.base:X}")

        print(f"[scan] -> ptrs to 0x{args.target:X}")
        ptrs, scanned, dur = scan_pointers_to(pm, args.target)
        print(f"[scan] {scanned/1e9:.2f}GB in {dur:.1f}s -> {len(ptrs)} hits")

        if not ptrs:
            return 1

        print(f"[idx] building klass→name table...")
        t0 = time.time()
        klass_to_name: Dict[int, str] = {}
        for name, rva in si.klass_rva.items():
            kp = pm.read_u64(ga.base + rva)
            if kp:
                klass_to_name[kp] = name
        print(f"[idx] {len(klass_to_name)} klasses in {time.time()-t0:.1f}s")

        print(f"[analyze] resolving owners for {len(ptrs)} ptr locations...")
        owners: List[Tuple[int, int, str]] = []  # (obj, off, name)
        unknown = 0
        for ploc in ptrs:
            best = None
            for off in range(0, 0x208, 8):
                cand = ploc - off
                k = pm.read_u64(cand)
                if k and k in klass_to_name:
                    best = (cand, off, klass_to_name[k])
                    break
            if best:
                owners.append(best)
            else:
                unknown += 1

        cls_counts = Counter(n for _, _, n in owners)
        print(f"\n[result] {len(owners)} owners identified, {unknown} unknown")
        print(f"[result] {len(cls_counts)} distinct owner types:")
        for name, cnt in cls_counts.most_common(30):
            print(f"   {cnt:>5}x  {name}")

        # 写明细
        out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), args.out)
        with open(out_path, "w") as f:
            json.dump({
                "target": f"0x{args.target:X}",
                "ptr_locations": [f"0x{p:X}" for p in ptrs],
                "owners": [{"obj": f"0x{o:X}", "off": f"0x{off:X}", "name": n} for o, off, n in owners],
                "unknown_count": unknown,
            }, f, indent=2)
        print(f"[write] {out_path}")
        return 0
    finally:
        pm.close()


if __name__ == "__main__":
    raise SystemExit(main())

