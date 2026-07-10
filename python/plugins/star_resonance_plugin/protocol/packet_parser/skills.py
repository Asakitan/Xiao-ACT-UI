# -*- coding: utf-8 -*-
# Profession / skill mapping tables extracted from packet_parser (physical split).
#
# Self-contained: depends on no other packet_parser submodule.
#

from typing import Dict


PROFESSION_NAMES = {
    1:  '雷影剑士',
    2:  '冰魔导师',
    3:  '涤罪恶火·战斧',
    4:  '青岚骑士',
    5:  '森语者',
    8:  '雷霆一闪·手炮',
    9:  '巨刃守护者',
    10: '暗灵祈舞·仪刀',
    11: '神射手',
    12: '神盾骑士',
    13: '灵魂乐手',
}

# ── 每个职业的普攻 skill_id (SlotPositionId=1) ──
PROFESSION_NORMAL_ATTACK: Dict[int, int] = {
    1:  1701,   # 雷影剑士 — 我流刀法·诛恶
    2:  1201,   # 冰魔导师 — 雨打潮生
    3:  1601,   # 涤罪恶火·战斧 — 赤焰突袭
    4:  1401,   # 青岚骑士 — 风华翔舞
    5:  1501,   # 森语者 — 掌控藤蔓
    8:  1801,   # 雷霆一闪·手炮 — 雷鸣电闪
    9:  1901,   # 巨刃守护者 — 止战之锋
    10: 2101,   # 暗灵祈舞·仪刀
    11: 2201,   # 神射手 — 弹无虚发
    12: 2401,   # 神盾骑士 — 公正之剑
    13: 2321,   # 灵魂乐手 — 琴弦叩击
}

# ── 每个职业的职业技能 skill_id (SlotPositionId=2, 固定) ──
PROFESSION_SKILL: Dict[int, int] = {
    1:  1714,   # 雷影剑士 — 居合斩
    2:  1242,   # 冰魔导师 — 冰霜之矛
    3:  1609,   # 涤罪恶火·战斧 — 红莲
    4:  1418,   # 青岚骑士 — 疾风刺
    5:  1518,   # 森语者 — 狂野绽放
    8:  1806,   # 雷霆一闪·手炮 — 雷域
    9:  1922,   # 巨刃守护者 — 护盾猛击
    10: 2105,   # 暗灵祈舞·仪刀 — 破灵
    11: 2220,   # 神射手 — 暴风箭矢
    12: 2405,   # 神盾骑士 — 英勇盾击
    13: 2306,   # 灵魂乐手 — 增幅节拍
}

# ── 每个职业的大招 / 终技 skill_id (SkillType=1, SlotPositionId=6) ──
PROFESSION_ULTIMATE: Dict[int, int] = {
    1:  1713,   # 雷影剑士 — 极诣·大破灭连斩
    2:  1248,   # 冰魔导师 — 极寒·冰雪颂歌
    3:  1614,   # 涤罪恶火·战斧 — 炎魔
    4:  1426,   # 青岚骑士 — 风神·破阵之风
    5:  1509,   # 森语者 — 繁盛·希望结界
    8:  1808,   # 雷霆一闪·手炮 — 雷爆溟灭
    9:  1907,   # 巨刃守护者 — 岩御·崩裂回环
    10: 2108,   # 暗灵祈舞·仪刀 — 神灵凭依
    11: 2209,   # 神射手 — 锐眼·光能巨箭
    12: 2407,   # 神盾骑士 — 凛威·圣光灌注
    13: 2314,   # 灵魂乐手 — 升格·劲爆全场
}

