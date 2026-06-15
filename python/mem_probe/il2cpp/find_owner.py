"""find_owner — 反向扫描堆找谁持有指向已知对象的指针.

对每个候选所有者:
  - 报告地址 + 该指针所在偏移
  - 读所有者首 8 字节 (klass), 反查 script.json 得到类名
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from typing import Dict, List, Tuple

_SAO_AUTO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
if _SAO_AUTO_ROOT not in sys.path:
    sys.path.insert(0, _SAO_AUTO_ROOT)

from mem_probe.process import StarProcess
from mem_probe.il2cpp.script_parser import ScriptIndex


def build_klass_index(si: ScriptIndex, ga_base: int, pm: StarProcess) -> Dict[int, str]:
    """读所有 *_TypeInfo RVA 解出 runtime klass_ptr → class_name 反查表."""
    out: Dict[int, str] = {}
    for name, rva in si.klass_rva.items():
        kp = pm.read_u64(ga_base + rva)
        if kp:
            # 后写覆盖前写, 留意冲突
            out[kp] = name
    return out


def scan_pointers_to(pm: StarProcess, target: int, max_region: int = 512 * 1024 * 1024,
                     max_hits: int = 5000) -> List[int]:
    """全扫描私有读区, 找 8 字节 == target 的位置 (cy_memscan AVX2)."""
    from mem_probe import cy_memscan as _cy
    hits: List[int] = []
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
                break
            for o in _cy.find_aligned_u64(blob, target_u64, max_hits=remaining):
                hits.append(r.base + off + o)
            if len(hits) >= max_hits:
                break
            off += n
        if len(hits) >= max_hits:
            break
    print(f"   scanned {scanned/1e9:.2f}GB in {time.time()-t0:.1f}s -> {len(hits)} hits")
    return hits


def main(argv=None) -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--target", type=lambda s: int(s, 16), default=0x13FBE4F660,
                   help="要找谁持有它的对象地址 (UserFightAttr 实例)")
    p.add_argument("--dump-id", default="ef9ef95a")
    p.add_argument("--known-uid", type=int, default=36668136)
    args = p.parse_args(argv)

    sj_path = os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "out", args.dump_id, "dumper_out", "script.json",
    )
    print(f"[load] {sj_path}")
    si = ScriptIndex.load(sj_path)

    pm = StarProcess()
    try:
        ga = next((m for m in pm.list_modules() if m.name.lower() == "gameassembly.dll"), None)
        assert ga
        print(f"[ga] base=0x{ga.base:X}")

        print(f"\n[1] 扫描堆找指向 0x{args.target:X} 的指针 ...")
        ptr_locations = scan_pointers_to(pm, args.target)

        if not ptr_locations:
            print("[fail] 没有指针指向该对象 (它可能是孤立的, 或者在大区里)")
            return 1

        print(f"\n[2] 构建 klass→ClassName 反查表 ({len(si.klass_rva)} 个类) ...")
        klass_to_name = build_klass_index(si, ga.base, pm)
        print(f"   有效 runtime klass: {len(klass_to_name)}")

        print(f"\n[3] 分析每个指针位置, 推测所有者 obj_base 和类名:")
        print(f"   尝试 [-0x200, 0] 区间, 找最近的有效 klass header")
        # 按 klass-name 聚合 owner
        owner_summary: Dict[str, List[Tuple[int, int]]] = {}
        for ploc in ptr_locations[:500]:  # 只详细分析前 500 个
            best = None
            for off in range(0, 0x208, 8):
                cand_obj = ploc - off
                klass = pm.read_u64(cand_obj)
                if klass and klass in klass_to_name:
                    best = (cand_obj, off, klass_to_name[klass])
                    break
            if best:
                obj_base, field_off, cname = best
                owner_summary.setdefault(cname, []).append((obj_base, field_off))

        print(f"\n[result] 不同所有者类型 ({len(owner_summary)}):")
        # 按命中数排序
        for cname, items in sorted(owner_summary.items(), key=lambda kv: -len(kv[1])):
            print(f"   {len(items):>4}x  {cname}  (示例字段偏移: {sorted(set(o for _, o in items))[:5]})")
            for obj, off in items[:3]:
                print(f"        obj=0x{obj:X}  field@+0x{off:X}")
        return 0
    finally:
        pm.close()


if __name__ == "__main__":
    raise SystemExit(main())

