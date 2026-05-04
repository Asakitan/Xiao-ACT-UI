"""Python facade over _sao_cy_memscan with pure-Python fallback.

All callers in tools/mem_probe should import scan kernels from THIS module,
not directly from _sao_cy_memscan.  This guarantees:

  * Cython kernel used when available (.pyd present + AVX2 or scalar path).
  * Transparent pure-Python fallback when the Cython kernel cannot be imported
    (e.g., dev machine without a built extension, or unsupported CPU).
  * One stable API surface for the rest of mem_probe to depend on.

Public surface (mirrors _sao_cy_memscan):

    find_aligned_u64(buf, needle, max_hits=...) -> list[int]
    find_aligned_u32(buf, needle, max_hits=...) -> list[int]
    find_aligned_u64_in_set(buf, needles, max_hits=...) -> list[(off, needle)]
    find_pattern_masked(buf, pattern, mask, max_hits=...) -> list[int]
    narrow_u32_batch(packed, expected) -> bytes
    narrow_u64_batch(packed, expected) -> bytes
    find_aligned_u64_with_anchor(buf, anchor_value, anchor_off, fp_size,
                                 expected_slots, max_hits=...) -> list[int]

    cpu_features() -> dict
    backend()      -> 'cython-avx2' | 'cython-scalar' | 'python'
"""
from __future__ import annotations

import struct
from typing import Iterable, List, Tuple


# ───────────────────────── Backend selection ─────────────────────────

_cy = None
_backend = "python"
_import_err: str | None = None
try:
    from . import _sao_cy_memscan as _cy  # type: ignore
except ImportError as _exc:
    _cy = None
    _import_err = str(_exc)
    _backend = "python"
else:
    _feat = _cy.cpu_features()
    _backend = "cython-avx2" if _feat.get("avx2") else "cython-scalar"


def cpu_features() -> dict:
    """Return CPU feature dict; empty when Cython kernel unavailable."""
    if _cy is not None:
        return dict(_cy.cpu_features())
    return {"avx2": False, "sse42": False}


def backend() -> str:
    """One of: 'cython-avx2', 'cython-scalar', 'python'."""
    return _backend


def import_error() -> str | None:
    """Reason _sao_cy_memscan failed to import, or None."""
    return _import_err


# ───────────────────────── Cython-backed paths ─────────────────────────

if _cy is not None:
    def find_aligned_u64(buf, needle: int, max_hits: int = 1_000_000) -> List[int]:
        return _cy.find_aligned_u64(buf, needle & 0xFFFFFFFFFFFFFFFF, max_hits)

    def find_aligned_u32(buf, needle: int, max_hits: int = 1_000_000) -> List[int]:
        return _cy.find_aligned_u32(buf, needle & 0xFFFFFFFF, max_hits)

    def find_aligned_u64_in_set(buf, needles: Iterable[int],
                                max_hits: int = 1_000_000) -> List[Tuple[int, int]]:
        # Cython expects a list/tuple of unsigned long long
        return _cy.find_aligned_u64_in_set(
            buf, [int(x) & 0xFFFFFFFFFFFFFFFF for x in needles], max_hits,
        )

    def find_pattern_masked(buf, pattern: bytes, mask: bytes,
                            max_hits: int = 1_000_000) -> List[int]:
        return _cy.find_pattern_masked(buf, pattern, mask, max_hits)

    def narrow_u32_batch(packed, expected: int) -> bytes:
        return _cy.narrow_u32_batch(packed, expected & 0xFFFFFFFF)

    def narrow_u64_batch(packed, expected: int) -> bytes:
        return _cy.narrow_u64_batch(packed, expected & 0xFFFFFFFFFFFFFFFF)

    def find_aligned_u64_with_anchor(buf, anchor_value: int, anchor_off: int,
                                     fp_size: int,
                                     expected_slots: List[Tuple[int, int]],
                                     max_hits: int = 1_000_000) -> List[int]:
        return _cy.find_aligned_u64_with_anchor(
            buf, anchor_value & 0xFFFFFFFFFFFFFFFF, anchor_off, fp_size,
            [(int(off), int(val) & 0xFFFFFFFFFFFFFFFF) for off, val in expected_slots],
            max_hits,
        )

    def unpack_struct_fields(buf, field_specs: List[Tuple[int, int]]) -> List[int]:
        return _cy.unpack_struct_fields(buf, [(int(o), int(w)) for o, w in field_specs])

    def unpack_array_fields(buf, stride: int, field_specs: List[Tuple[int, int]],
                            n_elements: int) -> List[int]:
        return _cy.unpack_array_fields(
            buf, int(stride),
            [(int(o), int(w)) for o, w in field_specs],
            int(n_elements),
        )

    # ────────────────────── Pure-Python fallback ──────────────────────
