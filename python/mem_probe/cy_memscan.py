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

if _fast is None:  # pragma: no cover - loud once, so a missing/broken .pyd in a
    # frozen build can't silently turn every heap sweep into a per-8-byte
    # Python loop (orders of magnitude slower) without anyone noticing.
    import logging
    logging.getLogger(__name__).warning("native extension unavailable, using fallback")


def _as_bytes(buf) -> bytes:
    if isinstance(buf, bytes):
        return buf
    return memoryview(buf).tobytes()


def _writable_view(buf):
    """Return a writable memoryview of ``buf``.

    Legacy-compat only: old compiled extensions demanded a writable buffer,
    forcing a full copy of every ``bytes`` chunk scanned.  Current builds use
    ``const`` memoryviews and take read-only buffers directly (zero copy);
    this helper is kept as the fallback when an old .pyd is on sys.path.
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


def _probe_readonly_support() -> bool:
    """One-time probe: does the loaded extension accept read-only buffers?"""
    if _fast is None or not hasattr(_fast, "find_aligned_u64"):
        return False
    try:
        _fast.find_aligned_u64(b"\x00" * 8, 1, 1)
        return True
    except Exception:
        return False


_READONLY_OK = _probe_readonly_support()


def _call_fast(fn, buf, *args):
    """Invoke a scan kernel, passing ``buf`` zero-copy when supported."""
    if _READONLY_OK:
        return fn(buf, *args)
    view = _writable_view(buf)
    try:
        return fn(view, *args)
    finally:
        if view is not buf:
            view.release()


def cpu_features() -> dict:
    if _fast is not None and hasattr(_fast, "cpu_features"):
        return _fast.cpu_features()
    return {"avx2": False, "sse42": False, "fallback": "python"}


def _driver_status() -> dict:
    try:
        from mem_probe import driver_backend as _drv
        return _drv.status()
    except Exception:
        return {"driver_available": False}


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
    ds = _driver_status()
    return {
        "extension_loaded": _fast is not None,
        "backend": backend,
        "features": features,
        "readonly_buffers_supported": _READONLY_OK,
        "bytes_copy_required": (_fast is not None) and (not _READONLY_OK),
        "read_backend": mem_read_backend(),
        "slab_read": has_slab_read(),
        "driver_available": ds.get("driver_available", False),
        "driver_pid": ds.get("attached_pid", 0),
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


def read_slab_many(handle: int, base_addrs, offsets, word_size: int = 8):
    """Slab batch read: one RPM per base covering all offsets.

    Returns flat list of ``len(base_addrs) * len(offsets)`` values, or None
    when the Cython extension lacks it.
    """
    if _fast is not None and hasattr(_fast, "read_slab_many"):
        return _fast.read_slab_many(int(handle), list(base_addrs),
                                     list(offsets), int(word_size))
    return None


def has_slab_read() -> bool:
    return _fast is not None and hasattr(_fast, "read_slab_many")


def driver_attach(pid: int) -> bool:
    """Activate driver read for all Cython _rpm() calls."""
    if _fast is not None and hasattr(_fast, "driver_attach"):
        return bool(_fast.driver_attach(int(pid)))
    return False


def driver_detach() -> None:
    """Deactivate driver read in Cython, revert to NtRVM/RPM."""
    if _fast is not None and hasattr(_fast, "driver_detach"):
        _fast.driver_detach()


def driver_active() -> bool:
    """Return whether the Cython driver fast-path is active."""
    if _fast is not None and hasattr(_fast, "driver_active"):
        return bool(_fast.driver_active())
    return False


def mem_read_backend() -> str:
    """Return which cross-process read backend is active."""
    if _fast is not None and hasattr(_fast, "mem_read_backend"):
        return _fast.mem_read_backend()
    ds = _driver_status()
    if ds.get("driver_available") and ds.get("attached_pid"):
        return "driver"
    return "rpm-python"


def find_aligned_u64(buf, needle: int, max_hits: int = 4096) -> List[int]:
    if _fast is not None and hasattr(_fast, "find_aligned_u64"):
        return list(_call_fast(_fast.find_aligned_u64, buf, needle, max_hits))
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
        return list(_call_fast(_fast.find_aligned_u32, buf, needle, max_hits))
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
        return list(_call_fast(_fast.find_aligned_u64_in_set, buf, list(needles), max_hits))
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
        return list(_call_fast(_fast.find_aligned_u32_in_set, buf, list(needles), max_hits))
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


def find_pattern_masked(buf, pattern: Sequence[int], mask: Sequence[int], max_hits: int = 4096) -> List[int]:
    if _fast is not None and hasattr(_fast, "find_pattern_masked"):
        return list(_call_fast(_fast.find_pattern_masked, buf,
                               list(pattern), list(mask) if mask is not None else None, max_hits))
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
        return list(_call_fast(_fast.narrow_u32_batch, packed, expected))
    data = _as_bytes(packed)
    target = int(expected) & 0xFFFFFFFF
    return [i for i in range(0, len(data) - 3, 4)
            if int.from_bytes(data[i:i + 4], "little", signed=False) == target]


def narrow_u64_batch(packed, expected: int):
    if _fast is not None and hasattr(_fast, "narrow_u64_batch"):
        return list(_call_fast(_fast.narrow_u64_batch, packed, expected))
    data = _as_bytes(packed)
    target = int(expected) & 0xFFFFFFFFFFFFFFFF
    return [i for i in range(0, len(data) - 7, 8)
            if int.from_bytes(data[i:i + 8], "little", signed=False) == target]


def collect_aligned_u64_in_range(buf, lo: int, hi: int, max_out: int = 1 << 20) -> List[int]:
    """Unique 8-aligned u64 values in [lo, hi] (GA klass-pointer pre-filter).

    Cython kernel preferred; numpy vectorised fallback; pure-Python last resort.
    """
    if _fast is not None and hasattr(_fast, "collect_aligned_u64_in_range"):
        return list(_call_fast(_fast.collect_aligned_u64_in_range, buf,
                               int(lo), int(hi), int(max_out)))
    try:
        import numpy as _np
        data = _as_bytes(buf)
        usable = (len(data) // 8) * 8
        if not usable:
            return []
        arr = _np.frombuffer(data[:usable], dtype="<u8")
        m = (arr >= int(lo)) & (arr <= int(hi))
        return _np.unique(arr[m]).tolist()[:int(max_out)]
    except Exception:
        pass
    data = _as_bytes(buf)
    seen = set()
    out: List[int] = []
    lo_i, hi_i = int(lo), int(hi)
    for off in range(0, (len(data) // 8) * 8, 8):
        v = int.from_bytes(data[off:off + 8], "little", signed=False)
        if lo_i <= v <= hi_i and v not in seen:
            seen.add(v)
            out.append(v)
            if len(out) >= int(max_out):
                break
    return out


def decode_i32_kv_pairs(buf, vmin: int, vmax: int) -> dict:
    """Packed (i32 key, i32 value) pairs -> {key: value}, keeping vmin <= v < vmax.

    Cython kernel preferred; numpy fallback; pure-Python last resort.
    """
    if _fast is not None and hasattr(_fast, "decode_i32_kv_pairs"):
        return _call_fast(_fast.decode_i32_kv_pairs, buf, int(vmin), int(vmax))
    data = _as_bytes(buf)
    usable = (len(data) // 8) * 8
    if not usable:
        return {}
    try:
        import numpy as _np
        arr = _np.frombuffer(data[:usable], dtype="<i4").reshape(-1, 2)
        m = (arr[:, 1] >= int(vmin)) & (arr[:, 1] < int(vmax))
        return dict(zip(arr[m, 0].tolist(), arr[m, 1].tolist()))
    except Exception:
        pass
    import struct
    out: dict = {}
    for k, v in struct.iter_unpack("<ii", data[:usable]):
        if int(vmin) <= v < int(vmax):
            out[k] = v
    return out


def find_aligned_u64_with_anchor(buf, anchor_value: int, anchor_off: int, fp_size: int,
                                 expected_slots, max_hits: int = 4096) -> List[int]:
    if _fast is not None and hasattr(_fast, "find_aligned_u64_with_anchor"):
        return list(_call_fast(_fast.find_aligned_u64_with_anchor, buf,
                               anchor_value, anchor_off, fp_size,
                               dict(expected_slots or {}), max_hits))
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
