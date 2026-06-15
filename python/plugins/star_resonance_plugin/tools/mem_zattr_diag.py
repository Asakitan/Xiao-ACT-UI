# -*- coding: utf-8 -*-
"""Live ZAttr diagnostic — discover the real ZMixItem<T> Value offset (read-only).

Does NOT use the (stale) static self path. It:
  1. opens the process + GameAssembly via StaticDpsSource.sr,
  2. resolves the Panda.ZGame.ZEntityMgr klass (bundle, else on-disk script.json),
  3. locates ZEntityMgr by best-entity-count (no self uid needed),
  4. enumerates bossDict_/monsterDict_ entities,
  5. for the entity carrying a MAX_HP (11320) attribute near a target value,
     dumps the IMixAttr object bytes so the real Value offset is visible.

Usage (game running, boss in scene):
    python tools/mem_zattr_diag.py --target-hp 17800000 --tol 1500000
"""
from __future__ import annotations

import argparse
import glob
import os
import struct
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from mem_probe.il2cpp.static_dps_source import StaticDpsSource
from mem_probe.il2cpp.mem_entity_mgr import (
    EntityMgrReader, ENT_ATTRS_OFF, ENT_UUID_OFF, ENT_CONFIG_OFF,
    BOSS_DICT_OFF, MONSTER_DICT_OFF,
)
from mem_probe.il2cpp.mem_attr_reader import ZAttrReader, MIXDICT_OFF
from mem_probe.il2cpp.script_parser import ScriptIndex

ENTITY_MGR_CLASS = "Panda.ZGame.ZEntityMgr"
A_HP, A_MAX_HP, A_BREAKING_STAGE = 11310, 11320, 455


def _candidate_mgr_klasses(src):
    """Return [(klass_ptr, source_str), ...] candidates, newest script.json first.

    The on-disk dumps may include stale versions whose RVA points to garbage for
    the running game, so we yield every plausible candidate and let the caller
    validate each by actually locating ZEntityMgr.
    """
    sr = src.sr
    ga = int(sr.ga)
    cands = []
    try:
        kp = sr.resolve_klass(ENTITY_MGR_CLASS) or 0
        if kp and 0x10000 <= kp <= 0x7FFFFFFFFFFF:
            cands.append((kp, "bundle"))
    except Exception:
        pass
    out_dir = os.path.join(os.path.dirname(os.path.abspath(
        sys.modules["mem_probe.il2cpp.mem_entity_mgr"].__file__)), "out")
    sjs = glob.glob(os.path.join(out_dir, "*", "script.json")) + \
        glob.glob(os.path.join(out_dir, "*", "dumper_out", "script.json"))
    sjs.sort(key=lambda p: os.path.getmtime(p), reverse=True)   # newest dump first
    for sj in sjs:
        try:
            si = ScriptIndex.load(sj)
            rva = si.find_klass(ENTITY_MGR_CLASS)
            if not rva:
                continue
            cand = sr.pm.read_u64(ga + rva) or 0
            if cand and 0x10000 <= cand <= 0x7FFFFFFFFFFF:
                cands.append((cand, f"{os.path.relpath(sj, out_dir)} rva={rva:#x}"))
        except Exception as exc:
            print(f"  [diag] {os.path.relpath(sj, out_dir)} failed: {exc}", file=sys.stderr)
    return cands


def _walk_attr_dict(pm, attrs_obj):
    """attrs_obj -> {attr_id: imixattr_ptr} (Dictionary<uint, IMixAttr> @ +0x28)."""
    r = ZAttrReader(pm)
    return r.read_attr_ptrs(attrs_obj)


