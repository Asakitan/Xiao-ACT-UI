"""模块相对指纹 v2 — 使用模块内指针 (typeinfo / vtable) 作为稳定锚.

核心洞察:
    Player struct (实为 Attribute 列表) 包含若干指向 GameAssembly.dll 静态段的
    typeinfo / vtable 指针。这些指针每次启动都因 ASLR 而变, 但相对模块基址的
    偏移是稳定的。

    用模块名 + 偏移 描述这些"指针槽", locate 时用当前模块基址重建预期字节,
    再做掩码扫描。

数据格式 (anchors.json: fingerprint_v2):
    {
        "size": 128,
        "before": 64,
        "delta_max_hp": -32,
        "fixed": [{"off": 8, "bytes": "1300000000000000"}, ...],  # 8 字节对齐, 必须等于
        "module_rel": [
            {"off": 16, "module": "GameAssembly.dll", "module_offset": 0x...},
            ...
        ],
        # 其余字节不参与匹配 (HP 值 / 时变状态)
    }

CLI:
    python -m tools.mem_probe.fingerprint_v2 capture
    python -m tools.mem_probe.fingerprint_v2 locate
"""

from __future__ import annotations

import argparse
import bisect
import json
import os
import sys
import time
from typing import Dict, List, Optional, Tuple

_SAO_AUTO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _SAO_AUTO_ROOT not in sys.path:
    sys.path.insert(0, _SAO_AUTO_ROOT)

from .process import StarProcess, StarProcessError, ModuleInfo, is_admin
from mem_probe import cy_memscan as _cy

DEFAULT_ANCHORS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "anchors.json")
_FP_BEFORE = 0x40  # HP 之前 64 字节
_FP_AFTER = 0x40   # HP 之后 64 字节
_FP_TOTAL = _FP_BEFORE + _FP_AFTER
_MAX_REGION_SIZE = 256 * 1024 * 1024
# 优先用以下模块作为指纹锚 (大且包含游戏代码 typeinfo)
_PREFERRED_MODULES = ("gameassembly.dll", "unityplayer.dll", "starbase.dll", "star.exe")


def _load_anchors(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _save_anchors(path: str, data: dict) -> None:
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)


def _build_module_index(mods: List[ModuleInfo]) -> List[Tuple[int, int, str]]:
    return sorted(((m.base, m.base + m.size, m.name) for m in mods), key=lambda x: x[0])


def _addr_in_module(
    addr: int, idx: List[Tuple[int, int, str]]
) -> Optional[Tuple[str, int]]:
    bases = [b for b, _, _ in idx]
    i = bisect.bisect_right(bases, addr) - 1
    if i < 0:
        return None
    base, end, name = idx[i]
    if base <= addr < end:
        return name, addr - base
    return None


def _mod_base(mods: List[ModuleInfo], name: str) -> Optional[int]:
    low = name.lower()
    for m in mods:
        if m.name.lower() == low:
            return m.base
    return None


# ───────────────────────── capture ─────────────────────────
def capture_v2(pm: StarProcess, anchors: dict, *, mask_value_offsets: List[int]) -> Optional[dict]:
    """返回 fingerprint_v2 dict.

    mask_value_offsets: 在 fingerprint 中"会变的字段"的偏移 (i32 起始位置), 例如 HP/MaxHP。
    """
    hp_addr = int(anchors["self_hp_addr"], 16)
    max_hp_addr = int(anchors["self_max_hp_addr"], 16)
    delta_max = max_hp_addr - hp_addr
    fp_base = hp_addr - _FP_BEFORE
    blob = pm.read_bytes(fp_base, _FP_TOTAL)
    if blob is None:
        print(f"[fail] 读 0x{fp_base:X} 失败")
        return None

    mods = pm.list_modules()
    mod_idx = _build_module_index(mods)

    # 8 字节对齐扫指针槽
    fixed: List[dict] = []     # 完全固定的 8 字节 (用 0/小常量等)
    module_rel: List[dict] = []  # 指向模块的指针 → (module, offset)
    masked_8b_offsets: set = set()  # 哪些 8 字节槽是变化字段
    for vo in mask_value_offsets:
        masked_8b_offsets.add(vo & ~7)

    for i in range(0, _FP_TOTAL, 8):
        if i + 8 > _FP_TOTAL:
            break
        if i in masked_8b_offsets:
            continue  # 这个 8 字节槽含 HP/MaxHP 等变化字段, 跳过
        slot = blob[i : i + 8]
        v = int.from_bytes(slot, "little")
        if v == 0:
            # 0 也是稳定的 (但太常见, 用 fixed 记录还是值得)
            fixed.append({"off": i, "bytes": slot.hex()})
            continue
        # 是否落在已加载模块?
        hit = _addr_in_module(v, mod_idx)
        if hit:
            mod, mod_off = hit
            module_rel.append({"off": i, "module": mod, "module_offset": mod_off})
            continue
        # 落在 0x100000000~0x7FFF... 但不在模块内 → 堆指针, 跨进程必变, 屏蔽
        if 0x100000000 <= v < 0x7FFFFFFFFFFF:
            continue
        # 小整数 / 标志位 → 稳定
        fixed.append({"off": i, "bytes": slot.hex()})

    fp = {
        "size": _FP_TOTAL,
        "before": _FP_BEFORE,
        "delta_max_hp": delta_max,
        "fixed": fixed,
        "module_rel": module_rel,
        "captured_at": time.time(),
        "captured_pid": pm.pid,
    }
    print(f"[fp v2] fixed slots: {len(fixed)}, module-relative slots: {len(module_rel)}")
    for mr in module_rel:
        print(f"   [{mr['off']:+#04x}] {mr['module']}+0x{mr['module_offset']:X}")
    return fp


