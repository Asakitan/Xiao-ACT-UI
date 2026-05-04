"""TcpSnapshotSource — production wrapper around PacketBridge for SmartLocator.

Differs from tools.mem_probe.auto_locate._TcpSource in two ways:

1. Maintains a sliding-window hp_history (default 1.0s capacity, sampled every
   ~50ms) so callers can ask "did TCP report hp == X within the last 300ms?".
   This compensates for the empirically-measured ~200ms delay between
   memory-side HP changes and TCP-side parsed packets — without this,
   first_run validation would frequently mismatch on a recently-changed value.

2. Records initial UID / max_hp / name as immutable identity once captured;
   used by SmartLocator to persist `known_uid` to anchors.json.

Usage:
    src = TcpSnapshotSource()
    src.start()
    snap = src.wait_ready(timeout=60)        # blocks until first valid snap
    cur = src.snapshot()                     # latest snap (non-blocking)
    if src.hp_in_window(mem_hp, window_s=0.3):
        print("memory HP matches a recent TCP value within 300ms")
    src.stop()
"""
from __future__ import annotations

import os
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Any, Deque, Dict, Optional, Tuple

# Ensure sao_auto/ is on sys.path so we can import packet_bridge / game_state
_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO_AUTO = os.path.dirname(_HERE)
if _SAO_AUTO not in sys.path:
    sys.path.insert(0, _SAO_AUTO)


@dataclass
class TcpSnapshot:
    ts: float
    uid: int
    name: str
    hp: int
    max_hp: int
    in_combat: bool
    packet_active: bool
    # Phase 1+2 extension: ground truth for substruct auto-discovery
    level_base: int = 0          # RoleLevel.Level
    level_extra: int = 0         # AttrChanges 临时 bracket bonus
    season_exp: int = 0          # RoleLevel.AccumulateExp
    profession_id: int = 0       # ProfessionList.CurProfessionId
    profession: str = ""         # derived from profession_id
    fight_point: int = 0         # CharBase.FightPoint
    stamina_current: int = 0     # EnergyItem-based
    stamina_max: int = 0         # EnergyItem.EnergyLimit (+Extra)


