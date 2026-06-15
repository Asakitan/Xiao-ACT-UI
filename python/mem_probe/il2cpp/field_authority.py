# -*- coding: utf-8 -*-
"""field_authority - per-component source-of-truth model for the smart hybrid.

Until Phase 5, PacketBridge read a single boolean ``mem_authoritative`` set by
MemStateBridge: when True the *entire* self-state (hp, level, skills, identity)
came from memory; when False TCP owned all of it. That single bit is the source
of the "one field bad → whole subsystem flips to TCP" failure mode Phase 5 fixes.

This module defines:

  - ``Source``: enum {tcp, memory, either}. Per-component source-of-truth.
  - ``ProbeReason``: enum classifying WHY a mem probe failed (anchor_invalid /
    process_missing / scan_in_progress / snapshot_none / layout_drift). Each
    reason maps to a different backoff policy so failure handling is precise
    instead of "decrement count, flip at 10".
  - ``FieldAuthority``: the live ``{component: (source, confidence, reason,
    last_updated_tick)}`` map. Components extend the existing DATA_SOURCE_COMPONENTS
    set (hp/level/stamina/skills/identity) with the things memory can also own
    (boss/scene/entity/name/profession).
  - ``BackoffPolicy``: per-reason backoff + re-probe cadence. Decoupled from
    MemSelfStateProvider so the same policy can be reused by all readers.

Back-compat shim: ``FieldAuthority.as_authoritative_bool()`` collapses back to the
single-bool signal so the change is non-breaking — flipping
``mem_per_field_authority = False`` in config restores today's behavior exactly.

Authoritative sources are independent of why the last probe failed: a field can
be ``Source.memory`` (it IS what the reader produces) while a *different* field
is in ``Source.tcp`` (its reader is in backoff). On failure we degrade ONE field
to tcp and schedule a re-probe; we do NOT flip the whole subsystem.
"""
from __future__ import annotations

import enum
import threading
import time
from dataclasses import dataclass, field
from typing import Dict, Optional


class Source(str, enum.Enum):
    TCP = "tcp"
    MEMORY = "memory"
    EITHER = "either"          # write only if no source has written recently


class ProbeReason(str, enum.Enum):
    """Classifies why a memory probe failed. Drives backoff policy."""
    OK = "ok"                              # success / not failed
    ANCHOR_INVALID = "anchor_invalid"      # cached singleton pointer went stale
    PROCESS_MISSING = "process_missing"    # Star.exe not running
    SCAN_IN_PROGRESS = "scan_in_progress"  # a bootstrap scan is mid-flight
    SNAPSHOT_NONE = "snapshot_none"        # scan done but no result
    LAYOUT_DRIFT = "layout_drift"          # field sanity failed (HP < 0 etc.)


# Per-reason policy. The key insight of Phase 5: NOT every failure should ramp
# backoff. ``scan_in_progress`` must NOT count as failure (long Stage-A scans can
# otherwise trip a global flip), and ``process_missing`` should NOT touch TCP at
# all (Star.exe is just gone, fallback should wait until it returns).
#
#   reason            →   counts?   initial_s   cap_s   action
#   ───────────────────────────────────────────────────────────────────
#   ANCHOR_INVALID        yes       1           30      relocate once, ramp on miss
#   PROCESS_MISSING       no        —           —       pause, no TCP flip
#   SCAN_IN_PROGRESS      no        —           —       NOT counted (latent bug)
#   SNAPSHOT_NONE         yes       2           30      single re-probe, ramp
#   LAYOUT_DRIFT          yes       1           30      relocate + per-field degrade
#
@dataclass
class BackoffState:
    consecutive_fails: int = 0
    next_probe_at: float = 0.0      # absolute monotonic time the next re-probe may run
    initial_s: float = 1.0
    cap_s: float = 30.0
    last_reason: ProbeReason = ProbeReason.OK

    def schedule(self, *, now: float) -> float:
        """Compute this round's backoff delay and stamp ``next_probe_at``.

        Exponential schedule: initial_s, 2×, 4×, ..., capped at ``cap_s``. Returns
        the delay in seconds (also stored as next_probe_at for ``should_retry``).
        When ``initial_s == 0`` (process_missing / scan_in_progress reasons) the
        delay is zero so the field can re-probe on the next tick."""
        if self.initial_s <= 0:
            delay = 0.0
        else:
            delay = min(self.initial_s * (2 ** max(0, self.consecutive_fails - 1)),
                        self.cap_s)
            delay = max(delay, 0.0)
        self.next_probe_at = now + delay
        return delay

    def should_retry(self, *, now: float) -> bool:
        return now >= self.next_probe_at

    def reset(self) -> None:
        self.consecutive_fails = 0
        self.next_probe_at = 0.0
        self.last_reason = ProbeReason.OK


