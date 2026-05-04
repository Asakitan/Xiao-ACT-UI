"""MemSelfWatcher — SELF polling thread + on_self_update callback.

Reads CharSerialize / UserFightAttr / substructs at 50ms cadence; emits
`on_self_update(player_data_dict)` throttled to 80ms (matches TCP path
node).  Uses persisted offsets from `anchors.smart_locator` so it works
even when IL2CPP dump.cs is stale.

Output dict schema mirrors `packet_parser.PlayerData.to_dict()` so
GameStateManager / DpsTracker / sao_webview.py see no difference vs
the TCP path.
"""
from __future__ import annotations

import os
import sys
import threading
import time
import traceback
from dataclasses import dataclass
from typing import Any, Callable, Dict, Optional

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO_AUTO = os.path.dirname(_HERE)
if _SAO_AUTO not in sys.path:
    sys.path.insert(0, _SAO_AUTO)


@dataclass
class SelfReadConfig:
    """Offsets needed to read the SELF object end-to-end without dump.cs.

    All offsets are in BYTES from the corresponding object base.
    """
    # CharSerialize layout
    char_obj: int
    uid_off: int
    attr_slot_off: int
    char_base_slot_off: int = -1       # -1 = unknown
    role_level_slot_off: int = -1
    profession_list_slot_off: int = -1
    energy_item_slot_off: int = -1
    season_medal_slot_off: int = -1
    # UserFightAttr layout
    attr_obj: int = 0
    cur_hp_off: int = -1
    max_hp_off: int = -1
    hp_width: int = 8
    # Substruct field offsets discovered by Phase 1
    role_level_field_off: int = -1     # Level i32 inside RoleLevel
    profession_field_off: int = -1     # CurProfessionId i32 inside ProfessionList
    energy_field_off: int = -1         # EnergyLimit i32 inside EnergyItem
    fight_point_field_off: int = -1    # FightPoint i32 inside CharBase
    # Other invariants
    char_id: int = 0   # the persisted UID — used for cheap liveness check


