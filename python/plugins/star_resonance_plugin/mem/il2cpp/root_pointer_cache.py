# -*- coding: utf-8 -*-
# root_pointer_cache - persistent, game-keyed cache of singleton instance pointers.
#
# This is THE component that delivers the *steady-state O(1)* contract in the plan:
# it caches the *address* of each game singleton / player-anchored object so the
# per-tick reader poll is a handful of ``ReadProcessMemory`` pointer-chases, never
# an O(heap) scan. All scans, metadata loads, and klass-only resolve-by-name work
# live in Stage A (``_bootstrap``) and write their result through this cache; Stage B
# (``_poll_fast``) only reads it + validates via a single klass-pointer read.
#
# Layout (per game_key, on disk under ``mem_probe/_cache/root_ptrs_<game_key>.json``)::
#
# {
# "game_key": "<sha256 first-1mb of GameAssembly.dll>",
# "version": 1,
# "entries": {
# "<name>": {
# "addr": <int instance address>,
# "klass_addr": <int expected klass pointer (Stage-B trust check)>,
# "klass_name": "<optional human name for diagnostics>",
# "extras": { ... free-form per-entry payload, e.g. region_bases for
# NameplateReader, dict_addrs for EntityMgrReader ... },
# "set_at": <unix timestamp>
# },
# ...
# }
# }
#
# Public API (thread-safe, never raises from inside the cache; readers must
# fall back to Stage A bootstrap when ``get/validate`` indicates no entry):
#
# get(game_key, name)            -> Optional[int]            # addr or None
# get_entry(game_key, name)      -> Optional[dict]           # full entry
# validate(game_key, name, pm)   -> bool                     # one read + klass compare
# set(game_key, name, *, addr, klass_addr=None, klass_name=None, extras=None)
# invalidate(game_key, name)
# invalidate_all(game_key)
# reload()                       # drop in-memory copy; reload from disk on next get
# persist(game_key)              # atomic write-through of one game_key
#
# Drift / invalidation model:
# - "validate" == one ``pm.read_u64(addr)`` and equality vs the cached
# ``klass_addr``. Mismatch ⇒ caller drops back to Stage A. This is the *only*
# Stage-B read that can fail, and it is independent of heap/metadata size.
# - Game patch auto-invalidates (different sha-256 → different game_key → empty
# cache → fresh bootstrap). No silent stale reads possible.
#
# Persistence policy:
# - Writes are batched: in-memory mutated entries are flushed by an explicit
# ``persist(game_key)`` call, or by ``flush_all()`` on graceful shutdown.
# This avoids per-tick disk I/O on the hot path. Callers in Stage A call
# ``set(...)`` then ``persist(game_key)`` once at the end of bootstrap.
#
# Failure semantics / no-feature-cuts:
# - All operations are exception-safe; a corrupted disk cache is ignored
# (loaded as empty), and ``set/get`` continue returning None until the next
# bootstrap populates it. Readers must therefore *always* be able to run
# without this cache (bootstrap-on-every-poll slow path) — flipping
# ``mem_root_ptr_cache_enabled = False`` in config restores that behavior.
from __future__ import annotations

import json
import os
import threading
import time
from typing import Any, Dict, Optional

_HERE = os.path.dirname(os.path.abspath(__file__))
_CACHE_DIR = os.path.join(_HERE, "_cache")


def _cache_path(game_key: str) -> str:
    # Disk path for one game_key's root-pointer cache.
    return os.path.join(_CACHE_DIR, f"root_ptrs_{game_key}.json")


# ───────────────────────── in-memory mirror ─────────────────────────
# {game_key: {"game_key": ..., "version": 1, "entries": {name: entry}}}
_STORE: Dict[str, Dict[str, Any]] = {}
_LOCK = threading.RLock()
_DIRTY: set = set()              # game_keys with unsaved mutations


def _load_locked(game_key: str) -> Dict[str, Any]:
    # Load one game_key into the in-memory mirror (or empty skeleton). Idempotent.
    if game_key in _STORE:
        return _STORE[game_key]
    path = _cache_path(game_key)
    data: Dict[str, Any] = {"game_key": game_key, "version": 1, "entries": {}}
    if os.path.isfile(path):
        try:
            with open(path, "r", encoding="utf-8") as f:
                raw = json.load(f)
            # basic shape guard; any corruption -> treat as empty (no silent stale reads)
            if isinstance(raw, dict) and raw.get("game_key") == game_key \
                    and isinstance(raw.get("entries"), dict):
                data = raw
        except Exception:
            data = {"game_key": game_key, "version": 1, "entries": {}}
    _STORE[game_key] = data
    return data


def get_entry(game_key: str, name: str) -> Optional[Dict[str, Any]]:
    # Full cached entry for ``name`` under ``game_key`` (or None).
    if not game_key or not name:
        return None
    with _LOCK:
        data = _STORE.get(game_key) or _load_locked(game_key)
        entry = data.get("entries", {}).get(name)
        return dict(entry) if isinstance(entry, dict) else None