def _dump_imixattr(pm, ptr, target, tol):
    blob = pm.read_bytes(ptr, 0x60)
    if not blob:
        print(f"      (could not read IMixAttr @ {ptr:#x})")
        return
    klass = struct.unpack_from("<Q", blob, 0)[0]
    print(f"      IMixAttr @ {ptr:#x} klass={klass:#x}")
    for off in range(0x10, 0x58, 4):
        i32 = struct.unpack_from("<i", blob, off)[0]
        i64 = struct.unpack_from("<q", blob, off)[0]
        flag32 = " <== MATCH(i32)" if abs(i32 - target) <= tol else ""
        flag64 = " <== MATCH(i64)" if abs(i64 - target) <= tol else ""
        if flag32 or flag64 or (0 < i32 < 10 ** 10) or (0 < i64 < 10 ** 11):
            print(f"        +{off:#04x}: i32={i32:<14} i64={i64}{flag32}{flag64}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target-hp", type=int, default=17_800_000)
    ap.add_argument("--tol", type=int, default=1_500_000)
    args = ap.parse_args()

    src = StaticDpsSource()
    try:
        _ = src.sr
    except Exception as exc:
        print(f"[diag] cannot open process/GameAssembly: {exc}", file=sys.stderr)
        return 1
    pm = src.sr.pm
    ga = int(src.sr.ga)
    print(f"[diag] process pid={pm.pid} ga={ga:#x}")

    cands = _candidate_mgr_klasses(src)
    print(f"[diag] {len(cands)} ZEntityMgr klass candidate(s)")
    if not cands:
        print("[diag] could not resolve any ZEntityMgr klass.", file=sys.stderr)
        return 2

    emr = EntityMgrReader(src)
    mgr = 0
    for klass, ksrc in cands:
        emr._resolve_mgr_klass = lambda k=klass: k
        emr._mgr_addr = 0
        print(f"[diag] trying klass={klass:#x} via {ksrc} ...")
        cand_mgr = emr.locate(0, force_rescan=True)
        if not cand_mgr:
            print("[diag]   not located with this klass")
            continue
        bd = pm.read_u64(cand_mgr + BOSS_DICT_OFF)
        md = pm.read_u64(cand_mgr + MONSTER_DICT_OFF)
        nb = len(emr._read_dict_entries(bd, max_entries=8))
        nm = len(emr._read_dict_entries(md, max_entries=8))
        print(f"[diag]   located @ {cand_mgr:#x} boss={nb} monster={nm}")
        if nb or nm:
            mgr = cand_mgr
            break
    if not mgr:
        print("[diag] ZEntityMgr not located with any candidate.", file=sys.stderr)
        return 3
    puid = pm.read_i64(mgr + 0x10)
    print(f"[diag] ZEntityMgr @ {mgr:#x} playerUuid_={puid}")

    target, tol = args.target_hp, args.tol
    for dict_name, dict_off in (("boss", BOSS_DICT_OFF), ("monster", MONSTER_DICT_OFF)):
        d = pm.read_u64(mgr + dict_off)
        ents = emr._read_dict_entries(d, max_entries=64)
        print(f"[diag] {dict_name}Dict_ @ {d:#x} -> {len(ents)} entities")
        for key, ent in ents:
            attrs = pm.read_u64(ent + ENT_ATTRS_OFF)
            uuid = pm.read_i64(ent + ENT_UUID_OFF)
            cfg = pm.read_i64(ent + ENT_CONFIG_OFF)
            ptrs = _walk_attr_dict(pm, attrs) if attrs else {}
            has_hp = A_MAX_HP in ptrs or A_HP in ptrs
            print(f"  {dict_name} uuid={uuid} cfg={cfg} attrs={attrs:#x} "
                  f"attr_count={len(ptrs)} has_hp={has_hp} "
                  f"ids={sorted(list(ptrs.keys()))[:14]}")
            if A_MAX_HP in ptrs:
                print(f"    MAX_HP(11320) ->")
                _dump_imixattr(pm, ptrs[A_MAX_HP], target, tol)
            if A_HP in ptrs:
                print(f"    HP(11310) ->")
                _dump_imixattr(pm, ptrs[A_HP], target, tol)
            if A_BREAKING_STAGE in ptrs:
                print(f"    BREAKING_STAGE(455) ->")
                _dump_imixattr(pm, ptrs[A_BREAKING_STAGE], 0, 0)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
