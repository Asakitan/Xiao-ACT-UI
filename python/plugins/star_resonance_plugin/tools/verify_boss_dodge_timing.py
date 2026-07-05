# -*- coding: utf-8 -*-
# Verify the dodge-timing the feed gives for a real boss: detect each attack
# start AND whether a usable cast_duration_ms is captured (learned from the
# attack's actor-state window) so a dodge can be timed with lead_ms.
#
# python tools/verify_boss_dodge_timing.py [seconds] [name_substr]
from __future__ import annotations

import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 18.0
NAME_SUB = sys.argv[2] if len(sys.argv) > 2 else ""
HZ = 25.0


def main():
    from plugins.star_resonance_plugin.mem.il2cpp.static_dps_source import StaticDpsSource
    from plugins.star_resonance_plugin.mem.il2cpp.mem_entity_provider import MemEntityProvider
    from plugins.star_resonance_plugin.mem.il2cpp.mem_boss_action_reader import BossActionTracker, BossDurationProbe
    try:
        from tools.tablekit.name_tables import names as NR
    except Exception:
        NR = None

    src = StaticDpsSource(dump_id="fdc7111b")
    prov = MemEntityProvider(src)
    ecr = prov._ecr
    probe = BossDurationProbe(prov._pm)
    tracker = BossActionTracker(prov._ecr, pm=prov._pm, name_resolver=NR, duration_probe=probe)

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
        print("[verify] no boss"); return 1
    uuid = int(boss["uuid"]); obj = int(boss["obj"])
    nm = (NR.monster(int(boss.get("base_id") or 0), default="") if NR else "") or "?"
    print(f"[verify] boss {nm}#{boss.get('base_id')} hp={boss.get('cur_hp')}/{boss.get('max_hp')}")
    print(f"[verify] watching {DURATION:.0f}s -- each attack start + its captured duration:\n")
    # seed identity
    tracker.update([{**boss, "name": nm}], boss)

    starts = 0
    with_dur = 0
    last_reloc = time.time()
    t0 = time.time()
    period = 1.0 / HZ
    while time.time() - t0 < DURATION:
        if time.time() - last_reloc > 0.5:
            last_reloc = time.time()
            b = resolve_boss()
            if b:
                obj = int(b["obj"]); uuid = int(b["uuid"])
        try:
            c = ecr.read_combat(obj)
        except Exception:
            c = None
        skill = int((c or {}).get("cast_skill_id") or 0)
        actor = probe.read_actor_state(obj)
        rec = tracker.update_fast(uuid, skill, actor, obj=obj)
        if rec is not None and rec["cast_edge"] == "start":
            starts += 1
            dur = rec.get("cast_duration_ms")
            srcd = rec.get("cast_duration_src")
            if dur:
                with_dur += 1
            t = time.time() - t0
            print(f"  +{t:5.2f}s  ATTACK start  skill={rec['skill_id'] or '(no-id)'}"
                  f"  duration={dur or '?'}ms ({srcd})  <- dodge can fire at start+duration-lead")
        time.sleep(period)

    print("\n[verify] ===== SUMMARY =====")
    print(f"[verify] attacks detected: {starts} over {DURATION:.0f}s = {starts/DURATION:.2f}/sec")
    print(f"[verify] attacks with a captured duration window: {with_dur}/{starts}")
    if with_dur:
        print("[verify] RESULT: boss attacks detected WITH a timing window -> dodge can be "
              "timed (react at start, or lead_ms before the window ends). ✓")
    elif starts:
        print("[verify] RESULT: attacks detected, but duration not yet learned (need >=1 "
              "completed attack; instant <200ms attacks have no window -> react on start).")
    else:
        print("[verify] RESULT: no attack starts in window.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        import traceback; traceback.print_exc(); print(f"[verify] ERROR: {exc}"); sys.exit(1)
