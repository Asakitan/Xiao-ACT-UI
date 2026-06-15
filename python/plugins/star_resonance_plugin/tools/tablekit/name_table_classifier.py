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
import importlib

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO = os.path.dirname(os.path.dirname(_HERE))
_REPO = os.path.dirname(_SAO)


def _resolve_assets_dir() -> str:
    """assets/ 根目录解析 (onedir 友好)。冻结后 assets/ 被提升到 BASE_DIR(exe 顶层),
    而 __file__ 落在 runtime/tools/tablekit/ → runtime/assets 已被搬空。优先
    config.resource_path(BASE_DIR 优先, BUNDLE_DIR 回退), 回退 __file__ 相对(dev 树)。"""
    try:
        from config import resource_path  # BASE_DIR-first, BUNDLE_DIR fallback
        cand = resource_path("assets")
        if os.path.isdir(cand):
            return cand
    except Exception:
        pass
    return os.path.join(_SAO, "assets")


_ASSETS = _resolve_assets_dir()
_DATATOOLS_CN = os.path.join(_REPO, "StarResonanceDps", "DataTools", "Data", "CN")
_RESONANCE_METER = os.path.join(_REPO, "resonance-logs-cn", "src-tauri", "meter-data")
_RESONANCE_CONFIG = os.path.join(_REPO, "resonance-logs-cn", "src", "lib", "config")
_SR_WINFORM_TABLE = os.path.join(_REPO, "StarResonanceDps", "StarResonanceDpsAnalysis.WinForm", "Core", "TabelJson")
_SR_WPF_MONSTER = os.path.join(_REPO, "StarResonanceDps", "StarResonanceDpsAnalysis.WPF", "Data", "Monster")
_SR_OLD_MONSTER = os.path.join(_REPO, "StarResonanceDps", "DataTools", "Old", "Data", "monster")

SKILL_KIND = "skill"
PLAYER_SKILL_KIND = "player_skill"
MONSTER_SKILL_KIND = "monster_skill"
ENVIRONMENT_SKILL_KIND = "environment_skill"
FIELD_MARKER_KIND = "field_marker"
BOSS_SKILL_KIND = "boss_skill"
ULTIMATE_SKILL_KIND = "ultimate_skill"
ROGUELIKE_AFFIX_KIND = "roguelike_affix"
PROFESSION_SKILL_KIND = "profession_skill"
SCRIPTED_SKILL_KIND = "scripted_skill"
VIRTUAL_SKILL_KIND = "virtual_skill"
BOSS_MECHANIC_SKILL_KIND = "boss_mechanic_skill"
CLIENT_EFFECT_SKILL_KIND = "client_effect_skill"
INTERACTION_SKILL_KIND = "interaction_skill"
COMPANION_SKILL_KIND = "companion_skill"
PROJECTILE_SKILL_KIND = "projectile_skill"
PASSIVE_SKILL_KIND = "passive_skill"
TEST_SKILL_KIND = "test_skill"
SYSTEM_SKILL_KIND = "system_skill"
BUFF_KIND = "buff"
PLAYER_BUFF_KIND = "player_buff"
FACTOR_BUFF_KIND = "factor_buff"
PROFESSION_SKILL_BUFF_KIND = "profession_skill_buff"
EVENT_KIND = "event"
BOSS_MECHANIC_KIND = "boss_mechanic"

