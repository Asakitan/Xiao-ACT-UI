# -*- coding: utf-8 -*-
# mem_attr_reader — decode ZAttrCollection into ``attr_id -> value`` (read-only).
#
# This is the F-B "master unlock": once an entity's attribute values can be read,
# boss HP / breaking_stage / extinction / stun / overdrive / shield / hated_char /
# self combat stats all become readable, because the in-memory attr ids are the
# SAME ``packet_parser.enums.AttrType`` ids the TCP parser already uses.
#
# Layout (Zproto.ZAttrCollection, verified against dump fdc7111b):
#
# ZAttrCollection
# +0x18  ZAttrCacheSlim              cacheSlim_     (Burst/SIMD cache — NOT decoded)
# +0x28  Dictionary<uint, IMixAttr>  mixItemDict_   <- decoded here
#
# Each value is an ``IMixAttr`` == ``ZMixAttr<T>`` (a class) whose current value
# lives in an INLINE ``ZMixItem<T>`` struct (``<Value>k__BackingField``).
# IL2CPPDumper reports that struct's field offsets as 0x0 for the open generic, so
# the concrete ``Value`` offset is **not** statically knowable. It depends on T
# (int vs long vs float), and the dict is heterogeneous, so we discover the offset
# at runtime PER KLASS by calibrating against TCP-known ``(attr_id, value)`` pairs
# (e.g. a monster's MAX_HP that TCP already decoded). Objects of the same concrete
# ``ZMixAttr<T>`` share a klass pointer (obj+0), so one calibrated offset serves
# every attribute of that type.
#
# Read-only. No persistence: calibration is tied to the GameAssembly base and is
# discarded when the base changes (relaunch / ASLR), matching the hybrid
# base-discovery posture (no blindly-trusted cross-session offsets).
from __future__ import annotations

from typing import Dict, List, Optional, Tuple


# ZAttrCollection field
MIXDICT_OFF = 0x28              # Dictionary<uint, IMixAttr> mixItemDict_

# .NET Dictionary<uint, ref> layout (IL2CPP/Mono):
#   +0x10 buckets(int[])  +0x18 entries(Entry[])  +0x20 count(int)
# Entry { int hashCode; int next; uint key; <pad>; ref value } = 24 bytes
DICT_BUCKETS_OFF = 0x10
DICT_ENTRIES_OFF = 0x18
DICT_COUNT_OFF = 0x20
ARRAY_LENGTH_OFF = 0x18
ARRAY_ELEMS_OFF = 0x20
ENTRY_HASH_OFF = 0
ENTRY_NEXT_OFF = 4
ENTRY_KEY_OFF = 8              # uint key
ENTRY_VAL_OFF = 16            # object ref (8-aligned)
ENTRY_SIZE = 24

# Calibration scan window inside an IMixAttr object (skip the 0x10 il2cpp header).
CALIB_SCAN_LO = 0x10
CALIB_SCAN_HI = 0x60
_MIN_HEAP = 0x0000_0001_0000_0000
_MAX_HEAP = 0x0000_7FFF_FFFF_FFFF


def _plausible_ptr(p: Optional[int]) -> bool:
    return bool(p and 0x10000 <= p <= _MAX_HEAP)


