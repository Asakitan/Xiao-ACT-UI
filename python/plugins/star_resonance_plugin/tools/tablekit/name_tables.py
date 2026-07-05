# name_tables - 统一 "ID -> 中文名" 解析器.
#
# 所有显示层(TCP 解析 / 面板)都经此把数字 ID 变中文名, 杜绝裸数字。
#
# 数据源(运行时合并, 优先级 高->低, 高覆盖低):
# 1. assets/name_tables/tcp_preparse_name_cache(.local).json
# ← 我们从游戏内存/TCP 实测解析出的名字(权威): text=内存真名, id=指针表/锚定匹配。
# 运行时最后合并, 故覆盖 <kind>.json。
# 2. assets/name_tables/<kind>.json, {id: name}
# ← 已固化的稳定名字表。由 tools.tablekit.hybrid_name_tables 生成: 以上面的解析
# 结果(cache)为权威, 隔壁 StarResonanceDps/resonance-logs-cn 静态表仅兜底缺失 id。
# 注意: hybrid --overlay-only 把 cache 安全叠加到本表(不触发分类器重建)。
# 3. 生成期参考: StarResonanceDps/resonance-logs-cn 静态表(隔壁工程, 可能过时/不全)。
#
# 注意: ``skill`` 是兼容入口, 只做语义分流和兜底标签, 不再对应 runtime
# ``skill.json`` 资产。所有技能 ID 必须进入更明确的 *_skill 分类。
#
# kind: skill / monster / boss / buff / dungeon / boss_mechanic / npc / item ...
#
# 用法:
# from tools.tablekit.name_tables import names
# names.skill(1101)            -> "场地标记01"  (或 "技能#1101" 兜底)
# names.resolve("dungeon", id) -> 中文名 或 兜底
# names.coverage()             -> {kind: count}
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


def _resolve_assets_dir() -> str:
    # assets/ 根目录解析 (onedir 友好)。
    #
    # 冻结 onedir 下 assets/ 被 build_release.bat 提升到 BASE_DIR(exe 顶层), 而本模块
    # __file__ 在 runtime/tools/tablekit/ → _SAO/assets=runtime/assets 已被搬空, 所有
    # name_table json 读不到 → names 全部回退 "技能#<id>" 占位。优先 config.resource_path
    # (BASE_DIR 优先, BUNDLE_DIR 回退), 找不到再回退 __file__ 相对(dev 树/未冻结)。
    try:
        from config import resource_path  # BASE_DIR-first, BUNDLE_DIR fallback
        cand = resource_path("assets")
        if os.path.isdir(cand):
            return cand
    except Exception:
        pass
    return os.path.join(_SAO, "assets")


_ASSETS = _resolve_assets_dir()
_EXTRACTED = os.path.join(_ASSETS, "name_tables")          # 解码器输出
_DATATOOLS_CN = os.path.join(_REPO, "StarResonanceDps", "DataTools", "Data", "CN")
_SRD_WPF_MONSTER = os.path.join(_REPO, "StarResonanceDps", "StarResonanceDpsAnalysis.WPF", "Data", "Monster")
_SRD_WINFORM_TABLE = os.path.join(_REPO, "StarResonanceDps", "StarResonanceDpsAnalysis.WinForm", "Core", "TabelJson")
_DATATOOLS_OLD_MONSTER = os.path.join(_REPO, "StarResonanceDps", "DataTools", "Old", "Data", "monster")
_RESONANCE_LOGS_CONFIG = os.path.join(_REPO, "resonance-logs-cn", "src", "lib", "config")
_RESONANCE_LOGS_METER_DATA = os.path.join(_REPO, "resonance-logs-cn", "src-tauri", "meter-data")
_TCP_PREPARSE_CACHE = os.path.join(_EXTRACTED, "tcp_preparse_name_cache.json")
_TCP_PREPARSE_LOCAL_CACHE = os.environ.get(
    "SAO_TCP_PREPARSE_NAME_CACHE_LOCAL",
    os.path.join(_EXTRACTED, "tcp_preparse_name_cache.local.json"),
)
_TCP_PREPARSE_CACHE_LOCK = threading.RLock()
_TCP_PREPARSE_CACHE_BY_KIND: Dict[
    str,
    tuple[tuple[tuple[str, Optional[int], Optional[int]], ...], Dict[int, str]],
] = {}

