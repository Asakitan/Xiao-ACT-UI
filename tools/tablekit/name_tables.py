"""name_tables - 统一 "ID -> 中文名" 解析器.

所有显示层(TCP 解析 / 面板)都经此把数字 ID 变中文名, 杜绝裸数字。

数据源(按优先级 高->低, 高覆盖低):
    1. assets/name_tables/<kind>.json   ← 轻量稳定 TCP/MEM/解析名字表, {id: name}
    2. assets/name_tables/tcp_preparse_name_cache.json ← 运行时 compact TCP/MEM cache
    3. StarResonanceDps/resonance-logs-cn 社区表(可能不全), {id: {Name:..}}
    4. assets/skill_names.json          ← 现有部分技能名 fallback, {id: name}

kind: skill / monster / boss / buff / dungeon / boss_mechanic / npc / item ...

用法:
    from tools.tablekit.name_tables import names
    names.skill(1101)            -> "场地标记01"  (或 "技能#1101" 兜底)
    names.resolve("dungeon", id) -> 中文名 或 兜底
    names.coverage()             -> {kind: count}
"""
from __future__ import annotations

import json
import os
import threading
from typing import Dict, Optional

_HERE = os.path.dirname(os.path.abspath(__file__))
# sao_auto/tools/tablekit -> sao_auto
_SAO = os.path.dirname(os.path.dirname(_HERE))
# 仓库根 (含 StarResonanceDps)
_REPO = os.path.dirname(_SAO)

_ASSETS = os.path.join(_SAO, "assets")
_EXTRACTED = os.path.join(_ASSETS, "name_tables")          # 解码器输出
_DATATOOLS_CN = os.path.join(_REPO, "StarResonanceDps", "DataTools", "Data", "CN")
_SRD_WPF_MONSTER = os.path.join(_REPO, "StarResonanceDps", "StarResonanceDpsAnalysis.WPF", "Data", "Monster")
_SRD_WINFORM_TABLE = os.path.join(_REPO, "StarResonanceDps", "StarResonanceDpsAnalysis.WinForm", "Core", "TabelJson")
_DATATOOLS_OLD_MONSTER = os.path.join(_REPO, "StarResonanceDps", "DataTools", "Old", "Data", "monster")
_RESONANCE_LOGS_CONFIG = os.path.join(_REPO, "resonance-logs-cn", "src", "lib", "config")
_RESONANCE_LOGS_METER_DATA = os.path.join(_REPO, "resonance-logs-cn", "src-tauri", "meter-data")
_SKILL_NAMES = os.path.join(_ASSETS, "skill_names.json")
_TCP_PREPARSE_CACHE = os.path.join(_EXTRACTED, "tcp_preparse_name_cache.json")

# 每个 kind 的数据源 (高优先级在前)
_SOURCES = {
    "skill":   [(_EXTRACTED, "skill.json")],
    "field_marker": [(_EXTRACTED, "field_marker.json")],
    "boss_skill": [(_EXTRACTED, "boss_skill.json")],
    "ultimate_skill": [(_EXTRACTED, "ultimate_skill.json")],
    "roguelike_affix": [(_EXTRACTED, "roguelike_affix.json")],
    "profession_skill": [(_EXTRACTED, "profession_skill.json")],
    "scripted_skill": [(_EXTRACTED, "scripted_skill.json")],
    "virtual_skill": [(_EXTRACTED, "virtual_skill.json")],
    "boss_mechanic_skill": [(_EXTRACTED, "boss_mechanic_skill.json")],
    "monster": [
        (_EXTRACTED, "monster.json"),
        (_DATATOOLS_CN, "MonsterTable.json"),
        (_SRD_WPF_MONSTER, "monster.zh-CN.json"),
        (_SRD_WINFORM_TABLE, "monster_names.json"),
        (_DATATOOLS_OLD_MONSTER, "monster_name_mapping.json"),
    ],
    "boss": [(_EXTRACTED, "boss.json")],
    "buff":    [(_EXTRACTED, "buff.json")],
    "player_buff": [(_EXTRACTED, "player_buff.json")],
    "factor_buff": [(_EXTRACTED, "factor_buff.json")],
    "profession_skill_buff": [(_EXTRACTED, "profession_skill_buff.json")],
    "event": [(_EXTRACTED, "event.json")],
    "dungeon": [
        (_EXTRACTED, "dungeon.json"),
        (_DATATOOLS_CN, "DungeonTable.json"),
        (_RESONANCE_LOGS_METER_DATA, "SceneName.json"),
        (_RESONANCE_LOGS_CONFIG, "SceneName.json"),
    ],
    "boss_mechanic": [(_EXTRACTED, "boss_mechanic.json")],
    "item":    [(_EXTRACTED, "item.json"),    (_DATATOOLS_CN, "ItemTable.json")],
    "npc":     [(_EXTRACTED, "npc.json"),     (_DATATOOLS_CN, "NpcTable.json")],
}

