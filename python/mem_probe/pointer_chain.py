# 指针链回溯 — 让 self_hp 地址跨进程持久.
#
# 输入:
# anchors.json 里的 self_hp_addr (堆地址, 跨进程必失效)
# 输出:
# anchors.json 新增字段 pointer_chain:
# {
# "module": "<main_module>",
# "module_base_when_found": "0x7ff6...",
# "static_offset": "0x...",   # 静态段内的偏移
# "deref_offsets": ["0x10", "0x40", "0x20"],
# "final_offset": "0x20",     # struct 内 hp 字段偏移
# "max_depth": 3
# }
#
# 回溯算法 (BFS, 限深):
# L0: anchor_set = {self_hp_addr - δ for δ in [0, 8, 16, ...]}  (目标对象起点附近)
# L1: 全堆扫所有 i64 == anchor_set, 得到 ptr_set
# - 命中模块静态段 → 链路成立, 终止
# L2: 全堆扫 ptr_set, 得到 ptr2_set, 同上
# L3: 同上 (默认止步 3)
#
# resolve_chain():
# 给定 chain, 用当前模块基址走一遍, 还原 final_addr; 与 reader 对接。
#
# CLI:
# python -m tools.mem_probe.pointer_chain backtrace [--depth 3]
# python -m tools.mem_probe.pointer_chain resolve
# python -m tools.mem_probe.pointer_chain verify    # 跑完跟 anchor 对一遍

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple

_SAO_AUTO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _SAO_AUTO_ROOT not in sys.path:
    sys.path.insert(0, _SAO_AUTO_ROOT)

from .process import (
    GameProcess, GameProcessError, ModuleInfo,
    is_admin,
)
from mem_probe import cy_memscan as _cy

DEFAULT_ANCHORS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "anchors.json")


def _get_preferred_modules() -> tuple:
    try:
        from config import POINTER_CHAIN_PREFERRED_MODULES
        return tuple(m.lower() for m in POINTER_CHAIN_PREFERRED_MODULES)
    except Exception:
        return ()

# 回溯参数
_OBJECT_BACK_DELTAS = (
    0, 0x8, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40,
    0x48, 0x50, 0x60, 0x70, 0x80, 0xA0, 0xC0, 0xE0, 0x100,
)
_MAX_PTR_PER_LEVEL = 8192   # 每层指针候选上限
_MAX_REGION_SIZE = 256 * 1024 * 1024
_DEREF_OFFSET_RANGE = (0, 0x800)  # ptr2 - ptr1 的允许范围 (deref 偏移)
_STATIC_SECTION_RANGE_FRAC = 1.0   # 模块整段都算静态可用


# ───────────────────────── helpers ─────────────────────────
def _load_anchors(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _save_anchors(path: str, data: dict) -> None:
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)


def _module_index(mods: List[ModuleInfo]) -> List[Tuple[int, int, str]]:
    # (base, base+size, name) 升序, 用于二分判定地址在哪个模块.
    out = sorted(((m.base, m.base + m.size, m.name) for m in mods), key=lambda x: x[0])
    return out


def _addr_in_module(addr: int, idx: List[Tuple[int, int, str]]) -> Optional[Tuple[str, int]]:
    # 返回 (module_name, offset_in_module) 或 None.
    import bisect
    bases = [b for b, _, _ in idx]
    i = bisect.bisect_right(bases, addr) - 1
    if i < 0:
        return None
    base, end, name = idx[i]
    if base <= addr < end:
        return name, addr - base
    return None


