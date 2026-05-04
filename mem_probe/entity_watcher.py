"""MemEntityWatcher — monster / nearby player tracking via Cython klass scan.

Strategy:
    fast-path (100ms): increment-read HP/extinction/is_dead for known entities
    discovery-path (1000ms): full-heap scan for monster klass_ptr → detect
                            new entities + drops

Emits `on_monster_update(entity_dict)` whenever an entity's HP / state
changes, in a schema compatible with packet_parser's MonsterData.to_dict().
"""
from __future__ import annotations

import os
import sys
import threading
import time
import traceback
import struct
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Set

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO_AUTO = os.path.dirname(_HERE)
if _SAO_AUTO not in sys.path:
    sys.path.insert(0, _SAO_AUTO)

from mem_probe import cy_memscan as _cy


@dataclass
class EntityReadConfig:
    """Per-entity-type read recipe.

    klass_ptr: the IL2CPP Il2CppClass* pointer (heap match for instance discovery)
    field_specs: list of (logical_name, byte_offset, byte_width) describing
                 fields to extract.

    Two layouts supported:
      FLAT  (attr_slot_off < 0): all fields read from obj body.
      NESTED (attr_slot_off >= 0): obj body contains uuid; obj+attr_slot_off
                                   is a ptr to an attr object. Fields whose
                                   logical_name starts with 'attr.' are read
                                   from the deref'd attr body instead. Fields
                                   without that prefix stay in obj body.

    Required logical names (semantics):
        'uuid'         — entity UUID (i64 typical) — always in obj body
        'hp'           — current HP — typically nested ('attr.hp')
        'max_hp'       — max HP — typically nested ('attr.max_hp')
    Optional:
        'is_dead', 'profession_id', 'max_extinction', 'extinction'
    """
    klass_ptr: int
    field_specs: List[tuple]   # [(name, off, width), ...]
    body_size: int = 0x200     # how many bytes to read per entity body
    name: str = "monster"      # for logging
    attr_slot_off: int = -1    # if >=0, deref obj+attr_slot_off → attr obj
    attr_body_size: int = 0x200  # how many bytes to read per attr body
    # Optional per-field encoding hints (logical_name -> 'i32'|'i64'|'f32'|'f64').
    # When a hint is present, the watcher reinterprets the unpacked u-int via
    # struct so the consumer sees the natural number. Default = u-int passthrough.
    field_encodings: Dict[str, str] = field(default_factory=dict)


@dataclass
class EntityState:
    uuid: int
    obj_addr: int
    hp: int = 0
    max_hp: int = 0
    is_dead: int = 0
    profession_id: int = 0
    max_extinction: int = 0
    extinction: int = 0
    last_seen_ts: float = 0.0
    last_emit_sig: tuple = ()


