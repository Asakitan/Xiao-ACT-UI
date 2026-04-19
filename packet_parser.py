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
import importlib
import importlib.util
import os
import json
import sys
import time
from typing import Optional, Callable, Dict, Any

logger = logging.getLogger('sao_auto.parser')
_PACKET_DEBUG_ENABLED = False  # Enable to log raw packet snapshots for field confirmation
_PENDING_SELF_NOTIFY_LIMIT = 16

# Lazy import for protobuf JSON conversion (used in full sync dump)
_MessageToDict = None





_zstd = None
_pb = None
_pb_last_error = ''


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


def _candidate_pb2_paths() -> list[str]:
    """Return likely local star_resonance_pb2.py locations.

    onedir builds lift proto/ to the exe top-level, while dev keeps it next to
    the source tree. Resolve both layouts explicitly instead of relying on
    `from proto import ...`, which can bind to an unrelated third-party package.
    """
    candidates = []
    seen = set()

    def _add(base_dir: Optional[str]):
        if not base_dir:
            return
        try:
            base_dir = os.path.abspath(base_dir)
        except Exception:
            return
        pb2_path = os.path.join(base_dir, 'proto', 'star_resonance_pb2.py')
        if pb2_path in seen:
            return
        seen.add(pb2_path)
        if os.path.isfile(pb2_path):
            candidates.append(pb2_path)

    current_dir = os.path.dirname(os.path.abspath(__file__))
    _add(current_dir)
    _add(os.path.dirname(current_dir))
    if getattr(sys, 'frozen', False):
        _add(os.path.dirname(sys.executable))
        _add(getattr(sys, '_MEIPASS', None))
    return candidates


def _import_local_pb_via_proto_package(pb2_path: str):
    """Import the local protobuf module through the canonical proto package name."""
    package_root = os.path.dirname(os.path.dirname(pb2_path))
    expected_init = os.path.join(package_root, 'proto', '__init__.py')
    expected_pb2 = os.path.abspath(pb2_path)
    previous_proto = sys.modules.get('proto')
    previous_pb = sys.modules.get('proto.star_resonance_pb2')
    added_path = False

    def _restore_previous():
        if previous_proto is not None:
            sys.modules['proto'] = previous_proto
        else:
            sys.modules.pop('proto', None)
        if previous_pb is not None:
            sys.modules['proto.star_resonance_pb2'] = previous_pb
        else:
            sys.modules.pop('proto.star_resonance_pb2', None)

    try:
        if package_root not in sys.path:
            sys.path.insert(0, package_root)
            added_path = True

        proto_mod = sys.modules.get('proto')
        if proto_mod is not None:
            proto_file = os.path.abspath(getattr(proto_mod, '__file__', '') or '')
            proto_paths = [os.path.abspath(p) for p in list(getattr(proto_mod, '__path__', []) or [])]
            expected_dir = os.path.abspath(os.path.dirname(pb2_path))
            is_local_proto = proto_file == os.path.abspath(expected_init) or expected_dir in proto_paths
            if not is_local_proto:
                sys.modules.pop('proto', None)

        pb_mod = sys.modules.get('proto.star_resonance_pb2')
        if pb_mod is not None:
            loaded_pb2 = os.path.abspath(getattr(pb_mod, '__file__', '') or '')
            if loaded_pb2 != expected_pb2:
                sys.modules.pop('proto.star_resonance_pb2', None)

        module = importlib.import_module('proto.star_resonance_pb2')
        loaded_file = os.path.abspath(getattr(module, '__file__', '') or '')
        if loaded_file != expected_pb2:
            raise ImportError(f'proto.star_resonance_pb2 resolved to unexpected path: {loaded_file}')
        return module
    except Exception:
        _restore_previous()
        if added_path:
            try:
                sys.path.remove(package_root)
            except ValueError:
                pass
        raise


def _load_pb_from_path(pb2_path: str):
    module_name = '_sao_star_resonance_pb2'
    spec = importlib.util.spec_from_file_location(module_name, pb2_path)
    if spec is None or spec.loader is None:
        raise ImportError(f'无法构建 protobuf 模块 spec: {pb2_path}')
    module = importlib.util.module_from_spec(spec)
    previous = sys.modules.get(module_name)
    sys.modules[module_name] = module
    try:
        spec.loader.exec_module(module)
    except Exception:
        if previous is not None:
            sys.modules[module_name] = previous
        else:
            sys.modules.pop(module_name, None)
        raise
    return module


def _ensure_pb():
    """Load compiled protobuf module if available."""
    global _pb, _MessageToDict, _pb_last_error
    if _pb is not None:
        return _pb

    if _MessageToDict is None:
        try:
            from google.protobuf.json_format import MessageToDict
            _MessageToDict = MessageToDict
        except ImportError:
            pass

    last_error = None

    for pb2_path in _candidate_pb2_paths():
        try:
            _pb = _import_local_pb_via_proto_package(pb2_path)
            logger.info(f'[Parser] using compiled protobuf via proto package: {pb2_path}')
            _pb_last_error = ''
            return _pb
        except Exception as exc:
            last_error = exc

    try:
        from proto import star_resonance_pb2
        _pb = star_resonance_pb2
        logger.info('[Parser] using compiled protobuf via import')
        _pb_last_error = ''
        return _pb
    except Exception as exc:
        last_error = exc

    for pb2_path in _candidate_pb2_paths():
        try:
            _pb = _load_pb_from_path(pb2_path)
            logger.info(f'[Parser] using compiled protobuf via direct file fallback: {pb2_path}')
            _pb_last_error = ''
            return _pb
        except Exception as exc:
            last_error = exc

    if last_error is not None:
        err_text = str(last_error)
        if err_text != _pb_last_error:
            logger.debug(f'[Parser] protobuf module unavailable: {err_text}')
            _pb_last_error = err_text

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


def _decode_utf8_bytes(raw: Optional[bytes]) -> str:
    if not raw:
        return ''
    try:
        return bytes(raw).decode('utf-8', 'ignore')
    except Exception:
        return ''


def _get_field_int(fields: Dict[int, list], field_num: int, default: int = 0) -> int:
    for value in fields.get(field_num, []) or []:
        if isinstance(value, int):
            return int(value)
    return int(default)


def _get_field_bytes(fields: Dict[int, list], field_num: int) -> Optional[bytes]:
    for value in fields.get(field_num, []) or []:
        if isinstance(value, (bytes, bytearray)):
            return bytes(value)
    return None


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
    """Compose a skill_level_id from a base skill_id and skill level.

    Convention: skill_level_id = skill_id * 100 + level.
    Returns skill_id as-is if level is 0 or negative.
    """
    skill_id = int(skill_id or 0)
    level = int(level or 0)
    if skill_id <= 0:
        return 0
    if level > 0:
        return skill_id * 100 + level
    return skill_id


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
        # Also consider break data present if we have raw values (even without max)
        # or if breaking_stage has been received (>= 0).
        _has_break = (self.max_extinction > 0 or self.max_stunned > 0
                      or self.extinction > 0 or self.stunned > 0
                      or self.breaking_stage >= 0)
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
                 'season_exp', 'season_exp_source',
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
        self.season_exp: int = 0
        self.season_exp_source: str = ''
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
                        values.append({'_hex': v.hex()[:512]})
                else:
                    values.append({'_hex': v.hex()[:512]})
            elif isinstance(v, int):
                values.append(v)
            elif isinstance(v, float):
                values.append(v)
            else:
                values.append(str(v)[:64])
        result[str(k)] = values[0] if len(values) == 1 else values
    return result


def _decode_buff_info_sync_pb(bfs) -> list:
    """Decode BuffInfoSync pb2 object → list of buff dicts.

    BuffInfoSync { int64 Uuid = 1; repeated BuffInfo BuffInfos = 2; }
    BuffInfo { BuffUuid(1), BaseId(2), Level(3), HostUuid(4), TableUuid(5),
               CreateTime(6), FireUuid(7), Layer(8), PartId(9), Count(10),
               Duration(11), FightSourceInfo(12), LogicEffect(13) }
    """
    buffs = []
    try:
        for bi in bfs.BuffInfos:
            base_id = bi.BaseId
            if not base_id:
                continue
            buffs.append({
                'buff_id': base_id,
                'buff_uuid': bi.BuffUuid,
                'begin_time': bi.CreateTime,
                'duration': bi.Duration,
                'layer': bi.Layer,
                'host_uuid': bi.HostUuid,
                'fire_uuid': bi.FireUuid,
                'level': bi.Level,
                'count': bi.Count,
                'cur_layer': bi.Layer,
            })
    except Exception as e:
        logger.debug(f'[Parser] _decode_buff_info_sync_pb error: {e}')
    return buffs



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


# ── Smart dirty-stream offset detection ──
# Base level is capped at 60 in the current game version; use it as an anchor
# to auto-detect correct parsing offsets when the binary format shifts.
_KNOWN_BASE_LEVEL = 60
_DIRTY_IDENT = 0xFFFFFFFE
_SMART_SCAN_MAX = 64  # max leading bytes to scan


def _smart_find_dirty_start(data: bytes) -> Optional[int]:
    """Scan *data* for the ``0xFFFFFFFE`` identifier to recover the correct
    starting offset when the dirty stream has unexpected leading bytes.

    For ``RoleLevel`` (field 22) candidates the scan is validated against
    the known base-level anchor (60) to avoid false positives.

    Returns the byte offset of the first valid identifier, or ``None``.
    """
    limit = min(len(data) - 12, _SMART_SCAN_MAX)
    best: Optional[int] = None
    for off in range(1, limit):
        if struct.unpack_from('<I', data, off)[0] != _DIRTY_IDENT:
            continue
        fi_pos = off + 8
        if fi_pos + 4 > len(data):
            continue
        fi = struct.unpack_from('<I', data, fi_pos)[0]
        if fi not in CHAR_FIELD_NAMES:
            continue
        # Extra validation for RoleLevel: nested value must equal 60
        if fi == 22:
            nested = fi_pos + 4
            if nested + 12 <= len(data):
                ident2 = struct.unpack_from('<I', data, nested)[0]
                if ident2 == _DIRTY_IDENT:
                    sf_pos = nested + 8
                    sf = struct.unpack_from('<I', data, sf_pos)[0]
                    val_pos = sf_pos + 4
                    if sf == 1 and val_pos + 4 <= len(data):
                        val = struct.unpack_from('<I', data, val_pos)[0]
                        if val == _KNOWN_BASE_LEVEL:
                            return off  # confirmed via anchor
                        continue  # wrong value → skip this candidate
        if best is None:
            best = off
    return best


def _is_sane_attr_stamina_max(value: int) -> bool:
    # Current observed self STA caps stay around 1200. Values like 1350/1500
    # and the larger 2100/3100 spikes are not stable enough to trust.
    return 0 < value <= 1300


_LEVEL_EXTRA_SOURCE_PRIORITY = {
    'deep_sleep': 200,    # 深眠心相仪等级 (field 102) — the actual (+XX) display level, highest priority
    'season_attr': 100,   # AttrSeasonLevel (10070) — server-authoritative total season level (preferred, covers experience tasks)
    'season_attr_lv': 100, # AttrSeasonLv (196) — same authority per DPS project reference
    'season_medal': 50,   # SeasonMedalInfo CoreHole (subsystem, not display level)
    'monster_hunt': 10,   # MonsterHuntInfo CurLevel from CharSerialize field 56 — may not reflect experience-task upgrades
    'battlepass': 5,
    'battlepass_data': 3,
}

# Sources that are reliable enough to commit on first observation (no 2-hit)
# season_medal comes from SyncContainerData field 52 (CoreHoleInfo.HoleLevel)
# — the server-authoritative season progression level, fires at login & dirty updates.
_TRUSTED_LEVEL_SOURCES = frozenset({'deep_sleep', 'season_attr', 'season_attr_lv', 'season_medal'})


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


def _normalize_season_medal_level(raw_level: int) -> int:
    """Normalize season medal level from raw server value.
    
    The server may send various representations of the medal level.
    This function converts to a canonical form.
    """
    return max(0, int(raw_level or 0))