# Per-reason policy knobs (cap_s and initial_s). Adjusted at runtime by the
# config keys ``mem_failure_backoff_max_s`` / per-reason. Conservative defaults
# keep production stable on the first boot of any change.
_POLICY_DEFAULTS: Dict[ProbeReason, Dict[str, float]] = {
    ProbeReason.ANCHOR_INVALID:  {"initial_s": 1.0, "cap_s": 30.0},
    ProbeReason.PROCESS_MISSING: {"initial_s": 0.0, "cap_s": 0.0},   # paused, no ramp
    ProbeReason.SCAN_IN_PROGRESS: {"initial_s": 0.0, "cap_s": 0.0},  # NOT counted
    ProbeReason.SNAPSHOT_NONE:   {"initial_s": 2.0, "cap_s": 30.0},
    ProbeReason.LAYOUT_DRIFT:    {"initial_s": 1.0, "cap_s": 30.0},
    ProbeReason.OK:              {"initial_s": 0.0, "cap_s": 0.0},
}


def make_policy(reason: ProbeReason, *,
                cap_override: Optional[float] = None) -> BackoffState:
    """Construct a BackoffState for a specific reason using the policy table."""
    cfg = _POLICY_DEFAULTS.get(reason, _POLICY_DEFAULTS[ProbeReason.SNAPSHOT_NONE])
    cap = cap_override if cap_override is not None else cfg["cap_s"]
    return BackoffState(initial_s=cfg["initial_s"], cap_s=cap,
                        last_reason=reason)


def counts_as_failure(reason: ProbeReason) -> bool:
    """``True`` iff this reason should increment a per-field consecutive-fail
    counter. SCAN_IN_PROGRESS and PROCESS_MISSING do NOT (the former is mid-scan,
    the latter is the game gone — neither implies the reader is broken)."""
    return reason in (ProbeReason.ANCHOR_INVALID, ProbeReason.SNAPSHOT_NONE,
                      ProbeReason.LAYOUT_DRIFT)


# ── FieldAuthority ──────────────────────────────────────────────────────────
# Components beyond DATA_SOURCE_COMPONENTS the memory readers can also own.
EXTENDED_COMPONENTS = ("boss", "scene", "entity", "name", "profession")


@dataclass
class _FieldState:
    source: Source = Source.TCP
    confidence: float = 0.0           # 0..1; readers may report data-plausibility
    last_updated_tick: float = 0.0
    last_reason: ProbeReason = ProbeReason.OK
    backoff: BackoffState = field(default_factory=BackoffState)


