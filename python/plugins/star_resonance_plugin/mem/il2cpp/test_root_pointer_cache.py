# -*- coding: utf-8 -*-
# O(1) steady-state contract test for RootPointerCache.
#
# The user's central requirement: the per-tick poll must be **near-instant**,
# i.e. **O(chain depth)** syscalls = bounded small constant K, INDEPENDENT of
# heap size, scene entity count, dump version, or buff count.
#
# This script enforces that contract with a *counting* mock `pm`. It deliberately
# avoids any third-party test framework (matches the project's
# ``python -m mem_probe.il2cpp.test_*`` plain-script convention) so it runs on the
# minimal install. Run::
#
# python -m mem_probe.il2cpp.test_root_pointer_cache
from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
from typing import Dict, Optional

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _SAO not in sys.path:
    sys.path.insert(0, _SAO)

from plugins.star_resonance_plugin.mem.il2cpp import root_pointer_cache as _rpc


GAME_KEY = "deadbeef" * 8


class CountingPM:
    # Mock pm exposing the ``read_u64(addr)`` contract validate() uses.
    #
    # Counts every read so the O(1) assertion can inspect syscall counts.

    def __init__(self, mapping: Optional[Dict[int, int]] = None):
        self._map = dict(mapping or {})
        self.read_u64_calls = 0

    def read_u64(self, addr: int) -> Optional[int]:
        self.read_u64_calls += 1
        return self._map.get(int(addr))


# tiny assert helper — collects passes/fails so the script returns a nonzero exit
# if any contract violation is found.
_RESULTS: list = []


def check(name: str, cond: bool, detail: str = "") -> None:
    status = "PASS" if cond else "FAIL"
    line = f"[{status}] {name}"
    if detail:
        line += f"   {detail}"
    print(line)
    _RESULTS.append((name, bool(cond)))


def setup_fresh() -> str:
    # Point RootPointerCache at a fresh temp dir + clear in-memory mirror.
    tmp = tempfile.mkdtemp(prefix="rpc_test_")
    _rpc._CACHE_DIR = tmp
    _rpc.reload()
    return tmp


def teardown(tmp: str) -> None:
    _rpc.reload()
    shutil.rmtree(tmp, ignore_errors=True)


# ── 1. the central O(1) contract: validate is exactly ONE read_u64 ────────────

def test_validate_match_is_one_read():
    _rpc.set(GAME_KEY, "SingletonX", addr=0x1_0000_0000,
             klass_addr=0x2_0000_0000, klass_name="SingletonX")
    pm = CountingPM({0x1_0000_0000: 0x2_0000_0000})
    ok = _rpc.validate(GAME_KEY, "SingletonX", pm)
    check("validate(match) returns True", ok is True)
    check("validate(match) is exactly 1 read_u64",
          pm.read_u64_calls == 1, f"got {pm.read_u64_calls}")


def test_validate_mismatch_is_one_read():
    _rpc.set(GAME_KEY, "SingletonY", addr=0x1_0000_0000,
             klass_addr=0x2_0000_0000, klass_name="SingletonY")
    pm = CountingPM({0x1_0000_0000: 0x3_0000_0000})   # drift
    ok = _rpc.validate(GAME_KEY, "SingletonY", pm)
    check("validate(mismatch) returns False", ok is False)
    check("validate(mismatch) is still 1 read_u64",
          pm.read_u64_calls == 1, f"got {pm.read_u64_calls}")


# ── 2. missing entry must not touch pm ────────────────────────────────────────

def test_missing_entry_is_zero_reads():
    pm = CountingPM()
    ok = _rpc.validate(GAME_KEY, "UnknownSingleton", pm)
    check("validate(missing entry) returns False", ok is False)
    check("validate(missing entry) is 0 reads",
          pm.read_u64_calls == 0, f"got {pm.read_u64_calls}")
    check("get(missing) returns None", _rpc.get(GAME_KEY, "Unknown") is None)


# ── 3. persistence roundtrip ──────────────────────────────────────────────────

def test_set_then_get_in_memory():
    _rpc.set(GAME_KEY, "Z", addr=0x1234_0000, klass_addr=0xABCD_0000)
    check("get() returns set addr",
          _rpc.get(GAME_KEY, "Z") == 0x1234_0000)
    entry = _rpc.get_entry(GAME_KEY, "Z")
    check("klass_addr preserved in entry",
          bool(entry) and entry["klass_addr"] == 0xABCD_0000)


def test_persist_then_reload_roundtrip():
    _rpc.set(GAME_KEY, "Persistent", addr=0x5555_0000, klass_addr=0x6666_0000,
             klass_name="Persistent", extras={"hint_region": 0x7})
    persisted_ok = _rpc.persist(GAME_KEY)
    check("persist() returns True", persisted_ok is True)
    _rpc.reload()             # drop in-memory mirror
    entry = _rpc.get_entry(GAME_KEY, "Persistent")
    check("after reload+re-read, addr restored",
          bool(entry) and entry["addr"] == 0x5555_0000)
    check("klass_addr restored after reload",
          bool(entry) and entry["klass_addr"] == 0x6666_0000)
    check("extras payload restored after reload",
          bool(entry) and entry["extras"] == {"hint_region": 0x7})


