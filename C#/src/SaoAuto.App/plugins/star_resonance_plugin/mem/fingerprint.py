"""指纹定位 — 替代指针链的 Plan B 方案.

思路:
    1. refine 完成时, 读 self_hp_addr 周围 128 字节作为"指纹" (typical: klass ptr +
       Mono header + 邻接字段值)。同时记录 hp 字段在指纹中的偏移。
    2. 指纹中 HP/MaxHP 字段的位置用 0xCC 屏蔽 (它们会变), 其它字节相对稳定。
    3. 下次启动时, 用 pymem.pattern.pattern_scan_module 风格的全堆掩码扫描, 找到
       唯一匹配的 128 字节区, 加上偏移就是新的 self_hp 地址。

这比指针链对 IL2CPP / 高度托管的游戏更鲁棒, 因为:
    - 不依赖游戏代码静态指针
    - GC 移动对象时整个 fingerprint 跟着搬, 模式不变
    - 误匹配率低 (128 字节里有 klass ptr + 字段标记 + 浮点常量等, 非常独特)

CLI:
    python -m tools.mem_probe.fingerprint capture
    python -m tools.mem_probe.fingerprint locate    # 当前进程实测
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from typing import List, Optional, Tuple

_SAO_AUTO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _SAO_AUTO_ROOT not in sys.path:
    sys.path.insert(0, _SAO_AUTO_ROOT)

from .process import StarProcess, StarProcessError, is_admin
from mem_probe import cy_memscan as _cy

DEFAULT_ANCHORS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "anchors.json")
_FP_BEFORE = 0x40   # HP 之前 64 字节
_FP_AFTER = 0x40    # HP 之后 64 字节
_FP_TOTAL = _FP_BEFORE + _FP_AFTER  # 128
_MAX_REGION_SIZE = 256 * 1024 * 1024


def _load_anchors(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _save_anchors(path: str, data: dict) -> None:
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)


def capture(pm: StarProcess, anchors: dict) -> Optional[dict]:
    """从当前 anchors 抓取指纹, 返回 dict (调用方负责写回)."""
    hp_addr = int(anchors["self_hp_addr"], 16)
    max_hp_addr = int(anchors["self_max_hp_addr"], 16)
    delta_max = max_hp_addr - hp_addr  # 例如 -0x20
    fp_base = hp_addr - _FP_BEFORE
    blob = pm.read_bytes(fp_base, _FP_TOTAL)
    if blob is None:
        print(f"[fail] 读 fp_base=0x{fp_base:X} 失败")
        return None

    # 屏蔽 HP/MaxHP 的 4 字节 (它们会变, 不稳定)
    mask = bytearray(b"\xff" * _FP_TOTAL)
    hp_off = _FP_BEFORE  # HP 在指纹中的偏移
    max_hp_off = _FP_BEFORE + delta_max
    for off in (hp_off, max_hp_off):
        if 0 <= off <= _FP_TOTAL - 4:
            for k in range(4):
                mask[off + k] = 0x00

    # 此外, 屏蔽明显的指针字段 (高 5 字节非 0 且 < 0x7F00..) — 跨进程会变
    # 简单策略: 8 字节对齐的位置, 若 i64 落在 0x100000000~0x7FFFFFFFFFFF 范围, 屏蔽
    blob_b = bytearray(blob)
    for i in range(0, _FP_TOTAL - 8 + 1, 8):
        v = int.from_bytes(blob_b[i : i + 8], "little")
        if 0x100000000 <= v < 0x7FFFFFFFFFFF:
            for k in range(8):
                mask[i + k] = 0x00

    fp = {
        "fp_base_offset": -_FP_BEFORE,  # fp_base = hp_addr + this
        "hp_offset_in_fp": hp_off,
        "max_hp_offset_in_fp": max_hp_off,
        "delta_max_hp": delta_max,
        "size": _FP_TOTAL,
        "pattern_hex": blob_b.hex(),
        "mask_hex": bytes(mask).hex(),
        "captured_at": time.time(),
        "captured_pid": pm.pid,
    }
    # 统计有效字节
    fixed = sum(1 for b in mask if b == 0xFF)
    print(f"[fp] captured {_FP_TOTAL} bytes, {fixed} 字节固定 ({_FP_TOTAL-fixed} 字节屏蔽)")
    return fp


