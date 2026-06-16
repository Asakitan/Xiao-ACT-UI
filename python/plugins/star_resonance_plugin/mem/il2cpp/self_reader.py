"""self_reader — 端到端读 self 玩家数据 (HP/MaxHP/UID).

工作流程:
  1. script.json → CharSerialize klass = *(GA + RVA(CharSerialize_TypeInfo))
  2. 全堆扫: 找首 8 字节 == klass 的对象 (CharSerialize 实例)
  3. 对每个实例验证 Attr@+0x88 指向有效 UserFightAttr (其 obj+0 == UserFightAttr klass)
  4. 读 CharId@+0x10 (UID), Attr→CurHp@+0x10, Attr→MaxHp@+0x18
  5. 校验 CharId == 已知 UID

CharSerialize 实例可能在 >256MB 的大私有区里, 必须扫所有可读私有区.
"""
from __future__ import annotations

import argparse
import os
import sys
import time
from typing import List, Optional

_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from mem_probe.process import StarProcess
from plugins.star_resonance_plugin.mem.il2cpp.script_parser import ScriptIndex
from mem_probe import cy_memscan as _cy


CHARSERIALIZE_CHARID_OFF = 0x10
CHARSERIALIZE_ATTR_OFF = 0x88
USERFIGHTATTR_CURHP_OFF = 0x10
USERFIGHTATTR_MAXHP_OFF = 0x18


def scan_objs_with_klass(pm, klass_ptr: int, max_region: Optional[int] = None,
                         max_hits: int = 1024) -> List[int]:
    """全私有读区扫描 *(addr) == klass_ptr.

    内层走 cy_memscan.find_aligned_u64 (AVX2: ~16 GB/s).
    """
    hits: List[int] = []
    klass = klass_ptr & 0xFFFFFFFFFFFFFFFF
    for r in pm.iter_regions(only_readable=True, only_private=True):
        if max_region is not None and r.size > max_region:
            continue
        chunk = 16 * 1024 * 1024
        off = 0
        while off < r.size:
            n = min(chunk, r.size - off)
            blob = pm.read_bytes(r.base + off, n)
            if blob is None:
                break
            remaining = max_hits - len(hits)
            if remaining <= 0:
                return hits
            offs = _cy.find_aligned_u64(blob, klass, max_hits=remaining)
            for o in offs:
                hits.append(r.base + off + o)
            if len(hits) >= max_hits:
                return hits
            off += n
    return hits


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--dump-id", default="ef9ef95a")
    p.add_argument("--known-uid", type=int, default=36668136)
    p.add_argument("--known-hp", type=int, default=None)
    args = p.parse_args()

    sj_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "out", args.dump_id, "dumper_out", "script.json")
    si = ScriptIndex.load(sj_path)
    cs_rva = si.find_klass("Zproto.CharSerialize")
    ufa_rva = si.find_klass("Zproto.UserFightAttr")

    pm = StarProcess()
    try:
        ga = next(m for m in pm.list_modules() if m.name.lower() == "gameassembly.dll")
        cs_klass = pm.read_u64(ga.base + cs_rva)
        ufa_klass = pm.read_u64(ga.base + ufa_rva)
        print(f"[meta] GA=0x{ga.base:X} cs_klass=0x{cs_klass:X} ufa_klass=0x{ufa_klass:X}")

        print(f"[scan] CharSerialize 实例 (无 region 大小限制)...")
        t0 = time.time()
        cands = scan_objs_with_klass(pm, cs_klass, max_region=None, max_hits=2048)
        print(f"[scan] {time.time()-t0:.1f}s -> {len(cands)} 候选")

        # 过滤: Attr@+0x88 必须指向有效 UserFightAttr (deref 后首字节 == ufa_klass)
        valid = []
        for obj in cands:
            attr = pm.read_u64(obj + CHARSERIALIZE_ATTR_OFF)
            if not attr:
                continue
            if pm.read_u64(attr) != ufa_klass:
                continue
            char_id = pm.read_i64(obj + CHARSERIALIZE_CHARID_OFF)
            cur_hp = pm.read_i64(attr + USERFIGHTATTR_CURHP_OFF)
            max_hp = pm.read_i64(attr + USERFIGHTATTR_MAXHP_OFF)
            valid.append((obj, attr, char_id, cur_hp, max_hp))

        print(f"[filter] {len(valid)} 个 CharSerialize 通过 Attr 验证 (Attr 指向有效 UserFightAttr):\n")
        for obj, attr, cid, hp, mhp in valid:
            tag = " *** SELF ***" if cid == args.known_uid else ""
            print(f"   CharSerialize@0x{obj:X}  CharId={cid}  HP={hp}/{mhp}{tag}")

        self_hits = [v for v in valid if v[2] == args.known_uid]
        if not self_hits:
            print(f"\n[FAIL] 没找到 CharId == {args.known_uid}")
            return 1

        print(f"\n[OK] Self 找到! CharId={args.known_uid}")
        for obj, attr, cid, hp, mhp in self_hits:
            print(f"   CharSerialize obj  = 0x{obj:X}")
            print(f"   UserFightAttr obj  = 0x{attr:X}")
            print(f"   UID (CharId @+0x10) = {cid}")
            print(f"   CurHp (Attr+0x10)   = {hp}")
            print(f"   MaxHp (Attr+0x18)   = {mhp}")
            if args.known_hp is not None:
                ok = (hp == args.known_hp)
                print(f"   [verify HP={args.known_hp}] {'OK' if ok else 'FAIL'}")
        return 0
    finally:
        pm.close()


if __name__ == "__main__":
    raise SystemExit(main())

