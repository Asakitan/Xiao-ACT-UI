"""CLI: discover SceneInfo / SceneManager klass + offsets via TCP truth + Cython.

Usage:
    python -m mem_probe.discover_scene
    python -m mem_probe.discover_scene --timeout 180

Workflow:
  1. Starts a TCP PacketBridge that reads SyncDungeonData → state_mgr.
  2. User enters a dungeon (any dungeon — we just need scene_uuid >0).
  3. Once we have scene_uuid (and ideally dungeon_id) the Cython value-anchor
     algorithm in scene_discovery runs.
  4. On success, anchors.json scene_manager block updated.

Heavy compute: Cython AVX2 i64 + i32 narrow scans (no Python inner loop).
"""
from __future__ import annotations

import argparse
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO_AUTO = os.path.dirname(_HERE)
if _SAO_AUTO not in sys.path:
    sys.path.insert(0, _SAO_AUTO)

from mem_probe.scene_discovery import (
    SceneTruth, discover_scene_klass, persist_scene_anchor,
)
from mem_probe.locator import ANCHORS_PATH


def _read_truth_from_state(state) -> SceneTruth:
    """Pull current scene values out of GameStateManager.state."""
    # Player scope first (most populated by SyncDungeonData)
    scene_uuid = 0
    dungeon_id = 0
    scene_id = 0
    layer = 0
    difficulty = 0
    try:
        # state may carry per-player dict; pick current uid's player if present.
        players = getattr(state, "_players", None)
        if isinstance(players, dict):
            cur = getattr(state, "_current_uid", 0)
            p = players.get(cur) if cur else (next(iter(players.values()), None))
            if p is not None:
                dungeon_id = int(getattr(p, "dungeon_id", 0) or 0)
                scene_id = int(getattr(p, "scene_id", 0) or 0)
                difficulty = int(getattr(p, "dungeon_difficulty", 0) or 0)
        # Some implementations expose flat fields on state directly
        if not dungeon_id:
            dungeon_id = int(getattr(state, "dungeon_id", 0) or 0)
        if not scene_id:
            scene_id = int(getattr(state, "scene_id", 0) or 0)
    except Exception:
        pass
    # Use dungeon_id as the unique scene_uuid anchor when SyncDungeonData
    # populates it (parser stores SceneUuid into player.dungeon_id; see
    # packet_parser.py:2398 `dungeon_id = scene_uuid`).
    scene_uuid = dungeon_id
    return SceneTruth(
        scene_uuid=int(scene_uuid),
        dungeon_id=int(dungeon_id),
        scene_id=int(scene_id),
        layer=int(layer),
        difficulty=int(difficulty),
    )


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Discover SceneInfo klass + offsets via SyncDungeonData truth")
    ap.add_argument("--timeout", type=float, default=180.0,
                    help="Max seconds to wait for scene truth (default: 180)")
    ap.add_argument("--print-every", type=float, default=2.0,
                    help="Status print interval (default: 2s)")
    ap.add_argument("--anchors", default=ANCHORS_PATH,
                    help=f"anchors.json path (default: {ANCHORS_PATH})")
    args = ap.parse_args(argv)

    from game_state import GameStateManager  # type: ignore
    from packet_bridge import PacketBridge  # type: ignore

    state_mgr = GameStateManager()
    scene_change_count = [0]

    def on_scene_change(ev):
        scene_change_count[0] += 1

    bridge = PacketBridge(
        state_mgr,
        on_monster_update=None,
        on_damage=None,
        on_boss_event=None,
        on_scene_change=on_scene_change,
        data_source='tcp',
    )

    print(f"[discover-scene] starting PacketBridge (data_source=tcp)")
    print(f"[discover-scene] PLEASE ENTER A DUNGEON in-game.")
    print(f"[discover-scene] We need scene_uuid>0 from SyncDungeonData. "
          f"timeout={args.timeout}s")
    bridge.start()

    deadline = time.time() + args.timeout
    last_print = 0.0
    truth: SceneTruth = SceneTruth(scene_uuid=0)
    try:
        while time.time() < deadline:
            try:
                state_obj = state_mgr.state
            except Exception:
                state_obj = None
            if state_obj is not None:
                truth = _read_truth_from_state(state_obj)
            now = time.time()
            if now - last_print > args.print_every:
                last_print = now
                print(f"[discover-scene] truth: scene_uuid={truth.scene_uuid} "
                      f"dungeon_id={truth.dungeon_id} scene_id={truth.scene_id} "
                      f"diff={truth.difficulty} ({int(deadline - now)}s left, "
                      f"{scene_change_count[0]} scene changes)")
            if truth.is_usable():
                # Wait one extra beat so dungeon_id and difficulty refine
                time.sleep(1.0)
                truth = _read_truth_from_state(state_mgr.state)
                break
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("[discover-scene] interrupted")

    if not truth.is_usable():
        print(f"[discover-scene] FAIL: never got a usable scene_uuid. "
              f"Is the player in a dungeon?")
        try:
            bridge.stop()
        except Exception:
            pass
        return 2

    print("[discover-scene] stopping TCP bridge to free CPU for memscan ...")
    try:
        bridge.stop()
    except Exception:
        pass
    time.sleep(0.5)

    from mem_probe.locator import SmartLocator
    sl = SmartLocator()
    pm = sl.pm
    print(f"[discover-scene] attached to Star.exe pid={pm.pid}")
    result = discover_scene_klass(pm, truth)
    if result is None:
        print(f"[discover-scene] FAIL: no convergent scene object")
        return 3

    print(f"[discover-scene] persisting scene_manager anchor → {args.anchors}")
    persist_scene_anchor(args.anchors, result)
    print(f"[discover-scene] DONE. Restart SAO-UI to activate the scene watcher.")
    import json
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