def get(game_key: str, name: str) -> Optional[int]:
    # Cached instance address (or None / not-yet-bootstrapped).
    entry = get_entry(game_key, name)
    if not entry:
        return None
    addr = entry.get("addr")
    if isinstance(addr, int) and addr > 0:
        return addr
    return None


def validate(game_key: str, name: str, pm) -> bool:
    # Stage-B trust check: one ``read_u64(addr)`` == expected klass_addr.
    #
    # Returns False iff: entry absent, addr is non-positive, the read failed,
    # or the live klass pointer at addr no longer matches the cached one. A False
    # return is the signal for the reader to drop back to Stage A.
    #
    # ``pm`` is any object exposing ``read_u64(addr) -> int | None``. If pm lacks
    # that method (or it raises), we treat it as a non-match and let the caller
    # bootstrap. Never raises from inside the cache.
    entry = get_entry(game_key, name)
    if not entry:
        return False
    addr = entry.get("addr")
    expected_klass = entry.get("klass_addr")
    if not isinstance(addr, int) or addr <= 0 or not isinstance(expected_klass, int):
        # An entry without a klass_addr can never be trusted from Stage B (the
        # hallmark of the O(1) contract); caller must bootstrap to fill it in.
        return False
    read = getattr(pm, "read_u64", None)
    if read is None:
        return False
    try:
        live_klass = read(addr)
    except Exception:
        return False
    return live_klass is not None and int(live_klass) == int(expected_klass)


def set(game_key: str, name: str, *, addr: int,
        klass_addr: Optional[int] = None, klass_name: Optional[str] = None,
        extras: Optional[Dict[str, Any]] = None) -> None:
    # Insert / replace an entry. Marks the game_key dirty for later ``persist()``.
    #
    # Klass_addr is optional but *required* for Stage-B validation; callers should
    # always supply it when they have just done a Stage-A scan (which by definition
    # read the klass pointer).
    if not game_key or not name or not isinstance(addr, int) or addr <= 0:
        return
    entry: Dict[str, Any] = {
        "addr": int(addr),
        "klass_addr": int(klass_addr) if isinstance(klass_addr, int) else None,
        "klass_name": klass_name or "",
        "extras": dict(extras) if extras else {},
        "set_at": int(time.time()),
    }
    with _LOCK:
        data = _load_locked(game_key)
        data.setdefault("entries", {})[name] = entry
        _DIRTY.add(game_key)


def invalidate(game_key: str, name: str) -> None:
    # Drop one entry (Stage-C relocate signal). Marks dirty for persist.
    if not game_key or not name:
        return
    with _LOCK:
        data = _STORE.get(game_key)
        if data and isinstance(data.get("entries"), dict) \
                and name in data["entries"]:
            data["entries"].pop(name, None)
            _DIRTY.add(game_key)


def invalidate_all(game_key: str) -> None:
    # Drop every entry under one game_key (full bootstrap re-trigger).
    if not game_key:
        return
    with _LOCK:
        data = _STORE.get(game_key)
        if data and isinstance(data.get("entries"), dict):
            data["entries"] = {}
            _DIRTY.add(game_key)


def persist(game_key: str) -> bool:
    # Atomic write-through of one game_key's pending mutations. Returns success.
    #
    # Safe no-op when the game_key has no pending writes. Failures (e.g. readonly
    # media) are swallowed and logged to stderr; in-memory mirror stays usable so
    # the in-session O(1) polling continues — only cross-session persistence is lost.
    if not game_key:
        return False
    with _LOCK:
        data = _STORE.get(game_key)
        if data is None:
            return True           # nothing to write
        _DIRTY.discard(game_key)
        try:
            os.makedirs(_CACHE_DIR, exist_ok=True)
            tmp_path = _cache_path(game_key) + ".tmp"
            with open(tmp_path, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False)
                f.flush()
                try:
                    os.fsync(f.fileno())
                except OSError:
                    pass
            os.replace(tmp_path, _cache_path(game_key))
            return True
        except Exception as exc:  # noqa: BLE001 - persistence must never break callers
            import sys
            print(f"[root_pointer_cache] persist({game_key}) failed: {exc}",
                  file=sys.stderr)
            return False


def flush_all() -> None:
    # Best-effort persist of every dirty game_key (graceful-shutdown hook).
    with _LOCK:
        keys = list(_DIRTY)
    for k in keys:
        persist(k)


def reload() -> None:
    # Drop in-memory mirror; next get() reloads from disk.
    with _LOCK:
        _STORE.clear()
        _DIRTY.clear()


def coverage(game_key: str) -> Dict[str, Any]:
    # Diagnostics for ``health()``: how many entries, ages, freshness.
    with _LOCK:
        data = _STORE.get(game_key) or _load_locked(game_key)
        entries = data.get("entries", {})
    now = int(time.time())
    ages = [now - int(e.get("set_at") or now) for e in entries.values()]
    return {
        "game_key": game_key,
        "entries": len(entries),
        "names": sorted(entries.keys()),
        "oldest_age_s": max(ages) if ages else 0,
        "dirty": game_key in _DIRTY,
    }


__all__ = [
    "get", "get_entry", "validate", "set", "invalidate", "invalidate_all",
    "persist", "flush_all", "reload", "coverage", "_cache_path",
]
