# -*- coding: utf-8 -*-
"""klass_index - process-wide, version-keyed shared klass-by-name resolver.

Every reader (entity mgr, damage mgr, string pool, scene table, camera, field
resolver) needs to turn a class name into the live ``Il2CppClass*`` for the
running game. Before this module, each one called
``auto_registration_locator.build_live_class_index`` independently — so a single
session scanned the GameAssembly image 5+ times (once per reader, each for a
single class name), and every game launch paid the full multi-hundred-MB scan
again from scratch.

This collapses all of that into:

  1. **One process-level index** keyed by ``(pid, ga_base)``. The union of every
     reader's wanted class names is scanned in a SINGLE pass; later lookups are
     dict hits. A name found once is never re-scanned in the session.

  2. **Persistent RVA cache** keyed by ``game_key`` (sha256 of the GA image's
     first 1 MB — the same version fingerprint ``bundle_store`` uses). On the
     next launch, each class resolves with ONE ``read_u64(ga + rva)`` + a live
     name check instead of a fresh image scan. A wrong RVA (game patched) simply
     fails the name check and falls through to a re-scan, then re-persists — it
     self-heals exactly like the offset bundle.

  3. **Negative cache** so a class the game genuinely doesn't expose is scanned
     for at most once per session, not on every reader's first lookup.

Read-only. The index holds live addresses (never persisted — only RVAs are), so
a relaunch (new ``ga_base``/pid) naturally builds a fresh instance.
"""
from __future__ import annotations

import hashlib
import json
import os
import threading
from typing import Dict, Iterable, Optional, Set

from plugins.star_resonance_plugin.mem.il2cpp.auto_registration_locator import (
    build_live_class_index, find_ga_module, klass_fullname,
)

_HERE = os.path.dirname(os.path.abspath(__file__))
_CACHE_DIR = os.path.join(_HERE, "_cache")
_RVA_PATH = os.path.join(_CACHE_DIR, "klass_rva.json")

# (pid, ga_base) -> SharedKlassIndex
_INSTANCES: Dict[tuple, "SharedKlassIndex"] = {}
_INSTANCES_LOCK = threading.Lock()