def _masked_search(blob: bytes, pattern: bytes, mask: bytes) -> List[int]:
    """在 blob 中找所有 pattern (按 mask 比对) 的偏移.

    内层走 cy_memscan.find_pattern_masked (memchr 锚字节 + 掩码逐字节验证).
    """
    return _cy.find_pattern_masked(blob, pattern, mask)


def locate(pm: StarProcess, fp: dict) -> List[int]:
    """全堆扫指纹, 返回所有匹配的 HP 地址."""
    pattern = bytes.fromhex(fp["pattern_hex"])
    mask = bytes.fromhex(fp["mask_hex"])
    fp_base_off = fp["fp_base_offset"]
    hp_off = fp["hp_offset_in_fp"]
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
            hits = _masked_search(blob, pattern, mask)
            for h in hits:
                fp_base_addr = region.base + offset + h
                hp_addr = fp_base_addr - fp_base_off  # = fp_base + _FP_BEFORE
                # 精确换算: hp_addr = fp_base + hp_off
                hp_addr = fp_base_addr + hp_off
                matches.append(hp_addr)
                if len(matches) > 100:
                    break
            if len(matches) > 100:
                break
            # 重叠 chunk: 后退 plen-1 字节, 避免跨 chunk 模式被切断
            if offset + n < region.size:
                offset += n - (len(pattern) - 1)
            else:
                offset += n
        if len(matches) > 100:
            break
    print(f"[locate] regions={region_count} scanned≈{scanned/1e9:.2f}GB "
          f"({time.time()-t0:.1f}s) -> {len(matches)} match(es)")
    return matches


# ───────────────────────── CLI ─────────────────────────
def cmd_capture(args) -> int:
    if not os.path.isfile(args.anchors):
        print(f"[fail] 找不到 {args.anchors}; 先跑 refine", file=sys.stderr)
        return 1
    anchors = _load_anchors(args.anchors)
    if not anchors.get("self_hp_addr"):
        print("[fail] anchors 中无 self_hp_addr", file=sys.stderr)
        return 1
    try:
        pm = StarProcess()
    except StarProcessError as e:
        print(f"[fail] {e}", file=sys.stderr)
        return 2
    try:
        fp = capture(pm, anchors)
        if fp is None:
            return 3
        anchors["fingerprint"] = fp
        _save_anchors(args.anchors, anchors)
        print(f"[ok] fingerprint saved to {args.anchors}")
        # 立刻自检
        matches = locate(pm, fp)
        print(f"[selftest] {len(matches)} match(es) in current process")
        for m in matches[:5]:
            v = pm.read_i32(m)
            print(f"   hp@0x{m:X} = {v}")
        return 0
    finally:
        pm.close()


def cmd_locate(args) -> int:
    anchors = _load_anchors(args.anchors)
    fp = anchors.get("fingerprint")
    if not fp:
        print("[fail] anchors 中无 fingerprint; 先跑 capture", file=sys.stderr)
        return 1
    try:
        pm = StarProcess()
    except StarProcessError as e:
        print(f"[fail] {e}", file=sys.stderr)
        return 2
    try:
        matches = locate(pm, fp)
        if not matches:
            print("[result] 0 match — 指纹失效 (游戏版本/账号变更?)")
            return 3
        for m in matches:
            v = pm.read_i32(m)
            mh_off = fp["delta_max_hp"]
            mh_addr = m + mh_off
            mh = pm.read_i32(mh_addr)
            print(f"hp@0x{m:X} = {v}   max_hp@0x{mh_addr:X} = {mh}")
        if len(matches) == 1:
            anchors["self_hp_addr"] = hex(matches[0])
            anchors["self_max_hp_addr"] = hex(matches[0] + fp["delta_max_hp"])
            anchors["fingerprint_relocate_ts"] = time.time()
            _save_anchors(args.anchors, anchors)
            print(f"[ok] anchors 已更新到当前进程地址")
        return 0
    finally:
        pm.close()


