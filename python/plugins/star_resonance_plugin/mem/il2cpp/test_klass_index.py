# -*- coding: utf-8 -*-
# Deterministic tests for the shared, version-keyed klass index.
#
# No live process: a fake pm backs a flat address space and the GA scan is
# monkeypatched so we exercise the cache / persisted-RVA / negative-cache logic
# directly.
from __future__ import annotations

import os
import sys
import tempfile

_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from plugins.star_resonance_plugin.mem.il2cpp import klass_index as ki

GA_BASE = 0x140000000
GA_SIZE = 0x01000000


class _Mod:
    def __init__(self, name, base, size):
        self.name, self.base, self.size = name, base, size


class FakePm:
    # Flat fake address space: u64 slots + byte blobs, GameAssembly module.

    def __init__(self, klass_names: dict):
        self.pid = 4321
        self._handle = 0xABCD
        self._u64 = {}        # addr -> u64
        self._bytes = {}      # addr -> bytes
        self.reads = 0
        # plant each klass: name ptr @ kp+0x10, namespace ptr @ kp+0x18=0, cstr blob
        nptr = 0x7F00_0000
        for kp, full in klass_names.items():
            short = full.rsplit(".", 1)[-1]
            self._u64[kp + 0x10] = nptr
            self._u64[kp + 0x18] = 0
            self._bytes[nptr] = short.encode("utf-8") + b"\x00"
            nptr += 0x100
        # game-key source bytes (first 1 MB of GA image)
        self._bytes[GA_BASE] = b"GAKEY-DETERMINISTIC" * 4096

    def list_modules(self):
        return [_Mod("GameAssembly.dll", GA_BASE, GA_SIZE)]

    def read_u64(self, addr):
        self.reads += 1
        return self._u64.get(int(addr), 0)

    def read_bytes(self, addr, n):
        b = self._bytes.get(int(addr))
        if b is None:
            return None
        return b[:n]


def _fresh_store_path():
    fd, path = tempfile.mkstemp(suffix=".json")
    os.close(fd)
    os.remove(path)
    return path


def _patch_store(monkey_path):
    ki._RVA_PATH = monkey_path  # type: ignore[attr-defined]


def test_scan_once_then_cache_hit():
    KP = 0x02000000
    pm = FakePm({KP: "Panda.ZGame.ZEntityMgr"})
    scans = {"n": 0}

    def fake_build(p, wanted, *, ga_module=None, time_budget_s=40.0):
        scans["n"] += 1
        return {"Panda.ZGame.ZEntityMgr": KP} if "Panda.ZGame.ZEntityMgr" in wanted else {}

    orig = ki.build_live_class_index
    ki.build_live_class_index = fake_build
    ki._INSTANCES.clear()
    _patch_store(_fresh_store_path())
    try:
        idx = ki.SharedKlassIndex(pm)
        r1 = idx.resolve_many({"Panda.ZGame.ZEntityMgr"})
        assert r1.get("Panda.ZGame.ZEntityMgr") == KP, r1
        r2 = idx.resolve_many({"Panda.ZGame.ZEntityMgr"})
        assert r2.get("Panda.ZGame.ZEntityMgr") == KP
        assert scans["n"] == 1, f"expected 1 scan (then cache hit), got {scans['n']}"
    finally:
        ki.build_live_class_index = orig
    print("[PASS] test_scan_once_then_cache_hit")


def test_persisted_rva_warm_start():
    KP = 0x02500000
    rva = KP - GA_BASE if KP >= GA_BASE else KP   # KP < GA_BASE -> rva path won't fire
    # put KP above GA_BASE so the RVA round-trips
    KP = GA_BASE + 0x123000
    pm = FakePm({KP: "Panda.ZGame.DamageDataMgr"})
    pm._u64[GA_BASE + (KP - GA_BASE)] = KP        # *(ga+rva) == KP for warm validate
    scans = {"n": 0}

    def fake_build(p, wanted, *, ga_module=None, time_budget_s=40.0):
        scans["n"] += 1
        return {"Panda.ZGame.DamageDataMgr": KP}

    orig = ki.build_live_class_index
    ki.build_live_class_index = fake_build
    ki._INSTANCES.clear()
    _patch_store(_fresh_store_path())
    try:
        idx1 = ki.SharedKlassIndex(pm)
        idx1.resolve_many({"Panda.ZGame.DamageDataMgr"})
        assert scans["n"] == 1
        # a NEW index instance (simulating relaunch) must warm-start from the persisted
        # RVA: validate by name (read_u64(ga+rva)==KP + name match), no scan.
        idx2 = ki.SharedKlassIndex(pm)
        r = idx2.resolve_many({"Panda.ZGame.DamageDataMgr"})
        assert r.get("Panda.ZGame.DamageDataMgr") == KP, r
        assert scans["n"] == 1, f"warm start must not rescan, scans={scans['n']}"
    finally:
        ki.build_live_class_index = orig
    print("[PASS] test_persisted_rva_warm_start")


def test_negative_cache_scans_once():
    pm = FakePm({})
    scans = {"n": 0}

    def fake_build(p, wanted, *, ga_module=None, time_budget_s=40.0):
        scans["n"] += 1
        return {}

    orig = ki.build_live_class_index
    ki.build_live_class_index = fake_build
    ki._INSTANCES.clear()
    _patch_store(_fresh_store_path())
    try:
        idx = ki.SharedKlassIndex(pm)
        idx.resolve_many({"Does.Not.Exist"})
        idx.resolve_many({"Does.Not.Exist"})
        idx.resolve_many({"Does.Not.Exist"})
        assert scans["n"] == 1, f"missing class must scan once then negative-cache, got {scans['n']}"
    finally:
        ki.build_live_class_index = orig
    print("[PASS] test_negative_cache_scans_once")


if __name__ == "__main__":
    test_scan_once_then_cache_hit()
    test_persisted_rva_warm_start()
    test_negative_cache_scans_once()
    print("RESULT: ALL GREEN")
