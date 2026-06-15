# -*- coding: utf-8 -*-
"""Wider net: poll the boss's full attr_map continuously and find any attr that
cycles through a SMALL REPEATING set of values (a skill-id / skill-type
signature) -- as opposed to HP / monotonic timers. Also dumps both buff lists.

Use the boss's different skills (吼/护盾/普攻) while this runs.

    python tools/diag_boss_skill_discriminator.py [seconds] [name_substr]
"""
from __future__ import annotations

import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 25.0
NAME_SUB = sys.argv[2] if len(sys.argv) > 2 else ""
HZ = 33.0
ENT_BUFFCOMP_OFF = 0x98
ENT_CLIENTBUFF_OFF = 0xA0
BUFFCOMP_LIST_OFF = 0x30
ZLIST_ITEMS_OFF, ZLIST_SIZE_OFF = 0x10, 0x18
ARR_LEN_OFF, ARR_ELEMS_OFF = 0x18, 0x20
BI_BASE_OFF, BI_DUR_OFF = 0x14, 0x38
A_SKILL = 100


def read_buff_list(pm, ent, comp_off):
    out = []
    comp = pm.read_u64(ent + comp_off)
    if not comp:
        return out
    zlist = pm.read_u64(comp + BUFFCOMP_LIST_OFF)
    if not zlist:
        return out
    size = pm.read_u32(zlist + ZLIST_SIZE_OFF) or 0
    items = pm.read_u64(zlist + ZLIST_ITEMS_OFF)
    if not items or size <= 0:
        return out
    for i in range(min(int(size), 64)):
        bi = pm.read_u64(items + ARR_ELEMS_OFF + i * 8)
        if bi:
            out.append((int(pm.read_u32(bi + BI_BASE_OFF) or 0),
                        int(pm.read_i64(bi + BI_DUR_OFF) or 0)))
    return out


def main():
    from mem_probe.il2cpp.static_dps_source import StaticDpsSource
    from mem_probe.il2cpp.mem_entity_provider import MemEntityProvider
    from mem_probe.il2cpp.mem_boss_action_reader import BossDurationProbe

    src = StaticDpsSource(dump_id="fdc7111b")
    prov = MemEntityProvider(src)
    ecr = prov._ecr
    pm = prov._pm
    probe = BossDurationProbe(pm)
    try:
        from tools.tablekit.name_tables import names as NR
    except Exception:
        NR = None

    def resolve_boss():
        snap = prov.snapshot()
        if not snap:
            return None
        if NAME_SUB and NR:
            for e in snap:
                if NAME_SUB in (NR.monster(int(e.get("base_id") or 0), default="") or ""):
                    return e
        return max(snap, key=lambda e: e["max_hp"])

    boss = resolve_boss()
    if not boss:
        print("[diag] no boss"); return 1
    obj = int(boss["obj"])
    print(f"[diag] boss base={boss.get('base_id')} obj={hex(obj)} -- polling {DURATION:.0f}s\n")

    series = {}        # attr_id -> ordered list of distinct transitions [(val)]
    seqs = {}          # attr_id -> full value sequence (for monotonic test)
    skill100_max = 0
    all_buffs = set()
    cbuffs = set()
    last_reloc = time.time()
    t0 = time.time()
    period = 1.0 / HZ
    n = 0
    while time.time() - t0 < DURATION:
        n += 1
        if time.time() - last_reloc > 0.4:
            last_reloc = time.time()
            b = resolve_boss()
            if b:
                obj = int(b["obj"])
        try:
            amap = ecr.read_attr_map(obj)
        except Exception:
            amap = {}
        sk = amap.get(A_SKILL)
        if isinstance(sk, (int, float)):
            skill100_max = max(skill100_max, int(sk))
        for k, v in amap.items():
            if not isinstance(v, (int, float)):
                continue
            seqs.setdefault(k, []).append(v)
        for base, _d in read_buff_list(pm, obj, ENT_BUFFCOMP_OFF):
            all_buffs.add(base)
        for base, _d in read_buff_list(pm, obj, ENT_CLIENTBUFF_OFF):
            cbuffs.add(base)
        time.sleep(period)

    print(f"[diag] {n} ticks. attr 100 (cast_skill_id) max seen = {skill100_max}")
    print(f"[diag] buffComp base_ids seen: {sorted(all_buffs) or '(none)'}")
    print(f"[diag] clientBuffComp base_ids seen: {sorted(cbuffs) or '(none)'}\n")

    def is_monotonic(seq):
        ups = sum(1 for i in range(1, len(seq)) if seq[i] > seq[i-1])
        downs = sum(1 for i in range(1, len(seq)) if seq[i] < seq[i-1])
        return ups == 0 or downs == 0   # strictly non-decreasing or non-increasing

    print("[diag] attrs cycling a SMALL REPEATING set (skill-id / skill-type candidates):")
    found = 0
    for k in sorted(seqs.keys()):
        seq = seqs[k]
        distinct = sorted(set(seq))
        if not (2 <= len(distinct) <= 8):
            continue
        if max(abs(x) for x in distinct) >= 1e9:    # exclude epoch/HP/big timers
            continue
        if is_monotonic(seq):                        # exclude countdown timers / HP
            continue
        # require a value to recur after a different one (categorical, not noise)
        recurs = any(seq[i] == seq[j] and any(seq[m] != seq[i] for m in range(i+1, j))
                     for i in range(len(seq)) for j in range(i+2, min(i+200, len(seq))))
        if not recurs:
            continue
        found += 1
        print(f"   attr {k}: values={distinct}"
              + (f"  names={[ (NR.skill(int(x),default='') if NR else '') for x in distinct]}"
                 if max(distinct) > 1000 else ""))
        if found > 30:
            print("   ..."); break
    if not found:
        print("   (NONE) -- no attr cycles a small skill-id set; skill identity is not in")
        print("   the attr cache. Discriminator would have to come from buffs/animation.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        import traceback; traceback.print_exc(); print(f"[diag] ERROR: {exc}"); sys.exit(1)
