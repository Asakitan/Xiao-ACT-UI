# Phase 0.1 — 从已知 self_hp_addr 反查所属 Il2CppClass + 字段偏移.
#
# 两种模式:
# A) --hp <addr>: 直接给 hp 地址 (anchors.json 还有效时用)
# B) --scan-hp <int> [--scan-max-hp <int>]: 给当前 HP 数值, 全堆扫 i32 找候选,
# 对每个候选反查 klass + 前后看 max_hp 是否相邻, 自动筛出唯一 obj.
# dev 可以从游戏 UI 上直接读出 HP/MaxHP 数值, 无需 TCP / 旧 anchors.
#
# 输出 JSON 到 il2cpp/discovery_notes.json + 人类可读摘要.

from __future__ import annotations

import argparse
import json
import os
import string
import sys
import time
from typing import Any, Dict, List, Optional, Tuple

from ..process import StarProcess, StarProcessError, MemoryRegion

# Il2CppClass v31 推测偏移
CLASS_NAME_OFF = 0x10
CLASS_NAMESPACE_OFF = 0x18

# 对象布局假设: 第一个 qword 通常是 Il2CppClass*
SEARCH_BACK_BYTES = 0x2000
SEARCH_FWD_BYTES = 0x100
STEP = 8

ANCHORS_DEFAULT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "anchors.json"
)
OUT_DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "discovery_notes.json")


def _is_printable_ascii(b: bytes) -> bool:
    if not b:
        return False
    ok = set(string.ascii_letters + string.digits + "_<>.,$`")
    return all(chr(c) in ok for c in b)


def _read_class_name(pm: StarProcess, klass_ptr: int) -> Optional[str]:
    name_ptr = pm.read_ptr(klass_ptr + CLASS_NAME_OFF)
    if not name_ptr:
        return None
    raw = pm.read_bytes(name_ptr, 128)
    if not raw:
        return None
    end = raw.find(b"\x00")
    if end <= 0:
        return None
    name = raw[:end]
    if not _is_printable_ascii(name):
        return None
    return name.decode("ascii", errors="replace")


def _read_namespace(pm: StarProcess, klass_ptr: int) -> Optional[str]:
    ns_ptr = pm.read_ptr(klass_ptr + CLASS_NAMESPACE_OFF)
    if not ns_ptr:
        return None
    raw = pm.read_bytes(ns_ptr, 256)
    if not raw:
        return ""
    end = raw.find(b"\x00")
    if end < 0:
        end = len(raw)
    if end == 0:
        return ""
    name = raw[:end]
    if not _is_printable_ascii(name):
        return None
    return name.decode("ascii", errors="replace")


def _ga_module(pm: StarProcess) -> Tuple[int, int]:
    # 返回 (GameAssembly.dll base, size).
    for m in pm.list_modules():
        if m.name.lower() == "gameassembly.dll":
            return m.base, m.size
    raise RuntimeError("未找到 GameAssembly.dll")


def scan_i32(pm: StarProcess, value: int, *, max_hits: int = 200_000) -> List[int]:
    # 全堆扫 i32 == value (cy_memscan AVX2). value<0 自动转 unsigned 同字节匹配.
    from mem_probe import cy_memscan as _cy
    needle_u = int(value) & 0xFFFFFFFF
    hits: List[int] = []
    for region in pm.iter_regions(only_readable=True, only_private=True):
        buf = pm.read_bytes(region.base, region.size)
        if buf is None:
            continue
        remaining = max_hits - len(hits)
        if remaining <= 0:
            return hits
        for o in _cy.find_aligned_u32(buf, needle_u, max_hits=remaining):
            hits.append(region.base + o)
        if len(hits) >= max_hits:
            return hits
    return hits


def scan_i64(pm: StarProcess, value: int, *, max_hits: int = 200_000) -> List[int]:
    # 全堆扫 i64 == value (cy_memscan AVX2), 返回 8-aligned 命中地址.
    from mem_probe import cy_memscan as _cy
    needle_u = int(value) & 0xFFFFFFFFFFFFFFFF
    hits: List[int] = []
    for region in pm.iter_regions(only_readable=True, only_private=True):
        buf = pm.read_bytes(region.base, region.size)
        if buf is None:
            continue
        remaining = max_hits - len(hits)
        if remaining <= 0:
            return hits
        for o in _cy.find_aligned_u64(buf, needle_u, max_hits=remaining):
            hits.append(region.base + o)
        if len(hits) >= max_hits:
            return hits
    return hits


