# -*- coding: utf-8 -*-
"""Selftest for BossDurationProbe (BuffComp Duration path + actor state).

Synthesizes a BuffComp -> ZList<BuffItem> chain in a fake address space and
verifies the probe reads BuffItem.Duration and picks a plausible cast buff.
No game / pymem needed.

    python tools/boss_duration_probe_selftest.py
"""
from __future__ import annotations

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from plugins.star_resonance_plugin.mem.il2cpp.mem_boss_action_reader import (  # noqa: E402
    BossDurationProbe,
    ENT_BUFFCOMP_OFF, ENT_STATEMACHINE_OFF, SM_CURSTATE_OFF, ACTOR_STATE_SINGING,
    BUFFCOMP_LIST_OFF, ZLIST_ITEMS_OFF, ZLIST_SIZE_OFF, ARR_LEN_OFF, ARR_ELEMS_OFF,
    BUFFITEM_UUID_OFF, BUFFITEM_BASEID_OFF, BUFFITEM_CREATE_OFF, BUFFITEM_DURATION_OFF,
)

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


class _FakeMem:
    def __init__(self):
        self.b = {}

    def w(self, addr, val, n):
        v = int(val) & ((1 << (8 * n)) - 1)
        for i in range(n):
            self.b[addr + i] = (v >> (8 * i)) & 0xFF

    def w64(self, a, v):
        self.w(a, v, 8)

    def w32(self, a, v):
        self.w(a, v, 4)

    def _read(self, a, n):
        return bytes(self.b.get(a + i, 0) for i in range(n))

    def read_u64(self, a):
        return int.from_bytes(self._read(a, 8), "little")

    def read_u32(self, a):
        return int.from_bytes(self._read(a, 4), "little")

    def read_i64(self, a):
        v = self.read_u64(a)
        return v - (1 << 64) if v >= (1 << 63) else v

    def read_i32(self, a):
        v = self.read_u32(a)
        return v - (1 << 32) if v >= (1 << 31) else v


def _build(items):
    """Lay out ENT -> BuffComp -> ZList -> BuffItem[]. Returns (mem, ent_addr)."""
    mem = _FakeMem()
    ENT, COMP, ZLIST, ARR = 0x100000, 0x200000, 0x300000, 0x400000
    SM = 0x600000
    mem.w64(ENT + ENT_BUFFCOMP_OFF, COMP)
    mem.w64(ENT + ENT_STATEMACHINE_OFF, SM)
    mem.w32(SM + SM_CURSTATE_OFF, ACTOR_STATE_SINGING)
    mem.w64(COMP + BUFFCOMP_LIST_OFF, ZLIST)
    mem.w32(ZLIST + ZLIST_SIZE_OFF, len(items))
    mem.w64(ZLIST + ZLIST_ITEMS_OFF, ARR)
    mem.w32(ARR + ARR_LEN_OFF, len(items))
    for i, it in enumerate(items):
        bi = 0x500000 + i * 0x1000
        mem.w64(ARR + ARR_ELEMS_OFF + i * 8, bi)
        mem.w32(bi + BUFFITEM_UUID_OFF, it["uuid"])
        mem.w32(bi + BUFFITEM_BASEID_OFF, it["base_id"])
        mem.w(bi + BUFFITEM_CREATE_OFF, it["create_ms"], 8)
        mem.w(bi + BUFFITEM_DURATION_OFF, it["duration_ms"], 8)
    return mem, ENT


def test_read_buffs():
    print("[read buffs]")
    mem, ent = _build([
        {"uuid": 7, "base_id": 9001, "create_ms": 5000, "duration_ms": 1500},
        {"uuid": 8, "base_id": 9002, "create_ms": 4000, "duration_ms": 30000},  # passive
    ])
    p = BossDurationProbe(mem)
    buffs = p.read_buffs(ent)
    check("two buffs read", len(buffs) == 2, repr(buffs))
    check("duration decoded", buffs[0]["duration_ms"] == 1500 and buffs[0]["base_id"] == 9001)


def test_actor_state():
    print("[actor state]")
    mem, ent = _build([])
    p = BossDurationProbe(mem)
    check("actor_state == Singing(1)", p.read_actor_state(ent) == ACTOR_STATE_SINGING)


def test_pick_cast_buff():
    print("[pick cast buff]")
    mem, ent = _build([
        {"uuid": 7, "base_id": 9001, "create_ms": 5000, "duration_ms": 1500},   # the cast buff
        {"uuid": 8, "base_id": 9002, "create_ms": 4000, "duration_ms": 30000},  # too long (passive)
    ])
    p = BossDurationProbe(mem)
    buffs = p.read_buffs(ent)
    # baseline empty -> both are "new"; only the in-range one is picked
    check("picks plausible cast buff = 1500", p.pick_cast_buff(buffs, set()) == 1500,
          repr(p.pick_cast_buff(buffs, set())))
    # if the cast buff was already present (in baseline), nothing is picked
    check("baseline excludes existing", p.pick_cast_buff(buffs, {7, 8}) is None)


def test_empty_safe():
    print("[empty / bad safe]")
    p = BossDurationProbe(_FakeMem())
    check("no buffcomp -> []", p.read_buffs(0x100000) == [])
    check("bad addr -> None state", p.read_actor_state(0) is None)


def main():
    test_read_buffs()
    test_actor_state()
    test_pick_cast_buff()
    test_empty_safe()
    print(f"\n{_passed} passed, {_failed} failed")
    return 1 if _failed else 0


if __name__ == "__main__":
    sys.exit(main())