# ───────────────────────── locate ─────────────────────────
def locate_v2(pm: StarProcess, fp: dict) -> List[int]:
    """全堆扫指纹 v2, 返回所有匹配的 HP 地址."""
    mods = pm.list_modules()
    size = fp["size"]
    before = fp["before"]
    fixed = fp["fixed"]
    module_rel = fp["module_rel"]

    # 重建预期: 各 module-relative 槽 → 当前 (module_base + offset) 的 8 字节
    expected: List[Tuple[int, bytes]] = []  # (offset_in_fp, expected_8_bytes)
    for f in fixed:
        expected.append((f["off"], bytes.fromhex(f["bytes"])))
    for mr in module_rel:
        base = _mod_base(mods, mr["module"])
        if base is None:
            print(f"[warn] 模块未加载: {mr['module']}; 跳过此槽")
            continue
        addr = base + mr["module_offset"]
        expected.append((mr["off"], addr.to_bytes(8, "little")))

    if not expected:
        print("[fail] 没有任何可匹配槽")
        return []
    expected.sort()
    print(f"[locate v2] 匹配 {len(expected)} 个 8 字节槽")

    # 选最特异的槽做粗筛锚 (优先选 module-relative 的, 它们最独特)
    # 找出非 0、非全 0xFF 的槽
    anchor_slot = None
    for off, eb in expected:
        v = int.from_bytes(eb, "little")
        if v not in (0, 0xFFFFFFFFFFFFFFFF):
            anchor_slot = (off, eb)
            break
    if anchor_slot is None:
        # 退化: 用第一个
        anchor_slot = expected[0]
    a_off, a_bytes = anchor_slot
    print(f"[locate v2] 锚槽: offset {a_off:+#04x}, value {a_bytes.hex()}")

    # Convert expected list to (off_in_fp, u64_value) tuples for cy kernel
    expected_slots: List[tuple] = []
    for off_e, eb_e in expected:
        if len(eb_e) != 8:
            # Non-8-byte slot: skip (cy kernel only handles aligned u64 slots).
            # In practice all v2 slots are 8 bytes by construction.
            continue
        expected_slots.append((off_e, int.from_bytes(eb_e, "little")))
    a_value = int.from_bytes(a_bytes, "little")

    matches: List[int] = []
    region_count = 0
    scanned = 0
    t0 = time.time()
    for region in pm.iter_regions(only_readable=True, only_private=True):
        if region.size > _MAX_REGION_SIZE:
            continue
        region_count += 1
        chunk_size = 16 * 1024 * 1024
        offset = 0
        while offset < region.size:
            n = min(chunk_size, region.size - offset)
            blob = pm.read_bytes(region.base + offset, n)
            if blob is None:
                break
            scanned += len(blob)
            remaining = 100 - len(matches) + 1
            if remaining <= 0:
                break
            # cy_memscan: AVX2 anchor scan + slot verify in one call.
            # Returns fp_base offsets within blob.
            fp_bases = _cy.find_aligned_u64_with_anchor(
                blob, a_value, a_off, size, expected_slots, max_hits=remaining,
            )
            for fp_base in fp_bases:
                hp_addr = region.base + offset + fp_base + before
                matches.append(hp_addr)
            if len(matches) > 100:
                break
            # chunk 重叠
            if offset + n < region.size:
                offset += n - (size - 1)
            else:
                offset += n
        if len(matches) > 100:
            break
    print(f"[locate v2] regions={region_count} scanned≈{scanned/1e9:.2f}GB "
          f"({time.time()-t0:.1f}s) -> {len(matches)} match(es)")
    return matches


