"""Compatibility facade for legacy mem_probe scan helpers.

The historical mem_probe tools import ``mem_probe.cy_memscan``.  The compiled
extension is now checked in at the repository root as ``_sao_cy_memscan`` (and
may also exist in build outputs), so this module forwards to it when available
and provides slow pure-Python fallbacks for read-only diagnostics.
"""
from __future__ import annotations

from typing import Iterable, List, Sequence, Tuple

try:  # Prefer the checked-in/root or sys.path-visible Cython extension.
    import _sao_cy_memscan as _fast  # type: ignore[import-not-found]
except Exception:  # pragma: no cover - fallback is for dev environments.
    _fast = None


def _as_bytes(buf) -> bytes:
    if isinstance(buf, bytes):
        return buf
    return memoryview(buf).tobytes()


def _writable_view(buf):
    """Return a writable memoryview of ``buf``.

    The Cython memoryview signatures on the compiled extension demand a
    writable buffer.  Plain ``bytes`` and read-only numpy arrays need to be
    copied first; the wrapper releases the temporary view once the call
    returns.  The caller is expected to use ``view.release()`` after use
    (helpers above already do that).
    """
    if isinstance(buf, memoryview):
        return buf if not buf.readonly else memoryview(bytes(buf))
    if isinstance(buf, (bytes, bytearray)):
        data = buf if isinstance(buf, bytearray) else bytearray(buf)
    else:
        # numpy / anything that supports the buffer protocol
        mv = memoryview(buf)
        if not mv.readonly:
            return mv
        data = bytearray(mv.tobytes())
        mv.release()
    return memoryview(data)


def cpu_features() -> dict:
    if _fast is not None and hasattr(_fast, "cpu_features"):
        return _fast.cpu_features()
    return {"avx2": False, "sse42": False, "fallback": "python"}


def backend_info() -> dict:
    """Return JSON-safe diagnostics for the active memscan backend."""
    features = dict(cpu_features() or {})
    fallback = str(features.get("fallback") or "")
    if _fast is None:
        backend = "python"
    elif fallback.startswith("cython"):
        backend = fallback
    else:
        backend = "cython"
    return {
        "extension_loaded": _fast is not None,
        "backend": backend,
        "features": features,
        "readonly_buffers_supported": False,
        "bytes_copy_required": _fast is not None,
    }


def force_disable_avx2() -> None:
    if _fast is not None and hasattr(_fast, "force_disable_avx2"):
        _fast.force_disable_avx2()


def force_enable_avx2() -> None:
    if _fast is not None and hasattr(_fast, "force_enable_avx2"):
        _fast.force_enable_avx2()


def read_words_many(handle: int, addrs, word_size: int):
    """Batch cross-process read of N scattered addresses in one nogil C loop.

    Returns a list of unsigned ints (None where the read failed), or None when
    the Cython extension is unavailable (caller should use its own fallback).
    """
    if _fast is not None and hasattr(_fast, "read_words_many"):
        return _fast.read_words_many(int(handle), list(addrs), int(word_size))
    return None


def has_batch_read() -> bool:
    return _fast is not None and hasattr(_fast, "read_words_many")


def find_aligned_u64(buf, needle: int, max_hits: int = 4096) -> List[int]:
    if _fast is not None and hasattr(_fast, "find_aligned_u64"):
        view = _writable_view(buf)
        try:
            return list(_fast.find_aligned_u64(view, needle, max_hits))
        finally:
            if view is not buf:
                view.release()
    data = _as_bytes(buf)
    target = int(needle).to_bytes(8, "little", signed=False)
    hits: List[int] = []
    for off in range(0, max(0, len(data) - 7), 8):
        if data[off:off + 8] == target:
            hits.append(off)
            if len(hits) >= max_hits:
                break
    return hits


def find_aligned_u32(buf, needle: int, max_hits: int = 4096) -> List[int]:
    if _fast is not None and hasattr(_fast, "find_aligned_u32"):
        view = _writable_view(buf)
        try:
            return list(_fast.find_aligned_u32(view, needle, max_hits))
        finally:
            if view is not buf:
                view.release()
    data = _as_bytes(buf)
    target = (int(needle) & 0xFFFFFFFF).to_bytes(4, "little", signed=False)
    hits: List[int] = []
    for off in range(0, max(0, len(data) - 3), 4):
        if data[off:off + 4] == target:
            hits.append(off)
            if len(hits) >= max_hits:
                break
    return hits