class FieldAuthority:
    """Thread-safe per-component source-of-truth + per-reason backoff.

    Lifecycle:
      - Reader succeeds → ``record_success(component, source=memory, confidence)``,
        which clears backoff, sets source to memory, last_updated = now.
      - Reader fails   → ``record_failure(component, reason)``, which:
          - if reason counts → increments consecutive_fails, schedules next retry;
          - if reason doesn't count (scan_in_progress / process_missing) leaves
            the source untouched (memory reader hasn't been proven broken);
          - the *field* degrades to Source.TCP only when consecutive_fails passes
            a threshold (default 3 counted failures, NOT 10 as the old constant).
      - Degraded field with Source.TCP → ``try_resync(component)`` may flip it
        back to memory on the next reader success.
    """

    DEFAULT_THRESHOLD = 3          # counted fails before degrade (was 10)

    def __init__(self, *,
                 fail_threshold: int = DEFAULT_THRESHOLD,
                 backoff_cap_s: float = 30.0):
        self._lock = threading.RLock()
        self._state: Dict[str, _FieldState] = {}
        self.fail_threshold = int(fail_threshold)
        self.backoff_cap_s = float(backoff_cap_s)

    # ── read API (called by publishers to decide if they should write) ──────
    def source(self, component: str) -> Source:
        with self._lock:
            st = self._state.get(component)
            return st.source if st else Source.TCP

    def confidence(self, component: str) -> float:
        with self._lock:
            st = self._state.get(component)
            return st.confidence if st else 0.0

    def report(self) -> Dict[str, Dict[str, object]]:
        """Snapshot for health(): per-component {source, confidence, reason, ago_s}."""
        now = time.monotonic()
        with self._lock:
            out: Dict[str, Dict[str, object]] = {}
            for comp, st in self._state.items():
                out[comp] = {
                    "source": st.source.value,
                    "confidence": round(st.confidence, 3),
                    "reason": st.last_reason.value,
                    "ago_s": round(now - st.last_updated_tick, 3) if st.last_updated_tick else None,
                    "consecutive_fails": st.backoff.consecutive_fails,
                }
            return out

    # ── write API (called by readers) ───────────────────────────────────────
    def record_success(self, component: str, *,
                       source: Source = Source.MEMORY,
                       confidence: float = 1.0) -> None:
        with self._lock:
            st = self._state.setdefault(component, _FieldState())
            st.source = source
            st.confidence = max(0.0, min(1.0, float(confidence)))
            st.last_updated_tick = time.monotonic()
            st.last_reason = ProbeReason.OK
            st.backoff.reset()

    def record_failure(self, component: str, reason: ProbeReason) -> bool:
        """Record a probe failure for one component.

        Returns ``True`` iff this failure SHOULD DEGRADE the field to TCP (i.e.
        we've crossed the threshold). The caller is then expected to flip the
        publish-side source for that field; nothing here touches publishers.
        """
        with self._lock:
            st = self._state.setdefault(component, _FieldState())
            st.last_reason = reason
            st.last_updated_tick = time.monotonic()
            if not counts_as_failure(reason):
                # process_missing / scan_in_progress don't reflect the reader being
                # broken; leave its source untouched (was latent bug before Phase 5).
                return False
            # Apply the per-reason policy to this field's backoff state. The state
            # object is persistent so consecutive_fails survives across calls, but
            # initial/cap need to follow the current failure reason and the runtime
            # cap override (mem_failure_backoff_max_s / tests with a tiny cap).
            if st.backoff.last_reason != reason:
                st.backoff.consecutive_fails = 0
            cfg = _POLICY_DEFAULTS.get(reason, _POLICY_DEFAULTS[ProbeReason.SNAPSHOT_NONE])
            st.backoff.initial_s = float(cfg["initial_s"])
            st.backoff.cap_s = float(self.backoff_cap_s)
            st.backoff.consecutive_fails += 1
            st.backoff.last_reason = reason
            st.backoff.schedule(now=time.monotonic())
            if st.backoff.consecutive_fails >= self.fail_threshold:
                st.source = Source.TCP
                st.confidence = 0.0
                return True
            return False

    def should_retry(self, component: str, *, now: Optional[float] = None) -> bool:
        """Has the per-component backoff elapsed for a degraded field?"""
        t = float(now if now is not None else time.monotonic())
        with self._lock:
            st = self._state.get(component)
            if st is None:
                return True
            return st.backoff.should_retry(now=t)

    # ── back-compat ─────────────────────────────────────────────────────────
    def as_authoritative_bool(self, components=("hp", "level", "stamina", "skills",
                                                "identity")) -> bool:
        """Collapse to the legacy single-bool signal. ``True`` iff ALL the
        self-state components are currently Source.MEMORY. This lets existing
        callers (PacketBridge.set_mem_authoritative) keep working unchanged when
        ``mem_per_field_authority = False``."""

        with self._lock:
            return all(
                self._state.get(c) is not None and
                self._state[c].source == Source.MEMORY
                for c in components
            )

    def snapshot(self) -> Dict[str, _FieldState]:
        with self._lock:
            return dict(self._state)


__all__ = [
    "Source", "ProbeReason", "BackoffState", "FieldAuthority",
    "EXTENDED_COMPONENTS", "make_policy", "counts_as_failure",
]
