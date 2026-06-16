# -*- coding: utf-8 -*-
"""Offline selftest for F-B ZAttrReader (no live game required).

Synthesises a ZAttrCollection -> mixItemDict_ (Dictionary<uint, IMixAttr>) ->
IMixAttr objects whose Value sits at a klass-specific offset, then verifies:
  - read_attr_ptrs walks the dict
  - calibrate() discovers the per-klass Value offset from TCP-known anchors
  - read_attrs() returns the correct values for HP / MAX_HP / breaking_stage / overdrive
  - ga_base change invalidates the calibration

Run:  python tools/mem_attr_selftest.py
"""
from __future__ import annotations

import os
import struct
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from plugins.star_resonance_plugin.mem.il2cpp.mem_attr_reader import (  # noqa: E402
    ZAttrReader, ENTRY_SIZE, ARRAY_ELEMS_OFF,
)

# AttrType ids (packet_parser/enums.py)
HP = 11310
MAX_HP = 11320
BREAKING_STAGE = 455
IN_OVERDRIVE = 444
EXTINCTION = 441

KL_LONG = 0x0000_0001_AAAA_0000   # klass ptr for ZMixAttr<long>
KL_INT = 0x0000_0001_BBBB_0000    # klass ptr for ZMixAttr<int>
LONG_VALUE_OFF = 0x20
INT_VALUE_OFF = 0x18


class FakeMem:
    """Flat byte-addressable memory with a bump allocator + StarProcess reads."""

    def __init__(self, base=0x0000_0002_0000_0000, size=0x20000):
        self.base = base
        self.buf = bytearray(size)
        self._next = base + 0x100

    def alloc(self, n):
        a = (self._next + 0xF) & ~0xF      # 16-align
        self._next = a + n
        return a

    def _idx(self, addr):
        return addr - self.base

    def w64(self, addr, v):
        struct.pack_into("<q", self.buf, self._idx(addr), int(v))

    def w32(self, addr, v):
        struct.pack_into("<i", self.buf, self._idx(addr), int(v))

    def wu32(self, addr, v):
        struct.pack_into("<I", self.buf, self._idx(addr), int(v) & 0xFFFFFFFF)

    def wu64(self, addr, v):
        struct.pack_into("<Q", self.buf, self._idx(addr), int(v) & 0xFFFFFFFFFFFFFFFF)

    # ---- StarProcess-like read API ----
    def read_bytes(self, addr, n):
        i = self._idx(addr)
        if i < 0 or i + n > len(self.buf):
            return None
        return bytes(self.buf[i:i + n])

    def read_u64(self, addr):
        b = self.read_bytes(addr, 8)
        return struct.unpack("<Q", b)[0] if b else None

    def read_i64(self, addr):
        b = self.read_bytes(addr, 8)
        return struct.unpack("<q", b)[0] if b else None

    def read_u32(self, addr):
        b = self.read_bytes(addr, 4)
        return struct.unpack("<I", b)[0] if b else None

    def read_i32(self, addr):
        b = self.read_bytes(addr, 4)
        return struct.unpack("<i", b)[0] if b else None


def _mk_imixattr(mem, klass, value, value_off, width):
    obj = mem.alloc(0x80)
    mem.wu64(obj, klass)                      # obj+0 = klass ptr
    if width == 8:
        mem.w64(obj + value_off, value)
    else:
        mem.w32(obj + value_off, value)
    return obj


def _mk_dict(mem, pairs):
    """pairs = [(attr_id, imixattr_ptr)]. Returns the Dictionary object addr."""
    n = len(pairs)
    entries = mem.alloc(ARRAY_ELEMS_OFF + (n + 2) * ENTRY_SIZE)
    mem.wu32(entries + 0x18, n + 2)           # array length (slots)
    base = entries + ARRAY_ELEMS_OFF
    # one empty slot first (hash=-1) to exercise the skip path
    mem.w32(base + 0, -1)
    for i, (aid, ptr) in enumerate(pairs):
        ep = base + (i + 1) * ENTRY_SIZE
        mem.w32(ep + 0, 0x1234 + i)           # hashCode >= 0 (active)
        mem.w32(ep + 4, -1)                   # next
        mem.wu32(ep + 8, aid)                 # uint key
        mem.wu64(ep + 16, ptr)                # value ref
    d = mem.alloc(0x30)
    mem.wu64(d + 0x18, entries)               # entries[]
    mem.w32(d + 0x20, n)                       # count
    return d


def _mk_attrs(mem, dict_addr):
    attrs = mem.alloc(0x40)
    mem.wu64(attrs + 0x28, dict_addr)          # mixItemDict_ @ +0x28
    return attrs


_fails = []


def check(name, cond):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}")
    if not cond:
        _fails.append(name)


