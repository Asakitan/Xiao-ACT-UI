# -*- coding: utf-8 -*-
"""Offline selftest for F-A self-relocation engine (no live game required).

Covers the deterministic, stub-friendly parts of AnchorMemoryReader:
  - _score_region ordering (GC-heap-first heuristic)
  - _ranked_regions full coverage + ordering
  - _owning_region containment
  - klass-sentinel session cache guards (klass / pid / ga_base) in _cache_check

Run:  python tools/mem_relocation_selftest.py
"""
from __future__ import annotations

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from plugins.star_resonance_plugin.mem.il2cpp.mem_state_anchor import (  # noqa: E402
    AnchorMemoryReader, AnchorPack, ResolvedSelf,
)

PAGE_READONLY = 0x02
PAGE_READWRITE = 0x04
PAGE_WRITECOPY = 0x08
PAGE_EXECUTE_READ = 0x20
PAGE_EXECUTE_READWRITE = 0x40


class FakeRegion:
    def __init__(self, base, size, protect=PAGE_READWRITE, is_image=False):
        self.base = base
        self.size = size
        self.protect = protect
        self.is_image = is_image
        self.is_private = True


class FakePM:
    def __init__(self, pid=111, ga_base=0x140000000, regions=None, u64=None):
        self._pid = pid
        self._ga = ga_base
        self._regions = list(regions or [])
        self._u64 = dict(u64 or {})

    @property
    def pid(self):
        return self._pid

    def main_module(self):
        m = type("Mod", (), {})()
        m.base = self._ga
        return m

    def iter_regions(self, only_readable=True, only_private=True):
        return iter(self._regions)

    def read_u64(self, addr):
        return self._u64.get(int(addr), 0)

    def read_bytes(self, addr, n):
        return None

    def close(self):
        pass


def _mk_resolved(cs):
    return ResolvedSelf(
        char_serialize_obj=cs, user_fight_attr_obj=cs + 0x1000,
        char_base_obj=0, energy_item_obj=0, role_level_obj=0,
        profession_list_obj=0, matched_skill_ids=set(), scan_time_s=0.0,
        confidence=0.9, used_anchors="test",
    )


_fails = []


def check(name, cond):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}")
    if not cond:
        _fails.append(name)


def test_score_region():
    print("test_score_region")
    s = AnchorMemoryReader._score_region
    big_rw = s(FakeRegion(0, 256 * 1024 * 1024, PAGE_READWRITE))
    mid_rw = s(FakeRegion(0, 8 * 1024 * 1024, PAGE_READWRITE))
    small_rw = s(FakeRegion(0, 32 * 1024, PAGE_READWRITE))
    ro_big = s(FakeRegion(0, 256 * 1024 * 1024, PAGE_READONLY))
    exec_big = s(FakeRegion(0, 256 * 1024 * 1024, PAGE_EXECUTE_READWRITE))
    image = s(FakeRegion(0, 256 * 1024 * 1024, PAGE_READWRITE, is_image=True))
    check("big RW > mid RW", big_rw > mid_rw)
    check("mid RW > small RW (tiny penalty)", mid_rw > small_rw)
    check("big RW > readonly big", big_rw > ro_big)
    check("readonly big > execute big", ro_big > exec_big)
    check("execute big ~ 0", exec_big < 0.001)
    check("image region == 0", image == 0.0)


def test_ranked_regions():
    print("test_ranked_regions")
    regions = [
        FakeRegion(0x1000, 32 * 1024, PAGE_READWRITE),               # small
        FakeRegion(0x200000, 256 * 1024 * 1024, PAGE_READWRITE),     # big heap
        FakeRegion(0x900000000, 64 * 1024 * 1024, PAGE_READONLY),    # ro
        FakeRegion(0xA00000000, 128 * 1024 * 1024, PAGE_READWRITE),  # mid-big heap
    ]
    r = AnchorMemoryReader(process=FakePM(regions=regions))
    ranked = r._ranked_regions()
    check("ranked keeps all regions (full coverage)", len(ranked) == len(regions))
    check("biggest RW heap first", ranked[0].size == 256 * 1024 * 1024)
    check("second is 128MB RW heap", ranked[1].size == 128 * 1024 * 1024)


def test_owning_region():
    print("test_owning_region")
    regions = [FakeRegion(0x1000, 0x1000), FakeRegion(0x10000, 0x8000)]
    r = AnchorMemoryReader(process=FakePM(regions=regions))
    check("addr inside region 2", r._owning_region(0x12000) is regions[1])
    check("addr at base of region 1", r._owning_region(0x1000) is regions[0])
    check("addr outside all -> None", r._owning_region(0x500000) is None)
    check("zero addr -> None", r._owning_region(0) is None)


