# -*- coding: utf-8 -*-
"""Diagnostic: what memory signal does the 精英守护木桩 counterattack produce?

Locates the dummy (base_id 122) and polls its full attr_map + actor_state +
BuffComp at high rate, logging every value that changes -- so we can see which
attr id / state / buff its ~1/sec counterattack actually flips (attr 100 did not).

    python tools/diag_dummy_counterattack.py [seconds] [base_id]
"""
from __future__ import annotations

import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0
WANT_BASE = int(sys.argv[2]) if len(sys.argv) > 2 else 122   # 精英守护木桩
HZ = 30.0


def main():
    from plugins.star_resonance_plugin.mem.il2cpp.static_dps_source import StaticDpsSource
    from plugins.star_resonance_plugin.mem.il2cpp.mem_entity_provider import MemEntityProvider
    from plugins.star_resonance_plugin.mem.il2cpp.mem_boss_action_reader import BossDurationProbe

    src = StaticDpsSource(dump_id="fdc7111b")
    prov = MemEntityProvider(src)
    ecr = prov._ecr
    probe = BossDurationProbe(prov._pm)

    print(f"[diag] locating entity base_id={WANT_BASE} ...")
    snap = prov.snapshot()
    target = None
    for e in snap:
        if int(e.get("base_id") or 0) == WANT_BASE:
            target = e
            break
    if target is None:
        # fall back to the highest-HP enemy dummy
        cands = [e for e in snap if int(e.get("base_id") or 0) in (122, 115, 114)]
        target = max(cands, key=lambda e: e["max_hp"]) if cands else (snap[0] if snap else None)
    if target is None:
        print("[diag] no entity found"); return 1
    obj = int(target["obj"])
    uuid = int(target["uuid"])
    print(f"[diag] target uuid={uuid} base_id={target.get('base_id')} obj={hex(obj)} "
          f"hp={target.get('cur_hp')}/{target.get('max_hp')}")

    # full attr id -> name labels for known combat attrs
    LABEL = {11310: "HP", 11320: "MAXHP", 455: "BREAK_STAGE", 444: "OVERDRIVE",
             443: "STUN", 441: "EXTINCTION", 100: "SKILL_ID(cast)", 442: "MAX_STUN",
             440: "MAX_EXT", 1: "NAME"}

    print(f"[diag] polling {HZ:.0f}Hz for {DURATION:.0f}s; logging value changes ...\n")
    prev = None
    prev_actor = None
    prev_buffs = set()
    changes = {}       # attr_id -> change count
    actor_seen = {}    # actor_state -> count
    buff_events = 0
    t0 = time.time()
    period = 1.0 / HZ
    n = 0
    while time.time() - t0 < DURATION:
        n += 1
        try:
            amap = ecr.read_attr_map(obj)
        except Exception:
            amap = {}
        actor = probe.read_actor_state(obj)
        buffs = {b["uuid"] for b in probe.read_buffs(obj)}

        if actor is not None:
            actor_seen[actor] = actor_seen.get(actor, 0) + 1
        if prev_actor is not None and actor != prev_actor:
            print(f"  +{time.time()-t0:5.2f}s  actor_state {prev_actor} -> {actor}")
        prev_actor = actor

        new_buffs = buffs - prev_buffs
        if new_buffs and prev is not None:
            buff_events += 1
            for bu in list(new_buffs)[:4]:
                bd = next((b for b in probe.read_buffs(obj) if b["uuid"] == bu), None)
                print(f"  +{time.time()-t0:5.2f}s  +BUFF uuid={bu} base={bd['base_id'] if bd else '?'} "
                      f"dur={bd['duration_ms'] if bd else '?'}ms")
        prev_buffs = buffs

        if prev is not None:
            for k, v in amap.items():
                if k in (11310,):  # HP changes constantly from your dps; skip noise
                    continue
                if prev.get(k) != v:
                    changes[k] = changes.get(k, 0) + 1
                    lbl = LABEL.get(k, "")
                    print(f"  +{time.time()-t0:5.2f}s  attr {k}{('('+lbl+')') if lbl else ''}: "
                          f"{prev.get(k)} -> {v}")
        prev = amap
        time.sleep(period)

    print(f"\n[diag] ===== SUMMARY ({n} ticks over {DURATION:.0f}s) =====")
    print(f"[diag] actor_state distribution: {actor_seen}  (1=Singing 2=Skill 8=Action 23=Breaking)")
    print(f"[diag] new-buff events: {buff_events}")
    if changes:
        print("[diag] attrs that changed (excluding HP):")
        for k, c in sorted(changes.items(), key=lambda kv: -kv[1]):
            print(f"   attr {k} {LABEL.get(k,'')}: {c} changes ({c/DURATION:.1f}/s)")
    else:
        print("[diag] NO non-HP attr changed -- counterattack isn't in this entity's attr cache.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        import traceback; traceback.print_exc(); print(f"[diag] ERROR: {exc}"); sys.exit(1)
