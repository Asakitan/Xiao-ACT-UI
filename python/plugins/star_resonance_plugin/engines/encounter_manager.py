# -*- coding: utf-8 -*-
# Encounter lifecycle manager for TCP-first ACT analytics.
#
# ``DpsTracker`` owns counters.  ``EncounterManager`` owns lifecycle metadata:
# why an encounter started, which scene/dungeon it belongs to, whether a reset is
# pending, and when a report was finalized.  It intentionally has no dependency
# on UI objects so replay tests, webview, and entity overlays can share it.

from __future__ import annotations

import copy
import threading
import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Tuple


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


def _event_ts(event: Dict[str, Any]) -> float:
    ts = _safe_float((event or {}).get("timestamp"), 0.0)
    return ts if ts > 0 else time.time()


@dataclass
class EncounterSnapshot:
    encounter_id: str = ""
    status: str = "idle"              # idle | active | pending_reset | finalized
    kind: str = "unknown"             # unknown | trash | boss | dungeon
    started_at: float = 0.0
    updated_at: float = 0.0
    ended_at: float = 0.0
    duration_s: float = 0.0
    dungeon_id: int = 0
    dungeon_scene_id: int = 0
    dungeon_difficulty: int = 0
    dungeon_name: str = ""
    boss_uuid: int = 0
    target_uuid: int = 0
    damage_events: int = 0
    heal_events: int = 0
    total_damage: int = 0
    total_heal: int = 0
    last_start_reason: str = ""
    last_reset_reason: str = ""
    pending_reset_reason: str = ""
    pending_reset_after: float = 0.0
    last_dungeon_event: Dict[str, Any] = field(default_factory=dict)
    last_skill_event: Dict[str, Any] = field(default_factory=dict)
    last_damage_event: Dict[str, Any] = field(default_factory=dict)
    last_final_report_id: str = ""

    def to_dict(self) -> Dict[str, Any]:
        return {
            "encounter_id": self.encounter_id,
            "status": self.status,
            "kind": self.kind,
            "started_at": self.started_at,
            "updated_at": self.updated_at,
            "ended_at": self.ended_at,
            "duration_s": round(max(0.0, self.duration_s), 3),
            "dungeon_id": self.dungeon_id,
            "dungeon_scene_id": self.dungeon_scene_id,
            "dungeon_difficulty": self.dungeon_difficulty,
            "dungeon_name": self.dungeon_name,
            "boss_uuid": self.boss_uuid,
            "target_uuid": self.target_uuid,
            "damage_events": self.damage_events,
            "heal_events": self.heal_events,
            "total_damage": self.total_damage,
            "total_heal": self.total_heal,
            "last_start_reason": self.last_start_reason,
            "last_reset_reason": self.last_reset_reason,
            "pending_reset_reason": self.pending_reset_reason,
            "pending_reset_after": self.pending_reset_after,
            "last_dungeon_event": dict(self.last_dungeon_event or {}),
            "last_skill_event": dict(self.last_skill_event or {}),
            "last_damage_event": dict(self.last_damage_event or {}),
            "last_final_report_id": self.last_final_report_id,
        }


