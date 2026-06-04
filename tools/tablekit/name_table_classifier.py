# -*- coding: utf-8 -*-
"""Semantic classification for runtime name tables.

The classifier consumes stable static tables only.  It never uses live heap
addresses and returns simple ID -> text maps for runtime resolver assets.
"""
from __future__ import annotations

import json
import os
import re
from functools import lru_cache
from typing import Any, Mapping

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO = os.path.dirname(os.path.dirname(_HERE))
_REPO = os.path.dirname(_SAO)
_ASSETS = os.path.join(_SAO, "assets")
_DATATOOLS_CN = os.path.join(_REPO, "StarResonanceDps", "DataTools", "Data", "CN")
_RESONANCE_METER = os.path.join(_REPO, "resonance-logs-cn", "src-tauri", "meter-data")
_RESONANCE_CONFIG = os.path.join(_REPO, "resonance-logs-cn", "src", "lib", "config")
_SR_WINFORM_TABLE = os.path.join(_REPO, "StarResonanceDps", "StarResonanceDpsAnalysis.WinForm", "Core", "TabelJson")
_SR_WPF_MONSTER = os.path.join(_REPO, "StarResonanceDps", "StarResonanceDpsAnalysis.WPF", "Data", "Monster")
_SR_OLD_MONSTER = os.path.join(_REPO, "StarResonanceDps", "DataTools", "Old", "Data", "monster")

SKILL_KIND = "skill"
FIELD_MARKER_KIND = "field_marker"
BOSS_SKILL_KIND = "boss_skill"
BUFF_KIND = "buff"
PLAYER_BUFF_KIND = "player_buff"
FACTOR_BUFF_KIND = "factor_buff"
EVENT_KIND = "event"
BOSS_STATUS_KIND = "boss_status"
BOSS_MECHANIC_KIND = "boss_mechanic"

_FIELD_MARKER_RE = re.compile(r"(场地标记|场地|地面|范围标记)")
_EVENT_RE = re.compile(r"(事件|Event|event|触发事件|剧情|玩法事件|任务事件)")
_BOSS_STATUS_RE = re.compile(r"(主体死亡|部位死亡|部位状态变化|护盾破裂|霸体破裂|进入破防|进入碎裂状态)")


def _load_json(path: str) -> Any:
    if not path or not os.path.isfile(path):
        return {}
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def _iter_entries(obj: Any):
    if isinstance(obj, Mapping):
        yield from obj.items()
    elif isinstance(obj, list):
        for item in obj:
            if isinstance(item, Mapping):
                yield item.get("Id") or item.get("id") or item.get("ID"), item


def _safe_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        return int(default)


def _clean_text(value: Any) -> str:
    return str(value or "").strip()


def _entry_name(value: Any, *, fields: tuple[str, ...] = ("Name", "NameDesign", "name", "text")) -> str:
    if isinstance(value, str):
        return _clean_text(value)
    if isinstance(value, Mapping):
        for field in fields:
            text = _clean_text(value.get(field))
            if text:
                return text
    return ""


def _plain_names(obj: Any, *, fields: tuple[str, ...] = ("Name", "NameDesign", "name", "text")) -> dict[int, str]:
    out: dict[int, str] = {}
    for key, value in _iter_entries(obj):
        iid = _safe_int(key)
        if iid <= 0:
            continue
        name = _entry_name(value, fields=fields)
        if name:
            out[iid] = name
    return out


@lru_cache(maxsize=1)
def skill_table() -> dict[int, Mapping[str, Any]]:
    data = _load_json(os.path.join(_DATATOOLS_CN, "SkillTable.json"))
    return {int(k): v for k, v in (data.items() if isinstance(data, Mapping) else []) if str(k).isdigit() and isinstance(v, Mapping)}


@lru_cache(maxsize=1)
def buff_table() -> dict[int, Mapping[str, Any]]:
    data = _load_json(os.path.join(_DATATOOLS_CN, "BuffTable.json"))
    return {int(k): v for k, v in (data.items() if isinstance(data, Mapping) else []) if str(k).isdigit() and isinstance(v, Mapping)}


@lru_cache(maxsize=1)
def monster_table() -> dict[int, Mapping[str, Any]]:
    data = _load_json(os.path.join(_DATATOOLS_CN, "MonsterTable.json"))
    return {int(k): v for k, v in (data.items() if isinstance(data, Mapping) else []) if str(k).isdigit() and isinstance(v, Mapping)}


