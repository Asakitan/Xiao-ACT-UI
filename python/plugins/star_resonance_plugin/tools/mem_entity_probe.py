# -*- coding: utf-8 -*-
# Live probe — verify F-B/F-C against the running game (read-only).
#
# This is the live-calibration gate for the memory-backed entity/boss data layer.
# It cannot run offline; point it at the running game to confirm that:
# 1. ZEntityMgr is located and boss/monster entities enumerate (F-C v1).
# 2. ZAttrReader calibrates a per-klass Value offset from a known HP (F-B).
# 3. Boss HP / breaking_stage / overdrive read correctly from memory.
#
# Calibration anchor: by default we use the SELF entity's own HP/MaxHp (which the
# existing self path already reads from UserFightAttr) to calibrate, then read
# boss values with the calibrated offsets. You can also pass a known boss value:
#
# python tools/mem_entity_probe.py                 # self-HP calibration
# python tools/mem_entity_probe.py --max-hp 5000000000   # explicit MAX_HP anchor
#
# Read-only. No writes, no injection.
from __future__ import annotations

import argparse
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from plugins.star_resonance_plugin.mem.il2cpp.static_dps_source import StaticDpsSource
from plugins.star_resonance_plugin.mem.il2cpp.mem_entity_mgr import (
    EntityMgrReader, A_HP, A_MAX_HP, A_BREAKING_STAGE, A_IN_OVERDRIVE,
    A_EXTINCTION, A_MAX_EXTINCTION, A_STUNNED, A_HATED_CHAR_ID, A_SKILL_ID,
)
from plugins.star_resonance_plugin.mem.il2cpp.mem_attr_reader import ZAttrReader


def _ga_base(src) -> int:
    try:
        return int(getattr(src.sr, "ga", 0) or src.sr.pm.main_module().base or 0)
    except Exception:
        return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-hp", type=int, default=0,
                    help="known boss/self MAX_HP to use as the calibration anchor")
    ap.add_argument("--hp", type=int, default=0, help="known HP anchor (optional)")
    args = ap.parse_args()

    src = StaticDpsSource()
    self_snap = src.get_self_snapshot(force_rescan=False)
    if self_snap is None:
        print("[probe] no self snapshot — is the game running and in a scene?", file=sys.stderr)
        return 1
    uid = int(self_snap.uid)
    ga = _ga_base(src)
    print(f"[probe] self uid={uid} cur_hp={self_snap.cur_hp} max_hp={self_snap.max_hp} ga={ga:#x}")

    emr = EntityMgrReader(src)
    t0 = time.time()
    mgr = emr.locate(uid)
    if not mgr:
        print("[probe] ZEntityMgr locate FAILED", file=sys.stderr)
        return 2
    print(f"[probe] ZEntityMgr @ {mgr:#x} (locate {time.time()-t0:.2f}s)")

    snap = emr.read(uid, include_monsters=True, include_npcs=False)
    if snap is None:
        print("[probe] entity read FAILED", file=sys.stderr)
        return 3
    print(f"[probe] bosses={len(snap.bosses)} monsters={len(snap.monsters)}")

    # --- calibrate ZAttrReader ---
    reader = ZAttrReader(src.sr.pm, ga_base=ga)
    known = {}
    if args.max_hp:
        known[A_MAX_HP] = args.max_hp
    if args.hp:
        known[A_HP] = args.hp
    calib_target = None
    if not known:
        # self-HP calibration: find the self entity (uuid == uid) and use its HP.
        all_ents = list(snap.bosses) + list(snap.monsters)
        # self usually lives in entityDict_, not boss/monster; fall back to a boss.
        for e in all_ents:
            if e.uuid == uid:
                calib_target = e
                break
        if calib_target is None and int(self_snap.max_hp or 0) > 0:
            # try self via entityDict lookup
            ent_dict = src.sr.pm.read_u64(mgr + 0x28)
            for k, v in emr._read_dict_entries(ent_dict, max_entries=512):
                if k == uid:
                    se = emr._read_entity(v)
                    if se:
                        calib_target = se
                        break
            if calib_target is not None:
                known = {A_HP: int(self_snap.cur_hp), A_MAX_HP: int(self_snap.max_hp)}
    if not known and snap.bosses:
        print("[probe] no calibration anchor; pass --max-hp <boss max hp> to calibrate.",
              file=sys.stderr)

    if known and (calib_target or snap.bosses):
        tgt = calib_target or snap.bosses[0]
        n = emr.calibrate_attrs(tgt.obj_addr, reader, known, ga_base=ga)
        print(f"[probe] calibrated {n} klass(es) from {known} on uuid={tgt.uuid}; "
              f"klass_off={reader._klass_value_off}")

    # --- read boss numerics ---
    for b in snap.bosses[:10]:
        emr.fill_entity_numeric(b, reader)
        print(f"  BOSS uuid={b.uuid} cfg={b.config_uuid} state={b.state} "
              f"hp={b.cur_hp}/{b.max_hp} ({b.hp_pct*100:.0f}%) "
              f"break={b.breaking_stage} ext={b.extinction}/{b.max_extinction} "
              f"stun={b.stunned} overdrive={b.in_overdrive} "
              f"hated={b.hated_char_id} cast={b.cast_skill_id} read={b.attrs_read}")
    if not snap.bosses:
        for m in snap.monsters[:8]:
            emr.fill_entity_numeric(m, reader)
            print(f"  MOB uuid={m.uuid} cfg={m.config_uuid} hp={m.cur_hp}/{m.max_hp} "
                  f"read={m.attrs_read}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