# ───────────────────────── CLI ─────────────────────────
def cmd_capture(args) -> int:
    if not os.path.isfile(args.anchors):
        print(f"[fail] 找不到 {args.anchors}", file=sys.stderr)
        return 1
    anchors = _load_anchors(args.anchors)
    if not anchors.get("self_hp_addr"):
        print("[fail] anchors 中无 self_hp_addr (先跑 refine)", file=sys.stderr)
        return 1
    try:
        pm = StarProcess()
    except StarProcessError as e:
        print(f"[fail] {e}", file=sys.stderr)
        return 2
    try:
        # HP/MaxHP 的 4 字节起始位置 (转为 8 字节槽偏移会被屏蔽)
        delta_max = int(anchors["self_max_hp_addr"], 16) - int(anchors["self_hp_addr"], 16)
        mask_offs = [_FP_BEFORE, _FP_BEFORE + delta_max]
        fp = capture_v2(pm, anchors, mask_value_offsets=mask_offs)
        if fp is None:
            return 3
        anchors["fingerprint_v2"] = fp
        _save_anchors(args.anchors, anchors)
        print(f"[ok] fingerprint_v2 saved")
        # 自检
        matches = locate_v2(pm, fp)
        print(f"[selftest] {len(matches)} match(es) in current process")
        for m in matches[:5]:
            print(f"   hp@0x{m:X} = {pm.read_i32(m)}")
        return 0
    finally:
        pm.close()


def cmd_locate(args) -> int:
    anchors = _load_anchors(args.anchors)
    fp = anchors.get("fingerprint_v2")
    if not fp:
        print("[fail] 无 fingerprint_v2; 先跑 capture", file=sys.stderr)
        return 1
    try:
        pm = StarProcess()
    except StarProcessError as e:
        print(f"[fail] {e}", file=sys.stderr)
        return 2
    try:
        matches = locate_v2(pm, fp)
        if not matches:
            print("[result] 0 match")
            return 3
        for m in matches[:10]:
            v = pm.read_i32(m)
            mh_addr = m + fp["delta_max_hp"]
            mh = pm.read_i32(mh_addr)
            print(f"hp@0x{m:X} = {v}   max_hp@0x{mh_addr:X} = {mh}")
        if len(matches) == 1:
            anchors["self_hp_addr"] = hex(matches[0])
            anchors["self_max_hp_addr"] = hex(matches[0] + fp["delta_max_hp"])
            anchors["fingerprint_v2_relocate_ts"] = time.time()
            _save_anchors(args.anchors, anchors)
            print(f"[ok] anchors 已更新")
        elif len(matches) > 1:
            print(f"[warn] {len(matches)} 处匹配 — 选 HP 最像的:")
            # 简单启发: HP 应在合理范围 (1 ~ 10_000_000)
            valid = [m for m in matches if 1 <= (pm.read_i32(m) or 0) <= 10_000_000]
            print(f"   {len(valid)} 个 HP 值合理")
            if len(valid) == 1:
                anchors["self_hp_addr"] = hex(valid[0])
                anchors["self_max_hp_addr"] = hex(valid[0] + fp["delta_max_hp"])
                anchors["fingerprint_v2_relocate_ts"] = time.time()
                _save_anchors(args.anchors, anchors)
                print(f"[ok] anchors 已更新到 0x{valid[0]:X}")
        return 0
    finally:
        pm.close()


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="python -m tools.mem_probe.fingerprint_v2")
    p.add_argument("--anchors", default=DEFAULT_ANCHORS)
    sub = p.add_subparsers(dest="cmd", required=True)
    pc = sub.add_parser("capture")
    pc.set_defaults(func=cmd_capture)
    pl = sub.add_parser("locate")
    pl.set_defaults(func=cmd_locate)
    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
