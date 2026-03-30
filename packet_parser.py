# -*- coding: utf-8 -*-
"""
Packet parser for the SAO Auto overlay.

This module decodes the framed game packets produced by `packet_capture.py`:
  [4B size BE][2B type BE][payload]

Supported features:
  - MessageType dispatch (Notify / FrameDown / Return)
  - Zstd decompression
  - Lightweight protobuf field decoding for the sync messages we use
  - AttrCollection parsing
  - SyncContainerDirtyData stream parsing
"""

import math
import struct
import logging
import os
import json
import sys
import time
from typing import Optional, Callable, Dict, Any

logger = logging.getLogger('sao_auto.parser')
_PACKET_DEBUG_ENABLED = True  # Enable to log raw packet snapshots for field confirmation





_zstd = None
_pb = None
_pb_loaded = False


def _ensure_zstd():
    global _zstd
    if _zstd is not None:
        return _zstd
    try:
        import zstandard
        _zstd = zstandard.ZstdDecompressor(max_window_size=2**25)
        return _zstd
    except ImportError:
        raise RuntimeError('缺少 zstandard 模块，请运行: pip install zstandard')


def _ensure_pb():
    """Load compiled protobuf module if available."""
    global _pb, _pb_loaded
    if _pb_loaded:
        return _pb
    _pb_loaded = True


    try:
        from proto import star_resonance_pb2
        _pb = star_resonance_pb2
        logger.info('[Parser] using compiled protobuf')
        return _pb
    except ImportError:
        pass


    try:
        from google.protobuf import descriptor_pb2, descriptor_pool, symbol_database
        from google.protobuf import reflection, descriptor
        import google.protobuf.descriptor as _desc


        proto_path = os.path.join(os.path.dirname(__file__), 'proto', 'star_resonance.proto')
        if os.path.exists(proto_path):


            pass
    except ImportError:
        pass

    logger.info('[Parser] using built-in mini protobuf decoder')
    return None



# Mini protobuf helpers