def cmd_recapture(args) -> int:
    """diff 加固: 读 anchors.self_hp_addr 周围 128 字节, 与旧指纹 pattern diff,
    把不一致的字节 mask 掉, 得到跨进程稳定的指纹。"""
    anchors = _load_anchors(args.anchors)
    old_fp = anchors.get("fingerprint")
    if not old_fp:
        print("[fail] 没有旧指纹; 先跑 capture", file=sys.stderr)
        return 1
    if not anchors.get("self_hp_addr"):
        print("[fail] anchors 无 self_hp_addr; 重启后请先跑 refine", file=sys.stderr)
        return 1
    try:
        pm = StarProcess()
    except StarProcessError as e:
        print(f"[fail] {e}", file=sys.stderr)
        return 2
    try:
        old_pattern = bytes.fromhex(old_fp["pattern_hex"])
        old_mask = bytearray.fromhex(old_fp["mask_hex"])
        # 兼容性: 假设 _FP_BEFORE/AFTER 与旧指纹一致
        size = old_fp["size"]
        before = -old_fp["fp_base_offset"]
        hp_addr = int(anchors["self_hp_addr"], 16)
        new_pattern = pm.read_bytes(hp_addr - before, size)
        if new_pattern is None:
            print(f"[fail] 读 0x{hp_addr - before:X} 失败")
            return 3

        # diff: 任何字节不一致 → mask=0
        new_mask = bytearray(old_mask)
        diff_count = 0
        for i in range(size):
            if old_mask[i] == 0:
                continue  # 已经屏蔽
            if old_pattern[i] != new_pattern[i]:
                new_mask[i] = 0
                diff_count += 1
        fixed = sum(1 for b in new_mask if b == 0xFF)
        print(f"[recapture] diff: {diff_count} 字节差异, "
              f"加固后固定字节 = {fixed} (旧: {sum(1 for b in old_mask if b == 0xFF)})")

        # pattern 用新的 (字段值用当前进程的)
        new_fp = dict(old_fp)
        new_fp["pattern_hex"] = new_pattern.hex()
        new_fp["mask_hex"] = bytes(new_mask).hex()
        new_fp["recaptured_at"] = time.time()
        new_fp["recapture_pid"] = pm.pid
        new_fp["recapture_diff_count"] = diff_count

        if fixed < 16:
            print(f"[warn] 加固后仅 {fixed} 字节稳定, 容易误匹配; 不保存")
            return 4

        anchors["fingerprint"] = new_fp
        _save_anchors(args.anchors, anchors)
        print(f"[ok] fingerprint 已加固")

        # 立即自检
        matches = locate(pm, new_fp)
        print(f"[selftest] {len(matches)} match(es) in current process")
        for m in matches[:5]:
            print(f"   hp@0x{m:X} = {pm.read_i32(m)}")
        if len(matches) == 1 and matches[0] == hp_addr:
            print("[ok] 自检通过 (唯一匹配且地址正确)")
            return 0
        elif len(matches) > 1:
            print(f"[warn] {len(matches)} 处匹配 — 可能需要再来一次 recapture (不同进程)")
            return 5
        return 0
    finally:
        pm.close()


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="python -m tools.mem_probe.fingerprint")
    p.add_argument("--anchors", default=DEFAULT_ANCHORS)
    sub = p.add_subparsers(dest="cmd", required=True)
    pc = sub.add_parser("capture", help="抓取当前 self_hp 周围 128 字节做指纹")
    pc.set_defaults(func=cmd_capture)
    pl = sub.add_parser("locate", help="用指纹在当前进程中重新定位 self_hp")
    pl.set_defaults(func=cmd_locate)
    pr = sub.add_parser("recapture", help="diff 加固: 旧指纹 vs 当前进程, 屏蔽差异字节")
    pr.set_defaults(func=cmd_recapture)
    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