# --- self-contained data sources (this project only, never a neighbour repo) ---
_NAME_TABLES = os.path.join(_ASSETS, "name_tables")
_SHARED_CACHE = os.path.join(_NAME_TABLES, "tcp_preparse_name_cache.json")
_LOCAL_CACHE = os.environ.get(
    "SAO_TCP_PREPARSE_NAME_CACHE_LOCAL",
    os.path.join(_NAME_TABLES, "tcp_preparse_name_cache.local.json"),
)
# Specific kinds per id domain, in disambiguation priority (first listed wins for
# the rare id present in more than one table). The skill domain has 0 intra-domain
# ambiguity; the buff domain resolves its overlaps by this order.
_SKILL_DOMAIN_KINDS = (
    FIELD_MARKER_KIND, ULTIMATE_SKILL_KIND, ROGUELIKE_AFFIX_KIND, PROFESSION_SKILL_KIND,
    SCRIPTED_SKILL_KIND, VIRTUAL_SKILL_KIND, BOSS_MECHANIC_SKILL_KIND, BOSS_SKILL_KIND,
    MONSTER_SKILL_KIND, ENVIRONMENT_SKILL_KIND, COMPANION_SKILL_KIND, PROJECTILE_SKILL_KIND,
    INTERACTION_SKILL_KIND, CLIENT_EFFECT_SKILL_KIND, PLAYER_SKILL_KIND, PASSIVE_SKILL_KIND,
    TEST_SKILL_KIND, SYSTEM_SKILL_KIND,
)
_BUFF_DOMAIN_KINDS = (EVENT_KIND, PROFESSION_SKILL_BUFF_KIND, PLAYER_BUFF_KIND, FACTOR_BUFF_KIND)
# Kinds materialised by load_classified_tables: specific + aggregate buff + boss.
_AGGREGATE_AND_BOSS_KINDS = (BUFF_KIND, "boss")