class MemEntityWatcher:
    FAST_INTERVAL_S = 0.1       # increment HP poll
    DISCOVERY_INTERVAL_S = 1.0  # full klass scan
    FAIL_THRESHOLD = 10
    DROP_AFTER_S = 3.0          # entity not seen for N seconds → mark dead

    def __init__(self, pm, configs: List[EntityReadConfig], *,
                 on_monster_update: Optional[Callable[[dict], None]] = None,
                 on_status_change: Optional[Callable[[str, str], None]] = None,
                 n_workers: int = 4):
        self.pm = pm
        self.configs = configs
        self.on_monster_update = on_monster_update
        self.on_status_change = on_status_change
        self.n_workers = n_workers
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._lock = threading.Lock()
        # uuid → EntityState
        self._entities: Dict[int, EntityState] = {}
        # obj_addr → klass_ptr (for fast-path bulk RPM)
        self._known_objs: Dict[int, int] = {}
        # klass_ptr → EntityReadConfig (lookup)
        self._cfg_by_klass: Dict[int, EntityReadConfig] = {
            c.klass_ptr: c for c in configs
        }
        self._fail_count = 0
        self._tick_count = 0
        self._last_discovery_ts = 0.0

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(
            target=self._loop, name="mem-entity-watcher", daemon=True)
        self._thread.start()

    def stop(self, timeout: float = 1.0) -> None:
        self._stop.set()
        if self._thread:
            if self._thread is threading.current_thread():
                return
            self._thread.join(timeout=timeout)
            self._thread = None

    def entities(self) -> Dict[int, EntityState]:
        """Snapshot of currently tracked entities. Used by combat_watcher
        for buff polling."""
        with self._lock:
            return {uuid: state for uuid, state in self._entities.items()}

    def _loop(self) -> None:
        while not self._stop.is_set():
            try:
                self._tick_count += 1
                now = time.time()
                # Discovery path — every DISCOVERY_INTERVAL_S
                if now - self._last_discovery_ts >= self.DISCOVERY_INTERVAL_S:
                    self._last_discovery_ts = now
                    self._discovery_pass()
                # Fast-path — every tick
                self._fast_pass()
                # Drop stale entities
                self._drop_stale(now)
            except Exception:
                traceback.print_exc()
                self._fail_count += 1
                if self._fail_count >= self.FAIL_THRESHOLD:
                    if self.on_status_change:
                        try:
                            self.on_status_change("error",
                                f"entity poll failed {self._fail_count}x")
                        except Exception:
                            pass
                    self._fail_count = 0
            self._stop.wait(self.FAST_INTERVAL_S)

    # ───────── discovery ─────────

    def _discovery_pass(self) -> None:
        """Full-heap scan for each configured klass; update _known_objs."""
        new_objs: Dict[int, int] = {}
        for cfg in self.configs:
            try:
                hits = self._scan_klass_full_heap(cfg.klass_ptr)
                for obj_addr in hits:
                    new_objs[obj_addr] = cfg.klass_ptr
            except Exception:
                traceback.print_exc()
        with self._lock:
            self._known_objs = new_objs

    def _scan_klass_full_heap(self, klass_ptr: int,
                              max_region_size: int = 256 * 1024 * 1024
                              ) -> List[int]:
        """Cython-AVX2 full-heap scan + parallel RPM."""
        hits: List[int] = []
        lock = threading.Lock()
        regions = [r for r in self.pm.iter_regions(
            only_readable=True, only_private=True)
            if r.size <= max_region_size]

        def scan_one(region):
            buf = self.pm.read_bytes(region.base, region.size)
            if buf is None:
                return
            offs = _cy.find_aligned_u64(buf, klass_ptr)
            if offs:
                with lock:
                    for off in offs:
                        hits.append(region.base + off)

        with ThreadPoolExecutor(max_workers=self.n_workers) as ex:
            list(ex.map(scan_one, regions))
        return hits

    # ───────── fast-path field read ─────────

    def _fast_pass(self) -> None:
        """For each known entity, read fields and emit if changed."""
        with self._lock:
            objs_snapshot = dict(self._known_objs)
        if not objs_snapshot:
            return

        # Group by klass for batch read with same field_specs
        by_klass: Dict[int, List[int]] = {}
        for obj, klass in objs_snapshot.items():
            by_klass.setdefault(klass, []).append(obj)

        for klass, obj_list in by_klass.items():
            cfg = self._cfg_by_klass.get(klass)
            if cfg is None:
                continue
            # Partition specs: 'attr.*' → nested attr body; others → obj body.
            obj_specs: List[tuple] = []   # (logical_name, off, width)
            attr_specs: List[tuple] = []
            for name, off, width in cfg.field_specs:
                if isinstance(name, str) and name.startswith("attr."):
                    attr_specs.append((name[5:], off, width))
                else:
                    obj_specs.append((name, off, width))
            obj_pack = [(off, width) for _n, off, width in obj_specs]
            attr_pack = [(off, width) for _n, off, width in attr_specs]
            encs = cfg.field_encodings or {}

            def _maybe_reinterpret(name: str, raw_uint: int, width: int) -> int:
                """Convert unpacked u-int back to natural number if encoded as float."""
                enc = encs.get(name)
                if enc == "f32" and width == 4:
                    return int(struct.unpack("<f", raw_uint.to_bytes(4, "little"))[0])
                if enc == "f64" and width == 8:
                    return int(struct.unpack("<d", raw_uint.to_bytes(8, "little"))[0])
                return raw_uint

            for obj in obj_list:
                blob = self.pm.read_bytes(obj, cfg.body_size)
                if not blob:
                    continue
                fields: Dict[str, int] = {}
                obj_vals = _cy.unpack_struct_fields(blob, obj_pack)
                for i, (n, _o, w) in enumerate(obj_specs):
                    fields[n] = _maybe_reinterpret(n, obj_vals[i], w)
                # NESTED: deref attr_slot, read attr body, unpack remaining fields
                if attr_specs and cfg.attr_slot_off >= 0:
                    if cfg.attr_slot_off + 8 > len(blob):
                        continue
                    attr_ptr = int.from_bytes(
                        blob[cfg.attr_slot_off:cfg.attr_slot_off + 8], "little")
                    if not (0x10000 <= attr_ptr <= 0x7FFFFFFFFFFF):
                        continue
                    attr_blob = self.pm.read_bytes(attr_ptr, cfg.attr_body_size)
                    if not attr_blob:
                        continue
                    attr_vals = _cy.unpack_struct_fields(attr_blob, attr_pack)
                    for i, (n, _o, w) in enumerate(attr_specs):
                        # field_encodings keys use the un-prefixed logical name
                        fields[n] = _maybe_reinterpret(n, attr_vals[i], w)
                self._update_entity(obj, klass, fields)

    def _update_entity(self, obj: int, klass: int, fields: dict) -> None:
        uuid = int(fields.get("uuid", 0))
        if uuid <= 0:
            return
        hp = int(fields.get("hp", 0))
        max_hp = int(fields.get("max_hp", 0))
        is_dead = int(fields.get("is_dead", 0))
        profession_id = int(fields.get("profession_id", 0))
        max_ext = int(fields.get("max_extinction", 0))
        ext = int(fields.get("extinction", 0))
        now = time.time()
        with self._lock:
            state = self._entities.get(uuid)
            if state is None:
                state = EntityState(uuid=uuid, obj_addr=obj)
                self._entities[uuid] = state
            state.obj_addr = obj
            state.hp = hp
            state.max_hp = max_hp
            state.is_dead = is_dead
            state.profession_id = profession_id
            state.max_extinction = max_ext
            state.extinction = ext
            state.last_seen_ts = now
            sig = (hp, max_hp, is_dead, max_ext, ext)
            should_emit = sig != state.last_emit_sig
            state.last_emit_sig = sig
        if should_emit and self.on_monster_update:
            try:
                self.on_monster_update({
                    "uuid": uuid,
                    "max_hp": max_hp,
                    "hp": hp,
                    "max_extinction": max_ext,
                    "is_dead": bool(is_dead),
                    "profession_id": profession_id,
                })
            except Exception:
                traceback.print_exc()

    def _drop_stale(self, now: float) -> None:
        """Mark entities not seen for DROP_AFTER_S as dead, then prune."""
        to_drop: List[int] = []
        emit_drops: List[dict] = []
        with self._lock:
            for uuid, state in self._entities.items():
                if now - state.last_seen_ts > self.DROP_AFTER_S:
                    to_drop.append(uuid)
                    if not state.is_dead:
                        emit_drops.append({
                            "uuid": uuid,
                            "max_hp": state.max_hp,
                            "hp": 0,
                            "max_extinction": state.max_extinction,
                            "is_dead": True,
                            "profession_id": state.profession_id,
                        })
            for uuid in to_drop:
                del self._entities[uuid]
        if self.on_monster_update:
            for ev in emit_drops:
                try:
                    self.on_monster_update(ev)
                except Exception:
                    traceback.print_exc()

    def health(self) -> dict:
        with self._lock:
            return {
                "alive": bool(self._thread and self._thread.is_alive()),
                "tick_count": self._tick_count,
                "fail_count": self._fail_count,
                "n_entities": len(self._entities),
                "n_known_objs": len(self._known_objs),
                "n_klass_configs": len(self.configs),
            }
