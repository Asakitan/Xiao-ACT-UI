# -*- coding: utf-8 -*-
"""Selftest for mem_boss_action_reader.BossActionTracker / BossDurationProbe.

Validates cast edge detection, chained casts, the learned-duration EMA, the
BuffComp duration upgrade, fast-path low-latency edges, pruning, and JSON safety.
No game / process needed (stubs).

    python tools/boss_action_tracker_selftest.py
"""
from __future__ import annotations

import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from plugins.star_resonance_plugin.mem.il2cpp.mem_boss_action_reader import BossActionTracker, BossDurationProbe  # noqa: E402

_passed = 0
_failed = 0


def check(name, cond, detail=""):
    global _passed, _failed
    if cond:
        _passed += 1
        print(f"  [ok] {name}")
    else:
        _failed += 1
        print(f"  [FAIL] {name} :: {detail}")


class _StubProbe(BossDurationProbe):
    def __init__(self):
        super().__init__(pm=None)
        self.buffs = []
        self.state = None

    def read_buffs(self, obj):
        return list(self.buffs)

    def read_actor_state(self, obj):
        return self.state


class _Names:
    def skill(self, sid, default=""):
        return {100: "横扫", 200: "俯冲"}.get(int(sid), default)


def _ent(uuid, skill, **kw):
    base = {"uuid": uuid, "base_id": 114, "name": "雷沃兰德", "obj": 0x100000,
            "cast_skill_id": skill, "cur_hp": 500, "max_hp": 1000, "hp_pct": 0.5,
            "breaking_stage": None, "overdrive": None, "stun": None, "extinction": None}
    base.update(kw)
    return base


def test_edges():
    print("[edge detection]")
    events = []
    tr = BossActionTracker(None, name_resolver=_Names(),
                           duration_probe=_StubProbe(), on_event=events.append)
    # no cast
    tr.update([_ent(1, 0)], None)
    check("idle no event", len(events) == 0)
    # start
    tr.update([_ent(1, 100)], None)
    check("cast_start fired", len(events) == 1 and events[-1]["cast_edge"] == "start")
    check("skill name resolved", events[-1]["skill_name"] == "横扫")
    check("cast_active true", events[-1]["cast_active"] is True)
    # hold -> no new edge
    tr.update([_ent(1, 100)], None)
    check("hold no new event", len(events) == 1)
    # end
    tr.update([_ent(1, 0)], None)
    check("cast_end fired", len(events) == 2 and events[-1]["cast_edge"] == "end")
    check("cast_active false on end", events[-1]["cast_active"] is False)


def test_chain():
    print("[chained cast]")
    events = []
    tr = BossActionTracker(None, name_resolver=_Names(),
                           duration_probe=_StubProbe(), on_event=events.append)
    tr.update([_ent(1, 100)], None)
    tr.update([_ent(1, 200)], None)   # 100 -> 200 directly = a fresh start
    check("chain emits start", events[-1]["cast_edge"] == "start" and events[-1]["skill_id"] == 200)
    check("two events total", len(events) == 2)


def test_learned_duration():
    print("[learned duration EMA]")
    tr = BossActionTracker(None, name_resolver=_Names(), duration_probe=_StubProbe())
    tr.update([_ent(1, 100)], None)        # start
    time.sleep(0.25)                       # observed ~250ms (>=200 floor)
    tr.update([_ent(1, 0)], None)          # end -> learn
    recs = tr.update([_ent(1, 100)], None) # start again -> should emit learned
    rec = recs[0]
    check("2nd cast has duration", rec["cast_duration_ms"] is not None, repr(rec["cast_duration_ms"]))
    check("src=learned", rec["cast_duration_src"] == "learned", rec["cast_duration_src"])
    check("learned in range", 200 <= (rec["cast_duration_ms"] or 0) <= 400, repr(rec["cast_duration_ms"]))