_FIELD_MARKER_RE = re.compile(r"(场地标记|场地|地面|范围标记)")
_EVENT_RE = re.compile(r"(事件|Event|event|触发事件|玩法事件|任务事件)")
_SCRIPTED_RE = re.compile(r"(剧情|表演|演出|锁定追击|空降|过场|脚本|idle|Idle|IDLE)")
_VIRTUAL_RE = re.compile(r"(虚拟体|虚拟|dummy|Dummy|DUMMY|VFX|vfx|假子弹|假体)")
_ULTIMATE_RE = re.compile(r"(奥义|幻想|终极|绝技|大招|ULT|ult)")
_ROGUELIKE_RE = re.compile(r"(肉鸽词条|大秘境词条|词条|赛季词缀|赛季词条)")
_BOSS_MECHANIC_SKILL_RE = re.compile(r"(读条|点名|分摊|致死|转阶段|阶段转换|机制杀|机制|踩塔|连线|全场|秒杀|破盾|破防|锁血|斩杀|狂暴|死亡计时|密码破译|角斗准备)")
_ENVIRONMENT_SKILL_RE = re.compile(r"(吸引怪物|拉怪|黯影堡垒专用|场景专用|地图专用|关卡专用|机关|陷阱|装置|传送|清怪辅助|交互|环境|地板|毒池|领域|子弹雨|弹雨|地刺|喷泉|光浪|风场|炮台|载具|矿车|电梯)")
_MONSTER_SKILL_RE = re.compile(r"(小怪|精英怪|魔物|哥布林|史莱姆|肉山|野猪|枪兵|盾兵|蜥蜴人|剧毒蜂巢|触手|爪击|撕咬|啃咬|扫尾|平A|挥击|下砸|砸地|践踏|撼地|冲拳|锤击|钳击|旋风斩|咆哮|三连砸|冲锋|撞击|跳劈|劈砍|前砍|吐息|飞扑|召唤伙伴|召唤导弹|子弹三连|灼烧弹)")
_LEGACY_MONSTER_CONTEXT_RE = re.compile(r"(英雄本|大师本|噩梦|多人|肉鸽大秘境|黯影堡垒|安德拉|黑石|鱼人|石头人|卷心菜|小猪|圣域飞鱼|虚蚀龙|野猪|蜥蜴人|蟹蛛|岩蛇|娜宝|多戈尔曼|蒂娜BOSS|BOSS|boss|Boss|首领|眼球王|军团盾|枪兵|盾兵|骑士|法师|弓手|小斧哥|双子机像|机器人|傀儡|召唤兽|野兽)")
_LEGACY_MONSTER_ACTION_RE = re.compile(r"(重击|飞刃|虚蚀弹|剑气|烈火|龙卷风|炸弹|巡逻子弹|组合技|沉默水池|天空水池|狙击|凝滞场|三连突刺|蓄力|连斩|破甲|重砍|突进|缠绕射击|压团血|死刑|顺劈|碎地|重拳|地震波|裂石风暴|疯狂锤地|撞墙|落石|内爆|冲撞|石怒|寒冰荆棘|流火之沼|影之瞬杀|虚蚀弹三连|单发|陀螺|吹风|光棱|魔法箭矢|光能箭矢|冰暗巨口|岩火巨口|森风巨口)")
_LEGACY_ENVIRONMENT_CONTEXT_RE = re.compile(r"(升降门|空气墙|场景|专用|调查团|药水|低重力|观光|资源AI|显影|变色|崩塌|落雷场景|可被吸引|内场资源|巨塔)")
_LEGACY_BOSS_CONTEXT_RE = re.compile(r"(P[123456]|首领|BOSS|boss|Boss|绯红剑影|炎光|幻华|殷红断狱|断狱|终焉|共罪|蚀心|神罚|陨星|大炎戒|狱炎轮回|灭世|角斗|誓死守护|领地驱逐|趴着演绎)")
_PLAYER_SKILL_RE = re.compile(r"(红光反制|飞鸟投|刹那|滋养|护盾猛击|幸运一击|止战之锋|普通攻击|普攻|特殊攻击|专精技能|共鸣技能|领地共鸣|共鸣|职业技能|武器技能)")
_LEGACY_BUFF_RE = re.compile(r"(BUFF|Buff|buff|子BUFF|子buff|计时BUFF|叠层buff|增伤BUFF|易伤|减益)")
_PROFESSION_BUFF_RE = re.compile(r"(职业|专精|天赋|流派|普攻|特攻|特殊攻击|大招|奥义|终技|技能强化|替换技能|派生|分支|圣令|气刃|寒冰能量|光铸|种子|协奏|狂音|雷之印|恩格|护盾猛击|先锋追击|狂野绽放|生命绽放)")
_SCRIPTED_EXCLUDE_RE = re.compile(r"(锁定追击|点名|分摊|机制|读条|致死|秒杀)")
_CLIENT_EFFECT_RE = re.compile(r"(特效|表现|屏幕|镜头|光效|音效|动画|波纹|水花|高亮|隐藏表现|前端|纯客户端|模型|消散|出生技|反馈)")
_INTERACTION_SKILL_RE = re.compile(r"(采集|钓鱼|挖矿|浇水|清扫|种花|家园|抚摸|吓猫|狗狗举手|射击狗狗|拔娜宝|坐下|演奏|游轮|弹簧|摩天轮|旋转木马|浮标|得分圈|滑翔|跑酷|传送门|可收集|阅读物|协会|狩猎|资源点)")
_COMPANION_SKILL_RE = re.compile(r"(宠物|伙伴|召唤兽|猎鹰|雄鹰|野兽伙伴|协同子弹)")
_PROJECTILE_SKILL_RE = re.compile(r"(子弹|箭矢|飞弹|导弹|火球|炸弹|陨石|水球|弹幕|飞刃|飞石|光棱|箭雨|爆炸箭)")
_PASSIVE_SKILL_RE = re.compile(r"(被动|Passive|passive|天赋|层数|计数|增伤|减伤|加速|减CD|刷新|冷却|能量|资源恢复|护盾|恢复|治疗|属性|标记|印记|状态|转为|类型|光环|强化|替换技能)")
_TEST_SKILL_RE = re.compile(r"(测试|test|Test|TEST|占位|废弃|作废|dummy_test|Debug|debug)")


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
def aoyi_skill_names() -> dict[int, str]:
    return _plain_names(_load_json(os.path.join(_RESONANCE_CONFIG, "skill_aoyi_icons.json")), fields=("NameDesign", "Name", "name", "text"))