def _set_level_extra_candidate(player: PlayerData, source: str, value: int) -> bool:
    source = str(source or '')
    value = max(0, int(value or 0))
    if value <= 0 or not source:
        return False

    current_priority = _source_priority(getattr(player, 'level_extra_source', ''))
    candidate_priority = _source_priority(source)

    # Block lower-priority sources from overriding a higher-priority confirmed value.
    # Exception: trusted sources may INCREASE level_extra (level-up arrives via
    # AttrSeasonLevel attr before dirty_update field 102 deep_sleep).
    if (
        getattr(player, 'level_extra', 0) > 0 and
        value != player.level_extra and
        current_priority > candidate_priority
    ):
        if not (source in _TRUSTED_LEVEL_SOURCES and value > player.level_extra):
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


def _set_season_exp_candidate(player: PlayerData, source: str, value: int) -> bool:
    source = str(source or '')
    if not source:
        return False

    value = max(0, int(value or 0))
    current_value = int(getattr(player, 'season_exp', 0) or 0)
    current_source = str(getattr(player, 'season_exp_source', '') or '')
    bound_source = str(getattr(player, 'level_extra_source', '') or '')
    current_priority = _source_priority(current_source)
    candidate_priority = _source_priority(source)
    bound_priority = _source_priority(bound_source)

    if current_value == value and current_source == source:
        return False

    if source == bound_source:
        player.season_exp = value
        player.season_exp_source = source
        return True

    if current_source and current_priority > candidate_priority:
        return False

    if bound_source and bound_priority > candidate_priority:
        return False

    player.season_exp = value
    player.season_exp_source = source
    return True


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
        # Team member cache: uid -> {uid, name, profession, profession_id,
        #                            fight_point, level, joined_at}
        # Populated from CharTeam (SyncContainerData / DirtyData).
        # Persists across scene changes so commander panel can always show info.
        self._team_members: Dict[int, Dict[str, Any]] = {}
        self._team_id: int = 0
        self._team_leader_uid: int = 0
        # Cache template_id → max_hp from observed monsters.
        # Survives scene resets so if a monster re-appears without MAX_HP in
        # its AttrCollection (only incremental deltas), we can restore max_hp
        # from this cache.  Limited to 1024 entries.
        self._monster_hp_cache: Dict[int, int] = {}  # template_id -> max_hp
        self._profession_skill_cache: Dict[int, Dict[int, int]] = {}  # profession_id -> slot map
        self._pending_self_notifies: list[tuple[int, bytes, str]] = []
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
            self._prepopulate_from_cache(self._current_uid)
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

    def _remember_pending_self_notify(self, method_id: int, payload: bytes, reason: str):
        if not payload:
            return
        self._pending_self_notifies.append((int(method_id), bytes(payload), str(reason or '')))
        if len(self._pending_self_notifies) > _PENDING_SELF_NOTIFY_LIMIT:
            del self._pending_self_notifies[:-_PENDING_SELF_NOTIFY_LIMIT]
        pending_count = len(self._pending_self_notifies)
        if pending_count <= 3 or pending_count == _PENDING_SELF_NOTIFY_LIMIT:
            logger.info(
                f'[Parser] buffered early self notify method=0x{method_id:X} '
                f'count={pending_count} reason={reason}'
            )

    def _replay_pending_self_notifies(self, source: str):
        if self._current_uid <= 0 or not self._pending_self_notifies:
            return
        pending = list(self._pending_self_notifies)
        self._pending_self_notifies.clear()
        logger.info(
            f'[Parser] replaying {len(pending)} buffered self notifies '
            f'after {source} uid={self._current_uid}'
        )
        for method_id, payload, reason in pending:
            try:
                if method_id == NotifyMethod.SYNC_CONTAINER_DIRTY_DATA:
                    self._on_sync_container_dirty(payload)
                elif method_id == NotifyMethod.SYNC_TO_ME_DELTA_INFO:
                    self._on_sync_to_me_delta(payload)
            except Exception as e:
                logger.warning(
                    f'[Parser] replay buffered notify failed method=0x{method_id:X} '
                    f'source={source} reason={reason}: {e}'
                )

    def _prepopulate_from_cache(self, uid: int):
        """Pre-populate player identity from player_cache.json so that the
        first _notify_self() (from SyncToMeDelta) already carries name/level
        instead of empty strings.  When SyncContainerData eventually arrives
        (login / map change), it will overwrite with fresh data."""
        import json as _json
        cache_path = os.path.join(os.path.dirname(__file__), 'player_cache.json')
        try:
            if not os.path.isfile(cache_path):
                return
            with open(cache_path, 'r', encoding='utf-8') as f:
                cache = _json.load(f)
            entry = cache.get(str(uid))
            if not entry:
                return
            player = self._get_player(uid)
            if entry.get('name'):
                player.name = entry['name']
            if entry.get('level', 0) > 0:
                player.level = entry['level']
            if entry.get('profession'):
                player.profession = entry['profession']
            if entry.get('fight_point', 0) > 0:
                player.fight_point = entry['fight_point']
            print(
                f'[Parser] 从缓存预填充: name={player.name!r} lv={player.level} '
                f'prof={player.profession!r} uid={uid}',
                flush=True,
            )
        except Exception as e:
            logger.debug(f'[Parser] player cache pre-load failed: {e}')

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
        # Server time for expiry check
        _server_now_ms = 0
        _ts_2020 = 1577836800000
        if self._server_time_offset_ms is not None:
            _server_now_ms = int(observed_at_ms + self._server_time_offset_ms)
        for skill_cd in skill_cds or []:
            skill_level_id = int(skill_cd.get('skill_level_id') or 0)
            # duration = total CD length; valid_cd_time = elapsed or progress
            total_ms = int(skill_cd.get('duration') or 0)
            if skill_level_id <= 0 or total_ms <= 0:
                continue
            # Skip CDs that have already expired based on server clock
            begin_time_ms = int(skill_cd.get('begin_time') or 0)
            if _server_now_ms > 0 and begin_time_ms > _ts_2020 and (begin_time_ms + total_ms) < _server_now_ms:
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

        # Expiry check: if begin_time + duration < server_time, CD already expired
        begin_time_ms = int(skill_cd.get('begin_time') or 0)
        if begin_time_ms > 0 and self._server_time_offset_ms is not None:
            server_now_ms = int(time.time() * 1000 + self._server_time_offset_ms)
            _ts_2020 = 1577836800000
            if begin_time_ms > _ts_2020 and (begin_time_ms + total_ms) < server_now_ms:
                # CD already expired — treat as ready
                player.skill_last_use_at[skill_level_id] = time.time()
                if skill_level_id in player.skill_cd_map:
                    del player.skill_cd_map[skill_level_id]
                    return True
                return seen_changed

        observed_at_ms = int(time.time() * 1000)

        # SkillCD proto (from SyncToMeDelta) only has 5 fields and does NOT
        # include ChargeCount, SubCDRatio, SubCDFixed, AccelerateCDRatio.
        # The caller hardcodes these as 0.  When a previous entry exists
        # (from UserFightAttr full-sync SkillCDInfo which *does* have those
        # fields), carry forward the full-sync values so we don't lose
        # charge state and CD-reduction data.
        prev = player.skill_cd_map.get(skill_level_id)
        _carry_charge   = max(0, int((prev or {}).get('charge_count') or 0))
        _carry_sub_r    = max(0, int((prev or {}).get('sub_cd_ratio') or 0))
        _carry_sub_f    = max(0, int((prev or {}).get('sub_cd_fixed') or 0))
        _carry_accel    = max(0, int((prev or {}).get('accelerate_cd_ratio') or 0))

        new_entry = {
            'skill_level_id': skill_level_id,
            'begin_time': max(0, int(skill_cd.get('begin_time') or 0)),
            'duration': max(0, int(skill_cd.get('duration') or 0)),
            'valid_cd_time': max(0, int(skill_cd.get('valid_cd_time') or 0)),
            'skill_cd_type': max(0, int(skill_cd.get('skill_cd_type') or 0)),
            'charge_count': max(0, int(skill_cd.get('charge_count') or 0)) or _carry_charge,
            'sub_cd_ratio': max(0, int(skill_cd.get('sub_cd_ratio') or 0)) or _carry_sub_r,
            'sub_cd_fixed': max(0, int(skill_cd.get('sub_cd_fixed') or 0)) or _carry_sub_f,
            'accelerate_cd_ratio': max(0, int(skill_cd.get('accelerate_cd_ratio') or 0)) or _carry_accel,
            'observed_at_ms': observed_at_ms,
            'source': 'delta',
        }
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
        # ── Team notify ──
        elif method_id == NotifyMethod.NOTIFY_ALL_MEMBER_READY:
            self._on_notify_team_generic('AllMemberReady', msg_payload)
        elif method_id == NotifyMethod.NOTIFY_CAPTAIN_READY:
            self._on_notify_team_generic('CaptainReady', msg_payload)
        elif method_id == NotifyMethod.ENTER_MATCH_RESULT_NTF:
            self._on_notify_team_generic('EnterMatchResult', msg_payload)
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
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.SyncServerTime()
            msg.ParseFromString(data)
        except Exception as e:
            logger.debug(f'[Parser] SyncServerTime pb2 parse error: {e}')
            return
        client_ms = msg.ClientMilliseconds
        server_ms = msg.ServerMilliseconds
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
        """Handle NotifyBuffChange (0x3003 / 12291) — buff replacement notification.

        NotifyBuffChange proto:
            int32 OldBuffId = 1;   // 被替换的buff
            int32 NewBuffId = 2;   // 新buff
        Note: Full buff state is tracked from AoiSyncDelta.BuffInfos and Entity.BuffInfos.
        """
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.NotifyBuffChange()
            msg.ParseFromString(data)
            old_id = msg.OldBuffId
            new_id = msg.NewBuffId
            logger.info(f'[Parser] NotifyBuffChange(pb2): old={old_id} new={new_id}')
            _append_packet_debug('buff_change', {'old_buff_id': old_id, 'new_buff_id': new_id})
        except Exception as e:
            logger.debug(f'[Parser] NotifyBuffChange decode error: {e}')

    def _on_sync_client_use_skill(self, data: bytes):
        """Handle SyncClientUseSkill (0x3002 / 12290) — skill use confirmation.

        SyncClientUseSkill proto:
            int64 SkillTargetUuid = 1;   // 技能目标
            int32 SkillLevelId = 2;      // 技能等级ID
        Note: caster is implied to be the local player ("Client" use skill).
        """
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.SyncClientUseSkill()
            msg.ParseFromString(data)
            target_uuid = msg.SkillTargetUuid
            skill_level_id = msg.SkillLevelId
            logger.info(
                f'[Parser] SyncClientUseSkill(pb2): target={target_uuid} '
                f'skill_level_id={skill_level_id}'
            )
            _append_packet_debug('use_skill', {
                'target_uuid': target_uuid,
                'skill_level_id': skill_level_id,
            })
            # Record skill use timestamp for CD tracking (caster = current player)
            uid = self._current_uid
            if uid and uid in self._players:
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
            int32 SkillUuid = 1;    // 技能会话ID
        """
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.SyncServerSkillEnd()
            msg.ParseFromString(data)
            logger.debug(f'[Parser] SyncServerSkillEnd(pb2): skill_uuid={msg.SkillUuid}')
            _append_packet_debug('skill_end', {'skill_uuid': msg.SkillUuid})
        except Exception as e:
            logger.debug(f'[Parser] SyncServerSkillEnd decode error: {e}')

    def _on_sync_server_skill_stage_end(self, data: bytes):
        """Handle SyncServerSkillStageEnd (0x3004 / 12292).

        SyncServerSkillStageEnd proto:
            ServerSkillStageEnd SkillStageEndInfo = 1;
        ServerSkillStageEnd proto:
            int32 SkillUuid = 1;
            uint32 StageId = 2;
            uint32 NewStageId = 3;
            uint32 ConditionId = 4;
        """
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.SyncServerSkillStageEnd()
            msg.ParseFromString(data)
            info = msg.SkillStageEndInfo
            logger.debug(
                f'[Parser] SyncServerSkillStageEnd(pb2): skill_uuid={info.SkillUuid} '
                f'stage={info.StageId} new_stage={info.NewStageId} cond={info.ConditionId}'
            )
        except Exception as e:
            logger.debug(f'[Parser] SyncServerSkillStageEnd decode error: {e}')

    def _on_qte_begin(self, data: bytes):
        """Handle QteBegin (0x3001 / 12289) — QTE event start.
        Note: QteBegin not in compiled proto — uses _decode_fields.
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
            DungeonSyncData VData = 1;
        DungeonSyncData proto:
            int64 SceneUuid = 1;
            DungeonFlowInfo FlowInfo = 2;
            DungeonSettlement Settlement = 7;
            DungeonSceneInfo DungeonSceneInfo = 21;
            ...
        """
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.SyncDungeonData()
            msg.ParseFromString(data)
            vd = msg.VData
            scene_uuid = vd.SceneUuid
            logger.info(f'[Parser] SyncDungeonData(pb2): scene_uuid={scene_uuid} last={self._last_dungeon_id}')
            _append_packet_debug('dungeon_data', {'scene_uuid': scene_uuid, 'last_dungeon_id': self._last_dungeon_id})
            # Detect dungeon change → reset scene to clear old monsters
            dungeon_id = scene_uuid  # Use scene_uuid as dungeon identity
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
            elif dungeon_id == self._last_dungeon_id and self._last_dungeon_id != 0:
                # Same dungeon retry (刷本): UUID 不变, 需要重置死亡状态
                # 不调用 reset_scene() 以保留 max_hp 缓存,
                # 仅清除 is_dead 让 boss HP 面板可以重新呼出。
                revived = 0
                for m in self._monsters.values():
                    if m.is_dead:
                        m.is_dead = False
                        m.hp = 0  # 等待新 HP 数据
                        m.last_update = time.time()
                        revived += 1
                if revived:
                    print(
                        f'[Parser] ♻ 同副本重开: dungeon={dungeon_id}, '
                        f'重置 {revived} 个死亡单位',
                        flush=True,
                    )
                    logger.info(
                        f'[Parser] Same dungeon restart: reset {revived} dead monsters '
                        f'dungeon_id={dungeon_id}'
                    )
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
            adopted_uid = False
            if uid > 0 and self._current_uid == 0:
                self._current_uid = uid
                adopted_uid = True
                logger.info(f'[Parser] auto-adopt UID from EnterGame: {uid}')
            if adopted_uid:
                self._replay_pending_self_notifies('EnterGame')
        except Exception as e:
            logger.debug(f'[Parser] EnterGame decode error: {e}')

    def _on_notify_revive_user(self, data: bytes):
        """Handle NotifyReviveUser (0x27) — player revived.

        NotifyReviveUser proto:
            int64 VActorUuid = 1;
        """
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.NotifyReviveUser()
            msg.ParseFromString(data)
            uuid = msg.VActorUuid
            logger.info(f'[Parser] NotifyReviveUser(pb2): uuid={uuid}')
            _append_packet_debug('revive_user', {'uuid': uuid})
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

    def _on_notify_team_generic(self, notify_name: str, data: bytes):
        """Handle team-related notify packets (AllMemberReady, CaptainReady, MatchResult).

        These packets may carry team roster data. We log the raw payload and
        attempt to decode it as CharTeam or other team-related messages.
        """
        print(
            f'[Parser] 收到队伍通知: {notify_name} (len={len(data)})',
            flush=True,
        )
        logger.info(f'[Parser] Team notify {notify_name}: len={len(data)} hex={data.hex()[:256]}')
        _append_packet_debug(f'team_notify_{notify_name}', {
            'raw_hex': data.hex()[:1024],
            'raw_len': len(data),
        })
        # Attempt proto decode as CharTeam (some team notifies may wrap CharTeam)
        pb = _ensure_pb()
        if pb and len(data) > 4:
            for msg_cls_name in ('CharTeam',):
                try:
                    msg_cls = getattr(pb, msg_cls_name, None)
                    if not msg_cls:
                        continue
                    tmsg = msg_cls()
                    tmsg.ParseFromString(data)
                    char_ids = list(tmsg.CharIds)
                    if char_ids:
                        print(
                            f'[Parser] {notify_name} decoded as {msg_cls_name}: '
                            f'team_id={tmsg.TeamId} char_ids={char_ids} '
                            f'member_keys={list(tmsg.TeamMemberData.keys())}',
                            flush=True,
                        )
                        self._process_char_team(self._current_uid, tmsg)
                        return
                except Exception:
                    pass
            # If not CharTeam, log what we can
            logger.info(f'[Parser] {notify_name}: not decodable as CharTeam, raw logged')

    def _try_sync_container_data_fallback(self, data: bytes, reason: str = '') -> bool:
        """Best-effort identity decode when compiled protobuf is unavailable.

        This keeps the startup identity path alive in frozen builds even if the
        generated pb2 module cannot import (missing/incompatible protobuf runtime).
        """
        try:
            outer_fields = _decode_fields(data)
            char_raw = _get_field_bytes(outer_fields, 1)
            if not char_raw:
                logger.error('[Parser] SyncContainerData fallback: missing CharSerialize payload')
                return False
            char_fields = _decode_fields(char_raw)
            uid = _get_field_int(char_fields, CharField.CHAR_ID, 0)
        except Exception as e:
            logger.error(f'[Parser] SyncContainerData fallback decode failed: {e}')
            return False

        if uid <= 0:
            logger.error('[Parser] SyncContainerData fallback: invalid uid=0')
            return False

        reason_text = f' ({reason})' if reason else ''
        logger.warning(
            f'[Parser] SyncContainerData fallback{reason_text}: '
            f'uid={uid}, current_uid={self._current_uid}'
        )
        print(
            f'[Parser] SyncContainerData fallback{reason_text}: '
            f'uid={uid}, current_uid={self._current_uid}',
            flush=True,
        )

        self._sync_container_count += 1

        adopted_uid = False
        if self._current_uid == 0:
            self._current_uid = uid
            adopted_uid = True
            logger.info(f'[Parser] auto-adopt self UID from SyncContainerData fallback: {uid}')
        elif self._sync_container_count > 1 and uid == self._current_uid:
            print(
                f'[Parser] SyncContainerData 重新同步 (第{self._sync_container_count}次), '
                f'保留 {len(self._monsters)} 个怪物 (fallback, 不重置场景)',
                flush=True,
            )

        player = self._get_player(uid)
        changed = False
        fallback_debug = {
            'uid': uid,
            'reason': reason,
            'field_count': len(char_fields),
        }

        char_base_raw = _get_field_bytes(char_fields, CharField.CHAR_BASE)
        if char_base_raw:
            char_base_fields = _decode_fields(char_base_raw)
            name = _decode_utf8_bytes(_get_field_bytes(char_base_fields, 5)).strip()
            if name:
                fallback_debug['name'] = name
                if name != player.name:
                    player.name = name
                    changed = True
            fight_point = _get_field_int(char_base_fields, 35, 0)
            if fight_point > 0:
                fallback_debug['fight_point'] = fight_point
                if fight_point != int(getattr(player, 'fight_point', 0) or 0):
                    player.fight_point = fight_point
                    changed = True

        role_level_raw = _get_field_bytes(char_fields, CharField.ROLE_LEVEL)
        if role_level_raw:
            role_level_fields = _decode_fields(role_level_raw)
            role_level = _get_field_int(role_level_fields, 1, 0)
            if role_level > 0:
                fallback_debug['role_level'] = role_level
                if role_level != player.level:
                    player.level = role_level
                    changed = True

        season_center_raw = _get_field_bytes(char_fields, CharField.SEASON_CENTER)
        if season_center_raw:
            season_center_fields = _decode_fields(season_center_raw)
            battlepass_raw = _get_field_bytes(season_center_fields, 2)
            if battlepass_raw:
                battlepass_fields = _decode_fields(battlepass_raw)
                battlepass_level = _get_field_int(battlepass_fields, 2, 0)
                if battlepass_level > 0:
                    player.battlepass_level = battlepass_level
                    fallback_debug['battlepass_level'] = battlepass_level
                    if _set_level_extra_candidate(player, 'battlepass', battlepass_level):
                        changed = True

        season_medal_raw = _get_field_bytes(char_fields, CharField.SEASON_MEDAL_INFO)
        if season_medal_raw:
            season_medal_fields = _decode_fields(season_medal_raw)
            core_hole_raw = _get_field_bytes(season_medal_fields, 3)
            if core_hole_raw:
                core_hole_fields = _decode_fields(core_hole_raw)
                season_medal_raw_level = _get_field_int(core_hole_fields, 2, 0)
                season_medal_level = _normalize_season_medal_level(season_medal_raw_level)
                if season_medal_level > 0:
                    player.season_medal_level = season_medal_level
                    player.season_level = max(int(getattr(player, 'season_level', 0) or 0), season_medal_level)
                    fallback_debug['season_medal_level'] = season_medal_level
                    if _set_level_extra_candidate(player, 'season_medal', season_medal_level):
                        changed = True

        monster_hunt_raw = _get_field_bytes(char_fields, CharField.MONSTER_HUNT_INFO)
        if monster_hunt_raw:
            monster_hunt_fields = _decode_fields(monster_hunt_raw)
            monster_hunt_level = _get_field_int(monster_hunt_fields, 2, 0)
            monster_hunt_exp = _get_field_int(monster_hunt_fields, 3, 0)
            if monster_hunt_level > 0:
                player.monster_hunt_level = monster_hunt_level
                player.season_level = max(int(getattr(player, 'season_level', 0) or 0), monster_hunt_level)
                fallback_debug['monster_hunt_level'] = monster_hunt_level
                if _set_level_extra_candidate(player, 'monster_hunt', monster_hunt_level):
                    changed = True
            if monster_hunt_exp > 0:
                fallback_debug['monster_hunt_exp'] = monster_hunt_exp
                if _set_season_exp_candidate(player, 'monster_hunt', monster_hunt_exp):
                    changed = True

        profession_list_raw = _get_field_bytes(char_fields, CharField.PROFESSION_LIST)
        if profession_list_raw:
            profession_fields = _decode_fields(profession_list_raw)
            profession_id = _get_field_int(profession_fields, 1, 0)
            if profession_id <= 0:
                for entry_raw in profession_fields.get(4, []) or []:
                    if not isinstance(entry_raw, (bytes, bytearray)):
                        continue
                    entry_fields = _decode_fields(bytes(entry_raw))
                    profession_id = _get_field_int(entry_fields, 1, 0)
                    if profession_id > 0:
                        break
            if profession_id > 0:
                fallback_debug['profession_id'] = profession_id
                profession_name = PROFESSION_NAMES.get(profession_id, '')
                if profession_id != player.profession_id or profession_name != player.profession:
                    player.profession_id = profession_id
                    player.profession = profession_name
                    changed = True
                if self._apply_cached_profession_slots(player):
                    changed = True

        _append_packet_debug('sync_container_data_fallback', fallback_debug)

        if changed and uid == self._current_uid:
            self._notify_self()

        print(
            f'[Parser] SyncContainerData fallback 完成: uid={uid} Lv.{player.level} '
            f'职业={player.profession!r}({player.profession_id})',
            flush=True,
        )
        if adopted_uid:
            self._replay_pending_self_notifies('SyncContainerData fallback')
        return True

    def _on_sync_container_data(self, data: bytes):
        """SyncContainerData { CharSerialize VData = 1 } — 纯 pb2 解析."""
        pb = _ensure_pb()
        if not pb:
            logger.error('[Parser] SyncContainerData: pb2 模块未加载，跳过')
            self._try_sync_container_data_fallback(data, reason='pb2 unavailable')
            return

        try:
            msg = pb.SyncContainerData()
            msg.ParseFromString(data)
            char = msg.VData
        except Exception as e:
            logger.error(f'[Parser] SyncContainerData pb2 解析失败: {e}')
            self._try_sync_container_data_fallback(data, reason='pb2 parse failed')
            return

        uid = char.CharId
        if uid <= 0:
            return

        logger.info(f'[Parser] SyncContainerData received: uid={uid}, current_uid={self._current_uid}')
        print(f'[Parser] SyncContainerData: uid={uid}, current_uid={self._current_uid}', flush=True)

        # Track how many SyncContainerData we've received.
        # First one = login. Subsequent ones = scene/dungeon transition re-sync.
        self._sync_container_count += 1

        # SyncContainerData 是登录时的完整同步, 如果当前 UID 未知则自动采纳
        adopted_uid = False
        if self._current_uid == 0:
            self._current_uid = uid
            adopted_uid = True
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
        if char.HasField('CharBase'):
            cb = char.CharBase
            if cb.Name:
                player.name = cb.Name
                changed = True
                logger.info(f'[Parser] SyncContainerData CharBase Name={cb.Name!r} uid={uid}')
            if cb.FightPoint > 0:
                player.fight_point = cb.FightPoint
                changed = True
            # ── CharTeam (field 20) — parse team members ──
            if cb.HasField('TeamInfo'):
                ti = cb.TeamInfo
                logger.info(
                    f'[Parser] SyncContainerData CharTeam: '
                    f'team_id={ti.TeamId} char_ids={list(ti.CharIds)} '
                    f'member_data_keys={list(ti.TeamMemberData.keys())}'
                )
                self._process_char_team(uid, ti)
            else:
                logger.info(f'[Parser] SyncContainerData: No TeamInfo in CharBase')

        # UserFightAttr (field 16)
        attr_resource_ids = []
        attr_resources = []
        attr_skill_cd_count = 0
        if char.HasField('Attr'):
            a = char.Attr
            # CurHp (field 1)
            if a.CurHp != 0:
                player.hp = a.CurHp
                player.hp_from_full_sync = True
                changed = True
            # MaxHp (field 2)
            if a.MaxHp != 0:
                player.max_hp = a.MaxHp
                changed = True
            # OriginEnergy (field 3, float)
            new_energy = float(a.OriginEnergy)
            new_energy_valid = math.isfinite(new_energy) and new_energy >= 0.0
            if (
                abs(new_energy - float(getattr(player, 'energy', 0.0) or 0.0)) > 0.001 or
                bool(getattr(player, 'energy_valid', False)) != bool(new_energy_valid)
            ):
                player.energy = new_energy
                player.energy_valid = bool(new_energy_valid)
                player.energy_source_priority = 99 if new_energy_valid else 0
                changed = True
            # ResourceIds / Resources (fields 4, 5)
            attr_resource_ids = list(a.ResourceIds)
            attr_resources = list(a.Resources)
            resource_values = _decode_resource_value_map(attr_resource_ids, attr_resources)
            if resource_values != player.resource_values:
                player.resource_values = resource_values
                changed = True
            # CdInfo (field 9, repeated SkillCDInfo)
            skill_cds = []
            for cd in a.CdInfo:
                if cd.SkillLevelId > 0:
                    # UserFightAttr sends SkillCDInfo (VCD at field 8).
                    # Also check field 5 (ValidCDTimeLegacy) for safety.
                    vcd_legacy = max(0, cd.ValidCDTimeLegacy)  # field 5
                    vcd_info   = max(0, cd.ValidCDTime)        # field 8
                    effective_vcd = vcd_info if vcd_info > 0 else vcd_legacy
                    skill_cds.append({
                        'skill_level_id': cd.SkillLevelId,
                        'begin_time': cd.SkillBeginTime,
                        'duration': cd.Duration,
                        'skill_cd_type': cd.SkillCDType,
                        'valid_cd_time': effective_vcd,
                        'charge_count': max(0, cd.ChargeCount),
                        'sub_cd_ratio': max(0, cd.SubCDRatio),
                        'sub_cd_fixed': max(0, int(cd.SubCDFixed)),
                        'accelerate_cd_ratio': max(0, cd.AccelerateCDRatio),
                    })
            attr_skill_cd_count = len(skill_cds)
            if self._replace_skill_cds(player, skill_cds):
                changed = True
            if _refresh_stamina_resource(player):
                changed = True

        # EnergyItem (field 13)
        energy_item = {
            'unlock_nums': [],
            'energy_info_map': {},
        }
        if char.HasField('EnergyItem'):
            ei = char.EnergyItem
            energy_limit = ei.EnergyLimit
            extra_energy_limit = ei.ExtraEnergyLimit
            energy_info_map = {}
            energy_values = []
            unlock_nums = []
            for eid, einfo in ei.EnergyInfo.items():
                ev = einfo.EnergyValue
                un = einfo.UnlockNum
                entry_info = {
                    'energy_value': ev,
                    'unlock_num': un,
                    'item_info_count': len(einfo.EnergyItemInfo),
                }
                if ev >= 0:
                    energy_values.append(ev)
                if un >= 0:
                    unlock_nums.append(un)
                if eid > 0:
                    energy_info_map[eid] = entry_info
            energy_item = {
                'unlock_nums': unlock_nums,
                'energy_info_map': energy_info_map,
            }
            if energy_info_map != player.energy_info_map:
                player.energy_info_map = dict(energy_info_map)
                changed = True
            total_limit = energy_limit + extra_energy_limit
            if total_limit > 0:
                player.energy_limit = energy_limit
                player.extra_energy_limit = extra_energy_limit
                changed = True
                logger.info(
                    f'[Parser] SyncContainerData EnergyItem '
                    f'limit={energy_limit} extra={extra_energy_limit} uid={uid}'
                )
            else:
                sane_limit = max(energy_values) if energy_values else 0
                if sane_limit > 0:
                    player.energy_limit = sane_limit
                    player.extra_energy_limit = 0
                    changed = True
                    logger.info(
                        f'[Parser] SyncContainerData EnergyItem derived_limit='
                        f'{sane_limit} uid={uid}'
                    )
            cur_energy = max(energy_values) if energy_values else 0
            if cur_energy > 0:
                player.energy_info_value = cur_energy
            if _refresh_stamina_resource(player):
                changed = True

        # ── Season / Level-related fields (50, 52, 56, 86, 102) ──
        # SeasonCenter (field 50) → BattlePass.Level
        battlepass_level = 0
        if char.HasField('SeasonCenter') and char.SeasonCenter.HasField('BattlePass'):
            battlepass_level = char.SeasonCenter.BattlePass.Level
        # SeasonMedalInfo (field 52) → CoreHoleInfo.HoleLevel
        season_medal_level = 0
        if char.HasField('SeasonMedalInfo') and char.SeasonMedalInfo.HasField('CoreHoleInfo'):
            raw_level = char.SeasonMedalInfo.CoreHoleInfo.HoleLevel
            season_medal_level = _normalize_season_medal_level(raw_level)
            if season_medal_level > 0:
                logger.info(
                    f'[Parser] SeasonMedalInfo core_raw={raw_level} '
                    f'core_norm={season_medal_level}'
                )
        # MonsterHuntInfo (field 56) → CurLevel / CurExp
        monster_hunt_level = 0
        monster_hunt_exp = 0
        if char.HasField('MonsterHuntInfo'):
            monster_hunt_level = char.MonsterHuntInfo.CurLevel
            monster_hunt_exp = max(0, int(char.MonsterHuntInfo.CurExp or 0))
        # BattlePassData (field 86) → max BattlePass.Level across entries
        battlepass_data_level = 0
        if char.HasField('BattlePassData'):
            bp_levels = [bp.Level for bp in char.BattlePassData.BattleMap.values() if bp.Level > 0]
            battlepass_data_level = max(bp_levels) if bp_levels else 0
        # DeepSleepResonance (field 102) → season_type==3 entry
        deep_sleep_level = 0
        deep_sleep_exp = 0
        if char.HasField('DeepSleepResonance'):
            for entry in char.DeepSleepResonance.Entries:
                if entry.SeasonType == 3 and entry.HasField('Info'):
                    ds_lv = entry.Info.Level
                    ds_exp = entry.Info.CurExp
                    if ds_lv > 0:
                        deep_sleep_level = ds_lv
                        deep_sleep_exp = ds_exp

        # Apply all level-related values (common for both paths)
        player.season_medal_level = season_medal_level
        player.monster_hunt_level = monster_hunt_level
        player.battlepass_level = battlepass_level
        player.battlepass_data_level = battlepass_data_level
        player.season_level = max(season_medal_level, monster_hunt_level)
        if _set_level_extra_candidate(player, 'season_medal', season_medal_level):
            changed = True
        if _set_level_extra_candidate(player, 'monster_hunt', monster_hunt_level):
            changed = True
        if _set_season_exp_candidate(player, 'monster_hunt', monster_hunt_exp):
            changed = True
        if _set_level_extra_candidate(player, 'battlepass', battlepass_level):
            changed = True
        if _set_level_extra_candidate(player, 'battlepass_data', battlepass_data_level):
            changed = True
        if deep_sleep_level > 0:
            if _set_level_extra_candidate(player, 'deep_sleep', deep_sleep_level):
                changed = True
            if _set_season_exp_candidate(player, 'deep_sleep', deep_sleep_exp):
                changed = True
            print(
                f'[Parser] SyncContainerData: 深眠心相仪等级 Lv.{deep_sleep_level} '
                f'经验={deep_sleep_exp} uid={uid}',
                flush=True,
            )
            _append_packet_debug('deep_sleep_level', {
                'uid': uid,
                'deep_sleep_level': deep_sleep_level,
                'deep_sleep_exp': deep_sleep_exp,
            })

        # ── Full dump of ALL CharSerialize fields for deep analysis (pb2) ──
        full_sync_dump = {'uid': uid, 'field_count': 0, 'fields': {}}
        if _MessageToDict:
            try:
                all_fields = _MessageToDict(char, preserving_proto_field_name=True)
                for fk, fv in all_fields.items():
                    if fk == 'CharId':
                        continue
                    full_sync_dump['fields'][fk] = fv
            except Exception as e:
                logger.debug(f'[Parser] MessageToDict error: {e}')
        else:
            # Fallback: list field names only
            for fd in char.DESCRIPTOR.fields:
                if fd.number == 1:
                    continue
                try:
                    if fd.message_type and char.HasField(fd.name):
                        full_sync_dump['fields'][fd.name] = f'<{fd.message_type.name}>'
                except (ValueError, AttributeError):
                    pass
        full_sync_dump['field_count'] = len(full_sync_dump['fields'])
        _append_packet_debug('sync_container_full_dump', full_sync_dump)

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

        # RoleLevel (field 22)
        if char.HasField('RoleLevel'):
            rl = char.RoleLevel
            role_level = rl.Level
            prev_season_max_lv = rl.PrevSeasonMaxLv
            role_level_debug = {
                'uid': uid,
                'role_level': role_level,
                'prev_season_max_lv': prev_season_max_lv,
                'last_season_day': rl.LastSeasonDay,
                'bless_exp_pool': rl.BlessExpPool,
                'grant_bless_exp': rl.GrantBlessExp,
                'accumulate_bless_exp': rl.AccumulateBlessExp,
                'accumulate_exp': rl.AccumulateExp,
            }
            logger.info(
                f'[Parser] SyncContainerData RoleLevel(pb2) '
                f'role_level={role_level}, prev_season_max_lv={prev_season_max_lv}, uid={uid}'
            )
            _append_packet_debug('role_level', role_level_debug)
            if role_level > 0:
                player.level = role_level
                changed = True
                print(f'[Parser] SyncContainerData: 等级 Lv.{role_level} uid={uid}', flush=True)
            else:
                logger.warning(
                    f'[Parser] SyncContainerData RoleLevel(pb2): level=0! uid={uid}'
                )

        # Slots (field 55) — full skill bar layout including resonance slots 7,8
        if char.HasField('Slots'):
            slot_bar = {}
            for slot_id, si in char.Slots.Slots.items():
                sid = si.SkillId
                if slot_id > 0 and sid > 0:
                    slot_bar[slot_id] = sid
            if slot_bar and slot_bar != player.slot_bar_map:
                player.slot_bar_map = dict(slot_bar)
                changed = True
                logger.info(f'[Parser] SyncContainerData Slots(55/pb2) bar={slot_bar} uid={uid}')
                _append_packet_debug(
                    'slot_bar',
                    {'uid': uid, 'slot_bar_map': slot_bar}
                )

        # ProfessionList (field 61)
        if char.HasField('ProfessionList'):
            pl = char.ProfessionList
            cur_prof_id = pl.CurProfessionId

            # Build all_skill_info from AoyiSkillInfoMap (field 7)
            all_skill_info: Dict[int, Dict[str, int]] = {}
            for key, psi in pl.AoyiSkillInfoMap.items():
                all_skill_info[key] = {
                    'skill_id': psi.SkillId,
                    'level': psi.Level,
                }

            # Find current profession data from ProfessionList_ (field 4)
            current_prof = None
            profession_entries = list(pl.ProfessionList_.items())
            for pid_key, pinfo in profession_entries:
                if pid_key == cur_prof_id:
                    current_prof = pinfo
                    break
            if current_prof is None and len(profession_entries) == 1:
                cur_prof_id = profession_entries[0][0]
                current_prof = profession_entries[0][1]

            profession_data: Dict[str, Any] = {
                'profession_id': cur_prof_id,
                'slot_skill_level_map': {},
                'active_skill_ids': [],
                'skill_info_map': {},
                'skill_level_info_map': {},
            }

            if current_prof is not None:
                # Current profession's SkillInfoMap (field 4 of ProfessionInfo)
                current_skill_info: Dict[int, Dict[str, int]] = {}
                for sk, psi in current_prof.SkillInfoMap.items():
                    current_skill_info[sk] = {
                        'skill_id': psi.SkillId,
                        'level': psi.Level,
                    }
                for sk, info in all_skill_info.items():
                    current_skill_info.setdefault(sk, info)

                profession_data['skill_info_map'] = current_skill_info

                # Build skill_level_info_map
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
                profession_data['skill_level_info_map'] = skill_level_info_map

                profession_data['active_skill_ids'] = list(current_prof.ActiveSkillIds)

                # SlotSkillInfoMap (field 7 of ProfessionInfo) → map<int32, int32>
                raw_slot_map = dict(current_prof.SlotSkillInfoMap)
                normalized_slot_map: Dict[int, int] = {}
                for slot, mapped_id in raw_slot_map.items():
                    if slot < 0 or mapped_id <= 0:
                        continue
                    skill_info = current_skill_info.get(mapped_id)
                    skill_level_id = 0
                    if skill_info:
                        skill_level_id = _compose_skill_level_id(mapped_id, skill_info.get('level', 0))
                    if skill_level_id <= 0:
                        for ik, iv in current_skill_info.items():
                            if iv.get('skill_id') == mapped_id:
                                skill_level_id = _compose_skill_level_id(ik, iv.get('level', 0))
                                break
                    if skill_level_id <= 0:
                        skill_level_id = mapped_id
                    normalized_slot_map[slot] = skill_level_id
                    if skill_level_id > 0 and skill_level_id not in skill_level_info_map:
                        si = current_skill_info.get(mapped_id) or {}
                        skill_level_info_map[skill_level_id] = {
                            'skill_id': int(si.get('skill_id') or mapped_id or 0),
                            'level': int(si.get('level', 0) or 0),
                        }
                profession_data['slot_skill_level_map'] = normalized_slot_map

            pid = profession_data['profession_id']
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

        # ── Parse ALL remaining CharSerialize fields via pb2 ──
        # Specific useful fields get dedicated pb2 decoders:
        #   3 (SceneData) → scene_id (MapId)
        #   6 (BuffDBInfo) → buff_list
        #   12 (EquipList) → equip info
        #   15 (DungeonList) → dungeon data
        #   28 (Resonance) → resonance data

        # SceneData (field 3) — current scene/map
        if char.HasField('SceneData'):
            sd = char.SceneData
            new_scene_id = sd.MapId
            scene = {
                'map_id': sd.MapId,
                'channel_id': sd.ChannelId,
                'plane_id': sd.PlaneId,
                'scene_layer': sd.SceneLayer,
            }
            if new_scene_id > 0:
                player.scene_id = new_scene_id
                changed = True
                if self._last_scene_id != 0 and new_scene_id != self._last_scene_id:
                    logger.info(
                        f'[Parser] 场景ID变更: {self._last_scene_id} → {new_scene_id} uid={uid}'
                    )
                self._last_scene_id = new_scene_id
            player.extended_data['SCENE_DATA'] = scene
            logger.info(f'[Parser] CharSerialize SceneData(pb2): {scene} uid={uid}')

        # BuffDBInfo (field 6) — active buffs on self
        if char.HasField('BuffInfo'):
            bi = char.BuffInfo
            buffs = []
            for buf_id, buf_data in bi.AllBuffDbData.items():
                buffs.append({
                    'buff_id': buf_id,
                    'config_id': buf_data.BuffConfigId,
                    'level': buf_data.Level,
                    'layer': buf_data.Layer,
                    'duration': buf_data.Duration,
                })
            player.buff_list = buffs
            player.extended_data['BUFF_DB_INFO'] = {'buff_count': len(buffs)}
            changed = True
            logger.info(f'[Parser] CharSerialize BuffDBInfo(pb2): {len(buffs)} buffs uid={uid}')

        # EquipList (field 12) — equipped items
        if char.HasField('Equip'):
            eq = char.Equip
            equip_count = len(eq.EquipList_)
            player.extended_data['EQUIP_LIST'] = {'equip_count': equip_count}
            logger.info(f'[Parser] CharSerialize EquipList(pb2): {equip_count} items uid={uid}')

        # DungeonList (field 15) — dungeon data
        if char.HasField('DungeonList'):
            dl = char.DungeonList
            dungeon_data = {
                'complete_count': len(dl.CompleteDungeon),
                'reset_time': dl.ResetTime,
            }
            player.extended_data['DUNGEON_LIST'] = dungeon_data
            logger.info(f'[Parser] CharSerialize DungeonList(pb2): {dungeon_data} uid={uid}')

        # Resonance (field 28) — resonance system
        if char.HasField('Resonance'):
            res = char.Resonance
            res_data = {
                'installed': res.Installed,
                'resonance_count': len(res.Resonances),
            }
            player.extended_data['RESONANCE'] = res_data
            logger.info(f'[Parser] CharSerialize Resonance(pb2): {res_data} uid={uid}')

        # Generic pb2 decode for ALL other CharSerialize fields
        _generic_decoded_fields = []
        for _fd in char.DESCRIPTOR.fields:
            if _fd.number <= 1:
                continue  # skip CharId
            if _fd.number in _HANDLED_CHAR_FIELDS:
                continue
            try:
                if _fd.message_type and char.HasField(_fd.name):
                    sub_msg = getattr(char, _fd.name)
                    if _MessageToDict:
                        player.extended_data[_fd.name] = _MessageToDict(
                            sub_msg, preserving_proto_field_name=True
                        )
                    else:
                        player.extended_data[_fd.name] = f'<{_fd.message_type.name}>'
                    _generic_decoded_fields.append(f'{_fd.name}({_fd.number})')
            except (ValueError, AttributeError):
                pass

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

        if adopted_uid:
            self._replay_pending_self_notifies('SyncContainerData')


    # SyncContainerDirtyData (incremental updates)


    def _on_sync_container_dirty(self, data: bytes):
        """Handle the custom dirty-data stream wrapper."""
        if self._current_uid == 0:
            self._remember_pending_self_notify(
                NotifyMethod.SYNC_CONTAINER_DIRTY_DATA,
                data,
                'current_uid=0',
            )
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
        if ident != _DIRTY_IDENT:
            # ── Smart offset detection: scan for identifier ──
            smart_pos = _smart_find_dirty_start(data)
            if smart_pos is None:
                logger.debug(
                    f'[Parser] DirtyData: no 0xFFFFFFFE at pos=0 and smart scan failed '
                    f'(len={len(data)}, head={data[:16].hex()})'
                )
                return
            logger.warning(
                f'[Parser] DirtyData smart offset correction: '
                f'found identifier at pos={smart_pos} (skipped {smart_pos} leading bytes)'
            )
            pos = smart_pos
        pos += 4
        # skip validation int32BE
        pos += 4

        if pos + 4 > len(data):
            return
        field_index = struct.unpack_from('<I', data, pos)[0]
        pos += 4
        _fname = CHAR_FIELD_NAMES.get(field_index, f'FIELD_{field_index}')
        debug_info = {
            'uid': uid,
            'field_index': field_index,
        }
        # Track DirtyData field distribution
        if not hasattr(self, '_dirty_field_counts'):
            self._dirty_field_counts = {}
        self._dirty_field_counts[field_index] = self._dirty_field_counts.get(field_index, 0) + 1
        total_dirty = sum(self._dirty_field_counts.values())
        if total_dirty <= 5 or total_dirty % 50 == 0:
            logger.info(f'[Parser] DirtyData field={field_index}({_fname}) total_dirty={total_dirty} dist={dict(sorted(self._dirty_field_counts.items()))}')

        if field_index == 2:  # CharBase - fully parsed for ALL sub_fields from proto CharBaseInfo
            debug_info['field_name'] = 'CharBase'
            # Dump full raw hex for deep analysis
            debug_info['full_raw_hex'] = data[pos:].hex()[:256]
            debug_info['remaining_len'] = len(data) - pos
            if pos + 8 > len(data):
                return
            ident2 = struct.unpack_from('<I', data, pos)[0]
            if ident2 != _DIRTY_IDENT:
                return
            pos += 8  # skip identifier + validation
            if pos + 4 > len(data):
                return
            sub_field = struct.unpack_from('<I', data, pos)[0]
            pos += 4
            debug_info['sub_field'] = sub_field

            # Complete mapping for ALL sub_fields in CharBaseInfo (from proto)
            # int64=Q, uint64=Q, int32=I, uint32=I, float=f, bool=B, string=str, enum=I
            # message types (FaceData, ProfileInfo, CharTeam, UserUnion, AvatarInfo) = 'msg'
            # repeated int32 = 'rep_i32'
            _CHARBASE_FIELDS = {
                1:  ('CharId', 'Q'),         # int64
                2:  ('AccountId', 'str'),     # string
                3:  ('ShowId', 'Q'),          # int64
                4:  ('ServerId', 'I'),        # uint32
                5:  ('Name', 'str'),          # string
                6:  ('Gender', 'I'),          # enum EGender
                7:  ('IsDeleted', 'B'),       # bool
                8:  ('IsForbid', 'B'),        # bool
                9:  ('IsMute', 'B'),          # bool
                10: ('X', 'f'),              # float
                11: ('Y', 'f'),              # float
                12: ('Z', 'f'),              # float
                13: ('Dir', 'f'),            # float
                14: ('FaceData', 'msg'),     # message FaceData
                15: ('CardId', 'I'),         # uint32
                16: ('CreateTime', 'Q'),     # int64
                17: ('OnlineTime', 'Q'),     # int64
                18: ('OfflineTime', 'Q'),    # int64
                19: ('ProfileInfo', 'msg'),  # message ProfileInfo
                20: ('TeamInfo', 'msg'),     # message CharTeam
                21: ('CharState', 'Q'),      # uint64
                22: ('BodySize', 'I'),       # enum EBodySize
                23: ('UnionInfo', 'msg'),    # message UserUnion
                24: ('PersonalState', 'rep_i32'),  # repeated int32
                25: ('AvatarInfo', 'msg'),   # message AvatarInfo
                26: ('TotalOnlineTime', 'Q'),# uint64
                27: ('OpenId', 'str'),       # string
                28: ('SdkType', 'I'),        # int32
                29: ('Os', 'I'),             # int32
                31: ('InitProfessionId', 'I'),  # int32
                32: ('LastCalTotalTime', 'Q'),  # uint64
                33: ('AreaId', 'I'),         # int32
                34: ('ClientVersion', 'str'),   # string
                35: ('FightPoint', 'I'),     # int32
                36: ('SumSave', 'Q'),        # int64
                37: ('ClientResourceVersion', 'str'),  # string
                38: ('LastOfflineTime', 'Q'),   # int64
                39: ('DayAccDurTime', 'I'),  # int32
                40: ('LastAccDurTimestamp', 'Q'),  # int64
                41: ('SaveSerial', 'Q'),     # int64
            }
            if sub_field in _CHARBASE_FIELDS:
                fname, ftype = _CHARBASE_FIELDS[sub_field]
                debug_info['sub_name'] = fname
                if ftype == 'str':
                    if pos + 4 > len(data):
                        debug_info['tail_hex'] = data[pos:].hex()
                    else:
                        slen = struct.unpack_from('<I', data, pos)[0]
                        pos += 4
                        if pos + slen <= len(data):
                            val = data[pos:pos+slen].decode('utf-8', 'ignore')
                            pos += slen
                            debug_info['value'] = val
                            if fname == 'Name':
                                player.name = val
                                changed = True
                        else:
                            debug_info['tail_hex'] = data[pos:].hex()
                elif ftype == 'I':
                    if pos + 4 <= len(data):
                        val = struct.unpack_from('<I', data, pos)[0]
                        pos += 4
                        debug_info['u32'] = val
                        if fname == 'FightPoint':
                            player.fight_point = val
                            changed = True
                    else:
                        debug_info['tail_hex'] = data[pos:].hex()
                elif ftype == 'Q':
                    if pos + 8 <= len(data):
                        val = struct.unpack_from('<Q', data, pos)[0]
                        pos += 8
                        debug_info['u64'] = val
                    else:
                        debug_info['tail_hex'] = data[pos:].hex()
                elif ftype == 'f':
                    if pos + 4 <= len(data):
                        val = struct.unpack_from('<f', data, pos)[0]
                        pos += 4
                        debug_info['float'] = round(val, 4)
                    else:
                        debug_info['tail_hex'] = data[pos:].hex()
                elif ftype == 'B':
                    if pos + 1 <= len(data):
                        val = data[pos]
                        pos += 1
                        debug_info['bool'] = bool(val)
                    else:
                        debug_info['tail_hex'] = data[pos:].hex()
                elif ftype in ('msg', 'rep_i32'):
                    # For message/repeated types, dump remaining raw bytes
                    debug_info['msg_hex'] = data[pos:].hex()[:256]
                    # ── Parse CharTeam (sub_field 20) via pb2 ──
                    if sub_field == 20 and ftype == 'msg':
                        try:
                            pb = _ensure_pb()
                            if pb:
                                team_msg = pb.CharTeam()
                                team_msg.ParseFromString(data[pos:])
                                logger.info(
                                    f'[Parser] DirtyData CharTeam received: '
                                    f'team_id={team_msg.TeamId} char_ids={list(team_msg.CharIds)} '
                                    f'member_data_keys={list(team_msg.TeamMemberData.keys())}'
                                )
                                self._process_char_team(self._current_uid, team_msg)
                        except Exception as e:
                            print(f'[Parser] DirtyData CharTeam 解析失败: {e}', flush=True)
                            logger.warning(f'[Parser] DirtyData CharTeam parse error: {e}')
                            _append_packet_debug('char_team_parse_error', {
                                'error': str(e),
                                'raw_hex': data[pos:].hex()[:512],
                                'raw_len': len(data) - pos,
                            })
            else:
                debug_info['unknown_sub'] = True
                debug_info['raw_hex'] = data[pos:].hex()[:256]

            # Always dump remaining bytes after parsed value
            if pos < len(data):
                debug_info['after_value_hex'] = data[pos:].hex()[:256]
                debug_info['after_value_len'] = len(data) - pos

        elif field_index == 16:  # UserFightAttr
            if pos + 8 > len(data):
                return
            ident2 = struct.unpack_from('<I', data, pos)[0]
            if ident2 != _DIRTY_IDENT:
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
            if ident2 != _DIRTY_IDENT:
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
                    if lv != _KNOWN_BASE_LEVEL:
                        logger.warning(
                            f'[Parser] DirtyData RoleLevel: expected {_KNOWN_BASE_LEVEL}, '
                            f'got {lv}. Possible offset error or game-cap change.'
                        )
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
            if ident2 != _DIRTY_IDENT:
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
                if ident3 != _DIRTY_IDENT:
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
            if ident2 != _DIRTY_IDENT:
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
                if ident3 != _DIRTY_IDENT:
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
            if ident2 != _DIRTY_IDENT:
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
            elif sub_field == 3 and pos + 4 <= len(data):  # CurExp
                hunt_exp = struct.unpack_from('<I', data, pos)[0]
                debug_info['u32'] = hunt_exp
                debug_info['monster_hunt_exp'] = hunt_exp
                logger.info(f'[Parser] DirtyData MonsterHuntInfo.CurExp -> {hunt_exp}')
                if _set_season_exp_candidate(player, 'monster_hunt', hunt_exp):
                    changed = True
            else:
                debug_info['raw_hex'] = data[pos:].hex()[:128]

        elif field_index == 86:  # BattlePassData
            debug_info['raw_hex'] = data[pos:].hex()[:128]

        elif field_index == 102:  # 深眠心相仪等级 (Deep Sleep Resonance Level)
            debug_info['field_name'] = 'DEEP_SLEEP_LEVEL'
            debug_info['full_raw_hex'] = data[pos:].hex()[:256]
            # Binary dirty format: _DIRTY_IDENT + validation + sub_field + nested data
            if pos + 8 <= len(data):
                ident2 = struct.unpack_from('<I', data, pos)[0]
                if ident2 == _DIRTY_IDENT:
                    pos += 8  # skip identifier + validation
                    if pos + 4 <= len(data):
                        sub_field = struct.unpack_from('<I', data, pos)[0]
                        pos += 4
                        debug_info['sub_field'] = sub_field
                        # sub_field=3 is 深眠心相仪
                        remaining = data[pos:]
                        if len(remaining) > 0:
                            try:
                                # Use pb2 DeepSleepSeasonEntry for inner protobuf decode
                                pb = _ensure_pb()
                                if pb:
                                    entry = pb.DeepSleepSeasonEntry()
                                    entry.ParseFromString(remaining)
                                    ds_lv = entry.Info.Level if entry.HasField('Info') else 0
                                    ds_exp = entry.Info.CurExp if entry.HasField('Info') else 0
                                    if ds_lv > 0:
                                        debug_info['level'] = ds_lv
                                        debug_info['exp'] = ds_exp
                                        if sub_field == 3:
                                            if _set_level_extra_candidate(player, 'deep_sleep', ds_lv):
                                                changed = True
                                            if _set_season_exp_candidate(player, 'deep_sleep', ds_exp):
                                                changed = True
                                            print(f'[Parser] DirtyData 深眠心相仪等级 -> Lv.{ds_lv} 经验={ds_exp}', flush=True)
                                else:
                                    # fallback to manual decode
                                    nested = _decode_fields(remaining)
                                    ds_data_raw = nested.get(2, [None])[0]
                                    if isinstance(ds_data_raw, bytes):
                                        ds_data = _decode_fields(ds_data_raw)
                                        ds_lv = ds_data.get(1, [0])[0]
                                        ds_exp = ds_data.get(2, [0])[0]
                                        if isinstance(ds_lv, int) and ds_lv > 0:
                                            debug_info['level'] = ds_lv
                                            debug_info['exp'] = ds_exp
                                            if sub_field == 3:
                                                if _set_level_extra_candidate(player, 'deep_sleep', ds_lv):
                                                    changed = True
                                                if _set_season_exp_candidate(player, 'deep_sleep', ds_exp):
                                                    changed = True
                                                print(f'[Parser] DirtyData 深眠心相仪等级 -> Lv.{ds_lv} 经验={ds_exp}', flush=True)
                            except Exception:
                                debug_info['nested_hex'] = remaining.hex()[:128]

        elif field_index == 61:  # ProfessionList
            if pos + 8 > len(data):
                return
            ident2 = struct.unpack_from('<I', data, pos)[0]
            if ident2 != _DIRTY_IDENT:
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
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.SyncNearEntities()
            msg.ParseFromString(data)
        except Exception as e:
            logger.debug(f'[Parser] SyncNearEntities pb2 parse error: {e}')
            return

        # ── Disappear ──
        for de in msg.Disappear:
            uuid = de.Uuid
            if not uuid:
                continue
            disappear_type = int(de.Type)

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

        # ── Appear ──
        player_uids_appeared = []
        monster_uuids_appeared = []
        for entity in msg.Appear:
            uuid = entity.Uuid
            if not uuid:
                continue

            has_attrs = entity.HasField('Attrs')
            has_temp_attrs = entity.HasField('TempAttrs')
            has_buffs = entity.HasField('BuffInfos')

            if _is_player(uuid):
                uid = _uuid_to_uid(uuid)
                player_uids_appeared.append(uid)
                if uid == self._current_uid and self._current_uuid != uuid:
                    self._current_uuid = uuid
                    logger.info(f'[Parser] matched self entity from SyncNearEntities: uuid={uuid} uid={uid}')
                if has_attrs:
                    self._process_attr_collection(uid, entity.Attrs)
                    # Debug: log what attrs were present for this player
                    attr_ids = [a.Id for a in entity.Attrs.Attrs]
                    has_name_attr = 1 in attr_ids
                    p = self._get_player(uid)
                    logger.info(
                        f'[Parser] Entity appear player uid={uid} attrs={attr_ids[:15]} '
                        f'has_name={has_name_attr} resolved_name={p.name!r}'
                    )
                    if uid != self._current_uid:
                        print(
                            f'[Parser] 其他玩家出现: uid={uid} name={p.name!r} '
                            f'has_name_attr={has_name_attr} n_attrs={len(attr_ids)}',
                            flush=True,
                        )
                else:
                    logger.info(f'[Parser] Entity appear player uid={uid} NO attrs')
                    if uid != self._current_uid:
                        print(f'[Parser] 其他玩家出现: uid={uid} NO attrs', flush=True)
                if has_temp_attrs:
                    if uid == self._current_uid or self._current_uid == 0:
                        self._process_temp_attr_collection(uid, entity.TempAttrs)
                # Decode initial buff state from entity appear
                if has_buffs:
                    if uid == self._current_uid or self._current_uid == 0:
                        player = self._get_player(uid)
                        buffs = _decode_buff_info_sync_pb(entity.BuffInfos)
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
                    self._notify_self()
            elif _is_monster(uuid):
                monster_uuids_appeared.append(uuid)
                monster = self._get_monster(uuid)
                monster.is_dead = False
                if has_attrs:
                    self._process_monster_attr_collection(uuid, entity.Attrs)
                if monster.max_hp == 0 and monster.template_id > 0:
                    cached_max = self._monster_hp_cache.get(monster.template_id)
                    if cached_max and cached_max > 0:
                        monster.max_hp = cached_max
                        logger.info(
                            f'[Parser] Monster appear: max_hp from cache '
                            f'tid={monster.template_id} max_hp={cached_max} uuid={uuid}'
                        )
                # Decode initial buff state on monster
                if has_buffs:
                    buffs = _decode_buff_info_sync_pb(entity.BuffInfos)
                    if buffs:
                        monster.buff_list = buffs
                        logger.debug(f'[Parser] Entity appear: {len(buffs)} buffs on monster uuid={uuid}')
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
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.SyncToMeDeltaInfo()
            msg.ParseFromString(data)
        except Exception as e:
            logger.debug(f'[Parser] SyncToMeDeltaInfo pb2 parse error: {e}')
            return
        di = msg.DeltaInfo  # AoiSyncToMeDelta
        player = None
        adopted_uid = False

        # UUID from AoiSyncToMeDelta.Uuid (field 5)
        uuid = di.Uuid
        if uuid != 0:
            if _is_player(uuid):
                new_uid = _uuid_to_uid(uuid)
                player = self._get_player(new_uid)
                if self._current_uuid != uuid:
                    self._current_uuid = uuid
                    adopted_uid = self._current_uid == 0 and new_uid > 0
                    self._current_uid = new_uid
                    logger.info(f'[Parser] confirmed self UUID={uuid}, UID={new_uid}')
                    if new_uid in self._players:
                        self._notify_self()
        else:
            # Uuid field 5 is 0 — try BaseDelta.Uuid, then fall back to _current_uid
            base_uuid = di.BaseDelta.Uuid if di.HasField('BaseDelta') else 0
            if base_uuid != 0 and _is_player(base_uuid):
                fallback_uid = _uuid_to_uid(base_uuid)
                player = self._get_player(fallback_uid)
                if self._current_uuid != base_uuid:
                    self._current_uuid = base_uuid
                    adopted_uid = self._current_uid == 0 and fallback_uid > 0
                    self._current_uid = fallback_uid
                    logger.info(f'[Parser] confirmed self UUID={base_uuid} (from BaseDelta), UID={fallback_uid}')
                    if fallback_uid in self._players:
                        self._notify_self()
            elif self._current_uid:
                # SyncToMeDelta is always about self — use known UID
                player = self._get_player(self._current_uid)

        # Debug: log SyncToMeDelta summary
        n_cds = len(di.SyncSkillCDs)
        n_fres = len(di.FightResCDs)
        has_base = di.HasField('BaseDelta')
        if n_cds > 0 or n_fres > 0:
            logger.info(
                f'[Parser] SyncToMeDelta: uuid={uuid} player={"yes" if player else "NO"} '
                f'skill_cds={n_cds} fight_res_cds={n_fres} has_base={has_base} '
                f'current_uid={self._current_uid}'
            )
        if player is None and self._current_uid == 0 and (n_cds > 0 or n_fres > 0 or has_base):
            self._remember_pending_self_notify(
                NotifyMethod.SYNC_TO_ME_DELTA_INFO,
                data,
                'current_uid=0',
            )
            logger.info(
                f'[Parser] SyncToMeDelta buffered until self uid is known '
                f'skill_cds={n_cds} fight_res_cds={n_fres} has_base={has_base}'
            )
            return

        skill_cd_changed = False
        sync_skill_cds = []
        fight_res_cds = []
        if player is not None:
            for cd in di.SyncSkillCDs:
                # Capture VCD from BOTH field 5 (SkillCD wire format used by
                # SyncToMeDelta) and field 8 (SkillCDInfo format used by
                # UserFightAttr).  The server sends SkillCD format for
                # SyncToMeDelta, where ValidCDTime lives at proto field 5.
                vcd_legacy = max(0, cd.ValidCDTimeLegacy)  # field 5
                vcd_info   = max(0, cd.ValidCDTime)        # field 8
                effective_vcd = vcd_legacy if vcd_legacy > 0 else vcd_info
                decoded_cd = {
                    'skill_level_id': cd.SkillLevelId,
                    'begin_time': cd.SkillBeginTime,
                    'duration': cd.Duration,
                    'skill_cd_type': int(cd.SkillCDType),
                    'valid_cd_time': effective_vcd,
                    'vcd_f5': vcd_legacy,
                    'vcd_f8': vcd_info,
                    'charge_count': cd.ChargeCount,
                    'sub_cd_ratio': cd.SubCDRatio,
                    'sub_cd_fixed': cd.SubCDFixed,
                    'accelerate_cd_ratio': cd.AccelerateCDRatio,
                }
                if decoded_cd['skill_level_id'] <= 0:
                    continue
                sync_skill_cds.append(decoded_cd)
                if self._update_skill_cd(player, decoded_cd):
                    skill_cd_changed = True
            if sync_skill_cds:
                # One-time diagnostic: log first VCD capture from field 5
                sample_vcd = [(c['skill_level_id'], c['vcd_f5'], c['vcd_f8'])
                              for c in sync_skill_cds[:5]]
                if any(v5 > 0 for _, v5, _ in sample_vcd):
                    logger.info(f'[Parser] VCD field5 captured: {sample_vcd}')
                _append_packet_debug('sync_skill_cd', {
                    'uid': player.uid, 'skill_cds': sync_skill_cds,
                })
            for fcd in di.FightResCDs:
                decoded_fight_cd = {
                    'res_id': fcd.ResId,
                    'begin_time': fcd.BeginTime,
                    'duration': fcd.Duration,
                    'valid_cd_time': fcd.ValidCDTime,
                }
                res_id = decoded_fight_cd['res_id']
                if res_id > 0:
                    fight_res_cds.append(decoded_fight_cd)
                    player.fight_res_cd_map[res_id] = {
                        'res_id': res_id,
                        'begin_time': decoded_fight_cd['begin_time'],
                        'duration': decoded_fight_cd['duration'],
                        'valid_cd_time': decoded_fight_cd['valid_cd_time'],
                        'observed_at_ms': int(time.time() * 1000),
                    }
            if fight_res_cds:
                _append_packet_debug('fight_res_cd', {
                    'uid': player.uid, 'fight_res_cds': fight_res_cds,
                })

        # AoiSyncDelta handling — BaseDelta (field 1)
        if di.HasField('BaseDelta'):
            self._process_aoi_sync_delta(di.BaseDelta)
        if skill_cd_changed and player is not None and player.uid == self._current_uid:
            self._notify_self()
        if adopted_uid:
            self._replay_pending_self_notifies('SyncToMeDelta')


    #  SyncNearDeltaInfo (0x2D)


    def _on_sync_near_delta(self, data: bytes):
        pb = _ensure_pb()
        if not pb:
            return
        try:
            msg = pb.SyncNearDeltaInfo()
            msg.ParseFromString(data)
        except Exception as e:
            logger.debug(f'[Parser] SyncNearDeltaInfo pb2 parse error: {e}')
            return
        for delta in msg.DeltaInfos:
            self._process_aoi_sync_delta(delta)


    # AoiSyncDelta handling


    def _process_aoi_sync_delta(self, delta):
        """Process an AoiSyncDelta pb2 object."""
        uuid = delta.Uuid
        if uuid == 0:
            return
        target_is_player = _is_player(uuid)
        target_is_monster = _is_monster(uuid)
        uid = _uuid_to_uid(uuid)

        # Attrs (field 2) — players and monsters
        if delta.HasField('Attrs'):
            if target_is_player:
                self._process_attr_collection(uid, delta.Attrs)
            elif target_is_monster:
                self._process_monster_attr_collection(uuid, delta.Attrs)

        # TempAttrs (field 3) — buff-based temporary attributes (CD modifiers etc.)
        if delta.HasField('TempAttrs') and target_is_player:
            if uid == self._current_uid or self._current_uid == 0:
                self._process_temp_attr_collection(uid, delta.TempAttrs)

        # BuffEffectSync (field 11) — boss buff events
        if delta.HasField('BuffEffect') and target_is_monster:
            self._process_buff_effect_sync(uuid, delta.BuffEffect)

        # BuffInfoSync (field 10) — buff state updates
        if delta.HasField('BuffInfos'):
            buffs = _decode_buff_info_sync_pb(delta.BuffInfos)
            if buffs:
                if target_is_player:
                    if uid in self._players:
                        self._players[uid].buff_list = buffs
                elif target_is_monster and uuid in self._monsters:
                    self._monsters[uuid].buff_list = buffs

        # PassiveSkillInfos (field 8) — passive skill triggers
        if delta.HasField('PassiveSkillInfos'):
            logger.debug(f'[Parser] AoiDelta PassiveSkillInfos uuid={uuid}')

        # PassiveSkillEndInfos (field 9) — passive skill end
        if delta.HasField('PassiveSkillEndInfos'):
            logger.debug(f'[Parser] AoiDelta PassiveSkillEndInfos uuid={uuid}')

        # SkillEffect (field 7) — damage extraction
        if delta.HasField('SkillEffects'):
            self._process_skill_effect(uuid, target_is_player, target_is_monster, delta.SkillEffects)

    def _process_skill_effect(self, target_uuid: int, target_is_player: bool,
                              target_is_monster: bool, se):
        """Decode SkillEffect (AoiSyncDelta field 7) and emit damage events.
        se is a pb2 SkillEffect object.
        """
        for dmg in se.Damages:
            try:
                self._decode_sync_damage_info(target_uuid, target_is_player,
                                              target_is_monster, dmg)
            except Exception as e:
                logger.debug(f'[Parser] damage decode error: {e}')

    def _decode_sync_damage_info(self, target_uuid: int, target_is_player: bool,
                                 target_is_monster: bool, dmg):
        """Decode a single SyncDamageInfo pb2 object and fire on_damage callback."""
        damage_type = int(dmg.Type)
        if damage_type in (DamageType.MISS, DamageType.FALL):
            return
        is_heal = damage_type == DamageType.HEAL
        is_immune = damage_type == DamageType.IMMUNE
        is_absorbed = damage_type == DamageType.ABSORBED
        type_flag = dmg.TypeFlag
        value = dmg.Value
        actual_value = dmg.ActualValue
        lucky_value = dmg.LuckyValue
        hp_lessen = dmg.HpLessenValue
        shield_lessen = dmg.ShieldLessenValue
        attacker_uuid_raw = dmg.AttackerUuid
        skill_id = dmg.OwnerId
        is_dead = dmg.IsDead
        element = int(dmg.Property)
        top_summoner = dmg.TopSummonerId

        # Use TopSummonerId as the real attacker if set (summon owner)
        attacker_uuid = top_summoner if top_summoner else attacker_uuid_raw
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

        # ── Track shield depletion via damage events ──
        # When ShieldLessenValue > 0, the target's shield absorbed that much damage.
        # Subtract from the tracked shield_total. If it reaches 0, auto-clear the
        # shield_active flag (handles case where server doesn't send a final
        # AttrShieldList update or SHIELD_BROKEN buff event).
        if target_is_monster and shield_lessen > 0:
            monster = self._monsters.get(target_uuid)
            if monster and monster.shield_active:
                monster.shield_total = max(0, monster.shield_total - int(shield_lessen))
                if monster.shield_total <= 0:
                    monster.shield_active = False
                    monster.shield_total = 0
                    logger.info(
                        f'[Parser] Shield depleted via damage events, '
                        f'auto-clearing shield_active uuid={target_uuid}'
                    )
                    self._notify_monster(monster)

        if self._on_damage:
            try:
                self._on_damage(event)
            except Exception as e:
                logger.debug(f'[Parser] damage callback error: {e}')


    # AttrCollection parsing — Monster


    def _process_monster_attr_collection(self, uuid: int, ac):
        """Decode AttrCollection pb2 object from a monster delta and update MonsterData."""
        if not ac.Attrs:
            return

        monster = self._get_monster(uuid)
        changed = False

        for attr in ac.Attrs:
            attr_id = attr.Id
            raw_data = attr.RawData
            if not raw_data or not attr_id:
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
                    # ── max_hp estimation ──
                    # The game server never sends AttrMaxHp for monsters.
                    # Estimate max_hp from the first HP observation (monster
                    # appears at full health) and track the highest HP seen
                    # to handle healing / regen.
                    if monster.max_hp == 0 and hp > 0:
                        monster.max_hp = hp
                        logger.info(
                            f'[Parser] Monster max_hp estimated from first HP: '
                            f'{hp} uuid={uuid} name={monster.name!r}'
                        )
                        # Save estimated max to cache
                        if monster.template_id > 0:
                            self._monster_hp_cache[monster.template_id] = hp
                    elif hp > monster.max_hp > 0:
                        monster.max_hp = hp  # HP exceeded estimate (healing)
                        logger.info(
                            f'[Parser] Monster max_hp raised to {hp} '
                            f'(healing/regen) uuid={uuid}'
                        )
                    if hp == 0 and monster.max_hp > 0:
                        monster.is_dead = True
                    elif hp > 0 and monster.is_dead:
                        # Auto-revive: same UUID re-used in dungeon retry
                        monster.is_dead = False
                        logger.info(
                            f'[Parser] Monster REVIVED (hp>0 on dead unit) '
                            f'uuid={uuid} hp={hp} name={monster.name!r}'
                        )
                    changed = True
            elif attr_id == AttrType.MAX_HP:
                # Server rarely sends this for monsters, but handle it
                # properly when it does arrive.
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
                    # Estimate max_extinction if server never sent it
                    # (same pattern as the max_hp estimation)
                    if monster.max_extinction == 0 and int_value > 0:
                        monster.max_extinction = int_value
                        logger.info(
                            f'[Parser] Monster max_extinction estimated from first value: '
                            f'{int_value} uuid={uuid}'
                        )
                    elif int_value > monster.max_extinction > 0:
                        monster.max_extinction = int_value  # recovery exceeded old max
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
                    # Estimate max_stunned if server never sent it
                    if monster.max_stunned == 0 and int_value > 0:
                        monster.max_stunned = int_value
                        logger.info(
                            f'[Parser] Monster max_stunned estimated from first value: '
                            f'{int_value} uuid={uuid}'
                        )
                    elif int_value > monster.max_stunned > 0:
                        monster.max_stunned = int_value  # new phase with higher max
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
                    # ── Use aggro name to populate player data ──
                    if monster.hated_char_id > 0:
                        _aggro_player = self._get_player(monster.hated_char_id)
                        if not _aggro_player.name:
                            _aggro_player.name = name
                            logger.info(f'[Parser] Player name from aggro: {name!r} uid={monster.hated_char_id}')
            elif attr_id == AttrType.HATED_CHAR_JOB:
                _job_id = int_value
                # Use aggro job to populate player profession
                if _job_id > 0 and monster.hated_char_id > 0:
                    _aggro_player = self._get_player(monster.hated_char_id)
                    if not _aggro_player.profession_id or _aggro_player.profession_id != _job_id:
                        _aggro_player.profession_id = _job_id
                        _aggro_player.profession = PROFESSION_NAMES.get(_job_id, '')
                        logger.info(f'[Parser] Player profession from aggro: {_job_id} uid={monster.hated_char_id}')
                logger.debug(f'[Parser] Monster aggro job 0x{attr_id:X}={int_value} uuid={uuid}')
            elif attr_id in AttrType._MONSTER_EXTENDED_IDS:
                # Remaining extended monster attrs — logged
                logger.debug(f'[Parser] Monster extended attr 0x{attr_id:X}={int_value} uuid={uuid}')
            elif attr_id == AttrType.HATED_CHAR_LIST:
                # Aggro list — logged
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
            # Final fallback: if max_hp is still 0 but HP is known, use HP as max
            if monster.max_hp == 0 and monster.hp > 0:
                monster.max_hp = monster.hp
                logger.info(
                    f'[Parser] Monster max_hp fallback from HP: '
                    f'{monster.hp} uuid={uuid}'
                )
            monster.last_update = time.time()
            self._notify_monster(monster)
            # Log break-related attrs to packet_debug for diagnosis
            if (monster.max_extinction > 0 or monster.max_stunned > 0
                    or monster.extinction > 0 or monster.stunned > 0
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

    # ── CharTeam parsing — party / team members ──

    def _process_char_team(self, self_uid: int, team_info):
        """Parse CharTeam pb2 message and populate _players + _team_members.

        CharTeam fields:
          TeamId(1), LeaderId(2), TeamTargetId(3), TeamNum(4),
          CharIds(5, repeated int64), IsMatching(6), CharTeamVersion(7),
          TeamMemberData(8, map<int64, TeamMemData>)

        TeamMemData → SocialData(9, TeamMemberSocialData) →
          BasicData(1) → Name(3), Level(6)
          ProfessionData(4) → ProfessionId(1)
          UserAttrData(8) → FightPoint(2)
        """
        team_id = team_info.TeamId
        leader_id = team_info.LeaderId
        char_ids = list(team_info.CharIds)
        member_map = team_info.TeamMemberData  # map<int64, TeamMemData>
        team_num = team_info.TeamNum
        is_matching = team_info.IsMatching
        team_version = team_info.CharTeamVersion

        # ── 详细日志: 原始 CharTeam 字段 ──
        print(
            f'[Parser] CharTeam 原始数据: team_id={team_id} leader={leader_id} '
            f'char_ids={char_ids} member_map_keys={list(member_map.keys())} '
            f'team_num={team_num} matching={is_matching} ver={team_version}',
            flush=True,
        )

        # ── 队伍解散检测 ──
        if not char_ids and not member_map:
            if self._team_id != 0:
                print('[Parser] 队伍已解散 (CharTeam char_ids 为空)', flush=True)
                logger.info('[Parser] Team disbanded (empty CharIds + empty MemberData)')
            self._team_id = 0
            self._team_leader_uid = 0
            self._team_members = {}
            _append_packet_debug('char_team_disband', {
                'self_uid': self_uid,
                'old_team_id': self._team_id,
            })
            return

        # Update team-level state
        self._team_id = team_id
        self._team_leader_uid = leader_id

        # Build new team member cache — preserve existing entries for
        # members still in the team, add new ones, remove departed ones.
        new_team: Dict[int, Dict[str, Any]] = {}
        now_ts = time.time()

        # Add self to team if present in char_ids
        if self_uid > 0 and self_uid in char_ids:
            self_player = self._players.get(self_uid)
            new_team[self_uid] = {
                'uid': self_uid,
                'name': (self_player.name if self_player else '') or '',
                'profession': (self_player.profession if self_player else '') or '',
                'profession_id': (self_player.profession_id if self_player else 0) or 0,
                'fight_point': (self_player.fight_point if self_player else 0) or 0,
                'level': (self_player.level if self_player else 0) or 0,
                'is_self': True,
                'joined_at': self._team_members.get(self_uid, {}).get('joined_at', now_ts),
            }

        members_parsed = []
        _social_missing_uids = []
        for char_id, mem_data in member_map.items():
            if char_id <= 0:
                continue
            if char_id == self_uid:
                continue  # self already added above

            # ── 调试: 输出 TeamMemData 所有字段 ──
            _mem_fields = {
                'CharId': mem_data.CharId,
                'EnterTime': mem_data.EnterTime,
                'CallStatus': mem_data.CallStatus,
                'TalentId': mem_data.TalentId,
                'OnlineStatus': mem_data.OnlineStatus,
                'SceneId': mem_data.SceneId,
                'VoiceIsOpen': mem_data.VoiceIsOpen,
                'GroupId': mem_data.GroupId,
                'HasSocialData': mem_data.HasField('SocialData'),
            }
            logger.info(f'[Parser] TeamMemData uid={char_id}: {_mem_fields}')

            social = mem_data.SocialData if mem_data.HasField('SocialData') else None

            name = ''
            level = 0
            prof_id = 0
            fight_point = 0
            _data_source = 'none'

            if social:
                _has_basic = social.HasField('BasicData')
                _has_prof = social.HasField('ProfessionData')
                _has_attr = social.HasField('UserAttrData')
                logger.info(
                    f'[Parser] TeamMemberSocialData uid={char_id}: '
                    f'HasBasicData={_has_basic} HasProfessionData={_has_prof} '
                    f'HasUserAttrData={_has_attr}'
                )
                if _has_basic:
                    bd = social.BasicData
                    name = bd.Name or ''
                    level = bd.Level
                if _has_prof:
                    pd = social.ProfessionData
                    prof_id = pd.ProfessionId
                if _has_attr:
                    ua = social.UserAttrData
                    fight_point = int(ua.FightPoint)
                _data_source = 'social'
            else:
                _social_missing_uids.append(char_id)

            # ── Fallback: 从 AoI _players 缓存补充缺失字段 ──
            aoi_player = self._players.get(int(char_id))
            if aoi_player:
                if not name and aoi_player.name:
                    name = aoi_player.name
                    _data_source = 'aoi' if _data_source == 'none' else _data_source + '+aoi'
                if level <= 0 and aoi_player.level > 0:
                    level = aoi_player.level
                if prof_id <= 0 and aoi_player.profession_id > 0:
                    prof_id = aoi_player.profession_id
                if fight_point <= 0 and aoi_player.fight_point > 0:
                    fight_point = aoi_player.fight_point

            # ── Fallback: 从上一轮 _team_members 缓存补充 ──
            if not name or fight_point <= 0:
                prev = self._team_members.get(int(char_id), {})
                if not name and prev.get('name'):
                    name = prev['name']
                    _data_source = (_data_source + '+prev') if _data_source != 'none' else 'prev'
                if fight_point <= 0 and prev.get('fight_point', 0) > 0:
                    fight_point = prev['fight_point']
                if level <= 0 and prev.get('level', 0) > 0:
                    level = prev['level']
                if prof_id <= 0 and prev.get('profession_id', 0) > 0:
                    prof_id = prev['profession_id']

            profession_name = PROFESSION_NAMES.get(prof_id, '')

            # Populate PlayerData for this team member
            member_player = self._get_player(int(char_id))
            member_changed = False
            if name and name != member_player.name:
                member_player.name = name
                member_changed = True
            if level > 0 and level != member_player.level:
                member_player.level = level
                member_changed = True
            if prof_id > 0 and prof_id != member_player.profession_id:
                member_player.profession_id = prof_id
                member_player.profession = profession_name
                member_changed = True
            if fight_point > 0 and fight_point != member_player.fight_point:
                member_player.fight_point = fight_point
                member_changed = True

            member_info = {
                'uid': int(char_id),
                'name': name,
                'profession': profession_name,
                'profession_id': prof_id,
                'fight_point': fight_point,
                'level': level,
                'is_self': False,
                'joined_at': self._team_members.get(int(char_id), {}).get('joined_at', now_ts),
                '_data_source': _data_source,
            }
            new_team[int(char_id)] = member_info
            members_parsed.append(member_info)

        # ── Fallback B: char_ids 中有成员不在 member_map 中, 尝试从 AoI 补充 ──
        _missing_from_map = [cid for cid in char_ids
                             if cid != self_uid and cid > 0 and cid not in member_map]
        for cid in _missing_from_map:
            aoi_p = self._players.get(int(cid))
            prev_m = self._team_members.get(int(cid), {})
            _name = (aoi_p.name if aoi_p else '') or prev_m.get('name', '')
            _level = (aoi_p.level if aoi_p else 0) or prev_m.get('level', 0)
            _pid = (aoi_p.profession_id if aoi_p else 0) or prev_m.get('profession_id', 0)
            _fp = (aoi_p.fight_point if aoi_p else 0) or prev_m.get('fight_point', 0)
            member_info = {
                'uid': int(cid),
                'name': _name,
                'profession': PROFESSION_NAMES.get(_pid, ''),
                'profession_id': _pid,
                'fight_point': _fp,
                'level': _level,
                'is_self': False,
                'joined_at': self._team_members.get(int(cid), {}).get('joined_at', now_ts),
                '_data_source': 'aoi_fallback' if aoi_p else ('prev_fallback' if prev_m else 'empty'),
            }
            new_team[int(cid)] = member_info
            members_parsed.append(member_info)
            print(
                f'[Parser] 队伍成员 (AoI/缓存 fallback): uid={cid} name={_name!r} '
                f'fight_point={_fp} lv={_level} prof={_pid} '
                f'aoi_exists={aoi_p is not None} prev_exists={bool(prev_m)}',
                flush=True,
            )

        # Atomically update team cache
        self._team_members = new_team

        # ── 日志: 显示完整队伍信息 ──
        if _social_missing_uids:
            print(
                f'[Parser] CharTeam: {len(_social_missing_uids)} 个成员缺少 SocialData: '
                f'{_social_missing_uids}',
                flush=True,
            )
        if _missing_from_map:
            print(
                f'[Parser] CharTeam: {len(_missing_from_map)} 个 char_ids 不在 TeamMemberData 中: '
                f'{_missing_from_map}',
                flush=True,
            )

        logger.info(
            f'[Parser] CharTeam parsed: team_id={team_id} leader={leader_id} '
            f'总成员={len(new_team)} (含自己) map_keys={len(member_map)} '
            f'char_ids={len(char_ids)} social_missing={len(_social_missing_uids)} '
            f'map_missing={len(_missing_from_map)}'
        )
        for m in members_parsed:
            print(
                f'[Parser] 队伍成员: {m["name"]} (UID={m["uid"]}) '
                f'职业={m["profession"]}({m["profession_id"]}) '
                f'战力={m["fight_point"]} Lv.{m["level"]} '
                f'[来源={m.get("_data_source", "?")}]',
                flush=True,
            )
        _append_packet_debug('char_team', {
            'self_uid': self_uid,
            'team_id': team_id,
            'leader_id': leader_id,
            'char_ids': char_ids,
            'member_map_keys': list(member_map.keys()),
            'social_missing': _social_missing_uids,
            'map_missing': _missing_from_map,
            'members': members_parsed,
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


    def _process_buff_effect_sync(self, host_uuid: int, sync):
        """Decode BuffEffectSync pb2 object (AoiSyncDelta field 11) for boss events."""
        try:
            for be in sync.BuffEffects:
                event_type = int(be.Type)
                if event_type not in _BOSS_BUFF_EVENTS:
                    continue
                buff_uuid = be.BuffUuid
                buff_host = be.HostUuid if be.HostUuid != 0 else host_uuid

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


    def _process_temp_attr_collection(self, uid: int, tac):
        """Process TempAttrCollection pb2 object for CD-related buff modifiers.

        Relevant TempAttr types (from resonance-logs-cn skill_cd_monitor.rs):
          100 = percent CD reduction (万分比, /10000) — cumulative across buffs
          101 = flat CD reduction (ms) — cumulative
          103 = CD acceleration (万分比, /10000) — cumulative
        """
        try:
            if not tac.Attrs:
                return

            player = self._get_player(uid)
            cd_pct = 0
            cd_fixed = 0
            cd_accel = 0

            for attr in tac.Attrs:
                attr_id = attr.Id
                attr_val = attr.Value
                # protobuf int32 is already signed

                if attr_id == 100:
                    cd_pct += attr_val
                elif attr_id == 101:
                    cd_fixed += attr_val
                elif attr_id == 103:
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


    def _process_attr_collection(self, uid: int, ac):
        """Process AttrCollection pb2 object for player attrs."""
        if not ac.Attrs:
            return

        player = self._get_player(uid)
        changed = False
        stamina_max_candidate = 0
        stamina_ratio_values = []

        for attr in ac.Attrs:
            attr_id = attr.Id
            raw_data = attr.RawData
            if not raw_data or not attr_id:
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
                # AttrSeasonLevel (10070) — server-calculated season level (supports up to +105+)
                sl = int_value
                if 0 < sl <= 110:
                    if _set_level_extra_candidate(player, 'season_attr', sl):
                        changed = True
                    logger.info(
                        f'[Parser] AttrCollection AttrSeasonLevel={sl} uid={uid}'
                    )
            elif attr_id == AttrType.SEASON_LV:
                # AttrSeasonLv (196) — per DPS project, treated same as AttrSeasonLevel (supports up to +105+)
                sl = int_value
                if 0 < sl <= 110:
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