class TcpSnapshotSource:
    """Background-sampled wrapper over PacketBridge with sliding HP history."""

    # Sampling cadence — must be << 300ms window so we have multiple samples
    # per measurement window even when HP is changing fast.
    SAMPLE_INTERVAL_S = 0.05
    # History capacity in seconds. 300ms window + 700ms slack for hiccups.
    HISTORY_CAPACITY_S = 1.0
    # Default validation window for hp_in_window (matches the empirically
    # measured TCP-vs-memory lag of ~200ms, plus 100ms slack).
    DEFAULT_HP_WINDOW_S = 0.3

    def __init__(self, *, mode: str = "full") -> None:
        """
        mode:
            'full'        — current behavior, full TCP packet parsing
            'anchor_only' — Phase 9 optimization: PacketBridge in tcp mode
                            but with packet_parser subscribed to a minimal
                            set (SyncContainerData only, for UID anchor)
        """
        from game_state import GameStateManager  # type: ignore
        from packet_bridge import PacketBridge  # type: ignore

        self._mode = str(mode or "full").lower()
        self._state_mgr = GameStateManager()
        # We always use 'tcp' data_source for TcpSnapshotSource (it IS the
        # TCP source by definition). 'anchor_only' is a parser-level filter,
        # applied after start() if available.
        self._bridge = PacketBridge(self._state_mgr, data_source='tcp')
        self._started = False
        self._stop_evt = threading.Event()
        self._sampler: Optional[threading.Thread] = None
        self._lock = threading.Lock()
        # Most recent sample
        self._latest: Optional[TcpSnapshot] = None
        # Sliding window of (ts, hp) for time-based validation
        max_samples = max(8, int(self.HISTORY_CAPACITY_S / self.SAMPLE_INTERVAL_S) * 2)
        self._hp_history: Deque[Tuple[float, int]] = deque(maxlen=max_samples)
        # Permanent record of every distinct hp / max_hp value ever observed.
        # The rolling _hp_history above evicts old samples after 1s, but the
        # FIRST report of a value (e.g. SyncContainerData reporting 483921)
        # may only show up once before being overwritten — value-anchor
        # SELF location relies on having those early-snapshot values around.
        self._all_seen_hp: set = set()
        self._all_seen_max_hp: set = set()
        # Phase 1+2: also track invariants that help substruct auto-discovery
        self._all_seen_level: set = set()
        self._all_seen_profession_id: set = set()
        self._all_seen_fight_point: set = set()
        self._all_seen_stamina_max: set = set()

    # ───────── lifecycle ─────────

    def start(self) -> None:
        if self._started:
            return
        self._bridge.start()
        # Phase 9: in anchor_only mode, restrict the parser to UID-bearing
        # messages only. Saves ~9/10ths of TCP CPU once SmartLocator has the
        # anchor and we just need occasional re-anchoring.
        if self._mode == "anchor_only":
            try:
                parser = getattr(self._bridge, "_parser", None)
                if parser is not None and hasattr(parser, "set_subscribed_messages"):
                    parser.set_subscribed_messages({"SyncContainerData"})
            except Exception:
                pass
        self._stop_evt.clear()
        self._sampler = threading.Thread(
            target=self._sample_loop, name="tcp-snap-src", daemon=True,
        )
        self._sampler.start()
        self._started = True

    def stop(self) -> None:
        self._stop_evt.set()
        if self._sampler is not None:
            self._sampler.join(timeout=1.0)
            self._sampler = None
        try:
            self._bridge.stop()
        except Exception:
            pass
        self._started = False

    def __enter__(self) -> "TcpSnapshotSource":
        self.start()
        return self

    def __exit__(self, *exc) -> None:
        self.stop()

    # ───────── sampling ─────────

    def _sample_loop(self) -> None:
        while not self._stop_evt.is_set():
            try:
                snap = self._read_once()
                if snap is not None:
                    with self._lock:
                        self._latest = snap
                        if snap.hp > 0:
                            self._hp_history.append((snap.ts, snap.hp))
                            self._all_seen_hp.add(snap.hp)
                            self._prune_history(snap.ts)
                        if snap.max_hp > 0:
                            self._all_seen_max_hp.add(snap.max_hp)
                        if snap.level_base > 0:
                            self._all_seen_level.add(snap.level_base)
                        if snap.profession_id > 0:
                            self._all_seen_profession_id.add(snap.profession_id)
                        if snap.fight_point > 0:
                            self._all_seen_fight_point.add(snap.fight_point)
                        if snap.stamina_max > 0:
                            self._all_seen_stamina_max.add(snap.stamina_max)
            except Exception:
                # Silent: sampler must never die. State manager attribute
                # access can race during PacketBridge shutdown.
                pass
            self._stop_evt.wait(self.SAMPLE_INTERVAL_S)

    def _read_once(self) -> Optional[TcpSnapshot]:
        st = self._state_mgr.state
        parser = getattr(self._bridge, "_parser", None)
        uid = 0
        if parser is not None:
            try:
                uid = int(getattr(parser, "_current_uid", 0) or 0)
            except Exception:
                uid = 0
        return TcpSnapshot(
            ts=time.time(),
            uid=uid,
            name=str(getattr(st, "player_name", "") or ""),
            hp=int(getattr(st, "hp_current", 0) or 0),
            max_hp=int(getattr(st, "hp_max", 0) or 0),
            in_combat=bool(getattr(st, "in_combat", False)),
            packet_active=bool(getattr(st, "packet_active", False)),
            level_base=int(getattr(st, "level_base", 0) or 0),
            level_extra=int(getattr(st, "level_extra", 0) or 0),
            season_exp=int(getattr(st, "season_exp", 0) or 0),
            profession_id=int(getattr(st, "profession_id", 0) or 0),
            profession=str(getattr(st, "profession_name", "") or ""),
            fight_point=int(getattr(st, "fight_point", 0) or 0),
            stamina_current=int(getattr(st, "stamina_current", 0) or 0),
            stamina_max=int(getattr(st, "stamina_max", 0) or 0),
        )

    def _prune_history(self, now: float) -> None:
        """Drop samples older than HISTORY_CAPACITY_S. Caller holds _lock."""
        cutoff = now - self.HISTORY_CAPACITY_S
        while self._hp_history and self._hp_history[0][0] < cutoff:
            self._hp_history.popleft()

    # ───────── public queries ─────────

    def snapshot(self) -> Dict[str, Any]:
        """Return latest snapshot as dict (compat with old _TcpSource API)."""
        with self._lock:
            s = self._latest
        if s is None:
            return {
                "uid": 0, "name": "", "hp": 0, "max_hp": 0,
                "in_combat": False, "packet_active": False, "ts": 0.0,
            }
        return {
            "uid": s.uid, "name": s.name, "hp": s.hp, "max_hp": s.max_hp,
            "in_combat": s.in_combat, "packet_active": s.packet_active,
            "ts": s.ts,
        }

    def latest_snap(self) -> Optional[TcpSnapshot]:
        """Latest snapshot as TcpSnapshot dataclass; None if not yet ready."""
        with self._lock:
            return self._latest

    def wait_ready(self, *, timeout: float = 60.0,
                   interval: float = 0.5) -> Dict[str, Any]:
        """Block until uid > 0 and hp > 0 (or raise on timeout)."""
        if not self._started:
            raise RuntimeError("TcpSnapshotSource.start() must be called first")
        deadline = time.time() + timeout
        last_print = 0.0
        while time.time() < deadline:
            snap = self.snapshot()
            if snap["uid"] and snap["hp"] > 0:
                return snap
            now = time.time()
            if now - last_print > 2.0:
                last_print = now
                print(
                    f"[tcp-src] waiting: packet_active={snap['packet_active']} "
                    f"uid={snap['uid']} hp={snap['hp']}/{snap['max_hp']} "
                    f"name={snap['name']!r}"
                )
            time.sleep(interval)
        raise RuntimeError(
            f"TcpSnapshotSource.wait_ready timed out after {timeout}s; "
            f"last={self.snapshot()}"
        )

    def hp_in_window(self, value: int, *,
                     window_s: float = DEFAULT_HP_WINDOW_S) -> bool:
        """True iff TCP reported hp == value at any point in last window_s.

        This is the core validation primitive for SmartLocator first_run:
        memory HP changes ~200ms before TCP reflects them, so a strict
        equality check at one instant frequently mismatches even when both
        sides see the SAME underlying state. The sliding window absorbs
        that lag.
        """
        cutoff = time.time() - window_s
        with self._lock:
            for ts, hp in self._hp_history:
                if ts >= cutoff and hp == value:
                    return True
        return False

    def history_summary(self, *, window_s: float = DEFAULT_HP_WINDOW_S) -> dict:
        """For diagnostics: distinct HP values seen in the last window_s."""
        cutoff = time.time() - window_s
        with self._lock:
            samples = [(ts, hp) for ts, hp in self._hp_history if ts >= cutoff]
        if not samples:
            return {"window_s": window_s, "count": 0, "values": []}
        values = sorted({hp for _, hp in samples})
        return {
            "window_s": window_s,
            "count": len(samples),
            "values": values,
            "span_ms": int((samples[-1][0] - samples[0][0]) * 1000),
        }

    def all_seen_hp(self) -> set:
        """Every distinct cur_hp value observed since start() (no time limit).

        Useful when later TCP packets overwrite the SELF hp field with a
        recomputed value (effective vs base) — the early snapshot's value
        survives here even after _hp_history evicts it.
        """
        with self._lock:
            return set(self._all_seen_hp)

    def all_seen_max_hp(self) -> set:
        """Every distinct max_hp value observed since start() (no time limit)."""
        with self._lock:
            return set(self._all_seen_max_hp)

    def all_seen_level(self) -> set:
        with self._lock:
            return set(self._all_seen_level)

    def all_seen_profession_id(self) -> set:
        with self._lock:
            return set(self._all_seen_profession_id)

    def all_seen_fight_point(self) -> set:
        with self._lock:
            return set(self._all_seen_fight_point)

    def all_seen_stamina_max(self) -> set:
        with self._lock:
            return set(self._all_seen_stamina_max)

    def stable_ground_truth(self) -> dict:
        """Return the most-likely "stable" values for substruct discovery.

        Heuristic: take the FIRST seen value (login snapshot is most reliable
        before any buff/gear shifts). For level/profession/fight_point this
        is the strongest invariant.
        """
        def first(s: set) -> int:
            return min(s) if s else 0
        with self._lock:
            return {
                "level_base": first(self._all_seen_level),
                "profession_id": first(self._all_seen_profession_id),
                "fight_point": first(self._all_seen_fight_point),
                "stamina_max": first(self._all_seen_stamina_max),
            }
