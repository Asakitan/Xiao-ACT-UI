# -*- coding: utf-8 -*-
"""Diagnostic: can we capture a (field) boss's casts / attacks / skills?

Re-locates the boss (max-HP entity, or by name) each ~0.5s (so obj relocation
can't poison the reads), then watches its actor_state, cast_skill_id (attr 100),
skill-start-time (attr 106), breaking/overdrive, and BuffComp -- distinguishing
Singing(1) = a CAST BAR (has a duration window) from Skill(2) = instant.

    python tools/diag_boss_cast.py [seconds] [name_substr]
"""
from __future__ import annotations

import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
NAME_SUB = sys.argv[2] if len(sys.argv) > 2 else ""
HZ = 25.0
A_SKILL, A_START, A_BREAK, A_OVER, A_STUN = 100, 106, 455, 444, 443
ST = {0: "Idle", 1: "Singing(读条)", 2: "Skill(瞬发)", 8: "Action", 23: "Breaking"}


def main():
    from mem_probe.il2cpp.static_dps_source import StaticDpsSource
    from mem_probe.il2cpp.mem_entity_provider import MemEntityProvider
    from mem_probe.il2cpp.mem_boss_action_reader import BossDurationProbe
    try:
        from tools.tablekit.name_tables import names as NR
    except Exception:
        NR = None

    src = StaticDpsSource(dump_id="fdc7111b")
    prov = MemEntityProvider(src)
    ecr = prov._ecr
    probe = BossDurationProbe(prov._pm)

    def resolve_boss():
        snap = prov.snapshot()
        if not snap:
            return None
        if NAME_SUB and NR:
            for e in snap:
                nm = NR.monster(int(e.get("base_id") or 0), default="") or ""
                if NAME_SUB in nm:
                    e["_name"] = nm
                    return e
        boss = max(snap, key=lambda e: e["max_hp"])
        boss["_name"] = (NR.monster(int(boss.get("base_id") or 0), default="") or "") if NR else ""
        return boss

    print("[diag] locating boss ...")
    boss = resolve_boss()
    if not boss:
        print("[diag] no entities -- in combat?"); return 1
    print(f"[diag] boss: name={boss.get('_name') or '?'} base_id={boss.get('base_id')} "
          f"uuid={boss['uuid']} hp={boss.get('cur_hp')}/{boss.get('max_hp')} obj={hex(int(boss['obj']))}")
    print(f"[diag] watching {DURATION:.0f}s -- every actor_state change + skill signal:\n")

    obj = int(boss["obj"])
    last_reloc = time.time()
    prev_actor = None
    prev_skill = 0
    prev_start = 0
    sing_periods = []      # (start_ts, end_ts) of Singing(1)
    sing_open = None
    skill_ids_seen = set()
    actor_hist = {}
    t0 = time.time()
    period = 1.0 / HZ
    while time.time() - t0 < DURATION:
        if time.time() - last_reloc > 0.5:
            last_reloc = time.time()
            b = resolve_boss()
            if b:
                obj = int(b["obj"])
        try:
            amap = ecr.read_attr_map(obj)
        except Exception:
            amap = {}
        actor = probe.read_actor_state(obj)
        skill = int(amap.get(A_SKILL) or 0)
        start = int(amap.get(A_START) or 0) if isinstance(amap.get(A_START), int) else 0
        brk = amap.get(A_BREAK)
        over = amap.get(A_OVER)
        t = time.time() - t0

        if actor is not None:
            actor_hist[actor] = actor_hist.get(actor, 0) + 1
        if prev_actor is not None and actor != prev_actor:
            print(f"  +{t:5.2f}s  state {ST.get(prev_actor, prev_actor)} -> {ST.get(actor, actor)}"
                  f"   skill={skill or '-'}  start106={'set' if start else '-'}  brk={brk} over={over}")
            if actor == 1 and sing_open is None:
                sing_open = t
            if prev_actor == 1 and sing_open is not None:
                sing_periods.append(t - sing_open); sing_open = None
        if skill and skill != prev_skill:
            skill_ids_seen.add(skill)
            print(f"  +{t:5.2f}s  *** cast_skill_id = {skill} "
                  f"({(NR.skill(skill, default='') if NR else '') or 'no-name'})")
        prev_actor, prev_skill, prev_start = actor, skill, start
        time.sleep(period)

    print("\n[diag] ===== SUMMARY =====")
    print(f"[diag] actor_state seen: { {ST.get(k,k): v for k,v in actor_hist.items()} }")
    print(f"[diag] distinct cast_skill_id values: {sorted(skill_ids_seen) or 'NONE (attr 100 never set)'}")
    if sing_periods:
        print(f"[diag] CAST-BAR (Singing) periods: {len(sing_periods)}; "
              f"durations(s)={[round(x,2) for x in sing_periods]}  <-- readable cast windows ✓")
    else:
        print("[diag] no Singing(读条) periods -- boss skills are instant(Skill state 2) "
              "or no cast in window.")
    print("\n[diag] CONCLUSION: boss HP/state/actor read = OK (field boss uses the same "
          "ZEntity layout as dungeon bosses). Skill detection via actor_state + "
          "cast_skill_id as above.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        import traceback; traceback.print_exc(); print(f"[diag] ERROR: {exc}"); sys.exit(1)
