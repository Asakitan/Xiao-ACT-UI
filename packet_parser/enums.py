# -*- coding: utf-8 -*-
"""Enum / constant definitions extracted from packet_parser (physical split).

Self-contained: depends on no other packet_parser submodule.
"""

from typing import Dict


_RESET_IGNORE_TARGETS = frozenset({
    1301104, 1301105, 1301106, 6521002, 6521003, 1083, 1302101,
})


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
    ENTER_SCENE = 0x03
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
    SYNC_DUNGEON_DIRTY_DATA = 0x18
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
    SYNC_CLIENT_USE_SKILL_WORLD = 0x43
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
    SKILL_ID = 0x64              # AttrSkillId — current/triggered skill id
    # ── Player info attrs ──
    COMBAT_STATE = 104           # AttrCombatState  — 0=out, 1=in combat
    COMBAT_STATE_TIME = 114      # AttrCombatStateTime — transition timestamp
    SEASON_LV = 196              # AttrSeasonLv (season star rank — display extra level)
    PROFESSION_ID = 0xDC         # 220 = AttrProfessionId
    SCENE_BASIC_ID = 0x155       # 341 = AttrSceneBasicId (EnterScene)
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
    FIGHT_RESOURCES = 50002              # AttrFightResources — packed fight resource values
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

WIPE_BUFF_BASE_ID = 510072

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
    MONSTER = 1    # Monster entity
    NPC = 2        # NPC
    SCENE_OBJECT = 3
    CHAR = 10      # Player character
    DUMMY = 11
    DROP = 12
    FIELD = 14
    TRAP = 15
    COLLECT = 16   # Collectible
    STATIC_OBJECT = 18

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
    # ── Extended fields (beyond proto definition) ──
    DEEP_SLEEP_LEVEL = 102  # 深眠心相仪等级 (Season Resonance Level shown as +XX)

# Reverse mapping: field_number → field_name (for logging)
CHAR_FIELD_NAMES = {
    v: k for k, v in vars(CharField).items()
    if isinstance(v, int) and not k.startswith('_')
}

# CharSerialize fields that have dedicated handlers (skip in generic loop)
_HANDLED_CHAR_FIELDS = frozenset({
    CharField.CHAR_ID, CharField.CHAR_BASE, CharField.SCENE_DATA,
    CharField.BUFF_DB_INFO, CharField.EQUIP_LIST, CharField.ENERGY_ITEM,
    CharField.DUNGEON_LIST, CharField.USER_FIGHT_ATTR, CharField.ROLE_LEVEL,
    CharField.RESONANCE,
    CharField.SEASON_CENTER, CharField.SEASON_MEDAL_INFO,
    CharField.SLOTS, CharField.MONSTER_HUNT_INFO,
    CharField.PROFESSION_LIST, CharField.BATTLE_PASS_DATA,
    CharField.DEEP_SLEEP_LEVEL,
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
