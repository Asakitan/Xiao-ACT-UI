# -*- coding: utf-8 -*-
"""probe_entity_position - 探索 ZEntity 的位置/坐标字段 (read-only, 活体).

链路: ZEntityMgr(堆扫) -> playerEnt_ -> 玩家实体对象 -> 活字段表逐字段解引用,
读出每个指针字段指向对象的 klass 名 -> 找 Move/Transform/Position 类组件 ->
dump 组件字段表 -> 候选 float 三元组按数值合理性打印。

用法:
  python -m tools.probe_entity_position                # 字段地图
  python -m tools.probe_entity_position --watch 0x...  # 监视一个地址的 float 三元组
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from mem_probe.il2cpp.static_dps_source import StaticDpsSource
from mem_probe.il2cpp.mem_entity_mgr import EntityMgrReader, ENTITY_CLASS
from mem_probe.il2cpp.live_field_resolver import LiveFieldResolver

_MIN_PTR = 0x10000
_MAX_PTR = 0x7FFF_FFFF_FFFF


def _utf8():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass


def _klass_name(pm, kp: int) -> str:
    try:
        np = pm.read_u64(kp + 0x10)
        if not (_MIN_PTR <= (np or 0) <= _MAX_PTR):
            return ""
        b = pm.read_bytes(np, 64)
        return b.split(b"\x00", 1)[0].decode("utf-8", "replace") if b else ""
    except Exception:
        return ""


def _obj_klass_name(pm, obj: int) -> str:
    try:
        kp = pm.read_u64(obj)
        if not (_MIN_PTR <= (kp or 0) <= _MAX_PTR):
            return ""
        return _klass_name(pm, kp)
    except Exception:
        return ""


def _plaus_coord(v: float) -> bool:
    return v == v and -100000.0 < v < 100000.0 and abs(v) > 1e-6


def dump_object_fields(pm, lfr: LiveFieldResolver, obj: int, label: str,
                       deref: bool = True) -> dict:
    kp = pm.read_u64(obj)
    kname = _klass_name(pm, kp)
    fmap = lfr._field_map(kp)
    print(f"\n== {label}: obj=0x{obj:X} klass={kname} ({len(fmap)} fields)")
    out = {}
    for nm, off in sorted(fmap.items(), key=lambda kv: kv[1]):
        if off <= 0 or off > 0x2000:
            continue
        raw = pm.read_u64(obj + off) or 0
        extra = ""
        tgt = ""
        if deref and _MIN_PTR <= raw <= _MAX_PTR and (raw & 0x3) == 0:
            tgt = _obj_klass_name(pm, raw)
            if tgt:
                extra = f" -> {tgt}"
        # float 三元组候选
        b = pm.read_bytes(obj + off, 12)
        if b and len(b) == 12:
            f3 = struct.unpack("<3f", b)
            if all(_plaus_coord(v) for v in f3):
                extra += f"  f3=({f3[0]:.2f},{f3[1]:.2f},{f3[2]:.2f})"
        print(f"  +0x{off:04X} {nm:40s} 0x{raw:016X}{extra}")
        out[nm] = (off, raw, tgt)
    return out


def main(argv=None) -> int:
    _utf8()
    ap = argparse.ArgumentParser()
    ap.add_argument("--watch", default=None,
                    help="监视地址(hex)的 12 字节 float 三元组, 1Hz x 10s")
    ap.add_argument("--dump-class", default=None,
                    help="额外 dump 一个类的字段表 (按名解析)")
    ap.add_argument("--dump-obj", default=None,
                    help="dump 任意对象地址(hex)的字段表+值")
    ap.add_argument("--find-class", default=None,
                    help="GA 全量扫: 列出类名含子串(不分大小写)的全部类")
    ap.add_argument("--find-instance", default=None,
                    help="按类全名堆扫实例并 dump 前 3 个")
    ap.add_argument("--max-instances", type=int, default=8)
    args = ap.parse_args(argv)

    src = StaticDpsSource()
    sr = src.sr
    pm = sr.pm
    print(f"[probe] attached, ga=0x{int(sr.ga):X}")

    if args.find_class:
        from mem_probe.il2cpp.auto_registration_locator import (
            find_ga_module, klass_fullname, KLASS_LO, KLASS_HI)
        try:
            import numpy as _np
        except Exception:
            _np = None
        ga = find_ga_module(pm)
        base, size = int(ga.base), int(ga.size)
        sub = args.find_class.lower()
        seen, hits = set(), {}
        off, chunk = 0, 32 * 1024 * 1024
        t0 = time.time()
        while off < size and (time.time() - t0) < 120:
            blob = pm.read_bytes(base + off, min(chunk, size - off))
            if blob is None:
                off += chunk
                continue
            usable = (len(blob) // 8) * 8
            if _np is not None and usable:
                arr = _np.frombuffer(blob[:usable], dtype="<u8")
                cands = _np.unique(arr[(arr >= KLASS_LO) & (arr <= KLASS_HI)]).tolist()
            else:
                cands = []
            for v in cands:
                v = int(v)
                if v in seen:
                    continue
                seen.add(v)
                full = klass_fullname(pm, v)
                if full and sub in full.lower():
                    hits[full] = v
            off += chunk
        for full, kp in sorted(hits.items()):
            print(f"  0x{kp:X}  {full}")
        print(f"[probe] {len(hits)} classes matching {args.find_class!r}")
        return 0

    if args.dump_obj:
        lfr0 = LiveFieldResolver(pm, klass_resolver=getattr(sr, "resolve_klass", None))
        dump_object_fields(pm, lfr0, int(args.dump_obj, 16), "obj")
        return 0

    if args.find_instance:
        from mem_probe.il2cpp.auto_registration_locator import build_live_class_index
        idx = build_live_class_index(pm, {args.find_instance}, time_budget_s=60)
        kp = int(idx.get(args.find_instance, 0) or 0)
        if not kp:
            print(f"[probe] class not found: {args.find_instance}")
            return 1
        print(f"[probe] klass 0x{kp:X}, scanning heap for instances...")
        kb = kp.to_bytes(8, "little")
        found = []
        for r in pm.iter_regions(only_readable=True, only_private=True):
            off, chunk = 0, 16 * 1024 * 1024
            while off < r.size and len(found) < args.max_instances:
                blob = pm.read_bytes(r.base + off, min(chunk, r.size - off))
                if blob is None:
                    break
                start = 0
                while len(found) < args.max_instances:
                    i = blob.find(kb, start)
                    if i < 0:
                        break
                    if (i & 0x7) == 0:
                        found.append(r.base + off + i)
                    start = i + 8
                off += chunk
            if len(found) >= args.max_instances:
                break
        print(f"[probe] {len(found)} instance(s)")
        lfr0 = LiveFieldResolver(pm, klass_resolver=getattr(sr, "resolve_klass", None))
        for obj in found[:3]:
            try:
                dump_object_fields(pm, lfr0, obj, f"instance 0x{obj:X}")
            except Exception as e:
                print(f"  ! dump failed: {e}")
        return 0

    if args.watch:
        addr = int(args.watch, 16)
        for _ in range(10):
            b = pm.read_bytes(addr, 12)
            if b:
                print("f3:", struct.unpack("<3f", b))
            time.sleep(1.0)
        return 0

    lfr = LiveFieldResolver(pm, klass_resolver=getattr(sr, "resolve_klass", None))

    emr = EntityMgrReader(src)
    mgr = emr.locate(0)
    if not mgr:
        print("[probe] ZEntityMgr NOT found")
        return 1
    print(f"[probe] ZEntityMgr @ 0x{mgr:X}")

    # playerEnt_ 偏移: 活字段表优先, 0x18 文献兜底
    off_player = lfr.field_offset("Panda.ZGame.ZEntityMgr", "playerEnt_") or 0x18
    player = pm.read_u64(mgr + off_player) or 0
    print(f"[probe] playerEnt_ @ +0x{off_player:X} -> 0x{player:X} "
          f"({_obj_klass_name(pm, player)})")
    if not player:
        print("[probe] no player entity (not in scene?)")
        return 1

    fields = dump_object_fields(pm, lfr, player, "PlayerEnt(instance)")

    # ZEntity 基类自己的字段 (派生类字段表只含自身字段)
    kp_base = lfr._resolve_klass(ENTITY_CLASS)
    if kp_base:
        base_map = lfr._field_map(kp_base)
        print(f"\n== {ENTITY_CLASS} base fields ({len(base_map)}):")
        for nm, off in sorted(base_map.items(), key=lambda kv: kv[1]):
            if off <= 0 or off > 0x2000:
                continue
            raw = pm.read_u64(player + off) or 0
            tgt = _obj_klass_name(pm, raw) if _MIN_PTR <= raw <= _MAX_PTR else ""
            b = pm.read_bytes(player + off, 12)
            f3txt = ""
            if b and len(b) == 12:
                f3 = struct.unpack("<3f", b)
                if all(_plaus_coord(v) for v in f3):
                    f3txt = f"  f3=({f3[0]:.2f},{f3[1]:.2f},{f3[2]:.2f})"
            print(f"  +0x{off:04X} {nm:40s} 0x{raw:016X}"
                  f"{(' -> ' + tgt) if tgt else ''}{f3txt}")
            fields[nm] = (off, raw, tgt)

    # 自动追组件: 名字含 move/trans/pos/loc 的指针字段
    PAT = ("move", "trans", "pos", "loc", "scene", "model", "body", "ctrl")
    seen = set()
    for nm, (off, raw, tgt) in list(fields.items()):
        l = nm.lower() + " " + (tgt or "").lower()
        if any(p in l for p in PAT) and _MIN_PTR <= raw <= _MAX_PTR and raw not in seen:
            seen.add(raw)
            try:
                dump_object_fields(pm, lfr, raw, f"comp {nm}")
            except Exception as e:
                print(f"  ! comp {nm} dump failed: {e}")

    if args.dump_class:
        kp = lfr._resolve_klass(args.dump_class)
        if kp:
            m = lfr._field_map(kp)
            print(f"\n== {args.dump_class} ({len(m)}):")
            for nm, off in sorted(m.items(), key=lambda kv: kv[1]):
                print(f"  +0x{off:04X} {nm}")
        else:
            print(f"[probe] class not found: {args.dump_class}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