def _load_rva_store() -> Dict[str, Dict[str, int]]:
    try:
        with open(_RVA_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except Exception:
        return {}


def _save_rva_store(store: Dict[str, Dict[str, int]]) -> None:
    try:
        os.makedirs(_CACHE_DIR, exist_ok=True)
        tmp = _RVA_PATH + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(store, f, ensure_ascii=False)
        os.replace(tmp, _RVA_PATH)
    except Exception:
        pass


class SharedKlassIndex:
    """One name->klass_ptr index per (pid, ga_base), with persistent RVAs."""

    def __init__(self, pm, ga_module=None):
        self.pm = pm
        self._ga = ga_module or find_ga_module(pm)
        self._ga_base = int(getattr(self._ga, "base", 0) or 0)
        self._resolved: Dict[str, int] = {}        # name -> kp (validated this session)
        self._negative: Set[str] = set()           # names scanned for, not present
        self._lock = threading.Lock()
        self._game_key: Optional[str] = None
        self._rva: Dict[str, int] = {}             # name -> rva (this game version)
        self._rva_loaded = False

    # ---- version key + persisted RVA ----
    def _key(self) -> str:
        if self._game_key is None:
            gk = ""
            try:
                blob = self.pm.read_bytes(self._ga_base, 1024 * 1024) if self._ga_base else None
                if blob:
                    gk = hashlib.sha256(blob).hexdigest()[:16]
            except Exception:
                gk = ""
            self._game_key = gk
        return self._game_key

    def _ensure_rva_loaded(self) -> None:
        if self._rva_loaded:
            return
        key = self._key()
        if key:
            self._rva = dict(_load_rva_store().get(key, {}))
        self._rva_loaded = True

    def _persist_rva(self, new: Dict[str, int]) -> None:
        key = self._key()
        if not key or not new:
            return
        store = _load_rva_store()
        bucket = dict(store.get(key, {}))
        changed = False
        for name, rva in new.items():
            if bucket.get(name) != int(rva):
                bucket[name] = int(rva)
                changed = True
        if changed:
            store[key] = bucket
            _save_rva_store(store)
            self._rva.update(bucket)

    # ---- klass name validation (cheap, 1-2 reads) ----
    def _name_ok(self, kp: int, full: str) -> bool:
        if not kp:
            return False
        try:
            got = klass_fullname(self.pm, kp)
        except Exception:
            return False
        if not got:
            return False
        # accept either the full namespaced name or a short-name match (callers
        # pass both forms; build_live_class_index already matches by short name).
        return got == full or got.rsplit(".", 1)[-1] == full.rsplit(".", 1)[-1]

    # ---- public ----
    def resolve_many(self, names: Iterable[str], *, time_budget_s: float = 40.0) -> Dict[str, int]:
        """Resolve ``names`` -> ``{name: kp}`` (only the found ones).

        Order per name: session cache -> persisted RVA (validate by name) ->
        single GA scan of all still-missing names. Newly discovered RVAs are
        persisted so the next launch skips the scan.
        """
        want = {str(n) for n in names if n}
        out: Dict[str, int] = {}
        with self._lock:
            self._ensure_rva_loaded()
            pending: Set[str] = set()
            for name in want:
                kp = self._resolved.get(name, 0)
                if kp and self._name_ok(kp, name):
                    out[name] = kp
                    continue
                if name in self._negative:
                    continue
                # try persisted RVA (instant warm start)
                rva = self._rva.get(name)
                if rva and self._ga_base:
                    try:
                        kp = int(self.pm.read_u64(self._ga_base + int(rva)) or 0)
                    except Exception:
                        kp = 0
                    if kp and self._name_ok(kp, name):
                        self._resolved[name] = kp
                        out[name] = kp
                        continue
                pending.add(name)
            if pending:
                try:
                    idx = build_live_class_index(
                        self.pm, pending, ga_module=self._ga,
                        time_budget_s=time_budget_s)
                except Exception:
                    idx = {}
                new_rva: Dict[str, int] = {}
                for name in pending:
                    kp = int(idx.get(name, 0) or 0)
                    if not kp:
                        # short-name fallback within this scan's results
                        short = name.rsplit(".", 1)[-1]
                        for full, p in idx.items():
                            if full.rsplit(".", 1)[-1] == short:
                                kp = int(p)
                                break
                    if kp:
                        self._resolved[name] = kp
                        out[name] = kp
                        if self._ga_base and self._ga_base <= kp:
                            new_rva[name] = kp - self._ga_base
                    else:
                        self._negative.add(name)
                self._persist_rva(new_rva)
        return out

    def resolve(self, name: str, *, time_budget_s: float = 40.0) -> int:
        """Resolve a single class name -> kp (0 if not found)."""
        return int(self.resolve_many({name}, time_budget_s=time_budget_s).get(name, 0) or 0)

    def invalidate(self) -> None:
        """Drop the session cache (keep persisted RVAs). Called on server change."""
        with self._lock:
            self._resolved.clear()
            self._negative.clear()


def get_shared_index(pm) -> Optional[SharedKlassIndex]:
    """Return the process-wide klass index for ``pm`` (one per pid+ga_base)."""
    if pm is None:
        return None
    try:
        ga = find_ga_module(pm)
    except Exception:
        ga = None
    ga_base = int(getattr(ga, "base", 0) or 0)
    pid = int(getattr(pm, "pid", 0) or 0)
    key = (pid, ga_base)
    with _INSTANCES_LOCK:
        inst = _INSTANCES.get(key)
        if inst is None:
            inst = SharedKlassIndex(pm, ga_module=ga)
            _INSTANCES[key] = inst
            # bound the registry (relaunches accumulate keys)
            if len(_INSTANCES) > 8:
                for k in list(_INSTANCES.keys()):
                    if k != key:
                        _INSTANCES.pop(k, None)
                        if len(_INSTANCES) <= 4:
                            break
    return inst


def resolve_klasses(pm, names: Iterable[str], *, time_budget_s: float = 40.0) -> Dict[str, int]:
    """Convenience: resolve ``names`` via the shared process index."""
    idx = get_shared_index(pm)
    if idx is None:
        return {}
    return idx.resolve_many(names, time_budget_s=time_budget_s)


__all__ = ["SharedKlassIndex", "get_shared_index", "resolve_klasses"]