# 每个 kind 的数据源 (高优先级在前)
_SOURCES = {
    "player_skill": [(_EXTRACTED, "player_skill.json")],
    "monster_skill": [(_EXTRACTED, "monster_skill.json")],
    "environment_skill": [(_EXTRACTED, "environment_skill.json")],
    "field_marker": [(_EXTRACTED, "field_marker.json")],
    "boss_skill": [(_EXTRACTED, "boss_skill.json")],
    "ultimate_skill": [(_EXTRACTED, "ultimate_skill.json")],
    "roguelike_affix": [(_EXTRACTED, "roguelike_affix.json")],
    "profession_skill": [(_EXTRACTED, "profession_skill.json")],
    "scripted_skill": [(_EXTRACTED, "scripted_skill.json")],
    "virtual_skill": [(_EXTRACTED, "virtual_skill.json")],
    "boss_mechanic_skill": [(_EXTRACTED, "boss_mechanic_skill.json")],
    "client_effect_skill": [(_EXTRACTED, "client_effect_skill.json")],
    "interaction_skill": [(_EXTRACTED, "interaction_skill.json")],
    "companion_skill": [(_EXTRACTED, "companion_skill.json")],
    "projectile_skill": [(_EXTRACTED, "projectile_skill.json")],
    "passive_skill": [(_EXTRACTED, "passive_skill.json")],
    "test_skill": [(_EXTRACTED, "test_skill.json")],
    "system_skill": [(_EXTRACTED, "system_skill.json")],
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
    "skill": "技能", "player_skill": "玩家技能", "monster_skill": "怪物技能",
    "environment_skill": "环境技能", "field_marker": "场地标记", "boss_skill": "Boss技能",
    "ultimate_skill": "幻想技能", "roguelike_affix": "肉鸽词条", "profession_skill": "职业技能",
    "scripted_skill": "剧情表演", "virtual_skill": "虚拟体技能", "boss_mechanic_skill": "Boss机制技能",
    "client_effect_skill": "客户端表现技能", "interaction_skill": "交互玩法技能", "companion_skill": "伙伴技能",
    "projectile_skill": "投射物技能", "passive_skill": "被动/修饰技能", "test_skill": "测试技能", "system_skill": "系统技能",
    "monster": "怪物", "boss": "Boss", "buff": "Buff", "player_buff": "玩家Buff",
    "factor_buff": "因子Buff", "profession_skill_buff": "职业技能Buff", "event": "事件",
    "dungeon": "地牢", "boss_mechanic": "机制", "item": "道具", "npc": "NPC",
}