def find_aligned_u64_in_set(buf, needles: Iterable[int], max_hits: int = 4096) -> List[Tuple[int, int]]:
    if _fast is not None and hasattr(_fast, "find_aligned_u64_in_set"):
        view = _writable_view(buf)
        try:
            return list(_fast.find_aligned_u64_in_set(view, list(needles), max_hits))
        finally:
            if view is not buf:
                view.release()
    wanted = {int(v) & 0xFFFFFFFFFFFFFFFF for v in needles}
    data = _as_bytes(buf)
    hits: List[Tuple[int, int]] = []
    for off in range(0, max(0, len(data) - 7), 8):
        value = int.from_bytes(data[off:off + 8], "little", signed=False)
        if value in wanted:
            hits.append((off, value))
            if len(hits) >= max_hits:
                break
    return hits


def find_aligned_u32_in_set(buf, needles: Iterable[int], max_hits: int = 4096) -> List[Tuple[int, int]]:
    if _fast is not None and hasattr(_fast, "find_aligned_u32_in_set"):
        view = _writable_view(buf)
        try:
            return list(_fast.find_aligned_u32_in_set(view, list(needles), max_hits))
        finally:
            if view is not buf:
                view.release()
    wanted = {int(v) & 0xFFFFFFFF for v in needles}
    data = _as_bytes(buf)
    hits: List[Tuple[int, int]] = []
    for off in range(0, max(0, len(data) - 3), 4):
        value = int.from_bytes(data[off:off + 4], "little", signed=False)
        if value in wanted:
            hits.append((off, value))
            if len(hits) >= max_hits:
                break
    return hits


def scan_repeated_field_candidates(buf, min_count: int = 50, max_count: int = 1024) -> List[dict]:
    """Find RepeatedField<T> header candidates in a private memory chunk.

    Cython-delegated: returns [{'rf_base', 'count', 'array_ptr'}, ...] for
    every 4-byte aligned int32 in [min_count, max_count] whose preceding
    8 bytes look like a valid heap pointer.
    """
    if _fast is not None and hasattr(_fast, "scan_repeated_field_candidates"):
        view = _writable_view(buf)
        try:
            return list(_fast.scan_repeated_field_candidates(view, int(min_count), int(max_count)))
        finally:
            if view is not buf:
                view.release()
    # Pure-Python fallback
    data = _as_bytes(buf)
    out: List[dict] = []
    for off in range(0x18, len(data) - 4, 4):
        cnt = int.from_bytes(data[off:off + 4], "little", signed=False)
        if cnt < min_count or cnt > max_count:
            continue
        ap = int.from_bytes(data[off - 0x08:off], "little", signed=False)
        if ap < 0x10000 or ap > 0x7FFFFFFFFFFF:
            continue
        out.append({'rf_base': off - 0x18, 'count': cnt, 'array_ptr': ap})
    return out


def find_skill_cd_arrays_in_blob(buf, region_base: int, skill_ids,
                                 min_count: int = 50, max_count: int = 1024,
                                 max_candidates: int = 2048) -> List[dict]:
    """Cython-accelerated: find RepeatedField<SkillCDInfo> array candidates
    in a single region blob, validating the array header and first element
    pointer in C without per-candidate RPCs.

    Returns a list of dicts: {rf_base, count, array_ptr, max_length,
    element0_ptr, in_blob, fast_match}.
    """
    if _fast is not None and hasattr(_fast, "find_skill_cd_arrays_in_blob"):
        view = _writable_view(buf)
        try:
            return list(_fast.find_skill_cd_arrays_in_blob(
                view, int(region_base), list(skill_ids),
                int(min_count), int(max_count), int(max_candidates),
            ))
        finally:
            if view is not buf:
                view.release()
    # Pure-Python fallback
    data = _as_bytes(buf)
    out: List[dict] = []
    skill_set = set(int(s) for s in skill_ids)
    for off in range(0x18, len(data) - 4, 4):
        cnt = int.from_bytes(data[off:off + 4], "little", signed=False)
        if cnt < min_count or cnt > max_count:
            continue
        ap = int.from_bytes(data[off - 0x08:off], "little", signed=False)
        if ap < 0x10000 or ap > 0x7FFFFFFFFFFF:
            continue
        if ap < region_base or ap + 0x20 > region_base + len(data):
            out.append({
                'rf_base': off - 0x18, 'count': cnt, 'array_ptr': ap,
                'max_length': 0, 'element0_ptr': 0,
                'in_blob': False, 'fast_match': False,
            })
            continue
        arr_off = ap - region_base
        max_len = int.from_bytes(data[arr_off + 0x10:arr_off + 0x14], "little", signed=False)
        if max_len < 50 or max_len > 1024:
            continue
        elem0 = int.from_bytes(data[arr_off + 0x18:arr_off + 0x20], "little", signed=False)
        if elem0 < 0x10000:
            continue
        out.append({
            'rf_base': off - 0x18, 'count': cnt, 'array_ptr': ap,
            'max_length': max_len, 'element0_ptr': elem0,
            'in_blob': (elem0 >= region_base and elem0 + 0x24 <= region_base + len(data)),
            'fast_match': False,
        })
    return out