def test_entity_integration():
    """F-C: synth ZEntity -> attrs_ -> calibrated ZAttrReader -> numeric fields."""
    print("test_entity_integration (F-C)")
    try:
        from plugins.star_resonance_plugin.mem.il2cpp.mem_entity_mgr import EntityMgrReader, EntitySnap
    except Exception as exc:  # heavy import chain (StaticDpsSource); skip if unavailable
        print(f"  [SKIP] mem_entity_mgr import failed: {exc}")
        return

    mem = FakeMem()
    a_hp = _mk_imixattr(mem, KL_LONG, 3_000_000_000, LONG_VALUE_OFF, 8)
    a_max = _mk_imixattr(mem, KL_LONG, 5_000_000_000, LONG_VALUE_OFF, 8)
    a_stage = _mk_imixattr(mem, KL_INT, 2, INT_VALUE_OFF, 4)
    a_over = _mk_imixattr(mem, KL_INT, 1, INT_VALUE_OFF, 4)
    dict_addr = _mk_dict(mem, [
        (HP, a_hp), (MAX_HP, a_max), (BREAKING_STAGE, a_stage), (IN_OVERDRIVE, a_over),
    ])
    attrs = _mk_attrs(mem, dict_addr)
    # synth ZEntity (fdc7111b offsets)
    ent = mem.alloc(0x140)
    mem.wu64(ent + 0x48, attrs)     # attrs_
    mem.w64(ent + 0xC0, 7777)       # Uuid
    mem.w64(ent + 0xC8, 30100)      # ConfigUuid (template)
    mem.w64(ent + 0xD8, 99)         # CharId
    mem.w32(ent + 0x28, 1)          # entityState_

    src = type("Src", (), {"sr": type("SR", (), {"pm": mem})(), "dump_id": "test"})()
    emr = EntityMgrReader(src)

    r = ZAttrReader(mem, ga_base=0x140000000)
    n = emr.calibrate_attrs(ent, r, {MAX_HP: 5_000_000_000, HP: 3_000_000_000,
                                     BREAKING_STAGE: 2, IN_OVERDRIVE: 1},
                            ga_base=0x140000000)
    check("calibrate_attrs via entity -> 2 klasses", n == 2)

    snap = emr.read_entity_numeric(ent, r)
    check("read_entity_numeric returns snap", snap is not None)
    check("uuid read", snap and snap.uuid == 7777)
    check("config_uuid (template) read", snap and snap.config_uuid == 30100)
    check("cur_hp filled", snap and snap.cur_hp == 3_000_000_000)
    check("max_hp filled", snap and snap.max_hp == 5_000_000_000)
    check("breaking_stage filled", snap and snap.breaking_stage == 2)
    check("in_overdrive filled", snap and snap.in_overdrive == 1)
    check("attrs_read flag set", snap and snap.attrs_read is True)
    check("hp_pct computed ~0.6", snap and abs(snap.hp_pct - 0.6) < 0.01)

    # Without a reader -> v1 identity only, no numeric fill
    snap2 = emr.read_entity_numeric(ent, None)
    check("no reader -> attrs_read False", snap2 and snap2.attrs_read is False)
    check("no reader -> cur_hp 0", snap2 and snap2.cur_hp == 0)
    check("no reader -> still has uuid/config", snap2 and snap2.uuid == 7777 and snap2.config_uuid == 30100)


def main():
    mem = FakeMem()
    # Boss-like attrs: HP/MAX_HP are <long> (need i64), stage/overdrive are <int>.
    a_hp = _mk_imixattr(mem, KL_LONG, 3_000_000_000, LONG_VALUE_OFF, 8)
    a_max = _mk_imixattr(mem, KL_LONG, 5_000_000_000, LONG_VALUE_OFF, 8)
    a_stage = _mk_imixattr(mem, KL_INT, 2, INT_VALUE_OFF, 4)
    a_over = _mk_imixattr(mem, KL_INT, 1, INT_VALUE_OFF, 4)
    dict_addr = _mk_dict(mem, [
        (HP, a_hp), (MAX_HP, a_max), (BREAKING_STAGE, a_stage), (IN_OVERDRIVE, a_over),
    ])
    attrs = _mk_attrs(mem, dict_addr)

    r = ZAttrReader(mem, ga_base=0x140000000)

    print("test_dict_walk")
    ptrs = r.read_attr_ptrs(attrs)
    check("dict walk finds 4 attrs", len(ptrs) == 4)
    check("HP ptr correct", ptrs.get(HP) == a_hp)
    check("MAX_HP ptr correct", ptrs.get(MAX_HP) == a_max)

    print("test_pre_calibration")
    pre = r.read_attrs(attrs, [HP, MAX_HP])
    check("uncalibrated read -> empty", pre == {})
    check("is_calibrated False before calibrate", r.is_calibrated(0x140000000) is False)

    print("test_calibrate")
    n_klass = r.calibrate(attrs, {MAX_HP: 5_000_000_000, HP: 3_000_000_000,
                                  BREAKING_STAGE: 2, IN_OVERDRIVE: 1},
                          ga_base=0x140000000)
    check("calibrated 2 klasses (long + int)", n_klass == 2)
    check("is_calibrated True after calibrate", r.is_calibrated(0x140000000) is True)

    print("test_read_values")
    vals = r.read_attrs(attrs, [HP, MAX_HP, BREAKING_STAGE, IN_OVERDRIVE, EXTINCTION])
    check("HP value correct (i64 path)", vals.get(HP) == 3_000_000_000)
    check("MAX_HP value correct (i64 path)", vals.get(MAX_HP) == 5_000_000_000)
    check("BREAKING_STAGE value correct", vals.get(BREAKING_STAGE) == 2)
    check("IN_OVERDRIVE value correct", vals.get(IN_OVERDRIVE) == 1)
    check("absent attr EXTINCTION not returned", EXTINCTION not in vals)

    print("test_ga_invalidation")
    # Recalibrating under a new ga_base with no usable anchors must clear offsets.
    r.calibrate(attrs, {}, ga_base=0x999000000)
    check("ga change clears calibration", r.is_calibrated(0x999000000) is False)
    check("read after invalidation -> empty", r.read_attrs(attrs, [HP, MAX_HP]) == {})

    test_entity_integration()

    print()
    if _fails:
        print(f"FAILED: {len(_fails)} -> {_fails}")
        sys.exit(1)
    print("ALL PASS")


if __name__ == "__main__":
    main()