@lru_cache(maxsize=1)
def skill_fallback_names() -> dict[int, str]:
    names: dict[int, str] = {}
    for path in (
        os.path.join(_ASSETS, "skill_names.json"),
    ):
        names.update(_plain_names(_load_json(path), fields=("Name", "NameDesign", "name", "text")))
    return names


@lru_cache(maxsize=1)
def buff_fallback_names() -> dict[int, str]:
    return _plain_names(_load_json(os.path.join(_RESONANCE_CONFIG, "BuffName.json")), fields=("NameDesign", "Name", "name", "text"))


@lru_cache(maxsize=1)
def monster_names() -> dict[int, str]:
    names: dict[int, str] = {}
    for path in (
        os.path.join(_DATATOOLS_CN, "MonsterTable.json"),
        os.path.join(_SR_WINFORM_TABLE, "monster_names.json"),
        os.path.join(_SR_WPF_MONSTER, "monster.zh-CN.json"),
        os.path.join(_SR_OLD_MONSTER, "monster_name_mapping.json"),
    ):
        names.update({k: v for k, v in _plain_names(_load_json(path)).items() if k not in names})
    return names


@lru_cache(maxsize=1)
def boss_monster_ids() -> set[int]:
    out = {mid for mid, row in monster_table().items() if _safe_int(row.get("MonsterType"), -1) == 2}
    for path in (
        os.path.join(_RESONANCE_METER, "MonsterIdNameType.json"),
        os.path.join(_RESONANCE_CONFIG, "MonsterIdNameType.json"),
    ):
        data = _load_json(path)
        if not isinstance(data, Mapping):
            continue
        for key, value in data.items():
            if _safe_int(value, -1) == 2:
                out.add(_safe_int(key))
    return {mid for mid in out if mid > 0}


@lru_cache(maxsize=1)
def boss_skill_ids() -> set[int]:
    ids: set[int] = set()
    bosses = boss_monster_ids()
    for monster_id, row in monster_table().items():
        if monster_id not in bosses and _safe_int(row.get("MonsterType"), -1) != 2:
            continue
        for raw in row.get("SkillIds") or []:
            sid = _safe_int(raw)
            if sid > 0:
                ids.add(sid)
    return ids


def classify_skill_id(skill_id: Any) -> str:
    sid = _safe_int(skill_id)
    if sid <= 0:
        return ""
    row = skill_table().get(sid) or {}
    name = _entry_name(row) or skill_fallback_names().get(sid, "")
    if row and _safe_int(row.get("SkillType"), -1) == 7:
        return FIELD_MARKER_KIND
    if _FIELD_MARKER_RE.search(name):
        return FIELD_MARKER_KIND
    if sid in boss_skill_ids():
        return BOSS_SKILL_KIND
    return SKILL_KIND


def _buff_name(buff_id: int) -> str:
    row = buff_table().get(buff_id) or {}
    name = _entry_name(row, fields=("NameDesign", "Name", "name", "text"))
    if name:
        return name
    if buff_id in buff_fallback_names():
        return buff_fallback_names()[buff_id]
    return ""


def _truthy(value: Any) -> bool:
    if value in (None, "", 0, "0", False):
        return False
    return True


def classify_buff_id(buff_id: Any) -> str:
    bid = _safe_int(buff_id)
    if bid <= 0:
        return ""
    name = _buff_name(bid)
    row = buff_table().get(bid) or {}
    if _BOSS_STATUS_RE.search(name):
        return BOSS_STATUS_KIND
    if _EVENT_RE.search(name):
        return EVENT_KIND
    if not row:
        return BUFF_KIND
    visible = _safe_int(row.get("Visible"), 0)
    is_client = bool(row.get("IsClientBuff"))
    show_hud = _truthy(row.get("ShowHUDIcon")) or _truthy(row.get("TipsDescription"))
    buff_ability_type = _safe_int(row.get("BuffAbilityType"), 0)
    buff_ability_subtype = _safe_int(row.get("BuffAbilitySubType"), 0)
    tags = row.get("Tags") or []
    special_attr = row.get("SpecialAttr") or []
    if visible != 0 or show_hud:
        return PLAYER_BUFF_KIND
    if is_client or visible == 0 or buff_ability_type or buff_ability_subtype or tags or special_attr:
        return FACTOR_BUFF_KIND
    return BUFF_KIND