class MemSelfWatcher:
    """Background polling watcher for SELF state.

    Lifecycle:
        watcher = MemSelfWatcher(config, on_self_update=fn, on_status=fn)
        watcher.start()
        ... runs in daemon thread ...
        watcher.stop()
    """

    POLL_INTERVAL_S = 0.05         # 50ms read cadence
    EMIT_THROTTLE_MS = 80          # min interval between on_self_update emits
    LIFE_CHECK_INTERVAL_S = 5.0    # validate refs alive every 5s
    MAX_CONSECUTIVE_FAILS = 20     # then trigger relocation / fallback

    def __init__(self, pm, config: SelfReadConfig, *,
                 on_self_update: Optional[Callable[[Dict[str, Any]], None]] = None,
                 on_status_change: Optional[Callable[[str, str], None]] = None,
                 on_relocate_needed: Optional[Callable[[], None]] = None):
        self.pm = pm
        self.config = config
        self.on_self_update = on_self_update
        self.on_status_change = on_status_change
        self.on_relocate_needed = on_relocate_needed
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._lock = threading.Lock()
        self._last_emit_ms = 0.0
        self._last_emit_sig: tuple = ()
        self._fail_count = 0
        self._last_snap: Dict[str, Any] = {}
        self._tick_count = 0

    # ───────── lifecycle ─────────

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(
            target=self._loop, name="mem-self-watcher", daemon=True,
        )
        self._thread.start()

    def stop(self, timeout: float = 1.0) -> None:
        self._stop.set()
        if self._thread:
            if self._thread is threading.current_thread():
                return
            self._thread.join(timeout=timeout)
            self._thread = None

    def latest(self) -> Dict[str, Any]:
        with self._lock:
            return dict(self._last_snap)

    # ───────── main loop ─────────

    def _loop(self) -> None:
        last_life_check = time.time()
        while not self._stop.is_set():
            try:
                self._tick_count += 1
                snap = self._read_snapshot()
                if snap is None:
                    self._fail_count += 1
                    if self._fail_count >= self.MAX_CONSECUTIVE_FAILS:
                        self._notify_status("error",
                            f"SELF read failed {self._fail_count} times")
                        if self.on_relocate_needed:
                            try:
                                self.on_relocate_needed()
                            except Exception:
                                traceback.print_exc()
                        self._fail_count = 0
                else:
                    self._fail_count = 0
                    self._maybe_emit(snap)

                # Periodic liveness validation
                now = time.time()
                if now - last_life_check >= self.LIFE_CHECK_INTERVAL_S:
                    last_life_check = now
                    if not self._validate_alive():
                        self._notify_status("error", "SELF refs no longer alive")
                        if self.on_relocate_needed:
                            try:
                                self.on_relocate_needed()
                            except Exception:
                                traceback.print_exc()
            except Exception:
                traceback.print_exc()
            self._stop.wait(self.POLL_INTERVAL_S)

    # ───────── reads ─────────

    def _read_snapshot(self) -> Optional[Dict[str, Any]]:
        """Single-shot read of all SELF fields. Returns dict compatible
        with packet_parser.PlayerData.to_dict() schema."""
        cfg = self.config
        # Read char_obj body once (covers UID + all substruct slot ptrs)
        char_blob = self.pm.read_bytes(cfg.char_obj, 0x300)
        if not char_blob:
            return None

        # UID
        uid = int.from_bytes(
            char_blob[cfg.uid_off:cfg.uid_off + 8], "little") if cfg.uid_off >= 0 else 0
        if cfg.char_id and uid != cfg.char_id:
            return None  # SELF object has been recycled

        # Attr ptr
        attr_obj = cfg.attr_obj
        if cfg.attr_slot_off >= 0:
            attr_obj = int.from_bytes(
                char_blob[cfg.attr_slot_off:cfg.attr_slot_off + 8], "little")
        cur_hp = max_hp = 0
        if attr_obj and cfg.cur_hp_off >= 0 and cfg.max_hp_off >= 0:
            attr_blob = self.pm.read_bytes(attr_obj, 0x100)
            if attr_blob:
                cur_hp = int.from_bytes(
                    attr_blob[cfg.cur_hp_off:cfg.cur_hp_off + cfg.hp_width],
                    "little")
                max_hp = int.from_bytes(
                    attr_blob[cfg.max_hp_off:cfg.max_hp_off + cfg.hp_width],
                    "little")

        # Substruct fields (Phase 1 discovered)
        level = self._read_substruct_field(
            char_blob, cfg.role_level_slot_off, cfg.role_level_field_off, 4)
        profession_id = self._read_substruct_field(
            char_blob, cfg.profession_list_slot_off,
            cfg.profession_field_off, 4)
        energy_max = self._read_substruct_field(
            char_blob, cfg.energy_item_slot_off, cfg.energy_field_off, 4)
        fight_point = self._read_substruct_field(
            char_blob, cfg.char_base_slot_off, cfg.fight_point_field_off, 4)

        # Build PlayerData-compatible dict
        # (See packet_parser.PlayerData for the canonical fields.)
        snap = {
            "uid": int(uid),
            "uuid": int(uid),   # in this game they're the same (placeholder)
            "name": "",         # filled by SyncContainerData; we can't read string easily
            "level": int(level or 0),
            "level_extra": 0,
            "season_exp": 0,
            "hp": int(cur_hp),
            "max_hp": int(max_hp),
            "profession_id": int(profession_id or 0),
            "profession": "",
            "fight_point": int(fight_point or 0),
            "stamina_current": 0,
            "stamina_max": int(energy_max or 0),
            "energy_current": 0,
            "energy_total": int(energy_max or 0),
            "skill_slot_map": {},
            "skill_cd_map": {},
            "attr_skill_cd": 0,
            "attr_skill_cd_pct": 0,
            "attr_cd_accelerate_pct": 0,
            "temp_attr_cd_pct": 0,
            "temp_attr_cd_fixed": 0,
            "temp_attr_cd_accel": 0,
            # Source marker so consumers can tell mem from tcp
            "_source": "mem",
            "_ts": time.time(),
        }
        with self._lock:
            self._last_snap = snap
        return snap

    def _read_substruct_field(self, char_blob: bytes,
                              slot_off: int, field_off: int,
                              width: int) -> int:
        """Helper: read SubObj.Field given parent's char_blob + slot offsets."""
        if slot_off < 0 or field_off < 0 or slot_off + 8 > len(char_blob):
            return 0
        sub_ptr = int.from_bytes(char_blob[slot_off:slot_off + 8], "little")
        if not (0x10000 <= sub_ptr <= 0x7FFFFFFFFFFF):
            return 0
        blob = self.pm.read_bytes(sub_ptr + field_off, width)
        if not blob:
            return 0
        return int.from_bytes(blob, "little")

    # ───────── liveness ─────────

    def _validate_alive(self) -> bool:
        """3-RPM cheap liveness check: klass_ptr lives in some module +
        UID still matches known_uid."""
        cfg = self.config
        try:
            # Read first 8 bytes (klass_ptr)
            klass_blob = self.pm.read_bytes(cfg.char_obj, 8)
            if not klass_blob:
                return False
            # Read uid
            uid_blob = self.pm.read_bytes(cfg.char_obj + cfg.uid_off, 8)
            if not uid_blob:
                return False
            uid = int.from_bytes(uid_blob, "little")
            if cfg.char_id and uid != cfg.char_id:
                return False
            return True
        except Exception:
            return False

    # ───────── emit throttling ─────────

    def _maybe_emit(self, snap: Dict[str, Any]) -> None:
        if self.on_self_update is None:
            return
        # Build a content signature (skip _ts to avoid emitting on every tick)
        sig = (snap.get("uid"), snap.get("hp"), snap.get("max_hp"),
               snap.get("level"), snap.get("profession_id"),
               snap.get("fight_point"), snap.get("stamina_max"))
        now_ms = time.time() * 1000.0
        # Throttle: emit at most every EMIT_THROTTLE_MS ms, unless content changed
        if sig == self._last_emit_sig and \
                (now_ms - self._last_emit_ms) < self.EMIT_THROTTLE_MS:
            return
        self._last_emit_sig = sig
        self._last_emit_ms = now_ms
        try:
            self.on_self_update(snap)
        except Exception:
            traceback.print_exc()

    def _notify_status(self, mode: str, err: str) -> None:
        if self.on_status_change is None:
            return
        try:
            self.on_status_change(mode, err)
        except Exception:
            pass

    # ───────── diagnostics ─────────

    def health(self) -> dict:
        with self._lock:
            return {
                "alive": bool(self._thread and self._thread.is_alive()),
                "tick_count": self._tick_count,
                "fail_count": self._fail_count,
                "last_emit_age_s": (
                    (time.time() * 1000 - self._last_emit_ms) / 1000.0
                    if self._last_emit_ms else None),
                "config": {
                    "char_obj": hex(self.config.char_obj),
                    "attr_obj": hex(self.config.attr_obj),
                    "char_id": self.config.char_id,
                },
            }


