"""MemCombatWatcher — in_combat status + buff/debuff event detection.

Two responsibilities:
  1. Poll SELF.user_fight_attr.in_combat (50ms) → emit on_combat_change(bool)
  2. Poll buff lists on every known entity (200ms) → diff buff_id sets,
     emit on_boss_event(event_type, host_uuid, buff_id, ...)

Buff event types (matches packet_parser TCP path):
    47 → shield_broken
    51 → super_armor_broken
    58 → enter_breaking
    88 → into_fracture_state
"""
from __future__ import annotations

import os
import sys
import threading
import time
import traceback
from dataclasses import dataclass
from typing import Callable, Dict, List, Optional, Set

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO_AUTO = os.path.dirname(_HERE)
if _SAO_AUTO not in sys.path:
    sys.path.insert(0, _SAO_AUTO)


@dataclass
class CombatReadConfig:
    # SELF in_combat field location
    self_attr_obj: int = 0
    in_combat_off: int = -1     # i32 offset inside user_fight_attr
    # Buff list location per entity (heuristic: typically a RepeatedField at
    # well-known offset inside UserFightAttr or a dedicated BuffComponent)
    buff_list_off: int = -1            # offset of List<BuffInfo> ptr inside attr_obj
    buff_list_count_off: int = 24      # +0x18 in IL2CPP RepeatedField/List<T>
    buff_list_array_off: int = 16      # +0x10 = T[] ptr
    buff_array_elems_off: int = 32     # +0x20 = first element address
    buff_struct_size: int = 0x30       # bytes per BuffInfo
    buff_id_field_off: int = 16        # offset of BuffId i32 within BuffInfo


# Buff event types we forward as on_boss_event (matches packet_parser)
TRACKED_BUFF_TYPES = {47, 51, 58, 88}


class MemCombatWatcher:
    COMBAT_POLL_INTERVAL_S = 0.05  # 50ms — combat toggle should be tight
    BUFF_POLL_INTERVAL_S = 0.20    # 200ms — buff event throughput
    FAIL_THRESHOLD = 10

    def __init__(self, pm, config: CombatReadConfig, *,
                 entity_provider: Optional[Callable[[], Dict[int, object]]] = None,
                 on_combat_change: Optional[Callable[[bool], None]] = None,
                 on_boss_event: Optional[Callable[[dict], None]] = None,
                 on_status_change: Optional[Callable[[str, str], None]] = None):
        self.pm = pm
        self.config = config
        self.entity_provider = entity_provider   # returns dict[uuid, EntityState]
        self.on_combat_change = on_combat_change
        self.on_boss_event = on_boss_event
        self.on_status_change = on_status_change
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._last_in_combat: Optional[bool] = None
        # uuid → set of buff_ids currently active
        self._buffs_per_entity: Dict[int, Set[int]] = {}
        self._last_buff_poll_ts = 0.0
        self._fail_count = 0
        self._tick_count = 0

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(
            target=self._loop, name="mem-combat-watcher", daemon=True)
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
                # in_combat tick (every loop ~50ms)
                self._poll_in_combat()
                # buff tick (every BUFF_POLL_INTERVAL_S)
                now = time.time()
                if now - self._last_buff_poll_ts >= self.BUFF_POLL_INTERVAL_S:
                    self._last_buff_poll_ts = now
                    self._poll_buffs()
            except Exception:
                traceback.print_exc()
                self._fail_count += 1
                if self._fail_count >= self.FAIL_THRESHOLD:
                    if self.on_status_change:
                        try:
                            self.on_status_change("error",
                                f"combat poll failed {self._fail_count}x")
                        except Exception:
                            pass
                    self._fail_count = 0
            self._stop.wait(self.COMBAT_POLL_INTERVAL_S)

    # ───────── in_combat ─────────

    def _poll_in_combat(self) -> None:
        cfg = self.config
        if cfg.self_attr_obj <= 0 or cfg.in_combat_off < 0:
            return
        blob = self.pm.read_bytes(cfg.self_attr_obj + cfg.in_combat_off, 4)
        if not blob:
            return
        in_combat = bool(int.from_bytes(blob, "little"))
        if in_combat != self._last_in_combat:
            self._last_in_combat = in_combat
            if self.on_combat_change:
                try:
                    self.on_combat_change(in_combat)
                except Exception:
                    traceback.print_exc()

    # ───────── buff diffing ─────────

    def _poll_buffs(self) -> None:
        if self.entity_provider is None or self.config.buff_list_off < 0:
            return
        try:
            entities = self.entity_provider()
        except Exception:
            return
        for uuid, state in list(entities.items()):
            try:
                self._poll_entity_buffs(uuid, state)
            except Exception:
                # Single-entity failure should not block others
                pass

    def _poll_entity_buffs(self, uuid: int, state) -> None:
        cfg = self.config
        attr_obj = getattr(state, "obj_addr", 0)
        if not attr_obj:
            return
        # Read buff list head pointer
        list_blob = self.pm.read_bytes(attr_obj + cfg.buff_list_off, 8)
        if not list_blob:
            return
        list_ptr = int.from_bytes(list_blob, "little")
        if not (0x10000 <= list_ptr <= 0x7FFFFFFFFFFF):
            return
        # Read RepeatedField/List header (count + array ptr)
        header = self.pm.read_bytes(list_ptr, 0x30)
        if not header or len(header) < 32:
            return
        count = int.from_bytes(
            header[cfg.buff_list_count_off:cfg.buff_list_count_off + 4],
            "little")
        if count <= 0 or count > 256:  # sanity
            current_buffs: Set[int] = set()
        else:
            arr_ptr = int.from_bytes(
                header[cfg.buff_list_array_off:cfg.buff_list_array_off + 8],
                "little")
            if not (0x10000 <= arr_ptr <= 0x7FFFFFFFFFFF):
                current_buffs = set()
            else:
                # Read all buff entries in one RPM
                total_bytes = cfg.buff_array_elems_off + \
                              count * cfg.buff_struct_size
                arr_blob = self.pm.read_bytes(arr_ptr, total_bytes)
                current_buffs = set()
                if arr_blob and len(arr_blob) >= total_bytes:
                    for i in range(count):
                        base = cfg.buff_array_elems_off + i * cfg.buff_struct_size
                        if base + cfg.buff_id_field_off + 4 > len(arr_blob):
                            break
                        bid = int.from_bytes(
                            arr_blob[base + cfg.buff_id_field_off:
                                     base + cfg.buff_id_field_off + 4],
                            "little")
                        current_buffs.add(bid)
        # Diff vs last known
        last = self._buffs_per_entity.get(uuid, set())
        added = current_buffs - last
        removed = last - current_buffs
        # Emit events for tracked buff types (TODO: distinguish event_type)
        # For now, treat any added buff in TRACKED_BUFF_TYPES as that event
        for bid in added:
            if bid in TRACKED_BUFF_TYPES and self.on_boss_event:
                try:
                    self.on_boss_event({
                        "event_type": bid,
                        "host_uuid": uuid,
                        "buff_id": bid,
                        "source_uid": 0,
                    })
                except Exception:
                    traceback.print_exc()
        self._buffs_per_entity[uuid] = current_buffs

    def health(self) -> dict:
        return {
            "alive": bool(self._thread and self._thread.is_alive()),
            "tick_count": self._tick_count,
            "fail_count": self._fail_count,
            "in_combat": self._last_in_combat,
            "n_entities_buffs": len(self._buffs_per_entity),
        }