# 兜底前缀 (找不到名字时显示 "<前缀>#<id>")
_FALLBACK_PREFIX = {
    "skill": "技能", "field_marker": "场地标记", "boss_skill": "Boss技能",
    "ultimate_skill": "幻想技能", "roguelike_affix": "肉鸽词条", "profession_skill": "职业技能",
    "scripted_skill": "剧情表演", "virtual_skill": "虚拟体技能", "boss_mechanic_skill": "Boss机制技能",
    "monster": "怪物", "boss": "Boss", "buff": "Buff", "player_buff": "玩家Buff",
    "factor_buff": "因子Buff", "profession_skill_buff": "职业技能Buff", "event": "事件",
    "dungeon": "地牢", "boss_mechanic": "机制", "item": "道具", "npc": "NPC",
}

_COMPAT_KIND_FALLBACKS = {
    "skill": (
        "ultimate_skill", "profession_skill", "boss_mechanic_skill", "boss_skill",
        "scripted_skill", "virtual_skill", "field_marker", "roguelike_affix",
    ),
    "buff": ("profession_skill_buff", "player_buff", "factor_buff"),
}

_LIVE_ID_SPACE_KIND = {
    "skill_id": "skill",
    "skill_id_or_legacy_skill_name_index": "skill",
    "sub_profession_skill_id": "skill",
    "buff_id": "buff",
    "monster_id": "monster",
    "boss_id": "boss",
    "dungeon_id": "dungeon",
    "scene_id": "dungeon",
    "boss_event_type": "boss_mechanic",
    "npc_id": "npc",
    "item_id": "item",
}


def _semantic_kind(kind: str, id_: object) -> str:
    try:
        from tools.tablekit.name_table_classifier import classify_id
        classified = classify_id(kind, id_)
        return classified or kind
    except Exception:
        return kind


def _coerce_table(obj) -> Dict[int, str]:
    """把 {id:name} 或 {id:{Name:..}} 统一成 {int_id: str_name}."""
    out: Dict[int, str] = {}
    if not isinstance(obj, dict):
        return out
    for k, v in obj.items():
        try:
            kid = int(k)
        except (TypeError, ValueError):
            continue
        name = None
        if isinstance(v, str):
            name = v
        elif isinstance(v, dict):
            # 优先 Name, 退 NameDesign
            name = v.get("Name") or v.get("name") or v.get("NameDesign")
        if name:
            name = str(name).strip()
            if name:
                out[kid] = name
    return out