def make_config_from_anchors(anchors: dict) -> Optional[SelfReadConfig]:
    """Build a SelfReadConfig from anchors.json `smart_locator` block.

    Reads either schema_v1 (flat keys) or schema_v2 (nested anchors.self).
    """
    sl = anchors.get("smart_locator", {})
    # v2 nested format
    self_anchor = sl.get("anchors", {}).get("self") if isinstance(
        sl.get("anchors"), dict) else None
    if self_anchor:
        substructs = self_anchor.get("substructs", {}) or {}
        def slot(name: str) -> int:
            d = substructs.get(name)
            if not isinstance(d, dict):
                return -1
            v = d.get("slot_off", -1)
            try:
                return int(v) if v not in (None, "") else -1
            except (ValueError, TypeError):
                return -1
        def fld(name: str) -> int:
            d = substructs.get(name)
            if not isinstance(d, dict):
                return -1
            v = d.get("discovered_field_off", -1)
            try:
                return int(v) if v not in (None, "") else -1
            except (ValueError, TypeError):
                return -1
        char_obj = int(self_anchor.get("obj_addr", "0x0"), 16)
        return SelfReadConfig(
            char_obj=char_obj,
            uid_off=int(self_anchor.get("uid_off", -1)),
            attr_slot_off=int(self_anchor.get("attr_slot_off", -1)),
            char_base_slot_off=slot("char_base"),
            role_level_slot_off=slot("role_level"),
            profession_list_slot_off=slot("profession_list"),
            energy_item_slot_off=slot("energy_item"),
            season_medal_slot_off=slot("season_medal_info"),
            attr_obj=int(substructs.get("user_fight_attr", {}).get(
                "obj_addr", "0x0"), 16) if isinstance(
                substructs.get("user_fight_attr"), dict) else 0,
            cur_hp_off=int(self_anchor.get("cur_hp_off", -1)),
            max_hp_off=int(self_anchor.get("max_hp_off", -1)),
            hp_width=int(self_anchor.get("hp_width", 8)),
            role_level_field_off=fld("role_level"),
            profession_field_off=fld("profession_list"),
            energy_field_off=fld("energy_item"),
            fight_point_field_off=fld("char_base"),
            char_id=int(sl.get("known_uid", 0)),
        )
    # v1 flat format (current production)
    char_obj = sl.get("last_self_obj")
    if not char_obj:
        return None
    def parse_hex(v) -> int:
        if v is None: return 0
        if isinstance(v, int): return v
        try: return int(str(v), 16)
        except (ValueError, TypeError): return 0
    return SelfReadConfig(
        char_obj=parse_hex(char_obj),
        uid_off=int(sl.get("last_uid_off", -1) or -1),
        attr_slot_off=int(sl.get("last_attr_slot_off", -1) or -1),
        attr_obj=parse_hex(sl.get("last_user_fight_attr")),
        cur_hp_off=int(sl.get("last_cur_hp_off", -1) or -1),
        max_hp_off=int(sl.get("last_max_hp_off", -1) or -1),
        hp_width=int(sl.get("last_hp_width", 8) or 8),
        char_id=int(sl.get("known_uid", 0)),
    )
