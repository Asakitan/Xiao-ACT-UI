# -*- coding: utf-8 -*-
"""auto_offsets - resolve game-struct field offsets BY NAME from the live dump
(``DumpCsIndex``), with a hardcoded fallback.

This is the bridge that turns a "fixed offset" reader into an "auto offset" one
without changing its fast read pattern. A reader resolves each offset ONCE (into
an int instance attribute) and keeps using its existing batched/inline reads, so
there is no per-tick cost and no RPC storm. When the game patches and a fresh
bundle is built, ``DumpCsIndex.field_offset`` returns the NEW offset and the
reader self-heals; when the dump lacks the field (open generics report 0x0, or a
class isn't in the curated bundle) the hardcoded literal is used so behaviour is
never worse than before.

A managed field appears in the dump either as a plain field (``foo_``) or, for a
C# auto-property, as its backing field (``<Foo>k__BackingField``); ``field_offset``
tries both so callers can pass the clean logical name.
"""
from __future__ import annotations

from typing import Any, Dict, Optional, Tuple


def dci_of(resolver: Any):
    """Best-effort fetch of a ``DumpCsIndex`` from a StaticResolver / StaticDpsSource
    / raw dci. Returns None when none is reachable (callers then keep literals).

    Fully exception-safe: ``StaticDpsSource.sr`` is a property that opens the bundle
    and may raise, and ``getattr`` does not swallow non-AttributeError, so every
    access is guarded."""
    if resolver is None:
        return None
    try:
        # raw DumpCsIndex
        if hasattr(resolver, "field_offset") and not hasattr(resolver, "dci"):
            return resolver
        # StaticResolver (.dci present)
        dci = getattr(resolver, "dci", None)
        if dci is not None and hasattr(dci, "field_offset"):
            return dci
        # StaticDpsSource (.sr opens the resolver — cheap bundle load, no scan)
        sr = getattr(resolver, "sr", None)
        if sr is not None:
            dci = getattr(sr, "dci", None)
            if dci is not None and hasattr(dci, "field_offset"):
                return dci
    except Exception:
        return None
    return None


def field_offset(dci, class_name: str, field_name: str) -> Optional[int]:
    """``dci.field_offset`` that also tries the ``<Name>k__BackingField`` form."""
    if dci is None:
        return None
    try:
        off = dci.field_offset(class_name, field_name)
        if off is None and not field_name.startswith("<"):
            off = dci.field_offset(class_name, f"<{field_name}>k__BackingField")
        return off
    except Exception:
        return None


def resolve(resolver, class_name: str, spec: Dict[str, Tuple[str, int]],
            *, log=None) -> Dict[str, int]:
    """Resolve ``spec`` = ``{attr: (field_name, fallback)}`` -> ``{attr: offset}``.

    Prefers the dump-resolved offset (so a fresh bundle self-heals a patch); falls
    back to ``fallback`` when the dump has nothing for that field. A dump value of
    0 is only trusted when the fallback is also 0 (open generics legitimately emit
    0x0 — never silently clobber a real literal with it).
    """
    dci = dci_of(resolver)
    out: Dict[str, int] = {}
    fell_back = []
    for attr, (fname, fallback) in spec.items():
        off = field_offset(dci, class_name, fname)
        if off is None or (off == 0 and int(fallback) != 0):
            out[attr] = int(fallback)
            fell_back.append(attr)
        else:
            out[attr] = int(off)
    if fell_back and log is not None:
        try:
            log(f"[auto_offsets] {class_name}: literal fallback for {fell_back} "
                f"(dump missing field — bundle stale or open-generic)")
        except Exception:
            pass
    return out


__all__ = ["dci_of", "field_offset", "resolve"]