def _scan_pointers_to(
    pm: GameProcess, targets: Set[int], *, max_hits_per_level: int = _MAX_PTR_PER_LEVEL,
    include_image: bool = True,
) -> Dict[int, int]:
    # 全堆 + 模块静态段扫: 8 字节对齐 i64, 值落在 targets 中则记录.
    #
    # include_image=True 是关键 — 模块 .data/.bss 在 MEM_IMAGE 段, 排除掉就找不到静态指针。
    # 返回 dict: pointer_address -> pointed_to_address
    #
    # 内层走 cy_memscan.find_aligned_u64_in_set (AVX2 hash-set scan).
    if not targets:
        return {}
    targets_list = [int(t) & 0xFFFFFFFFFFFFFFFF for t in targets]
    found: Dict[int, int] = {}
    region_count = 0
    image_count = 0
    scanned_bytes = 0
    t0 = time.time()
    # 第一轮: 私有堆 (对象-对象引用)
    # 第二轮: 镜像段 (静态指针)
    passes = [(True, False)]  # (only_private, only_image)
    if include_image:
        passes.append((False, True))
    for only_private, only_image in passes:
        if only_image:
            iterator = pm.iter_regions(only_readable=True, only_private=False)
        else:
            iterator = pm.iter_regions(only_readable=True, only_private=True)
        for region in iterator:
            if only_image and not (region.type_ & 0x1000000):  # MEM_IMAGE
                continue
            if only_private and (region.type_ & 0x1000000):
                continue
            if region.size > _MAX_REGION_SIZE:
                continue
            region_count += 1
            if only_image:
                image_count += 1
            chunk_size = 16 * 1024 * 1024
            offset_in_region = 0
            while offset_in_region < region.size:
                n = min(chunk_size, region.size - offset_in_region)
                blob = pm.read_bytes(region.base + offset_in_region, n)
                if blob is None:
                    break
                scanned_bytes += len(blob)
                remaining = max_hits_per_level - len(found)
                if remaining <= 0:
                    break
                # cy_memscan 一次扫所有目标, 返回 [(off_in_blob, matched_value)]
                hits = _cy.find_aligned_u64_in_set(
                    blob, targets_list, max_hits=remaining,
                )
                for off, v in hits:
                    found[region.base + offset_in_region + off] = v
                if len(found) >= max_hits_per_level:
                    break
                offset_in_region += n
            if len(found) >= max_hits_per_level:
                break
        if len(found) >= max_hits_per_level:
            break
    print(f"   [scan-ptr] regions={region_count} (image={image_count}) "
          f"scanned≈{scanned_bytes/1e9:.2f}GB ({time.time()-t0:.1f}s) "
          f"-> {len(found)} pointer hits")
    return found


# ───────────────────────── 回溯主流程 ─────────────────────────
@dataclass
class PointerLevel:
    pointers: Dict[int, int] = field(default_factory=dict)  # ptr_addr -> pointed_to
    static_hits: List[Tuple[int, str, int, int]] = field(default_factory=list)
    # static_hits = [(ptr_addr, module_name, module_offset, pointed_to)]


def backtrace(
    pm: GameProcess,
    anchor_addr: int,
    *,
    max_depth: int = 3,
) -> Optional[Dict]:
    # 从 anchor_addr 回溯, 返回找到的最短链 (深度优先打分: 浅 > 深).
    mods = pm.list_modules()
    mod_idx = _module_index(mods)
    print(f"[backtrace] modules={len(mods)}, anchor=0x{anchor_addr:X}")

    # L0: 起点候选 = anchor 周围若干 obj-base 偏移
    initial_targets: Set[int] = {anchor_addr - d for d in _OBJECT_BACK_DELTAS}
    level_targets = initial_targets
    levels: List[PointerLevel] = []

    for depth in range(1, max_depth + 1):
        print(f"\n[backtrace] L{depth}: scanning {len(level_targets)} target addrs ...")
        ptrs = _scan_pointers_to(pm, level_targets)
        lvl = PointerLevel(pointers=ptrs)
        # 检查命中静态段
        for p_addr, pointed in ptrs.items():
            hit = _addr_in_module(p_addr, mod_idx)
            if hit:
                lvl.static_hits.append((p_addr, hit[0], hit[1], pointed))
        levels.append(lvl)
        print(f"   pointer hits: {len(ptrs)}, static hits: {len(lvl.static_hits)}")
        # 打印每个非静态命中, 帮助分析数据结构 (常见: List<T>._items 数组里的对象引用)
        if ptrs and not lvl.static_hits:
            shown = 0
            for p_addr, pointed in sorted(ptrs.items()):
                hint = _addr_in_module(p_addr, mod_idx)
                where = f"[{hint[0]}+0x{hint[1]:X}]" if hint else "(heap)"
                print(f"      ptr@0x{p_addr:016X} -> 0x{pointed:016X}  {where}")
                shown += 1
                if shown >= 20:
                    print(f"      ... +{len(ptrs)-shown} more")
                    break
        if lvl.static_hits:
            preferred_priority = _get_preferred_modules()
            def rank(item):
                _p_addr, mod, _off, _pt = item
                low = mod.lower()
                for i, p in enumerate(preferred_priority):
                    if low == p:
                        return i
                return len(preferred_priority)
            lvl.static_hits.sort(key=rank)
            chosen = lvl.static_hits[0]
            return _build_chain(pm, anchor_addr, levels, chosen)
        if not ptrs:
            print(f"[backtrace] L{depth}: 0 指针候选, 终止")
            return None
        # 下一轮: 不仅取 L_n 的 ptr_addr 作为目标, 还要展开 ptr_addr ± obj-base δ
        # 因为 L_n 的 ptr 常常在 List<T>._items 数组的某个 element slot 上,
        # 真正"被指"的是数组 base (slot - element_index*8)。展开多个候选位置覆盖。
        next_targets: Set[int] = set()
        for p in ptrs.keys():
            for d in _OBJECT_BACK_DELTAS:
                next_targets.add(p - d)
        # 同时也把 ptr 地址自己加入 (兜底: 如果有人直接持有 slot 指针)
        next_targets.update(ptrs.keys())
        if len(next_targets) > _MAX_PTR_PER_LEVEL:
            next_targets = set(list(next_targets)[: _MAX_PTR_PER_LEVEL])
        level_targets = next_targets

    print(f"[backtrace] 走完 {max_depth} 层, 未命中静态段")
    return None


