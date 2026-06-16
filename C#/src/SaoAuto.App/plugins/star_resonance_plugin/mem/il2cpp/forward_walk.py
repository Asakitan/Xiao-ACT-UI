"""forward_walk — 用 dump 元数据正向走 CharSerialize 找 self.

策略 (Phase 1+2 雏形):
    1. 加载 script.json, 取 CharSerialize_TypeInfo, UserFightAttr_TypeInfo 的 RVA
    2. 解 *(GA + RVA) 得到运行时 klass_ptr (klass 在游戏自定义堆里)
    3. 全堆扫: 找首 8 字节 == CharSerialize klass_ptr 的对象 → 候选 self
    4. 对每个候选, 读 CharId @ +0x10, 比对已知 UID 366681365. 命中后, 沿 Attr (offset 0x88) 的指针走到 UserFightAttr 实例
    6. 读 UserFightAttr.CurHp @ +0x10, MaxHp @ +0x18 验证

输出: 完整的 [GA+RVA→klass→scan_for_obj_with_klass→+0x88→deref→+0x10] 链.

CLI:
    python -m tools.mem_probe.il2cpp.forward_walk
"""
from __future__ import annotations

import argparse
import os
import sys
import time
from typing import List

_SAO_AUTO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
if _SAO_AUTO_ROOT not in sys.path:
    sys.path.insert(0, _SAO_AUTO_ROOT)

from mem_probe.process import StarProcess
from plugins.star_resonance_plugin.mem.il2cpp.script_parser import ScriptIndex


_DEFAULT_DUMP_ID = "ef9ef95a"
# 从 dump.cs 抠出来的 CharSerialize 字段偏移
CHARSERIALIZE_CHARID_OFF = 0x10   # public long CharId
CHARSERIALIZE_ATTR_OFF = 0x88     # public UserFightAttr Attr
USERFIGHTATTR_CURHP_OFF = 0x10    # public long CurHp
USERFIGHTATTR_MAXHP_OFF = 0x18    # public long MaxHp

_MAX_REGION_SIZE = 256 * 1024 * 1024


def scan_objects_with_klass(pm: StarProcess, klass_ptr: int, max_hits: int = 256) -> List[int]:
    """全私有堆扫描: 找首 8 字节 == klass_ptr 的对象. 返回 obj_base 列表 (cy_memscan AVX2)."""
    from mem_probe import cy_memscan as _cy
    hits: List[int] = []
    t0 = time.time()
    region_count = 0
    scanned = 0
    klass = int(klass_ptr) & 0xFFFFFFFFFFFFFFFF
    for r in pm.iter_regions(only_readable=True, only_private=True):
        if r.size > _MAX_REGION_SIZE:
            continue
        region_count += 1
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
            for o in _cy.find_aligned_u64(blob, klass, max_hits=remaining):
                hits.append(r.base + off + o)
            if len(hits) >= max_hits:
                break
            off += n
        if len(hits) >= max_hits:
            break
    print(f"   [scan-klass] regions={region_count} scanned={scanned/1e9:.2f}GB "
          f"({time.time()-t0:.1f}s) -> {len(hits)} obj")
    return hits


def main(argv=None) -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--dump-id", default=_DEFAULT_DUMP_ID)
    p.add_argument("--known-uid", type=int, default=36668136, help="已知 self 的 CharId/UID")
    p.add_argument("--known-hp", type=int, default=None, help="已知 self HP (可选, 二次验证)")
    args = p.parse_args(argv)

    sj_path = os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "out", args.dump_id, "dumper_out", "script.json",
    )
    print(f"[load] {sj_path}")
    si = ScriptIndex.load(sj_path)
    print(f"[load] klass entries={len(si.klass_rva)}, type_var entries={len(si.type_var_rva)}")

    cs_rva = si.find_klass("Zproto.CharSerialize")
    ufa_rva = si.find_klass("Zproto.UserFightAttr")
    if cs_rva is None or ufa_rva is None:
        print(f"[fail] 元数据缺失: CharSerialize={cs_rva}, UserFightAttr={ufa_rva}")
        return 1
    print(f"[meta] CharSerialize_TypeInfo @ RVA 0x{cs_rva:X}")
    print(f"[meta] UserFightAttr_TypeInfo @ RVA 0x{ufa_rva:X}")

    pm = StarProcess()
    try:
        ga = next((m for m in pm.list_modules() if m.name.lower() == "gameassembly.dll"), None)
        assert ga, "GameAssembly.dll 未加载"
        print(f"[ga] base=0x{ga.base:X} size=0x{ga.size:X}")

        cs_klass = pm.read_u64(ga.base + cs_rva)
        ufa_klass = pm.read_u64(ga.base + ufa_rva)
        print(f"[klass] CharSerialize klass = 0x{cs_klass:X}")
        print(f"[klass] UserFightAttr klass = 0x{ufa_klass:X}")
        if not cs_klass or not ufa_klass:
            print("[fail] klass 指针读取失败 (可能游戏未初始化此类)")
            return 2

        # Phase 2: 全堆扫 CharSerialize 实例
        print("\n[scan] 扫描 CharSerialize 实例 ...")
        cs_objs = scan_objects_with_klass(pm, cs_klass)
        print(f"[scan] 找到 {len(cs_objs)} 个 CharSerialize 对象")

        # Phase 3: 对每个 obj 读 CharId, 比对 UID
        matched = []
        for obj in cs_objs:
            char_id = pm.read_i64(obj + CHARSERIALIZE_CHARID_OFF)
            attr_ptr = pm.read_u64(obj + CHARSERIALIZE_ATTR_OFF)
            if attr_ptr:
                cur_hp = pm.read_i64(attr_ptr + USERFIGHTATTR_CURHP_OFF) if attr_ptr else None
                max_hp = pm.read_i64(attr_ptr + USERFIGHTATTR_MAXHP_OFF) if attr_ptr else None
                # 验证 attr 对象首字节确实是 ufa_klass
                attr_klass = pm.read_u64(attr_ptr) if attr_ptr else 0
                klass_ok = "✓" if attr_klass == ufa_klass else "✗"
            else:
                cur_hp = max_hp = None
                klass_ok = "-"
                attr_klass = 0
            tag = "  ★ MATCH" if char_id == args.known_uid else ""
            print(f"  obj=0x{obj:X}  CharId={char_id}  Attr=0x{(attr_ptr or 0):X}({klass_ok}) "
                  f"CurHp={cur_hp}  MaxHp={max_hp}{tag}")
            if char_id == args.known_uid:
                matched.append((obj, attr_ptr, cur_hp, max_hp))

        if not matched:
            print(f"\n[result] ❌ 没有 CharSerialize 对象的 CharId == {args.known_uid}")
            print("  → 可能 self 用别的类承载, 或 CharId 字段偏移不对")
            return 3

        print(f"\n[result] ✅ 找到 {len(matched)} 个 self CharSerialize:")
        for obj, attr, hp, mhp in matched:
            print(f"  CharSerialize @ 0x{obj:X}")
            print(f"    CharId           = {pm.read_i64(obj + CHARSERIALIZE_CHARID_OFF)}  ← UID")
            print(f"    Attr (UserFightAttr*) = 0x{attr:X}")
            print(f"      CurHp          = {hp}")
            print(f"      MaxHp          = {mhp}")
            if args.known_hp is not None:
                ok = (hp == args.known_hp)
                print(f"      [verify HP={args.known_hp}] {'✅ MATCH' if ok else '❌ MISMATCH'}")
        return 0
    finally:
        pm.close()


if __name__ == "__main__":
    raise SystemExit(main())

