# -*- coding: utf-8 -*-
# AI editor engine actions owned by the Star Resonance plugin.

from __future__ import annotations

from typing import Any, Dict, List

_ACTION_KEYS = (
    "game_state",
    "entity_list",
    "dps_summary",
    "dps_report",
    "boss_status",
    "combat_status",
    "buff_list",
    "auto_key_status",
)


def install_ai_engine_actions(owner: Any) -> None:
    actions = dict(getattr(owner, "_ai_engine_actions", None) or {})
    actions.update({
        "game_state": lambda **kw: _game_state(owner),
        "entity_list": lambda **kw: _entity_list(owner),
        "dps_summary": lambda **kw: _dps_summary(owner),
        "dps_report": lambda **kw: _dps_report(owner),
        "boss_status": lambda **kw: _boss_status(owner),
        "combat_status": lambda **kw: _combat_status(owner),
        "buff_list": lambda **kw: _buff_list(owner, kw.get("target", "self")),
        "auto_key_status": lambda **kw: _auto_key_status(owner),
    })
    owner._ai_engine_actions = actions


def uninstall_ai_engine_actions(owner: Any) -> None:
    actions = getattr(owner, "_ai_engine_actions", None)
    if not isinstance(actions, dict):
        return
    for key in _ACTION_KEYS:
        actions.pop(key, None)


def _state(owner: Any) -> Any:
    mgr = getattr(owner, "_state_mgr", None) or getattr(owner, "_game_state", None)
    return getattr(mgr, "state", mgr)


def _get(obj: Any, key: str, default: Any = None) -> Any:
    if isinstance(obj, dict):
        return obj.get(key, default)
    return getattr(obj, key, default)


def _game_state(owner: Any) -> Dict[str, Any]:
    state = _state(owner)
    return {
        key: _get(state, key)
        for key in (
            "uid", "player_id", "name", "player_name", "level",
            "profession", "profession_name", "hp", "hp_current", "max_hp",
            "hp_max", "scene", "scene_id", "in_combat",
        )
        if _get(state, key) is not None
    }


def _entity_list(owner: Any) -> List[Dict[str, Any]]:
    rows = getattr(owner, "_rows", None) or {}
    values = rows.values() if isinstance(rows, dict) else rows
    out = []
    for row in values or []:
        if not isinstance(row, dict):
            continue
        out.append({
            key: row.get(key)
            for key in (
                "uuid", "name", "kind", "hp", "max_hp", "level",
                "total_damage", "dps",
            )
        })
        if len(out) >= 50:
            break
    return out


def _dps_summary(owner: Any) -> Dict[str, Any]:
    rows = getattr(owner, "_rows", None) or {}
    values = rows.values() if isinstance(rows, dict) else rows
    players = []
    for row in values or []:
        if not isinstance(row, dict):
            continue
        damage = row.get("total_damage", 0) or 0
        if damage <= 0:
            continue
        players.append({
            "name": row.get("name"),
            "damage": damage,
            "dps": row.get("dps", 0),
            "pct": row.get("damage_pct", 0),
        })
    players.sort(key=lambda item: item["damage"], reverse=True)
    return {"players": players}


def _dps_report(owner: Any) -> Dict[str, Any]:
    tracker = getattr(owner, "_dps_tracker", None)
    fn = getattr(tracker, "get_last_report", None)
    if callable(fn):
        report = fn()
        if report:
            return report if isinstance(report, dict) else {"data": str(report)}
    return {"note": "No report available"}


def _boss_status(owner: Any) -> Dict[str, Any]:
    state = _state(owner)
    keys = ("boss_hp", "boss_max_hp", "boss_break", "boss_shield", "boss_name")
    return {key: _get(state, key) for key in keys if _get(state, key) is not None}


def _combat_status(owner: Any) -> Dict[str, Any]:
    state = _state(owner)
    encounter = getattr(owner, "_encounter_mgr", None) or getattr(owner, "_encounter_manager", None)
    result = {"in_combat": bool(_get(state, "in_combat", False))}
    if encounter is not None:
        result["duration"] = getattr(encounter, "combat_duration", 0)
        result["encounter_count"] = getattr(encounter, "encounter_count", 0)
    return result


def _buff_list(owner: Any, target: str) -> Dict[str, Any]:
    state = _state(owner)
    if target == "boss":
        return {"target": "boss", "buffs": _get(state, "boss_buffs", [])}
    data = getattr(owner, "_buffmon_data", {}) or {}
    return {"target": target or "self", "buffs": data.get("buff_list", [])}


def _auto_key_status(owner: Any) -> Dict[str, Any]:
    engine = getattr(owner, "_auto_key_engine", None)
    if engine is None:
        return {"available": False}
    fn = getattr(engine, "get_status", None)
    if callable(fn):
        status = fn()
        if isinstance(status, dict):
            status["available"] = True
            return status
    return {
        "available": True,
        "running": bool(getattr(engine, "_running", False)),
    }