else:
    def find_aligned_u64(buf, needle: int, max_hits: int = 1_000_000) -> List[int]:
        n = len(buf)
        needle &= 0xFFFFFFFFFFFFFFFF
        view = memoryview(buf)
        hits: List[int] = []
        for i in range(0, (n // 8) * 8, 8):
            if int.from_bytes(view[i:i + 8], "little") == needle:
                hits.append(i)
                if len(hits) >= max_hits:
                    break
        return hits

    def find_aligned_u32(buf, needle: int, max_hits: int = 1_000_000) -> List[int]:
        n = len(buf)
        needle &= 0xFFFFFFFF
        view = memoryview(buf)
        hits: List[int] = []
        for i in range(0, (n // 4) * 4, 4):
            if int.from_bytes(view[i:i + 4], "little") == needle:
                hits.append(i)
                if len(hits) >= max_hits:
                    break
        return hits

    def find_aligned_u64_in_set(buf, needles: Iterable[int],
                                max_hits: int = 1_000_000) -> List[Tuple[int, int]]:
        needle_set = {int(x) & 0xFFFFFFFFFFFFFFFF for x in needles}
        if not needle_set:
            return []
        lo, hi = min(needle_set), max(needle_set)
        n = len(buf)
        view = memoryview(buf)
        out: List[Tuple[int, int]] = []
        for i in range(0, (n // 8) * 8, 8):
            v = int.from_bytes(view[i:i + 8], "little")
            if v < lo or v > hi:
                continue
            if v in needle_set:
                out.append((i, v))
                if len(out) >= max_hits:
                    break
        return out

    def find_pattern_masked(buf, pattern: bytes, mask: bytes,
                            max_hits: int = 1_000_000) -> List[int]:
        plen = len(pattern)
        if plen == 0 or len(mask) != plen or len(buf) < plen:
            return []
        anchor_idx = -1
        for i in range(plen):
            if mask[i] == 0xFF:
                anchor_idx = i
                break
        if anchor_idx < 0:
            return []
        anchor_byte = pattern[anchor_idx]
        n = len(buf)
        view = bytes(buf) if not isinstance(buf, (bytes, bytearray)) else buf
        out: List[int] = []
        pos = 0
        while pos < n:
            i = view.find(anchor_byte, pos)
            if i < 0 or i + (plen - anchor_idx) > n:
                break
            start = i - anchor_idx
            if start >= 0:
                ok = True
                for k in range(plen):
                    if mask[k] == 0:
                        continue
                    if view[start + k] != pattern[k]:
                        ok = False
                        break
                if ok:
                    out.append(start)
                    if len(out) >= max_hits:
                        break
            pos = i + 1
        return out

    def narrow_u32_batch(packed, expected: int) -> bytes:
        view = memoryview(packed)
        n = len(view) // 4
        expected &= 0xFFFFFFFF
        out = bytearray(n)
        for i in range(n):
            if int.from_bytes(view[i * 4:i * 4 + 4], "little") == expected:
                out[i] = 1
        return bytes(out)

    def narrow_u64_batch(packed, expected: int) -> bytes:
        view = memoryview(packed)
        n = len(view) // 8
        expected &= 0xFFFFFFFFFFFFFFFF
        out = bytearray(n)
        for i in range(n):
            if int.from_bytes(view[i * 8:i * 8 + 8], "little") == expected:
                out[i] = 1
        return bytes(out)

    def find_aligned_u64_with_anchor(buf, anchor_value: int, anchor_off: int,
                                     fp_size: int,
                                     expected_slots: List[Tuple[int, int]],
                                     max_hits: int = 1_000_000) -> List[int]:
        anchor_value &= 0xFFFFFFFFFFFFFFFF
        n = len(buf)
        if n < fp_size:
            return []
        view = memoryview(buf)
        # Stage 1: find anchor (8-byte aligned)
        s1 = []
        for i in range(0, (n // 8) * 8, 8):
            if int.from_bytes(view[i:i + 8], "little") == anchor_value:
                s1.append(i)
        # Stage 2: verify all expected slots
        out: List[int] = []
        for hit in s1:
            fp_base = hit - anchor_off
            if fp_base < 0 or fp_base + fp_size > n:
                continue
            ok = True
            for off, val in expected_slots:
                if int.from_bytes(view[fp_base + off:fp_base + off + 8], "little") != (
                    val & 0xFFFFFFFFFFFFFFFF
                ):
                    ok = False
                    break
            if ok:
                out.append(fp_base)
                if len(out) >= max_hits:
                    break
        return out

    def unpack_struct_fields(buf, field_specs: List[Tuple[int, int]]) -> List[int]:
        view = memoryview(buf)
        n = len(view)
        out: List[int] = []
        for off, width in field_specs:
            if off < 0 or off + width > n:
                out.append(0)
            else:
                out.append(int.from_bytes(view[off:off + width], "little"))
        return out

    def unpack_array_fields(buf, stride: int, field_specs: List[Tuple[int, int]],
                            n_elements: int) -> List[int]:
        view = memoryview(buf)
        n = len(view)
        out: List[int] = []
        for e in range(n_elements):
            base = e * stride
            if base + stride > n:
                break
            for off, width in field_specs:
                addr = base + off
                if addr + width > n:
                    out.append(0)
                else:
                    out.append(int.from_bytes(view[addr:addr + width], "little"))
        return out


__all__ = [
    "cpu_features", "backend", "import_error",
    "find_aligned_u64", "find_aligned_u32", "find_aligned_u64_in_set",
    "find_pattern_masked", "narrow_u32_batch", "narrow_u64_batch",
    "find_aligned_u64_with_anchor",
    "unpack_struct_fields", "unpack_array_fields",
]
