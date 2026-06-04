# -*- coding: utf-8 -*-
"""Shared combat semantic enrichment for ACT, AutoKey, and BossRaid.

This module turns parser runtime events into small, stable ``facts`` rows.
It does not own packet parsing and does not store raw payload bytes; callers can
attach the returned dictionaries to ACT event payloads, runtime caches, or
diagnostic snapshots.
"""

from __future__ import annotations

from typing import Any, Mapping

try:
    from packet_parser.enums import BuffEventType
except Exception:  # pragma: no cover - standalone tool fallback
    class BuffEventType:  # type: ignore[no-redef]
        HOST_DEATH = 12
        BODY_PART_DEAD = 15
        BODY_PART_STATE_CHANGE = 17
        SHIELD_BROKEN = 47
        SUPER_ARMOR_BROKEN = 51
        ENTER_BREAKING = 58
        INTO_FRACTURE_STATE = 88

try:
    from packet_parser.skills import (
        PROFESSION_NORMAL_ATTACK,
        PROFESSION_SKILL,
        PROFESSION_ULTIMATE,
        SUB_PROFESSION_NAMES,
        _SKILL_TO_PROFESSION,
    )
except Exception:  # pragma: no cover - standalone tool fallback
    PROFESSION_NORMAL_ATTACK = {}
    PROFESSION_SKILL = {}
    PROFESSION_ULTIMATE = {}
    SUB_PROFESSION_NAMES = {}
    _SKILL_TO_PROFESSION = {}


