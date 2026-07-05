# -*- coding: utf-8 -*-
# Contract tests for the mem_probe AVX2 memory scanner.
#
# Covers the heap-scan hot paths optimised for speed:
# - find_aligned_u64 / find_aligned_u32  (real AVX2 vs scalar parity)
# - find_aligned_u64_in_set / find_aligned_u32_in_set  (Bloom prefilter +
# C++ unordered_set; collision-but-absent values must NOT match)
# - read-only buffer acceptance (bytes go in zero-copy, no bytearray copy)
# - find_aligned_u64_with_anchor (SIMD anchor prefilter vs scalar walk)
# - read_entity_combat_many (synthetic Burst index in our own process,
# read back through the real ReadProcessMemory batch path)
# against planted needles, and verifies the AVX2 and scalar code paths agree.
from __future__ import annotations

import ctypes
import os
import struct
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

try:
    import _sao_cy_memscan as fast  # type: ignore[import-not-found]
except Exception:  # pragma: no cover - extension must be built to run these
    fast = None


@unittest.skipIf(fast is None, "_sao_cy_memscan extension not built")
class MemScanTests(unittest.TestCase):
    def setUp(self) -> None:
        # Deterministic buffer (no os.urandom) so planted needles are the only hits.
        self.n = 1 << 20
        self.buf = bytearray(self.n)  # all zeros
        fast.force_enable_avx2()

    def _plant_u64(self, value: int, offsets: list[int]) -> None:
        for off in offsets:
            self.buf[off:off + 8] = (value & 0xFFFFFFFFFFFFFFFF).to_bytes(8, "little")

    def _plant_u32(self, value: int, offsets: list[int]) -> None:
        for off in offsets:
            self.buf[off:off + 4] = (value & 0xFFFFFFFF).to_bytes(4, "little")

    def test_find_aligned_u64_finds_exactly_planted(self) -> None:
        needle = 0x9E3779B97F4A7C15
        offs = [0, 64, 4096, 65536, self.n - 32, self.n - 8]
        self._plant_u64(needle, offs)
        hits = list(fast.find_aligned_u64(memoryview(self.buf), needle, 1 << 20))
        self.assertEqual(sorted(hits), sorted(offs))
        for o in hits:
            self.assertEqual(int.from_bytes(self.buf[o:o + 8], "little"), needle)

    def test_u64_avx2_matches_scalar(self) -> None:
        needle = 0xCAFEF00DDEADBEEF
        self._plant_u64(needle, [8, 800, 80000, self.n - 8])
        mv = memoryview(self.buf)
        fast.force_enable_avx2()
        avx2 = sorted(fast.find_aligned_u64(mv, needle, 1 << 20))
        fast.force_disable_avx2()
        scalar = sorted(fast.find_aligned_u64(mv, needle, 1 << 20))
        fast.force_enable_avx2()
        self.assertEqual(avx2, scalar)

    def test_u32_avx2_matches_scalar(self) -> None:
        needle = 0x12345678
        self._plant_u32(needle, [0, 128, 4096, self.n - 4])
        mv = memoryview(self.buf)
        fast.force_enable_avx2()
        avx2 = sorted(fast.find_aligned_u32(mv, needle, 1 << 20))
        fast.force_disable_avx2()
        scalar = sorted(fast.find_aligned_u32(mv, needle, 1 << 20))
        fast.force_enable_avx2()
        self.assertEqual(avx2, scalar)
        self.assertTrue({0, 128, 4096, self.n - 4}.issubset(set(avx2)))

    def test_find_aligned_u64_in_set(self) -> None:
        needles = [0x7FF000000000 + i * 0x1000 for i in range(8)]
        offs = [i * 0x10000 for i in range(8)]
        for value, off in zip(needles, offs):
            self.buf[off:off + 8] = (value & 0xFFFFFFFFFFFFFFFF).to_bytes(8, "little")
        res = fast.find_aligned_u64_in_set(memoryview(self.buf), needles, 1 << 20)
        found = {off: val for off, val in res}
        for value, off in zip(needles, offs):
            self.assertIn(off, found)
            self.assertEqual(found[off], value & 0xFFFFFFFFFFFFFFFF)

    def test_find_aligned_u32_in_set(self) -> None:
        needles = [0xABCD0000 + i for i in range(6)]
        offs = [i * 0x20000 for i in range(6)]
        for value, off in zip(needles, offs):
            self.buf[off:off + 4] = (value & 0xFFFFFFFF).to_bytes(4, "little")
        res = fast.find_aligned_u32_in_set(memoryview(self.buf), needles, 1 << 20)
        found = {off: val for off, val in res}
        for value, off in zip(needles, offs):
            self.assertIn(off, found)
            self.assertEqual(found[off], value & 0xFFFFFFFF)

    def test_in_set_bloom_collision_rejected(self) -> None:
        # Decoys share the needles' low 6 bits (pass the Bloom mask) but are
        # NOT in the set -- they must never be reported.
        needle = 0x7FF000000040          # low6 = 0
        decoys = [0x123400000000, 0x7FF000000080, 0xAAAA00000000]  # low6 = 0 too
        self._plant_u64(needle, [0x100])
        for i, d in enumerate(decoys):
            self._plant_u64(d, [0x200 + i * 8])
        res = fast.find_aligned_u64_in_set(memoryview(self.buf), [needle], 1 << 20)
        self.assertEqual(res, [(0x100, needle)])

    def test_in_set_zero_needle_still_matches(self) -> None:
        # the zero-skip fast path must NOT apply when 0 is itself a needle
        self._plant_u64(0xFFFFFFFFFFFFFFFF, [0, 8])   # non-zero noise
        res = fast.find_aligned_u64_in_set(memoryview(self.buf[:32]), [0], 1 << 20)
        self.assertEqual(res, [(16, 0), (24, 0)])

    def test_scans_accept_readonly_bytes(self) -> None:
        # const memoryview signatures: plain bytes must be accepted zero-copy.
        needle = 0x1122334455667788
        self._plant_u64(needle, [64, 4096])
        frozen = bytes(self.buf)
        self.assertEqual(sorted(fast.find_aligned_u64(frozen, needle, 1 << 20)),
                         [64, 4096])
        self.assertEqual(
            sorted(o for o, _ in fast.find_aligned_u64_in_set(frozen, [needle], 1 << 20)),
            [64, 4096])
        self.assertEqual(fast.narrow_u64_batch(frozen[:128], needle), [64])
        pat = needle.to_bytes(8, "little")
        self.assertEqual(
            fast.find_pattern_masked(frozen[:128], list(pat), [0xFF] * 8, 16), [64])

    # ---- find_aligned_u64_with_anchor ----

    def _plant_fp(self, base: int, anchor_off: int, anchor: int, slots: dict) -> None:
        self.buf[base + anchor_off:base + anchor_off + 8] = anchor.to_bytes(8, "little")
        for so, sv in slots.items():
            self.buf[base + so:base + so + 8] = sv.to_bytes(8, "little")

    def test_with_anchor_aligned_prefilter(self) -> None:
        anchor = 0xDEAD00000000BEEF
        slots = {0x00: 0x1111111111111111, 0x30: 0x2222222222222222}
        fp_size = 0x80
        bases = [0x400, 0x10000, self.n - fp_size]
        for b in bases:
            self._plant_fp(b, 0x18, anchor, slots)
        # decoy: anchor present but one slot wrong -> must be rejected
        self._plant_fp(0x8000, 0x18, anchor, {0x00: 0x1111111111111111, 0x30: 0xBAD})
        hits = list(fast.find_aligned_u64_with_anchor(
            memoryview(self.buf), anchor, 0x18, fp_size, slots, 4096))
        self.assertEqual(sorted(hits), sorted(bases))
        # AVX2 prefilter vs scalar parity
        fast.force_disable_avx2()
        scalar = list(fast.find_aligned_u64_with_anchor(
            memoryview(self.buf), anchor, 0x18, fp_size, slots, 4096))
        fast.force_enable_avx2()
        self.assertEqual(sorted(hits), sorted(scalar))

    def test_with_anchor_misaligned_offset(self) -> None:
        # anchor_off not 8-aligned -> scalar walk branch
        anchor = 0xCAFE0000FEED0001
        slots = {0x20: 0x3333333333333333}           # clear of the anchor bytes
        self._plant_fp(0x900, 0x0C, anchor, slots)   # base 0x900 is 8-aligned
        hits = list(fast.find_aligned_u64_with_anchor(
            memoryview(self.buf), anchor, 0x0C, 0x40, slots, 4096))
        self.assertIn(0x900, hits)