def find_object_base(
    pm: StarProcess,
    field_addr: int,
    ga_base: int,
    ga_size: int,
    *,
    back: int = SEARCH_BACK_BYTES,
    fwd: int = SEARCH_FWD_BYTES,
    step: int = STEP,
) -> List[Dict[str, Any]]:
    # 在 field_addr 周围找疑似对象基址.
    #
    # 返回所有满足"首 qword 是合法 Il2CppClass*"的候选, 按距离 field_addr 排序.
    # 每个候选 = {"obj_base", "klass_ptr", "klass_name", "klass_namespace",
    # "field_offset", "distance"}
    ga_end = ga_base + ga_size
    out: List[Dict[str, Any]] = []
    seen_klass: set = set()

    # 先往低地址扫
    start = (field_addr - back) & ~(step - 1)
    for off in range(start, field_addr + fwd, step):
        klass_ptr = pm.read_ptr(off)
        if klass_ptr is None:
            continue
        # Il2CppClass* 必须在 GameAssembly.dll 模块的 .data/.rdata 段
        if klass_ptr < ga_base or klass_ptr >= ga_end:
            continue
        # 8-byte 对齐
        if klass_ptr & 0x7:
            continue
        if klass_ptr in seen_klass:
            continue
        # 验证: name_ptr 必须可读 + ASCII
        name = _read_class_name(pm, klass_ptr)
        if not name:
            continue
        seen_klass.add(klass_ptr)
        ns = _read_namespace(pm, klass_ptr)
        out.append({
            "obj_base": off,
            "klass_ptr": klass_ptr,
            "klass_name": name,
            "klass_namespace": ns or "",
            "field_offset": field_addr - off,
            "distance": abs(field_addr - off),
        })

    # 按距离排序; 优先距离 hp_addr 最近的
    out.sort(key=lambda d: (d["distance"], d["obj_base"]))
    return out


