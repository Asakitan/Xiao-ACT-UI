"""MemSceneWatcher — scene/dungeon polling + on_scene_change callback.

Watches SceneManager.scene_id / dungeon_id / layer for transitions.
Emits scene-change events compatible with packet_parser's on_scene_change
schema:
    {kind, reason, preserve_combat, reset_on_next_damage}

Reasons:
    - 'dungeon_enter'  — dungeon_id 0 → non-zero
    - 'dungeon_leave'  — non-zero → 0
    - 'layer_change'   — same dungeon, different scene_id (preserve_combat=True)
    - 'scene_restart'  — anything else
"""
from __future__ import annotations

import os
import sys
import threading
import time
import traceback
from dataclasses import dataclass
from typing import Callable, Optional

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO_AUTO = os.path.dirname(_HERE)
if _SAO_AUTO not in sys.path:
    sys.path.insert(0, _SAO_AUTO)


@dataclass
class SceneReadConfig:
    obj_addr: int                   # SceneManager singleton address
    scene_id_off: int = -1          # current scene id (i32)
    dungeon_id_off: int = -1        # current dungeon id (i32)
    layer_off: int = -1             # optional, layer index (i32)


class MemSceneWatcher:
    POLL_INTERVAL_S = 0.5    # 500ms — scene change is not high frequency
    LIFE_FAIL_THRESHOLD = 10

    def __init__(self, pm, config: SceneReadConfig, *,
                 on_scene_change: Optional[Callable[[dict], None]] = None,
                 on_status_change: Optional[Callable[[str, str], None]] = None):
        self.pm = pm
        self.config = config
        self.on_scene_change = on_scene_change
        self.on_status_change = on_status_change
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._last_scene_id = -1
        self._last_dungeon_id = -1
        self._last_layer = -1
        self._fail_count = 0
        self._tick_count = 0

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(
            target=self._loop, name="mem-scene-watcher", daemon=True)
        self._thread.start()

    def stop(self, timeout: float = 1.0) -> None:
        self._stop.set()
        if self._thread:
            if self._thread is threading.current_thread():
                return
            self._thread.join(timeout=timeout)
            self._thread = None

    def _loop(self) -> None:
        while not self._stop.is_set():
            try:
                self._tick_count += 1
                state = self._read_state()
                if state is None:
                    self._fail_count += 1
                else:
                    self._fail_count = 0
                    self._detect_change(state)
            except Exception:
                traceback.print_exc()
                self._fail_count += 1
            self._stop.wait(self.POLL_INTERVAL_S)

    def _read_state(self) -> Optional[tuple]:
        cfg = self.config
        if cfg.obj_addr <= 0:
            return None
        scene_id = dungeon_id = layer = 0
        if cfg.scene_id_off >= 0:
            blob = self.pm.read_bytes(cfg.obj_addr + cfg.scene_id_off, 4)
            if blob:
                scene_id = int.from_bytes(blob, "little")
        if cfg.dungeon_id_off >= 0:
            blob = self.pm.read_bytes(cfg.obj_addr + cfg.dungeon_id_off, 4)
            if blob:
                dungeon_id = int.from_bytes(blob, "little")
        if cfg.layer_off >= 0:
            blob = self.pm.read_bytes(cfg.obj_addr + cfg.layer_off, 4)
            if blob:
                layer = int.from_bytes(blob, "little")
        return (scene_id, dungeon_id, layer)

    def _detect_change(self, state: tuple) -> None:
        scene_id, dungeon_id, layer = state
        # First read — just record
        if self._last_scene_id < 0:
            self._last_scene_id = scene_id
            self._last_dungeon_id = dungeon_id
            self._last_layer = layer
            return
        if (scene_id, dungeon_id, layer) == (
                self._last_scene_id, self._last_dungeon_id, self._last_layer):
            return

        # Transition logic
        prev_d = self._last_dungeon_id
        prev_s = self._last_scene_id
        if prev_d == 0 and dungeon_id != 0:
            event = {
                "kind": "hard",
                "reason": "dungeon_enter",
                "preserve_combat": False,
                "reset_on_next_damage": False,
                "dungeon_id": dungeon_id,
                "scene_id": scene_id,
            }
        elif prev_d != 0 and dungeon_id == 0:
            event = {
                "kind": "hard",
                "reason": "dungeon_leave",
                "preserve_combat": False,
                "reset_on_next_damage": False,
                "dungeon_id": prev_d,
                "scene_id": scene_id,
            }
        elif prev_d == dungeon_id and prev_s != scene_id:
            event = {
                "kind": "soft",
                "reason": "layer_change",
                "preserve_combat": True,
                "reset_on_next_damage": False,
                "dungeon_id": dungeon_id,
                "scene_id": scene_id,
                "from_scene_id": prev_s,
            }
        else:
            event = {
                "kind": "hard",
                "reason": "scene_restart",
                "preserve_combat": False,
                "reset_on_next_damage": False,
                "dungeon_id": dungeon_id,
                "scene_id": scene_id,
            }
        self._last_scene_id = scene_id
        self._last_dungeon_id = dungeon_id
        self._last_layer = layer
        if self.on_scene_change:
            try:
                self.on_scene_change(event)
            except Exception:
                traceback.print_exc()

    def latest(self) -> dict:
        return {
            "scene_id": self._last_scene_id,
            "dungeon_id": self._last_dungeon_id,
            "layer": self._last_layer,
        }

    def health(self) -> dict:
        return {
            "alive": bool(self._thread and self._thread.is_alive()),
            "tick_count": self._tick_count,
            "fail_count": self._fail_count,
            "current": self.latest(),
        }