def _read_varint(data: bytes, pos: int):
    """Read a protobuf varint and return `(value, new_pos)`."""
    result = 0
    shift = 0
    while pos < len(data):
        b = data[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if (b & 0x80) == 0:
            return result, pos
        shift += 7
    return result, pos


def _read_signed_varint(data: bytes, pos: int):
    """Read a signed varint using direct int32 two's-complement semantics."""
    val, pos = _read_varint(data, pos)
    if val > 0x7FFFFFFF:
        val -= 0x100000000
    return val, pos


def _decode_fields(data: bytes) -> Dict[int, list]:
    """
    Decode protobuf bytes into a `{field_number: [values]}` dictionary.

    Supported wire types:
      0 = varint
      1 = 64-bit
      2 = length-delimited
      5 = 32-bit
    """
    fields: Dict[int, list] = {}
    pos = 0
    length = len(data)
    while pos < length:
        tag, pos = _read_varint(data, pos)
        field_num = tag >> 3
        wire_type = tag & 0x07
        if wire_type == 0:  # varint
            val, pos = _read_varint(data, pos)
            fields.setdefault(field_num, []).append(val)
        elif wire_type == 1:  # 64-bit
            if pos + 8 > length:
                break
            val = struct.unpack_from('<q', data, pos)[0]
            pos += 8
            fields.setdefault(field_num, []).append(val)
        elif wire_type == 2:  # length-delimited
            vlen, pos = _read_varint(data, pos)
            if pos + vlen > length:
                break
            val = data[pos:pos + vlen]
            pos += vlen
            fields.setdefault(field_num, []).append(val)
        elif wire_type == 5:  # 32-bit
            if pos + 4 > length:
                break
            val = struct.unpack_from('<f', data, pos)[0]
            pos += 4
            fields.setdefault(field_num, []).append(val)
        else:
            break  # unknown wire type
    return fields


def _varint_to_int64(val: int) -> int:
    """Convert a protobuf varint to signed int64."""
    if val > 0x7FFFFFFFFFFFFFFF:
        val -= 0x10000000000000000
    return val


def _varint_to_int32(val: int) -> int:
    if val > 0x7FFFFFFF:
        val -= 0x100000000
    return val


def _decode_string_from_raw(raw: bytes) -> str:
    """Match protobufjs `reader.string()`: `[varint length][utf-8 bytes]`."""
    if not raw:
        return ''
    try:
        str_len, pos = _read_varint(raw, 0)
        if str_len > 0 and pos + str_len <= len(raw):
            return raw[pos:pos + str_len].decode('utf-8', 'ignore')
    except Exception:
        pass

    try:
        return raw.decode('utf-8', 'ignore')
    except Exception:
        return ''


def _decode_int32_from_raw(raw: bytes) -> int:
    """Match protobufjs `reader.int32()` on a raw varint payload."""
    if not raw:
        return 0
    try:
        val, _ = _read_varint(raw, 0)
        return _varint_to_int32(val)
    except Exception:
        return 0


def _decode_float32_from_raw(raw: bytes) -> Optional[float]:
    if not raw or len(raw) < 4:
        return None
    try:
        return struct.unpack_from('<f', raw, 0)[0]
    except Exception:
        return None


def _append_packet_debug(tag: str, payload: Dict[str, Any]):
    """Append a small packet debug snapshot for later field confirmation."""
    if not _PACKET_DEBUG_ENABLED:
        return
    try:
        debug_path = os.path.join(os.path.dirname(__file__), 'packet_debug.jsonl')
        row = {
            'ts': round(time.time(), 3),
            'tag': tag,
            **payload,
        }
        with open(debug_path, 'a', encoding='utf-8') as f:
            f.write(json.dumps(row, ensure_ascii=False) + '\n')
    except Exception:
        pass






class MessageType:
    NONE = 0
    CALL = 1
    NOTIFY = 2
    RETURN = 3
    ECHO = 4
    FRAME_UP = 5
    FRAME_DOWN = 6


class NotifyMethod:
    # ── Core sync (handled with full logic) ──
    SYNC_NEAR_ENTITIES = 0x06
    SYNC_CONTAINER_DATA = 0x15
    SYNC_CONTAINER_DIRTY_DATA = 0x16
    SYNC_SERVER_TIME = 0x2B
    SYNC_NEAR_DELTA_INFO = 0x2D
    SYNC_TO_ME_DELTA_INFO = 0x2E
    # ── Login / Session ──
    SYNC_PIONEER_INFO = 0x0E
    SYNC_SWITCH_CHANGE = 0x12
    SYNC_SWITCH_INFO = 0x13
    ENTER_GAME = 0x14
    SYNC_DUNGEON_DATA = 0x17
    # ── Awards / Items ──
    AWARD_NOTIFY = 0x19
    CARD_INFO_ACK = 0x1A
    SYNC_SEASON = 0x1B
    # ── Actions / Social ──
    USER_ACTION = 0x1C
    NOTIFY_DISPLAY_PLAY_HELP = 0x1D
    NOTIFY_APPLICATION_INTERACTION = 0x1E
    NOTIFY_IS_AGREE = 0x1F
    NOTIFY_CANCEL_ACTION = 0x20
    NOTIFY_UPLOAD_PICTURE_RESULT = 0x21
    SYNC_INVITE = 0x24
    NOTIFY_RED_DOT_CHANGE = 0x25
    CHANGE_NAME_RESULT_NTF = 0x26
    # ── Combat / Revive ──
    NOTIFY_REVIVE_USER = 0x27
    # ── Parkour ──
    NOTIFY_PARKOUR_RANK_INFO = 0x28
    NOTIFY_PARKOUR_RECORD_INFO = 0x29
    # ── UI Notifications ──
    NOTIFY_SHOW_TIPS = 0x2A
    NOTIFY_NOTICE_INFO = 0x2C
    # ── Session management ──
    NOTIFY_CLIENT_KICK_OFF = 0x31
    PAYMENT_RESPONSE = 0x33
    NOTIFY_UNLOCK_COOK_BOOK = 0x35
    NOTIFY_CUSTOM_EVENT = 0x36
    NOTIFY_START_PLAYING_DUNGEON = 0x37
    CHANGE_SHOW_ID_RESULT_NTF = 0x38
    NOTIFY_SHOW_ITEMS = 0x39
    NOTIFY_SEASON_ACTIVATION_TARGET_INFO = 0x3A
    NOTIFY_TEXT_CHECK_RESULT = 0x3B
    NOTIFY_DEBUG_MESSAGE_TIP = 0x3D
    NOTIFY_USER_CLOSE_FUNCTION = 0x3E
    NOTIFY_SERVER_CLOSE_FUNCTION = 0x3F
    # ── Team ──
    NOTIFY_AWARD_ALL_ITEMS = 0x45
    NOTIFY_ALL_MEMBER_READY = 0x46
    NOTIFY_CAPTAIN_READY = 0x47
    # ── Privilege / Quest / BattlePass ──
    NOTIFY_USER_ALL_SOURCE_PRIVILEGE_EFFECT_DATA = 0x4A
    NOTIFY_QUEST_ACCEPT = 0x4B
    NOTIFY_QUEST_CHANGE_STEP = 0x4C
    NOTIFY_QUEST_GIVE_UP = 0x4D
    NOTIFY_QUEST_COMPLETE = 0x4E
    NOTIFY_USER_ALL_VALID_BATTLE_PASS_DATA = 0x4F
    NOTIFY_NOTICE_MULTI_LANGUAGE_INFO = 0x53
    # ── Skill / Combat (battle server 0x3000 range) ──
    QTE_BEGIN = 0x3001                        # 12289
    SYNC_CLIENT_USE_SKILL = 0x3002            # 12290
    NOTIFY_BUFF_CHANGE = 0x3003               # 12291
    SYNC_SERVER_SKILL_STAGE_END = 0x3004      # 12292
    SYNC_SERVER_SKILL_END = 0x3005            # 12293
    # ── Quest (0x6000 range) ──
    QUEST_ABORT = 0x6001
    # ── Shop (0x29000 range) ──
    NOTIFY_BUY_SHOP_RESULT = 0x29001
    NOTIFY_SHOP_ITEM_CAN_BUY = 0x29002
    # ── World Boss (0x46000 range) ──
    WORLD_BOSS_RANK_INFO_NTF = 0x46001
    # ── Match (0x48000 range) ──
    ENTER_MATCH_RESULT_NTF = 0x48001
    # ── Ride (0x4D000 range) ──
    NOTIFY_DRIVER_APPLY_RIDE = 0x4D001
    NOTIFY_INVITE_APPLY_RIDE = 0x4D002
    NOTIFY_RIDE_IS_AGREE = 0x4D003
    # ── Payment (0x51000 range) ──
    NOTIFY_PAY_INFO = 0x51001
    # ── Life Profession (0x52000 range) ──
    NOTIFY_LIFE_PROFESSION_WORK_HISTORY_CHANGE = 0x52001
    NOTIFY_LIFE_PROFESSION_UNLOCK_RECIPE = 0x52002
    # ── Sign-in (0x5E000 range) ──
    SIGN_REWARD_NOTIFY = 0x5E001
    # ── Random (0x6B000 range) ──
    NOTIFY_ENTRY_RANDOM_DATA = 0x6B001


# Reverse lookup for notify method names (for logging)
_NOTIFY_METHOD_NAMES = {
    v: k for k, v in vars(NotifyMethod).items()
    if isinstance(v, int) and not k.startswith('_')
}


class AttrType:
    NAME = 0x01
    ID = 0x0A
    # ── Player info attrs ──
    COMBAT_STATE = 104           # AttrCombatState  — 0=out, 1=in combat
    COMBAT_STATE_TIME = 114      # AttrCombatStateTime — transition timestamp
    SEASON_LV = 196              # AttrSeasonLv (season star rank — display extra level)
    PROFESSION_ID = 0xDC         # 220 = AttrProfessionId
    MONSTER_SEASON_LEVEL = 462   # AttrMonsterSeasonLevel — monster season level
    FIGHT_POINT = 0x272E         # 10030
    LEVEL = 0x2710               # 10000
    RANK_LEVEL = 0x274C          # 10060
    SEASON_LEVEL = 10070         # AttrSeasonLevel (from StarResonanceDps)
    CRI = 0x2B66                 # 11110
    LUCKY = 0x2B7A               # 11130
    HP = 0x2C2E                  # 11310
    MAX_HP = 0x2C38              # 11320
    MAX_HP_VARIANTS = (11321, 11322, 11323, 11324, 11325)  # Base/Pct/Add/Total/WithShield
    ELEMENT_FLAG = 0x646D6C
    REDUCTION_LEVEL = 0x64696D
    ENERGY_FLAG = 0x543CD3C6     # Flag field, not a stamina value
    # ── Season strength (梦境强度) ──
    SEASON_STRENGTH = 11440      # AttrSeasonStrength base
    SEASON_STRENGTH_VARIANTS = (11440, 11441, 11442, 11443, 11444, 11445)
    # ── Stamina ──
    STA_MAX_FALLBACK = 11324
    STA_RATIO_SET = (11850, 11851, 11852)
    # ── CD modifier attrs: base + Total variants ──
    SKILL_CD_TOTAL = 11751      # AttrSkillCDTotal (server-computed sum)
    SKILL_CD_PCT_TOTAL = 11761  # AttrSkillCDPCTTotal (server-computed sum)
    CD_ACCELERATE_PCT_TOTAL = 11961  # AttrCdAcceleratePctTotal (server-computed sum)
    # ── Boss / Monster mechanic attrs (from SRDPS enum_e_attr_type.proto) ──
    MAX_EXTINCTION = 440        # Breaking bar max (extinction gauge)
    EXTINCTION = 441            # Breaking bar current
    MAX_STUNNED = 442           # Stun gauge max
    STUNNED = 443               # Stun gauge current
    IN_OVERDRIVE = 444          # Boss overdrive (enraged) flag
    IS_LOCK_STUNNED = 445       # Locked-stun flag
    STOP_BREAKING_TICKING = 453 # AttrStopBreakingBarTickingFlag
    BREAKING_STAGE = 455        # Breaking phase stage (0/1/2...)
    SHIELD_LIST = 60050         # AttrShieldList — repeated ShieldInfo message
    STUNNED_DAMAGE_PCT = 11830  # Bonus damage % during stun
    # ── Player CD-modifier attrs (from resonance-logs-cn skill_cd_monitor.rs) ──
    SKILL_CD = 11750            # AttrSkillCD — flat CD reduction (ms)
    SKILL_CD_PCT = 11760        # AttrSkillCDPCT — percent CD reduction (万分比, /10000)
    CD_ACCELERATE_PCT = 11960   # AttrCdAcceleratePct — CD acceleration (万分比, /10000)
    # ── Fight resource / general CD speed ──
    FIGHT_RES_CD_SPEED_PCT = 11980       # AttrFightResCdSpeedPct — CD speed pct (/10000)
    FIGHT_RES_CD_SPEED_PCT_TOTAL = 11981 # AttrFightResCdSpeedPctTotal
    # ── Base stats (from EAttrType enum) ──
    STR = 11010;              STR_TOTAL = 11011
    INT_ATTR = 11020;         INT_ATTR_TOTAL = 11021
    DEX = 11030;              DEX_TOTAL = 11031
    VIT = 11040;              VIT_TOTAL = 11041
    # ── Combat stats ──
    HASTE = 11120;            HASTE_TOTAL = 11121
    MASTERY = 11140;          MASTERY_TOTAL = 11141
    VERSATILITY = 11150;      VERSATILITY_TOTAL = 11151
    ATTACK = 11330;           ATTACK_TOTAL = 11331
    M_ATTACK = 11340;         M_ATTACK_TOTAL = 11341
    DEFENSE = 11350;          DEFENSE_TOTAL = 11351
    M_DEFENSE = 11360;        M_DEFENSE_TOTAL = 11361
    CRIT_RATE = 11110;        CRIT_RATE_TOTAL = 11111
    CRIT_DAMAGE = 12510;      CRIT_DAMAGE_TOTAL = 12511
    ATTACK_SPEED_PCT = 11720; ATTACK_SPEED_PCT_TOTAL = 11721
    CAST_SPEED_PCT = 11730;   CAST_SPEED_PCT_TOTAL = 11731
    CHARGE_SPEED_PCT = 11740; CHARGE_SPEED_PCT_TOTAL = 11741
    HEAL_POWER = 11790;       HEAL_POWER_TOTAL = 11791
    # ── Damage modifiers ──
    DAM_INC = 12550;          DAM_INC_TOTAL = 12551
    M_DAM_INC = 12570;        M_DAM_INC_TOTAL = 12571
    BOSS_DAM_INC = 12630;     BOSS_DAM_INC_TOTAL = 12631
    # ── Damage resistance ──
    DAM_RES = 12560;          DAM_RES_TOTAL = 12561
    M_DAM_RES = 12580;        M_DAM_RES_TOTAL = 12581
    # ── Origin energy ──
    ORIGIN_ENERGY = 20010     # AttrOriginEnergy
    MAX_ORIGIN_ENERGY = 20020 # AttrMaxOriginEnergy
    # ── Monster-specific extended ──
    STATE = 11               # AttrState (EActorState)
    DEAD_TYPE = 78           # AttrDeadType
    DEAD_TIME = 206          # AttrDeadTime
    FIRST_ATTACK = 456       # AttrFirstAttack flag
    HATED_CHAR_ID = 471      # AttrHatedCharId — aggro target
    HATED_CHAR_JOB = 472     # AttrHatedCharJob
    HATED_CHAR_NAME = 473    # AttrHatedCharName
    HATED_CHAR_LIST = 474    # AttrHatedCharList
    # ── Sets for batch matching ──
    _BASE_STAT_IDS = frozenset({
        11010, 11011, 11020, 11021, 11030, 11031, 11040, 11041,
        11120, 11121, 11140, 11141, 11150, 11151,
    })
    _COMBAT_STAT_IDS = frozenset({
        11330, 11331, 11340, 11341, 11350, 11351, 11360, 11361,
        11110, 11111, 12510, 12511, 11720, 11721, 11730, 11731,
        11740, 11741, 11790, 11791,
    })
    _DAMAGE_MOD_IDS = frozenset({
        12550, 12551, 12570, 12571, 12630, 12631,
        12560, 12561, 12580, 12581,
    })
    _MONSTER_EXTENDED_IDS = frozenset({11, 78, 206, 456, 471, 472, 473, 474})


SERVICE_UUID_C3SB = 0x0000000063335342

# ── Boss-relevant EBuffEventType values (from SRDPS enum_e_buff_event_type.proto) ──
class BuffEventType:
    HOST_DEATH = 12
    BODY_PART_DEAD = 15
    BODY_PART_STATE_CHANGE = 17
    SHIELD_BROKEN = 47
    SUPER_ARMOR_BROKEN = 51
    ENTER_BREAKING = 58
    INTO_FRACTURE_STATE = 88

# Which BuffEventType values we want to emit as boss events
_BOSS_BUFF_EVENTS = frozenset({
    BuffEventType.HOST_DEATH,
    BuffEventType.BODY_PART_DEAD,
    BuffEventType.BODY_PART_STATE_CHANGE,
    BuffEventType.SHIELD_BROKEN,
    BuffEventType.SUPER_ARMOR_BROKEN,
    BuffEventType.ENTER_BREAKING,
    BuffEventType.INTO_FRACTURE_STATE,
})

# EDamageType enum (from SRDPS enum_e_damage_type.proto)
class DamageType:
    NORMAL = 0
    MISS = 1
    HEAL = 2
    IMMUNE = 3
    FALL = 4
    ABSORBED = 5

# Entity types in SyncNearEntities.Appear
class EntityType:
    CHAR = 1       # Player character
    MONSTER = 3    # Monster entity
    NPC = 5        # NPC
    COLLECT = 7    # Collectible

# EDisappearType enum (from star_resonance.proto)
class DisappearType:
    DISAPPEAR_NONE = 0
    DISAPPEAR_DEAD = 1
    DISAPPEAR_FAR_AWAY = 2
    DISAPPEAR_REGION = 3
    DISAPPEAR_TELEPORT = 4
    DISAPPEAR_ENTER_VEHICLE = 5
    DISAPPEAR_ENTER_RIDE = 6

# EAppearType enum (from star_resonance.proto)
class AppearType:
    APPEAR_NONE = 0
    APPEAR_BORN = 1
    APPEAR_NEAR = 2
    APPEAR_REGION = 3
    APPEAR_TELEPORT = 4
    APPEAR_EXIT_VEHICLE = 5
    APPEAR_REVIVE = 6

# ESkillCDType enum (from star_resonance.proto)
class SkillCDType:
    NORMAL = 0
    CHARGE = 1           # Charge-type skill (multiple charges)
    PROFESSION_HOLD = 2  # Hold-type profession skill

# CharSerialize field definitions (all 98 fields mapped)
class CharField:
    CHAR_ID = 1
    CHAR_BASE = 2
    SCENE_DATA = 3
    SCENE_LUA_DATA = 4
    PIONEER_DATA = 5
    BUFF_DB_INFO = 6
    ITEM_PACKAGE = 7
    QUEST_DATA_LIST = 8
    SETTING_DATA = 9
    MISC_INFO = 10
    EXCHANGE_ITEM = 11
    EQUIP_LIST = 12
    ENERGY_ITEM = 13
    MAP_DATA = 14
    DUNGEON_LIST = 15
    USER_FIGHT_ATTR = 16
    FASHION_MGR = 17
    PROFILE_LIST = 18
    PLAY_HELPER = 19
    COUNTER_LIST = 20
    PERSONAL_OBJECT = 21
    ROLE_LEVEL = 22
    PIVOT = 23
    TRANSFER_POINT = 24
    PLANET_MEMORY = 25
    SEASON_TARGET = 26
    RED_DOT_DATA = 27
    RESONANCE = 28
    CUTS_STATE = 29
    INVESTIGATE_LIST = 30
    PARKOUR_RECORD_LIST = 31
    INTERACTION_INFO = 32
    SEASON_QUEST_LIST = 33
    ROLE_FACE = 34
    MAP_BOOK_INFO_LIST = 35
    FUNCTION_DATA = 36
    ANTI_ADDICTION_INFO = 37
    MONSTER_EXPLORE_LIST = 38
    SHOW_PIECE_DATA = 39
    # Fields 40, 41 do not exist in proto
    COLLECTION_BOOK = 42
    NOT_GET_PROCEED_AWARD_INFO = 43
    COOK_LIST = 44
    TIMER_REFRESH_DATA_LIST = 45
    CHALLENGE_DUNGEON_INFO = 46
    SYNC_AWARD_DATA = 47
    SEASON_ACHIEVEMENT_LIST = 48
    SEASON_RANK_LIST = 49
    SEASON_CENTER = 50
    PERSONAL_ZONE = 51
    SEASON_MEDAL_INFO = 52
    COMMUNITY_HOME_DATA = 53
    SEASON_ACTIVATION = 54
    SLOTS = 55
    MONSTER_HUNT_INFO = 56
    MOD = 57
    WORLD_EVENT_MAP = 58
    FISH_SETTING = 59
    FREIGHT_DATA = 60
    PROFESSION_LIST = 61
    TRIAL_ROAD = 62
    GASHA_DATA = 63
    SHOP_DATA = 64
    PERSONAL_WORLD_BOSS_INFO = 65
    CRAFT_ENERGY_RECORD = 66
    WEEKLY_TOWER_RECORD = 67
    CUT_SCENE_INFOS = 68
    USER_RECOMMEND_PLAY_DATA = 69
    RIDE_LIST = 70
    PAY_ORDER_LIST = 71
    LIFE_PROFESSION = 72
    LIFE_PROFESSION_WORK = 73
    USER_ACTIVITY_LIST = 74
    PLAYER_RECORD = 75
    DROP_CONTAINER_INFO = 76
    MONTHLY_CARD = 77
    FASHION_BENEFIT = 78
    ITEM_CURRENCY = 79
    PRIVILEGE_EFFECT_DATA = 80
    TREASURE = 81
    UNLOCK_EMOJI_DATA = 82
    PLAYER_ORDER_CONTAINER_INFO = 83
    PLAYER_BOX = 84
    LAUNCH_PRIVILEGE_DATA = 85
    BATTLE_PASS_DATA = 86
    RECHARGE_DATA = 87
    LUCKY_VALUE_MGR = 88
    HANDBOOK_DATA = 89
    MASTER_MODE_DUNGEON_INFO = 90
    STATISTICS_DATA = 91
    COMPENSATION_STATISTICS = 92
    BUBBLE_ACT_DATA = 93
    MAIL_CLAIMED_INFO = 94
    NEWBIE_DATA = 95
    FIGHT_POINT_DATA = 96
    SIGN_INFO = 97
    CHAR_STATISTICS_DATA = 98

# Reverse mapping: field_number → field_name (for logging)
CHAR_FIELD_NAMES = {
    v: k for k, v in vars(CharField).items()
    if isinstance(v, int) and not k.startswith('_')
}

# CharSerialize fields that have dedicated handlers (skip in generic loop)
_HANDLED_CHAR_FIELDS = frozenset({
    CharField.CHAR_ID, CharField.CHAR_BASE, CharField.ENERGY_ITEM,
    CharField.USER_FIGHT_ATTR, CharField.ROLE_LEVEL,
    CharField.SEASON_CENTER, CharField.SEASON_MEDAL_INFO,
    CharField.SLOTS, CharField.MONSTER_HUNT_INFO,
    CharField.PROFESSION_LIST, CharField.BATTLE_PASS_DATA,
})

# AoiSyncDelta field names (from star_resonance.proto)
class DeltaField:
    UUID = 1
    ATTRS = 2                    # AttrCollection
    TEMP_ATTRS = 3               # TempAttrCollection
    EVENT_DATA_LIST = 4          # Repeated EventData
    BULLET_EVENT = 5             # BulletEvent
    BODY_PART_INFOS = 6          # ActorBodyPartInfos
    SKILL_EFFECTS = 7            # SkillEffect (damage)
    PASSIVE_SKILL_INFOS = 8      # SeqPassiveSkillInfo
    PASSIVE_SKILL_END_INFOS = 9  # SeqPassiveSkillEndInfo
    BUFF_INFOS = 10              # BuffInfoSync
    BUFF_EFFECT = 11             # BuffEffectSync
    FAKE_BULLETS = 12            # SeqFakeBullet
    RIDE_QUEUE_CHANGE = 13       # MagneticRideQueueChangeInfoList

# Entity field names (SyncNearEntities.Appear)
class EntityField:
    UUID = 1
    ENT_TYPE = 2                 # EEntityType
    ATTRS = 3                    # AttrCollection
    TEMP_ATTRS = 4               # TempAttrCollection
    BODY_PART_INFOS = 5          # ActorBodyPartInfos
    PASSIVE_SKILL_INFOS = 6      # SeqPassiveSkillInfo
    BUFF_INFOS = 7               # BuffInfoSync
    BUFF_EFFECT = 8              # BuffEffectSync
    APPEAR_TYPE = 9              # EAppearType
    RIDE_QUEUE_CHANGE = 10       # map

# AoiSyncToMeDelta field names
class ToMeDeltaField:
    BASE_DELTA = 1          # AoiSyncDelta
    SYNC_HATE_IDS = 2       # repeated int64
    SYNC_SKILL_CDS = 3      # repeated SkillCD
    FIGHT_RES_CDS = 4       # repeated FightResCD
    UUID = 5                # int64 (self UUID)

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



# UUID helpers


def _is_player(uuid: int) -> bool:
    return (uuid & 0xFFFF) == 640


def _is_monster(uuid: int) -> bool:
    low = uuid & 0xFFFF
    return low == 64 or low == 32832  # 0x0040 or 0x8040


def _uuid_to_uid(uuid: int) -> int:
    return uuid >> 16


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


class MonsterData:
    """Tracks one monster entity's parsed state."""
    __slots__ = ('uuid', 'uid', 'name', 'template_id',
                 'hp', 'max_hp', 'season_level',
                 'breaking_stage', 'extinction', 'max_extinction',
                 'stunned', 'max_stunned', 'in_overdrive',
                 'is_lock_stunned', 'stop_breaking_ticking',
                 'shield_active', 'shield_total', 'shield_max_total',
                 'is_dead', 'last_update',
                 # Extended monster fields (from AttrCollection)
                 'state', 'dead_type', 'dead_time',
                 'first_attack', 'hated_char_id', 'hated_char_name',
                 'buff_list')

    def __init__(self, uuid: int, uid: int = 0):
        self.uuid = uuid
        self.uid = uid or _uuid_to_uid(uuid)
        self.name: str = ''
        self.template_id: int = 0
        self.hp: int = 0
        self.max_hp: int = 0
        self.season_level: int = 0
        self.breaking_stage: int = -1   # -1 = not received; 0 = Breaking (broken), 1 = BreakEnd (normal)
        self.extinction: int = 0
        self.max_extinction: int = 0
        self.stunned: int = 0
        self.max_stunned: int = 0
        self.in_overdrive: bool = False
        self.is_lock_stunned: bool = False
        self.stop_breaking_ticking: bool = False
        self.shield_active: bool = False
        self.shield_total: int = 0
        self.shield_max_total: int = 0
        self.is_dead: bool = False
        self.last_update: float = 0.0
        # Extended monster fields
        self.state: int = 0              # EActorState
        self.dead_type: int = 0          # AttrDeadType
        self.dead_time: int = 0          # AttrDeadTime (timestamp)
        self.first_attack: bool = False  # AttrFirstAttack flag
        self.hated_char_id: int = 0      # Aggro target UID
        self.hated_char_name: str = ''   # Aggro target name
        self.buff_list: list = []        # Active buffs on this monster

    def to_dict(self) -> dict:
        # Break gauge: extinction works like HP — starts at max, depletes to 0.
        # remaining = current / max  →  100% = undamaged, 0% = fully broken.
        _has_break = self.max_extinction > 0 or self.max_stunned > 0
        if self.max_extinction > 0:
            _ext_pct = max(0.0, self.extinction / self.max_extinction)
        elif self.max_stunned > 0:
            _ext_pct = max(0.0, self.stunned / self.max_stunned)
        else:
            _ext_pct = 0.0
        return {
            'uuid': self.uuid,
            'uid': self.uid,
            'name': self.name,
            'template_id': self.template_id,
            'season_level': self.season_level,
            'hp': self.hp,
            'max_hp': self.max_hp,
            'hp_pct': (self.hp / self.max_hp) if self.max_hp > 0 else 0.0,
            'has_break_data': _has_break,
            'breaking_stage': self.breaking_stage,
            'extinction': self.extinction,
            'max_extinction': self.max_extinction,
            'extinction_pct': _ext_pct,
            'stunned': self.stunned,
            'max_stunned': self.max_stunned,
            'in_overdrive': self.in_overdrive,
            'stop_breaking_ticking': self.stop_breaking_ticking,
            'shield_active': self.shield_active,
            'shield_total': self.shield_total,
            'shield_max_total': self.shield_max_total,
            'shield_pct': (self.shield_total / self.shield_max_total) if self.shield_max_total > 0 else 0.0,
            'is_dead': self.is_dead,
        }


class PlayerData:
    """Tracks one player's parsed data."""
    __slots__ = ('uid', 'name', 'level', 'rank_level', 'season_level',
                 'level_extra', 'level_extra_source',
                 'level_extra_pending_source', 'level_extra_pending_value',
                 'level_extra_pending_hits',
                 'fight_point', 'season_strength',
                 'in_combat', 'combat_state_time',
                 'hp', 'max_hp', 'energy', 'energy_limit', 'extra_energy_limit',
                 'energy_info_value', 'energy_valid', 'energy_source_priority',
                 'resource_values', 'energy_info_map', 'stamina_resource_id',
                 'stamina_ratio', 'stamina_ratio_observed_at',
                 'season_medal_level', 'monster_hunt_level',
                 'battlepass_level', 'battlepass_data_level',
                 'profession', 'profession_id', 'sub_profession',
                 'hp_from_full_sync',
                 'skill_slot_map', 'skill_level_info_map',
                 'slot_bar_map',
                 'skill_cd_map', 'skill_last_use_at', 'skill_seen_ids',
                 '_inferred_skill_count',
                 'fight_res_cd_map',
                 'server_time_offset_ms',
                 'attr_skill_cd', 'attr_skill_cd_pct', 'attr_cd_accelerate_pct',
                 'temp_attr_cd_pct', 'temp_attr_cd_fixed', 'temp_attr_cd_accel',
                 'attr_fight_res_cd_speed',
                 'cd_speed_ratio',
                 # ── Extended combat stats (from AttrCollection) ──
                 'attack', 'magic_attack', 'defense', 'magic_defense',
                 'crit_rate', 'crit_damage',
                 'attack_speed_pct', 'cast_speed_pct', 'charge_speed_pct',
                 'heal_power',
                 'dam_inc', 'mdam_inc', 'boss_dam_inc',
                 # ── Buff tracking ──
                 'buff_list',
                 # ── Scene / dungeon ──
                 'scene_id', 'dungeon_id',
                 # ── Extended CharSerialize data (decoded generic) ──
                 'extended_data')

    def __init__(self, uid: int):
        self.uid = uid
        self.name: str = ''
        self.level: int = 0          # Visible character level from AttrLevel 0x2710
        self.rank_level: int = 0     # Rank/star level from AttrRankLevel 0x274C
        self.season_level: int = 0   # Seasonal extra level shown as (+XX)
        self.level_extra: int = 0
        self.level_extra_source: str = ''
        self.level_extra_pending_source: str = ''
        self.level_extra_pending_value: int = 0
        self.level_extra_pending_hits: int = 0
        self.fight_point: int = 0
        self.season_strength: int = 0     # AttrSeasonStrength (11440) — 梦境强度
        self.in_combat: bool = False      # AttrCombatState (104)
        self.combat_state_time: int = 0   # AttrCombatStateTime (114) — timestamp ms
        self.hp: int = 0
        self.max_hp: int = 0
        self.energy: float = 0.0
        self.energy_limit: int = 0
        self.extra_energy_limit: int = 0
        self.energy_info_value: int = 0
        self.energy_valid: bool = False
        self.energy_source_priority: int = 0
        self.resource_values: Dict[int, int] = {}
        self.energy_info_map: Dict[int, Dict[str, int]] = {}
        self.stamina_resource_id: int = 0
        self.stamina_ratio: float = -1.0
        self.stamina_ratio_observed_at: float = 0.0
        self.season_medal_level: int = 0
        self.monster_hunt_level: int = 0
        self.battlepass_level: int = 0
        self.battlepass_data_level: int = 0
        self.profession: str = ''
        self.profession_id: int = 0
        self.sub_profession: str = ''     # 子职业分支名 (e.g. '防盾', '光盾')
        self.hp_from_full_sync: bool = False  # Whether HP came from a trusted full sync
        self.skill_slot_map: Dict[int, int] = {}
        self.skill_level_info_map: Dict[int, Dict[str, int]] = {}
        self.slot_bar_map: Dict[int, int] = {}  # CharSerialize.Slots (field 55): slot_id → skill_id
        self.skill_cd_map: Dict[int, Dict[str, Any]] = {}
        self.skill_last_use_at: Dict[int, float] = {}
        self.skill_seen_ids = []
        self._inferred_skill_count: int = 0
        self.fight_res_cd_map: Dict[int, Dict[str, Any]] = {}  # res_id → FightResCD state
        self.server_time_offset_ms: Optional[float] = None
        # Entity-level CD modifiers (from AttrCollection + TempAttr)
        self.attr_skill_cd: int = 0           # AttrSkillCD 11750 — flat CD reduction ms
        self.attr_skill_cd_pct: int = 0       # AttrSkillCDPCT 11760 — pct /10000
        self.attr_cd_accelerate_pct: int = 0  # AttrCdAcceleratePct 11960 — accel /10000
        self.temp_attr_cd_pct: int = 0        # TempAttr type 100 — buff pct CD reduce /10000
        self.temp_attr_cd_fixed: int = 0      # TempAttr type 101 — buff flat CD reduce ms
        self.temp_attr_cd_accel: int = 0      # TempAttr type 103 — buff CD accelerate /10000
        # FightResCdSpeedPct (11980) — CD speed/duration modifier /10000
        # 10000 = base (1x), values below 10000 → shorter CDs
        self.attr_fight_res_cd_speed: int = 0
        # Observed VCD speed ratio: how fast valid_cd_time ticks vs real time
        # e.g. 2.3 means VCD advances 2.3ms per 1ms real time (CD acceleration)
        self.cd_speed_ratio: float = 1.0
        # Extended combat stats (from AttrCollection — Total variants preferred)
        self.attack: int = 0             # AttrAttack/Total (11330/11331)
        self.magic_attack: int = 0       # AttrMAttack/Total (11340/11341)
        self.defense: int = 0            # AttrDefense/Total (11350/11351)
        self.magic_defense: int = 0      # AttrMDefense/Total (11360/11361)
        self.crit_rate: int = 0          # AttrCri/Total (11110/11111) 万分比
        self.crit_damage: int = 0        # AttrCritDamage/Total (12510/12511) 万分比
        self.attack_speed_pct: int = 0   # AttrAttackSpeedPCT (11720) 万分比
        self.cast_speed_pct: int = 0     # AttrCastSpeedPCT (11730) 万分比
        self.charge_speed_pct: int = 0   # AttrChargeSpeedPCT (11740) 万分比
        self.heal_power: int = 0         # AttrHeal (11790)
        self.dam_inc: int = 0            # AttrDamInc (12550) physical dmg inc 万分比
        self.mdam_inc: int = 0           # AttrMdamInc (12570) magic dmg inc 万分比
        self.boss_dam_inc: int = 0       # AttrBossDamInc (12630) boss dmg inc 万分比
        # Buff tracking from BuffInfoSync / NotifyBuffChange
        self.buff_list: list = []        # [{buff_id, begin_time, duration, layer, ...}]
        # Scene / dungeon context
        self.scene_id: int = 0           # from CharSerialize.SceneData or SyncDungeonData
        self.dungeon_id: int = 0         # current dungeon ID
        # Extended CharSerialize data — decoded but not actively used by overlay
        # Keys are CharField names, values are decoded field dicts
        self.extended_data: Dict[str, Any] = {}


def _fields_to_debug_dict(fields: dict, max_depth: int = 3) -> dict:
    """Convert protobuf decoded fields dict to JSON-serializable debug dict.

    Recursively decodes nested protobuf bytes up to max_depth.
    Used for generic CharSerialize / Entity field logging.
    """
    if max_depth <= 0:
        return {'_truncated': True}
    result = {}
    for k, v_list in fields.items():
        values = []
        for v in v_list:
            if isinstance(v, bytes):
                if max_depth > 1:
                    try:
                        sub = _decode_fields(v)
                        values.append(_fields_to_debug_dict(sub, max_depth - 1))
                    except Exception:
                        values.append({'_hex': v.hex()[:128]})
                else:
                    values.append({'_hex': v.hex()[:128]})
            elif isinstance(v, int):
                values.append(v)
            elif isinstance(v, float):
                values.append(v)
            else:
                values.append(str(v)[:64])
        result[str(k)] = values[0] if len(values) == 1 else values
    return result


def _decode_buff_info_sync(data: bytes) -> list:
    """Decode BuffInfoSync { repeated BuffInfo Infos = 1 } from entity/delta.

    BuffInfo proto:
        int32 BuffId = 1;
        int64 BeginTime = 2;
        int32 Duration = 3;
        int32 Layer = 4;
        int64 OuterUuid = 5;
        int32 SkillId = 6;
        int32 SkillLevelId = 7;
        int32 CurLayer = 8;
        float LeftTime = 9;
    """
    buffs = []
    try:
        outer = _decode_fields(data)
        for buff_raw in outer.get(1, []):
            if not isinstance(buff_raw, bytes):
                continue
            bf = _decode_fields(buff_raw)
            buff_id = bf.get(1, [0])[0]
            if not buff_id:
                continue
            buffs.append({
                'buff_id': buff_id,
                'begin_time': _varint_to_int64(bf.get(2, [0])[0]),
                'duration': bf.get(3, [0])[0],
                'layer': bf.get(4, [0])[0],
                'outer_uuid': _varint_to_int64(bf.get(5, [0])[0]),
                'skill_id': bf.get(6, [0])[0],
                'skill_level_id': bf.get(7, [0])[0],
                'cur_layer': bf.get(8, [0])[0],
            })
    except Exception as e:
        logger.debug(f'[Parser] _decode_buff_info_sync error: {e}')
    return buffs


def _decode_scene_data(data: bytes) -> dict:
    """Decode CharSerialize.SceneData (field 3) for scene/map identification.

    SceneData proto:
        int32 SceneId = 1;
        int32 MapId = 2;
        ScenePosition Position = 3;
        ...
    """
    try:
        sf = _decode_fields(data)
        return {
            'scene_id': sf.get(1, [0])[0],
            'map_id': sf.get(2, [0])[0],
        }
    except Exception:
        return {}


def _decode_buff_db_info(data: bytes) -> list:
    """Decode CharSerialize.BuffDBInfo (field 6) — active buffs on self.

    BuffDBInfo proto:
        repeated BuffDBData Infos = 1;
    BuffDBData proto:
        int32 BuffId = 1;
        int64 BeginTime = 2;
        int32 Duration = 3;
        int32 Layer = 4;
        int64 OuterUuid = 5;
        int32 SkillId = 6;
        int32 SkillLevelId = 7;
    """
    buffs = []
    try:
        outer = _decode_fields(data)
        for buf_raw in outer.get(1, []):
            if not isinstance(buf_raw, bytes):
                continue
            bf = _decode_fields(buf_raw)
            buff_id = bf.get(1, [0])[0]
            if not buff_id:
                continue
            buffs.append({
                'buff_id': buff_id,
                'begin_time': _varint_to_int64(bf.get(2, [0])[0]),
                'duration': bf.get(3, [0])[0],
                'layer': bf.get(4, [0])[0],
                'skill_id': bf.get(6, [0])[0],
                'skill_level_id': bf.get(7, [0])[0],
            })
    except Exception as e:
        logger.debug(f'[Parser] _decode_buff_db_info error: {e}')
    return buffs


def _decode_dungeon_list(data: bytes) -> dict:
    """Decode CharSerialize.DungeonList (field 15) for dungeon context.

    DungeonList proto:
        int32 CurDungeonId = 1;
        repeated DungeonData Datas = 2;
    """
    try:
        df = _decode_fields(data)
        raw_id = df.get(1, [0])[0]
        # _decode_fields may return bytes for varint fields — ensure int
        if isinstance(raw_id, bytes):
            raw_id = int.from_bytes(raw_id, 'little', signed=False) if raw_id else 0
        return {
            'cur_dungeon_id': int(raw_id),
            'dungeon_count': len(df.get(2, [])),
        }
    except Exception:
        return {}


def _decode_equip_list(data: bytes) -> dict:
    """Decode CharSerialize.EquipList (field 12) — equipped items summary.

    EquipList proto:
        repeated EquipInfo Equips = 1;
    EquipInfo proto:
        int32 Pos = 1;
        int64 Uid = 2;
        int32 TableId = 3;
        ...
    """
    equips = []
    try:
        outer = _decode_fields(data)
        for eq_raw in outer.get(1, []):
            if not isinstance(eq_raw, bytes):
                continue
            ef = _decode_fields(eq_raw)
            equips.append({
                'pos': ef.get(1, [0])[0],
                'uid': _varint_to_int64(ef.get(2, [0])[0]),
                'table_id': ef.get(3, [0])[0],
            })
    except Exception as e:
        logger.debug(f'[Parser] _decode_equip_list error: {e}')
    return {'equip_count': len(equips), 'equips': equips}


def _decode_resonance_data(data: bytes) -> dict:
    """Decode CharSerialize.Resonance (field 28) — resonance system.

    Resonance proto:
        repeated ResonanceSlot Slots = 1;
        int32 ActivePlan = 2;
        ...
    """
    try:
        rf = _decode_fields(data)
        return {
            'slot_count': len(rf.get(1, [])),
            'active_plan': rf.get(2, [0])[0],
        }
    except Exception:
        return {}


def _decode_energy_item(data: bytes) -> Dict[str, Any]:
    """Decode CharSerialize.EnergyItem (field 13) from the upstream schema."""
    result = {
        'energy_limit': 0,
        'extra_energy_limit': 0,
        'energy_values': [],
        'unlock_nums': [],
        'current_energy_value': 0,
        'derived_total_limit': 0,
        'energy_info_map': {},
    }
    if not isinstance(data, bytes) or not data:
        return result

    fields = _decode_fields(data)
    energy_limit = fields.get(1, [0])[0]
    extra_energy_limit = fields.get(2, [0])[0]
    if isinstance(energy_limit, int) and energy_limit > 0:
        result['energy_limit'] = energy_limit
    if isinstance(extra_energy_limit, int) and extra_energy_limit > 0:
        result['extra_energy_limit'] = extra_energy_limit

    for entry_raw in fields.get(3, []):
        if not isinstance(entry_raw, bytes):
            continue
        entry = _decode_fields(entry_raw)
        energy_id_raw = entry.get(1, [None])[0]
        value_raw = entry.get(2, [None])[0]
        if not isinstance(value_raw, bytes):
            continue
        energy_info = _decode_fields(value_raw)
        energy_value = energy_info.get(1, [None])[0]
        unlock_num = energy_info.get(2, [None])[0]
        energy_id = _varint_to_int32(energy_id_raw) if isinstance(energy_id_raw, int) else 0
        entry_info = {
            'energy_value': _varint_to_int32(energy_value) if isinstance(energy_value, int) else 0,
            'unlock_num': _varint_to_int32(unlock_num) if isinstance(unlock_num, int) else 0,
            'item_info_count': len(energy_info.get(3, [])),
        }
        if isinstance(energy_value, int) and energy_value >= 0:
            result['energy_values'].append(energy_value)
        if isinstance(unlock_num, int) and unlock_num >= 0:
            result['unlock_nums'].append(unlock_num)
        if energy_id > 0:
            result['energy_info_map'][energy_id] = entry_info

    total_limit = result['energy_limit'] + result['extra_energy_limit']
    sane_limit = total_limit if total_limit > 0 else 20000
    sane_values = [v for v in result['energy_values'] if 0 <= v <= sane_limit]
    if sane_values:
        result['derived_total_limit'] = max(sane_values)
    if sane_values:
        result['current_energy_value'] = max(sane_values)

    return result


def _decode_battlepass_level(data: bytes) -> int:
    """Decode SeasonCenter.BattlePass.Level from CharSerialize field 50."""
    if not isinstance(data, bytes) or not data:
        return 0

    season_center = _decode_fields(data)
    battlepass_raw = season_center.get(2, [None])[0]
    if not isinstance(battlepass_raw, bytes):
        return 0

    battlepass = _decode_fields(battlepass_raw)
    level_raw = battlepass.get(2, [None])[0]
    if isinstance(level_raw, int) and level_raw > 0:
        return level_raw
    return 0


def _decode_season_medal_level(data: bytes) -> int:
    """Decode CoreHoleInfo.HoleLevel from SeasonMedalInfo (field 52).

    Returns the normalized core hole level as the season medal value.
    NormalHoleInfos are individual skill-tree nodes — NOT additive to displayed level.
    The accurate +N comes from AttrSeasonLevel (10070) or AttrSeasonLv (196) attrs
    when available; this core level serves as a fallback.
    """
    if not isinstance(data, bytes) or not data:
        return 0

    medal = _decode_fields(data)
    core_level_raw = 0
    core_level_norm = 0

    # CoreHoleInfo (field 3) → MedalHole.HoleLevel (field 2)
    core_hole_raw = medal.get(3, [None])[0]
    if isinstance(core_hole_raw, bytes):
        core_hole = _decode_fields(core_hole_raw)
        core_level_raw = core_hole.get(2, [None])[0] or 0
        if isinstance(core_level_raw, int) and core_level_raw > 0:
            core_level_norm = _normalize_season_medal_level(core_level_raw)

    if core_level_norm > 0:
        logger.info(
            f'[Parser] SeasonMedalInfo core_raw={core_level_raw} '
            f'core_norm={core_level_norm}'
        )

    return core_level_norm


def _decode_monster_hunt_level(data: bytes) -> int:
    """Decode MonsterHuntInfo.CurLevel from CharSerialize field 56."""
    if not isinstance(data, bytes) or not data:
        return 0

    hunt = _decode_fields(data)
    cur_level = hunt.get(2, [None])[0]
    if isinstance(cur_level, int) and cur_level > 0:
        return cur_level
    return 0


def _normalize_season_medal_level(raw_level: int) -> int:
    """Normalize raw SeasonMedal level values to the UI-visible seasonal level.
    SeasonMedalInfo hole_level values >= 100 that are multiples of 10 encode
    as ``raw_level // 10`` (previously had an off-by-one ``-1``).
    """
    if raw_level <= 0:
        return 0
    if raw_level >= 100 and raw_level % 10 == 0:
        return max(0, raw_level // 10)
    return raw_level


def _decode_battlepass_data_level(data: bytes) -> int:
    """Decode the highest BattlePass.Level from BattlePassData (field 86)."""
    if not isinstance(data, bytes) or not data:
        return 0

    bp_data = _decode_fields(data)
    levels = []
    for entry_raw in bp_data.get(1, []):
        if not isinstance(entry_raw, bytes):
            continue
        entry = _decode_fields(entry_raw)
        battle_raw = entry.get(2, [None])[0]
        if not isinstance(battle_raw, bytes):
            continue
        battle = _decode_fields(battle_raw)
        level = battle.get(2, [None])[0]
        if isinstance(level, int) and level > 0:
            levels.append(level)
    return max(levels) if levels else 0


def _decode_slot_bar(data: bytes) -> Dict[int, int]:
    """Decode CharSerialize.Slots (field 55).

    Proto schema (from StarResonanceDps):
        message Slot {
            map<int32, SlotInfo> Slots = 1;
        }
        message SlotInfo {
            int32 Id = 1;
            int32 SkillId = 2;
            bool IsAutoBattleClose = 3;
        }

    Returns: {slot_id: skill_id}
    """
    result: Dict[int, int] = {}
    if not isinstance(data, bytes) or not data:
        return result
    outer = _decode_fields(data)
    # field 1 = map<int32, SlotInfo>, each entry is a nested map-entry message
    for entry_raw in outer.get(1, []):
        if not isinstance(entry_raw, bytes):
            continue
        entry = _decode_fields(entry_raw)
        key_raw = entry.get(1, [None])[0]
        value_raw = entry.get(2, [None])[0]
        if not isinstance(key_raw, int):
            continue
        slot_id = _varint_to_int32(key_raw)
        if slot_id <= 0:
            continue
        # value is SlotInfo message
        skill_id = 0
        if isinstance(value_raw, bytes):
            si = _decode_fields(value_raw)
            sid_raw = si.get(2, [None])[0]  # SkillId = field 2
            if isinstance(sid_raw, int):
                skill_id = _varint_to_int32(sid_raw)
        elif isinstance(value_raw, int):
            # Fallback: if SlotInfo is just a varint (unlikely)
            skill_id = _varint_to_int32(value_raw)
        if skill_id > 0:
            result[slot_id] = skill_id
    return result


def _decode_int_map(entries) -> Dict[int, int]:
    """Decode protobuf map<int32, int32> entries from raw nested messages."""
    result: Dict[int, int] = {}
    for entry_raw in entries or []:
        if not isinstance(entry_raw, bytes):
            continue
        entry = _decode_fields(entry_raw)
        key_raw = entry.get(1, [None])[0]
        value_raw = entry.get(2, [None])[0]
        if not isinstance(key_raw, int) or not isinstance(value_raw, int):
            continue
        key = _varint_to_int32(key_raw)
        value = _varint_to_int32(value_raw)
        result[key] = value
    return result


def _decode_profession_skill_info(data: bytes) -> Dict[str, int]:
    """Decode ProfessionSkillInfo enough to map slot entries to SkillLevelId."""
    if not isinstance(data, bytes) or not data:
        return {}
    fields = _decode_fields(data)
    skill_id_raw = fields.get(1, [None])[0]
    level_raw = fields.get(2, [None])[0]
    result = {
        'skill_id': _varint_to_int32(skill_id_raw) if isinstance(skill_id_raw, int) else 0,
        'level': _varint_to_int32(level_raw) if isinstance(level_raw, int) else 0,
    }
    return result


def _compose_skill_level_id(skill_id: int, level: int) -> int:
    """Compose the runtime skill_level_id used by SyncSkillCDs from skill_id + learned level."""
    skill_id = int(skill_id or 0)
    level = int(level or 0)
    if skill_id <= 0:
        return 0
    if 1 <= level <= 99:
        return (skill_id * 100) + level
    return skill_id


def _decode_profession_skill_map(entries) -> Dict[int, Dict[str, int]]:
    """Decode map<int32, ProfessionSkillInfo> into {skill_level_id: info}."""
    result: Dict[int, Dict[str, int]] = {}
    for entry_raw in entries or []:
        if not isinstance(entry_raw, bytes):
            continue
        entry = _decode_fields(entry_raw)
        key_raw = entry.get(1, [None])[0]
        value_raw = entry.get(2, [None])[0]
        if not isinstance(key_raw, int) or not isinstance(value_raw, bytes):
            continue
        key = _varint_to_int32(key_raw)
        if key <= 0:
            continue
        result[key] = _decode_profession_skill_info(value_raw)
    return result


def _decode_profession_list(data: bytes) -> Dict[str, Any]:
    """Decode current profession skill-slot mapping from CharSerialize.ProfessionList."""
    result = {
        'profession_id': 0,
        'slot_skill_level_map': {},
        'active_skill_ids': [],
        'skill_info_map': {},
        'skill_level_info_map': {},
    }
    if not isinstance(data, bytes) or not data:
        return result

    fields = _decode_fields(data)
    cur_prof_raw = fields.get(1, [None])[0]
    cur_prof_id = _varint_to_int32(cur_prof_raw) if isinstance(cur_prof_raw, int) else 0
    result['profession_id'] = cur_prof_id

    all_skill_info = _decode_profession_skill_map(fields.get(7, []))
    current_prof_raw = None
    profession_entries = []
    for entry_raw in fields.get(4, []):
        if not isinstance(entry_raw, bytes):
            continue
        entry = _decode_fields(entry_raw)
        key_raw = entry.get(1, [None])[0]
        value_raw = entry.get(2, [None])[0]
        if not isinstance(key_raw, int) or not isinstance(value_raw, bytes):
            continue
        prof_id = _varint_to_int32(key_raw)
        profession_entries.append((prof_id, value_raw))
        if prof_id == cur_prof_id:
            current_prof_raw = value_raw

    if current_prof_raw is None and len(profession_entries) == 1:
        result['profession_id'] = profession_entries[0][0]
        current_prof_raw = profession_entries[0][1]

    if not isinstance(current_prof_raw, bytes):
        return result

    current_prof = _decode_fields(current_prof_raw)
    current_skill_info = _decode_profession_skill_map(current_prof.get(4, []))
    for skill_level_id, info in all_skill_info.items():
        current_skill_info.setdefault(skill_level_id, info)
    result['skill_info_map'] = current_skill_info
    skill_level_info_map: Dict[int, Dict[str, int]] = {}
    for info_key, info in current_skill_info.items():
        skill_id = int(info.get('skill_id') or info_key or 0)
        skill_level_id = _compose_skill_level_id(skill_id, info.get('level', 0))
        if skill_level_id <= 0:
            skill_level_id = int(info_key or 0)
        if skill_level_id > 0:
            skill_level_info_map[skill_level_id] = {
                'skill_id': skill_id,
                'level': int(info.get('level', 0) or 0),
            }
    result['skill_level_info_map'] = skill_level_info_map
    result['active_skill_ids'] = [
        _varint_to_int32(v) for v in current_prof.get(6, []) if isinstance(v, int)
    ]

    raw_slot_map = _decode_int_map(current_prof.get(7, []))
    normalized_slot_map: Dict[int, int] = {}
    for slot, mapped_id in raw_slot_map.items():
        if slot < 0 or mapped_id <= 0:
            continue
        skill_info = current_skill_info.get(mapped_id)
        skill_level_id = 0
        if skill_info:
            skill_level_id = _compose_skill_level_id(mapped_id, skill_info.get('level', 0))
        if skill_level_id <= 0:
            for info_key, info in current_skill_info.items():
                if info.get('skill_id') == mapped_id:
                    skill_level_id = _compose_skill_level_id(info_key, info.get('level', 0))
                    break
        if skill_level_id <= 0:
            skill_level_id = mapped_id
        normalized_slot_map[slot] = skill_level_id
        if skill_level_id > 0 and skill_level_id not in skill_level_info_map:
            skill_info = current_skill_info.get(mapped_id) or {}
            skill_level_info_map[skill_level_id] = {
                'skill_id': int(skill_info.get('skill_id') or mapped_id or 0),
                'level': int(skill_info.get('level', 0) or 0),
            }

    result['slot_skill_level_map'] = normalized_slot_map
    return result


def _decode_skill_cd(data: bytes) -> Dict[str, int]:
    """Decode AoiSyncToMeDelta.SyncSkillCDs.

    The C# authoritative proto (StarResonanceDps) declares SyncSkillCDs as
    ``repeated SkillCDInfo`` which uses field 7=ChargeCount, 8=ValidCDTime.
    The older JS schema (SRDC) defined a simpler ``SkillCD`` with field
    5=ValidCDTime and no ChargeCount.

    To be robust against both wire formats we try the SkillCDInfo fields
    first and fall back to the SkillCD positions.
    """
    if not isinstance(data, bytes) or not data:
        return {}
    fields = _decode_fields(data)
    skill_level_id_raw = fields.get(1, [None])[0]
    begin_time_raw = fields.get(2, [None])[0]
    duration_raw = fields.get(3, [None])[0]
    cd_type_raw = fields.get(4, [None])[0]

    # valid_cd_time: prefer field 8 (SkillCDInfo), fall back to field 5 (SkillCD)
    vcd_f8_raw = fields.get(8, [None])[0]
    vcd_f5_raw = fields.get(5, [None])[0]
    vcd_f8 = _varint_to_int32(vcd_f8_raw) if isinstance(vcd_f8_raw, int) else 0
    vcd_f5 = _varint_to_int32(vcd_f5_raw) if isinstance(vcd_f5_raw, int) else 0
    valid_cd_time = vcd_f8 if vcd_f8 > 0 else vcd_f5

    # charge_count: field 7 (SkillCDInfo only; SkillCD has none)
    charge_count_raw = fields.get(7, [None])[0]
    charge_count = _varint_to_int32(charge_count_raw) if isinstance(charge_count_raw, int) else 0

    # CD acceleration fields (SkillCDInfo only, fields 9/10/11)
    sub_cd_ratio_raw = fields.get(9, [None])[0]
    sub_cd_fixed_raw = fields.get(10, [None])[0]
    accel_cd_ratio_raw = fields.get(11, [None])[0]
    sub_cd_ratio = _varint_to_int32(sub_cd_ratio_raw) if isinstance(sub_cd_ratio_raw, int) else 0
    sub_cd_fixed = _varint_to_int64(sub_cd_fixed_raw) if isinstance(sub_cd_fixed_raw, int) else 0
    accel_cd_ratio = _varint_to_int32(accel_cd_ratio_raw) if isinstance(accel_cd_ratio_raw, int) else 0

    skill_level_id = _varint_to_int32(skill_level_id_raw) if isinstance(skill_level_id_raw, int) else 0
    return {
        'skill_level_id': skill_level_id,
        'begin_time': _varint_to_int64(begin_time_raw) if isinstance(begin_time_raw, int) else 0,
        'duration': _varint_to_int32(duration_raw) if isinstance(duration_raw, int) else 0,
        'skill_cd_type': _varint_to_int32(cd_type_raw) if isinstance(cd_type_raw, int) else 0,
        'valid_cd_time': max(0, valid_cd_time),
        'charge_count': max(0, charge_count),
        'sub_cd_ratio': max(0, sub_cd_ratio),
        'sub_cd_fixed': max(0, int(sub_cd_fixed)),
        'accelerate_cd_ratio': max(0, accel_cd_ratio),
    }


def _decode_skill_cd_info(data: bytes) -> Dict[str, int]:
    """Decode UserFightAttr.CdInfo / SkillCDInfo.

    Field numbers follow the StarResonanceDps authoritative schema:
      1=SkillLevelId, 2=SkillBeginTime, 3=Duration, 4=SkillCDType,
      6=ProfessionHoldBeginTime (field 5 unused), 7=ChargeCount,
      8=ValidCDTime, 9=SubCDRatio, 10=SubCDFixed, 11=AccelerateCDRatio
    """
    if not isinstance(data, bytes) or not data:
        return {}
    fields = _decode_fields(data)
    skill_level_id_raw = fields.get(1, [None])[0]
    begin_time_raw = fields.get(2, [None])[0]
    duration_raw = fields.get(3, [None])[0]
    cd_type_raw = fields.get(4, [None])[0]
    charge_count_raw = fields.get(7, [None])[0]    # field 7 per StarResonanceDps
    valid_cd_time_raw = fields.get(8, [None])[0]    # field 8 per StarResonanceDps
    # CD acceleration fields (fields 9/10/11)
    sub_cd_ratio_raw = fields.get(9, [None])[0]
    sub_cd_fixed_raw = fields.get(10, [None])[0]
    accel_cd_ratio_raw = fields.get(11, [None])[0]
    skill_level_id = _varint_to_int32(skill_level_id_raw) if isinstance(skill_level_id_raw, int) else 0
    return {
        'skill_level_id': skill_level_id,
        'begin_time': _varint_to_int64(begin_time_raw) if isinstance(begin_time_raw, int) else 0,
        'duration': _varint_to_int32(duration_raw) if isinstance(duration_raw, int) else 0,
        'skill_cd_type': _varint_to_int32(cd_type_raw) if isinstance(cd_type_raw, int) else 0,
        'valid_cd_time': _varint_to_int32(valid_cd_time_raw) if isinstance(valid_cd_time_raw, int) else 0,
        'charge_count': _varint_to_int32(charge_count_raw) if isinstance(charge_count_raw, int) else 0,
        'sub_cd_ratio': max(0, _varint_to_int32(sub_cd_ratio_raw)) if isinstance(sub_cd_ratio_raw, int) else 0,
        'sub_cd_fixed': max(0, int(_varint_to_int64(sub_cd_fixed_raw))) if isinstance(sub_cd_fixed_raw, int) else 0,
        'accelerate_cd_ratio': max(0, _varint_to_int32(accel_cd_ratio_raw)) if isinstance(accel_cd_ratio_raw, int) else 0,
    }


def _decode_fight_res_cd(data: bytes) -> Dict[str, int]:
    """Decode AoiSyncToMeDelta.FightResCDs for future stamina/resource mapping."""
    if not isinstance(data, bytes) or not data:
        return {}
    fields = _decode_fields(data)
    res_id_raw = fields.get(1, [None])[0]
    begin_time_raw = fields.get(2, [None])[0]
    duration_raw = fields.get(3, [None])[0]
    valid_cd_time_raw = fields.get(4, [None])[0]
    return {
        'res_id': _varint_to_int32(res_id_raw) if isinstance(res_id_raw, int) else 0,
        'begin_time': _varint_to_int64(begin_time_raw) if isinstance(begin_time_raw, int) else 0,
        'duration': _varint_to_int32(duration_raw) if isinstance(duration_raw, int) else 0,
        'valid_cd_time': _varint_to_int32(valid_cd_time_raw) if isinstance(valid_cd_time_raw, int) else 0,
    }


def _decode_dirty_energy_value(raw_u32: int, raw_f32: float, stamina_max: int = 0) -> Optional[float]:
    """Pick the sane representation from dirty-stream energy payload."""
    max_allowed = max(20000.0, float(stamina_max) * 1.2) if stamina_max > 0 else 20000.0
    if math.isfinite(raw_f32):
        if 0.0 <= raw_f32 <= 1.05 and stamina_max > 0:
            return float(raw_f32)
        if 0.01 <= raw_f32 <= max_allowed:
            return float(raw_f32)
        if raw_f32 == 0.0:
            return 0.0
    if 0 <= raw_u32 <= max_allowed:
        return float(raw_u32)
    return None


def _is_sane_attr_stamina_max(value: int) -> bool:
    # Current observed self STA caps stay around 1200. Values like 1350/1500
    # and the larger 2100/3100 spikes are not stable enough to trust.
    return 0 < value <= 1300


_LEVEL_EXTRA_SOURCE_PRIORITY = {
    'season_attr': 100,   # AttrSeasonLevel (10070) — server-authoritative total season level
    'season_attr_lv': 100, # AttrSeasonLv (196) — same authority per DPS project reference
    'season_medal': 50,   # SeasonMedalInfo CoreHole (subsystem, not display level)
    'monster_hunt': 10,   # MonsterHuntInfo CurLevel from CharSerialize field 56
    'battlepass': 5,
    'battlepass_data': 3,
}

# Sources that are reliable enough to commit on first observation (no 2-hit)
# season_medal comes from SyncContainerData field 52 (CoreHoleInfo.HoleLevel)
# — the server-authoritative season progression level, fires at login & dirty updates.
_TRUSTED_LEVEL_SOURCES = frozenset({'season_attr', 'season_attr_lv', 'season_medal'})


def _source_priority(source: str) -> int:
    return int(_LEVEL_EXTRA_SOURCE_PRIORITY.get(str(source or ''), 0))


def _commit_level_extra(player: PlayerData, source: str, value: int) -> bool:
    source = str(source or '')
    value = max(0, int(value or 0))
    if value <= 0:
        return False
    if player.level_extra == value and player.level_extra_source == source:
        return False
    player.level_extra = value
    player.level_extra_source = source
    player.level_extra_pending_source = ''
    player.level_extra_pending_value = 0
    player.level_extra_pending_hits = 0
    return True


def _set_level_extra_candidate(player: PlayerData, source: str, value: int) -> bool:
    source = str(source or '')
    value = max(0, int(value or 0))
    if value <= 0 or not source:
        return False

    current_priority = _source_priority(getattr(player, 'level_extra_source', ''))
    candidate_priority = _source_priority(source)

    # Block lower-priority sources from overriding a higher-priority confirmed value.
    if (
        getattr(player, 'level_extra', 0) > 0 and
        value != player.level_extra and
        current_priority > candidate_priority
    ):
        return False

    # Trusted sources (season_medal, season_attr) commit immediately.
    if source in _TRUSTED_LEVEL_SOURCES:
        return _commit_level_extra(player, source, value)

    # Untrusted sources still require 2 matching hits.
    if player.level_extra_pending_source == source and player.level_extra_pending_value == value:
        player.level_extra_pending_hits += 1
    else:
        player.level_extra_pending_source = source
        player.level_extra_pending_value = value
        player.level_extra_pending_hits = 1

    if player.level_extra == value and player.level_extra_source == source:
        return False

    if player.level_extra_pending_hits < 2:
        return False

    return _commit_level_extra(player, source, value)


def _decode_resource_value_map(resource_ids, resources) -> Dict[int, int]:
    result: Dict[int, int] = {}
    count = min(len(resource_ids or []), len(resources or []))
    for idx in range(count):
        res_id_raw = resource_ids[idx]
        value_raw = resources[idx]
        if not isinstance(res_id_raw, int) or not isinstance(value_raw, int):
            continue
        res_id = _varint_to_int32(res_id_raw)
        value = _varint_to_int32(value_raw)
        if res_id > 0 and value >= 0:
            result[res_id] = value
    return result


def _pick_stamina_resource_id(player: PlayerData) -> int:
    resource_values = getattr(player, 'resource_values', {}) or {}
    energy_info_map = getattr(player, 'energy_info_map', {}) or {}
    total_limit = max(0, int(getattr(player, 'energy_limit', 0) or 0))
    total_limit += max(0, int(getattr(player, 'extra_energy_limit', 0) or 0))

    candidates = []
    for resource_id, current_value in resource_values.items():
        info = energy_info_map.get(resource_id) or {}
        max_value = max(0, int(info.get('energy_value', 0) or 0))
        score = 0
        if max_value > 0:
            score += 50
            if 0 <= current_value <= max_value:
                score += 25
            if total_limit > 0 and abs(max_value - total_limit) <= max(12, int(total_limit * 0.08)):
                score += 25
            if _is_sane_attr_stamina_max(max_value):
                score += 15
        if 0 <= current_value <= 1300:
            score += 8
        if int(info.get('unlock_num', 0) or 0) >= 0:
            score += 2
        candidates.append((score, -abs(total_limit - max_value) if total_limit > 0 and max_value > 0 else 0, resource_id))

    if not candidates and len(energy_info_map) == 1:
        try:
            return int(next(iter(energy_info_map.keys())))
        except Exception:
            return 0

    candidates.sort(reverse=True)
    return int(candidates[0][2]) if candidates else 0


def _refresh_stamina_resource(player: PlayerData) -> bool:
    picked_resource_id = _pick_stamina_resource_id(player)
    if picked_resource_id <= 0 or picked_resource_id == int(getattr(player, 'stamina_resource_id', 0) or 0):
        return False
    player.stamina_resource_id = picked_resource_id
    return True


class PacketParser:
    """Parse game packets and notify the callback when self data changes."""

    def __init__(self, on_self_update: Callable[[PlayerData], None],
                 on_damage: Optional[Callable[[dict], None]] = None,
                 on_monster_update: Optional[Callable[[dict], None]] = None,
                 on_boss_event: Optional[Callable[[dict], None]] = None,
                 on_scene_change: Optional[Callable[[], None]] = None,
                 preferred_uid: int = 0):
        self._on_update = on_self_update
        self._on_damage = on_damage   # callback(DamageEvent dict)
        self._on_monster_update = on_monster_update  # callback(MonsterData.to_dict())
        self._on_boss_event = on_boss_event          # callback({event_type, host_uuid, ...})
        self._on_scene_change = on_scene_change      # callback() — 场景服务器切换时清理
        self._current_uuid: int = 0   # Current player UUID
        self._current_uid: int = max(0, int(preferred_uid))    # Current player UID (uuid >> 16)
        self._players: Dict[int, PlayerData] = {}  # uid -> PlayerData
        self._monsters: Dict[int, MonsterData] = {}  # uuid -> MonsterData
        # Cache template_id → max_hp from observed monsters.
        # Survives scene resets so if a monster re-appears without MAX_HP in
        # its AttrCollection (only incremental deltas), we can restore max_hp
        # from this cache.  Limited to 1024 entries.
        self._monster_hp_cache: Dict[int, int] = {}  # template_id -> max_hp
        self._profession_skill_cache: Dict[int, Dict[int, int]] = {}  # profession_id -> slot map
        self._zstd = None
        self._server_time_offset_ms: Optional[float] = None
        self._last_dungeon_id: int = 0   # Track dungeon transitions
        self._last_scene_id: int = 0     # Track scene/map transitions
        self._sync_container_count: int = 0  # Count SyncContainerData receives
        self.stats = {
            'raw_frames': 0,
            'game_frames': 0,
            'unknown_message_types': 0,
            'unknown_notify_methods': 0,
            'zstd_failures': 0,
            'damage_events': 0,
            'monster_updates': 0,
            'boss_events': 0,
            'scene_changes': 0,
        }
        if self._current_uid > 0:
            logger.info(f'[Parser] bootstrap self UID from cache: {self._current_uid}')
        try:
            debug_path = os.path.join(os.path.dirname(__file__), 'packet_debug.jsonl')
            if os.path.exists(debug_path):
                os.remove(debug_path)
        except Exception:
            pass

    def _get_player(self, uid: int) -> PlayerData:
        if uid not in self._players:
            self._players[uid] = PlayerData(uid)
        return self._players[uid]

    def _get_monster(self, uuid: int) -> MonsterData:
        if uuid not in self._monsters:
            self._monsters[uuid] = MonsterData(uuid)
        return self._monsters[uuid]

    def reset_scene(self):
        """场景服务器切换时重置场景数据。

        清除:
        - 怪物缓存 (旧场景的怪物不会出现在新场景)
        - 服务器时间偏移 (新服务器有独立时间)
        - 副本 ID 追踪 (防止后续 SyncDungeonData 二次重置)

        保留:
        - 玩家数据 (player identity/profession 跨场景不变)
        - 玩家 UUID/UID (保持身份连续)
        - 职业技能缓存 (profession_skill_cache 跨场景不变)
        """
        old_count = len(self._monsters)
        # Save template_id → max_hp to cache before clearing
        for m in self._monsters.values():
            if m.template_id > 0 and m.max_hp > 0:
                self._monster_hp_cache[m.template_id] = m.max_hp
        # Limit cache size
        if len(self._monster_hp_cache) > 1024:
            # Keep only the most recently added entries
            items = list(self._monster_hp_cache.items())
            self._monster_hp_cache = dict(items[-1024:])
        self._monsters.clear()
        self._server_time_offset_ms = None
        # 重置副本 ID: 服务器切换后 SyncDungeonData 会带新 dungeon_id,
        # 若 _last_dungeon_id 还保留旧值则会触发二次 reset_scene,
        # 把已经由 SyncNearEntities 填充的新怪物清空 (包括 MAX_HP)
        # → 后续 AoiSyncDelta 只发 HP 增量 → max_hp=0 → boss bar 失效。
        self._last_dungeon_id = 0
        self.stats['scene_changes'] += 1
        logger.info(
            f'[Parser] 场景重置: 清除 {old_count} 个怪物, '
            f'保留 {len(self._players)} 个玩家, '
            f'current_uid={self._current_uid}'
        )
        print(
            f'[Parser] 场景切换重置: 清除 {old_count} 个旧怪物, '
            f'等待新场景 SyncNearEntities / SyncContainerData',
            flush=True,
        )
        # 通知上层 (bridge/webview) 场景已切换
        if self._on_scene_change:
            try:
                self._on_scene_change()
            except Exception as e:
                logger.error(f'[Parser] on_scene_change callback error: {e}')

    def get_monsters(self) -> Dict[int, MonsterData]:
        """Return the current monster tracking dict (uuid → MonsterData)."""
        return self._monsters

    def get_players(self) -> Dict[int, 'PlayerData']:
        """Return all tracked players (uid → PlayerData) for info sync."""
        return self._players

    def get_alive_monsters(self) -> list:
        """Return list of alive monster dicts (for UI consumption)."""
        return [m.to_dict() for m in self._monsters.values()
                if not m.is_dead and m.max_hp > 0]

    def _notify_monster(self, monster: MonsterData):
        """Fire on_monster_update callback if registered."""
        if self._on_monster_update:
            self.stats['monster_updates'] += 1
            try:
                self._on_monster_update(monster.to_dict())
            except Exception as e:
                logger.debug(f'[Parser] monster update callback error: {e}')

    def _notify_boss_event(self, event_type: int, host_uuid: int,
                           buff_uuid: int = 0, extra: Optional[dict] = None):
        """Fire on_boss_event callback for boss-relevant buff events."""
        if self._on_boss_event:
            self.stats['boss_events'] += 1
            event = {
                'event_type': event_type,
                'host_uuid': host_uuid,
                'buff_uuid': buff_uuid,
                'timestamp': time.time(),
            }
            if extra:
                event.update(extra)
            try:
                self._on_boss_event(event)
            except Exception as e:
                logger.debug(f'[Parser] boss event callback error: {e}')

    def _notify_self(self):
        """Notify callback for current player if available."""
        if self._current_uid and self._current_uid in self._players:
            p = self._players[self._current_uid]
            self._apply_cached_profession_slots(p)
            p.server_time_offset_ms = self._server_time_offset_ms
            logger.debug(f'[Parser] notify_self: name={p.name!r} lv={p.level} rank_lv={p.rank_level} '
                         f'hp={p.hp}/{p.max_hp} uid={self._current_uid}')
            try:
                self._on_update(p)
            except Exception as e:
                print(f'[Parser] !! 回调异常 (bridge callback): {e}', flush=True)
                import traceback
                print(traceback.format_exc(), flush=True)
                logger.error(f'[Parser] callback error: {e}')

    def _apply_cached_profession_slots(self, player: PlayerData) -> bool:
        """Reuse the last known slot map for a profession when only profession id is available."""
        profession_id = int(getattr(player, 'profession_id', 0) or 0)
        if profession_id <= 0:
            return False
        cached_slot_map = self._profession_skill_cache.get(profession_id) or {}
        if not cached_slot_map or cached_slot_map == player.skill_slot_map:
            return False
        player.skill_slot_map = dict(cached_slot_map)
        return True

    def _try_detect_profession(self, player: PlayerData, skill_level_id: int) -> bool:
        """Auto-detect profession from observed skill IDs when SyncContainerData was missed.

        Uses reverse lookup from PROFESSION_NORMAL_ATTACK / PROFESSION_SKILL /
        PROFESSION_ULTIMATE / PROFESSION_SKILL_VARIANTS tables to identify
        the player's current profession from any matching skill_level_id.
        Also detects sub-profession branch from SUB_PROFESSION_NAMES.
        """
        base = skill_level_id // 100 if skill_level_id >= 100 else skill_level_id

        # Try to detect sub-profession branch even if profession is already known
        if base in SUB_PROFESSION_NAMES:
            sub = SUB_PROFESSION_NAMES[base]
            if sub and sub != getattr(player, 'sub_profession', ''):
                player.sub_profession = sub
                logger.info(
                    f'[Parser] detected sub_profession={sub!r} '
                    f'from skill_level_id={skill_level_id} (base={base})'
                )

        if int(getattr(player, 'profession_id', 0) or 0) > 0:
            return False  # Profession already known
        pid = _SKILL_TO_PROFESSION.get(base, 0)
        if pid > 0:
            player.profession_id = pid
            player.profession = PROFESSION_NAMES.get(pid, '')
            logger.info(
                f'[Parser] auto-detected profession={pid} ({player.profession}) '
                f'from skill_level_id={skill_level_id} (base={base})'
            )
            self._apply_cached_profession_slots(player)
            return True
        return False

    def _remember_seen_skill(self, player: PlayerData, skill_level_id: int) -> bool:
        """Cache every observed skill id, including zero-duration attack pings."""
        skill_level_id = int(skill_level_id or 0)
        if skill_level_id <= 0:
            return False
        seen = getattr(player, 'skill_seen_ids', None)
        if seen is None:
            player.skill_seen_ids = []
            seen = player.skill_seen_ids
        if skill_level_id in seen:
            return False
        seen.append(skill_level_id)
        return True

    def _replace_skill_cds(self, player: PlayerData, skill_cds) -> bool:
        """Replace the full self cooldown snapshot from UserFightAttr.CdInfo."""
        normalized: Dict[int, Dict[str, Any]] = {}
        observed_at_ms = int(time.time() * 1000)
        previous = player.skill_cd_map
        seen_changed = False
        for skill_cd in skill_cds or []:
            skill_level_id = int(skill_cd.get('skill_level_id') or 0)
            # duration = total CD length; valid_cd_time = elapsed or progress
            total_ms = int(skill_cd.get('duration') or 0)
            if skill_level_id <= 0 or total_ms <= 0:
                continue
            if self._remember_seen_skill(player, skill_level_id):
                seen_changed = True
            normalized[skill_level_id] = {
                'skill_level_id': skill_level_id,
                'begin_time': max(0, int(skill_cd.get('begin_time') or 0)),
                'duration': max(0, int(skill_cd.get('duration') or 0)),
                'valid_cd_time': max(0, int(skill_cd.get('valid_cd_time') or 0)),
                'skill_cd_type': max(0, int(skill_cd.get('skill_cd_type') or 0)),
                'charge_count': max(0, int(skill_cd.get('charge_count') or 0)),
                'sub_cd_ratio': max(0, int(skill_cd.get('sub_cd_ratio') or 0)),
                'sub_cd_fixed': max(0, int(skill_cd.get('sub_cd_fixed') or 0)),
                'accelerate_cd_ratio': max(0, int(skill_cd.get('accelerate_cd_ratio') or 0)),
                'observed_at_ms': observed_at_ms,
                'source': 'full_sync',
            }
            prev = previous.get(skill_level_id)
            if prev and prev.get('begin_time') == normalized[skill_level_id].get('begin_time'):
                # Same CD instance — carry forward first-observation time and speed
                normalized[skill_level_id]['observed_at_ms'] = prev.get('observed_at_ms', observed_at_ms)
                # VCD value changed → update last_vcd_update_ms to now
                normalized[skill_level_id]['last_vcd_update_ms'] = observed_at_ms
                # Carry forward VCD speed tracking
                if 'vcd_speed_ratio' in prev:
                    normalized[skill_level_id]['vcd_speed_ratio'] = prev['vcd_speed_ratio']

        if previous == normalized and not seen_changed:
            return False

        player.skill_cd_map = normalized
        return True

    def _update_skill_cd(self, player: PlayerData, skill_cd: Dict[str, Any]) -> bool:
        """Merge one cooldown delta from AoiSyncToMeDelta.SyncSkillCDs."""
        skill_level_id = int(skill_cd.get('skill_level_id') or 0)
        if skill_level_id <= 0:
            return False
        seen_changed = self._remember_seen_skill(player, skill_level_id)

        # Auto-detect profession from observed skill IDs when SyncContainerData missed
        self._try_detect_profession(player, skill_level_id)

        total_ms = int(skill_cd.get('duration') or 0)
        if total_ms <= 0:
            player.skill_last_use_at[skill_level_id] = time.time()
            if skill_level_id in player.skill_cd_map:
                del player.skill_cd_map[skill_level_id]
                return True
            return seen_changed

        observed_at_ms = int(time.time() * 1000)
        new_entry = {
            'skill_level_id': skill_level_id,
            'begin_time': max(0, int(skill_cd.get('begin_time') or 0)),
            'duration': max(0, int(skill_cd.get('duration') or 0)),
            'valid_cd_time': max(0, int(skill_cd.get('valid_cd_time') or 0)),
            'skill_cd_type': max(0, int(skill_cd.get('skill_cd_type') or 0)),
            'charge_count': max(0, int(skill_cd.get('charge_count') or 0)),
            'sub_cd_ratio': max(0, int(skill_cd.get('sub_cd_ratio') or 0)),
            'sub_cd_fixed': max(0, int(skill_cd.get('sub_cd_fixed') or 0)),
            'accelerate_cd_ratio': max(0, int(skill_cd.get('accelerate_cd_ratio') or 0)),
            'observed_at_ms': observed_at_ms,
            'source': 'delta',
        }
        prev = player.skill_cd_map.get(skill_level_id)
        if prev:
            same_timing = (
                prev.get('begin_time') == new_entry['begin_time'] and
                prev.get('duration') == new_entry['duration']
            )
            same_core = (
                same_timing and
                prev.get('valid_cd_time') == new_entry['valid_cd_time'] and
                prev.get('skill_cd_type') == new_entry['skill_cd_type'] and
                prev.get('charge_count') == new_entry['charge_count'] and
                prev.get('accelerate_cd_ratio') == new_entry['accelerate_cd_ratio'] and
                prev.get('sub_cd_ratio') == new_entry['sub_cd_ratio'] and
                prev.get('sub_cd_fixed') == new_entry['sub_cd_fixed']
            )
            if same_core:
                new_entry['observed_at_ms'] = prev.get('observed_at_ms', observed_at_ms)
                new_entry['last_vcd_update_ms'] = prev.get('last_vcd_update_ms', observed_at_ms)
                # Carry forward VCD speed ratio
                if 'vcd_speed_ratio' in prev:
                    new_entry['vcd_speed_ratio'] = prev['vcd_speed_ratio']
                player.skill_cd_map[skill_level_id] = new_entry
                return False

            # Mid-CD acceleration/reduction change (same begin_time, same duration,
            # but acceleration fields differ)
            accel_changed = same_timing and (
                prev.get('accelerate_cd_ratio') != new_entry['accelerate_cd_ratio'] or
                prev.get('sub_cd_ratio') != new_entry['sub_cd_ratio'] or
                prev.get('sub_cd_fixed') != new_entry['sub_cd_fixed']
            )
            if accel_changed:
                new_entry['last_vcd_update_ms'] = observed_at_ms

            # --- VCD speed tracking ---
            # Track how fast valid_cd_time progresses vs real time to estimate
            # real remaining CD (server bakes in CD acceleration).
            if same_timing and not accel_changed:
                prev_vcd = int(prev.get('valid_cd_time') or 0)
                new_vcd = new_entry['valid_cd_time']
                delta_vcd = new_vcd - prev_vcd
                # Use last_vcd_update_ms (not observed_at_ms) to avoid stale timing
                prev_vcd_time = int(prev.get('last_vcd_update_ms') or prev.get('observed_at_ms') or 0)
                delta_real_ms = observed_at_ms - prev_vcd_time
                if delta_vcd > 0 and delta_real_ms > 50:
                    sample_speed = delta_vcd / delta_real_ms
                    if 0.5 < sample_speed < 25.0:  # sanity bounds
                        old_skill = prev.get('vcd_speed_ratio') or player.cd_speed_ratio
                        new_entry['vcd_speed_ratio'] = 0.3 * sample_speed + 0.7 * old_skill
                        player.cd_speed_ratio = (
                            0.2 * sample_speed + 0.8 * player.cd_speed_ratio
                        )
                elif 'vcd_speed_ratio' in prev:
                    new_entry['vcd_speed_ratio'] = prev['vcd_speed_ratio']
            elif 'vcd_speed_ratio' in prev:
                new_entry['vcd_speed_ratio'] = prev['vcd_speed_ratio']
            # Always refresh last_vcd_update_ms when VCD changes
            new_entry['last_vcd_update_ms'] = observed_at_ms

            if new_entry['begin_time'] != prev.get('begin_time'):
                # Only treat a *new* begin_time as a fresh skill cast.
                player.skill_last_use_at[skill_level_id] = time.time()
                # Keep vcd_speed_ratio — player buff doesn't change per-cast
        else:
            player.skill_last_use_at[skill_level_id] = time.time()

        player.skill_cd_map[skill_level_id] = new_entry
        return True





    def process_packet(self, frame: bytes):
        """Process one framed packet: `[4B size][2B type][payload]`."""
        if len(frame) < 6:
            return
        self.stats['raw_frames'] += 1
        if self.stats['raw_frames'] == 1:
            print(f'[Parser] 首个数据帧到达 (size={len(frame)})', flush=True)
        offset = 0
        total = len(frame)
        while offset < total:
            if offset + 6 > total:
                break
            pkt_size = struct.unpack_from('>I', frame, offset)[0]
            if pkt_size < 6 or offset + pkt_size > total:
                break
            pkt_type = struct.unpack_from('>H', frame, offset + 4)[0]
            is_zstd = bool(pkt_type & 0x8000)
            msg_type = pkt_type & 0x7FFF
            payload = frame[offset + 6:offset + pkt_size]
            offset += pkt_size

            try:
                if msg_type == MessageType.NOTIFY:
                    self.stats['game_frames'] += 1
                    self._on_notify(payload, is_zstd)
                elif msg_type == MessageType.FRAME_DOWN:
                    self.stats['game_frames'] += 1
                    self._on_frame_down(payload, is_zstd)
                else:
                    self.stats['unknown_message_types'] += 1

            except Exception as e:
                import traceback
                print(f'[Parser] !! 消息处理异常 (type={msg_type}): {e}', flush=True)
                print(traceback.format_exc(), flush=True)
                logger.error(f'[Parser] message handling error (type={msg_type}): {e}\n{traceback.format_exc()}')


    #  FrameDown


    def _on_frame_down(self, payload: bytes, is_zstd: bool):
        if len(payload) < 4:
            return
        # server_seq_id = struct.unpack_from('>I', payload, 0)[0]
        nested = payload[4:]
        if not nested:
            return
        if is_zstd:
            nested = self._decompress(nested)
            if nested is None:
                return

        self.process_packet(nested)


    #  Notify


    def _on_notify(self, payload: bytes, is_zstd: bool):
        if len(payload) < 16:
            return
        # serviceUuid (8B) + stubId (4B) + methodId (4B)
        service_uuid = struct.unpack_from('>Q', payload, 0)[0]
        # stub_id = struct.unpack_from('>I', payload, 8)[0]
        method_id = struct.unpack_from('>I', payload, 12)[0]

        if service_uuid != SERVICE_UUID_C3SB:
            return

        # 首次收到游戏消息时打印
        if not getattr(self, '_first_notify_printed', False):
            self._first_notify_printed = True
            print(f'[Parser] 首个游戏Notify: method=0x{method_id:02X} zstd={is_zstd}', flush=True)

        msg_payload = payload[16:]
        if is_zstd:
            msg_payload = self._decompress(msg_payload)
            if msg_payload is None:
                return

        if method_id == NotifyMethod.SYNC_CONTAINER_DATA:
            self._on_sync_container_data(msg_payload)
        elif method_id == NotifyMethod.SYNC_CONTAINER_DIRTY_DATA:
            self._on_sync_container_dirty(msg_payload)
        elif method_id == NotifyMethod.SYNC_SERVER_TIME:
            self._on_sync_server_time(msg_payload)
        elif method_id == NotifyMethod.SYNC_NEAR_ENTITIES:
            self._on_sync_near_entities(msg_payload)
        elif method_id == NotifyMethod.SYNC_TO_ME_DELTA_INFO:
            self._on_sync_to_me_delta(msg_payload)
        elif method_id == NotifyMethod.SYNC_NEAR_DELTA_INFO:
            self._on_sync_near_delta(msg_payload)
        # ── Combat notify (battle server) ──
        elif method_id == NotifyMethod.NOTIFY_BUFF_CHANGE:
            self._on_notify_buff_change(msg_payload)
        elif method_id == NotifyMethod.SYNC_CLIENT_USE_SKILL:
            self._on_sync_client_use_skill(msg_payload)
        elif method_id == NotifyMethod.SYNC_SERVER_SKILL_END:
            self._on_sync_server_skill_end(msg_payload)
        elif method_id == NotifyMethod.SYNC_SERVER_SKILL_STAGE_END:
            self._on_sync_server_skill_stage_end(msg_payload)
        elif method_id == NotifyMethod.QTE_BEGIN:
            self._on_qte_begin(msg_payload)
        # ── Dungeon / Scene ──
        elif method_id == NotifyMethod.SYNC_DUNGEON_DATA:
            self._on_sync_dungeon_data(msg_payload)
        elif method_id == NotifyMethod.NOTIFY_START_PLAYING_DUNGEON:
            self._on_notify_start_playing_dungeon(msg_payload)
        # ── Login / Session ──
        elif method_id == NotifyMethod.ENTER_GAME:
            self._on_enter_game(msg_payload)
        elif method_id == NotifyMethod.NOTIFY_REVIVE_USER:
            self._on_notify_revive_user(msg_payload)
        elif method_id == NotifyMethod.NOTIFY_CLIENT_KICK_OFF:
            logger.info('[Parser] NotifyClientKickOff received — session ended')
        # ── All other known methods — log for completeness ──
        else:
            method_name = _NOTIFY_METHOD_NAMES.get(method_id)
            if method_name:
                logger.debug(f'[Parser] Unhandled notify: {method_name} (0x{method_id:X}) len={len(msg_payload)}')
            else:
                self.stats['unknown_notify_methods'] += 1
                logger.debug(f'[Parser] Unknown notify method 0x{method_id:X} len={len(msg_payload)}')


    # SyncContainerData (full character sync)


    def _on_sync_server_time(self, data: bytes):
        """Track server/client time delta for packet-only cooldown progress."""
        fields = _decode_fields(data)
        client_ms_raw = fields.get(1, [None])[0]
        server_ms_raw = fields.get(2, [None])[0]
        client_ms = _varint_to_int64(client_ms_raw) if isinstance(client_ms_raw, int) else 0
        server_ms = _varint_to_int64(server_ms_raw) if isinstance(server_ms_raw, int) else 0
        if client_ms <= 0 and server_ms <= 0:
            return

        local_ms = int(time.time() * 1000)
        if client_ms > 0 and server_ms > 0:
            self._server_time_offset_ms = float(server_ms - client_ms)
        elif server_ms > 0:
            self._server_time_offset_ms = float(server_ms - local_ms)

        if self._current_uid and self._current_uid in self._players:
            player = self._players[self._current_uid]
            player.server_time_offset_ms = self._server_time_offset_ms
            if player.skill_cd_map:
                self._notify_self()


    # ── Combat notify handlers (battle server 0x3000 range) ──


    def _on_notify_buff_change(self, data: bytes):
        """Handle NotifyBuffChange (0x3003 / 12291) — buff add/remove/update.

        NotifyBuffChange proto:
            int64 Uuid = 1;            // Target entity UUID
            int32 ChangeType = 2;      // 1=Add, 2=Remove, 3=Update
            BuffInfo Info = 3;         // The buff data
        """
        try:
            outer = _decode_fields(data)
            uuid_raw = outer.get(1, [0])[0]
            uuid = _varint_to_int64(uuid_raw) if isinstance(uuid_raw, int) else 0
            change_type = outer.get(2, [0])[0]  # 1=Add, 2=Remove, 3=Update
            buff_raw = outer.get(3, [None])[0]
            buff_id = 0
            buff_info = {}
            if isinstance(buff_raw, bytes):
                bf = _decode_fields(buff_raw)
                buff_id = bf.get(1, [0])[0]
                buff_info = {
                    'buff_id': buff_id,
                    'begin_time': _varint_to_int64(bf.get(2, [0])[0]),
                    'duration': bf.get(3, [0])[0],
                    'layer': bf.get(4, [0])[0],
                    'skill_id': bf.get(6, [0])[0],
                    'skill_level_id': bf.get(7, [0])[0],
                    'cur_layer': bf.get(8, [0])[0],
                }
            logger.info(
                f'[Parser] NotifyBuffChange: uuid={uuid} type={change_type} '
                f'buff_id={buff_id} info={buff_info}'
            )
            _append_packet_debug('buff_change', {
                'uuid': uuid,
                'change_type': change_type,
                'buff_info': buff_info,
            })
            # Update buff_list on player/monster if tracked
            if _is_player(uuid):
                uid = _uuid_to_uid(uuid)
                if uid in self._players:
                    player = self._players[uid]
                    if change_type == 1 and buff_info:  # Add
                        player.buff_list.append(buff_info)
                    elif change_type == 2 and buff_id:  # Remove
                        player.buff_list = [b for b in player.buff_list if b.get('buff_id') != buff_id]
                    elif change_type == 3 and buff_info:  # Update
                        player.buff_list = [b for b in player.buff_list if b.get('buff_id') != buff_id]
                        player.buff_list.append(buff_info)
                    if uid == self._current_uid:
                        self._notify_self()
            elif _is_monster(uuid) and uuid in self._monsters:
                monster = self._monsters[uuid]
                if change_type == 1 and buff_info:
                    monster.buff_list.append(buff_info)
                elif change_type == 2 and buff_id:
                    monster.buff_list = [b for b in monster.buff_list if b.get('buff_id') != buff_id]
                elif change_type == 3 and buff_info:
                    monster.buff_list = [b for b in monster.buff_list if b.get('buff_id') != buff_id]
                    monster.buff_list.append(buff_info)
        except Exception as e:
            logger.debug(f'[Parser] NotifyBuffChange decode error: {e}')

    def _on_sync_client_use_skill(self, data: bytes):
        """Handle SyncClientUseSkill (0x3002 / 12290) — skill use confirmation.

        SyncClientUseSkill proto:
            int64 Uuid = 1;
            int32 SkillId = 2;
            int32 SkillLevelId = 3;
            ScenePosition Pos = 4;
            ScenePosition Dir = 5;
            int64 TargetUuid = 6;
        """
        try:
            outer = _decode_fields(data)
            uuid_raw = outer.get(1, [0])[0]
            uuid = _varint_to_int64(uuid_raw) if isinstance(uuid_raw, int) else 0
            skill_id = outer.get(2, [0])[0]
            skill_level_id = outer.get(3, [0])[0]
            target_raw = outer.get(6, [0])[0]
            target_uuid = _varint_to_int64(target_raw) if isinstance(target_raw, int) else 0
            logger.info(
                f'[Parser] SyncClientUseSkill: uuid={uuid} skill_id={skill_id} '
                f'skill_level_id={skill_level_id} target={target_uuid}'
            )
            _append_packet_debug('use_skill', {
                'uuid': uuid,
                'skill_id': skill_id,
                'skill_level_id': skill_level_id,
                'target_uuid': target_uuid,
            })
            # Record skill use timestamp for CD tracking
            if _is_player(uuid):
                uid = _uuid_to_uid(uuid)
                if uid == self._current_uid and uid in self._players:
                    player = self._players[uid]
                    if skill_level_id > 0:
                        player.skill_last_use_at[skill_level_id] = time.time()
                        self._remember_seen_skill(player, skill_level_id)
                        self._try_detect_profession(player, skill_level_id)
        except Exception as e:
            logger.debug(f'[Parser] SyncClientUseSkill decode error: {e}')

    def _on_sync_server_skill_end(self, data: bytes):
        """Handle SyncServerSkillEnd (0x3005 / 12293) — skill cast completed.

        SyncServerSkillEnd proto:
            int64 Uuid = 1;
            int32 SkillId = 2;
            int32 SkillLevelId = 3;
        """
        try:
            outer = _decode_fields(data)
            uuid_raw = outer.get(1, [0])[0]
            uuid = _varint_to_int64(uuid_raw) if isinstance(uuid_raw, int) else 0
            skill_id = outer.get(2, [0])[0]
            skill_level_id = outer.get(3, [0])[0]
            logger.debug(
                f'[Parser] SyncServerSkillEnd: uuid={uuid} skill_id={skill_id} '
                f'skill_level_id={skill_level_id}'
            )
            _append_packet_debug('skill_end', {
                'uuid': uuid, 'skill_id': skill_id,
                'skill_level_id': skill_level_id,
            })
        except Exception as e:
            logger.debug(f'[Parser] SyncServerSkillEnd decode error: {e}')

    def _on_sync_server_skill_stage_end(self, data: bytes):
        """Handle SyncServerSkillStageEnd (0x3004 / 12292).

        SyncServerSkillStageEnd proto:
            int64 Uuid = 1;
            int32 SkillId = 2;
            int32 SkillLevelId = 3;
            int32 StageIndex = 4;
        """
        try:
            outer = _decode_fields(data)
            uuid_raw = outer.get(1, [0])[0]
            uuid = _varint_to_int64(uuid_raw) if isinstance(uuid_raw, int) else 0
            skill_id = outer.get(2, [0])[0]
            skill_level_id = outer.get(3, [0])[0]
            stage_index = outer.get(4, [0])[0]
            logger.debug(
                f'[Parser] SyncServerSkillStageEnd: uuid={uuid} skill={skill_id} '
                f'slid={skill_level_id} stage={stage_index}'
            )
        except Exception as e:
            logger.debug(f'[Parser] SyncServerSkillStageEnd decode error: {e}')

    def _on_qte_begin(self, data: bytes):
        """Handle QteBegin (0x3001 / 12289) — QTE event start.

        QteBegin proto:
            int32 QteId = 1;
            int32 QteType = 2;
            float Duration = 3;
        """
        try:
            outer = _decode_fields(data)
            qte_id = outer.get(1, [0])[0]
            qte_type = outer.get(2, [0])[0]
            logger.info(f'[Parser] QteBegin: id={qte_id} type={qte_type}')
            _append_packet_debug('qte_begin', {
                'qte_id': qte_id, 'qte_type': qte_type,
            })
        except Exception as e:
            logger.debug(f'[Parser] QteBegin decode error: {e}')


    # ── Dungeon / Scene notify handlers ──


    def _on_sync_dungeon_data(self, data: bytes):
        """Handle SyncDungeonData (0x17) — dungeon context sync.

        SyncDungeonData proto:
            int32 CurDungeonId = 1;
            repeated DungeonData Datas = 2;
        """
        try:
            outer = _decode_fields(data)
            dungeon_id = outer.get(1, [0])[0]
            logger.info(f'[Parser] SyncDungeonData: dungeon_id={dungeon_id} last={self._last_dungeon_id}')
            _append_packet_debug('dungeon_data', {'dungeon_id': dungeon_id, 'last_dungeon_id': self._last_dungeon_id})
            # Detect dungeon change → reset scene to clear old monsters
            if dungeon_id != self._last_dungeon_id and self._last_dungeon_id != 0:
                print(
                    f'[Parser] ⚡ 副本切换: dungeon {self._last_dungeon_id} → {dungeon_id}, 重置场景',
                    flush=True,
                )
                self.reset_scene()
            self._last_dungeon_id = dungeon_id
            if self._current_uid and self._current_uid in self._players:
                player = self._players[self._current_uid]
                if dungeon_id > 0:
                    player.dungeon_id = dungeon_id
        except Exception as e:
            logger.debug(f'[Parser] SyncDungeonData decode error: {e}')

    def _on_notify_start_playing_dungeon(self, data: bytes):
        """Handle NotifyStartPlayingDungeon (0x37) — dungeon play started.

        NotifyStartPlayingDungeon proto:
            int32 DungeonId = 1;
        """
        try:
            outer = _decode_fields(data)
            dungeon_id = outer.get(1, [0])[0]
            logger.info(f'[Parser] NotifyStartPlayingDungeon: dungeon_id={dungeon_id} last={self._last_dungeon_id}')
            _append_packet_debug('start_dungeon', {'dungeon_id': dungeon_id, 'last_dungeon_id': self._last_dungeon_id})
            # NotifyStartPlayingDungeon definitively means new dungeon → reset scene
            # Guard: skip if _last_dungeon_id == 0 (already reset by server change)
            if dungeon_id != self._last_dungeon_id and self._last_dungeon_id != 0:
                print(
                    f'[Parser] ⚡ 开始副本: dungeon {self._last_dungeon_id} → {dungeon_id}, 重置场景',
                    flush=True,
                )
                self.reset_scene()
            self._last_dungeon_id = dungeon_id
            if self._current_uid and self._current_uid in self._players:
                player = self._players[self._current_uid]
                player.dungeon_id = dungeon_id
        except Exception as e:
            logger.debug(f'[Parser] NotifyStartPlayingDungeon decode error: {e}')


    # ── Login / Session notify handlers ──


    def _on_enter_game(self, data: bytes):
        """Handle EnterGame (0x14) — login notification.

        EnterGame proto:
            int64 Uid = 1;
            string ServerName = 2;
        """
        try:
            outer = _decode_fields(data)
            uid_raw = outer.get(1, [0])[0]
            uid = _varint_to_int64(uid_raw) if isinstance(uid_raw, int) else 0
            server_raw = outer.get(2, [None])[0]
            server_name = server_raw.decode('utf-8', 'ignore') if isinstance(server_raw, bytes) else ''
            logger.info(f'[Parser] EnterGame: uid={uid} server={server_name!r}')
            _append_packet_debug('enter_game', {
                'uid': uid, 'server_name': server_name,
            })
            if uid > 0 and self._current_uid == 0:
                self._current_uid = uid
                logger.info(f'[Parser] auto-adopt UID from EnterGame: {uid}')
        except Exception as e:
            logger.debug(f'[Parser] EnterGame decode error: {e}')

    def _on_notify_revive_user(self, data: bytes):
        """Handle NotifyReviveUser (0x27) — player revived.

        NotifyReviveUser proto:
            int64 Uuid = 1;
            int32 ReviveType = 2;
        """
        try:
            outer = _decode_fields(data)
            uuid_raw = outer.get(1, [0])[0]
            uuid = _varint_to_int64(uuid_raw) if isinstance(uuid_raw, int) else 0
            revive_type = outer.get(2, [0])[0]
            logger.info(f'[Parser] NotifyReviveUser: uuid={uuid} type={revive_type}')
            _append_packet_debug('revive_user', {
                'uuid': uuid, 'revive_type': revive_type,
            })
            # Reset dead state for player
            if _is_player(uuid):
                uid = _uuid_to_uid(uuid)
                if uid in self._players:
                    player = self._players[uid]
                    player.hp = player.max_hp  # Assume full HP on revive
                    if uid == self._current_uid:
                        self._notify_self()
        except Exception as e:
            logger.debug(f'[Parser] NotifyReviveUser decode error: {e}')

    def _on_sync_container_data(self, data: bytes):
        """
        SyncContainerData { CharSerialize VData = 1 }
        CharSerialize {
            int64 CharId = 1;
            CharBaseInfo CharBase = 2;
            EnergyItem EnergyItem = 13;
            UserFightAttr Attr = 16;
            RoleLevel RoleLevel = 22;
            ProfessionList ProfessionList = 61;
        }
        """
        outer = _decode_fields(data)
        vdata_raw = outer.get(1, [None])[0]
        if not vdata_raw or not isinstance(vdata_raw, bytes):
            return
        vdata = _decode_fields(vdata_raw)


        char_id_raw = vdata.get(1, [0])[0]
        if isinstance(char_id_raw, int):
            uid = char_id_raw
        else:
            return
        if uid <= 0:
            return

        logger.info(f'[Parser] SyncContainerData received: uid={uid}, current_uid={self._current_uid}')
        print(f'[Parser] SyncContainerData: uid={uid}, current_uid={self._current_uid}', flush=True)

        # Track how many SyncContainerData we've received.
        # First one = login. Subsequent ones = scene/dungeon transition re-sync.
        self._sync_container_count += 1

        # SyncContainerData 是登录时的完整同步, 如果当前 UID 未知则自动采纳
        if self._current_uid == 0:
            self._current_uid = uid
            logger.info(f'[Parser] auto-adopt self UID from SyncContainerData: {uid}')
        elif self._sync_container_count > 1 and uid == self._current_uid:
            # NOTE: Do NOT call reset_scene() here!
            # SyncContainerData arrives AFTER SyncNearEntities has already populated
            # new monsters with full AttrCollection (including MAX_HP).
            # Calling reset_scene() here would wipe those monsters.
            # Subsequent SyncNearDelta only sends CHANGED attrs (HP changes but
            # MAX_HP is NOT re-sent) → monsters get recreated with max_hp=0
            # → boss bar check `max_hp > 0` fails → bar never shows.
            #
            # Scene resets are handled by:
            #   - Capture layer _on_server_change (cross-server transitions)
            #   - SyncDungeonData (dungeon_id change)
            #   - NotifyStartPlayingDungeon (dungeon start)
            print(
                f'[Parser] SyncContainerData 重新同步 (第{self._sync_container_count}次), '
                f'保留 {len(self._monsters)} 个怪物 (不重置场景)',
                flush=True,
            )

        player = self._get_player(uid)
        changed = False

        # CharBase (field 2)
        char_base_raw = vdata.get(2, [None])[0]
        if isinstance(char_base_raw, bytes):
            cb = _decode_fields(char_base_raw)
            # Name (field 5)
            name_raw = cb.get(5, [None])[0]
            if isinstance(name_raw, bytes):
                name = name_raw.decode('utf-8', 'ignore')
                if name:
                    player.name = name
                    changed = True
                    logger.info(f'[Parser] SyncContainerData CharBase Name={name!r} uid={uid}')
            # FightPoint (field 35)
            fp = cb.get(35, [None])[0]
            if isinstance(fp, int) and fp > 0:
                player.fight_point = fp
                changed = True

        # UserFightAttr (field 16)
        attr_raw = vdata.get(16, [None])[0]
        attr_resource_ids = []
        attr_resources = []
        attr_skill_cd_count = 0
        if isinstance(attr_raw, bytes):
            attr = _decode_fields(attr_raw)
            # CurHp (field 1)
            cur_hp = attr.get(1, [None])[0]
            if isinstance(cur_hp, int):
                player.hp = _varint_to_int64(cur_hp)
                player.hp_from_full_sync = True  # HP from the full sync path is trusted
                changed = True
            # MaxHp (field 2)
            max_hp = attr.get(2, [None])[0]
            if isinstance(max_hp, int):
                player.max_hp = _varint_to_int64(max_hp)
                changed = True
            # OriginEnergy (field 3, wire=5 float)
            energy = attr.get(3, [None])[0]
            if energy is not None:
                new_energy = player.energy
                new_energy_valid = False
                if isinstance(energy, float):
                    new_energy = energy
                    new_energy_valid = math.isfinite(energy) and energy >= 0.0
                elif isinstance(energy, int):
                    new_energy = float(energy)
                    new_energy_valid = energy >= 0
                if (
                    abs(float(new_energy or 0.0) - float(getattr(player, 'energy', 0.0) or 0.0)) > 0.001 or
                    bool(getattr(player, 'energy_valid', False)) != bool(new_energy_valid)
                ):
                    player.energy = new_energy
                    player.energy_valid = bool(new_energy_valid)
                    player.energy_source_priority = 99 if new_energy_valid else 0
                    changed = True
            attr_resource_ids = [v for v in attr.get(4, []) if isinstance(v, int)]
            attr_resources = [v for v in attr.get(5, []) if isinstance(v, int)]
            resource_values = _decode_resource_value_map(attr_resource_ids, attr_resources)
            if resource_values != player.resource_values:
                player.resource_values = resource_values
                changed = True
            skill_cds = []
            for cd_raw in attr.get(9, []):
                if not isinstance(cd_raw, bytes):
                    continue
                decoded_cd = _decode_skill_cd_info(cd_raw)
                if decoded_cd.get('skill_level_id', 0) > 0:
                    skill_cds.append(decoded_cd)
            attr_skill_cd_count = len(skill_cds)
            if self._replace_skill_cds(player, skill_cds):
                changed = True
            if _refresh_stamina_resource(player):
                changed = True

        # EnergyItem (field 13)
        energy_item_raw = vdata.get(13, [None])[0]
        energy_item = {
            'unlock_nums': [],
            'energy_info_map': {},
        }
        if isinstance(energy_item_raw, bytes):
            energy_item = _decode_energy_item(energy_item_raw)
            if energy_item.get('energy_info_map', {}) != player.energy_info_map:
                player.energy_info_map = dict(energy_item.get('energy_info_map', {}))
                changed = True
            total_limit = energy_item['energy_limit'] + energy_item['extra_energy_limit']
            if total_limit > 0:
                player.energy_limit = energy_item['energy_limit']
                player.extra_energy_limit = energy_item['extra_energy_limit']
                changed = True
                logger.info(
                    f'[Parser] SyncContainerData EnergyItem '
                    f'limit={energy_item["energy_limit"]} '
                    f'extra={energy_item["extra_energy_limit"]} uid={uid}'
                )
            elif energy_item['derived_total_limit'] > 0:
                player.energy_limit = energy_item['derived_total_limit']
                player.extra_energy_limit = 0
                changed = True
                logger.info(
                    f'[Parser] SyncContainerData EnergyItem derived_limit='
                    f'{energy_item["derived_total_limit"]} uid={uid}'
                )
            if energy_item['current_energy_value'] > 0:
                player.energy_info_value = energy_item['current_energy_value']
            if _refresh_stamina_resource(player):
                changed = True

        season_medal_raw = vdata.get(52, [None])[0]
        monster_hunt_raw = vdata.get(56, [None])[0]
        season_center_raw = vdata.get(50, [None])[0]
        battlepass_data_raw = vdata.get(86, [None])[0]
        season_medal_level = _decode_season_medal_level(season_medal_raw) if isinstance(season_medal_raw, bytes) else 0
        monster_hunt_level = _decode_monster_hunt_level(monster_hunt_raw) if isinstance(monster_hunt_raw, bytes) else 0
        battlepass_level = _decode_battlepass_level(season_center_raw) if isinstance(season_center_raw, bytes) else 0
        battlepass_data_level = _decode_battlepass_data_level(battlepass_data_raw) if isinstance(battlepass_data_raw, bytes) else 0
        player.season_medal_level = season_medal_level
        player.monster_hunt_level = monster_hunt_level
        player.battlepass_level = battlepass_level
        player.battlepass_data_level = battlepass_data_level
        player.season_level = max(season_medal_level, monster_hunt_level)
        if _set_level_extra_candidate(player, 'season_medal', season_medal_level):
            changed = True
        if _set_level_extra_candidate(player, 'monster_hunt', monster_hunt_level):
            changed = True
        if _set_level_extra_candidate(player, 'battlepass', battlepass_level):
            changed = True
        if _set_level_extra_candidate(player, 'battlepass_data', battlepass_data_level):
            changed = True
        _append_packet_debug(
            'sync_container_data',
            {
                'uid': uid,
                'energy_limit': player.energy_limit,
                'extra_energy_limit': player.extra_energy_limit,
                'energy': player.energy,
                'energy_valid': player.energy_valid,
                'energy_info_value': player.energy_info_value,
                'energy_info_map': player.energy_info_map,
                'energy_unlock_nums': energy_item.get('unlock_nums', []),
                'resource_values': player.resource_values,
                'stamina_resource_id': player.stamina_resource_id,
                'level_extra': player.level_extra,
                'level_extra_source': player.level_extra_source,
                'season_level': player.season_level,
                'season_medal_level': season_medal_level,
                'monster_hunt_level': monster_hunt_level,
                'battlepass_level': battlepass_level,
                'battlepass_data_level': battlepass_data_level,
                'attr_resource_ids': attr_resource_ids,
                'attr_resources': attr_resources,
                'attr_skill_cd_count': attr_skill_cd_count,
            }
        )

        role_lv_raw = vdata.get(22, [None])[0]
        if isinstance(role_lv_raw, bytes):
            rl = _decode_fields(role_lv_raw)
            role_level = rl.get(1, [None])[0]
            prev_season_max_lv = rl.get(11, [None])[0]
            role_level_debug = {
                'uid': uid,
                'role_level': role_level,
                'prev_season_max_lv': prev_season_max_lv,
                'last_season_day': rl.get(6, [None])[0],
                'bless_exp_pool': rl.get(7, [None])[0],
                'grant_bless_exp': rl.get(8, [None])[0],
                'accumulate_bless_exp': rl.get(9, [None])[0],
                'accumulate_exp': rl.get(10, [None])[0],
            }
            logger.info(
                f'[Parser] SyncContainerData RoleLevel raw_fields={dict(rl)}, '
                f'role_level={role_level}, prev_season_max_lv={prev_season_max_lv}, uid={uid}'
            )
            _append_packet_debug('role_level', role_level_debug)
            if isinstance(role_level, int) and role_level > 0:
                # Upstream packet.js treats RoleLevel.Level like the visible level.
                player.level = role_level
                changed = True
                print(f'[Parser] SyncContainerData: 等级 Lv.{role_level} uid={uid}', flush=True)
            else:
                logger.warning(
                    f'[Parser] SyncContainerData RoleLevel: level NOT parsed! '
                    f'raw={role_level!r} type={type(role_level).__name__} uid={uid}'
                )
        else:
            logger.warning(
                f'[Parser] SyncContainerData: field 22 (RoleLevel) missing or not bytes! '
                f'raw={role_lv_raw!r} uid={uid}'
            )

        # Slots (field 55) — full skill bar layout including resonance slots 7,8
        slots_bar_raw = vdata.get(55, [None])[0]
        if isinstance(slots_bar_raw, bytes):
            slot_bar = _decode_slot_bar(slots_bar_raw)
            if slot_bar and slot_bar != player.slot_bar_map:
                player.slot_bar_map = dict(slot_bar)
                changed = True
                logger.info(f'[Parser] SyncContainerData Slots(55) bar={slot_bar} uid={uid}')
                _append_packet_debug(
                    'slot_bar',
                    {'uid': uid, 'slot_bar_map': slot_bar}
                )

        # ProfessionList (field 61)
        prof_raw = vdata.get(61, [None])[0]
        if isinstance(prof_raw, bytes):
            profession_data = _decode_profession_list(prof_raw)
            pid = int(profession_data.get('profession_id') or 0)
            if pid > 0:
                player.profession_id = pid
                player.profession = PROFESSION_NAMES.get(pid, '')
                changed = True
            slot_skill_map = profession_data.get('slot_skill_level_map') or {}
            skill_level_info_map = profession_data.get('skill_level_info_map') or {}
            if pid > 0 and slot_skill_map:
                self._profession_skill_cache[pid] = dict(slot_skill_map)
            elif pid > 0 and not slot_skill_map:
                slot_skill_map = self._profession_skill_cache.get(pid) or {}
            if slot_skill_map != player.skill_slot_map:
                player.skill_slot_map = dict(slot_skill_map)
                changed = True
            merged_skill_info = dict(getattr(player, 'skill_level_info_map', {}) or {})
            merged_skill_info.update(skill_level_info_map)
            if merged_skill_info != player.skill_level_info_map:
                player.skill_level_info_map = merged_skill_info
                changed = True
            _append_packet_debug(
                'profession_list',
                {
                    'uid': uid,
                    'profession_id': pid,
                    'slot_skill_level_map': slot_skill_map,
                    'active_skill_ids': profession_data.get('active_skill_ids') or [],
                    'skill_info_keys': sorted((profession_data.get('skill_info_map') or {}).keys()),
                    'skill_level_info_keys': sorted((profession_data.get('skill_level_info_map') or {}).keys()),
                }
            )

        # ── Parse ALL remaining CharSerialize fields (generic decode) ──
        # Fields 1-98 not in _HANDLED_CHAR_FIELDS are decoded generically
        # and stored in player.extended_data for completeness.
        #
        # Specific useful fields get dedicated decoders:
        #   3 (SceneData) → scene_id
        #   6 (BuffDBInfo) → buff_list
        #   12 (EquipList) → equip info
        #   15 (DungeonList) → dungeon_id
        #   28 (Resonance) → resonance data

        # SceneData (field 3) — current scene/map
        scene_raw = vdata.get(CharField.SCENE_DATA, [None])[0]
        if isinstance(scene_raw, bytes):
            scene = _decode_scene_data(scene_raw)
            new_scene_id = scene.get('scene_id', 0)
            if new_scene_id > 0:
                player.scene_id = new_scene_id
                changed = True
                # Track scene_id changes at parser level
                if self._last_scene_id != 0 and new_scene_id != self._last_scene_id:
                    logger.info(
                        f'[Parser] 场景ID变更: {self._last_scene_id} → {new_scene_id} uid={uid}'
                    )
                self._last_scene_id = new_scene_id
            player.extended_data['SCENE_DATA'] = scene
            logger.info(f'[Parser] CharSerialize SceneData: {scene} uid={uid}')

        # BuffDBInfo (field 6) — active buffs on self
        buff_db_raw = vdata.get(CharField.BUFF_DB_INFO, [None])[0]
        if isinstance(buff_db_raw, bytes):
            buffs = _decode_buff_db_info(buff_db_raw)
            player.buff_list = buffs
            player.extended_data['BUFF_DB_INFO'] = {'buff_count': len(buffs)}
            changed = True
            logger.info(f'[Parser] CharSerialize BuffDBInfo: {len(buffs)} buffs uid={uid}')

        # EquipList (field 12) — equipped items
        equip_raw = vdata.get(CharField.EQUIP_LIST, [None])[0]
        if isinstance(equip_raw, bytes):
            equip_data = _decode_equip_list(equip_raw)
            player.extended_data['EQUIP_LIST'] = equip_data
            logger.info(f'[Parser] CharSerialize EquipList: {equip_data.get("equip_count",0)} items uid={uid}')

        # DungeonList (field 15) — dungeon data
        dungeon_raw = vdata.get(CharField.DUNGEON_LIST, [None])[0]
        if isinstance(dungeon_raw, bytes):
            dungeon_data = _decode_dungeon_list(dungeon_raw)
            if dungeon_data.get('cur_dungeon_id', 0) > 0:
                player.dungeon_id = dungeon_data['cur_dungeon_id']
            player.extended_data['DUNGEON_LIST'] = dungeon_data
            logger.info(f'[Parser] CharSerialize DungeonList: {dungeon_data} uid={uid}')

        # Resonance (field 28) — resonance system
        resonance_raw = vdata.get(CharField.RESONANCE, [None])[0]
        if isinstance(resonance_raw, bytes):
            res_data = _decode_resonance_data(resonance_raw)
            player.extended_data['RESONANCE'] = res_data
            logger.info(f'[Parser] CharSerialize Resonance: {res_data} uid={uid}')

        # Generic decode for ALL other CharSerialize fields
        _generic_decoded_fields = []
        for _fnum in range(1, 99):
            if _fnum in _HANDLED_CHAR_FIELDS:
                continue
            # Skip fields already handled above with specific decoders
            if _fnum in (CharField.SCENE_DATA, CharField.BUFF_DB_INFO,
                         CharField.EQUIP_LIST, CharField.DUNGEON_LIST,
                         CharField.RESONANCE):
                continue
            _fraw = vdata.get(_fnum, [None])[0]
            if isinstance(_fraw, bytes) and len(_fraw) > 0:
                _fname = CHAR_FIELD_NAMES.get(_fnum, f'FIELD_{_fnum}')
                try:
                    _fdecoded = _decode_fields(_fraw)
                    player.extended_data[_fname] = _fields_to_debug_dict(_fdecoded, max_depth=2)
                except Exception:
                    player.extended_data[_fname] = {'raw_size': len(_fraw)}
                _generic_decoded_fields.append(f'{_fname}({_fnum}:{len(_fraw)}B)')

        if _generic_decoded_fields:
            logger.info(
                f'[Parser] CharSerialize generic decoded {len(_generic_decoded_fields)} fields: '
                f'{", ".join(_generic_decoded_fields[:20])} uid={uid}'
            )
            _append_packet_debug('char_serialize_generic', {
                'uid': uid,
                'decoded_fields': _generic_decoded_fields,
            })

        if changed:
            if uid == self._current_uid:
                self._notify_self()
            print(
                f'[Parser] SyncContainerData 完成: uid={uid} Lv.{player.level} '
                f'HP={player.hp}/{player.max_hp} 职业={player.profession!r}({player.profession_id}) '
                f'slots={len(player.skill_slot_map)} slot_bar={len(player.slot_bar_map)} '
                f'cd_count={len(player.skill_cd_map)} energy={player.energy:.1f}',
                flush=True,
            )


    # SyncContainerDirtyData (incremental updates)


    def _on_sync_container_dirty(self, data: bytes):
        """Handle the custom dirty-data stream wrapper."""
        if self._current_uid == 0:
            logger.debug('[Parser] _on_sync_container_dirty: skipped (no current_uid)')
            return

        outer = _decode_fields(data)
        buf_raw = outer.get(1, [None])[0]
        if not isinstance(buf_raw, bytes):
            return
        buf_fields = _decode_fields(buf_raw)
        buf_bytes = buf_fields.get(1, [None])[0]
        if not isinstance(buf_bytes, bytes) or len(buf_bytes) < 8:
            return

        self._parse_dirty_stream(buf_bytes)

    def _parse_dirty_stream(self, data: bytes):
        """Parse the custom dirty-data binary stream used by V3.3.6."""
        pos = 0
        uid = self._current_uid
        player = self._get_player(uid)
        changed = False


        if pos + 8 > len(data):
            return
        ident = struct.unpack_from('<I', data, pos)[0]
        if ident != 0xFFFFFFFE:
            return
        pos += 4
        # skip validation int32BE
        pos += 4

        if pos + 4 > len(data):
            return
        field_index = struct.unpack_from('<I', data, pos)[0]
        pos += 4
        debug_info = {
            'uid': uid,
            'field_index': field_index,
        }

        if field_index == 2:  # CharBase

            if pos + 8 > len(data):
                return
            ident2 = struct.unpack_from('<I', data, pos)[0]
            if ident2 != 0xFFFFFFFE:
                return
            pos += 8  # skip identifier + validation
            if pos + 4 > len(data):
                return
            sub_field = struct.unpack_from('<I', data, pos)[0]
            pos += 4
            if sub_field == 5:  # Name
                if pos + 4 > len(data):
                    return
                str_len = struct.unpack_from('<I', data, pos)[0]
                pos += 4
                if pos + str_len > len(data):
                    return
                name = data[pos:pos + str_len].decode('utf-8', 'ignore')
                if name:
                    player.name = name
                    changed = True
            elif sub_field == 35:  # FightPoint
                if pos + 4 > len(data):
                    return
                fp = struct.unpack_from('<I', data, pos)[0]
                if fp > 0:
                    player.fight_point = fp
                    changed = True

        elif field_index == 16:  # UserFightAttr
            if pos + 8 > len(data):
                return
            ident2 = struct.unpack_from('<I', data, pos)[0]
            if ident2 != 0xFFFFFFFE:
                return
            pos += 8
            if pos + 4 > len(data):
                return
            sub_field = struct.unpack_from('<I', data, pos)[0]
            pos += 4
            debug_info['sub_field'] = sub_field
            if sub_field == 1:  # CurHp
                if pos + 4 > len(data):
                    return
                hp = struct.unpack_from('<I', data, pos)[0]
                debug_info['u32'] = hp

                if hp > 0 or player.max_hp == 0:
                    player.hp = hp
                    changed = True
                else:
                    logger.debug(f'[Parser] DirtyData ignored CurHp=0 (max_hp={player.max_hp})')
            elif sub_field == 2:  # MaxHp
                if pos + 4 > len(data):
                    return
                max_hp = struct.unpack_from('<I', data, pos)[0]
                debug_info['u32'] = max_hp
                if max_hp > 0:
                    player.max_hp = max_hp
                    changed = True
                else:
                    logger.debug(f'[Parser] DirtyData ignored MaxHp=0 (current={player.max_hp})')
            elif sub_field == 3:  # OriginEnergy (stamina)
                if pos + 4 > len(data):
                    return

                try:
                    energy_f = struct.unpack_from('<f', data, pos)[0]
                    energy_i = struct.unpack_from('<I', data, pos)[0]
                    stamina_max = max(0, player.energy_limit) + max(0, player.extra_energy_limit)
                    energy_v = _decode_dirty_energy_value(energy_i, energy_f, stamina_max=stamina_max)
                    if energy_v is not None:
                        player.energy = energy_v
                        player.energy_valid = True
                        player.energy_source_priority = 99
                        changed = True
                        debug_info.update({
                            'energy_float': energy_f,
                            'energy_int': energy_i,
                            'energy_picked': energy_v,
                            'stamina_max': stamina_max,
                        })
                        logger.debug(
                            f'[Parser] DirtyData OriginEnergy: '
                            f'f={energy_f}, i={energy_i}, picked={energy_v}'
                        )
                except Exception:
                    pass
            else:
                debug_info['raw_hex'] = data[pos:].hex()[:128]

        elif field_index == 22:  # RoleLevel
            if pos + 8 > len(data):
                return
            ident2 = struct.unpack_from('<I', data, pos)[0]
            if ident2 != 0xFFFFFFFE:
                return
            pos += 8
            if pos + 4 > len(data):
                return
            sub_field = struct.unpack_from('<I', data, pos)[0]
            pos += 4
            debug_info['sub_field'] = sub_field
            if sub_field == 1:  # Level
                if pos + 4 > len(data):
                    return
                lv = struct.unpack_from('<I', data, pos)[0]
                debug_info['u32'] = lv
                if lv > 0:
                    player.level = lv
                    changed = True
                    debug_info['role_level'] = lv
                    logger.info(f'[Parser] DirtyData Level -> {lv}')
            elif pos + 4 <= len(data):
                debug_info['u32'] = struct.unpack_from('<I', data, pos)[0]
                debug_info['raw_hex'] = data[pos:].hex()[:128]

        elif field_index == 50:  # SeasonCenter
            if pos + 8 > len(data):
                return
            ident2 = struct.unpack_from('<I', data, pos)[0]
            if ident2 != 0xFFFFFFFE:
                return
            pos += 8
            if pos + 4 > len(data):
                return
            sub_field = struct.unpack_from('<I', data, pos)[0]
            pos += 4
            debug_info['sub_field'] = sub_field
            if sub_field == 2:  # BattlePass
                if pos + 8 > len(data):
                    return
                ident3 = struct.unpack_from('<I', data, pos)[0]
                if ident3 != 0xFFFFFFFE:
                    return
                pos += 8
                if pos + 4 > len(data):
                    return
                bp_sub_field = struct.unpack_from('<I', data, pos)[0]
                pos += 4
                debug_info['nested_sub_field'] = bp_sub_field
                if bp_sub_field == 2 and pos + 4 <= len(data):  # BattlePass.Level
                    battlepass_lv = struct.unpack_from('<I', data, pos)[0]
                    debug_info['u32'] = battlepass_lv
                    if battlepass_lv > 0:
                        player.battlepass_level = battlepass_lv
                        debug_info['battlepass_level'] = battlepass_lv
                        logger.info(f'[Parser] DirtyData SeasonCenter.BattlePass.Level -> {battlepass_lv}')
                        if _set_level_extra_candidate(player, 'battlepass', battlepass_lv):
                            changed = True
                else:
                    debug_info['raw_hex'] = data[pos:].hex()[:128]
            else:
                debug_info['raw_hex'] = data[pos:].hex()[:128]

        elif field_index == 52:  # SeasonMedalInfo
            if pos + 8 > len(data):
                return
            ident2 = struct.unpack_from('<I', data, pos)[0]
            if ident2 != 0xFFFFFFFE:
                return
            pos += 8
            if pos + 4 > len(data):
                return
            sub_field = struct.unpack_from('<I', data, pos)[0]
            pos += 4
            debug_info['sub_field'] = sub_field
            if sub_field == 3:  # CoreHoleInfo
                if pos + 8 > len(data):
                    return
                ident3 = struct.unpack_from('<I', data, pos)[0]
                if ident3 != 0xFFFFFFFE:
                    return
                pos += 8
                if pos + 4 > len(data):
                    return
                hole_sub_field = struct.unpack_from('<I', data, pos)[0]
                pos += 4
                debug_info['nested_sub_field'] = hole_sub_field
                if hole_sub_field == 2 and pos + 4 <= len(data):  # HoleLevel
                    medal_lv_raw = struct.unpack_from('<I', data, pos)[0]
                    medal_lv = _normalize_season_medal_level(medal_lv_raw)
                    debug_info['u32'] = medal_lv_raw
                    debug_info['season_medal_level_raw'] = medal_lv_raw
                    if medal_lv > 0:
                        player.season_medal_level = medal_lv
                        player.season_level = max(player.season_level, medal_lv)
                        debug_info['season_medal_level'] = medal_lv
                        logger.info(
                            f'[Parser] DirtyData SeasonMedalInfo.CoreHoleInfo.HoleLevel '
                            f'raw={medal_lv_raw} normalized={medal_lv}'
                        )
                        if _set_level_extra_candidate(player, 'season_medal', medal_lv):
                            changed = True
                else:
                    debug_info['raw_hex'] = data[pos:].hex()[:128]
            else:
                debug_info['raw_hex'] = data[pos:].hex()[:128]

        elif field_index == 56:  # MonsterHuntInfo
            if pos + 8 > len(data):
                return
            ident2 = struct.unpack_from('<I', data, pos)[0]
            if ident2 != 0xFFFFFFFE:
                return
            pos += 8
            if pos + 4 > len(data):
                return
            sub_field = struct.unpack_from('<I', data, pos)[0]
            pos += 4
            debug_info['sub_field'] = sub_field
            if sub_field == 2 and pos + 4 <= len(data):  # CurLevel
                hunt_lv = struct.unpack_from('<I', data, pos)[0]
                debug_info['u32'] = hunt_lv
                if hunt_lv > 0:
                    player.monster_hunt_level = hunt_lv
                    player.season_level = max(player.season_level, hunt_lv)
                    debug_info['monster_hunt_level'] = hunt_lv
                    logger.info(f'[Parser] DirtyData MonsterHuntInfo.CurLevel -> {hunt_lv}')
                    if _set_level_extra_candidate(player, 'monster_hunt', hunt_lv):
                        changed = True
            else:
                debug_info['raw_hex'] = data[pos:].hex()[:128]

        elif field_index == 86:  # BattlePassData
            debug_info['raw_hex'] = data[pos:].hex()[:128]

        elif field_index == 61:  # ProfessionList
            if pos + 8 > len(data):
                return
            ident2 = struct.unpack_from('<I', data, pos)[0]
            if ident2 != 0xFFFFFFFE:
                return
            pos += 8
            if pos + 4 > len(data):
                return
            sub_field = struct.unpack_from('<I', data, pos)[0]
            pos += 4
            debug_info['sub_field'] = sub_field
            if sub_field == 1:  # CurProfessionId
                if pos + 4 > len(data):
                    return
                pid = struct.unpack_from('<I', data, pos)[0]
                debug_info['u32'] = pid
                if pid > 0:
                    player.profession_id = pid
                    player.profession = PROFESSION_NAMES.get(pid, '')
                    changed = True
                    if self._apply_cached_profession_slots(player):
                        changed = True

        else:
            # Generic handler for ALL other CharSerialize dirty fields
            _fname = CHAR_FIELD_NAMES.get(field_index, f'FIELD_{field_index}')
            debug_info['field_name'] = _fname
            debug_info['raw_hex'] = data[pos:].hex()[:128]
            logger.debug(f'[Parser] DirtyData unhandled field {_fname} (idx={field_index})')

        # Log ALL dirty updates (not just the handled ones)
        _append_packet_debug('dirty_update', debug_info)
        if changed:
            self._notify_self()


    # SyncNearEntities (0x06)


    def _on_sync_near_entities(self, data: bytes):
        outer = _decode_fields(data)

        # ── Disappear (field 2, repeated) ──
        disappear_list = outer.get(2, [])
        for entity_raw in disappear_list:
            if not isinstance(entity_raw, bytes):
                continue
            ef = _decode_fields(entity_raw)
            uuid_raw = ef.get(1, [0])[0]
            if not isinstance(uuid_raw, int) or uuid_raw == 0:
                continue
            uuid = _varint_to_int64(uuid_raw)
            disappear_type = ef.get(2, [0])[0]  # EDisappearType: 1=Dead, 2=FarAway, ...

            if _is_monster(uuid) and uuid in self._monsters:
                monster = self._monsters[uuid]
                if disappear_type == 1:  # EDisappearDead
                    monster.is_dead = True
                    monster.hp = 0
                    monster.last_update = time.time()
                    logger.info(f'[Parser] Monster DEAD (disappear) uuid={uuid} name={monster.name!r}')
                    self._notify_monster(monster)
                else:
                    logger.debug(f'[Parser] Monster disappeared type={disappear_type} uuid={uuid}')

        # ── Appear (field 1, repeated) ──
        appear_list = outer.get(1, [])
        player_uids_appeared = []
        monster_uuids_appeared = []
        for entity_raw in appear_list:
            if not isinstance(entity_raw, bytes):
                continue
            ef = _decode_fields(entity_raw)
            uuid_raw = ef.get(1, [0])[0]
            if not isinstance(uuid_raw, int) or uuid_raw == 0:
                continue
            uuid = _varint_to_int64(uuid_raw)
            ent_type = ef.get(2, [0])[0]

            # AttrCollection (field 3)
            attr_raw = ef.get(3, [None])[0]
            # TempAttrCollection (field 4)
            temp_attr_raw = ef.get(4, [None])[0]
            # BuffInfoSync (field 7) — initial buff state
            buff_info_raw = ef.get(EntityField.BUFF_INFOS, [None])[0]

            if _is_player(uuid):
                uid = _uuid_to_uid(uuid)
                player_uids_appeared.append(uid)
                if isinstance(attr_raw, bytes):
                    self._process_attr_collection(uid, attr_raw)
                if isinstance(temp_attr_raw, bytes):
                    if uid == self._current_uid or self._current_uid == 0:
                        self._process_temp_attr_collection(uid, temp_attr_raw)
                # Decode initial buff state from entity appear
                if isinstance(buff_info_raw, bytes):
                    if uid == self._current_uid or self._current_uid == 0:
                        player = self._get_player(uid)
                        buffs = _decode_buff_info_sync(buff_info_raw)
                        if buffs:
                            player.buff_list = buffs
                            logger.info(f'[Parser] Entity appear: {len(buffs)} buffs on player uid={uid}')
                # Log player state after entity appear for debugging
                if uid == self._current_uid or self._current_uid == 0:
                    player = self._get_player(uid)
                    logger.info(
                        f'[Parser] Entity appear SELF: uid={uid} lv={player.level} '
                        f'hp={player.hp}/{player.max_hp} prof={player.profession!r} '
                        f'slots={len(player.skill_slot_map)}'
                    )
                    print(
                        f'[Parser] 玩家实体出现: uid={uid} Lv.{player.level} '
                        f'HP={player.hp}/{player.max_hp} 职业={player.profession!r}',
                        flush=True
                    )
                    # If this is a re-appear (scene change), trigger a player update
                    # to ensure bridge/webview gets fresh data immediately
                    self._notify_self()
            elif _is_monster(uuid):
                monster_uuids_appeared.append(uuid)
                # Reset dead state on re-appear
                monster = self._get_monster(uuid)
                monster.is_dead = False
                if isinstance(attr_raw, bytes):
                    self._process_monster_attr_collection(uuid, attr_raw)
                # If max_hp still unknown after attr processing, try cache
                if monster.max_hp == 0 and monster.template_id > 0:
                    cached_max = self._monster_hp_cache.get(monster.template_id)
                    if cached_max and cached_max > 0:
                        monster.max_hp = cached_max
                        logger.info(
                            f'[Parser] Monster appear: max_hp from cache '
                            f'tid={monster.template_id} max_hp={cached_max} uuid={uuid}'
                        )
                # Decode initial buff state on monster
                if isinstance(buff_info_raw, bytes):
                    buffs = _decode_buff_info_sync(buff_info_raw)
                    if buffs:
                        monster.buff_list = buffs
                        logger.debug(f'[Parser] Entity appear: {len(buffs)} buffs on monster uuid={uuid}')
                # Always notify on appear so boss bar can track new scene monsters
                # (_process_monster_attr_collection only notifies if attrs changed)
                monster.last_update = time.time()
                self._notify_monster(monster)

        if player_uids_appeared:
            is_self = self._current_uid in player_uids_appeared
            logger.info(
                f'[Parser] SyncNearEntities: {len(player_uids_appeared)} players appeared '
                f'(self={is_self}, current_uid={self._current_uid}, '
                f'appeared_uids={player_uids_appeared[:5]})'
            )
        if monster_uuids_appeared:
            logger.info(
                f'[Parser] SyncNearEntities: {len(monster_uuids_appeared)} monsters appeared '
                f'(uuids={monster_uuids_appeared[:5]})'
            )


    #  SyncToMeDeltaInfo (0x2E)


    def _on_sync_to_me_delta(self, data: bytes):
        outer = _decode_fields(data)
        delta_raw = outer.get(1, [None])[0]
        if not isinstance(delta_raw, bytes):
            return

        df = _decode_fields(delta_raw)
        player = None

        # UUID helpers
        uuid_raw = df.get(5, [0])[0]
        if isinstance(uuid_raw, int) and uuid_raw != 0:
            uuid = _varint_to_int64(uuid_raw)
            if _is_player(uuid):
                new_uid = _uuid_to_uid(uuid)
                player = self._get_player(new_uid)
                if self._current_uuid != uuid:
                    self._current_uuid = uuid
                    self._current_uid = new_uid
                    logger.info(f'[Parser] confirmed self UUID={uuid}, UID={new_uid}')
                    if new_uid in self._players:
                        self._notify_self()

        skill_cd_changed = False
        sync_skill_cds = []
        fight_res_cds = []
        if player is not None:
            for cd_raw in df.get(3, []):
                if not isinstance(cd_raw, bytes):
                    continue
                decoded_cd = _decode_skill_cd(cd_raw)
                if decoded_cd.get('skill_level_id', 0) <= 0:
                    continue
                sync_skill_cds.append(decoded_cd)
                if self._update_skill_cd(player, decoded_cd):
                    skill_cd_changed = True
                # Log when CD acceleration is active (光盾被动 / 时间法令 etc.)
                accel = int(decoded_cd.get('accelerate_cd_ratio') or 0)
                sub_r = int(decoded_cd.get('sub_cd_ratio') or 0)
                sub_f = int(decoded_cd.get('sub_cd_fixed') or 0)
                if accel > 0 or sub_r > 0 or sub_f > 0:
                    logger.info(
                        f'[Parser] SkillCD acceleration: slid={decoded_cd["skill_level_id"]} '
                        f'accel_ratio={accel} sub_ratio={sub_r} sub_fixed={sub_f} '
                        f'duration={decoded_cd.get("duration")} valid_cd={decoded_cd.get("valid_cd_time")}'
                    )
            if sync_skill_cds:
                _append_packet_debug(
                    'sync_skill_cd',
                    {
                        'uid': player.uid,
                        'skill_cds': sync_skill_cds,
                    }
                )
            else:
                # Log when SyncToMeDelta arrives but has no skill CDs
                # (helps diagnose missing skill data)
                raw_field3 = df.get(3, [])
                if raw_field3:
                    _append_packet_debug(
                        'sync_skill_cd_empty',
                        {
                            'uid': player.uid,
                            'raw_field3_count': len(raw_field3),
                            'raw_field3_types': [type(x).__name__ for x in raw_field3[:5]],
                        }
                    )
            for fight_cd_raw in df.get(4, []):
                if not isinstance(fight_cd_raw, bytes):
                    continue
                decoded_fight_cd = _decode_fight_res_cd(fight_cd_raw)
                res_id = decoded_fight_cd.get('res_id', 0)
                if res_id > 0:
                    fight_res_cds.append(decoded_fight_cd)
                    # Store on player for resource CD tracking
                    player.fight_res_cd_map[res_id] = {
                        'res_id': res_id,
                        'begin_time': decoded_fight_cd.get('begin_time', 0),
                        'duration': decoded_fight_cd.get('duration', 0),
                        'valid_cd_time': decoded_fight_cd.get('valid_cd_time', 0),
                        'observed_at_ms': int(time.time() * 1000),
                    }
            if fight_res_cds:
                _append_packet_debug(
                    'fight_res_cd',
                    {
                        'uid': player.uid,
                        'fight_res_cds': fight_res_cds,
                    }
                )

        # AoiSyncDelta handling
        base_raw = df.get(1, [None])[0]
        if isinstance(base_raw, bytes):
            self._process_aoi_sync_delta(base_raw)
        if skill_cd_changed and player is not None and player.uid == self._current_uid:
            self._notify_self()


    #  SyncNearDeltaInfo (0x2D)


    def _on_sync_near_delta(self, data: bytes):
        outer = _decode_fields(data)
        for delta_raw in outer.get(1, []):
            if isinstance(delta_raw, bytes):
                self._process_aoi_sync_delta(delta_raw)


    # AoiSyncDelta handling


    def _process_aoi_sync_delta(self, data: bytes):
        df = _decode_fields(data)
        uuid_raw = df.get(1, [0])[0]
        if not isinstance(uuid_raw, int) or uuid_raw == 0:
            return
        uuid = _varint_to_int64(uuid_raw)
        target_is_player = _is_player(uuid)
        target_is_monster = _is_monster(uuid)
        uid = _uuid_to_uid(uuid)

        # Attrs (field 2) — players and monsters
        attr_raw = df.get(2, [None])[0]
        if isinstance(attr_raw, bytes):
            if target_is_player:
                self._process_attr_collection(uid, attr_raw)
            elif target_is_monster:
                self._process_monster_attr_collection(uuid, attr_raw)

        # TempAttrs (field 3) — buff-based temporary attributes (CD modifiers etc.)
        temp_attr_raw = df.get(3, [None])[0]
        if isinstance(temp_attr_raw, bytes) and target_is_player:
            if uid == self._current_uid or self._current_uid == 0:
                self._process_temp_attr_collection(uid, temp_attr_raw)

        # BuffEffectSync (field 11) — boss buff events
        buff_effect_raw = df.get(11, [None])[0]
        if isinstance(buff_effect_raw, bytes) and target_is_monster:
            self._process_buff_effect_sync(uuid, buff_effect_raw)

        # BuffInfoSync (field 10) — buff state updates
        buff_info_raw = df.get(DeltaField.BUFF_INFOS, [None])[0]
        if isinstance(buff_info_raw, bytes):
            buffs = _decode_buff_info_sync(buff_info_raw)
            if buffs:
                if target_is_player:
                    if uid in self._players:
                        self._players[uid].buff_list = buffs
                elif target_is_monster and uuid in self._monsters:
                    self._monsters[uuid].buff_list = buffs

        # PassiveSkillInfos (field 8) — passive skill triggers
        passive_raw = df.get(DeltaField.PASSIVE_SKILL_INFOS, [None])[0]
        if isinstance(passive_raw, bytes):
            logger.debug(f'[Parser] AoiDelta PassiveSkillInfos uuid={uuid} len={len(passive_raw)}')

        # PassiveSkillEndInfos (field 9) — passive skill end
        passive_end_raw = df.get(DeltaField.PASSIVE_SKILL_END_INFOS, [None])[0]
        if isinstance(passive_end_raw, bytes):
            logger.debug(f'[Parser] AoiDelta PassiveSkillEndInfos uuid={uuid} len={len(passive_end_raw)}')

        # SkillEffect (field 7) — damage extraction
        skill_effect_raw = df.get(7, [None])[0]
        if isinstance(skill_effect_raw, bytes):
            self._process_skill_effect(uuid, target_is_player, target_is_monster, skill_effect_raw)

    def _process_skill_effect(self, target_uuid: int, target_is_player: bool,
                              target_is_monster: bool, data: bytes):
        """Decode SkillEffect (AoiSyncDelta field 7) and emit damage events.

        SkillEffect {
            int64 Uuid = 1;
            repeated SyncDamageInfo Damages = 2;
            int64 TotalDamage = 3;
        }
        SyncDamageInfo fields: see star_resonance.proto lines 5107-5132.
        """
        se = _decode_fields(data)
        for dmg_raw in se.get(2, []):
            if not isinstance(dmg_raw, bytes):
                continue
            try:
                self._decode_sync_damage_info(target_uuid, target_is_player,
                                              target_is_monster, dmg_raw)
            except Exception as e:
                logger.debug(f'[Parser] damage decode error: {e}')

    def _decode_sync_damage_info(self, target_uuid: int, target_is_player: bool,
                                 target_is_monster: bool, data: bytes):
        """Decode a single SyncDamageInfo and fire on_damage callback."""
        df = _decode_fields(data)
        damage_type = df.get(4, [0])[0]     # EDamageType: 0=Normal, 1=Miss, 2=Heal, 3=Immune, 4=Fall, 5=Absorbed
        # Miss and Fall are truly irrelevant — skip them.
        # Immune and Absorbed are now emitted for invincibility detection.
        if damage_type in (DamageType.MISS, DamageType.FALL):
            return
        is_heal = damage_type == DamageType.HEAL
        is_immune = damage_type == DamageType.IMMUNE
        is_absorbed = damage_type == DamageType.ABSORBED
        type_flag = df.get(5, [0])[0]       # bit0=crit, bit2=cause_lucky
        value = df.get(6, [0])[0]           # Primary damage/heal amount
        actual_value = df.get(7, [0])[0]
        lucky_value = df.get(8, [0])[0]     # Non-zero when lucky proc
        hp_lessen = df.get(9, [0])[0]       # Actual HP reduction on target
        shield_lessen = df.get(10, [0])[0]  # Shield damage absorbed
        attacker_raw = df.get(11, [0])[0]   # AttackerUuid
        skill_id = df.get(12, [0])[0]       # OwnerId = skill ID
        is_dead = bool(df.get(17, [0])[0])  # Target died
        element = df.get(18, [0])[0]        # EDamageProperty
        top_summoner_raw = df.get(21, [0])[0]  # TopSummonerId (real owner if summon)

        # Use signed int64 for UUIDs
        attacker_uuid = _varint_to_int64(top_summoner_raw) if top_summoner_raw else _varint_to_int64(attacker_raw)
        damage_amount = value if value else lucky_value

        # For Immune/Absorbed events, allow zero-damage through (they signal invincibility)
        if not (is_immune or is_absorbed):
            if damage_amount <= 0 and hp_lessen <= 0:
                return

        # Determine if this is self-outgoing damage (self attacks monster)
        attacker_is_self = False
        if self._current_uuid and attacker_uuid:
            if attacker_uuid == self._current_uuid:
                attacker_is_self = True
            elif _is_player(attacker_uuid) and _uuid_to_uid(attacker_uuid) == self._current_uid:
                attacker_is_self = True
        elif self._current_uid and attacker_uuid and _is_player(attacker_uuid):
            # Fallback: _current_uuid not yet known (SyncToMeDelta not received),
            # but _current_uid is available from SyncContainerData / cache.
            if _uuid_to_uid(attacker_uuid) == self._current_uid:
                attacker_is_self = True

        event = {
            'target_uuid': target_uuid,
            'target_is_player': target_is_player,
            'target_is_monster': target_is_monster,
            'attacker_uuid': attacker_uuid,
            'attacker_is_self': attacker_is_self,
            'skill_id': _varint_to_int32(skill_id) if skill_id else 0,
            'damage': int(damage_amount),
            'hp_lessen': int(hp_lessen),
            'shield_lessen': int(shield_lessen),
            'damage_type': int(damage_type),
            'is_heal': is_heal,
            'is_immune': is_immune,
            'is_absorbed': is_absorbed,
            'is_crit': bool(type_flag & 1),
            'is_dead': is_dead,
            'element': int(element),
            'timestamp': time.time(),
        }
        self.stats['damage_events'] += 1
        if self._on_damage:
            try:
                self._on_damage(event)
            except Exception as e:
                logger.debug(f'[Parser] damage callback error: {e}')


    # AttrCollection parsing — Monster


    def _process_monster_attr_collection(self, uuid: int, data: bytes):
        """Decode AttrCollection from a monster delta and update MonsterData."""
        ac = _decode_fields(data)
        attrs_list = ac.get(2, [])
        if not attrs_list:
            return

        monster = self._get_monster(uuid)
        changed = False

        for attr_raw in attrs_list:
            if not isinstance(attr_raw, bytes):
                continue
            af = _decode_fields(attr_raw)
            attr_id = af.get(1, [0])[0]
            raw_data = af.get(2, [None])[0]
            if not isinstance(raw_data, bytes) or not attr_id:
                continue
            int_value = _decode_int32_from_raw(raw_data)

            if attr_id == AttrType.NAME:
                name = _decode_string_from_raw(raw_data)
                if name and name != monster.name:
                    monster.name = name
                    changed = True
                    logger.info(f'[Parser] Monster NAME={name!r} uuid={uuid}')
            elif attr_id == AttrType.ID:
                tid = int_value
                if tid > 0 and tid != monster.template_id:
                    monster.template_id = tid
                    changed = True
            elif attr_id == AttrType.HP:
                hp = int_value
                if hp >= 0 and hp != monster.hp:
                    monster.hp = hp
                    if hp == 0 and monster.max_hp > 0:
                        monster.is_dead = True
                    changed = True
            elif attr_id == AttrType.MAX_HP:
                mhp = int_value
                if mhp > 0 and mhp != monster.max_hp:
                    monster.max_hp = mhp
                    changed = True
                    # Save to template cache for future scene-change recovery
                    if monster.template_id > 0:
                        self._monster_hp_cache[monster.template_id] = mhp
            elif attr_id == AttrType.MONSTER_SEASON_LEVEL:
                # AttrMonsterSeasonLevel (462) — monster's season level
                if int_value > 0 and int_value != monster.season_level:
                    monster.season_level = int_value
                    changed = True
                    logger.info(f'[Parser] Monster SEASON_LEVEL={int_value} uuid={uuid}')
            elif attr_id == AttrType.BREAKING_STAGE:
                if int_value != monster.breaking_stage:
                    monster.breaking_stage = int_value
                    changed = True
                    logger.info(f'[Parser] Monster BREAKING_STAGE={int_value} uuid={uuid}')
            elif attr_id == AttrType.EXTINCTION:
                if int_value != monster.extinction:
                    monster.extinction = int_value
                    changed = True
                    logger.info(f'[Parser] Monster EXTINCTION={int_value} max={monster.max_extinction} uuid={uuid}')
            elif attr_id == AttrType.MAX_EXTINCTION:
                if int_value > 0 and int_value != monster.max_extinction:
                    monster.max_extinction = int_value
                    changed = True
                    logger.info(f'[Parser] Monster MAX_EXTINCTION={int_value} uuid={uuid}')
            elif attr_id == AttrType.STUNNED:
                if int_value != monster.stunned:
                    monster.stunned = int_value
                    changed = True
                    logger.info(f'[Parser] Monster STUNNED={int_value} max={monster.max_stunned} uuid={uuid}')
            elif attr_id == AttrType.MAX_STUNNED:
                if int_value > 0 and int_value != monster.max_stunned:
                    monster.max_stunned = int_value
                    changed = True
                    logger.info(f'[Parser] Monster MAX_STUNNED={int_value} uuid={uuid}')
            elif attr_id == AttrType.IN_OVERDRIVE:
                flag = bool(int_value)
                if flag != monster.in_overdrive:
                    monster.in_overdrive = flag
                    changed = True
                    logger.info(f'[Parser] Monster IN_OVERDRIVE={flag} uuid={uuid}')
            elif attr_id == AttrType.IS_LOCK_STUNNED:
                flag = bool(int_value)
                if flag != monster.is_lock_stunned:
                    monster.is_lock_stunned = flag
                    changed = True
            elif attr_id == AttrType.STOP_BREAKING_TICKING:
                flag = bool(int_value)
                if flag != monster.stop_breaking_ticking:
                    monster.stop_breaking_ticking = flag
                    changed = True
            elif attr_id == AttrType.SHIELD_LIST:
                # AttrShieldList = repeated ShieldInfo {uuid=1, shield_type=2, value=3, initial_value=4, max_value=5}
                self._decode_shield_list(monster, raw_data)
                changed = True
            # ── Extended monster attrs ──
            elif attr_id == AttrType.STATE:
                monster.state = int_value
                changed = True
            elif attr_id == AttrType.DEAD_TYPE:
                monster.dead_type = int_value
                changed = True
            elif attr_id == AttrType.DEAD_TIME:
                monster.dead_time = int_value
                changed = True
            elif attr_id == AttrType.FIRST_ATTACK:
                monster.first_attack = bool(int_value)
                changed = True
            elif attr_id == AttrType.HATED_CHAR_ID:
                monster.hated_char_id = int_value
                changed = True
                logger.debug(f'[Parser] Monster aggro target uid={int_value} uuid={uuid}')
            elif attr_id == AttrType.HATED_CHAR_NAME:
                name = _decode_string_from_raw(raw_data)
                if name:
                    monster.hated_char_name = name
                    changed = True
            elif attr_id in AttrType._MONSTER_EXTENDED_IDS:
                # Remaining extended monster attrs — logged
                logger.debug(f'[Parser] Monster extended attr 0x{attr_id:X}={int_value} uuid={uuid}')
            elif attr_id in (AttrType.HATED_CHAR_JOB, AttrType.HATED_CHAR_LIST):
                # Aggro related — logged
                logger.debug(f'[Parser] Monster aggro attr 0x{attr_id:X}={int_value} uuid={uuid}')
            else:
                # Log unknown monster attrs at debug level for future analysis
                logger.debug(f'[Parser] Unknown monster AttrType 0x{attr_id:X} '
                             f'int={int_value} len={len(raw_data)} uuid={uuid}')

        if changed:
            # If monster has HP data but still no max_hp, try to recover from cache
            if monster.max_hp == 0 and monster.hp > 0 and monster.template_id > 0:
                cached_max = self._monster_hp_cache.get(monster.template_id)
                if cached_max and cached_max > 0:
                    monster.max_hp = cached_max
                    logger.info(
                        f'[Parser] Monster max_hp recovered from cache: '
                        f'tid={monster.template_id} max_hp={cached_max} uuid={uuid}'
                    )
            monster.last_update = time.time()
            self._notify_monster(monster)
            # Log break-related attrs to packet_debug for diagnosis
            if (monster.max_extinction > 0 or monster.max_stunned > 0
                    or monster.breaking_stage >= 0 or monster.shield_active):
                _append_packet_debug('monster_break', {
                    'uuid': uuid,
                    'uid': monster.uid,
                    'name': monster.name,
                    'breaking_stage': monster.breaking_stage,
                    'extinction': monster.extinction,
                    'max_extinction': monster.max_extinction,
                    'stunned': monster.stunned,
                    'max_stunned': monster.max_stunned,
                    'in_overdrive': monster.in_overdrive,
                    'stop_ticking': monster.stop_breaking_ticking,
                    'shield_active': monster.shield_active,
                    'shield_total': monster.shield_total,
                    'shield_max': monster.shield_max_total,
                    'hp': monster.hp,
                    'max_hp': monster.max_hp,
                })

    def _decode_shield_list(self, monster: MonsterData, raw_data: bytes):
        """Decode AttrShieldList (60050) — repeated ShieldInfo messages."""
        # The raw_data for AttrShieldList is a protobuf with repeated ShieldInfo
        # ShieldInfo: uuid=1(int32), shield_type=2(int32), value=3(int64),
        #             initial_value=4(int64), max_value=5(int64)
        try:
            shields = _decode_fields(raw_data)
            total_value = 0
            total_max = 0
            # ShieldList can be a single message or we treat raw_data as repeated
            # Try reading as a container of repeated shield entries
            for shield_raw in shields.get(1, []):
                if isinstance(shield_raw, bytes):
                    sf = _decode_fields(shield_raw)
                    value = sf.get(3, [0])[0]
                    max_val = sf.get(5, [0])[0]
                    total_value += max(0, int(value))
                    total_max += max(0, int(max_val))
                elif isinstance(shield_raw, int):
                    # Single shield inline — field 3 = value
                    value = shields.get(3, [0])[0]
                    max_val = shields.get(5, [0])[0]
                    total_value = max(0, int(value))
                    total_max = max(0, int(max_val))
                    break

            monster.shield_total = total_value
            monster.shield_max_total = total_max
            monster.shield_active = total_value > 0
            logger.debug(f'[Parser] Monster shield: {total_value}/{total_max} uuid={monster.uuid}')
        except Exception as e:
            logger.debug(f'[Parser] shield list decode error: {e}')


    # BuffEffectSync parsing


    def _process_buff_effect_sync(self, host_uuid: int, data: bytes):
        """Decode BuffEffectSync (AoiSyncDelta field 11) for boss events.

        BuffEffectSync { int64 Uuid = 1; repeated BuffEffect BuffEffects = 2; }
        BuffEffect { EBuffEventType Type = 1; int32 BuffUuid = 2;
                     int64 HostUuid = 3; int64 TriggerTime = 4; ... }
        """
        try:
            sync = _decode_fields(data)
            for buff_raw in sync.get(2, []):
                if not isinstance(buff_raw, bytes):
                    continue
                bf = _decode_fields(buff_raw)
                event_type = bf.get(1, [0])[0]
                if event_type not in _BOSS_BUFF_EVENTS:
                    continue
                buff_uuid = bf.get(2, [0])[0]
                buff_host = bf.get(3, [0])[0]
                if isinstance(buff_host, int) and buff_host != 0:
                    buff_host = _varint_to_int64(buff_host)
                else:
                    buff_host = host_uuid

                logger.info(f'[Parser] BuffEvent type={event_type} buff={buff_uuid} '
                            f'host={buff_host} target_uuid={host_uuid}')

                # Update monster state based on event type
                monster = self._monsters.get(host_uuid)
                if monster:
                    if event_type == BuffEventType.ENTER_BREAKING:
                        logger.info(f'[Parser] Monster ENTER_BREAKING uuid={host_uuid}')
                        monster.breaking_stage = 0   # EBreakingStage.Breaking
                        monster.extinction = 0        # bar depleted — force 0%
                        monster.last_update = time.time()
                        self._notify_monster(monster)
                    elif event_type == BuffEventType.SHIELD_BROKEN:
                        monster.shield_active = False
                        monster.shield_total = 0
                        monster.last_update = time.time()
                        self._notify_monster(monster)
                    elif event_type == BuffEventType.HOST_DEATH:
                        monster.is_dead = True
                        monster.hp = 0
                        monster.last_update = time.time()
                        self._notify_monster(monster)

                # Fire boss event callback for all matching events
                self._notify_boss_event(event_type, host_uuid, buff_uuid)
        except Exception as e:
            logger.debug(f'[Parser] BuffEffectSync decode error: {e}')


    # TempAttrCollection parsing — Player CD buff modifiers


    def _process_temp_attr_collection(self, uid: int, data: bytes):
        """Process TempAttrCollection for CD-related buff modifiers.

        TempAttrCollection { repeated TempAttr Attrs = 1; }
        TempAttr { int32 Id = 1; int32 Value = 2; }

        Relevant TempAttr types (from resonance-logs-cn skill_cd_monitor.rs):
          100 = percent CD reduction (万分比, /10000) — cumulative across buffs
          101 = flat CD reduction (ms) — cumulative
          103 = CD acceleration (万分比, /10000) — cumulative
        """
        try:
            tac = _decode_fields(data)
            attrs_list = tac.get(1, [])
            if not attrs_list:
                return

            player = self._get_player(uid)
            # TempAttrs are replacement-style: recompute accumulated values from the full list
            cd_pct = 0
            cd_fixed = 0
            cd_accel = 0

            for attr_raw in attrs_list:
                if not isinstance(attr_raw, bytes):
                    continue
                af = _decode_fields(attr_raw)
                attr_id = af.get(1, [0])[0]
                attr_val = af.get(2, [0])[0]
                if isinstance(attr_val, int) and attr_val > 0x7FFFFFFF:
                    attr_val -= 0x100000000  # signed int32

                if attr_id == 100:    # Percent CD reduction
                    cd_pct += attr_val
                elif attr_id == 101:  # Flat CD reduction (ms)
                    cd_fixed += attr_val
                elif attr_id == 103:  # CD acceleration
                    cd_accel += attr_val

            changed = False
            if player.temp_attr_cd_pct != cd_pct:
                player.temp_attr_cd_pct = cd_pct
                changed = True
            if player.temp_attr_cd_fixed != cd_fixed:
                player.temp_attr_cd_fixed = cd_fixed
                changed = True
            if player.temp_attr_cd_accel != cd_accel:
                player.temp_attr_cd_accel = cd_accel
                changed = True

            if changed:
                logger.info(
                    f'[Parser] TempAttr CD modifiers: pct={cd_pct} fixed={cd_fixed} '
                    f'accel={cd_accel} uid={uid}'
                )
                _append_packet_debug(
                    'temp_attr_cd',
                    {
                        'uid': uid,
                        'cd_pct': cd_pct,
                        'cd_fixed': cd_fixed,
                        'cd_accel': cd_accel,
                    }
                )
                if uid == self._current_uid:
                    self._notify_self()
        except Exception as e:
            logger.debug(f'[Parser] TempAttrCollection decode error: {e}')


    # AttrCollection parsing — Player


    def _process_attr_collection(self, uid: int, data: bytes):
        ac = _decode_fields(data)
        attrs_list = ac.get(2, [])
        if not attrs_list:
            return

        player = self._get_player(uid)
        changed = False
        stamina_max_candidate = 0
        stamina_ratio_values = []

        for attr_raw in attrs_list:
            if not isinstance(attr_raw, bytes):
                continue
            af = _decode_fields(attr_raw)
            attr_id = af.get(1, [0])[0]
            raw_data = af.get(2, [None])[0]
            if not isinstance(raw_data, bytes) or not attr_id:
                continue
            int_value = _decode_int32_from_raw(raw_data)

            if uid == self._current_uid or self._current_uid == 0:
                _append_packet_debug(
                    'attr_collection',
                    {
                        'uid': uid,
                        'attr_id': attr_id,
                        'raw_hex': raw_data.hex(),
                        'int32': int_value,
                        'float32': _decode_float32_from_raw(raw_data),
                    }
                )

            if attr_id == AttrType.NAME:
                name = _decode_string_from_raw(raw_data)
                if name:
                    player.name = name
                    changed = True
                    logger.info(f'[Parser] AttrCollection NAME={name!r} uid={uid}')
            elif attr_id == AttrType.LEVEL:
                lv = int_value
                logger.info(f'[Parser] AttrCollection LEVEL={lv} raw={raw_data.hex()} uid={uid}')
                if lv > 0:
                    player.level = lv
                    changed = True
            elif attr_id == AttrType.RANK_LEVEL:
                rl = int_value
                logger.info(f'[Parser] AttrCollection RANK_LEVEL={rl} raw={raw_data.hex()} uid={uid}')
                if rl >= 0:
                    player.rank_level = rl
                    changed = True
            elif attr_id == AttrType.FIGHT_POINT:
                fp = int_value
                if fp > 0:
                    player.fight_point = fp
                    changed = True
            elif attr_id == AttrType.HP:
                hp = int_value
                # Ignore transient HP=0 attr updates to avoid false death states.
                # A real zero is accepted from the full sync path instead.
                if hp > 0 or player.max_hp == 0:
                    player.hp = hp
                    changed = True
                else:
                    logger.debug(f'[Parser] AttrCollection ignored HP=0 (max_hp={player.max_hp})')
            elif attr_id == AttrType.MAX_HP:
                mhp = int_value
                if mhp > 0:
                    player.max_hp = mhp
                    changed = True
            elif attr_id == AttrType.PROFESSION_ID:
                pid = int_value
                if pid > 0:
                    player.profession_id = pid
                    player.profession = PROFESSION_NAMES.get(pid, '')
                    changed = True
                    if self._apply_cached_profession_slots(player):
                        changed = True
            elif attr_id == AttrType.ENERGY_FLAG:
                # This is a flag field, not the actual stamina value.
                ef_i = int_value
                logger.debug(f'[Parser] AttrCollection EnergyFlag={ef_i} (flag only)')
            elif attr_id == AttrType.SEASON_LEVEL:
                # AttrSeasonLevel (10070) — server-calculated season level
                sl = int_value
                if 0 < sl <= 60:
                    if _set_level_extra_candidate(player, 'season_attr', sl):
                        changed = True
                    logger.info(
                        f'[Parser] AttrCollection AttrSeasonLevel={sl} uid={uid}'
                    )
            elif attr_id == AttrType.SEASON_LV:
                # AttrSeasonLv (196) — per DPS project, treated same as AttrSeasonLevel
                sl = int_value
                if 0 < sl <= 60:
                    if _set_level_extra_candidate(player, 'season_attr_lv', sl):
                        changed = True
                    logger.info(
                        f'[Parser] AttrCollection AttrSeasonLv={sl} uid={uid}'
                    )
            elif attr_id in (AttrType.CRI, AttrType.LUCKY, AttrType.ELEMENT_FLAG,
                             AttrType.REDUCTION_LEVEL, AttrType.ID):
                pass
            elif attr_id == AttrType.COMBAT_STATE:
                # AttrCombatState (104) — 0=out of combat, 1=in combat
                flag = bool(int_value)
                if flag != player.in_combat:
                    player.in_combat = flag
                    changed = True
                    logger.info(f'[Parser] AttrCollection COMBAT_STATE={flag} uid={uid}')
            elif attr_id == AttrType.COMBAT_STATE_TIME:
                # AttrCombatStateTime (114) — transition timestamp in server ms
                if int_value > 0:
                    player.combat_state_time = int_value
                    changed = True
            elif attr_id in AttrType.SEASON_STRENGTH_VARIANTS:
                # AttrSeasonStrength (11440-11445) — 梦境强度 (any variant)
                if int_value > 0 and int_value > player.season_strength:
                    player.season_strength = int_value
                    changed = True
                    logger.info(f'[Parser] AttrCollection SeasonStrength={int_value} (0x{attr_id:X}) uid={uid}')
            elif attr_id in AttrType.MAX_HP_VARIANTS:
                # MaxHp sub-components (Base/Pct/Add/Rate/WithShield).
                # ONLY 11321 (base max HP) is close to the real max_hp.
                # 11323-11325 are HP rate/increment components with small values
                # (e.g. 345, 1800) that would corrupt player.max_hp → HP display jumps.
                # Log for debugging but do NOT set max_hp — only AttrType.MAX_HP (11320) does that.
                if int_value > 0:
                    logger.debug(
                        f'[Parser] AttrCollection MaxHP_Variant 0x{attr_id:X}={int_value} '
                        f'(current max_hp={player.max_hp}) uid={uid} — NOT applied'
                    )
            elif attr_id in (AttrType.SKILL_CD, AttrType.SKILL_CD_TOTAL):
                # Flat CD reduction in ms (from equipment/passives)
                # Prefer Total (11751) — server-computed sum of all contributions
                if int_value >= 0:
                    player.attr_skill_cd = int_value
                    changed = True
                    logger.info(f'[Parser] AttrCollection AttrSkillCD={int_value} (0x{attr_id:X}) uid={uid}')
            elif attr_id in (AttrType.SKILL_CD_PCT, AttrType.SKILL_CD_PCT_TOTAL):
                # Percent CD reduction (万分比, /10000)
                if int_value >= 0:
                    player.attr_skill_cd_pct = int_value
                    changed = True
                    logger.info(f'[Parser] AttrCollection AttrSkillCDPCT={int_value} (0x{attr_id:X}) uid={uid}')
            elif attr_id in (AttrType.CD_ACCELERATE_PCT, AttrType.CD_ACCELERATE_PCT_TOTAL):
                # CD acceleration percent (万分比, /10000) — includes passive bonuses
                if int_value >= 0:
                    player.attr_cd_accelerate_pct = int_value
                    changed = True
                    logger.info(f'[Parser] AttrCollection AttrCdAcceleratePct={int_value} (0x{attr_id:X}) uid={uid}')
            elif attr_id in (AttrType.FIGHT_RES_CD_SPEED_PCT, AttrType.FIGHT_RES_CD_SPEED_PCT_TOTAL):
                # FightResCdSpeedPct — CD speed/duration modifier (万分比, /10000)
                if int_value > 0:
                    player.attr_fight_res_cd_speed = int_value
                    changed = True
                    logger.info(f'[Parser] AttrCollection FightResCdSpeedPct={int_value} (0x{attr_id:X}) uid={uid}')
            # ── Extended combat stats (from EAttrType enum) ──
            elif attr_id in (AttrType.ATTACK, AttrType.ATTACK_TOTAL):
                if int_value > 0:
                    player.attack = int_value
                    changed = True
            elif attr_id in (AttrType.M_ATTACK, AttrType.M_ATTACK_TOTAL):
                if int_value > 0:
                    player.magic_attack = int_value
                    changed = True
            elif attr_id in (AttrType.DEFENSE, AttrType.DEFENSE_TOTAL):
                if int_value > 0:
                    player.defense = int_value
                    changed = True
            elif attr_id in (AttrType.M_DEFENSE, AttrType.M_DEFENSE_TOTAL):
                if int_value > 0:
                    player.magic_defense = int_value
                    changed = True
            elif attr_id in (AttrType.CRIT_RATE, AttrType.CRIT_RATE_TOTAL):
                if int_value >= 0:
                    player.crit_rate = int_value
                    changed = True
            elif attr_id in (AttrType.CRIT_DAMAGE, AttrType.CRIT_DAMAGE_TOTAL):
                if int_value >= 0:
                    player.crit_damage = int_value
                    changed = True
            elif attr_id in (AttrType.ATTACK_SPEED_PCT, AttrType.ATTACK_SPEED_PCT_TOTAL):
                if int_value >= 0:
                    player.attack_speed_pct = int_value
                    changed = True
            elif attr_id in (AttrType.CAST_SPEED_PCT, AttrType.CAST_SPEED_PCT_TOTAL):
                if int_value >= 0:
                    player.cast_speed_pct = int_value
                    changed = True
            elif attr_id in (AttrType.CHARGE_SPEED_PCT, AttrType.CHARGE_SPEED_PCT_TOTAL):
                if int_value >= 0:
                    player.charge_speed_pct = int_value
                    changed = True
            elif attr_id in (AttrType.HEAL_POWER, AttrType.HEAL_POWER_TOTAL):
                if int_value >= 0:
                    player.heal_power = int_value
                    changed = True
            elif attr_id in (AttrType.DAM_INC, AttrType.DAM_INC_TOTAL):
                if int_value >= 0:
                    player.dam_inc = int_value
                    changed = True
            elif attr_id in (AttrType.M_DAM_INC, AttrType.M_DAM_INC_TOTAL):
                if int_value >= 0:
                    player.mdam_inc = int_value
                    changed = True
            elif attr_id in (AttrType.BOSS_DAM_INC, AttrType.BOSS_DAM_INC_TOTAL):
                if int_value >= 0:
                    player.boss_dam_inc = int_value
                    changed = True
            elif attr_id in AttrType._BASE_STAT_IDS:
                # Base stats (STR/INT/DEX/VIT/Haste/Mastery/Versatility) — logged
                logger.debug(f'[Parser] AttrCollection BaseStat 0x{attr_id:X}={int_value} uid={uid}')
            elif attr_id in AttrType._DAMAGE_MOD_IDS:
                # Damage resistance modifiers — logged
                logger.debug(f'[Parser] AttrCollection DamageMod 0x{attr_id:X}={int_value} uid={uid}')
            elif attr_id in (AttrType.ORIGIN_ENERGY, AttrType.MAX_ORIGIN_ENERGY):
                # Origin energy as attr (separate from UserFightAttr stamina)
                logger.debug(f'[Parser] AttrCollection OriginEnergy 0x{attr_id:X}={int_value} uid={uid}')
            else:
                if attr_id == AttrType.STA_MAX_FALLBACK and _is_sane_attr_stamina_max(int_value):
                    stamina_max_candidate = max(stamina_max_candidate, int_value)
                elif attr_id in AttrType.STA_RATIO_SET and 0 <= int_value <= 1000:
                    stamina_ratio_values.append(int_value)

                logger.debug(f'[Parser] Unknown AttrType 0x{attr_id:X}, len={len(raw_data)}, uid={uid}')

        if _is_sane_attr_stamina_max(stamina_max_candidate):
            if player.energy_limit != stamina_max_candidate or player.extra_energy_limit != 0:
                player.energy_limit = stamina_max_candidate
                player.extra_energy_limit = 0
                changed = True
                logger.info(f'[Parser] AttrCollection STA max fallback={stamina_max_candidate} uid={uid}')

        if stamina_ratio_values:
            ratio_counts = {}
            for value in stamina_ratio_values:
                ratio_counts[value] = ratio_counts.get(value, 0) + 1
            picked_ratio, picked_count = max(ratio_counts.items(), key=lambda item: (item[1], item[0]))
            if picked_count >= 2:
                ratio_value = max(0.0, min(1.0, picked_ratio / 1000.0))
                if (
                    abs(float(getattr(player, 'stamina_ratio', -1.0) or -1.0) - ratio_value) > 0.001 or
                    (time.time() - float(getattr(player, 'stamina_ratio_observed_at', 0.0) or 0.0)) > 0.8
                ):
                    player.stamina_ratio = ratio_value
                    player.stamina_ratio_observed_at = time.time()
                    changed = True
                logger.info(
                    f'[Parser] AttrCollection stamina_ratio={picked_ratio}/1000 '
                    f'(samples={picked_count}) uid={uid}'
                )

        if _refresh_stamina_resource(player):
            changed = True

        if changed and uid == self._current_uid:
            self._notify_self()


    #  Zstd


    def _decompress(self, data: bytes) -> Optional[bytes]:
        """Zstd decompression helper matching the Node.js reference behavior."""
        try:
            if self._zstd is None:
                self._zstd = _ensure_zstd()

            with self._zstd.stream_reader(data) as reader:
                chunks = []
                while True:
                    chunk = reader.read(65536)
                    if not chunk:
                        break
                    chunks.append(chunk)
                return b''.join(chunks)
        except Exception as e:
            self.stats['zstd_failures'] += 1
            if self.stats['zstd_failures'] <= 3:
                print(f'[Parser] zstd解压失败 ({self.stats["zstd_failures"]}): {e}', flush=True)
            logger.debug(f'[Parser] zstd decompress failed: {e}')
            _append_packet_debug('zstd_failure', {
                'error': str(e),
                'input_size': len(data or b''),
            })
            return None
