# -*- coding: utf-8 -*-
"""Game-specific Cython combat facades (thin wrappers over _sao_cy_memscan).

Extracted from platform cy_memscan.py — these functions decode Star Resonance
Il2Cpp entity/boss combat structures and skill-CD arrays.
"""
from __future__ import annotations

from typing import Iterable, List, Sequence

try:
    import _sao_cy_memscan as _fast  # type: ignore[import-not-found]
except Exception:
    _fast = None

from mem_probe.cy_memscan import _as_bytes, _call_fast


def read_boss_combat_cached(handle: int, ent_addr: int,
                            off_attrs: int = 0x48, off_indexpart: int = 0x18,
                            off_values: int = 0x20):
    if _fast is not None and hasattr(_fast, "read_boss_combat_cached"):
        try:
            return _fast.read_boss_combat_cached(
                int(handle), int(ent_addr),
                int(off_attrs), int(off_indexpart), int(off_values))
        except Exception:
            pass
    return None


def boss_cache_invalidate() -> None:
    if _fast is not None and hasattr(_fast, "boss_cache_invalidate"):
        _fast.boss_cache_invalidate()


def has_boss_combat_cached() -> bool:
    return _fast is not None and hasattr(_fast, "read_boss_combat_cached")


def read_entity_combat_many(handle: int, ent_addrs,
                            off_attrs: int = 0x48, off_indexpart: int = 0x18,
                            off_values: int = 0x20):
    if _fast is not None and hasattr(_fast, "read_entity_combat_many"):
        try:
            return _fast.read_entity_combat_many(int(handle), list(ent_addrs),
                                                 int(off_attrs), int(off_indexpart),
                                                 int(off_values))
        except TypeError:
            return _fast.read_entity_combat_many(int(handle), list(ent_addrs))
    return None


def has_full_combat_decode() -> bool:
    return _fast is not None and hasattr(_fast, "read_entity_combat_many")


def scan_repeated_field_candidates(buf, min_count: int = 50, max_count: int = 1024) -> List[dict]:
    if _fast is not None and hasattr(_fast, "scan_repeated_field_candidates"):
        return list(_call_fast(_fast.scan_repeated_field_candidates, buf,
                               int(min_count), int(max_count)))
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
    if _fast is not None and hasattr(_fast, "find_skill_cd_arrays_in_blob"):
        return list(_call_fast(_fast.find_skill_cd_arrays_in_blob, buf,
                               int(region_base), list(skill_ids),
                               int(min_count), int(max_count), int(max_candidates)))
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
