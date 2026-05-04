"""Correctness + benchmark suite for cy_memscan.

Run from sao_auto/:
    e:\\Py\\python.exe -m tools.mem_probe.test_cy_memscan
    e:\\Py\\python.exe -m tools.mem_probe.test_cy_memscan --bench

Tests:
    - find_aligned_u64 / u32 hit positions match a Python oracle on synthetic
      buffers with known injected values (incl. boundary cases, AVX2 tail).
    - find_pattern_masked matches under nontrivial masks.
    - find_aligned_u64_in_set returns correct (offset, needle) tuples.
    - find_aligned_u64_with_anchor double-stage verifies all slots.
    - narrow_u32_batch / narrow_u64_batch pack/unpack matches.

Benchmark prints throughput (MB/s) for AVX2 path vs scalar fallback path
vs pure-Python oracle, on 256 MiB synthetic buffer.
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO_AUTO = os.path.dirname(_HERE)
if _SAO_AUTO not in sys.path:
    sys.path.insert(0, _SAO_AUTO)

from mem_probe import cy_memscan as cm

# Direct import for AVX2-disable hooks (only needed for benchmarks)
try:
    from mem_probe import _sao_cy_memscan as _cy_raw  # type: ignore
except ImportError:
    _cy_raw = None


# ───────────────────────── Test helpers ─────────────────────────

def _py_find_aligned_u64(buf, needle):
    n = len(buf)
    needle &= 0xFFFFFFFFFFFFFFFF
    return [i for i in range(0, (n // 8) * 8, 8)
            if int.from_bytes(buf[i:i + 8], "little") == needle]


def _py_find_aligned_u32(buf, needle):
    n = len(buf)
    needle &= 0xFFFFFFFF
    return [i for i in range(0, (n // 4) * 4, 4)
            if int.from_bytes(buf[i:i + 4], "little") == needle]


# ───────────────────────── Correctness tests ─────────────────────────

def test_find_aligned_u64():
    print("\n[test] find_aligned_u64")
    needle = 0xCAFEBABE_DEADBEEF
    # Buffer with hits at offsets 0, 256, 4097 (unaligned!), 65536, end-8
    size = 1 * 1024 * 1024
    buf = bytearray(size)
    needle_bytes = needle.to_bytes(8, "little")
    aligned_hits = [0, 256, 65536, size - 8]
    for off in aligned_hits:
        buf[off:off + 8] = needle_bytes
    # Inject an UNALIGNED occurrence: offset 4097 (not 8-byte aligned)
    buf[4097:4105] = needle_bytes
    # Inject another aligned hit immediately after AVX2 chunk boundary
    buf[32:40] = needle_bytes
    aligned_hits.append(32)
    aligned_hits.sort()

    got = cm.find_aligned_u64(bytes(buf), needle)
    expected = sorted(aligned_hits)
    assert got == expected, f"got {got}, expected {expected}"
    print(f"  OK: {len(got)} hits, all 8-byte aligned, unaligned occurrence ignored")

    # Edge: tail handling - last 31 bytes (less than AVX2 chunk)
    # Already covered by size-8 hit above; verify no false positive in tail
    tail_buf = b"\x00" * 24 + needle_bytes  # 32 bytes total, hit at 24
    got_tail = cm.find_aligned_u64(tail_buf, needle)
    assert got_tail == [24], f"tail hit failed: {got_tail}"
    print(f"  OK: tail offset 24")

    # Empty / tiny buffers
    assert cm.find_aligned_u64(b"", needle) == []
    assert cm.find_aligned_u64(b"\x00" * 7, needle) == []
    print(f"  OK: empty / undersized buffers")


def test_find_aligned_u32():
    print("\n[test] find_aligned_u32")
    needle = 0xDEADBEEF
    size = 256 * 1024
    buf = bytearray(size)
    needle_bytes = needle.to_bytes(4, "little")
    aligned_hits = [0, 4, 64, 4096, size - 4]
    for off in aligned_hits:
        buf[off:off + 4] = needle_bytes
    # Unaligned occurrence (offset 17 mod 4 != 0)
    buf[17:21] = needle_bytes

    got = cm.find_aligned_u32(bytes(buf), needle)
    expected = sorted(aligned_hits)
    assert got == expected, f"got {got}, expected {expected}"
    print(f"  OK: {len(got)} hits, all 4-byte aligned")

    # Cross-check with Python oracle on a smaller buf
    small = os.urandom(8 * 1024)
    rare = 0x12345678
    # Inject a few
    small_b = bytearray(small)
    for off in (0, 100, 1000, 8188):
        small_b[off:off + 4] = rare.to_bytes(4, "little")
    expected = _py_find_aligned_u32(small_b, rare)
    got = cm.find_aligned_u32(bytes(small_b), rare)
    assert got == expected, f"oracle mismatch: {got} vs {expected}"
    print(f"  OK: oracle match on random buffer ({len(got)} hits)")


def test_find_aligned_u64_in_set():
    print("\n[test] find_aligned_u64_in_set")
    size = 16 * 1024
    buf = bytearray(size)
    needles = [0x1111_2222_3333_4444,
               0xAAAA_BBBB_CCCC_DDDD,
               0xDEAD_BEEF_CAFE_BABE]
    placements = [(64, needles[0]),
                  (128, needles[1]),
                  (256, needles[2]),
                  (1024, needles[0]),  # repeat
                  (size - 8, needles[1])]
    for off, n in placements:
        buf[off:off + 8] = n.to_bytes(8, "little")

    got = cm.find_aligned_u64_in_set(bytes(buf), needles)
    got_sorted = sorted(got)
    expected = sorted([(off, n) for off, n in placements])
    assert got_sorted == expected, f"got {got_sorted}, expected {expected}"
    print(f"  OK: {len(got)} (offset, needle) tuples")


def test_find_pattern_masked():
    print("\n[test] find_pattern_masked")
    # Pattern: 16 bytes, mask = first 4 fixed, next 8 wildcard, last 4 fixed
    pat = bytes.fromhex("DEADBEEF" + "00112233445566AA" + "FEEDFACE")
    mask = bytes.fromhex("FFFFFFFF" + "0000000000000000" + "FFFFFFFF")
    size = 8 * 1024
    buf = bytearray(size)
    # Inject pattern at known positions; vary the wildcard middle
    pat_a = bytes.fromhex("DEADBEEF" + "AAAAAAAAAAAAAAAA" + "FEEDFACE")
    pat_b = bytes.fromhex("DEADBEEF" + "BBBBCCCCDDDDEEEE" + "FEEDFACE")
    placements = [(0, pat_a), (1024, pat_b), (size - 16, pat_a)]
    for off, p in placements:
        buf[off:off + 16] = p
    # Inject a near-miss: same anchor but middle is fixed, last 4 differ
    near_miss = bytes.fromhex("DEADBEEF" + "0000000000000000" + "DEADC0DE")
    buf[2048:2064] = near_miss

    got = cm.find_pattern_masked(bytes(buf), pat, mask)
    expected = sorted([off for off, _ in placements])
    assert sorted(got) == expected, f"got {sorted(got)}, expected {expected}"
    print(f"  OK: {len(got)} pattern hits, near-miss correctly rejected")


def test_narrow_batch():
    print("\n[test] narrow_u32_batch / narrow_u64_batch")
    # Pack 16 u32 values; expected matches some
    expected = 0xDEADBEEF
    vals = [expected, 0, expected, 1, expected, 2, 3, expected,
            0, expected, 0, 0, expected, 0, 0, expected]
    packed = b"".join(v.to_bytes(4, "little") for v in vals)
    got = cm.narrow_u32_batch(packed, expected)
    expected_mask = bytes(1 if v == expected else 0 for v in vals)
    assert got == expected_mask, f"u32 narrow: {got.hex()} vs {expected_mask.hex()}"
    print(f"  OK: u32 narrow matched {sum(got)}/{len(got)}")

    # u64
    e64 = 0x1122334455667788
    vals64 = [e64, 0, e64, 1, e64]
    packed64 = b"".join(v.to_bytes(8, "little") for v in vals64)
    got64 = cm.narrow_u64_batch(packed64, e64)
    assert got64 == bytes([1, 0, 1, 0, 1])
    print(f"  OK: u64 narrow matched {sum(got64)}/{len(got64)}")


def test_anchor_scan():
    print("\n[test] find_aligned_u64_with_anchor")
    # Construct a 128-byte fingerprint with anchor at offset 24
    fp_size = 128
    anchor_off = 24
    anchor_value = 0xAAAA_BBBB_CCCC_DDDD
    # Fixed slots at offsets 0, 8, 40, 56
    expected_slots = [
        (0,  0x1111111111111111),
        (8,  0x2222222222222222),
        (40, 0x3333333333333333),
        (56, 0x4444444444444444),
    ]

    def build_fp():
        b = bytearray(fp_size)
        for off, val in expected_slots:
            b[off:off + 8] = val.to_bytes(8, "little")
        b[anchor_off:anchor_off + 8] = anchor_value.to_bytes(8, "little")
        return bytes(b)

    fp = build_fp()
    # Embed fingerprint at known positions in a 256 KiB buffer, with one
    # decoy that has the anchor but wrong slot values.
    size = 256 * 1024
    buf = bytearray(size)
    embed_offsets = [256, 4096, size - fp_size]
    for off in embed_offsets:
        buf[off:off + fp_size] = fp
    # Decoy: anchor present at offset (1024 + anchor_off), but slot 0 wrong
    decoy = bytearray(fp_size)
    decoy[anchor_off:anchor_off + 8] = anchor_value.to_bytes(8, "little")
    decoy[0:8] = (0xDEAD).to_bytes(8, "little")  # slot 0 mismatch
    buf[1024:1024 + fp_size] = decoy

    got = cm.find_aligned_u64_with_anchor(
        bytes(buf), anchor_value, anchor_off, fp_size, expected_slots,
    )
    expected = sorted(embed_offsets)
    assert sorted(got) == expected, f"got {sorted(got)}, expected {expected}"
    print(f"  OK: {len(got)} fp hits (decoy with mismatching slot rejected)")


# ───────────────────────── Benchmark ─────────────────────────

def bench_throughput(size_mib: int = 256):
    print(f"\n[bench] buffer = {size_mib} MiB random bytes")
    # Use a fixed seed for reproducibility; inject ~10 hits to ensure realistic
    # "rare hit" performance (most chunks have 0 hits).
    import random
    random.seed(0xC0FFEE)
    buf = bytearray(os.urandom(size_mib * 1024 * 1024))
    needle_u64 = 0xDEAD_BEEF_CAFE_BABE
    needle_u32 = 0xDEAD_BEEF
    for _ in range(10):
        off = random.randint(0, len(buf) - 8) & ~7
        buf[off:off + 8] = needle_u64.to_bytes(8, "little")
    buf = bytes(buf)

    def time_it(name, fn, expected_hits=None):
        # Warmup
        fn()
        # Time
        t0 = time.perf_counter()
        result = fn()
        dt = time.perf_counter() - t0
        mb_per_s = (size_mib / dt) if dt > 0 else float("inf")
        gb_per_s = mb_per_s / 1024.0
        msg = f"  {name:30s}: {dt*1000:7.1f} ms  ({mb_per_s:6.0f} MB/s = {gb_per_s:.2f} GB/s) hits={len(result)}"
        if expected_hits is not None and len(result) != expected_hits:
            msg += f"  ⚠ EXPECTED {expected_hits}"
        print(msg)
        return dt, len(result)

    # ── u64 scan ──
    print(f"\n  [u64 scan: needle=0x{needle_u64:016X}]")
    if _cy_raw is not None and _cy_raw.cpu_features().get("avx2"):
        _cy_raw.force_enable_avx2()
        _, n_hits = time_it("Cython AVX2 u64", lambda: cm.find_aligned_u64(buf, needle_u64))
        _cy_raw.force_disable_avx2()
        time_it("Cython scalar u64", lambda: cm.find_aligned_u64(buf, needle_u64), n_hits)
        _cy_raw.force_enable_avx2()
    elif _cy_raw is not None:
        _, n_hits = time_it("Cython scalar u64 (no AVX2)",
                            lambda: cm.find_aligned_u64(buf, needle_u64))
    else:
        n_hits = None
    time_it("Pure Python u64", lambda: _py_find_aligned_u64(buf, needle_u64), n_hits)

    # ── u32 scan ──
    print(f"\n  [u32 scan: needle=0x{needle_u32:08X}]")
    if _cy_raw is not None and _cy_raw.cpu_features().get("avx2"):
        _cy_raw.force_enable_avx2()
        _, n_hits = time_it("Cython AVX2 u32", lambda: cm.find_aligned_u32(buf, needle_u32))
        _cy_raw.force_disable_avx2()
        time_it("Cython scalar u32", lambda: cm.find_aligned_u32(buf, needle_u32), n_hits)
        _cy_raw.force_enable_avx2()
    elif _cy_raw is not None:
        _, n_hits = time_it("Cython scalar u32 (no AVX2)",
                            lambda: cm.find_aligned_u32(buf, needle_u32))
    else:
        n_hits = None
    # Skip pure-Python u32 — very slow on 256 MiB
    if size_mib <= 64:
        time_it("Pure Python u32", lambda: _py_find_aligned_u32(buf, needle_u32), n_hits)
    else:
        print(f"  {'Pure Python u32':30s}: SKIPPED (too slow on {size_mib} MiB)")

    # ── multi-needle (in_set) ──
    print(f"\n  [u64_in_set: 50 needles]")
    needle_set = [0xDEAD_BEEF_CAFE_BABE + i for i in range(50)]
    time_it("Cython find_u64_in_set", lambda: cm.find_aligned_u64_in_set(buf, needle_set))


# ───────────────────────── Main ─────────────────────────

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--bench", action="store_true", help="Run throughput benchmark")
    p.add_argument("--bench-size", type=int, default=256,
                   help="Benchmark buffer size in MiB (default: 256)")
    p.add_argument("--quick", action="store_true",
                   help="Smaller benchmark (32 MiB) so pure-Python finishes")
    args = p.parse_args()

    print(f"backend       : {cm.backend()}")
    print(f"cpu features  : {cm.cpu_features()}")
    if cm.import_error():
        print(f"import error  : {cm.import_error()}")

    print("\n=== correctness ===")
    test_find_aligned_u64()
    test_find_aligned_u32()
    test_find_aligned_u64_in_set()
    test_find_pattern_masked()
    test_narrow_batch()
    test_anchor_scan()
    print("\n[ok] all correctness tests passed")

    if args.bench or args.quick:
        size = 32 if args.quick else args.bench_size
        print("\n=== benchmark ===")
        bench_throughput(size_mib=size)


if __name__ == "__main__":
    main()
