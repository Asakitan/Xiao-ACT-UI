"""CLI: discover MonsterEntity klass + HP/MaxHP offsets via TCP truth + Cython.

Usage:
    python -m mem_probe.discover_entities
    python -m mem_probe.discover_entities --min-monsters 3 --timeout 120

Workflow:
  1. Starts a TCP PacketBridge with on_monster_update callback.
  2. User enters combat in-game; SyncNearEntities packets carry (uuid, hp, max_hp).
  3. Once we collected ≥ MIN_MONSTERS distinct UUIDs (default 2), the Cython
     value-anchor algorithm in entity_discovery runs.
  4. On success, anchors.json is updated with the entity_collection block;
     UnifiedDataSource will pick it up next time it (re)spins watchers.

Heavy compute paths (per repo policy):
  - Full-heap multi-UUID scan: cy_memscan.find_aligned_u64_in_set (AVX2)
  - Per-body HP narrowing:     cy_memscan.narrow_uXX_batch
"""
from __future__ import annotations

import argparse
import os
import sys
import time
import threading
from typing import Dict, List

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO_AUTO = os.path.dirname(_HERE)
if _SAO_AUTO not in sys.path:
    sys.path.insert(0, _SAO_AUTO)

from mem_probe.entity_discovery import (
    MonsterTruth, discover_monster_klass, persist_entity_anchor,
)
from mem_probe.locator import ANCHORS_PATH


class _MonsterCollector:
    """Thread-safe sink for on_monster_update callbacks."""

    def __init__(self):
        self._lock = threading.Lock()
        self._by_uuid: Dict[int, MonsterTruth] = {}
        self._n_updates = 0

    def __call__(self, payload: dict) -> None:
        try:
            uuid = int(payload.get("uuid") or 0)
            hp = int(payload.get("hp") or 0)
            max_hp = int(payload.get("max_hp") or 0)
            name = str(payload.get("name") or "")
        except Exception:
            return
        if uuid <= 0 or max_hp <= 0:
            return
        with self._lock:
            self._n_updates += 1
            prev = self._by_uuid.get(uuid)
            if prev is None or max_hp > prev.max_hp or (hp > prev.hp and max_hp == prev.max_hp):
                self._by_uuid[uuid] = MonsterTruth(uuid=uuid, hp=hp, max_hp=max_hp, name=name)

    def snapshot(self) -> List[MonsterTruth]:
        with self._lock:
            return list(self._by_uuid.values())

    def n_distinct(self) -> int:
        with self._lock:
            return len(self._by_uuid)

    def n_updates(self) -> int:
        with self._lock:
            return self._n_updates


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Discover MonsterEntity klass + offsets (TCP truth + Cython AVX2)")
    ap.add_argument("--min-monsters", type=int, default=2,
                    help="Wait for ≥ this many distinct monster UUIDs (default: 2)")
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="Max seconds to wait for TCP truth (default: 120)")
    ap.add_argument("--print-every", type=float, default=2.0,
                    help="Status print interval (default: 2s)")
    ap.add_argument("--anchors", default=ANCHORS_PATH,
                    help=f"anchors.json path (default: {ANCHORS_PATH})")
    args = ap.parse_args(argv)

    # ── Set up TCP bridge with monster callback ──
    from game_state import GameStateManager  # type: ignore
    from packet_bridge import PacketBridge  # type: ignore

    state_mgr = GameStateManager()
    collector = _MonsterCollector()
    bridge = PacketBridge(
        state_mgr,
        on_monster_update=collector,
        on_damage=None,
        on_boss_event=None,
        on_scene_change=None,
        data_source='tcp',
    )

    print(f"[discover-entities] starting PacketBridge (data_source=tcp)")
    print(f"[discover-entities] PLEASE GO INTO COMBAT in-game. Need ≥{args.min_monsters} "
          f"distinct monster UUIDs; will wait up to {args.timeout}s.")
    bridge.start()

    deadline = time.time() + args.timeout
    last_print = 0.0
    try:
        while time.time() < deadline:
            n = collector.n_distinct()
            now = time.time()
            if now - last_print > args.print_every:
                last_print = now
                print(f"[discover-entities] collected {n} distinct UUIDs "
                      f"({collector.n_updates()} updates, "
                      f"{int(deadline - now)}s remaining)")
            if n >= args.min_monsters:
                # Give the parser a couple more ticks to refine HP values
                time.sleep(1.5)
                break
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("[discover-entities] interrupted, using whatever we have so far")

    truths = collector.snapshot()
    n = len(truths)
    print(f"[discover-entities] gathered {n} distinct monsters")
    if n < 2:
        print(f"[discover-entities] FAIL: need at least 2 distinct UUIDs, only got {n}")
        try:
            bridge.stop()
        except Exception:
            pass
        return 2

    # ── Stop TCP bridge BEFORE memory scan to avoid CPU contention ──
    print("[discover-entities] stopping TCP bridge to free CPU for memscan ...")
    try:
        bridge.stop()
    except Exception:
        pass
    time.sleep(0.5)

    # ── Open process (read-only) and run the Cython discovery ──
    from mem_probe.locator import SmartLocator
    sl = SmartLocator()
    pm = sl.pm
    print(f"[discover-entities] attached to Star.exe pid={pm.pid}")
    result = discover_monster_klass(pm, truths)
    if result is None:
        print(f"[discover-entities] FAIL: convergence not reached. "
              f"Try with more monsters (--min-monsters 3+) or fight non-summons.")
        return 3

    print(f"[discover-entities] persisting entity_collection anchor → {args.anchors}")
    persist_entity_anchor(args.anchors, result)
    print(f"[discover-entities] DONE. Restart SAO-UI to activate the entity watcher.")
    print(f"[discover-entities] anchor block:")
    import json
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