def test_invalid_addr_rejected():
    _rpc.set(GAME_KEY, "Garbage", addr=0, klass_addr=0x10)
    check("addr=0 not cached", _rpc.get(GAME_KEY, "Garbage") is None)
    _rpc.set(GAME_KEY, "Negative", addr=-1, klass_addr=0x10)
    check("negative addr not cached", _rpc.get(GAME_KEY, "Negative") is None)


# ── 4. invalidation ───────────────────────────────────────────────────────────

def test_invalidate_drops_entry():
    _rpc.set(GAME_KEY, "Doomed", addr=0x1, klass_addr=0x2)
    check("entry present before invalidate", _rpc.get(GAME_KEY, "Doomed") == 0x1)
    _rpc.invalidate(GAME_KEY, "Doomed")
    check("entry gone after invalidate", _rpc.get(GAME_KEY, "Doomed") is None)
    pm = CountingPM({0x1: 0x2})
    check("validate fails after invalidate", _rpc.validate(GAME_KEY, "Doomed", pm) is False)


def test_invalidate_all_clears_every_entry():
    _rpc.set(GAME_KEY, "A", addr=0x10, klass_addr=0x20)
    _rpc.set(GAME_KEY, "B", addr=0x30, klass_addr=0x40)
    _rpc.invalidate_all(GAME_KEY)
    check("A dropped by invalidate_all", _rpc.get(GAME_KEY, "A") is None)
    check("B dropped by invalidate_all", _rpc.get(GAME_KEY, "B") is None)


def test_invalidate_unknown_is_noop():
    try:
        _rpc.invalidate(GAME_KEY, "Nonexistent")
        _rpc.invalidate_all("not-a-game-key")
        check("invalidate on unknown name/key does not raise", True)
    except Exception as exc:
        check("invalidate on unknown must not raise", False, str(exc))


# ── 5. corrupted / mismatched disk cache is ignored ───────────────────────────

def test_mismatched_game_key_on_disk_is_ignored():
    # A cache file whose stored game_key != requested key must NOT be loaded —
    # this is the auto-invalidation guarantee on a game-patch.
    stale_path = os.path.join(_rpc._CACHE_DIR, f"root_ptrs_{GAME_KEY}.json")
    with open(stale_path, "w", encoding="utf-8") as f:
        json.dump({"game_key": "different", "version": 1,
                   "entries": {"Stale": {"addr": 0xDEAD, "klass_addr": 0xBEEF}}},
                  f)
    _rpc.reload()
    check("cross-key file payload not loaded",
          _rpc.get(GAME_KEY, "Stale") is None)


def test_malformed_json_is_ignored():
    stale_path = os.path.join(_rpc._CACHE_DIR, f"root_ptrs_{GAME_KEY}.json")
    with open(stale_path, "w", encoding="utf-8") as f:
        f.write("{not valid json")
    _rpc.reload()
    check("malformed JSON not crashing", _rpc.get(GAME_KEY, "Anything") is None)


# ── 6. diagnostics + flush_all ────────────────────────────────────────────────

def test_coverage_reports_counts_and_dirty_flag():
    check("coverage reports 0 entries at start",
          _rpc.coverage(GAME_KEY)["entries"] == 0)
    _rpc.set(GAME_KEY, "X", addr=0x1000, klass_addr=0x2000)
    cov = _rpc.coverage(GAME_KEY)
    check("coverage reports 1 entry after set", cov["entries"] == 1)
    check("coverage names list", cov["names"] == ["X"])
    check("coverage shows dirty after set", cov["dirty"] is True)
    _rpc.persist(GAME_KEY)
    check("coverage no longer dirty after persist",
          _rpc.coverage(GAME_KEY)["dirty"] is False)


def test_flush_all_writes_all_dirty_keys():
    other = "cafe" * 8
    _rpc.set(GAME_KEY, "A", addr=0x1, klass_addr=0x2)
    _rpc.set(other, "B", addr=0x3, klass_addr=0x4)
    _rpc.flush_all()
    f1 = os.path.isfile(os.path.join(_rpc._CACHE_DIR, f"root_ptrs_{GAME_KEY}.json"))
    f2 = os.path.isfile(os.path.join(_rpc._CACHE_DIR, f"root_ptrs_{other}.json"))
    check("flush_all persists first game_key", f1)
    check("flush_all persists other game_key", f2)


# ── runner ────────────────────────────────────────────────────────────────────

def main() -> int:
    tmp = setup_fresh()
    try:
        for fn in [
            test_validate_match_is_one_read,
            test_validate_mismatch_is_one_read,
            test_missing_entry_is_zero_reads,
            test_set_then_get_in_memory,
            test_persist_then_reload_roundtrip,
            test_invalid_addr_rejected,
            test_invalidate_drops_entry,
            test_invalidate_all_clears_every_entry,
            test_invalidate_unknown_is_noop,
            test_mismatched_game_key_on_disk_is_ignored,
            test_malformed_json_is_ignored,
            test_coverage_reports_counts_and_dirty_flag,
            test_flush_all_writes_all_dirty_keys,
        ]:
            fn()
    finally:
        teardown(tmp)

    n_pass = sum(1 for _, ok in _RESULTS if ok)
    n_fail = len(_RESULTS) - n_pass
    print()
    print(f"[root_pointer_cache] {n_pass} passed, {n_fail} failed")
    return 0 if n_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
