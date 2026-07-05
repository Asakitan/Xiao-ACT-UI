# -*- coding: utf-8 -*-
# packet_parser package.
#
# Physical split of the former single-file ``packet_parser.py`` (5099 lines).
# The public surface is 100% backward compatible: ``from packet_parser import
# PacketParser, PlayerData, MonsterData`` and ``import packet_parser`` followed
# by attribute access (``packet_parser.PacketParser`` etc.) keep working exactly
# as before.
#
# Submodules (import them in topological order — enums/skills have no internal
# deps, helpers depends on enums, data depends on helpers, parser depends on
# all of them):
# - enums.py    : MessageType / NotifyMethod / AttrType / ... enum classes
# - skills.py   : PROFESSION_* / SUB_PROFESSION_* mapping tables
# - helpers.py  : Cython bindings, lazy loaders, decode / level helpers
# - data.py     : MonsterData / PlayerData state dataclasses
# - parser.py   : PacketParser main class

# ── enums / constants ──────────────────────────────────────────────────────
from plugins.star_resonance_plugin.protocol.packet_parser.enums import (
    MessageType,
    NotifyMethod,
    _NOTIFY_METHOD_NAMES,
    AttrType,
    SERVICE_UUID_C3SB,
    BuffEventType,
    _BOSS_BUFF_EVENTS,
    WIPE_BUFF_BASE_ID,
    DamageType,
    EntityType,
    DisappearType,
    AppearType,
    SkillCDType,
    CharField,
    CHAR_FIELD_NAMES,
    _HANDLED_CHAR_FIELDS,
    DeltaField,
    EntityField,
    ToMeDeltaField,
    _RESET_IGNORE_TARGETS,
)

# ── skills / profession tables ─────────────────────────────────────────────
from plugins.star_resonance_plugin.protocol.packet_parser.skills import (
    PROFESSION_NAMES,
    PROFESSION_NORMAL_ATTACK,
    PROFESSION_SKILL,
    PROFESSION_ULTIMATE,
    PROFESSION_SKILL_VARIANTS,
    SUB_PROFESSION_NAMES,
    _compose_skill_level_id,
    _SKILL_TO_PROFESSION,
    _PROFESSION_PREFIX,
    _ALL_PROFESSION_PREFIXES,
)

# ── data classes ───────────────────────────────────────────────────────────
from plugins.star_resonance_plugin.protocol.packet_parser.data import (
    MonsterData,
    PlayerData,
)

# ── main parser ────────────────────────────────────────────────────────────
from plugins.star_resonance_plugin.protocol.packet_parser.parser import PacketParser

__all__ = [
    # core (must work for external imports)
    'PacketParser',
    'PlayerData',
    'MonsterData',
    # enums / constants
    'MessageType',
    'NotifyMethod',
    '_NOTIFY_METHOD_NAMES',
    'AttrType',
    'SERVICE_UUID_C3SB',
    'BuffEventType',
    '_BOSS_BUFF_EVENTS',
    'WIPE_BUFF_BASE_ID',
    'DamageType',
    'EntityType',
    'DisappearType',
    'AppearType',
    'SkillCDType',
    'CharField',
    'CHAR_FIELD_NAMES',
    '_HANDLED_CHAR_FIELDS',
    'DeltaField',
    'EntityField',
    'ToMeDeltaField',
    '_RESET_IGNORE_TARGETS',
    # skills / professions
    'PROFESSION_NAMES',
    'PROFESSION_NORMAL_ATTACK',
    'PROFESSION_SKILL',
    'PROFESSION_ULTIMATE',
    'PROFESSION_SKILL_VARIANTS',
    'SUB_PROFESSION_NAMES',
    '_compose_skill_level_id',
    '_SKILL_TO_PROFESSION',
    '_PROFESSION_PREFIX',
    '_ALL_PROFESSION_PREFIXES',
]
