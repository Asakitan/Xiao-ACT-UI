# -*- coding: utf-8 -*-
"""Phase 5 contract test for FieldAuthority + PacketBridge publish gating.

Validates:
  - Per-component source granularity: one field in TCP doesn't force another
    field out of MEMORY.
  - Classified-failure backoff: SCAN_IN_PROGRESS and PROCESS_MISSING do NOT
    count as failures (the latent bug Phase 5 fixes).
  - Threshold-based degrade (3 counted fails), exponential backoff schedule.
  - Back-compat shim: set_field_authority(None) restores the legacy single-bool
    behavior exactly — no test breakage.

Run::

    python -m mem_probe.il2cpp.test_field_authority
"""
from __future__ import annotations

import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _SAO not in sys.path:
    sys.path.insert(0, _SAO)

from plugins.star_resonance_plugin.mem.il2cpp.field_authority import (
    FieldAuthority, Source, ProbeReason, counts_as_failure,
)


_RESULTS: list = []


def check(name: str, cond: bool, detail: str = "") -> None:
    status = "PASS" if cond else "FAIL"
    line = f"[{status}] {name}"
    if detail:
        line += f"   {detail}"
    print(line)
    _RESULTS.append((name, bool(cond)))


# ── 1. Per-field granularity (the central Phase 5 contract) ──────────────────

def test_one_field_bad_doesnt_flip_others():
    fa = FieldAuthority()
    fa.record_success('hp', source=Source.MEMORY, confidence=0.9)
    fa.record_success('skills', source=Source.MEMORY, confidence=0.9)
    # HP fails repeatedly; skills should stay memory.
    for _ in range(5):
        fa.record_failure('hp', ProbeReason.ANCHOR_INVALID)
    check("HP degrades to TCP after threshold",
          fa.source('hp') == Source.TCP, f"got {fa.source('hp')}")
    check("Skills stays MEMORY despite HP failure",
          fa.source('skills') == Source.MEMORY,
          f"got {fa.source('skills')}")


# ── 2. SCAN_IN_PROGRESS and PROCESS_MISSING must NOT count as failures ────────

def test_scan_in_progress_does_not_degrade():
    fa = FieldAuthority()
    fa.record_success('hp', source=Source.MEMORY, confidence=1.0)
    # Ten scan-in-progress events in a row — must NOT degrade.
    for _ in range(20):
        degraded = fa.record_failure('hp', ProbeReason.SCAN_IN_PROGRESS)
        check("scan_in_progress never returns degraded=True",
              degraded is False)
    check("source unchanged after 20 scan_in_progress",
          fa.source('hp') == Source.MEMORY)


def test_process_missing_does_not_degrade():
    fa = FieldAuthority()
    fa.record_success('hp', source=Source.MEMORY, confidence=1.0)
    for _ in range(20):
        fa.record_failure('hp', ProbeReason.PROCESS_MISSING)
    check("PROCESS_MISSING keeps source=memory",
          fa.source('hp') == Source.MEMORY)


def test_counts_as_failure_table():
    check("anchor_invalid counts", counts_as_failure(ProbeReason.ANCHOR_INVALID))
    check("snapshot_none counts", counts_as_failure(ProbeReason.SNAPSHOT_NONE))
    check("layout_drift counts", counts_as_failure(ProbeReason.LAYOUT_DRIFT))
    check("scan_in_progress does NOT count",
          not counts_as_failure(ProbeReason.SCAN_IN_PROGRESS))
    check("process_missing does NOT count",
          not counts_as_failure(ProbeReason.PROCESS_MISSING))


# ── 3. Threshold + exponential backoff ────────────────────────────────────────

def test_threshold_default_is_3():
    fa = FieldAuthority()
    check("threshold is 3", fa.fail_threshold == 3)
    fa.record_success('hp', source=Source.MEMORY)
    # 2 fails: NOT yet degraded
    fa.record_failure('hp', ProbeReason.ANCHOR_INVALID)
    degraded_at_2 = fa.record_failure('hp', ProbeReason.ANCHOR_INVALID)
    check("2 fails do not degrade", degraded_at_2 is False)
    # 3rd fail: degraded
    degraded_at_3 = fa.record_failure('hp', ProbeReason.ANCHOR_INVALID)
    check("3rd fail degrades to TCP", degraded_at_3 is True)


def test_backoff_resets_on_success():
    fa = FieldAuthority()
    fa.record_success('hp', source=Source.MEMORY)
    fa.record_failure('hp', ProbeReason.ANCHOR_INVALID)
    fa.record_failure('hp', ProbeReason.ANCHOR_INVALID)
    # Now a success — counter must reset
    fa.record_success('hp', source=Source.MEMORY)
    rep = fa.report()['hp']
    check("consecutive_fails reset on success",
          rep['consecutive_fails'] == 0, str(rep))