BOSS_MECHANIC_EVENTS: dict[int, dict[str, Any]] = {
    int(BuffEventType.HOST_DEATH): {
        "key": "host_death",
        "label": "主体死亡",
        "trigger_family": "death",
        "severity": "high",
    },
    int(BuffEventType.BODY_PART_DEAD): {
        "key": "body_part_dead",
        "label": "部位死亡",
        "trigger_family": "part",
        "severity": "medium",
    },
    int(BuffEventType.BODY_PART_STATE_CHANGE): {
        "key": "body_part_state_change",
        "label": "部位状态变化",
        "trigger_family": "part",
        "severity": "medium",
    },
    int(BuffEventType.SHIELD_BROKEN): {
        "key": "shield_broken",
        "label": "护盾破裂",
        "trigger_family": "shield",
        "severity": "high",
    },
    int(BuffEventType.SUPER_ARMOR_BROKEN): {
        "key": "super_armor_broken",
        "label": "霸体破裂",
        "trigger_family": "shield",
        "severity": "high",
    },
    int(BuffEventType.ENTER_BREAKING): {
        "key": "enter_breaking",
        "label": "进入破防",
        "trigger_family": "breaking",
        "severity": "high",
    },
    int(BuffEventType.INTO_FRACTURE_STATE): {
        "key": "into_fracture_state",
        "label": "进入碎裂状态",
        "trigger_family": "breaking",
        "severity": "high",
    },
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


def _clean_text(value: Any) -> str:
    return str(value or "").strip()


def _resolver_name(kind: str, id_: int) -> str:
    if id_ <= 0:
        return ""
    try:
        from tools.tablekit.name_tables import names
        resolver = getattr(names, kind, None)
        if callable(resolver):
            return _clean_text(resolver(id_, default=""))
        return _clean_text(names.resolve(kind, id_, default=""))
    except Exception:
        return ""


def _skill_base_id(event: Mapping[str, Any]) -> int:
    skill_id = _safe_int(event.get("skill_id"), 0)
    skill_level_id = _safe_int(event.get("skill_level_id"), 0)
    if skill_id <= 0 and skill_level_id > 0:
        skill_id = skill_level_id // 100 if skill_level_id >= 100 else skill_level_id
    return max(0, skill_id)


def skill_role(skill_id: int, *, profession_id: int = 0) -> str:
    skill_id = _safe_int(skill_id, 0)
    profession_id = _safe_int(profession_id, 0)
    if skill_id <= 0:
        return ""
    if profession_id > 0:
        if skill_id == _safe_int(PROFESSION_NORMAL_ATTACK.get(profession_id), 0):
            return "normal_attack"
        if skill_id == _safe_int(PROFESSION_ULTIMATE.get(profession_id), 0):
            return "ultimate"
        if skill_id == _safe_int(PROFESSION_SKILL.get(profession_id), 0):
            return "profession_skill"
    if skill_id in set(_safe_int(v, 0) for v in PROFESSION_NORMAL_ATTACK.values()):
        return "normal_attack"
    if skill_id in set(_safe_int(v, 0) for v in PROFESSION_ULTIMATE.values()):
        return "ultimate"
    if skill_id in set(_safe_int(v, 0) for v in PROFESSION_SKILL.values()):
        return "profession_skill"
    if skill_id in SUB_PROFESSION_NAMES:
        return "sub_profession_skill"
    return "skill"


def enrich_skill_event(event: Mapping[str, Any] | None) -> dict[str, Any]:
    src = dict(event or {})
    skill_id = _skill_base_id(src)
    skill_level_id = _safe_int(src.get("skill_level_id"), 0)
    profession_id = _safe_int(src.get("profession_id") or src.get("caster_profession_id"), 0)
    if profession_id <= 0 and skill_id > 0:
        profession_id = _safe_int(_SKILL_TO_PROFESSION.get(skill_id), 0)
    name = _clean_text(src.get("skill_name") or src.get("name")) or _resolver_name("skill", skill_id)
    sub_profession = _clean_text(src.get("sub_profession")) or _clean_text(SUB_PROFESSION_NAMES.get(skill_id, ""))
    fact = {
        "kind": _clean_text(src.get("kind")),
        "skill_id": skill_id,
        "skill_level_id": skill_level_id,
        "skill_name": name,
        "display_name": name or (f"技能#{skill_id}" if skill_id > 0 else ""),
        "skill_role": skill_role(skill_id, profession_id=profession_id),
        "profession_id": profession_id,
        "sub_profession": sub_profession,
        "target_uuid": _safe_int(src.get("target_uuid"), 0),
        "caster_uid": _safe_int(src.get("caster_uid"), 0),
        "caster_uuid": _safe_int(src.get("caster_uuid"), 0),
        "source": _clean_text(src.get("source")) or "tcp",
    }
    if src.get("stage_id") is not None:
        fact["stage_id"] = _safe_int(src.get("stage_id"), 0)
    if src.get("new_stage_id") is not None:
        fact["new_stage_id"] = _safe_int(src.get("new_stage_id"), 0)
    if src.get("condition_id") is not None:
        fact["condition_id"] = _safe_int(src.get("condition_id"), 0)
    return fact


def enrich_dungeon_event(event: Mapping[str, Any] | None) -> dict[str, Any]:
    src = dict(event or {})
    dungeon_id = _safe_int(src.get("dungeon_id") or src.get("scene_uuid"), 0)
    scene_id = _safe_int(src.get("scene_id") or src.get("cur_map_id"), 0)
    lookup_id = dungeon_id or scene_id
    name = _clean_text(src.get("dungeon_name") or src.get("scene_name")) or _resolver_name("dungeon", lookup_id)
    fact = {
        "kind": _clean_text(src.get("kind")),
        "dungeon_id": dungeon_id,
        "scene_id": scene_id,
        "scene_uuid": _safe_int(src.get("scene_uuid"), 0),
        "dungeon_difficulty": _safe_int(src.get("dungeon_difficulty") or src.get("difficulty") or src.get("level_id"), 0),
        "dungeon_name": name,
        "display_name": name or (f"地牢#{lookup_id}" if lookup_id > 0 else ""),
        "flow_state": _safe_int(src.get("flow_state"), 0),
        "target_count": len(src.get("targets") or []) if isinstance(src.get("targets"), list) else 0,
        "source": _clean_text(src.get("source")) or "tcp",
    }
    for key in ("scene_guid", "connect_guid"):
        if src.get(key):
            fact[key] = _clean_text(src.get(key))
    return fact


def enrich_boss_event(event: Mapping[str, Any] | None) -> dict[str, Any]:
    src = dict(event or {})
    event_type = _safe_int(src.get("event_type"), 0)
    meta = BOSS_MECHANIC_EVENTS.get(event_type, {})
    label = _resolver_name("boss_mechanic", event_type) or _clean_text(meta.get("label")) or f"机制事件#{event_type}"
    fact = {
        "event_type": event_type,
        "boss_mechanic_key": _clean_text(meta.get("key")) or f"buff_event_{event_type}",
        "boss_mechanic_label": label,
        "trigger_family": _clean_text(meta.get("trigger_family")) or "buff_event",
        "severity": _clean_text(meta.get("severity")) or "medium",
        "host_uuid": _safe_int(src.get("host_uuid"), 0),
        "buff_uuid": _safe_int(src.get("buff_uuid"), 0),
        "buff_id": _safe_int(src.get("buff_id") or src.get("base_id"), 0),
        "source": _clean_text(src.get("source")) or "tcp",
    }
    buff_name = _clean_text(src.get("buff_name")) or _resolver_name("buff", fact["buff_id"])
    if buff_name:
        fact["buff_name"] = buff_name
    return fact


def enrich_monster_event(monster: Mapping[str, Any] | None) -> dict[str, Any]:
    src = dict(monster or {})
    template_id = _safe_int(src.get("template_id") or src.get("monster_id"), 0)
    name = _clean_text(src.get("name") or src.get("monster_name") or src.get("boss_name")) or _resolver_name("boss", template_id) or _resolver_name("monster", template_id)
    hp = max(0, _safe_int(src.get("hp"), 0))
    max_hp = max(0, _safe_int(src.get("max_hp"), 0))
    mechanics: list[str] = []
    if bool(src.get("shield_active")):
        mechanics.append("shield")
    if _safe_int(src.get("breaking_stage"), -1) >= 0 or bool(src.get("has_break_data")):
        mechanics.append("breaking")
    if bool(src.get("in_overdrive")):
        mechanics.append("overdrive")
    if bool(src.get("stop_breaking_ticking")):
        mechanics.append("break_tick_stopped")
    if bool(src.get("is_dead")) or (max_hp > 0 and hp <= 0):
        mechanics.append("dead")
    return {
        "uuid": _safe_int(src.get("uuid"), 0),
        "uid": _safe_int(src.get("uid"), 0),
        "monster_id": template_id,
        "monster_name": name,
        "boss_name": name if bool(src.get("is_boss") or src.get("boss") or src.get("boss_raid_active")) else "",
        "display_name": name or (f"怪物#{template_id}" if template_id > 0 else ""),
        "hp": hp,
        "max_hp": max_hp,
        "hp_pct": max(0.0, min(1.0, _safe_float(src.get("hp_pct"), (hp / max_hp) if max_hp > 0 else 0.0))),
        "shield_active": bool(src.get("shield_active")),
        "shield_pct": max(0.0, min(1.0, _safe_float(src.get("shield_pct"), 0.0))),
        "breaking_stage": _safe_int(src.get("breaking_stage"), -1),
        "extinction_pct": max(0.0, min(1.0, _safe_float(src.get("extinction_pct"), 0.0))),
        "in_overdrive": bool(src.get("in_overdrive")),
        "mechanics": mechanics,
        "buff_count": len(src.get("buff_list") or []) if isinstance(src.get("buff_list"), list) else 0,
    }


__all__ = [
    "BOSS_MECHANIC_EVENTS",
    "enrich_boss_event",
    "enrich_dungeon_event",
    "enrich_monster_event",
    "enrich_skill_event",
    "skill_role",
]