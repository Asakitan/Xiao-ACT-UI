# -*- coding: utf-8 -*-
"""Selftest for mem_probe.mem_access — gating, JSON-safety, async search lifecycle.

Runs entirely against stub owners / a synthetic StarProcess; no game required.

    python tools/mem_access_selftest.py
"""
from __future__ import annotations

import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from mem_probe.mem_access import MemAccess  # noqa: E402

_passed = 0
_failed = 0


def check(name: str, cond: bool, detail: str = "") -> None:
    global _passed, _failed
    if cond:
        _passed += 1
        print(f"  [ok] {name}")
    else:
        _failed += 1
        print(f"  [FAIL] {name} :: {detail}")


# ── stubs ─────────────────────────────────────────────────────────────────────

class _Region:
    def __init__(self, base, size):
        self.base = base
        self.size = size


class _FakePm:
    """Minimal StarProcess stand-in over one in-memory buffer."""

    def __init__(self, base=0x100000, buf=b""):
        self._base = base
        self._buf = buf
        self.name = "fake.exe"
        self.pid = 4321

    def iter_regions(self, *, only_readable=True, only_private=True):
        yield _Region(self._base, len(self._buf))

    def read_bytes(self, addr, n):
        off = addr - self._base
        if off < 0 or off + n > len(self._buf):
            return None
        return self._buf[off:off + n]

    def _ri(self, addr, n, signed):
        b = self.read_bytes(addr, n)
        return int.from_bytes(b, "little", signed=signed) if b else None

    def read_u32(self, addr):
        return self._ri(addr, 4, False)

    def read_i32(self, addr):
        return self._ri(addr, 4, True)

    def read_u64(self, addr):
        return self._ri(addr, 8, False)

    def read_i64(self, addr):
        return self._ri(addr, 8, True)

    def read_ptr(self, addr):
        return self.read_u64(addr)

    def read_f32(self, addr):
        import struct
        b = self.read_bytes(addr, 4)
        return struct.unpack("<f", b)[0] if b else None

    def read_utf16(self, addr, max_chars=64):
        return None

    def read_cstr(self, addr, max_len=256):
        return None

    def list_modules(self):
        return []


class _EP:
    def __init__(self, pm):
        self._pm = pm
        self._ecr = None


class _FakeBridge:
    def __init__(self, pm):
        self._entity_provider = _EP(pm)
        self._damage_reader = None
        self._provider = None
        self.mode = "memory"
        self.last_error = ""
        self.last_uid = 0
        self.last_hp = 1000
        self.last_max_hp = 2000
        self.last_profession_id = 5
        self.last_char_name = "Kirito"
        self.last_skill_cd_count = 3
        self.last_resources = {}
        self.last_is_dead = False
        self.last_entities = []
        self.last_boss_mem = None
        self.last_mem_damage = {}

    def snapshot(self):
        return None

    def _name_resolver(self):
        return None


def _hybrid_owner(bridge):
    ms = type("MS", (), {"_bridge": bridge})()
    pe = type("PE", (), {"_mem_source": ms})()
    o = type("Owner", (), {})()
    o._packet_engine = pe
    o._mem_bridge = None
    o._get_mem_data_source = lambda: "hybrid"
    return o


def _tcp_owner():
    o = type("Owner", (), {})()
    o._packet_engine = type("PE", (), {"_mem_source": None})()
    o._mem_bridge = None
    o._get_mem_data_source = lambda: "tcp"
    return o


# ── tests ─────────────────────────────────────────────────────────────────────

def test_tcp_gate():
    print("[tcp gate]")
    ma = MemAccess(_tcp_owner())
    check("self_state gated", ma.self_state().get("reason") == "mode_tcp")
    check("entities gated", ma.entities().get("reason") == "mode_tcp")
    check("search gated", ma.search(1, "i32").get("reason") == "mode_tcp")
    check("status not gated", ma.status().get("ok") is True and ma.status().get("active") is False)
    check("catalog not gated", len(ma.catalog().get("categories") or []) == 10)


def test_no_bridge():
    print("[hybrid, no bridge]")
    o = type("Owner", (), {})()
    o._packet_engine = type("PE", (), {"_mem_source": None})()
    o._mem_bridge = None
    o._get_mem_data_source = lambda: "hybrid"
    ma = MemAccess(o)
    check("self_state no_bridge", ma.self_state().get("reason") == "no_bridge")


def test_json_safety():
    print("[json safety]")
    pm = _FakePm()
    bridge = _FakeBridge(pm)
    big_uuid = 1 << 60
    bridge.last_entities = [{
        "uuid": big_uuid, "config_uuid": 0, "base_id": 7, "kind": "boss",
        "cur_hp": 100, "max_hp": 200, "hp_pct": 0.5, "obj": 0x7FF600001234,
        "breaking_stage": 1, "overdrive": None, "stun": None, "cast_skill_id": 0,
        "name": "Boss",
    }]
    ma = MemAccess(_hybrid_owner(bridge))
    row = ma.entities()["entities"][0]
    check("uuid stringified", isinstance(row["uuid"], str) and row["uuid"] == str(big_uuid))
    check("obj is hex string", isinstance(row["obj"], str) and row["obj"].startswith("0x"))
    ss = ma.self_state()
    check("self uid string", isinstance(ss["uid"], str))
    check("self hp int", ss["hp"] == 1000 and ss["max_hp"] == 2000)


def test_search_lifecycle():
    print("[search lifecycle]")
    # buffer with u32 value 12345 (0x3039) at aligned offsets 0 and 16
    needle = (12345).to_bytes(4, "little")
    buf = bytearray(64)
    buf[0:4] = needle
    buf[16:20] = needle
    pm = _FakePm(base=0x200000, buf=bytes(buf))
    bridge = _FakeBridge(pm)
    ma = MemAccess(_hybrid_owner(bridge))

    res = ma.search("12345", "u32")
    check("search started", res.get("ok") and res.get("job_id"))
    job = res.get("job_id")

    done = None
    for _ in range(60):
        st = ma.search_status(job)
        if st.get("done"):
            done = st
            break
        time.sleep(0.05)
    check("search completed", done is not None and done.get("state") == "done", repr(done))
    if done:
        check("found 2 hits", done.get("count") == 2, repr(done.get("count")))
        results = done.get("results") or []
        check("hits carry hex addr", bool(results) and results[0]["addr"].startswith("0x"))
        check("hit decodes u32=12345", bool(results) and results[0]["as"].get("u32") == 12345,
              repr(results[0].get("as") if results else None))

    # narrow to the same value keeps both hits
    nres = ma.narrow(job, "12345")
    check("narrow started", nres.get("ok"))
    ndone = None
    for _ in range(40):
        st = ma.search_status(job)
        if st.get("done"):
            ndone = st
            break
        time.sleep(0.05)
    check("narrow kept 2", bool(ndone) and ndone.get("count") == 2, repr(ndone.get("count") if ndone else None))

    # cancel + list
    check("search_list ok", ma.search_list().get("ok") is True)
    check("cancel unknown not_found", ma.search_cancel("nope").get("reason") == "not_found")


def main():
    test_tcp_gate()
    test_no_bridge()
    test_json_safety()
    test_search_lifecycle()
    print(f"\n{_passed} passed, {_failed} failed")
    return 1 if _failed else 0


if __name__ == "__main__":
    sys.exit(main())
