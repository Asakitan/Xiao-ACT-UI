# -*- coding: utf-8 -*-
"""Offline replay harness for ACT/GameState/DPS integration.

This module is deliberately small and side-effect free: it consumes normalized
event dictionaries that mirror the parser/PacketBridge callbacks, then updates
``GameStateManager`` and ``DpsTracker`` the same way runtime UI callbacks do.
It gives future tests and manual diagnostics a stable TCP-first contract without
requiring Npcap, protobuf payload construction, or a running game.
"""

from __future__ import annotations

import time
from typing import Any, Dict, Iterable, Optional

from engines.combat_analytics import boss_state_from_monster_update, build_act_snapshot
from engines.act_trigger_engine import ActTriggerEngine
from engines.dps_tracker import DpsTracker
from engines.encounter_manager import EncounterManager
from engines.game_state import GameStateManager


_BOSS_UPDATE_KEYS = {
    "boss_current_hp",
    "boss_total_hp",
    "boss_hp_est_pct",
    "boss_hp_source",
    "boss_shield_active",
    "boss_shield_pct",
    "boss_breaking_stage",
    "boss_extinction_pct",
    "boss_in_overdrive",
    "boss_invincible",
    "boss_raid_active",
    "boss_raid_phase",
    "boss_raid_phase_name",
}


def _safe_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        return int(default)


def _safe_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except Exception:
        return float(default)


