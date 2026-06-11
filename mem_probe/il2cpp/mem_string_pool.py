# -*- coding: utf-8 -*-
"""mem_string_pool - reusable mlid -> localized string bridge (read-only).

Config-table row blobs store localized text as an MLString id (mlid), not a
string pointer. The client's own localization pool resolves them:

  Panda.Module.StringPoolRuntimeImpl
    allLocalizationString_ : string[]               (slot -> Il2CppString)
    indexes_               : NativeArray<KV<int,int>>  (mlid -> slot)

Every IL2CPP class + field offset is resolved BY NAME (live field table walk
with tiny literal fallbacks); only the runtime-defined string/array/NativeArray
layouts are constants. One heavy heap scan on ``build()``, then O(1) lookups.
Never raises; returns "" on any failure.
"""
from __future__ import annotations

import struct
from typing import Dict, Optional

from mem_probe.il2cpp.klass_index import resolve_klasses

try:
    from mem_probe import cy_memscan as _cy
except Exception:  # pragma: no cover
    _cy = None

# IL2CPP / .NET structural layout (runtime-defined, layout-stable across patches)
CLASS_FIELDS_OFF = 0x80          # Il2CppClass.fields -> Il2CppFieldInfo[]
FI_NAME_OFF, FI_PARENT_OFF, FI_OFF_OFF, FI_STRIDE = 0x0, 0x10, 0x18, 0x20
STR_LEN_OFF, STR_CHARS_OFF = 0x10, 0x14          # Il2CppString
ARR_LEN_OFF, ARR_ELEMS_OFF = 0x18, 0x20          # IL2CPP array
NATIVEARRAY_LEN_OFF = 0x8                         # NativeArray<T>: m_Buffer@0, m_Length@8

POOL_CLS = "Panda.Module.StringPoolRuntimeImpl"

# layout fallbacks if the live field-table walk can't resolve a name (kept tiny)
_FALLBACK = {("StringPoolRuntimeImpl", "allLocalizationString_"): 0x10,
             ("StringPoolRuntimeImpl", "indexes_"): 0x18}


def _plaus(p: Optional[int]) -> bool:
    return bool(p and 0x10000 <= p <= 0x7FFFFFFFFFFF)


def _is_cjk(s: str) -> bool:
    return any("一" <= c <= "鿿" for c in (s or ""))


