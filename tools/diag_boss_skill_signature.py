# -*- coding: utf-8 -*-
"""Find the per-skill discriminator: capture the boss's full attr_map + buffs at
each attack start and report which attr ids / buffs DIFFER between attacks (so we
can tell a 吼 from a 护盾 from a 普攻).

Tell me roughly which skill each attack was (order) and I can map the signature.

    python tools/diag_boss_skill_signature.py [seconds] [name_substr]
"""
from __future__ import annotations

import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 28.0
NAME_SUB = sys.argv[2] if len(sys.argv) > 2 else ""
HZ = 30.0
ENT_UUID_OFF = 0xC0


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
    pm = prov._pm
    probe = BossDurationProbe(pm)

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
    uuid = int(boss["uuid"]); obj = int(boss["obj"]); base = int(boss.get("base_id") or 0)
    print(f"[diag] boss base={base} uuid={uuid} obj={hex(obj)} -- capturing attack signatures\n")

    attacks = []        # list of (t, attrs_dict, buff_set)
    prev_actor = None
    last_reloc = time.time()
    last_beat = time.time()
    t0 = time.time()
    period = 1.0 / HZ
    while time.time() - t0 < DURATION and len(attacks) < 12:
        if time.time() - last_reloc > 0.4:
            last_reloc = time.time()
            b = resolve_boss()
            if b:
                obj = int(b["obj"]); uuid = int(b["uuid"])
        actor = probe.read_actor_state(obj)
        # heartbeat so we can see whether the boss is doing anything
        if time.time() - last_beat > 2.0:
            last_beat = time.time()
            print(f"  ...+{time.time()-t0:4.0f}s actor_state={actor}")
        # capture on entering an active skill state (Singing/Skill) from idle/other
        if prev_actor is not None and prev_actor in (0, None) and actor in (1, 2):
            amap = {}
            try:
                amap = {int(k): v for k, v in ecr.read_attr_map(obj).items()
                        if isinstance(v, (int, float))}
            except Exception:
                pass
            buffs = {b["base_id"] for b in probe.read_buffs(obj)}
            attacks.append((time.time() - t0, amap, buffs))
            print(f"  attack #{len(attacks)} at +{time.time()-t0:5.2f}s  state={actor}  buffs={sorted(buffs)}")
        prev_actor = actor
        time.sleep(period)

    print(f"\n[diag] captured {len(attacks)} attacks. finding discriminators ...\n")
    if len(attacks) < 2:
        print("[diag] need >=2 attacks"); return 0

    # attrs whose value differs across attacks (candidate skill-id / skill-type)
    all_keys = set()
    for _t, a, _b in attacks:
        all_keys |= set(a.keys())
    varying = {}
    for k in all_keys:
        vals = [a.get(k) for _t, a, _b in attacks]
        distinct = set(v for v in vals if v is not None)
        # skip HP/timestamp-like and always-same; keep attrs with 2..N distinct values
        if 2 <= len(distinct) <= len(attacks):
            varying[k] = vals

    # rank: prefer small integer-ish discriminators (skill ids), skip huge/epoch
    def _interesting(vals):
        nums = [v for v in vals if isinstance(v, (int, float))]
        if not nums:
            return False
        mx = max(abs(n) for n in nums)
        return mx < 1e9   # exclude epoch-ms (1.7e12) and big timers

    print("[diag] attr ids that DIFFER per attack (likely skill id / skill type):")
    shown = 0
    for k in sorted(varying.keys()):
        vals = varying[k]
        if not _interesting(vals):
            continue
        per = ", ".join(f"#{i+1}={v}" for i, v in enumerate(vals))
        print(f"   attr {k}: {per}")
        shown += 1
        if shown > 25:
            print("   ... (truncated)"); break
    if not shown:
        print("   (none in skill-id range -- discriminator may be buffs below)")

    print("\n[diag] buffs per attack (护盾/增益会在这里体现):")
    for i, (_t, _a, b) in enumerate(attacks):
        print(f"   attack #{i+1}: buff base_ids = {sorted(b) or '(none)'}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        import traceback; traceback.print_exc(); print(f"[diag] ERROR: {exc}"); sys.exit(1)
