# -*- coding: utf-8 -*-
"""live_field_resolver - resolve IL2CPP field offsets BY NAME from the live process.

This is the "intelligent base+offset" path: it needs NO dump, NO Il2CppDumper, NO
bundle rebuild. Each Il2CppClass carries its own field table in memory:

    klass + 0x80  -> Il2CppFieldInfo[]   (fields)
    Il2CppFieldInfo (stride 0x20):
        +0x00  const char*      name
        +0x08  Il2CppType*      type
        +0x10  Il2CppClass*     parent   (== this klass for its own fields)
        +0x18  int32_t          offset   (field offset from the object start)

So we resolve the klass by name (validated bundle RVA, else an in-memory name
scan -- both already version-robust), then read its field table into
``{name: offset}``. Because this IS the running game's own metadata, the offsets
are always current: a weekly patch self-heals automatically with zero tooling.

The field array is walked until ``FieldInfo.parent != klass`` (layout-independent
stop -- no need to know the version-specific field_count offset). Static fields
report offset 0 / a thread-static sentinel; the readers only ever query instance
fields, and ``offset()`` rejects an implausible 0 so a name collision can't poison
a real layout. Read-only; caches per-klass field maps for the session.
"""
from __future__ import annotations

from typing import Dict, Optional

CLASS_FIELDS_OFF = 0x80     # Il2CppClass.fields -> Il2CppFieldInfo[]
FI_NAME_OFF = 0x0
FI_PARENT_OFF = 0x10
FI_OFFSET_OFF = 0x18
FI_STRIDE = 0x20
_MAX_FIELDS = 1024          # safety cap (largest classes have a few hundred fields)
_MIN_PTR = 0x10000
_MAX_PTR = 0x7FFF_FFFF_FFFF


class LiveFieldResolver:
    """``class_name``/``field_name`` -> offset, read live from the process metadata."""

    def __init__(self, pm, *, klass_resolver=None, time_budget_s: float = 20.0):
        self.pm = pm
        # optional fast klass-by-name (e.g. StaticResolver.resolve_klass: bundle RVA
        # validated by live klass name). Falls back to an in-memory name scan.
        self._klass_resolver = klass_resolver
        self._budget = float(time_budget_s)
        self._klass: Dict[str, int] = {}     # class_name -> klass_ptr (0 = not found)
        self._fields: Dict[int, Dict[str, int]] = {}  # klass_ptr -> {field: offset}

    # ---------- klass ----------
    def _resolve_klass(self, class_name: str) -> int:
        kp = self._klass.get(class_name)
        if kp is not None:
            return kp
        kp = 0
        if self._klass_resolver is not None:
            try:
                kp = int(self._klass_resolver(class_name) or 0)
            except Exception:
                kp = 0
        if not kp:
            try:
                # Process-wide shared index: one GA scan serves every reader and
                # warm starts from the persisted per-version RVA cache.
                from plugins.star_resonance_plugin.mem.il2cpp.klass_index import resolve_klasses
                kp = int(resolve_klasses(self.pm, {class_name},
                                         time_budget_s=self._budget).get(class_name, 0) or 0)
            except Exception:
                kp = 0
        self._klass[class_name] = kp
        return kp

    # ---------- fields ----------
    def _read_cstr(self, addr: int, n: int = 96) -> str:
        if not addr or not (_MIN_PTR <= addr <= _MAX_PTR):
            return ""
        b = self.pm.read_bytes(addr, n)
        if not b:
            return ""
        e = b.find(b"\x00")
        return (b[:e] if e >= 0 else b).decode("utf-8", "replace")

    def _field_map(self, kp: int) -> Dict[str, int]:
        m = self._fields.get(kp)
        if m is not None:
            return m
        m = {}
        fields = self.pm.read_u64(kp + CLASS_FIELDS_OFF)
        if fields and _MIN_PTR <= fields <= _MAX_PTR:
            for i in range(_MAX_FIELDS):
                fi = fields + i * FI_STRIDE
                parent = self.pm.read_u64(fi + FI_PARENT_OFF)
                if parent != kp:           # past this klass's own field block
                    break
                nm = self._read_cstr(self.pm.read_u64(fi + FI_NAME_OFF), 96)
                if not nm:
                    continue
                off = self.pm.read_i32(fi + FI_OFFSET_OFF)
                if off is not None:
                    m[nm] = int(off)
        self._fields[kp] = m
        return m

    def field_offset(self, class_name: str, field_name: str) -> Optional[int]:
        """Live offset of ``class_name.field_name`` (tries the auto-property backing
        field), or None when the klass/field can't be resolved from memory."""
        kp = self._resolve_klass(class_name)
        if not kp:
            return None
        m = self._field_map(kp)
        if field_name in m:
            return m[field_name]
        return m.get(f"<{field_name}>k__BackingField")

    def class_resolved(self, class_name: str) -> bool:
        return bool(self._resolve_klass(class_name))


__all__ = ["LiveFieldResolver"]
