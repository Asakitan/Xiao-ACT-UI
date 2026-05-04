"""UnifiedDataSource — PacketBridge-compatible facade integrating all watchers.

Replaces TCP packet parsing as the data source for SAO-UI. Combines:
    SmartLocator      → first/warm/cache anchoring
    MemSelfWatcher    → on_self_update
    MemSceneWatcher   → on_scene_change
    MemEntityWatcher  → on_monster_update
    MemCombatWatcher  → on_boss_event + in_combat
    MemDamageWatcher  → on_damage (PATH B: TCP damage-only by default)

Entry point used by `packet_bridge.PacketBridge` when `data_source='memory'`
or `'hybrid'`. Main app (`sao_webview.py`) doesn't change beyond a single
settings read added in Phase 7.
"""
from __future__ import annotations

import json
import os
import sys
import threading
import time
import traceback
from dataclasses import dataclass
from typing import Any, Callable, Dict, List, Optional

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO_AUTO = os.path.dirname(_HERE)
if _SAO_AUTO not in sys.path:
    sys.path.insert(0, _SAO_AUTO)

from mem_probe.locator import SmartLocator, SmartLocatorError, ANCHORS_PATH
from mem_probe.self_watcher import MemSelfWatcher, make_config_from_anchors
from mem_probe.scene_watcher import MemSceneWatcher, SceneReadConfig
from mem_probe.entity_watcher import MemEntityWatcher, EntityReadConfig
from mem_probe.combat_watcher import MemCombatWatcher, CombatReadConfig
from mem_probe.damage_watcher import MemDamageWatcher


