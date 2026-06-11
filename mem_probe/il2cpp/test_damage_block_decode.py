# -*- coding: utf-8 -*-
"""Deterministic test for MemDamageReader's batched entries[] block decode.

A fake pm backs a flat address space holding a ZDictionary entries slab; the
block-read path must produce the same {uuid: total} / {skill: value} the
per-slot path would, validating the struct formats against the documented
ZDictionary entry layout.
"""
from __future__ import annotations

import os
import struct
import sys

_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from mem_probe.il2cpp import mem_damage_reader as mdr

# ZDictionary offsets (mirror the module constants)
ZDICT_COUNT_OFF = mdr.ZDICT_COUNT_OFF
ZDICT_ENTRIES_OFF = mdr.ZDICT_ENTRIES_OFF
ARRAY_LEN_OFF = mdr.ARRAY_LEN_OFF
ARRAY_ELEMS_OFF = mdr.ARRAY_ELEMS_OFF


class FakePm:
    def __init__(self):
        self.pid = 1
        self._handle = 1
        self.mem = bytearray(0x40000)
        self.base = 0x10000000          # where our fake heap "starts" (addr - base = index)

    def _idx(self, addr):
        return int(addr) - self.base

    def write(self, addr, data):
        i = self._idx(addr)
        self.mem[i:i + len(data)] = data

    def read_bytes(self, addr, n):
        i = self._idx(addr)
        if i < 0 or i + n > len(self.mem):
            return None
        return bytes(self.mem[i:i + n])

    def read_i32(self, addr):
        b = self.read_bytes(addr, 4)
        return struct.unpack("<i", b)[0] if b else None

    def read_u32(self, addr):
        b = self.read_bytes(addr, 4)
        return struct.unpack("<I", b)[0] if b else None

    def read_i64(self, addr):
        b = self.read_bytes(addr, 8)
        return struct.unpack("<q", b)[0] if b else None

    def read_u64(self, addr):
        b = self.read_bytes(addr, 8)
        return struct.unpack("<Q", b)[0] if b else None


def _build_zdict(pm, addr, entries_addr, entries: list, stride: int):
    """entries: list of raw entry bytes (each `stride` long)."""
    # dict header: count @ COUNT_OFF, entries ptr @ ENTRIES_OFF
    pm.write(addr + ZDICT_COUNT_OFF, struct.pack("<i", len(entries)))
    pm.write(addr + ZDICT_ENTRIES_OFF, struct.pack("<Q", entries_addr))
    # array header: length @ ARRAY_LEN_OFF, elems @ ARRAY_ELEMS_OFF
    pm.write(entries_addr + ARRAY_LEN_OFF, struct.pack("<I", len(entries)))
    blob = b"".join(entries)
    pm.write(entries_addr + ARRAY_ELEMS_OFF, blob)


def _entry_18(hash_, key, val, key_signed=True):
    # Entry { u32 hash; i32 next; <8B key>; <8B value> } stride 0x18
    kf = "<q" if key_signed else "<Q"
    return struct.pack("<Ii", hash_ & 0xFFFFFFFF, 0) + struct.pack(kf, key) + struct.pack("<q", val)


def test_player_totals_block_decode():
    pm = FakePm()
    r = mdr.MemDamageReader.__new__(mdr.MemDamageReader)
    r.pm = pm
    r.off_totalplayer = 0xD0
    r._inst = 0
    r._klass = 0
    # locate() shortcut: pretend we have an instance by overriding it
    INST = pm.base + 0x100
    OUTER = pm.base + 0x1000        # totalPlayerValue_ dict
    OUTER_ENTRIES = pm.base + 0x1100
    INNER = pm.base + 0x2000        # the Damage(1) inner dict {uuid: total}
    INNER_ENTRIES = pm.base + 0x2100
    pm.write(INST + r.off_totalplayer, struct.pack("<Q", OUTER))
    # outer: key=total_type(1) -> value=INNER
    _build_zdict(pm, OUTER, OUTER_ENTRIES, [_entry_18(1, mdr.TOTAL_DAMAGE, INNER)], 0x18)
    # inner: {uuid: total}
    _build_zdict(pm, INNER, INNER_ENTRIES, [
        _entry_18(1, 111111, 5000),
        _entry_18(1, 222222, 9000),
    ], 0x18)

    # drive read_player_totals with locate() forced to INST
    r.locate = lambda **k: INST  # type: ignore
    out = r.read_player_totals(mdr.TOTAL_DAMAGE)
    assert out == {111111: 5000, 222222: 9000}, out
    print("[PASS] test_player_totals_block_decode")


def test_skill_damage_block_decode():
    pm = FakePm()
    r = mdr.MemDamageReader.__new__(mdr.MemDamageReader)
    r.pm = pm
    r.off_damagevalue = 0xB0
    r.off_skill_entry_actual = mdr.ENTRY_VAL_OFF + mdr.DMG_ACTUALVALUE_OFF   # 0x10+0x18 = 0x28
    r._inst = 0
    r._klass = 0
    INST = pm.base + 0x100
    DMGV = pm.base + 0x1000          # damageValue_ {uuid: innerDict}
    DMGV_ENTRIES = pm.base + 0x1100
    INNER = pm.base + 0x3000         # {skillId: DamageData struct}
    INNER_ENTRIES = pm.base + 0x3100
    pm.write(INST + r.off_damagevalue, struct.pack("<Q", DMGV))
    _build_zdict(pm, DMGV, DMGV_ENTRIES, [_entry_18(1, 777, INNER)], 0x18)
    # inner entries stride 0x30: { u32 hash; i32 next; i32 skill@0x8; DamageData@0x10 }
    # actualValue lives at entry+0x28.
    def _skill_entry(skill, actual):
        buf = bytearray(0x30)
        struct.pack_into("<I", buf, 0, 1)            # hash
        struct.pack_into("<i", buf, 8, skill)        # key skillId @ 0x8
        struct.pack_into("<q", buf, 0x28, actual)    # actualValue @ 0x28
        return bytes(buf)
    _build_zdict(pm, INNER, INNER_ENTRIES, [
        _skill_entry(101, 3000),
        _skill_entry(202, 8000),
    ], mdr.SKILL_ENTRY_STRIDE)

    r.locate = lambda **k: INST  # type: ignore
    out = r.read_player_skill_damage(777)
    assert out == {101: 3000, 202: 8000}, out
    print("[PASS] test_skill_damage_block_decode")


if __name__ == "__main__":
    test_player_totals_block_decode()
    test_skill_damage_block_decode()
    print("RESULT: ALL GREEN")
