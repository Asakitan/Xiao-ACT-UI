"""name_tables - 统一 "ID -> 中文名" 解析器.

所有显示层(TCP 解析 / 面板)都经此把数字 ID 变中文名, 杜绝裸数字。

数据源(按优先级 高->低, 高覆盖低):
  1. assets/name_tables/<kind>.json   ← Bokura 解码器抽出的权威表(最准最全), {id: name}
  2. StarResonanceDps/DataTools/Data/CN/<Kind>Table.json  ← 社区表(可能不全), {id: {Name:..}}
  3. assets/skill_names.json          ← 现有部分技能名, {id: name}

kind: skill / monster / buff / dungeon / npc / item ...

用法:
    from tools.tablekit.name_tables import names
    names.skill(1101)            -> "场地标记01"  (或 "技能#1101" 兜底)
    names.resolve("dungeon", id) -> 中文名 或 兜底
    names.coverage()             -> {kind: count}
"""
from __future__ import annotations

import json
import os
from typing import Dict, Optional

_HERE = os.path.dirname(os.path.abspath(__file__))
# sao_auto/tools/tablekit -> sao_auto
_SAO = os.path.dirname(os.path.dirname(_HERE))
# 仓库根 (含 StarResonanceDps)
_REPO = os.path.dirname(_SAO)

_ASSETS = os.path.join(_SAO, "assets")
_EXTRACTED = os.path.join(_ASSETS, "name_tables")          # 解码器输出
_DATATOOLS_CN = os.path.join(_REPO, "StarResonanceDps", "DataTools", "Data", "CN")
_SKILL_NAMES = os.path.join(_ASSETS, "skill_names.json")

# 每个 kind 的数据源 (高优先级在前)
_SOURCES = {
    "skill":   [(_EXTRACTED, "skill.json"),   (_DATATOOLS_CN, "SkillTable.json"),  (_ASSETS, "skill_names.json")],
    "monster": [(_EXTRACTED, "monster.json"), (_DATATOOLS_CN, "MonsterTable.json")],
    "buff":    [(_EXTRACTED, "buff.json"),    (_DATATOOLS_CN, "BuffTable.json"),   (_ASSETS, "skill_names.json")],
    "dungeon": [(_EXTRACTED, "dungeon.json"), (_DATATOOLS_CN, "DungeonTable.json")],
    "item":    [(_EXTRACTED, "item.json"),    (_DATATOOLS_CN, "ItemTable.json")],
    "npc":     [(_EXTRACTED, "npc.json"),     (_DATATOOLS_CN, "NpcTable.json")],
}

# 兜底前缀 (找不到名字时显示 "<前缀>#<id>")
_FALLBACK_PREFIX = {
    "skill": "技能", "monster": "怪物", "buff": "Buff",
    "dungeon": "地牢", "item": "道具", "npc": "NPC",
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


class NameResolver:
    def __init__(self):
        self._tables: Dict[str, Dict[int, str]] = {}

    def _load_kind(self, kind: str) -> Dict[int, str]:
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

    def buff(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("buff", id_, default)

    def dungeon(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("dungeon", id_, default)

    def has(self, kind: str, id_: object) -> bool:
        try:
            return int(id_) in self._load_kind(kind)
        except (TypeError, ValueError):
            return False

    def coverage(self) -> Dict[str, int]:
        return {k: len(self._load_kind(k)) for k in _SOURCES}

    def reload(self):
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
