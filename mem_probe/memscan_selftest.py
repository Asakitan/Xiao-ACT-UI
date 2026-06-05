# -*- coding: utf-8 -*-
"""Contract tests for the mem_probe AVX2 memory scanner.

Covers the heap-scan hot paths optimised for speed:
  - find_aligned_u64 / find_aligned_u32  (real AVX2 vs scalar parity)
  - find_aligned_u64_in_set / find_aligned_u32_in_set  (C++ unordered_set)
against planted needles, and verifies the AVX2 and scalar code paths agree.
"""
from __future__ import annotations

import os
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