class ActReplayHarness:
    """Small offline ACT replay surface.

    Parameters are injectable so later tests can share a real ``DpsTracker`` or
    a fixture ``GameStateManager``.  By default it creates both and wires them
    through the existing ``build_act_snapshot`` facade.
    """

    def __init__(self,
                 state_mgr: Optional[GameStateManager] = None,
                 dps_tracker: Optional[DpsTracker] = None,
                 encounter_mgr: Optional[EncounterManager] = None,
                 trigger_engine: Optional[ActTriggerEngine] = None,
                 source_probe: Any = None,
                 history_store: Any = None) -> None:
        self.state_mgr = state_mgr or GameStateManager()
        self.dps_tracker = dps_tracker or DpsTracker()
        self.encounter_mgr = encounter_mgr or EncounterManager()
        self.trigger_engine = trigger_engine
        self.source_probe = source_probe
        self.history_store = history_store
        self.events = []

    def set_self_uid(self, uid: int) -> None:
        uid = _safe_int(uid)
        self.dps_tracker.set_self_uid(uid)
        if uid > 0:
            self.state_mgr.update(player_id=str(uid))

    def emit_dungeon_event(self, event: Dict[str, Any]) -> Dict[str, Any]:
        """Apply a normalized dungeon/scene event to ``GameState``.

        Unknown names are kept empty/pending: this harness must never fabricate
        CN names before runtime ZTable extraction provides authoritative maps.
        """
        event = dict(event or {})
        event.setdefault("timestamp", time.time())
        event.setdefault("source", "replay")
        updates: Dict[str, Any] = {"last_dungeon_event": event}
        dungeon_id = _safe_int(event.get("dungeon_id"))
        scene_id = _safe_int(
            event.get("scene_id")
            or event.get("scene_uuid")
            or event.get("cur_map_id")
        )
        difficulty = _safe_int(
            event.get("dungeon_difficulty")
            or event.get("difficulty")
            or event.get("level_id")
        )
        if dungeon_id > 0:
            updates["dungeon_id"] = dungeon_id
            try:
                from tools.tablekit.name_tables import names
                resolved = names.dungeon(dungeon_id, default="")
                if resolved:
                    updates["dungeon_name"] = resolved
            except Exception:
                pass
        if scene_id > 0:
            updates["dungeon_scene_id"] = scene_id
        if difficulty > 0:
            updates["dungeon_difficulty"] = difficulty
        self.state_mgr.update(**updates)
        try:
            self.encounter_mgr.on_dungeon_event(event)
        except Exception:
            pass
        self.events.append(("dungeon", event))
        return event

    def emit_skill_event(self, event: Dict[str, Any]) -> Dict[str, Any]:
        event = dict(event or {})
        event.setdefault("timestamp", time.time())
        event.setdefault("source", "replay")
        self.state_mgr.update(last_skill_event=event)
        try:
            self.encounter_mgr.on_skill_event(event)
        except Exception:
            pass
        self.events.append(("skill", event))
        return event

    def emit_boss_state(self, state: Dict[str, Any]) -> Dict[str, Any]:
        state = dict(state or {})
        updates = {k: v for k, v in state.items() if k in _BOSS_UPDATE_KEYS}
        if "boss_current_hp" in updates and "boss_total_hp" in updates:
            cur = _safe_int(updates.get("boss_current_hp"))
            total = _safe_int(updates.get("boss_total_hp"))
            if total > 0:
                updates.setdefault("boss_hp_est_pct", max(0.0, min(1.0, cur / total)))
                updates.setdefault("boss_hp_source", "tcp")
        if updates:
            self.state_mgr.update(**updates)
        self.events.append(("boss_state", state))
        return state

    def emit_boss_event(self, event: Dict[str, Any]) -> Dict[str, Any]:
        event = dict(event or {})
        event.setdefault("timestamp", time.time())
        event.setdefault("source", "replay")
        self.state_mgr.update(last_boss_event=event)
        self.events.append(("boss_event", event))
        return event

    def emit_monster_update(self, event: Dict[str, Any]) -> Dict[str, Any]:
        event = dict(event or {})
        event.setdefault("timestamp", time.time())
        event.setdefault("source", "replay")
        updates = boss_state_from_monster_update(event)
        if updates:
            self.state_mgr.update(**updates)
        self.events.append(("monster_update", event))
        return event

    def emit_damage_event(self, event: Dict[str, Any]) -> Dict[str, Any]:
        event = dict(event or {})
        event.setdefault("timestamp", time.time())
        if event.get("attacker_is_self") and event.get("target_uuid"):
            try:
                self.dps_tracker.set_boss_uuid(_safe_int(event.get("target_uuid")))
            except Exception:
                pass
        try:
            self.encounter_mgr.on_damage_event(event)
        except Exception:
            pass
        self.dps_tracker.on_damage_event(event)
        self.events.append(("damage", event))
        return event

    def replay(self, events: Iterable[Dict[str, Any]]) -> Dict[str, Any]:
        """Replay a sequence of normalized events and return the ACT snapshot."""
        for event in events:
            kind = str((event or {}).get("kind") or (event or {}).get("type") or "").lower()
            if kind in (
                "enter_scene", "start_dungeon", "start_playing_dungeon",
                "sync_dungeon_data", "sync_dungeon_dirty_data", "dungeon", "scene",
            ):
                self.emit_dungeon_event(event)
            elif kind in ("client_use", "server_stage_end", "server_end", "skill"):
                self.emit_skill_event(event)
            elif kind in ("boss_state", "boss_attrs", "boss"):
                self.emit_boss_state(event)
            elif kind in ("boss_event", "boss_buff_event", "boss_buff"):
                self.emit_boss_event(event)
            elif kind in ("monster_update", "monster", "monster_attrs"):
                self.emit_monster_update(event)
            elif kind in ("damage", "heal"):
                self.emit_damage_event(event)
            else:
                self.events.append(("unknown", dict(event or {})))
        return self.snapshot()

    def snapshot(self) -> Dict[str, Any]:
        return build_act_snapshot(
            dps_tracker=self.dps_tracker,
            history_store=self.history_store,
            state_mgr=self.state_mgr,
            encounter_mgr=self.encounter_mgr,
            trigger_engine=self.trigger_engine,
            source_probe=self.source_probe,
        )

    def assert_minimal_act_contract(self) -> Dict[str, Any]:
        """Raise ``AssertionError`` if the core ACT snapshot contract regresses."""
        snap = self.snapshot()
        context = snap.get("context") or {}
        live = snap.get("live") or {}
        assert "last_dungeon_event" in context
        assert "last_skill_event" in context
        assert "last_boss_event" in context
        assert "status" in (snap.get("encounter") or {})
        assert "entities" in live
        assert "total_damage" in live
        return snap


__all__ = ["ActReplayHarness"]