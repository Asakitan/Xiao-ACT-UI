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
_PACKET_DEBUG_ENABLED = False   # 调试写盘已暂时关闭





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
    SYNC_NEAR_ENTITIES = 0x06
    SYNC_CONTAINER_DATA = 0x15
    SYNC_CONTAINER_DIRTY_DATA = 0x16
    SYNC_SERVER_TIME = 0x2B
    SYNC_NEAR_DELTA_INFO = 0x2D
    SYNC_TO_ME_DELTA_INFO = 0x2E


class AttrType:
    NAME = 0x01
    ID = 0x0A
    PROFESSION_ID = 0xDC
    FIGHT_POINT = 0x272E
    LEVEL = 0x2710
    RANK_LEVEL = 0x274C
    CRI = 0x2B66
    LUCKY = 0x2B7A
    HP = 0x2C2E
    MAX_HP = 0x2C38
    ELEMENT_FLAG = 0x646D6C
    REDUCTION_LEVEL = 0x64696D
    ENERGY_FLAG = 0x543CD3C6    # Flag field, not a stamina value
    STA_MAX_FALLBACK = 11324
    STA_RATIO_SET = (11850, 11851, 11852)
    SEASON_LEVEL = 10070       # AttrSeasonLevel (authoritative, from StarResonanceDps)
    SEASON_LV = 196             # AttrSeasonLv (alternate attr)


SERVICE_UUID_C3SB = 0x0000000063335342

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



# UUID helpers


def _is_player(uuid: int) -> bool:
    return (uuid & 0xFFFF) == 640


def _uuid_to_uid(uuid: int) -> int:
    return uuid >> 16