class EncounterManager:
    # Thread-safe encounter lifecycle state machine.

    def __init__(self,
                 idle_timeout_s: float = 15.0,
                 finalize_hook: Optional[Callable[[Dict[str, Any]], None]] = None) -> None:
        self.idle_timeout_s = float(idle_timeout_s or 0.0)
        self._lock = threading.RLock()
        self._snap = EncounterSnapshot()
        self._recent_reports: List[Dict[str, Any]] = []
        self._finalize_hooks: List[Callable[[Dict[str, Any]], None]] = []
        if callable(finalize_hook):
            self._finalize_hooks.append(finalize_hook)

    def register_finalized_hook(self, callback: Callable[[Dict[str, Any]], None]) -> None:
        if not callable(callback):
            return
        with self._lock:
            if callback not in self._finalize_hooks:
                self._finalize_hooks.append(callback)

    def unregister_finalized_hook(self, callback: Callable[[Dict[str, Any]], None]) -> None:
        with self._lock:
            try:
                self._finalize_hooks.remove(callback)
            except ValueError:
                pass

    def on_dungeon_event(self, event: Dict[str, Any]) -> Dict[str, Any]:
        event = dict(event or {})
        ts = _event_ts(event)
        finalize_report = None
        hooks: List[Callable[[Dict[str, Any]], None]] = []
        with self._lock:
            self._snap.last_dungeon_event = event
            dungeon_id = _safe_int(event.get("dungeon_id"), 0)
            scene_id = _safe_int(
                event.get("scene_id") or event.get("scene_uuid") or event.get("cur_map_id"), 0)
            difficulty = _safe_int(
                event.get("dungeon_difficulty") or event.get("difficulty") or event.get("level_id"), 0)
            if dungeon_id > 0:
                self._snap.dungeon_id = dungeon_id
            if scene_id > 0:
                old_scene = self._snap.dungeon_scene_id
                if old_scene and old_scene != scene_id and self._snap.status == "active":
                    if bool(event.get("preserve_combat", False)):
                        self._snap.pending_reset_reason = ""
                        self._snap.pending_reset_after = 0.0
                    elif bool(event.get("reset_on_next_damage", False)):
                        self.arm_pending_reset(
                            str(event.get("reason") or event.get("kind") or "scene_transition"),
                            delay_s=_safe_float(event.get("reset_delay_s"), 3.0),
                            now=ts,
                        )
                    else:
                        finalize_report, hooks = self._finalize_locked("scene_change", ts)
                        self._reset_locked(clear_context=False)
                self._snap.dungeon_scene_id = scene_id
            if difficulty > 0:
                self._snap.dungeon_difficulty = difficulty
            name = str(event.get("dungeon_name") or "")
            if name:
                self._snap.dungeon_name = name[:120]
            self._snap.updated_at = ts
        self._dispatch_hooks(finalize_report, hooks)
        return event

    def on_skill_event(self, event: Dict[str, Any]) -> Dict[str, Any]:
        event = dict(event or {})
        with self._lock:
            self._snap.last_skill_event = event
            self._snap.updated_at = _event_ts(event)
            kind = str(event.get("kind") or "")
            if kind in ("client_use", "server_stage_end", "server_end") and self._snap.status == "active":
                self._snap.kind = "boss" if self._snap.boss_uuid else self._snap.kind
        return event

    def on_damage_event(self, event: Dict[str, Any]) -> Dict[str, Any]:
        event = dict(event or {})
        ts = _event_ts(event)
        finalize_report = None
        hooks: List[Callable[[Dict[str, Any]], None]] = []
        with self._lock:
            if self._should_apply_pending_locked(event, ts):
                finalize_report, hooks = self._finalize_locked(
                    self._snap.pending_reset_reason or "pending_reset", ts)
                self._reset_locked(clear_context=False)
            amount = _safe_int(event.get("damage") or event.get("heal"), 0)
            is_heal = bool(event.get("is_heal", False) or str(event.get("kind") or "") == "heal")
            if amount <= 0 and not (event.get("is_immune") or event.get("is_absorbed")):
                return event
            if self._snap.status not in ("active", "pending_reset"):
                self._start_locked(event, ts, "first_damage")
            self._snap.status = "active"
            self._snap.updated_at = ts
            self._snap.ended_at = ts
            self._snap.duration_s = max(0.0, ts - self._snap.started_at)
            self._snap.last_damage_event = event
            if is_heal:
                self._snap.heal_events += 1
                self._snap.total_heal += max(0, amount)
            else:
                self._snap.damage_events += 1
                self._snap.total_damage += max(0, amount)
            target_uuid = _safe_int(event.get("target_uuid"), 0)
            if target_uuid > 0:
                self._snap.target_uuid = target_uuid
                if bool(event.get("target_is_combat_target") or event.get("target_is_monster")):
                    self._snap.boss_uuid = target_uuid
                    self._snap.kind = "boss"
            if self._snap.kind == "unknown":
                self._snap.kind = "trash" if self._snap.total_damage < 1_000_000 else "boss"
        self._dispatch_hooks(finalize_report, hooks)
        return event

    def arm_pending_reset(self, reason: str = "restart", delay_s: float = 3.0,
                          now: Optional[float] = None) -> None:
        ts = float(now if now is not None else time.time())
        with self._lock:
            self._snap.pending_reset_reason = str(reason or "restart")
            self._snap.pending_reset_after = ts + max(0.0, float(delay_s or 0.0))
            if self._snap.status == "active":
                self._snap.status = "pending_reset"
            self._snap.updated_at = ts

    def finalize_if_idle(self, now: Optional[float] = None,
                         reason: str = "idle_timeout") -> bool:
        ts = float(now if now is not None else time.time())
        report = None
        hooks: List[Callable[[Dict[str, Any]], None]] = []
        with self._lock:
            if self.idle_timeout_s <= 0 or self._snap.status not in ("active", "pending_reset"):
                return False
            if self._snap.updated_at <= 0 or (ts - self._snap.updated_at) < self.idle_timeout_s:
                return False
            report, hooks = self._finalize_locked(reason, ts)
            self._reset_locked(clear_context=False)
        self._dispatch_hooks(report, hooks)
        return report is not None

    def reset(self, reason: str = "manual_reset") -> Optional[Dict[str, Any]]:
        report = None
        hooks: List[Callable[[Dict[str, Any]], None]] = []
        with self._lock:
            report, hooks = self._finalize_locked(reason, time.time())
            self._reset_locked(clear_context=True)
        self._dispatch_hooks(report, hooks)
        return report

    def snapshot(self) -> Dict[str, Any]:
        with self._lock:
            snap = copy.deepcopy(self._snap)
            if snap.status in ("active", "pending_reset") and snap.started_at > 0:
                snap.duration_s = max(0.0, (snap.ended_at or snap.updated_at or time.time()) - snap.started_at)
            return snap.to_dict()

    def recent_reports(self, limit: int = 20) -> List[Dict[str, Any]]:
        with self._lock:
            return [copy.deepcopy(r) for r in self._recent_reports[-max(0, int(limit)):]][::-1]

    def _start_locked(self, event: Dict[str, Any], ts: float, reason: str) -> None:
        self._snap.encounter_id = uuid.uuid4().hex
        self._snap.status = "active"
        self._snap.kind = "unknown"
        self._snap.started_at = ts
        self._snap.updated_at = ts
        self._snap.ended_at = ts
        self._snap.duration_s = 0.0
        self._snap.damage_events = 0
        self._snap.heal_events = 0
        self._snap.total_damage = 0
        self._snap.total_heal = 0
        self._snap.target_uuid = 0
        self._snap.boss_uuid = 0
        self._snap.last_start_reason = str(reason or "start")
        self._snap.last_reset_reason = ""
        self._snap.pending_reset_reason = ""
        self._snap.pending_reset_after = 0.0

    def _should_apply_pending_locked(self, event: Dict[str, Any], ts: float) -> bool:
        if not self._snap.pending_reset_reason or self._snap.pending_reset_after <= 0:
            return False
        if ts < self._snap.pending_reset_after:
            return False
        if bool(event.get("is_heal", False)):
            return False
        amount = _safe_int(event.get("damage"), 0)
        return amount > 0 or bool(event.get("is_immune") or event.get("is_absorbed"))

    def _finalize_locked(self, reason: str, ts: float) -> Tuple[Optional[Dict[str, Any]], List[Callable[[Dict[str, Any]], None]]]:
        if self._snap.status not in ("active", "pending_reset"):
            return None, []
        if self._snap.total_damage <= 0 and self._snap.total_heal <= 0:
            return None, []
        self._snap.status = "finalized"
        self._snap.ended_at = ts
        self._snap.updated_at = ts
        self._snap.duration_s = max(0.0, ts - self._snap.started_at)
        self._snap.last_reset_reason = str(reason or "completed")
        report = self._snap.to_dict()
        report.update({
            "report_id": uuid.uuid4().hex,
            "report_reason": self._snap.last_reset_reason,
            "completed_at": ts,
            "completed_local_time": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(ts)),
        })
        self._snap.last_final_report_id = report["report_id"]
        self._recent_reports.append(copy.deepcopy(report))
        if len(self._recent_reports) > 100:
            self._recent_reports = self._recent_reports[-100:]
        return report, list(self._finalize_hooks)

    def _reset_locked(self, clear_context: bool) -> None:
        old = self._snap
        self._snap = EncounterSnapshot()
        if not clear_context:
            self._snap.dungeon_id = old.dungeon_id
            self._snap.dungeon_scene_id = old.dungeon_scene_id
            self._snap.dungeon_difficulty = old.dungeon_difficulty
            self._snap.dungeon_name = old.dungeon_name
            self._snap.last_dungeon_event = dict(old.last_dungeon_event or {})
            self._snap.last_skill_event = dict(old.last_skill_event or {})
            self._snap.last_final_report_id = old.last_final_report_id

    @staticmethod
    def _dispatch_hooks(report: Optional[Dict[str, Any]],
                        hooks: List[Callable[[Dict[str, Any]], None]]) -> None:
        if not report or not hooks:
            return
        for cb in hooks:
            try:
                cb(copy.deepcopy(report))
            except Exception:
                pass


__all__ = ["EncounterManager", "EncounterSnapshot"]