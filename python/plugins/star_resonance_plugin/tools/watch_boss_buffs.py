# -*- coding: utf-8 -*-
# Watch boss buffs live: print every buff that appears on a boss + resolve its
# name (boss_skill / mechanic / buff). Boss cast-bar skills live in the buff list.
#
# python tools/watch_boss_buffs.py [seconds]
from __future__ import annotations

import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 18.0
HZ = 30.0
ENT_BUFFCOMP_OFF, ENT_CLIENTBUFF_OFF = 0x98, 0xA0
LIST_OFF, ITEMS_OFF, SIZE_OFF = 0x30, 0x18, 0x20
ARR_LEN_OFF, ARR_ELEMS_OFF = 0x18, 0x20
BI_UUID_OFF, BI_BASE_OFF, BI_DUR_OFF = 0x10, 0x14, 0x38


def buffs(pm, ent, comp_off):
    out = []
    comp = pm.read_u64(ent + comp_off)
    if not comp:
        return out
    zl = pm.read_u64(comp + LIST_OFF)
    if not zl:
        return out
    size = pm.read_u32(zl + SIZE_OFF) or 0
    items = pm.read_u64(zl + ITEMS_OFF)
    if not items or size <= 0:
        return out
    for i in range(min(int(size), 64)):
        bi = pm.read_u64(items + ARR_ELEMS_OFF + i * 8)
        if bi:
            out.append((int(pm.read_u32(bi + BI_UUID_OFF) or 0),
                        int(pm.read_u32(bi + BI_BASE_OFF) or 0),
                        int(pm.read_i64(bi + BI_DUR_OFF) or 0)))
    return out


def main():
    from plugins.star_resonance_plugin.mem.il2cpp.static_dps_source import StaticDpsSource
    from plugins.star_resonance_plugin.mem.il2cpp.mem_entity_provider import MemEntityProvider
    from plugins.star_resonance_plugin.mem.il2cpp.mem_boss_action_reader import BossDurationProbe
    from tools.tablekit.name_tables import names as N

    def bname(bid):
        for m in ("boss_mechanic_skill", "boss_skill", "boss_mechanic", "buff", "skill", "monster_skill"):
            fn = getattr(N, m, None)
            if fn:
                try:
                    nm = fn(int(bid), default="")
                    if nm:
                        return f"{nm} [{m}]"
                except Exception:
                    pass
        return ""

    src = StaticDpsSource(dump_id="fdc7111b")
    prov = MemEntityProvider(src)
    pm = prov._pm
    probe = BossDurationProbe(pm)

    snap = prov.snapshot()
    if not snap:
        print("[buffs] no entities"); return 1
    # the two highest-HP entities are the raid bosses
    bosses = sorted(snap, key=lambda e: -e["max_hp"])[:3]
    targets = {}
    for b in bosses:
        nm = N.monster(int(b.get("base_id") or 0), default="") or f"#{b.get('base_id')}"
        targets[int(b["uuid"])] = {"obj": int(b["obj"]), "name": nm, "base": int(b.get("base_id") or 0)}
        print(f"[buffs] boss: {nm} base={b.get('base_id')} hp={b.get('cur_hp')}/{b.get('max_hp')} obj={hex(int(b['obj']))}")
    print(f"\n[buffs] watching {DURATION:.0f}s -- new buffs (= boss skills/mechanics):\n")

    seen = {u: set() for u in targets}
    last_actor = {u: None for u in targets}
    last_reloc = time.time()
    t0 = time.time()
    period = 1.0 / HZ
    while time.time() - t0 < DURATION:
        if time.time() - last_reloc > 0.4:
            last_reloc = time.time()
            s2 = prov.snapshot()
            if s2:
                for b in sorted(s2, key=lambda e: -e["max_hp"])[:3]:
                    u = int(b["uuid"])
                    if u in targets:
                        targets[u]["obj"] = int(b["obj"])
        for u, m in targets.items():
            obj = m["obj"]
            act = probe.read_actor_state(obj)
            if act != last_actor[u] and act in (1, 2):
                last_actor[u] = act
            cur = buffs(pm, obj, ENT_BUFFCOMP_OFF) + [(("c"+str(x[0])), x[1], x[2]) for x in buffs(pm, obj, ENT_CLIENTBUFF_OFF)]
            for uuid, base, dur in cur:
                key = (uuid, base)
                if key in seen[u] or base <= 0:
                    continue
                seen[u].add(key)
                nm = bname(base)
                print(f"  +{time.time()-t0:5.2f}s  {m['name']}  +BUFF base={base} dur={dur}ms  state={last_actor[u]}  -> {nm or '(no name in tables)'}")
        time.sleep(period)

    print("\n[buffs] SUMMARY: distinct buff base_ids per boss:")
    for u, m in targets.items():
        bases = sorted({b for _uu, b in seen[u]})
        print(f"  {m['name']}: {bases or '(none seen)'}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        import traceback; traceback.print_exc(); print(f"[buffs] ERROR: {exc}"); sys.exit(1)