def _build_chain(
    pm: GameProcess,
    final_anchor: int,
    levels: List[PointerLevel],
    static_hit: Tuple[int, str, int, int],
) -> Dict:
    # 从静态命中向前 (向 anchor 方向) 反推 deref 偏移序列.
    static_ptr_addr, mod_name, mod_offset, _ = static_hit
    # 从 levels[-1].static_hits 选中 static_ptr_addr 这个起点; 它指向 levels[-1].pointers[static_ptr_addr]
    chain_addrs: List[int] = []  # [ptr1_addr, ptr2_addr, ..., obj_addr, final_anchor]
    cur = static_ptr_addr
    chain_addrs.append(cur)
    for lvl in levels:
        pointed = lvl.pointers.get(cur)
        if pointed is None:
            break
        chain_addrs.append(pointed)
        cur = pointed
    # 最后一段: 末端是 obj_base, anchor (self_hp) 与 obj_base 的差
    obj_base = chain_addrs[-1]
    final_offset = final_anchor - obj_base

    # deref_offsets: 每一步从 *(ptr) 之后再加多少
    # chain_addrs[0] = static_ptr_addr
    # chain_addrs[1] = *(static_ptr_addr) = obj_base_or_intermediate
    # 实际上 deref 链是: walk(static_ptr_addr) → A; A + 0 = chain_addrs[1]
    # 中间层如果有偏移 (我们 _OBJECT_BACK_DELTAS 引入的), 在 _build_chain 末尾合入 final_offset
    deref_offsets: List[int] = []
    for i in range(1, len(chain_addrs) - 1):
        # 中间 deref 之间通常 offset = 0 (本回溯算法没考虑非 0 中间偏移)
        deref_offsets.append(0)

    mods_now = pm.list_modules()
    mod_obj = next((m for m in mods_now if m.name.lower() == mod_name.lower()), None)
    if mod_obj is None:
        return {}
    chain = {
        "module": mod_obj.name,
        "module_base_when_found": hex(mod_obj.base),
        "static_offset": hex(mod_offset),
        "deref_offsets": [hex(x) for x in deref_offsets],
        "final_offset": hex(final_offset),
        "depth": len(deref_offsets) + 1,
        "anchor_at_build": hex(final_anchor),
        "obj_base_at_build": hex(obj_base),
        "built_at": time.time(),
    }
    return chain


# ───────────────────────── 解析 / 验证 ─────────────────────────
def resolve_chain(pm: GameProcess, chain: Dict) -> Optional[int]:
    # 用当前进程的模块基址走一遍 chain, 返回最终地址 (即 self_hp 的当前地址).
    mod_name = chain["module"]
    mods = pm.list_modules()
    mod = next((m for m in mods if m.name.lower() == mod_name.lower()), None)
    if mod is None:
        print(f"[resolve] 当前进程未加载 {mod_name}")
        return None
    static_off = int(chain["static_offset"], 16)
    deref_offs = [int(x, 16) for x in chain.get("deref_offsets", [])]
    final_off = int(chain["final_offset"], 16)

    cur = mod.base + static_off
    # 每一层: 读 *(cur), 然后 cur = *(cur) + offset
    for off in deref_offs:
        v = pm.read_i64(cur)
        if v is None:
            print(f"[resolve] read_i64(0x{cur:X}) failed")
            return None
        cur = v + off
    # 末层: 读 *(cur), 加 final_offset
    v = pm.read_i64(cur)
    if v is None:
        print(f"[resolve] final read_i64(0x{cur:X}) failed")
        return None
    return v + final_off


