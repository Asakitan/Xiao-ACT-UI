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
    "skill":   [(_EXTRACTED, "skill.json"),   (_DATATOOLS_CN, "SkillTable.json"),  (_ASSETS, "skill_names.json")],
    "monster": [
        (_EXTRACTED, "monster.json"),
        (_DATATOOLS_CN, "MonsterTable.json"),
        (_SRD_WPF_MONSTER, "monster.zh-CN.json"),
        (_SRD_WINFORM_TABLE, "monster_names.json"),
        (_DATATOOLS_OLD_MONSTER, "monster_name_mapping.json"),
    ],
    "boss": [(_EXTRACTED, "boss.json")],
    "buff":    [(_EXTRACTED, "buff.json"),    (_DATATOOLS_CN, "BuffTable.json"),   (_ASSETS, "skill_names.json")],
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
    "skill": "技能", "monster": "怪物", "boss": "Boss", "buff": "Buff",
    "dungeon": "地牢", "boss_mechanic": "机制", "item": "道具", "npc": "NPC",
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
        name = self._load_kind(kind).get(iid)
        if name:
            return name
        if default is not None:
            return default
        return "%s#%d" % (_FALLBACK_PREFIX.get(kind, kind), iid)

    def skill(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("skill", id_, default)

    def monster(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("monster", id_, default)

    def boss(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("boss", id_, default)

    def buff(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("buff", id_, default)

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