def test_should_retry_after_window():
    fa = FieldAuthority(backoff_cap_s=0.05)
    fa.record_success('hp', source=Source.MEMORY)
    fa.record_failure('hp', ProbeReason.ANCHOR_INVALID)
    # immediately after a fail, retry should be False
    immediately = fa.should_retry('hp', now=time.monotonic())
    # wait past the (tiny) backoff window
    time.sleep(0.06)
    later = fa.should_retry('hp', now=time.monotonic())
    check("should_retry False immediately after fail", immediately is False)
    check("should_retry True after backoff window", later is True)


# ── 4. back-compat bool collapse ──────────────────────────────────────────────

def test_legacy_bool_all_memory():
    fa = FieldAuthority()
    for c in ("hp", "level", "stamina", "skills", "identity"):
        fa.record_success(c, source=Source.MEMORY)
    check("legacy bool True when all self-state is memory",
          fa.as_authoritative_bool() is True)


def test_legacy_bool_false_when_any_is_tcp():
    fa = FieldAuthority()
    for c in ("hp", "level", "stamina", "skills"):
        fa.record_success(c, source=Source.MEMORY)
    # identity stays default (TCP)
    check("legacy bool False when identity is TCP",
          fa.as_authoritative_bool() is False)


def test_legacy_bool_custom_components():
    fa = FieldAuthority()
    fa.record_success('boss', source=Source.MEMORY)
    check("custom-components legacy bool",
          fa.as_authoritative_bool(('boss',)) is True)


# ── 5. report shape for health() ──────────────────────────────────────────────

def test_report_shape():
    fa = FieldAuthority()
    fa.record_success('hp', source=Source.MEMORY, confidence=0.75)
    rep = fa.report()
    check("report includes hp", 'hp' in rep)
    hp = rep['hp']
    check("report source field present", hp['source'] == 'memory')
    check("report confidence field", abs(hp['confidence'] - 0.75) < 1e-9)
    check("report reason field", hp['reason'] == 'ok')


# ── 6. PacketBridge back-compat (set_field_authority None = legacy) ───────────
# Note: we test only the gating logic via the helper; full PacketBridge
# instantiation requires the cython packet module and is covered by replay fixtures.

def test_packet_bridge_helper_with_no_authority_falls_back_to_bool():
    """When set_field_authority(None) is called, _component_source_for_publish
    must consult the legacy single bool — no behavior change vs pre-Phase-5."""
    # Build a minimal stub mirroring only the two attributes the helper reads.
    class _BridgeStub:
        _field_authority = None
        _mem_authoritative = True        # legacy bool

    b = _BridgeStub()
    # Bind the real helper to the stub
    from plugins.star_resonance_plugin.net.packet_bridge import PacketBridge
    helper = PacketBridge._component_source_for_publish
    check("legacy bool True => 'memory'",
          helper(b, 'hp') == 'memory')
    b._mem_authoritative = False
    check("legacy bool False => 'tcp'",
          helper(b, 'hp') == 'tcp')


def test_packet_bridge_helper_uses_field_authority_when_present():
    fa = FieldAuthority()
    fa.record_success('hp', source=Source.MEMORY, confidence=0.9)
    fa.record_success('skills', source=Source.TCP)         # reader in backoff

    class _BridgeStub:
        _field_authority = fa
        _mem_authoritative = False                          # ignored when FA set

    from plugins.star_resonance_plugin.net.packet_bridge import PacketBridge
    helper = PacketBridge._component_source_for_publish
    check("FA-set hp => memory", helper(_BridgeStub(), 'hp') == 'memory')
    check("FA-set skills => tcp", helper(_BridgeStub(), 'skills') == 'tcp')


def main() -> int:
    for fn in [
        test_one_field_bad_doesnt_flip_others,
        test_scan_in_progress_does_not_degrade,
        test_process_missing_does_not_degrade,
        test_counts_as_failure_table,
        test_threshold_default_is_3,
        test_backoff_resets_on_success,
        test_should_retry_after_window,
        test_legacy_bool_all_memory,
        test_legacy_bool_false_when_any_is_tcp,
        test_legacy_bool_custom_components,
        test_report_shape,
        test_packet_bridge_helper_with_no_authority_falls_back_to_bool,
        test_packet_bridge_helper_uses_field_authority_when_present,
    ]:
        fn()
    n_pass = sum(1 for _, ok in _RESULTS if ok)
    n_fail = len(_RESULTS) - n_pass
    print()
    print(f"[field_authority] {n_pass} passed, {n_fail} failed")
    return 0 if n_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