@unittest.skipIf(fast is None, "_sao_cy_memscan extension not built")
class ReadEntityCombatManyTests(unittest.TestCase):
    # Synthetic ZAttrCacheSlim Burst index laid out in our own process,
    # read back via the real cross-process batch decoder (pseudo-handle -1).

    OFF_ATTRS = 0x48
    OFF_INDEXPART = 0x18
    OFF_VALUES = 0x20

    def setUp(self) -> None:
        self.mem = bytearray(0x10000)
        self.base = ctypes.addressof((ctypes.c_char * 1).from_buffer(self.mem))
        self._next = 0x100
        k32 = ctypes.WinDLL("kernel32")
        k32.GetCurrentProcess.restype = ctypes.c_void_p
        self.handle = int(ctypes.c_uint64(k32.GetCurrentProcess() or 0).value)
        if self.handle == 0:  # pseudo handle (-1) came back as c_void_p None-safe
            self.handle = 0xFFFFFFFFFFFFFFFF

    def _alloc(self, n: int) -> int:
        a = (self._next + 0xF) & ~0xF
        self._next = a + n
        return self.base + a

    def _w(self, addr: int, fmt: str, v: int) -> None:
        struct.pack_into(fmt, self.mem, addr - self.base, v)

    def _mk_value_obj(self, is_long: bool, value: int) -> int:
        o = self._alloc(0x30)
        if is_long:
            self._w(o + 0x18, "<q", value)
        else:
            self._w(o + 0x14, "<i", value)
        return o

    def _mk_entity(self, attr_values: dict) -> int:
        # attr_values: {attr_id: (is_long, value)} laid out across segments.
        ent = self._alloc(0x60)
        attrs = self._alloc(0x40)
        ip = self._alloc(0x310)
        vals = self._alloc(0x20 + 16 * 0x10)
        self._w(ent + self.OFF_ATTRS, "<Q", attrs)
        self._w(attrs + self.OFF_INDEXPART, "<Q", ip)
        self._w(attrs + self.OFF_VALUES, "<Q", vals)
        # value-object pages: vals+0x20 + page*0x10 + 0x8 -> arr; arr+0x20+slot*8 -> obj
        items = list(attr_values.items())
        arr = self._alloc(0x20 + 32 * 8)
        self._w(vals + 0x20 + 0x8, "<Q", arr)        # page 0 (vidx 0..31)
        for vidx, (aid, (is_long, value)) in enumerate(items):
            self._w(arr + 0x20 + vidx * 8, "<Q", self._mk_value_obj(is_long, value))
        # spread keys over two segments (4 per segment, like the live layout)
        per_seg = 4
        for k in range((len(items) + per_seg - 1) // per_seg):
            seg = self._alloc(0x24)
            vip = self._alloc(0x20)
            part = items[k * per_seg:(k + 1) * per_seg]
            for pos, (aid, _) in enumerate(part):
                self._w(seg + pos * 4, "<I", aid)
                self._w(vip + pos * 4, "<i", k * per_seg + pos)
            self._w(seg + 0x20, "<i", len(part))
            self._w(ip + 0x10 + k * 8, "<Q", seg)
            self._w(ip + 0x110 + k * 8, "<Q", vip)
        return ent

    def test_full_decode(self) -> None:
        ent = self._mk_entity({
            11310: (True, 123456),    # cur HP
            11320: (True, 654321),    # max HP
            455: (False, 2),          # breaking stage
            444: (False, 7),          # overdrive
            443: (False, 1),          # stun
            441: (False, 33),         # extinction
            100: (False, 9901),       # cast skill id
            440: (False, 100),        # max extinction
        })
        rows = fast.read_entity_combat_many(
            self.handle, [ent], self.OFF_ATTRS, self.OFF_INDEXPART, self.OFF_VALUES)
        self.assertEqual(rows, [123456, 654321, 2, 7, 1, 33, 9901, 100])

    def test_noncombat_and_partial(self) -> None:
        # entity with no readable attrs ptr -> all -1
        bad_ent = self._alloc(0x60)
        self._w(bad_ent + self.OFF_ATTRS, "<Q", 0)
        # entity with only state attrs (no HP pair) -> cur/max stay -1
        state_ent = self._mk_entity({455: (False, 3)})
        rows = fast.read_entity_combat_many(
            self.handle, [bad_ent, state_ent],
            self.OFF_ATTRS, self.OFF_INDEXPART, self.OFF_VALUES)
        self.assertEqual(rows[:8], [-1] * 8)
        self.assertEqual(rows[8:10], [-1, -1])
        self.assertEqual(rows[10], 3)


if __name__ == "__main__":
    unittest.main(verbosity=2)