def _load_anchors() -> dict:
    if not os.path.isfile(ANCHORS_PATH):
        return {}
    try:
        with open(ANCHORS_PATH, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return {}


def _get_v2_anchor(anchors: dict, name: str) -> dict:
    sl = anchors.get("smart_locator", {})
    nested = sl.get("anchors", {})
    if isinstance(nested, dict):
        return nested.get(name, {}) or {}
    return {}


def _parse_hex(v) -> int:
    if v is None:
        return 0
    if isinstance(v, int):
        return v
    try:
        return int(str(v), 16)
    except (ValueError, TypeError):
        return 0


class UnifiedDataSource:
    """Facade that owns the locator + all watchers and emits TCP-compatible
    callbacks. PacketBridge delegates to this when in memory/hybrid mode.
    """

    def __init__(self, state_mgr, *,
                 mode: str = "auto",   # 'auto' | 'memory' | 'hybrid'
                 on_self_update: Optional[Callable[[dict], None]] = None,
                 on_damage: Optional[Callable[[dict], None]] = None,
                 on_monster_update: Optional[Callable[[dict], None]] = None,
                 on_boss_event: Optional[Callable[[dict], None]] = None,
                 on_scene_change: Optional[Callable[[dict], None]] = None,
                 on_status_change: Optional[Callable[[str, str], None]] = None):
        self._state_mgr = state_mgr
        self.mode = mode
        self._cb_self = on_self_update
        self._cb_damage = on_damage
        self._cb_monster = on_monster_update
        self._cb_boss = on_boss_event
        self._cb_scene = on_scene_change
        self._cb_status = on_status_change
        self._locator: Optional[SmartLocator] = None
        self._self_watcher: Optional[MemSelfWatcher] = None
        self._scene_watcher: Optional[MemSceneWatcher] = None
        self._entity_watcher: Optional[MemEntityWatcher] = None
        self._combat_watcher: Optional[MemCombatWatcher] = None
        self._damage_watcher: Optional[MemDamageWatcher] = None
        self._lock = threading.Lock()
        self._started = False
        self._status: str = "init"
        self._last_error: str = ""

    # ───────── lifecycle ─────────

    def start(self) -> bool:
        """Start all watchers. Returns True on success.

        On failure (e.g. Star.exe not running), sets status='error' and
        returns False. Caller (PacketBridge) decides whether to fall back
        to TCP.
        """
        if self._started:
            return True
        try:
            self._set_status("locating")
            self._locator = SmartLocator()
            refs = self._locator.locate()
            if not refs.char_serialize:
                raise SmartLocatorError("locator returned empty refs")
        except Exception as e:
            self._set_status("error", f"locate failed: {e}")
            traceback.print_exc()
            return False

        try:
            self._spin_up_watchers(refs)
        except Exception as e:
            self._set_status("error", f"watchers init failed: {e}")
            traceback.print_exc()
            return False

        self._started = True
        self._set_status("running")
        return True

    def stop(self) -> None:
        for w in (self._self_watcher, self._scene_watcher,
                  self._entity_watcher, self._combat_watcher,
                  self._damage_watcher):
            if w is not None:
                try:
                    w.stop()
                except Exception:
                    pass
        self._started = False

    def _spin_up_watchers(self, refs) -> None:
        anchors = _load_anchors()
        pm = self._locator.pm

        # SELF watcher
        self_cfg = make_config_from_anchors(anchors)
        if self_cfg is not None:
            self._self_watcher = MemSelfWatcher(
                pm, self_cfg,
                on_self_update=self._handle_self_update,
                on_status_change=self._set_status,
                on_relocate_needed=self._trigger_relocate,
            )
            self._self_watcher.start()

        # Scene watcher
        scene_anchor = _get_v2_anchor(anchors, "scene_manager")
        if scene_anchor.get("obj_addr"):
            scene_cfg = SceneReadConfig(
                obj_addr=_parse_hex(scene_anchor.get("obj_addr")),
                scene_id_off=int(scene_anchor.get("scene_id_off", -1)),
                dungeon_id_off=int(scene_anchor.get("dungeon_id_off", -1)),
                layer_off=int(scene_anchor.get("layer_off", -1)),
            )
            self._scene_watcher = MemSceneWatcher(
                pm, scene_cfg,
                on_scene_change=self._handle_scene_change,
                on_status_change=self._set_status,
            )
            self._scene_watcher.start()

        # Entity watcher
        entity_anchor = _get_v2_anchor(anchors, "entity_collection")
        monster_klass = _parse_hex(entity_anchor.get("monster_klass_ptr"))
        if monster_klass:
            # Build a minimal EntityReadConfig — field offsets must come from
            # a Phase 4 discovery (or hardcoded if game-specific).
            entity_cfg = EntityReadConfig(
                klass_ptr=monster_klass,
                field_specs=[
                    # Default IL2CPP-style layout (game-specific; tune via phase 4)
                    ("uuid", 0x10, 8),
                    ("hp", 0x18, 4),
                    ("max_hp", 0x1C, 4),
                    ("is_dead", 0x20, 1),
                    ("profession_id", 0x24, 4),
                ],
                body_size=0x40,
                name="monster",
            )
            self._entity_watcher = MemEntityWatcher(
                pm, [entity_cfg],
                on_monster_update=self._handle_monster_update,
                on_status_change=self._set_status,
            )
            self._entity_watcher.start()

        # Combat watcher (in_combat + buff events)
        attr_obj = getattr(refs, "user_fight_attr", 0)
        if attr_obj:
            combat_cfg = CombatReadConfig(
                self_attr_obj=attr_obj,
                in_combat_off=-1,         # filled in via discovery later
                buff_list_off=-1,         # ditto
            )
            self._combat_watcher = MemCombatWatcher(
                pm, combat_cfg,
                entity_provider=(self._entity_watcher.entities
                                 if self._entity_watcher else None),
                on_combat_change=self._handle_combat_change,
                on_boss_event=self._handle_boss_event,
                on_status_change=self._set_status,
            )
            self._combat_watcher.start()

        # Damage watcher (PATH B by default)
        if self.mode in ("hybrid", "auto", "memory"):
            self._damage_watcher = MemDamageWatcher(
                on_damage=self._handle_damage,
                on_status_change=self._set_status,
                path="B",
            )
            self._damage_watcher.start()

    # ───────── callback dispatchers ─────────

    def _handle_self_update(self, payload: dict) -> None:
        # Update GameStateManager
        try:
            if self._state_mgr is not None and hasattr(self._state_mgr, "update"):
                self._state_mgr.update(
                    hp_current=payload.get("hp", 0),
                    hp_max=payload.get("max_hp", 0),
                    level_base=payload.get("level", 0),
                    profession_id=payload.get("profession_id", 0),
                    fight_point=payload.get("fight_point", 0),
                    stamina_max=payload.get("stamina_max", 0),
                )
        except Exception:
            traceback.print_exc()
        if self._cb_self:
            try:
                self._cb_self(payload)
            except Exception:
                traceback.print_exc()

    def _handle_damage(self, ev: dict) -> None:
        if self._cb_damage:
            try:
                self._cb_damage(ev)
            except Exception:
                traceback.print_exc()

    def _handle_monster_update(self, ev: dict) -> None:
        if self._cb_monster:
            try:
                self._cb_monster(ev)
            except Exception:
                traceback.print_exc()

    def _handle_boss_event(self, ev: dict) -> None:
        if self._cb_boss:
            try:
                self._cb_boss(ev)
            except Exception:
                traceback.print_exc()

    def _handle_scene_change(self, ev: dict) -> None:
        if self._cb_scene:
            try:
                self._cb_scene(ev)
            except Exception:
                traceback.print_exc()

    def _handle_combat_change(self, in_combat: bool) -> None:
        # Fold into state_mgr if available
        try:
            if self._state_mgr is not None and hasattr(self._state_mgr, "update"):
                self._state_mgr.update(in_combat=in_combat)
        except Exception:
            traceback.print_exc()

    # ───────── status / health ─────────

    def _set_status(self, status: str, error: str = "") -> None:
        with self._lock:
            self._status = status
            if error:
                self._last_error = error
        if self._cb_status:
            try:
                self._cb_status(status, error)
            except Exception:
                pass

    def _trigger_relocate(self) -> None:
        """Watcher signaled SELF refs are stale; re-locate and reconfigure."""
        if self._locator is None:
            return
        try:
            self._set_status("relocating")
            new_refs = self._locator.locate()
            if not new_refs.char_serialize:
                self._set_status("error", "relocate returned empty refs")
                return
            # Restart self watcher with new config
            anchors = _load_anchors()
            new_cfg = make_config_from_anchors(anchors)
            if new_cfg is not None and self._self_watcher is not None:
                self._self_watcher.stop()
                self._self_watcher = MemSelfWatcher(
                    self._locator.pm, new_cfg,
                    on_self_update=self._handle_self_update,
                    on_status_change=self._set_status,
                    on_relocate_needed=self._trigger_relocate,
                )
                self._self_watcher.start()
            self._set_status("running")
        except Exception as e:
            self._set_status("error", f"relocate failed: {e}")
            traceback.print_exc()

    def health(self) -> dict:
        return {
            "started": self._started,
            "mode": self.mode,
            "status": self._status,
            "last_error": self._last_error,
            "watchers": {
                "self": self._self_watcher.health() if self._self_watcher else None,
                "scene": self._scene_watcher.health() if self._scene_watcher else None,
                "entity": self._entity_watcher.health() if self._entity_watcher else None,
                "combat": self._combat_watcher.health() if self._combat_watcher else None,
                "damage": self._damage_watcher.health() if self._damage_watcher else None,
            },
        }