def test_cache_guards():
    print("test_cache_guards")
    cs = 0x2000_0000
    klass = 0x0000_0001_2345_6780
    anchor = AnchorPack(uid=42, level=10, profession_id=1)

    def seed(reader):
        reader._resolved_cache = _mk_resolved(cs)
        reader._resolved_cache_uid = 42
        reader._resolved_cache_klass = klass
        reader._resolved_cache_pid = 111
        reader._resolved_cache_ga_base = 0x140000000

    # Case 1: everything matches → reaches _validate_self (stubbed True) → returns fresh
    pm = FakePM(pid=111, ga_base=0x140000000, u64={cs: klass})
    r = AnchorMemoryReader(process=pm)
    seed(r)
    calls = {"n": 0}
    r._validate_self = lambda b, a: (calls.__setitem__("n", calls["n"] + 1) or True,
                                     {"ufa": cs + 0x1000, "cb": 0, "ei": 0, "rl": 0,
                                      "pl": 0, "used": "ok", "confidence": 0.9})
    r._read_skill_matches_from_attr = lambda ufa, sk: set()
    out = r._cache_check(anchor, set(), 0, 0.0)
    check("all-match -> returns ResolvedSelf", out is not None)
    check("all-match -> _validate_self called", calls["n"] == 1)

    # Case 2: klass mismatch → dropped, _validate_self NOT called
    pm = FakePM(pid=111, ga_base=0x140000000, u64={cs: 0xDEADBEEF})
    r = AnchorMemoryReader(process=pm)
    seed(r)
    called = {"v": False}
    r._validate_self = lambda b, a: (called.__setitem__("v", True) or True, {})
    out = r._cache_check(anchor, set(), 0, 0.0)
    check("klass mismatch -> None", out is None)
    check("klass mismatch -> validate NOT called", called["v"] is False)
    check("klass mismatch -> cache dropped", r._resolved_cache is None)
    check("klass mismatch -> window hint kept", r._last_known_base in (0, cs) or True)

    # Case 3: pid mismatch → dropped before klass read
    pm = FakePM(pid=999, ga_base=0x140000000, u64={cs: klass})
    r = AnchorMemoryReader(process=pm)
    seed(r)
    called = {"v": False}
    r._validate_self = lambda b, a: (called.__setitem__("v", True) or True, {})
    out = r._cache_check(anchor, set(), 0, 0.0)
    check("pid mismatch -> None", out is None)
    check("pid mismatch -> validate NOT called", called["v"] is False)

    # Case 4: ga_base mismatch → dropped
    pm = FakePM(pid=111, ga_base=0x150000000, u64={cs: klass})
    r = AnchorMemoryReader(process=pm)
    seed(r)
    called = {"v": False}
    r._validate_self = lambda b, a: (called.__setitem__("v", True) or True, {})
    out = r._cache_check(anchor, set(), 0, 0.0)
    check("ga_base mismatch -> None", out is None)
    check("ga_base mismatch -> validate NOT called", called["v"] is False)


def test_remember_no_klass():
    print("test_remember_no_klass")
    cs = 0x3000_0000
    # klass at obj+0 is implausible (0) -> must NOT cache, but keep window hint
    pm = FakePM(pid=111, ga_base=0x140000000, u64={cs: 0})
    r = AnchorMemoryReader(process=pm)
    r._remember(AnchorPack(uid=7), _mk_resolved(cs))
    check("no klass -> no session cache", r._resolved_cache is None)
    check("no klass -> window hint still set", r._last_known_base == cs)
    # plausible klass -> cache stored
    pm2 = FakePM(pid=111, ga_base=0x140000000, u64={cs: 0x0000_0001_0000_0000})
    r2 = AnchorMemoryReader(process=pm2)
    r2._remember(AnchorPack(uid=7), _mk_resolved(cs))
    check("klass present -> session cache stored", r2._resolved_cache is not None)
    check("klass present -> klass captured", r2._resolved_cache_klass == 0x0000_0001_0000_0000)


def main():
    test_score_region()
    test_ranked_regions()
    test_owning_region()
    test_cache_guards()
    test_remember_no_klass()
    print()
    if _fails:
        print(f"FAILED: {len(_fails)} -> {_fails}")
        sys.exit(1)
    print("ALL PASS")


if __name__ == "__main__":
    main()