class ZAttrReader:
    # Decode an entity/self ``ZAttrCollection`` via its ``mixItemDict_``.
    #
    # ``pm`` only needs the StarProcess read primitives:
    # ``read_bytes / read_u32 / read_u64 / read_i64 / read_i32``.

    def __init__(self, pm, *, ga_base: int = 0, resolver=None):
        self.pm = pm
        # klass_ptr -> (value_offset, width_bytes)  (width in {4, 8})
        self._klass_value_off: Dict[int, Tuple[int, int]] = {}
        self._calibrated_ga: int = 0
        self._req_ga: int = int(ga_base or 0)
        # auto-offset: ZAttrCollection.mixItemDict_ by name (dump), literal fallback.
        from plugins.star_resonance_plugin.mem.il2cpp import auto_offsets as _ao
        self.off_mixdict = _ao.resolve(resolver, "Panda.ZGame.ZAttrCollection", {
            "mixdict": ("mixItemDict_", MIXDICT_OFF)})["mixdict"]

    # ---------- dict walk ----------

    def _read_dict_entries(self, dict_addr: int, max_entries: int = 512) -> List[Tuple[int, int]]:
        # Return ``[(attr_id_uint, imixattr_ptr), ...]`` from a Dictionary<uint, ref>.
        pm = self.pm
        if not _plausible_ptr(dict_addr):
            return []
        try:
            count = pm.read_i32(dict_addr + DICT_COUNT_OFF) or 0
        except Exception:
            return []
        if count <= 0 or count > 8192:
            return []
        entries_arr = pm.read_u64(dict_addr + DICT_ENTRIES_OFF)
        if not _plausible_ptr(entries_arr):
            return []
        try:
            max_len = pm.read_u32(entries_arr + ARRAY_LENGTH_OFF) or 0
        except Exception:
            return []
        scan_n = min(int(max_len), max_entries * 4)
        if scan_n <= 0:
            return []
        base = entries_arr + ARRAY_ELEMS_OFF
        out: List[Tuple[int, int]] = []
        for i in range(scan_n):
            if len(out) >= count or len(out) >= max_entries:
                break
            ep = base + i * ENTRY_SIZE
            try:
                hash_code = pm.read_i32(ep + ENTRY_HASH_OFF)
            except Exception:
                continue
            if hash_code is None or hash_code < 0:   # empty slot (.NET convention)
                continue
            try:
                key = pm.read_u32(ep + ENTRY_KEY_OFF)
                val = pm.read_u64(ep + ENTRY_VAL_OFF)
            except Exception:
                continue
            if val and _plausible_ptr(val):
                out.append((int(key), int(val)))
        return out

    def read_attr_ptrs(self, attrs_obj: int) -> Dict[int, int]:
        # ``attrs_obj`` (ZAttrCollection) -> ``{attr_id: imixattr_ptr}``.
        if not _plausible_ptr(attrs_obj):
            return {}
        mixdict = self.pm.read_u64(attrs_obj + self.off_mixdict)
        if not _plausible_ptr(mixdict):
            return {}
        return dict(self._read_dict_entries(mixdict))

    # ---------- calibration ----------

    def is_calibrated(self, ga_base: int = 0) -> bool:
        ga = int(ga_base or self._req_ga or 0)
        if ga and self._calibrated_ga and ga != self._calibrated_ga:
            return False
        return bool(self._klass_value_off)

    def calibrate(self, attrs_obj: int, known: Dict[int, int], *, ga_base: int = 0) -> int:
        # Discover the per-klass Value offset from TCP-known ``{attr_id: value}``.
        #
        # Objects of the same ``ZMixAttr<T>`` share a klass (obj+0); the consistent
        # offset across multiple same-klass anchors is the ``<Value>`` field. Returns
        # the number of distinct klasses calibrated.
        ga = int(ga_base or self._req_ga or 0)
        if ga and self._calibrated_ga and ga != self._calibrated_ga:
            self._klass_value_off = {}      # base changed → discard stale offsets
        ptrs = self.read_attr_ptrs(attrs_obj)
        if not ptrs:
            return 0
        # klass -> {(off, width): hit_count}
        tally: Dict[int, Dict[Tuple[int, int], int]] = {}
        for aid, expected in known.items():
            ptr = ptrs.get(int(aid))
            if not ptr:
                continue
            try:
                klass = self.pm.read_u64(ptr)
                blob = self.pm.read_bytes(ptr, CALIB_SCAN_HI)
            except Exception:
                continue
            if not klass or not blob or len(blob) < CALIB_SCAN_LO + 8:
                continue
            exp = int(expected)
            kt = tally.setdefault(int(klass), {})
            hi = min(len(blob) - 8, CALIB_SCAN_HI - 8)
            off = CALIB_SCAN_LO
            while off <= hi:
                v64 = int.from_bytes(blob[off:off + 8], "little", signed=True)
                if v64 == exp:
                    kt[(off, 8)] = kt.get((off, 8), 0) + 1
                v32 = int.from_bytes(blob[off:off + 4], "little", signed=True)
                if v32 == exp:
                    kt[(off, 4)] = kt.get((off, 4), 0) + 1
                off += 4
        if not tally:
            return 0
        for klass, cand in tally.items():
            if not cand:
                continue
            # Most-hit offset wins; tie-break to the smaller offset, prefer 8-byte.
            best = max(cand.items(), key=lambda kv: (kv[1], kv[0][1], -kv[0][0]))[0]
            self._klass_value_off[klass] = best
        self._calibrated_ga = ga
        return len(self._klass_value_off)

    # ---------- value read ----------

    def read_value(self, imixattr_ptr: int) -> Optional[int]:
        # Read the calibrated ``Value`` of one IMixAttr (None if uncalibrated).
        if not _plausible_ptr(imixattr_ptr) or not self._klass_value_off:
            return None
        try:
            klass = self.pm.read_u64(imixattr_ptr)
        except Exception:
            return None
        off_w = self._klass_value_off.get(int(klass or 0))
        if not off_w:
            return None
        off, width = off_w
        try:
            return self.pm.read_i64(imixattr_ptr + off) if width == 8 \
                else self.pm.read_i32(imixattr_ptr + off)
        except Exception:
            return None

    def read_attrs(self, attrs_obj: int, attr_ids) -> Dict[int, int]:
        # Read a set of ``attr_id`` values from a ZAttrCollection.
        #
        # Returns only the ids present in the dict AND readable with a calibrated
        # offset. Safe to call before calibration (returns ``{}``).
        want = {int(a) for a in attr_ids}
        ptrs = self.read_attr_ptrs(attrs_obj)
        out: Dict[int, int] = {}
        for aid in want:
            ptr = ptrs.get(aid)
            if not ptr:
                continue
            v = self.read_value(ptr)
            if v is not None:
                out[aid] = int(v)
        return out


__all__ = ["ZAttrReader", "MIXDICT_OFF"]