@lru_cache(maxsize=1)
def damage_attr_names() -> dict[int, str]:
    return _plain_names(_load_json(os.path.join(_RESONANCE_CONFIG, "DamageAttrIdName.json")), fields=("NameDesign", "Name", "name", "text"))


@lru_cache(maxsize=1)
def profession_skill_ids() -> set[int]:
    ids: set[int] = set()
    try:
        skills = importlib.import_module("packet_parser.skills")
    except Exception:
        return ids
    for attr in ("PROFESSION_NORMAL_ATTACK", "PROFESSION_SKILL", "PROFESSION_ULTIMATE"):
        mapping = getattr(skills, attr, {}) or {}
        if isinstance(mapping, Mapping):
            ids.update(_safe_int(value) for value in mapping.values())
    variants = getattr(skills, "PROFESSION_SKILL_VARIANTS", {}) or {}
    if isinstance(variants, Mapping):
        for values in variants.values():
            if isinstance(values, (list, tuple, set)):
                ids.update(_safe_int(value) for value in values)
    sub_names = getattr(skills, "SUB_PROFESSION_NAMES", {}) or {}
    if isinstance(sub_names, Mapping):
        ids.update(_safe_int(key) for key in sub_names)
    return {sid for sid in ids if sid > 0}


@lru_cache(maxsize=1)
def ultimate_skill_ids() -> set[int]:
    ids = set(aoyi_skill_names())
    try:
        skills = importlib.import_module("packet_parser.skills")
    except Exception:
        skills = None
    mapping = getattr(skills, "PROFESSION_ULTIMATE", {}) if skills is not None else {}
    if isinstance(mapping, Mapping):
        ids.update(_safe_int(value) for value in mapping.values())
    return {sid for sid in ids if sid > 0}


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


@lru_cache(maxsize=1)
def monster_skill_ids() -> set[int]:
    ids: set[int] = set()
    bosses = boss_monster_ids()
    for monster_id, row in monster_table().items():
        if monster_id in bosses or _safe_int(row.get("MonsterType"), -1) == 2:
            continue
        for raw in row.get("SkillIds") or []:
            sid = _safe_int(raw)
            if sid > 0:
                ids.add(sid)
    return ids


@lru_cache(maxsize=1)
def environment_skill_ids() -> set[int]:
    ids: set[int] = set()
    for sid in set(skill_table()) | set(skill_fallback_names()):
        text = _skill_name_text(sid)
        row = skill_table().get(sid) or {}
        if _ENVIRONMENT_SKILL_RE.search(text):
            ids.add(sid)
            continue
        if row and _safe_int(row.get("VehicleSkillType"), 0) > 0:
            ids.add(sid)
    return {sid for sid in ids if sid > 0}


@lru_cache(maxsize=1)
def player_skill_ids() -> set[int]:
    ids = set(profession_skill_ids()) | set(ultimate_skill_ids())
    for sid, row in skill_table().items():
        text = _skill_name_text(sid, row)
        if _PLAYER_SKILL_RE.search(text) and sid not in boss_skill_ids() and sid not in monster_skill_ids():
            ids.add(sid)
    return {sid for sid in ids if sid > 0}


def _slot_positions(row: Mapping[str, Any]) -> set[int]:
    raw = row.get("SlotPositionId") or []
    if isinstance(raw, (int, str)):
        raw = [raw]
    return {_safe_int(value, -1) for value in raw if _safe_int(value, -1) >= 0}


def _skill_name_text(skill_id: int, row: Mapping[str, Any] | None = None) -> str:
    row = row or skill_table().get(skill_id) or {}
    pieces = [
        _entry_name(row),
        _clean_text(row.get("NameDesign") if isinstance(row, Mapping) else ""),
        skill_fallback_names().get(skill_id, ""),
        aoyi_skill_names().get(skill_id, ""),
        damage_attr_names().get(skill_id, ""),
    ]
    return " ".join(text for text in pieces if text)