class PlayerData:
    """Tracks one player's parsed data."""
    __slots__ = ('uid', 'name', 'level', 'rank_level', 'season_level',
                 'level_extra', 'level_extra_source',
                 'level_extra_pending_source', 'level_extra_pending_value',
                 'level_extra_pending_hits',
                 'fight_point',
                 'hp', 'max_hp', 'energy', 'energy_limit', 'extra_energy_limit',
                 'energy_info_value', 'energy_valid', 'energy_source_priority',
                 'resource_values', 'energy_info_map', 'stamina_resource_id',
                 'stamina_ratio', 'stamina_ratio_observed_at',
                 'season_medal_level', 'monster_hunt_level',
                 'battlepass_level', 'battlepass_data_level',
                 'profession', 'profession_id',
                 'hp_from_full_sync',
                 'skill_slot_map', 'skill_level_info_map',
                 'slot_bar_map',
                 'skill_cd_map', 'skill_last_use_at', 'skill_seen_ids',
                 'server_time_offset_ms')

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
        self.hp_from_full_sync: bool = False  # Whether HP came from a trusted full sync
        self.skill_slot_map: Dict[int, int] = {}
        self.skill_level_info_map: Dict[int, Dict[str, int]] = {}
        self.slot_bar_map: Dict[int, int] = {}  # CharSerialize.Slots (field 55): slot_id → skill_id
        self.skill_cd_map: Dict[int, Dict[str, Any]] = {}
        self.skill_last_use_at: Dict[int, float] = {}
        self.skill_seen_ids = []
        self.server_time_offset_ms: float = 0.0


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
    """Decode the most likely seasonal level from SeasonMedalInfo (field 52)."""
    if not isinstance(data, bytes) or not data:
        return 0

    medal = _decode_fields(data)
    core_hole_raw = medal.get(3, [None])[0]
    if isinstance(core_hole_raw, bytes):
        core_hole = _decode_fields(core_hole_raw)
        core_level = core_hole.get(2, [None])[0]
        if isinstance(core_level, int) and core_level > 0:
            return _normalize_season_medal_level(core_level)

    normal_levels = []
    for entry_raw in medal.get(2, []):
        if not isinstance(entry_raw, bytes):
            continue
        entry = _decode_fields(entry_raw)
        hole_raw = entry.get(2, [None])[0]
        if not isinstance(hole_raw, bytes):
            continue
        hole = _decode_fields(hole_raw)
        hole_level = hole.get(2, [None])[0]
        if isinstance(hole_level, int) and hole_level > 0:
            normal_levels.append(_normalize_season_medal_level(hole_level))

    return max(normal_levels) if normal_levels else 0


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
    """Normalize raw SeasonMedal level values to the UI-visible seasonal level."""
    if raw_level <= 0:
        return 0
    if raw_level >= 100 and raw_level % 10 == 0:
        return max(0, (raw_level // 10) - 1)
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

    skill_level_id = _varint_to_int32(skill_level_id_raw) if isinstance(skill_level_id_raw, int) else 0
    return {
        'skill_level_id': skill_level_id,
        'begin_time': _varint_to_int64(begin_time_raw) if isinstance(begin_time_raw, int) else 0,
        'duration': _varint_to_int32(duration_raw) if isinstance(duration_raw, int) else 0,
        'skill_cd_type': _varint_to_int32(cd_type_raw) if isinstance(cd_type_raw, int) else 0,
        'valid_cd_time': max(0, valid_cd_time),
        'charge_count': max(0, charge_count),
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
    skill_level_id = _varint_to_int32(skill_level_id_raw) if isinstance(skill_level_id_raw, int) else 0
    return {
        'skill_level_id': skill_level_id,
        'begin_time': _varint_to_int64(begin_time_raw) if isinstance(begin_time_raw, int) else 0,
        'duration': _varint_to_int32(duration_raw) if isinstance(duration_raw, int) else 0,
        'skill_cd_type': _varint_to_int32(cd_type_raw) if isinstance(cd_type_raw, int) else 0,
        'valid_cd_time': _varint_to_int32(valid_cd_time_raw) if isinstance(valid_cd_time_raw, int) else 0,
        'charge_count': _varint_to_int32(charge_count_raw) if isinstance(charge_count_raw, int) else 0,
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
    'season_attr': 100,   # AttrSeasonLevel (10070) / AttrSeasonLv (196) — authoritative
    'season_medal': 50,   # SeasonMedalInfo CoreHole from CharSerialize field 52
    'monster_hunt': 10,   # MonsterHuntInfo CurLevel from CharSerialize field 56
    'battlepass': 5,
    'battlepass_data': 3,
}

# Sources that are reliable enough to commit on first observation (no 2-hit)
_TRUSTED_LEVEL_SOURCES = frozenset({'season_attr', 'season_medal'})


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

    def __init__(self, on_self_update: Callable[[PlayerData], None], preferred_uid: int = 0):
        self._on_update = on_self_update
        self._current_uuid: int = 0   # Current player UUID
        self._current_uid: int = max(0, int(preferred_uid))    # Current player UID (uuid >> 16)
        self._players: Dict[int, PlayerData] = {}  # uid -> PlayerData
        self._profession_skill_cache: Dict[int, Dict[int, int]] = {}  # profession_id -> slot map
        self._zstd = None
        self._server_time_offset_ms: float = 0.0
        self.stats = {
            'raw_frames': 0,
            'game_frames': 0,
            'unknown_message_types': 0,
            'unknown_notify_methods': 0,
            'zstd_failures': 0,
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
                'observed_at_ms': observed_at_ms,
                'source': 'full_sync',
            }
            prev = previous.get(skill_level_id)
            if prev and prev.get('begin_time') == normalized[skill_level_id].get('begin_time'):
                normalized[skill_level_id]['observed_at_ms'] = prev.get('observed_at_ms', observed_at_ms)

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
            'observed_at_ms': observed_at_ms,
            'source': 'delta',
        }
        prev = player.skill_cd_map.get(skill_level_id)
        if prev:
            same_core = (
                prev.get('begin_time') == new_entry['begin_time'] and
                prev.get('duration') == new_entry['duration'] and
                prev.get('valid_cd_time') == new_entry['valid_cd_time'] and
                prev.get('skill_cd_type') == new_entry['skill_cd_type'] and
                prev.get('charge_count') == new_entry['charge_count']
            )
            if same_core:
                new_entry['observed_at_ms'] = prev.get('observed_at_ms', observed_at_ms)
                return False
            if new_entry['begin_time'] != prev.get('begin_time'):
                # Only treat a *new* begin_time as a fresh skill cast.
                # valid_cd_time increases naturally during CD progress and
                # must NOT reset last_use_at, otherwise the 'active' state
                # flickers on every server delta update.
                player.skill_last_use_at[skill_level_id] = time.time()
        else:
            player.skill_last_use_at[skill_level_id] = time.time()

        player.skill_cd_map[skill_level_id] = new_entry
        return True





    def process_packet(self, frame: bytes):
        """Process one framed packet: `[4B size][2B type][payload]`."""
        if len(frame) < 6:
            return
        self.stats['raw_frames'] += 1
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
                logger.debug(f'[Parser] message handling error (type={msg_type}): {e}')


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
        else:
            self.stats['unknown_notify_methods'] += 1


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
            if isinstance(role_level, int) and role_level > 0 and player.level <= 0:
                # Upstream packet.js treats RoleLevel.Level like the visible level.
                player.level = role_level
                changed = True

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

        if changed:
            if uid == self._current_uid:
                self._notify_self()


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
                player.max_hp = max_hp
                changed = True
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

        if field_index in (16, 22, 50, 52, 56, 61, 86):
            _append_packet_debug('dirty_update', debug_info)
        if changed:
            self._notify_self()


    # SyncNearEntities (0x06)


    def _on_sync_near_entities(self, data: bytes):
        outer = _decode_fields(data)
        # Appear (field 1, repeated)
        for entity_raw in outer.get(1, []):
            if not isinstance(entity_raw, bytes):
                continue
            ef = _decode_fields(entity_raw)
            uuid_raw = ef.get(1, [0])[0]
            if not isinstance(uuid_raw, int) or uuid_raw == 0:
                continue
            uuid = _varint_to_int64(uuid_raw)
            ent_type = ef.get(2, [0])[0]

            if not _is_player(uuid):
                continue  # Skip non-player entities.
            uid = _uuid_to_uid(uuid)

            # AttrCollection (field 3)
            attr_raw = ef.get(3, [None])[0]
            if isinstance(attr_raw, bytes):
                self._process_attr_collection(uid, attr_raw)


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
                if decoded_fight_cd.get('res_id', 0) > 0:
                    fight_res_cds.append(decoded_fight_cd)
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
        if not _is_player(uuid):
            return
        uid = _uuid_to_uid(uuid)

        # Attrs (field 2)
        attr_raw = df.get(2, [None])[0]
        if isinstance(attr_raw, bytes):
            self._process_attr_collection(uid, attr_raw)


    # AttrCollection parsing


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
            elif attr_id in (AttrType.SEASON_LEVEL, AttrType.SEASON_LV):
                # Authoritative server-calculated season level
                sl = int_value
                if 0 < sl <= 60:
                    if _set_level_extra_candidate(player, 'season_attr', sl):
                        changed = True
                    logger.info(
                        f'[Parser] AttrCollection AttrSeasonLevel={sl} '
                        f'(attr_id=0x{attr_id:X}) uid={uid}'
                    )
            elif attr_id in (AttrType.CRI, AttrType.LUCKY, AttrType.ELEMENT_FLAG,
                             AttrType.REDUCTION_LEVEL, AttrType.ID):
                pass
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
            logger.debug(f'[Parser] zstd decompress failed: {e}')
            _append_packet_debug('zstd_failure', {
                'error': str(e),
                'input_size': len(data or b''),
            })
            return None