def test_buff_duration():
    print("[buff duration upgrade]")
    probe = _StubProbe()
    tr = BossActionTracker(None, name_resolver=_Names(), duration_probe=probe)
    probe.buffs = []                        # baseline at cast-start: no buffs
    # buff-duration upgrade is boss-only (non-boss buff reads were the crowd hot
    # path), so the tracked entity must be the boss here.
    tr.update([_ent(1, 100)], _ent(1, 100))         # start (baseline captured = {})
    # the cast-channel buff now appears with a real Duration
    probe.buffs = [{"uuid": 7, "base_id": 9001, "create_ms": 5000, "duration_ms": 1500}]
    recs = tr.update([_ent(1, 100)], _ent(1, 100))  # still casting -> probe upgrades
    rec = recs[0] if recs else tr.boss_action(1)
    check("src=buff", rec["cast_duration_src"] == "buff", rec["cast_duration_src"])
    check("duration=1500", rec["cast_duration_ms"] == 1500, repr(rec["cast_duration_ms"]))
    # implausible buff is ignored
    probe2 = _StubProbe()
    tr2 = BossActionTracker(None, duration_probe=probe2)
    probe2.buffs = []
    tr2.update([_ent(2, 100)], _ent(2, 100))
    probe2.buffs = [{"uuid": 8, "base_id": 1, "create_ms": 1, "duration_ms": 99999}]  # too long
    tr2.update([_ent(2, 100)], _ent(2, 100))
    check("implausible buff ignored", tr2.boss_action(2)["cast_duration_src"] != "buff")


def test_fast_path():
    print("[fast path]")
    events = []
    tr = BossActionTracker(None, name_resolver=_Names(),
                           duration_probe=_StubProbe(), on_event=events.append)
    # rich tick first to cache base_id/name
    tr.update([_ent(5, 0)], None)
    rec = tr.update_fast(5, 100, actor_state=1, obj=0x100000)
    check("fast cast_start", rec is not None and rec["cast_edge"] == "start")
    check("fast keeps base_id", rec["boss_base_id"] == 114)
    check("fast actor_state", rec["actor_state"] == 1)
    # rich tick now sees skill already held -> no duplicate edge
    before = len(events)
    tr.update([_ent(5, 100)], None)
    check("rich no dup edge after fast", len(events) == before)


def test_prune_and_json():
    print("[prune + json safety]")
    tr = BossActionTracker(None, duration_probe=_StubProbe())
    big = 1 << 60
    tr.update([_ent(big, 100)], None)
    rec = tr.boss_action(big)
    check("big uuid kept as int", isinstance(rec["boss_uuid"], int) and rec["boss_uuid"] == big)
    # entity disappears while idle -> pruned
    tr.update([_ent(big, 0)], None)         # end
    tr.update([], None)                      # gone
    check("idle departed pruned", tr.boss_action(big) is None)


def test_boss_only_buff_reads():
    """Perf guard: reading buffs (RPM loop) for EVERY casting entity in a crowd was
    the per-tick O(N) GIL-holding hot path that made hybrid laggy with many players.
    Only the boss may read buffs; non-boss casters use the cheap snapshot path."""
    print("[boss-only buff reads]")

    class _CountProbe(BossDurationProbe):
        def __init__(self):
            super().__init__(pm=None)
            self.reads = 0

        def read_actor_state(self, obj):
            return 0

        def read_buffs(self, obj):
            self.reads += 1
            return []

        def pick_cast_buff(self, buffs, baseline):
            return None

    probe = _CountProbe()
    tr = BossActionTracker(None, duration_probe=probe)
    boss = _ent(999, 700)
    crowd = [_ent(u, 500) for u in range(1, 31)] + [boss]   # 30 casting non-boss + boss
    for _ in range(5):
        tr.update(crowd, boss)
    # 30 non-boss casters over 5 ticks would be 150+ read_buffs if not gated;
    # boss-only keeps it tiny (baseline + per-tick probe + detect_buff_skill).
    check("non-boss casters skip buff reads", probe.reads <= 20, f"{probe.reads} reads")


def main():
    test_edges()
    test_chain()
    test_learned_duration()
    test_buff_duration()
    test_fast_path()
    test_prune_and_json()
    test_boss_only_buff_reads()
    print(f"\n{_passed} passed, {_failed} failed")
    return 1 if _failed else 0


if __name__ == "__main__":
    sys.exit(main())