@lru_cache(maxsize=1)
def _cache_by_kind() -> dict[str, dict[int, str]]:
    """id->name per kind from our own MEM/TCP parse cache (shared + local)."""
    out: dict[str, dict[int, str]] = {}
    for path in (_SHARED_CACHE, _LOCAL_CACHE):
        data = _load_json(path)
        by_kind = (((data.get("names") or {}).get("by_kind")) or {}) if isinstance(data, Mapping) else {}
        if not isinstance(by_kind, Mapping):
            continue
        for kind, bucket in by_kind.items():
            if str(kind) == "player" or not isinstance(bucket, Mapping):
                continue
            for sid, entry in bucket.items():
                iid = _safe_int(sid)
                if iid <= 0:
                    continue
                text = entry if isinstance(entry, str) else _entry_name(entry, fields=("text", "name", "Name"))
                text = _clean_text(text)
                if text:
                    out.setdefault(str(kind), {})[iid] = text
    return out


@lru_cache(maxsize=1)
def _our_index() -> tuple[dict[int, str], dict[int, str], dict[int, str]]:
    """Build (skill_id->kind, buff_id->kind, id->name) from OUR project data only:
    committed per-kind tables + the MEM/TCP parse cache. No neighbour repo."""
    skill_idx: dict[int, str] = {}
    buff_idx: dict[int, str] = {}
    names: dict[int, str] = {}
    for kind in _SKILL_DOMAIN_KINDS:
        for sid, nm in _load_json(os.path.join(_NAME_TABLES, f"{kind}.json")).items():
            iid = _safe_int(sid)
            if iid > 0:
                skill_idx.setdefault(iid, kind)
                if nm:
                    names.setdefault(iid, str(nm))
    for kind in _BUFF_DOMAIN_KINDS:
        for sid, nm in _load_json(os.path.join(_NAME_TABLES, f"{kind}.json")).items():
            iid = _safe_int(sid)
            if iid > 0:
                buff_idx.setdefault(iid, kind)
                if nm:
                    names.setdefault(iid, str(nm))
    # Our parse cache is authoritative: it refines kind and supplies in-memory names.
    for kind, bucket in _cache_by_kind().items():
        for iid, text in bucket.items():
            if kind in _SKILL_DOMAIN_KINDS:
                skill_idx[iid] = kind
            elif kind in _BUFF_DOMAIN_KINDS:
                buff_idx[iid] = kind
            if text:
                names[iid] = text
    return skill_idx, buff_idx, names


def classify_skill_id(skill_id: Any) -> str:
    sid = _safe_int(skill_id)
    if sid <= 0:
        return ""
    return _classify_skill_id_cached(sid)