def _skill_name(skill_id: int) -> str:
    row = skill_table().get(skill_id) or {}
    return _entry_name(row) or skill_fallback_names().get(skill_id, "")


def _skill_maps() -> dict[str, dict[int, str]]:
    out = {SKILL_KIND: {}, FIELD_MARKER_KIND: {}, BOSS_SKILL_KIND: {}}
    ids = set(skill_table()) | set(skill_fallback_names()) | boss_skill_ids()
    for sid in sorted(ids):
        name = _skill_name(sid)
        if not name:
            continue
        kind = classify_skill_id(sid)
        if kind in out:
            out[kind][sid] = name
    return out


def _buff_maps() -> dict[str, dict[int, str]]:
    out = {BUFF_KIND: {}, PLAYER_BUFF_KIND: {}, FACTOR_BUFF_KIND: {}, EVENT_KIND: {}, BOSS_STATUS_KIND: {}}
    ids = set(buff_table())
    ids.update(buff_fallback_names())
    for bid in sorted(ids):
        name = _buff_name(bid)
        if not name:
            continue
        kind = classify_buff_id(bid)
        if kind in {EVENT_KIND, BOSS_STATUS_KIND}:
            out[kind][bid] = name
            continue
        if kind == PLAYER_BUFF_KIND:
            out[PLAYER_BUFF_KIND][bid] = name
        elif kind == FACTOR_BUFF_KIND:
            out[FACTOR_BUFF_KIND][bid] = name
        else:
            out[BUFF_KIND][bid] = name
        if kind in {PLAYER_BUFF_KIND, FACTOR_BUFF_KIND, BUFF_KIND}:
            out[BUFF_KIND][bid] = name
    return out


def _boss_mechanic_map() -> dict[int, str]:
    try:
        from tools.tablekit.combat_preparse import BOSS_MECHANIC_EVENTS
    except Exception:
        return {}
    return {int(event_type): _clean_text((meta or {}).get("label")) for event_type, meta in BOSS_MECHANIC_EVENTS.items() if _clean_text((meta or {}).get("label"))}


def _boss_map() -> dict[int, str]:
    names = monster_names()
    return {mid: names[mid] for mid in sorted(boss_monster_ids()) if mid in names}


def _to_runtime(entries: Mapping[int, str]) -> dict[str, dict[str, str]]:
    return {str(iid): {"text": text, "confidence": "static"} for iid, text in sorted(entries.items()) if text}


@lru_cache(maxsize=1)
def load_classified_tables() -> dict[str, dict[str, dict[str, str]]]:
    skills = _skill_maps()
    buffs = _buff_maps()
    mechanics = _boss_mechanic_map()
    result: dict[str, dict[str, dict[str, str]]] = {
        SKILL_KIND: _to_runtime(skills[SKILL_KIND]),
        FIELD_MARKER_KIND: _to_runtime(skills[FIELD_MARKER_KIND]),
        BOSS_SKILL_KIND: _to_runtime(skills[BOSS_SKILL_KIND]),
        BUFF_KIND: _to_runtime(buffs[BUFF_KIND]),
        PLAYER_BUFF_KIND: _to_runtime(buffs[PLAYER_BUFF_KIND]),
        FACTOR_BUFF_KIND: _to_runtime(buffs[FACTOR_BUFF_KIND]),
        EVENT_KIND: _to_runtime(buffs[EVENT_KIND]),
        BOSS_STATUS_KIND: _to_runtime({**mechanics, **buffs[BOSS_STATUS_KIND]}),
        BOSS_MECHANIC_KIND: _to_runtime(mechanics),
        "boss": _to_runtime(_boss_map()),
    }
    return result


def classify_id(kind: str, id_: Any) -> str:
    if kind in {"skill", "skill_id", "skill_id_or_legacy_skill_name_index", "sub_profession_skill_id"}:
        return classify_skill_id(id_)
    if kind in {"buff", "buff_id"}:
        return classify_buff_id(id_)
    return str(kind or "")


def coverage() -> dict[str, int]:
    return {kind: len(table) for kind, table in sorted(load_classified_tables().items())}


if __name__ == "__main__":
    print(json.dumps(coverage(), ensure_ascii=False, indent=2, sort_keys=True))