_COMPAT_KIND_FALLBACKS = {
    "skill": (
        "ultimate_skill", "profession_skill", "player_skill", "environment_skill",
        "boss_mechanic_skill", "boss_skill", "monster_skill",
        "scripted_skill", "virtual_skill", "field_marker", "roguelike_affix",
        "companion_skill", "projectile_skill", "interaction_skill", "client_effect_skill",
        "passive_skill", "system_skill", "test_skill",
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
    # 把 {id:name} 或 {id:{Name:..}} 统一成 {int_id: str_name}.
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


# 离线全表枚举的静态缓存 (tools/build_name_cache.py 产出): 最低优先级基线, 把
# curated/tcp 名字表没覆盖的 id 全填上 (skill/buff/monster 各数千条)。
_STATIC_CACHE_PATH = os.path.join(_EXTRACTED, "static_id_name_cache.json")
# 各 kind → 静态缓存里的 section (skill 家族都走 skill 段, boss/怪走 monster 段)
_STATIC_KIND_SECTION = {
    "skill": "skill", "player_skill": "skill", "monster_skill": "skill",
    "environment_skill": "skill", "boss_skill": "skill", "ultimate_skill": "skill",
    "profession_skill": "skill", "scripted_skill": "skill", "virtual_skill": "skill",
    "boss_mechanic_skill": "skill", "field_marker": "skill",
    "buff": "buff", "monster": "monster", "boss": "monster",
}
_STATIC_CACHE_BY_SECTION: Dict[str, Dict[int, str]] = {}
_STATIC_CACHE_LOADED = False


def _load_static_cache(kind: str) -> Dict[int, str]:
    # 静态全表缓存里该 kind 段的 {id:name} (一次性加载, 最低优先级基线)。
    global _STATIC_CACHE_LOADED
    section = _STATIC_KIND_SECTION.get(kind)
    if section is None:
        return {}
    if not _STATIC_CACHE_LOADED:
        _STATIC_CACHE_LOADED = True
        try:
            # 用 json.loads(read) 而非 json.load(file): 与 tcp-preparse 测试对 json.load
            # 的调用计数解耦 (静态缓存是固定基线, 不参与那条缓存复用断言)。
            with open(_STATIC_CACHE_PATH, "r", encoding="utf-8") as f:
                data = json.loads(f.read())
            for sec in ("skill", "buff", "monster"):
                _STATIC_CACHE_BY_SECTION[sec] = _coerce_table(data.get(sec) or {})
        except Exception:
            pass
    return _STATIC_CACHE_BY_SECTION.get(section, {})


def _tcp_preparse_cache_paths() -> tuple[str, ...]:
    paths = [_TCP_PREPARSE_CACHE]
    if _TCP_PREPARSE_LOCAL_CACHE and _TCP_PREPARSE_LOCAL_CACHE not in paths:
        paths.append(_TCP_PREPARSE_LOCAL_CACHE)
    return tuple(paths)


def _tcp_preparse_cache_signature() -> tuple[tuple[str, Optional[int], Optional[int]], ...]:
    signature = []
    for path in _tcp_preparse_cache_paths():
        try:
            st = os.stat(path)
        except OSError:
            signature.append((path, None, None))
            continue
        signature.append((path, int(st.st_mtime_ns), int(st.st_size)))
    return tuple(signature)


def _load_tcp_preparse_cache(kind: str) -> Dict[int, str]:
    if kind not in _SOURCES and kind != "skill":
        return {}
    signature = _tcp_preparse_cache_signature()
    with _TCP_PREPARSE_CACHE_LOCK:
        cached = _TCP_PREPARSE_CACHE_BY_KIND.get(kind)
        if cached and cached[0] == signature:
            return cached[1]

    out: Dict[int, str] = {}
    for path, mtime_ns, _size in signature:
        if mtime_ns is None:
            continue
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
        except Exception:
            continue
        names = data.get("names") if isinstance(data, dict) else {}
        by_kind = names.get("by_kind") if isinstance(names, dict) else {}
        bucket = by_kind.get(kind) if isinstance(by_kind, dict) else {}
        if not isinstance(bucket, dict):
            continue
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
                out[iid] = text
    with _TCP_PREPARSE_CACHE_LOCK:
        _TCP_PREPARSE_CACHE_BY_KIND[kind] = (signature, out)
    return out


def _load_live_act_matches(kind: str) -> Dict[int, str]:
    # Backward-compatible view of live-assisted stable name matches.
    #
    # Historical ACT replay checks imported this private helper directly.  The
    # volatile ``live_probe_act_matched_rows.json`` dump is intentionally local;
    # the reusable result now lives in the stable per-kind tables plus sanitized
    # TCP preparse cache.
    return NameResolver().kind_map(kind)


class NameResolver:
    def __init__(self):
        self._tables: Dict[str, Dict[int, str]] = {}
        self._lock = threading.RLock()

    def _load_kind(self, kind: str) -> Dict[int, str]:
        with self._lock:
            if kind in self._tables:
                return self._tables[kind]
            merged: Dict[int, str] = {}
            # 最低优先级基线: 离线全表静态缓存 (curated/tcp 名字会覆盖它)
            merged.update(_load_static_cache(kind))
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
        if kind in _SOURCES:
            name = self._load_kind(kind).get(iid)
            if name:
                return name
        for compat_kind in _COMPAT_KIND_FALLBACKS.get(kind, ()):
            name = self._load_kind(compat_kind).get(iid)
            if name:
                return name
        if kind == "skill":
            name = _load_tcp_preparse_cache(kind).get(iid)
            if name:
                return name
        if default is not None:
            return default
        return "%s#%d" % (_FALLBACK_PREFIX.get(kind, kind), iid)

    def skill(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("skill", id_, default)

    def player_skill(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("player_skill", id_, default)

    def monster_skill(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("monster_skill", id_, default)

    def environment_skill(self, id_: object, default: Optional[str] = None) -> str:
        return self.resolve("environment_skill", id_, default)

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

    def kind_map(self, kind: str) -> Dict[int, str]:
        return dict(self._load_kind(kind))

    def export_kind_map(self, kind: str) -> Dict[int, str]:
        return self.kind_map(kind)

    def reload(self):
        with self._lock:
            self._tables.clear()
        with _TCP_PREPARSE_CACHE_LOCK:
            _TCP_PREPARSE_CACHE_BY_KIND.clear()


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