@lru_cache(maxsize=16384)
def _classify_skill_id_cached(sid: int) -> str:
    # Hot path: called per damage event via name_tables.resolve()->_semantic_kind().
    # 1) trust our own classification (committed tables + MEM/TCP parse cache);
    # 2) for a brand-new id fall back to name-text regex + our packet_parser id
    # sets. No neighbour metadata (SlotPosition/SkillType/IsDangerSkill/...).
    skill_idx, _, names = _our_index()
    known = skill_idx.get(sid)
    if known:
        return known
    text = names.get(sid, "")
    if _FIELD_MARKER_RE.search(text):
        return FIELD_MARKER_KIND
    if _TEST_SKILL_RE.search(text):
        return TEST_SKILL_KIND
    if _VIRTUAL_RE.search(text):
        return VIRTUAL_SKILL_KIND
    if _SCRIPTED_RE.search(text):
        if _BOSS_MECHANIC_SKILL_RE.search(text) and not _SCRIPTED_EXCLUDE_RE.search(text):
            return BOSS_MECHANIC_SKILL_KIND
        return SCRIPTED_SKILL_KIND
    if _ROGUELIKE_RE.search(text):
        return ROGUELIKE_AFFIX_KIND
    if _EVENT_RE.search(text):
        return EVENT_KIND
    if _LEGACY_BUFF_RE.search(text):
        if _PROFESSION_BUFF_RE.search(text):
            return PROFESSION_SKILL_BUFF_KIND
        return FACTOR_BUFF_KIND
    if sid in ultimate_skill_ids() or _ULTIMATE_RE.search(text):
        return ULTIMATE_SKILL_KIND
    if sid in profession_skill_ids():
        return PROFESSION_SKILL_KIND
    if _BOSS_MECHANIC_SKILL_RE.search(text):
        return BOSS_MECHANIC_SKILL_KIND
    if _ENVIRONMENT_SKILL_RE.search(text) or _LEGACY_ENVIRONMENT_CONTEXT_RE.search(text):
        return ENVIRONMENT_SKILL_KIND
    if _LEGACY_BOSS_CONTEXT_RE.search(text):
        return BOSS_SKILL_KIND
    if _PLAYER_SKILL_RE.search(text):
        return PLAYER_SKILL_KIND
    if _COMPANION_SKILL_RE.search(text):
        return COMPANION_SKILL_KIND
    if _MONSTER_SKILL_RE.search(text) or _LEGACY_MONSTER_CONTEXT_RE.search(text) or _LEGACY_MONSTER_ACTION_RE.search(text):
        return MONSTER_SKILL_KIND
    if _INTERACTION_SKILL_RE.search(text):
        return INTERACTION_SKILL_KIND
    if _CLIENT_EFFECT_RE.search(text):
        return CLIENT_EFFECT_SKILL_KIND
    if _PROJECTILE_SKILL_RE.search(text):
        return PROJECTILE_SKILL_KIND
    if _PASSIVE_SKILL_RE.search(text):
        return PASSIVE_SKILL_KIND
    return SYSTEM_SKILL_KIND


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
    return _classify_buff_id_cached(bid)


@lru_cache(maxsize=16384)
def _classify_buff_id_cached(bid: int) -> str:
    # 1) trust our own classification; 2) name-text regex for brand-new ids.
    # No neighbour BuffTable metadata (Visible/IsClientBuff/Tags/...).
    _, buff_idx, names = _our_index()
    known = buff_idx.get(bid)
    if known:
        return known
    text = names.get(bid, "")
    if _EVENT_RE.search(text):
        return EVENT_KIND
    if _SCRIPTED_RE.search(text) or _VIRTUAL_RE.search(text):
        return EVENT_KIND
    if _ROGUELIKE_RE.search(text):
        return FACTOR_BUFF_KIND
    if _PROFESSION_BUFF_RE.search(text):
        return PROFESSION_SKILL_BUFF_KIND
    return BUFF_KIND


def _skill_name(skill_id: int) -> str:
    row = skill_table().get(skill_id) or {}
    return _entry_name(row) or skill_fallback_names().get(skill_id, "") or aoyi_skill_names().get(skill_id, "") or damage_attr_names().get(skill_id, "")


def _skill_maps() -> dict[str, dict[int, str]]:
    out = {
        PLAYER_SKILL_KIND: {},
        MONSTER_SKILL_KIND: {},
        ENVIRONMENT_SKILL_KIND: {},
        FIELD_MARKER_KIND: {},
        BOSS_SKILL_KIND: {},
        ULTIMATE_SKILL_KIND: {},
        ROGUELIKE_AFFIX_KIND: {},
        PROFESSION_SKILL_KIND: {},
        SCRIPTED_SKILL_KIND: {},
        VIRTUAL_SKILL_KIND: {},
        BOSS_MECHANIC_SKILL_KIND: {},
        CLIENT_EFFECT_SKILL_KIND: {},
        INTERACTION_SKILL_KIND: {},
        COMPANION_SKILL_KIND: {},
        PROJECTILE_SKILL_KIND: {},
        PASSIVE_SKILL_KIND: {},
        TEST_SKILL_KIND: {},
        SYSTEM_SKILL_KIND: {},
        BUFF_KIND: {},
        FACTOR_BUFF_KIND: {},
        PLAYER_BUFF_KIND: {},
        PROFESSION_SKILL_BUFF_KIND: {},
        EVENT_KIND: {},
    }
    ids = set(skill_table()) | set(skill_fallback_names()) | set(aoyi_skill_names()) | boss_skill_ids() | monster_skill_ids() | environment_skill_ids() | player_skill_ids()
    ids.update(sid for sid, text in damage_attr_names().items() if _ULTIMATE_RE.search(text) or _ROGUELIKE_RE.search(text) or _VIRTUAL_RE.search(text) or _BOSS_MECHANIC_SKILL_RE.search(text))
    for sid in sorted(ids):
        name = _skill_name(sid)
        if not name:
            continue
        kind = classify_skill_id(sid)
        if kind in out:
            out[kind][sid] = name
    return out