class StringPoolBridge:
    """mlid -> localized CN string. ``build()`` once (heap scan), then O(1)."""

    def __init__(self, dps_source):
        self._src = dps_source
        self._sr = dps_source.sr
        self.pm = dps_source.sr.pm
        self._klass: Dict[str, int] = {}
        self._field: Dict[tuple, Optional[int]] = {}
        self._pool_arr = 0
        self._pool_len = 0
        self._pool_kv: Dict[int, int] = {}

    # ── klass by name + field offset by live field table ──────────────────────
    def resolve_klass(self, full: str, *, time_budget_s: float = 90) -> int:
        if full in self._klass:
            return self._klass[full]
        # Shared process index: one GA scan + persisted per-version RVA warm start.
        kp = int(resolve_klasses(self.pm, {full}, time_budget_s=time_budget_s).get(full, 0) or 0)
        self._klass[full] = kp
        return kp

    def field_off(self, klass: int, field: str) -> Optional[int]:
        key = (klass, field)
        if key in self._field:
            return self._field[key]
        off = None
        fields = self.pm.read_u64(klass + CLASS_FIELDS_OFF) if _plaus(klass) else 0
        if _plaus(fields):
            want = field.encode("utf-8")
            backing = ("<%s>k__BackingField" % field).encode("utf-8")
            # Read the whole FieldInfo slab once (512 * 0x20 = 16 KB) and parse the
            # parent/name/offset columns locally — turns ~3 RPMs/field into 1 block
            # read + a few name-pointer reads.
            import struct
            slab = self.pm.read_bytes(fields, 512 * FI_STRIDE)
            if slab and len(slab) >= FI_STRIDE:
                nfi = len(slab) // FI_STRIDE
                for i in range(nfi):
                    o = i * FI_STRIDE
                    parent = struct.unpack_from("<Q", slab, o + FI_PARENT_OFF)[0]
                    if parent != klass:
                        break
                    namep = struct.unpack_from("<Q", slab, o + FI_NAME_OFF)[0]
                    nm = self.pm.read_bytes(namep, 64) if _plaus(namep) else None
                    if nm:
                        nm = nm.split(b"\x00", 1)[0]
                        if nm == want or nm == backing:
                            off = struct.unpack_from("<i", slab, o + FI_OFF_OFF)[0]
                            break
            else:
                for i in range(512):
                    fi = fields + i * FI_STRIDE
                    if self.pm.read_u64(fi + FI_PARENT_OFF) != klass:
                        break
                    namep = self.pm.read_u64(fi + FI_NAME_OFF)
                    nm = self.pm.read_bytes(namep, 64) if _plaus(namep) else None
                    if nm:
                        nm = nm.split(b"\x00", 1)[0]
                        if nm == want or nm == backing:
                            off = self.pm.read_i32(fi + FI_OFF_OFF)
                            break
        self._field[key] = off
        return off

    def read_str(self, sp: int) -> str:
        if not _plaus(sp):
            return ""
        ln = self.pm.read_u32(sp + STR_LEN_OFF) or 0
        if ln <= 0 or ln > 128:
            return ""
        raw = self.pm.read_bytes(sp + STR_CHARS_OFF, ln * 2)
        return raw.decode("utf-16-le", "replace") if raw else ""

    # ── pool build (one heap scan) ─────────────────────────────────────────────
    def build(self) -> bool:
        """Locate the live StringPoolRuntimeImpl and index mlid -> slot."""
        if self._pool_kv:
            return True
        try:
            pk = self.resolve_klass(POOL_CLS)
            if not pk:
                return False
            arr_off = self.field_off(pk, "allLocalizationString_") \
                or _FALLBACK[("StringPoolRuntimeImpl", "allLocalizationString_")]
            idx_off = self.field_off(pk, "indexes_") \
                or _FALLBACK[("StringPoolRuntimeImpl", "indexes_")]
            # find_instances yields many klass-metadata false hits; validate the real
            # impl by a sane string[] pool + a sane indexes_ NativeArray + a real string.
            for p in self._sr.find_instances(pk, max_hits=4000):
                arr = self.pm.read_u64(p + arr_off)
                if not _plaus(arr):
                    continue
                alen = self.pm.read_u32(arr + ARR_LEN_OFF) or 0
                if not (10000 <= alen <= 500000):
                    continue
                if not self.read_str(self.pm.read_u64(arr + ARR_ELEMS_OFF)):
                    continue
                ibuf = self.pm.read_u64(p + idx_off)
                ilen = self.pm.read_i32(p + idx_off + NATIVEARRAY_LEN_OFF) or 0
                if not (_plaus(ibuf) and 1000 <= ilen <= 2_000_000):
                    continue
                raw = self.pm.read_bytes(ibuf, ilen * 8) or b""
                # mlid -> slot: up to ~2M (i32,i32) pairs. The decode + range filter
                # is the dominant one-time build cost; push it into the Cython kernel
                # (numpy / pure-Python fallbacks inside decode_i32_kv_pairs).
                if _cy is not None and hasattr(_cy, "decode_i32_kv_pairs"):
                    kv = _cy.decode_i32_kv_pairs(raw, 0, int(alen))
                else:
                    kv = {}
                    for j in range(len(raw) // 8):
                        k, v = struct.unpack_from("<ii", raw, j * 8)
                        if 0 <= v < alen:
                            kv[k] = v
                if len(kv) < 1000:
                    continue
                self._pool_arr, self._pool_len, self._pool_kv = arr, alen, kv
                return True
            return False
        except Exception:
            return False

    @property
    def ready(self) -> bool:
        return bool(self._pool_kv)

    def resolve(self, mlid: Optional[int]) -> str:
        """mlid -> localized string ("" when unknown / pool not built)."""
        if not mlid:
            return ""
        v = self._pool_kv.get(int(mlid))
        if v is None or not (0 <= v < self._pool_len):
            return ""
        return self.read_str(self.pm.read_u64(self._pool_arr + ARR_ELEMS_OFF + v * 8))


__all__ = ["StringPoolBridge", "POOL_CLS", "_FALLBACK", "_plaus", "_is_cjk"]