# ───────────────────────── CLI ─────────────────────────
def cmd_backtrace(args) -> int:
    if not is_admin():
        print("[warn] 非管理员", file=sys.stderr)
    if not os.path.isfile(args.anchors):
        print(f"[fail] 找不到 {args.anchors}; 请先跑 refine", file=sys.stderr)
        return 1
    anchors = _load_anchors(args.anchors)
    hp_addr_s = anchors.get("self_hp_addr")
    if not hp_addr_s:
        print("[fail] anchors 中无 self_hp_addr", file=sys.stderr)
        return 1
    anchor_addr = int(hp_addr_s, 16)
    try:
        pm = GameProcess()
    except GameProcessError as e:
        print(f"[fail] {e}", file=sys.stderr)
        return 2
    try:
        chain = backtrace(pm, anchor_addr, max_depth=args.depth)
        if not chain:
            print("\n[result] 未找到指针链")
            return 3
        print("\n=== POINTER CHAIN FOUND ===")
        print(json.dumps(chain, indent=2, ensure_ascii=False))
        # 立刻验证一下
        resolved = resolve_chain(pm, chain)
        if resolved == anchor_addr:
            print(f"\n[verify] resolve == anchor (0x{anchor_addr:X})  ✅")
        else:
            print(f"\n[verify] resolve=0x{resolved:X} != anchor=0x{anchor_addr:X}  ⚠️")
        anchors["pointer_chain"] = chain
        _save_anchors(args.anchors, anchors)
        print(f"[ok] saved to {args.anchors}")
        return 0
    finally:
        pm.close()


def cmd_resolve(args) -> int:
    anchors = _load_anchors(args.anchors)
    chain = anchors.get("pointer_chain")
    if not chain:
        print("[fail] anchors 中无 pointer_chain; 先跑 backtrace", file=sys.stderr)
        return 1
    try:
        pm = GameProcess()
    except GameProcessError as e:
        print(f"[fail] {e}", file=sys.stderr)
        return 2
    try:
        addr = resolve_chain(pm, chain)
        if addr is None:
            return 3
        print(f"[resolve] self_hp = 0x{addr:X}")
        # 顺便读一下当前值
        hp = pm.read_i32(addr)
        print(f"[read] *self_hp = {hp}")
        return 0
    finally:
        pm.close()


def cmd_verify(args) -> int:
    # 对比静态链解析的地址 与 当前 anchors.self_hp_addr 是否一致.
    anchors = _load_anchors(args.anchors)
    chain = anchors.get("pointer_chain")
    cur_anchor_s = anchors.get("self_hp_addr")
    if not chain or not cur_anchor_s:
        print("[fail] 缺 pointer_chain 或 self_hp_addr")
        return 1
    cur_anchor = int(cur_anchor_s, 16)
    try:
        pm = GameProcess()
    except GameProcessError as e:
        print(f"[fail] {e}", file=sys.stderr)
        return 2
    try:
        resolved = resolve_chain(pm, chain)
        if resolved is None:
            return 3
        ok = resolved == cur_anchor
        marker = "✅" if ok else "❌"
        print(f"[verify] resolve=0x{resolved:X}  anchor=0x{cur_anchor:X}  {marker}")
        if ok:
            hp = pm.read_i32(resolved)
            print(f"[read] HP = {hp}")
        return 0 if ok else 4
    finally:
        pm.close()


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="python -m tools.mem_probe.pointer_chain")
    p.add_argument("--anchors", default=DEFAULT_ANCHORS)
    sub = p.add_subparsers(dest="cmd", required=True)
    pb = sub.add_parser("backtrace", help="从 self_hp_addr 反推静态指针链")
    pb.add_argument("--depth", type=int, default=3)
    pb.set_defaults(func=cmd_backtrace)
    pr = sub.add_parser("resolve", help="用 chain 算出当前 self_hp 地址并读取")
    pr.set_defaults(func=cmd_resolve)
    pv = sub.add_parser("verify", help="链解析地址 vs anchors.self_hp_addr")
    pv.set_defaults(func=cmd_verify)
    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