# ── 每个职业的职业技能变体 (两个分支子职业) ──
# 来源: StarResonanceDps ProfessionExtends.cs
# 每个职业有两个子专精分支，slot 2 的职业技能随分支不同而不同
PROFESSION_SKILL_VARIANTS: Dict[int, tuple] = {
    1:  (1714, 44701),             # 雷影剑士:   居合斩(居合) / 月刃(月刃)
    2:  (1242, 1241),              # 冰魔导师:   冰霜之矛(冰矛) / 寒冰射线(射线)
    3:  (1609, 1605, 1606),        # 涤罪恶火:   红莲 / 无相 / 赤红
    4:  (1418, 1419),              # 青岚骑士:   疾风刺(重装) / 翔返(空枪)
    5:  (1518, 20301),             # 森语者:     狂野绽放(惩戒) / 生命绽放(愈合)
    8:  (1806,),                   # 雷霆一闪·手炮
    9:  (1922, 1930, 199902),      # 巨刃守护者: 护盾猛击 / 格挡冲击(格挡) / 地崩山摧(岩盾)
    10: (2105,),                   # 暗灵祈舞·仪刀
    11: (2220, 2292, 220112),      # 神射手:     暴风箭矢 / 幻影魔狼(狼弓) / 光能箭矢(鹰弓)
    12: (2405, 2406),              # 神盾骑士:   英勇盾击(防盾) / 先锋追击(光盾)
    13: (2306, 2307),              # 灵魂乐手:   增幅节拍(狂音) / 协奏
}

# ── 子职业分支名称映射 (skill_id → 分支名) ──
# 来源: StarResonanceDps ProfessionExtends.cs GetSubProfessionBySkillId()
SUB_PROFESSION_NAMES: Dict[int, str] = {
    # 雷影剑士
    1714: '居合', 1734: '居合',
    44701: '月刃', 179906: '月刃',
    # 冰魔导师
    120901: '冰矛', 120902: '冰矛', 1242: '冰矛',
    1241: '射线',
    # 涤罪恶火
    1605: '无相', 1606: '赤红',
    # 青岚骑士
    1405: '重装', 1418: '重装',
    1419: '空枪',
    # 森语者
    1518: '惩戒', 1541: '惩戒', 21402: '惩戒',
    20301: '愈合',
    # 巨刃守护者
    199902: '岩盾',
    1930: '格挡', 1931: '格挡', 1934: '格挡', 1935: '格挡', 1922: '格挡',
    # 神射手
    2292: '狼弓', 1700820: '狼弓', 1700825: '狼弓', 1700827: '狼弓',
    220112: '鹰弓', 2203622: '鹰弓', 220106: '鹰弓',
    # 神盾骑士
    2405: '防盾', 2406: '光盾',
    # 灵魂乐手
    2306: '狂音',
    2307: '协奏', 2361: '协奏', 55302: '协奏',
}


# Skill level ID composition helper


def _compose_skill_level_id(skill_id: int, level: int = 0) -> int:
    # Compose a skill_level_id from a base skill_id and skill level.
    #
    #     Convention: skill_level_id = skill_id * 100 + level.
    #     Returns skill_id as-is if level is 0 or negative.
    #
    skill_id = int(skill_id or 0)
    level = int(level or 0)
    if skill_id <= 0:
        return 0
    if level > 0:
        return skill_id * 100 + level
    return skill_id


# Reverse lookup: base_skill_id → profession_id (for auto-detection when
# SyncContainerData is missed, e.g. tool started after login)
_SKILL_TO_PROFESSION: Dict[int, int] = {}
for _pid, _sid in PROFESSION_NORMAL_ATTACK.items():
    _SKILL_TO_PROFESSION[_sid] = _pid
for _pid, _sid in PROFESSION_SKILL.items():
    _SKILL_TO_PROFESSION[_sid] = _pid
for _pid, _sid in PROFESSION_ULTIMATE.items():
    _SKILL_TO_PROFESSION[_sid] = _pid
# Include all sub-profession branch variants for reverse lookup
for _pid, _variants in PROFESSION_SKILL_VARIANTS.items():
    for _sid in _variants:
        _SKILL_TO_PROFESSION[_sid] = _pid

# Profession skill prefix: each profession uses a unique 2-digit
# prefix (base_skill_id // 100).  Used to filter out other-profession
# skills during slot inference.
_PROFESSION_PREFIX: Dict[int, int] = {}   # profession_id → prefix
for _pid, _sid in PROFESSION_NORMAL_ATTACK.items():
    _PROFESSION_PREFIX[_pid] = _sid // 100
_ALL_PROFESSION_PREFIXES: frozenset = frozenset(_PROFESSION_PREFIX.values())