def _buff_maps() -> dict[str, dict[int, str]]:
    out = {BUFF_KIND: {}, PLAYER_BUFF_KIND: {}, FACTOR_BUFF_KIND: {}, PROFESSION_SKILL_BUFF_KIND: {}, EVENT_KIND: {}}
    ids = set(buff_table())
    ids.update(buff_fallback_names())
    for bid in sorted(ids):
        name = _buff_name(bid)
        if not name:
            continue
        kind = classify_buff_id(bid)
        if kind == EVENT_KIND:
            out[kind][bid] = name
            continue
        if kind == PROFESSION_SKILL_BUFF_KIND:
            out[PROFESSION_SKILL_BUFF_KIND][bid] = name
        elif kind == PLAYER_BUFF_KIND:
            out[PLAYER_BUFF_KIND][bid] = name
        elif kind == FACTOR_BUFF_KIND:
            out[FACTOR_BUFF_KIND][bid] = name
        else:
            out[BUFF_KIND][bid] = name
        if kind in {PLAYER_BUFF_KIND, FACTOR_BUFF_KIND, PROFESSION_SKILL_BUFF_KIND, BUFF_KIND}:
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
    """Materialise each kind table from OUR data only: committed ``<kind>.json``
    overlaid with the MEM/TCP parse cache, plus combat_preparse boss mechanics.
    No neighbour repo, so a missing StarResonanceDps never empties a table."""
    result: dict[str, dict[str, dict[str, str]]] = {}
    cache = _cache_by_kind()
    for kind in _SKILL_DOMAIN_KINDS + _BUFF_DOMAIN_KINDS + _AGGREGATE_AND_BOSS_KINDS:
        table: dict[str, dict[str, str]] = {}
        for sid, nm in _load_json(os.path.join(_NAME_TABLES, f"{kind}.json")).items():
            iid = _safe_int(sid)
            text = _clean_text(nm)
            if iid > 0 and text:
                table[str(iid)] = {"text": text, "confidence": "static"}
        for iid, text in cache.get(kind, {}).items():
            if text:
                table[str(iid)] = {"text": text, "confidence": "tcp"}
        result[kind] = table
    # buff aggregate carries every buff sub-kind id (event stays out of the
    # aggregate, matching the legacy split).
    aggregate = result.setdefault(BUFF_KIND, {})
    for kind in (PROFESSION_SKILL_BUFF_KIND, PLAYER_BUFF_KIND, FACTOR_BUFF_KIND):
        for sid, entry in result.get(kind, {}).items():
            aggregate.setdefault(sid, entry)
    # boss mechanics from our combat_preparse table (fall back to committed json).
    mechanics = _boss_mechanic_map()
    if mechanics:
        result[BOSS_MECHANIC_KIND] = _to_runtime(mechanics)
    else:
        boss_mech: dict[str, dict[str, str]] = {}
        for sid, nm in _load_json(os.path.join(_NAME_TABLES, "boss_mechanic.json")).items():
            iid = _safe_int(sid)
            text = _clean_text(nm)
            if iid > 0 and text:
                boss_mech[str(iid)] = {"text": text, "confidence": "static"}
        result[BOSS_MECHANIC_KIND] = boss_mech
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
