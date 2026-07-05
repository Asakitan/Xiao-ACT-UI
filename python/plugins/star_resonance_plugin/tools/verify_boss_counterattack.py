# -*- coding: utf-8 -*-
# Live verification: detect a boss/dummy counterattack via the boss-action feed.
#
# Attaches read-only to star.exe, locates visible entities, fast-polls each one's
# cast_skill_id (attr 100) + actor_state, and reports every cast edge the
# BossActionTracker emits -- i.e. each counterattack the dummy performs.
#
# python tools/verify_boss_counterattack.py [seconds]
from __future__ import annotations

import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0
FAST_HZ = 18.0
SNAP_EVERY_S = 0.6


def main():
    from plugins.star_resonance_plugin.mem.il2cpp.static_dps_source import StaticDpsSource
    from plugins.star_resonance_plugin.mem.il2cpp.mem_entity_provider import MemEntityProvider
    from plugins.star_resonance_plugin.mem.il2cpp.mem_boss_action_reader import BossActionTracker, BossDurationProbe
    try:
        from tools.tablekit.name_tables import names as NR
    except Exception:
        NR = None

    print("[verify] opening StaticDpsSource (fdc7111b) ...")
    src = StaticDpsSource(dump_id="fdc7111b")
    prov = MemEntityProvider(src)
    probe = BossDurationProbe(prov._pm)
    tracker = BossActionTracker(prov._ecr, pm=prov._pm, name_resolver=NR, duration_probe=probe)

    print("[verify] first snapshot (locates ZEntityMgr, may take a few seconds) ...")
    t_locate0 = time.time()
    snap = prov.snapshot()
    print(f"[verify] located in {time.time() - t_locate0:.1f}s; {len(snap)} combat entities visible")
    if not snap:
        print("[verify] NO combat entities -- are you in combat / is the dummy visible?")
        return 1

    # identity cache uuid -> (obj, base_id, name, max_hp)
    def _name(e):
        bid = int(e.get("base_id") or 0)
        nm = ""
        if NR and bid:
            try:
                nm = NR.monster(bid, default="") or ""
            except Exception:
                nm = ""
        return nm
    ents = {}
    for e in snap:
        ents[int(e["uuid"])] = {"obj": int(e["obj"]), "base_id": int(e.get("base_id") or 0),
                                "name": _name(e), "max_hp": int(e.get("max_hp") or 0)}
    print("[verify] entities:")
    for uuid, m in sorted(ents.items(), key=lambda kv: -kv[1]["max_hp"]):
        print(f"   uuid={uuid} base_id={m['base_id']} name={m['name'] or '?'} max_hp={m['max_hp']:,} obj={hex(m['obj'])}")

    # feed the rich snapshot once so the tracker has base_id/name
    for e in snap:
        e.setdefault("name", _name(e))
    tracker.update(snap, max(snap, key=lambda e: e["max_hp"]))

    print(f"\n[verify] watching cast edges for {DURATION:.0f}s (fast poll {FAST_HZ:.0f}Hz)...")
    print("[verify] each line = a detected skill cast (the dummy's counterattack):\n")

    starts = []      # (ts, uuid, skill_id, name, actor, dur, src)
    last_snap = time.time()
    t0 = time.time()
    period = 1.0 / FAST_HZ
    while time.time() - t0 < DURATION:
        # refresh the entity list periodically (new mobs / obj changes)
        if time.time() - last_snap > SNAP_EVERY_S:
            last_snap = time.time()
            try:
                s2 = prov.snapshot()
                for e in s2:
                    ents[int(e["uuid"])] = {"obj": int(e["obj"]), "base_id": int(e.get("base_id") or 0),
                                            "name": _name(e), "max_hp": int(e.get("max_hp") or 0)}
                    e.setdefault("name", _name(e))
                if s2:
                    tracker.update(s2, max(s2, key=lambda e: e["max_hp"]))
            except Exception:
                pass
        # fast-poll each entity's cast skill id + actor state
        for uuid, m in list(ents.items()):
            obj = m["obj"]
            try:
                c = prov._ecr.read_combat(obj)
            except Exception:
                c = None
            if c is None:
                continue
            skill = int(c.get("cast_skill_id") or 0)
            actor = probe.read_actor_state(obj)
            rec = tracker.update_fast(uuid, skill, actor, obj=obj)
            if rec is not None and rec["cast_edge"] == "start":
                nm = m["name"] or rec.get("boss_name") or "?"
                sname = rec.get("skill_name") or ""
                dur = rec.get("cast_duration_ms")
                durs = f"{dur}ms({rec.get('cast_duration_src')})" if dur else "?"
                starts.append((time.time() - t0, uuid, skill, sname, actor, durs))
                print(f"  +{time.time()-t0:5.2f}s  CAST  {nm}#{m['base_id']}  skill={skill} {sname}"
                      f"  actor_state={actor}  dur={durs}")
        time.sleep(period)

    # summary
    print("\n[verify] ===== SUMMARY =====")
    print(f"[verify] total casts detected: {len(starts)} over {DURATION:.0f}s "
          f"= {len(starts)/DURATION:.2f}/sec")
    if starts:
        by_skill = {}
        for _ts, _u, sk, snm, _a, _d in starts:
            by_skill.setdefault(sk, [snm, 0])[1] += 1
        print("[verify] casts by skill_id:")
        for sk, (snm, cnt) in sorted(by_skill.items(), key=lambda kv: -kv[1][1]):
            print(f"   skill={sk} {snm or '(no name)'}  x{cnt}")
        gaps = [starts[i][0] - starts[i-1][0] for i in range(1, len(starts))]
        if gaps:
            print(f"[verify] inter-cast gap: avg {sum(gaps)/len(gaps):.2f}s "
                  f"min {min(gaps):.2f}s max {max(gaps):.2f}s")
        actors = {s[4] for s in starts}
        print(f"[verify] actor_state values seen at cast: {sorted(a for a in actors if a is not None)}  "
              f"(1 == Singing/casting)")
        durs = [s[5] for s in starts if not s[5].startswith('?')]
        print(f"[verify] cast_duration resolved on {len(durs)}/{len(starts)} casts "
              f"(buff=real ms, learned=measured): "
              f"{durs[:5]}{'...' if len(durs) > 5 else ''}")
        print("\n[verify] RESULT: the boss-action feed DETECTS the counterattack. ✓")
    else:
        print("[verify] RESULT: no cast edges detected. The dummy's counterattack may not")
        print("         flip attr 100 (cast_skill_id), or casts are <1 frame. Check entities above.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        import traceback
        traceback.print_exc()
        print(f"[verify] ERROR: {exc}")
        sys.exit(1)
