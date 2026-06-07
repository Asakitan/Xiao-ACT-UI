# -*- coding: utf-8 -*-
"""auto_registration_locator - derive klass pointers by NAME, in-memory (no dump).

The per-version data that breaks on every game patch is the klass RVA (the address
of the static Il2CppClass* in GameAssembly's .data). The Il2CppClass objects
themselves carry their name (+0x10) and namespace (+0x18), and GameAssembly's
.data is full of pointers to them. So we can rebuild a name -> klass_ptr index
purely from process memory:

    scan GameAssembly's image for 8-aligned u64 pointing into the klass region,
    deref, validate the target as an Il2CppClass by reading an ASCII name, and
    match the (namespace.name) against the wanted set.

No script.json, no Il2CppDumper, no 250MB dump, no hard-coded RVA -> version-robust
and onedir-safe. The klass region is auto-discovered (no hard-coded address): we
validate every candidate by name, so wrong regions simply yield no valid names.

Field offsets still come from the per-version bundle (layout-stable, name-keyed).
"""
from __future__ import annotations

import re
import time
from typing import Dict, Optional, Set

try:
    import numpy as _np
except Exception:  # pragma: no cover
    _np = None

CLASS_NAME_OFF = 0x10
CLASS_NAMESPACE_OFF = 0x18
GAME_ASSEMBLY = "GameAssembly.dll"

# il2cpp class/namespace names: identifiers plus generics/nested markers.
_IDENT = re.compile(rb"^[A-Za-z_<][A-Za-z0-9_`<>.|=,\[\]+ -]{0,180}$")
# Candidate klass-region bounds (low managed/metadata heap, well below the GA image
# base ~0x7ffb_xxxxxxxx). Broad on purpose; the name check rejects non-klasses.
KLASS_LO = 0x0100_0000
KLASS_HI = 0x8000_0000


def find_ga_module(pm):
    for m in pm.list_modules():
        if m.name.lower() == GAME_ASSEMBLY.lower():
            return m
    return None


def _read_cstr(pm, addr: int, n: int = 160) -> Optional[bytes]:
    if not (0x10000 <= (addr or 0) <= 0x7FFFFFFFFFFF):
        return None
    b = pm.read_bytes(addr, n)
    if not b:
        return None
    e = b.find(b"\x00")
    return b[:e] if e >= 0 else b


def klass_fullname(pm, kp: int) -> Optional[str]:
    """Return 'namespace.Name' for a candidate Il2CppClass*, or None if not one."""
    name = _read_cstr(pm, pm.read_u64(kp + CLASS_NAME_OFF), 128)
    if not name or not _IDENT.match(name):
        return None
    nm = name.decode("utf-8", "replace")
    nsp = pm.read_u64(kp + CLASS_NAMESPACE_OFF)
    ns = _read_cstr(pm, nsp, 128) if nsp else None
    if ns and _IDENT.match(ns):
        return f"{ns.decode('utf-8', 'replace')}.{nm}"
    return nm


def build_live_class_index(pm, wanted: Set[str], *, ga_module=None,
                           klass_lo: int = KLASS_LO, klass_hi: int = KLASS_HI,
                           time_budget_s: float = 60.0) -> Dict[str, int]:
    """Resolve ``wanted`` full class names -> klass_ptr by scanning GameAssembly.

    Validates every candidate by reading its name; early-exits once all wanted
    names are found. Caller should cache the result for the session (and the
    discovered RVAs keyed by game_key for instant next-launch resolution).
    """
    ga = ga_module or find_ga_module(pm)
    if ga is None:
        return {}
    base, size = int(ga.base), int(ga.size)
    want = {str(w) for w in wanted}
    short_want = {w.rsplit(".", 1)[-1] for w in want}
    found: Dict[str, int] = {}
    seen: Set[int] = set()
    t0 = time.time()
    off = 0
    chunk = 32 * 1024 * 1024
    while off < size and len(found) < len(want) and (time.time() - t0) < time_budget_s:
        n = min(chunk, size - off)
        blob = pm.read_bytes(base + off, n)
        if blob is None:
            off += n
            continue
        # vectorised klass-range pre-filter (numpy) or scalar fallback
        cands = []
        usable = (len(blob) // 8) * 8
        if _np is not None and usable:
            arr = _np.frombuffer(blob[:usable], dtype="<u8")
            m = (arr >= klass_lo) & (arr <= klass_hi)
            cands = _np.unique(arr[m]).tolist()
        else:
            import struct
            uniq = set()
            for p in range(0, usable - 8, 8):
                v = struct.unpack_from("<Q", blob, p)[0]
                if klass_lo <= v <= klass_hi:
                    uniq.add(v)
            cands = list(uniq)
        for v in cands:
            v = int(v)
            if v in seen:
                continue
            seen.add(v)
            full = klass_fullname(pm, v)
            if not full:
                continue
            if full.rsplit(".", 1)[-1] not in short_want:
                continue
            if full in want and full not in found:
                found[full] = v
                if len(found) >= len(want):
                    break
        off += n
    return found


class LiveKlassResolver:
    """A name -> klass_ptr resolver backed by an in-memory index (cached)."""

    def __init__(self, pm, wanted: Set[str], *, time_budget_s: float = 60.0):
        self.pm = pm
        self._wanted = {str(w) for w in wanted}
        self._budget = float(time_budget_s)
        self._index: Dict[str, int] = {}
        self._built = False

    def build(self) -> int:
        self._index = build_live_class_index(self.pm, self._wanted,
                                             time_budget_s=self._budget)
        self._built = True
        return len(self._index)

    def resolve(self, name: str) -> int:
        if not self._built:
            self.build()
        kp = self._index.get(name, 0)
        if kp:
            return kp
        # short-name fallback (caller may pass 'ZEntityMgr' or full name)
        short = name.rsplit(".", 1)[-1]
        for full, p in self._index.items():
            if full.rsplit(".", 1)[-1] == short:
                return p
        return 0


__all__ = ["build_live_class_index", "LiveKlassResolver", "klass_fullname",
           "find_ga_module"]
