"""Deterministic tests for mem_state_anchor candidate validation."""
from __future__ import annotations

import os
import sys

_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from mem_probe.il2cpp.mem_state_anchor import AnchorMemoryReader, AnchorPack


class FakeAnchorReader(AnchorMemoryReader):
    def __init__(self, values: dict[int, object]):
        # Do not open StarProcess for deterministic unit tests.
        self.values = dict(values)
        self._cached_regions = []
        self._cache_ts = 0.0
        self._regen_ttl = 5.0
        self._scan_scratch = None
        self._resolved_cache = None
        self._resolved_cache_uid = 0
        self._resolved_cache_ts = 0.0
        self.max_scan_regions_mb = 0
        self.last_region_scan_bytes = 0
        self.last_region_scan_limited = False
        # Populate the proto field-offset layout maps (CHAR_SERIALIZE etc.) from the
        # literals — no live process / dump needed (offset(None, ...) -> literal).
        self._init_layout(None)

    def _read_i64(self, addr: int):
        return self.values.get(addr)

    def _read_u64(self, addr: int):
        return self.values.get(addr)

    def _read_i32(self, addr: int):
        return self.values.get(addr)

    def _read_u32(self, addr: int):
        return self.values.get(addr)

    def _read_f32(self, addr: int):
        return self.values.get(addr)

    def _read_il2cpp_string(self, str_field_addr: int):
        return self.values.get(str_field_addr)


UID = 36668136
CS = 0x1578CB8540
ATTR = 0x15C1579D80
CB = 0x1578001000
RL = 0x1578002000
PL = 0x1578003000
EI = 0x1578004000


def _base_values(overrides=None):
    values = {
        CS + 0x10: UID,
        CS + 0x88: ATTR,
        ATTR + 0x10: 244207,
        ATTR + 0x18: 244207,
        ATTR + 0x38: 0,
        CS + 0x18: CB,
        CB + 0x10: UID,
        CB + 0x30: "咲",
        CB + 0xC8: 5,
        CS + 0x70: EI,
        CS + 0xB8: RL,
        RL + 0x10: 60,
        CS + 0x1F8: PL,
        PL + 0x10: 5,
    }
    values.update(overrides or {})
    return values


def test_uid_only_candidate_accepts_plausible_self_graph() -> None:
    reader = FakeAnchorReader(_base_values())

    ok, info = reader._validate_self(CS, AnchorPack(uid=UID))

    assert ok
    assert info["ufa"] == ATTR
    assert info["cb"] == CB
    assert info["rl"] == RL
    assert info["pl"] == PL
    assert info["confidence"] >= 0.7
    assert "plausible=" in info["used"]


def test_uid_only_candidate_rejects_garbage_hp_pair() -> None:
    reader = FakeAnchorReader(_base_values({
        ATTR + 0x10: 88133910528,
        ATTR + 0x18: 14,
    }))

    ok, _info = reader._validate_self(CS, AnchorPack(uid=UID))

    assert not ok


def test_uid_only_candidate_rejects_sparse_false_positive() -> None:
    reader = FakeAnchorReader({
        CS + 0x10: UID,
        CS + 0x88: ATTR,
        ATTR + 0x10: 1000,
        ATTR + 0x18: 2000,
        ATTR + 0x38: 0,
    })

    ok, _info = reader._validate_self(CS, AnchorPack(uid=UID))

    assert not ok


def test_strong_level_anchor_rejects_level_mismatch() -> None:
    reader = FakeAnchorReader(_base_values())

    ok, _info = reader._validate_self(CS, AnchorPack(uid=UID, level=61))

    assert not ok


if __name__ == "__main__":
    tests = [
        test_uid_only_candidate_accepts_plausible_self_graph,
        test_uid_only_candidate_rejects_garbage_hp_pair,
        test_uid_only_candidate_rejects_sparse_false_positive,
        test_strong_level_anchor_rejects_level_mismatch,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