def discover_one(
    pm: StarProcess,
    label: str,
    field_addr: int,
    ga_base: int,
    ga_size: int,
) -> Dict[str, Any]:
    print(f"\n[{label}] field_addr=0x{field_addr:X}")
    val = pm.read_i32(field_addr)
    print(f"[{label}] current i32 value = {val}")
    candidates = find_object_base(pm, field_addr, ga_base, ga_size)
    print(f"[{label}] {len(candidates)} class candidates")
    for i, c in enumerate(candidates[:8]):
        ns = f"{c['klass_namespace']}." if c["klass_namespace"] else ""
        print(
            f"  #{i}  obj=0x{c['obj_base']:X}  klass=0x{c['klass_ptr']:X}"
            f"  +0x{c['field_offset']:03X}  {ns}{c['klass_name']}"
        )
    return {
        "label": label,
        "field_addr": hex(field_addr),
        "current_value": val,
        "candidates": [
            {**c, "obj_base": hex(c["obj_base"]),
             "klass_ptr": hex(c["klass_ptr"]),
             "field_offset": hex(c["field_offset"])}
            for c in candidates[:32]
        ],
    }


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m tools.mem_probe.il2cpp.discover_hp_field")
    ap.add_argument("--anchors", default=ANCHORS_DEFAULT, help="anchors.json 路径 (模式 A)")
    ap.add_argument("--out", default=OUT_DEFAULT, help="输出 discovery_notes.json")
    # 模式 A: 直接给地址
    ap.add_argument("--hp", type=lambda s: int(s, 0), default=None,
                    help="模式 A: 覆盖 self_hp_addr (例 0x120278c1e0)")
    ap.add_argument("--max-hp", type=lambda s: int(s, 0), default=None)
    ap.add_argument("--uid", type=lambda s: int(s, 0), default=None)
    # 模式 B: 给 HP 数值, 全堆扫
    ap.add_argument("--scan-hp", type=int, default=None,
                    help="模式 B: 当前游戏内 HP 数值 (从 UI 上读出来)")
    ap.add_argument("--scan-max-hp", type=int, default=None,
                    help="模式 B: 当前 MaxHP 数值 (与 scan-hp 配合, 用相邻过滤)")
    ap.add_argument("--scan-uid", type=int, default=None,
                    help="模式 B: UID (i64)")
    args = ap.parse_args(argv)

    # 决定模式
    use_scan = args.scan_hp is not None
    if not use_scan:
        if not os.path.isfile(args.anchors):
            print(f"[fail] anchors.json 不存在且未提供 --scan-hp: {args.anchors}", file=sys.stderr)
            return 1
        with open(args.anchors, "r", encoding="utf-8") as f:
            anchors = json.load(f)
        hp_addr = args.hp or int(anchors["self_hp_addr"], 16)
        max_hp_addr = args.max_hp or (
            int(anchors.get("self_max_hp_addr", "0x0"), 16) or None
        )
        uid_addr = args.uid or (
            int(anchors["self_uid_addr"], 16) if anchors.get("self_uid_addr") else None
        )
        print(f"[input/A] hp=0x{hp_addr:X}  "
              f"max_hp={'0x%X' % max_hp_addr if max_hp_addr else None}  "
              f"uid={'0x%X' % uid_addr if uid_addr else None}")

    try:
        pm = StarProcess()
    except StarProcessError as e:
        print(f"[fail] {e}", file=sys.stderr)
        return 2

    try:
        ga_base, ga_size = _ga_module(pm)
        print(f"[ga] GameAssembly.dll base=0x{ga_base:X} size=0x{ga_size:X}")

        report: Dict[str, Any] = {
            "saved_at": time.time(),
            "pid": pm.pid,
            "ga_base": hex(ga_base),
            "ga_size": hex(ga_size),
            "mode": "scan" if use_scan else "addr",
            "findings": {},
        }

        if use_scan:
            # 模式 B: 全堆扫 HP 数值, 对每个候选反查 klass
            print(f"\n[scan] HP={args.scan_hp}  MaxHP={args.scan_max_hp}  UID={args.scan_uid}")
            t0 = time.time()
            hp_hits = scan_i32(pm, args.scan_hp)
            print(f"[scan] HP candidates: {len(hp_hits)} ({time.time()-t0:.1f}s)")

            mh_hits: List[int] = []
            if args.scan_max_hp is not None:
                t0 = time.time()
                mh_hits = scan_i32(pm, args.scan_max_hp)
                print(f"[scan] MaxHP candidates: {len(mh_hits)} ({time.time()-t0:.1f}s)")

            uid_hits: List[int] = []
            if args.scan_uid is not None:
                t0 = time.time()
                uid_hits = scan_i64(pm, args.scan_uid)
                print(f"[scan] UID candidates: {len(uid_hits)} ({time.time()-t0:.1f}s)")
                for ua in uid_hits[:16]:
                    print(f"        uid@0x{ua:X}")

            # 用 UID 邻接过滤 HP 候选 (UID 全局唯一, 是最可靠的 self 标识)
            uid_set_sorted = sorted(uid_hits)
            UID_NEAR = 0x1000  # ±4KB

            def has_uid_near(addr: int) -> Optional[int]:
                if not uid_hits:
                    return None
                import bisect
                lo = bisect.bisect_left(uid_set_sorted, addr - UID_NEAR)
                hi = bisect.bisect_right(uid_set_sorted, addr + UID_NEAR)
                if hi > lo:
                    return min(uid_set_sorted[lo:hi], key=lambda a: abs(a - addr))
                return None

            # 对 HP 候选反查 klass; 同时若 MaxHP 在 ±0x100 附近则加分
            mh_set_sorted = sorted(mh_hits)

            def has_max_hp_near(hp_addr: int) -> Optional[int]:
                if not mh_hits:
                    return None
                import bisect
                lo = bisect.bisect_left(mh_set_sorted, hp_addr - 0x200)
                hi = bisect.bisect_right(mh_set_sorted, hp_addr + 0x200)
                if hi > lo:
                    cand = [a for a in mh_set_sorted[lo:hi] if a != hp_addr]
                    if cand:
                        return min(cand, key=lambda a: abs(a - hp_addr))
                return None

            qualified: List[Dict[str, Any]] = []
            for hp_addr in hp_hits:
                mh_near = has_max_hp_near(hp_addr)
                uid_near = has_uid_near(hp_addr)
                cands = find_object_base(pm, hp_addr, ga_base, ga_size)
                best = cands[0] if cands else None
                qualified.append({
                    "hp_addr": hp_addr,
                    "max_hp_near": mh_near,
                    "uid_near": uid_near,
                    "obj_base": best["obj_base"] if best else None,
                    "klass_ptr": best["klass_ptr"] if best else None,
                    "klass_name": best["klass_name"] if best else None,
                    "klass_namespace": best["klass_namespace"] if best else None,
                    "hp_field_offset": best["field_offset"] if best else None,
                    "max_hp_field_offset": (mh_near - best["obj_base"]) if (mh_near and best) else None,
                })

            # 排序: UID 邻近 + MaxHP 邻近 优先
            qualified.sort(
                key=lambda d: (d["uid_near"] is None, d["max_hp_near"] is None, d["hp_addr"])
            )
            print(f"\n[scan] HP 候选 ({len(qualified)}):")
            for i, q in enumerate(qualified):
                ns = f"{q['klass_namespace']}." if q["klass_namespace"] else ""
                kls = f"{ns}{q['klass_name']}" if q["klass_name"] else "(无 GA klass)"
                mh_str = f" mh@0x{q['max_hp_near']:X}" if q["max_hp_near"] else " mh=NONE"
                uid_str = (
                    f" UID@0x{q['uid_near']:X}(d={q['uid_near']-q['hp_addr']:+X})"
                    if q["uid_near"] else " UID=NONE"
                )
                print(f"  #{i}  hp=0x{q['hp_addr']:X}  {kls}{mh_str}{uid_str}")

            # 诊断: 当所有 qualified 都没 UID 邻接时, 打印命中前后布局
            best_q = next((q for q in qualified if q["uid_near"]), None)
            if best_q is None and hp_hits:
                print("\n[diag] 没有 HP 候选邻近 UID; 打印命中地址前 0x80 qword 布局:")
                modules = pm.list_modules()
                mod_ranges = [(m.base, m.base + m.size, m.name) for m in modules]

                def classify(p: int) -> str:
                    if not p:
                        return "NULL"
                    for b, e, n in mod_ranges:
                        if b <= p < e:
                            return f"MOD:{n}+0x{p-b:X}"
                    return "HEAP" if 0x10000000000 <= p < 0x800000000000 else "?"

                for hp_addr in hp_hits[:8]:
                    print(f"\n  -- hp_addr=0x{hp_addr:X} --")
                    for off in range(-0x80, 0x10, 8):
                        a = hp_addr + off
                        v = pm.read_u64(a)
                        if v is None:
                            continue
                        marker = "<-- HP" if off == 0 else ""
                        print(f"    [{off:+04X}] 0x{a:X} = 0x{v:016X}  {classify(v)}  {marker}")
            elif best_q:
                ns = f"{best_q['klass_namespace']}." if best_q["klass_namespace"] else ""
                kls = f"{ns}{best_q['klass_name']}" if best_q["klass_name"] else "(无 GA klass)"
                print(f"\n[best] hp_addr=0x{best_q['hp_addr']:X}  {kls}")
                if best_q["uid_near"]:
                    print(f"       UID @ 0x{best_q['uid_near']:X}  (offset {best_q['uid_near']-best_q['hp_addr']:+d} from HP)")
                if best_q["max_hp_near"]:
                    print(f"       MaxHP @ 0x{best_q['max_hp_near']:X}  (offset {best_q['max_hp_near']-best_q['hp_addr']:+d} from HP)")
                if best_q["obj_base"]:
                    print(f"       obj_base = 0x{best_q['obj_base']:X}, hp_field_offset = 0x{best_q['hp_field_offset']:X}")
                report["best_match"] = {
                    "hp_addr": hex(best_q["hp_addr"]),
                    "uid_addr": hex(best_q["uid_near"]) if best_q["uid_near"] else None,
                    "max_hp_addr": hex(best_q["max_hp_near"]) if best_q["max_hp_near"] else None,
                    "klass_name": best_q["klass_name"],
                    "klass_namespace": best_q["klass_namespace"],
                    "obj_base": hex(best_q["obj_base"]) if best_q["obj_base"] else None,
                    "hp_field_offset": hex(best_q["hp_field_offset"]) if best_q["hp_field_offset"] is not None else None,
                    "max_hp_field_offset": hex(best_q["max_hp_field_offset"]) if best_q["max_hp_field_offset"] is not None else None,
                }

            report["findings"]["scan"] = {
                "scan_hp": args.scan_hp,
                "scan_max_hp": args.scan_max_hp,
                "scan_uid": args.scan_uid,
                "hp_hit_count": len(hp_hits),
                "max_hp_hit_count": len(mh_hits),
                "uid_hit_count": len(uid_hits),
                "hp_hits": [hex(a) for a in hp_hits],
                "max_hp_hits": [hex(a) for a in mh_hits],
                "uid_hits": [hex(a) for a in uid_hits],
                "qualified": [
                    {
                        "hp_addr": hex(q["hp_addr"]),
                        "max_hp_near": hex(q["max_hp_near"]) if q["max_hp_near"] else None,
                        "uid_near": hex(q["uid_near"]) if q["uid_near"] else None,
                        "obj_base": hex(q["obj_base"]) if q["obj_base"] else None,
                        "klass_ptr": hex(q["klass_ptr"]) if q["klass_ptr"] else None,
                        "klass_name": q["klass_name"],
                        "klass_namespace": q["klass_namespace"],
                        "hp_field_offset": hex(q["hp_field_offset"]) if q["hp_field_offset"] is not None else None,
                        "max_hp_field_offset": hex(q["max_hp_field_offset"]) if q["max_hp_field_offset"] is not None else None,
                    }
                    for q in qualified[:64]
                ],
            }

            # 二级反查: 对每个 HP 命中地址做 i64 反扫, 找谁指向它,
            # 然后在指针位置周围 ±0x1000 找 UID.
            # 这是定位 "self 主对象" 的关键路径 (HP 数据是被引用的, UID 在主对象上).
            print("\n[ptr-back] 反扫指向每个 HP 候选的指针, 找 UID 邻接...")
            ptrback_results = []
            for hp_addr in hp_hits:
                t0 = time.time()
                refs = scan_i64(pm, hp_addr, max_hits=2000)
                # 看哪些 ref 周围有 UID
                ref_with_uid = []
                for r in refs:
                    u = has_uid_near(r)
                    if u:
                        ref_with_uid.append({"ref_addr": r, "uid_near": u, "off_to_uid": u - r})
                if refs:
                    print(f"  hp@0x{hp_addr:X}: {len(refs)} 个反引用, "
                          f"{len(ref_with_uid)} 个邻近 UID  ({time.time()-t0:.1f}s)")
                if ref_with_uid:
                    for rwu in ref_with_uid[:5]:
                        # 反查: 这个 ref 在哪个对象上 (尝试在 GA 模块范围内找 klass)
                        cands = find_object_base(pm, rwu["ref_addr"], ga_base, ga_size)
                        klass_str = ""
                        if cands:
                            c = cands[0]
                            ns = f"{c['klass_namespace']}." if c["klass_namespace"] else ""
                            klass_str = f"  klass={ns}{c['klass_name']}+0x{c['field_offset']:X}"
                        print(f"    ref@0x{rwu['ref_addr']:X} UID@0x{rwu['uid_near']:X}"
                              f" (off={rwu['off_to_uid']:+d}){klass_str}")
                ptrback_results.append({
                    "hp_addr": hex(hp_addr),
                    "ref_count": len(refs),
                    "refs_with_uid_near": [
                        {"ref_addr": hex(r["ref_addr"]),
                         "uid_near": hex(r["uid_near"]),
                         "off_to_uid": r["off_to_uid"]}
                        for r in ref_with_uid[:32]
                    ],
                })
            report["findings"]["ptr_back"] = ptrback_results
        else:
            # 模式 A: 直接反查
            report["anchors_input"] = {
                "hp": hex(hp_addr),
                "max_hp": hex(max_hp_addr) if max_hp_addr else None,
                "uid": hex(uid_addr) if uid_addr else None,
            }
            report["findings"]["hp"] = discover_one(pm, "hp", hp_addr, ga_base, ga_size)
            if max_hp_addr:
                report["findings"]["max_hp"] = discover_one(pm, "max_hp", max_hp_addr, ga_base, ga_size)
            if uid_addr:
                report["findings"]["uid"] = discover_one(pm, "uid", uid_addr, ga_base, ga_size)

            # 找共同候选
            common: Dict[int, Dict[str, int]] = {}
            for label in ("hp", "max_hp", "uid"):
                f = report["findings"].get(label)
                if not f:
                    continue
                for c in f["candidates"]:
                    obj = int(c["obj_base"], 16)
                    common.setdefault(obj, {})[label] = int(c["field_offset"], 16)
            common_full = sorted(
                [(obj, fields) for obj, fields in common.items() if len(fields) >= 2],
                key=lambda kv: -len(kv[1]),
            )
            if common_full:
                print(f"\n[common] {len(common_full)} 对象同时容纳 ≥2 个字段:")
                top = common_full[0]
                obj, fields = top
                klass_name = next(
                    (c["klass_name"] for c in report["findings"]["hp"]["candidates"]
                     if int(c["obj_base"], 16) == obj),
                    "?"
                )
                print(f"  最佳: obj=0x{obj:X} klass={klass_name} fields={fields}")
                report["best_match"] = {
                    "obj_base": hex(obj),
                    "klass_name": klass_name,
                    "field_offsets": {k: hex(v) for k, v in fields.items()},
                }

        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2, ensure_ascii=False)
        print(f"\n[ok] 报告写入: {args.out}")
        return 0
    finally:
        pm.close()


if __name__ == "__main__":
    raise SystemExit(main())