def _load_tcp_preparse_cache(kind: str) -> Dict[int, str]:
    out: Dict[int, str] = {}
    if kind not in _SOURCES:
        return out
    if not os.path.isfile(_TCP_PREPARSE_CACHE):
        return out
    try:
        with open(_TCP_PREPARSE_CACHE, "r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception:
        return out
    names = data.get("names") if isinstance(data, dict) else {}
    by_kind = names.get("by_kind") if isinstance(names, dict) else {}
    bucket = by_kind.get(kind) if isinstance(by_kind, dict) else {}
    if not isinstance(bucket, dict):
        return out
    for key, entry in bucket.items():
        try:
            iid = int(key)
        except (TypeError, ValueError):
            continue
        text = ""
        if isinstance(entry, str):
            text = entry
        elif isinstance(entry, dict):
            text = str(entry.get("text") or entry.get("name") or "")
        text = text.strip()
        if text:
            out.setdefault(iid, text)
    return out


class NameResolver:
    def __init__(self):
        self._tables: Dict[str, Dict[int, str]] = {}
        self._lock = threading.RLock()

    def _load_kind(self, kind: str) -> Dict[int, str]:
        with self._lock:
            if kind in self._tables:
                return self._tables[kind]
            merged: Dict[int, str] = {}
            # 低优先级先填, 高优先级后覆盖
            for folder, fname in reversed(_SOURCES.get(kind, [])):
                path = os.path.join(folder, fname)
                if not os.path.isfile(path):
                    continue
                try:
                    with open(path, "r", encoding="utf-8") as f:
                        data = json.load(f)
                    merged.update(_coerce_table(data))
                except Exception:
                    pass
            merged.update(_load_tcp_preparse_cache(kind))
            self._tables[kind] = merged
            return merged

    def resolve(self, kind: str, id_: object, default: Optional[str] = None) -> str:
        try:
            iid = int(id_)
        except (TypeError, ValueError):
            return default if default is not None else str(id_)
        semantic_kind = _semantic_kind(kind, iid)
        if semantic_kind != kind and semantic_kind in _SOURCES:
            name = self._load_kind(semantic_kind).get(iid)
            if name:
                return name
        name = self._load_kind(kind).get(iid)
        if name:
            return name
        for compat_kind in _COMPAT_KIND_FALLBACKS.get(kind, ()):
            name = self._load_kind(compat_kind).get(iid)
            if name:
                return name
        if default is not None:
            return default
        return "%s#%d" % (_FALLBACK_PREFIX.get(kind, kind), iid)

    def skill(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("skill", id_, default)

    def field_marker(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("field_marker", id_, default)

    def boss_skill(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("boss_skill", id_, default)

    def ultimate_skill(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("ultimate_skill", id_, default)

    def roguelike_affix(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("roguelike_affix", id_, default)

    def profession_skill(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("profession_skill", id_, default)

    def scripted_skill(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("scripted_skill", id_, default)

    def virtual_skill(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("virtual_skill", id_, default)

    def boss_mechanic_skill(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("boss_mechanic_skill", id_, default)

    def monster(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("monster", id_, default)

    def boss(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("boss", id_, default)

    def buff(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("buff", id_, default)

    def player_buff(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("player_buff", id_, default)

    def factor_buff(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("factor_buff", id_, default)

    def profession_skill_buff(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("profession_skill_buff", id_, default)

    def event(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("event", id_, default)

    def boss_status(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("boss_mechanic", id_, default)

    def dungeon(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("dungeon", id_, default)

    def boss_mechanic(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("boss_mechanic", id_, default)

    def has(self, kind: str, id_: object) -> bool:
        try:
            return int(id_) in self._load_kind(kind)
        except (TypeError, ValueError):
            return False

    def coverage(self) -> Dict[str, int]:
        return {k: len(self._load_kind(k)) for k in _SOURCES}

    def reload(self):
        with self._lock:
            self._tables.clear()


# 模块级单例
names = NameResolver()


if __name__ == "__main__":
    print("== name_tables coverage ==")
    for kind, cnt in names.coverage().items():
        print("  %-8s %6d entries" % (kind, cnt))
    print("\n== samples ==")
    print("  skill 1101  ->", names.skill(1101))
    print("  monster 101 ->", names.monster(101))
    print("  dungeon 1   ->", names.dungeon(1))