def find_pattern_masked(buf, pattern: Sequence[int], mask: Sequence[int], max_hits: int = 4096) -> List[int]:
    if _fast is not None and hasattr(_fast, "find_pattern_masked"):
        view = _writable_view(buf)
        try:
            return list(_fast.find_pattern_masked(view, list(pattern), list(mask) if mask is not None else None, max_hits))
        finally:
            if view is not buf:
                view.release()
    data = _as_bytes(buf)
    pat = bytes(pattern)
    m = bytes(mask)
    if not pat or len(pat) != len(m):
        return []
    hits: List[int] = []
    plen = len(pat)
    for off in range(0, len(data) - plen + 1):
        for i in range(plen):
            if (data[off + i] & m[i]) != (pat[i] & m[i]):
                break
        else:
            hits.append(off)
            if len(hits) >= max_hits:
                break
    return hits


def narrow_u32_batch(packed, expected: int):
    if _fast is not None and hasattr(_fast, "narrow_u32_batch"):
        view = _writable_view(packed)
        try:
            return list(_fast.narrow_u32_batch(view, expected))
        finally:
            if view is not packed:
                view.release()
    data = _as_bytes(packed)
    target = int(expected) & 0xFFFFFFFF
    return [i for i in range(0, len(data) - 3, 4)
            if int.from_bytes(data[i:i + 4], "little", signed=False) == target]


def narrow_u64_batch(packed, expected: int):
    if _fast is not None and hasattr(_fast, "narrow_u64_batch"):
        view = _writable_view(packed)
        try:
            return list(_fast.narrow_u64_batch(view, expected))
        finally:
            if view is not packed:
                view.release()
    data = _as_bytes(packed)
    target = int(expected) & 0xFFFFFFFFFFFFFFFF
    return [i for i in range(0, len(data) - 7, 8)
            if int.from_bytes(data[i:i + 8], "little", signed=False) == target]


def find_aligned_u64_with_anchor(buf, anchor_value: int, anchor_off: int, fp_size: int,
                                 expected_slots, max_hits: int = 4096) -> List[int]:
    if _fast is not None and hasattr(_fast, "find_aligned_u64_with_anchor"):
        view = _writable_view(buf)
        try:
            return list(_fast.find_aligned_u64_with_anchor(
                view, anchor_value, anchor_off, fp_size, dict(expected_slots or {}), max_hits))
        finally:
            if view is not buf:
                view.release()
    data = _as_bytes(buf)
    anchor = int(anchor_value).to_bytes(8, "little", signed=False)
    hits: List[int] = []
    for base in range(0, max(0, len(data) - fp_size + 1), 8):
        pos = base + int(anchor_off)
        if pos < 0 or pos + 8 > len(data) or data[pos:pos + 8] != anchor:
            continue
        ok = True
        for slot_off, expected in dict(expected_slots).items():
            slot = base + int(slot_off)
            if slot < 0 or slot + 8 > len(data):
                ok = False
                break
            value = int.from_bytes(data[slot:slot + 8], "little", signed=False)
            if value != (int(expected) & 0xFFFFFFFFFFFFFFFF):
                ok = False
                break
        if ok:
            hits.append(base)
            if len(hits) >= max_hits:
                break
    return hits
