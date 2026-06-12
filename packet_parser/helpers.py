# -*- coding: utf-8 -*-
"""Lazy loaders, Cython bindings and low-level decode/level helpers
extracted from packet_parser (physical split).

`from __future__ import annotations` keeps the ``player: PlayerData`` type
hints as strings so this module needs no runtime import of data.py, which
would otherwise form an import cycle (data.py -> helpers._uuid_to_uid).
"""

from __future__ import annotations

import os
import json
import time
import logging
from typing import Optional, Dict, Any

from packet_parser.enums import AttrType
# NOTE: PlayerData appears only in this module's type hints (player: PlayerData).
# `from __future__ import annotations` above keeps those hints as strings, so we
# deliberately do NOT import data.py here — that would create an import cycle
# (data.py imports helpers._uuid_to_uid at module load time).


logger = logging.getLogger('sao_auto.parser')
_PACKET_DEBUG_ENABLED = False  # Enable to log raw packet snapshots for field confirmation

from utils.perf_probe import probe as _probe

import _sao_cy_combat as _CY_COMBAT  # type: ignore[import-not-found]
import _sao_cy_packet as _CY_PACKET  # type: ignore[import-not-found]

# Lazy import for protobuf JSON conversion (used in full sync dump)
_MessageToDict = None





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
    global _pb, _pb_loaded, _MessageToDict
    if _pb_loaded:
        return _pb
    _pb_loaded = True

    try:
        from proto import star_resonance_pb2
        _pb = star_resonance_pb2
        logger.info('[Parser] using compiled protobuf')
    except ImportError:
        pass

    try:
        from google.protobuf.json_format import MessageToDict
        _MessageToDict = MessageToDict
    except ImportError:
        pass

    if _pb is not None:
        return _pb

    logger.info('[Parser] using built-in mini protobuf decoder')
    return None



# Mini protobuf helpers — mandatory Cython bindings, no Python fallback.


_read_varint = _CY_PACKET.read_varint
_read_signed_varint = _CY_PACKET.read_signed_varint
_decode_fields = _CY_PACKET.decode_fields


_parse_dungeon_dirty_buffer = _CY_PACKET.parse_dungeon_dirty_buffer




_varint_to_int64 = _CY_PACKET.varint_to_int64
_varint_to_int32 = _CY_PACKET.varint_to_int32
_decode_string_from_raw = _CY_PACKET.decode_string_from_raw
_decode_int32_from_raw = _CY_PACKET.raw_varint_to_int32
_decode_float32_from_raw = _CY_PACKET.decode_float32_from_raw
# v2.4.31: dirty-stream header helper (collapses 0xFFFFFFFE marker + sub_field
# read into one cython call across CharBase/UserFightAttr/RoleLevel/...).
_parse_dirty_subfield_header = _CY_PACKET.parse_dirty_subfield_header
_try_read_le_u32_at = _CY_PACKET.try_read_le_u32_at


_DIRTY_CHARBASE_FIELDS: Dict[int, tuple] = {
    1:  ('CharId', 'Q'),
    2:  ('AccountId', 'str'),
    3:  ('ShowId', 'Q'),
    4:  ('ServerId', 'I'),
    5:  ('Name', 'str'),
    6:  ('Gender', 'I'),
    7:  ('IsDeleted', 'B'),
    8:  ('IsForbid', 'B'),
    9:  ('IsMute', 'B'),
    10: ('X', 'f'),
    11: ('Y', 'f'),
    12: ('Z', 'f'),
    13: ('Dir', 'f'),
    14: ('FaceData', 'msg'),
    15: ('CardId', 'I'),
    16: ('CreateTime', 'Q'),
    17: ('OnlineTime', 'Q'),
    18: ('OfflineTime', 'Q'),
    19: ('ProfileInfo', 'msg'),
    20: ('TeamInfo', 'msg'),
    21: ('CharState', 'Q'),
    22: ('BodySize', 'I'),
    23: ('UnionInfo', 'msg'),
    24: ('PersonalState', 'rep_i32'),
    25: ('AvatarInfo', 'msg'),
    26: ('TotalOnlineTime', 'Q'),
    27: ('OpenId', 'str'),
    28: ('SdkType', 'I'),
    29: ('Os', 'I'),
    31: ('InitProfessionId', 'I'),
    32: ('LastCalTotalTime', 'Q'),
    33: ('AreaId', 'I'),
    34: ('ClientVersion', 'str'),
    35: ('FightPoint', 'I'),
    36: ('SumSave', 'Q'),
    37: ('ClientResourceVersion', 'str'),
    38: ('LastOfflineTime', 'Q'),
    39: ('DayAccDurTime', 'I'),
    40: ('LastAccDurTimestamp', 'Q'),
    41: ('SaveSerial', 'Q'),
}


# ── 全量 TCP notify dump (诊断/回归分析用) ──────────────────────────────
# 把每个解压后的游戏 notify (method_id + 原始 hex) 以及未识别的顶层帧写到
# sao_auto/tcp_dump.jsonl, 供离线解码分析 (找伤害/场景包到底走了哪个 method)。
# 抓完一段后请把 _TCP_DUMP_ENABLED 改回 False (避免持续写盘 / 体积膨胀)。
_TCP_DUMP_ENABLED = False  # 诊断开关: 抓包分析时临时改 True (写 tcp_dump.jsonl)
_TCP_DUMP_MAX_BYTES = 150_000_000  # ~150MB 上限, 防跑飞
_tcp_dump_bytes = 0
_tcp_dump_printed = False


def _dump_tcp(kind: str, **fields):
    """Append one raw TCP frame record to sao_auto/tcp_dump.jsonl (offline analysis).

    kind='notify'        — 解压后的游戏 notify: mid(method_id)/name/zstd/n(len)/hex
    kind='unknown_msgtype' — 顶层消息类型既非 NOTIFY 也非 FRAME_DOWN 的原始帧
    """
    global _tcp_dump_bytes, _tcp_dump_printed
    if not _TCP_DUMP_ENABLED:
        return
    try:
        path = os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            'tcp_dump.jsonl')
        if not _tcp_dump_printed:
            _tcp_dump_printed = True
            print(f'[TCP-DUMP] 已开启 → {path} '
                  f'(抓完把 helpers._TCP_DUMP_ENABLED 改回 False)', flush=True)
        if _tcp_dump_bytes > _TCP_DUMP_MAX_BYTES:
            return
        row = {'ts': round(time.time(), 3), 'kind': kind, **fields}
        line = json.dumps(row, ensure_ascii=False) + '\n'
        _tcp_dump_bytes += len(line)
        with open(path, 'a', encoding='utf-8') as f:
            f.write(line)
    except Exception:
        pass


def _append_packet_debug(tag: str, payload: Dict[str, Any]):
    """Append a small packet debug snapshot for later field confirmation."""
    if not _PACKET_DEBUG_ENABLED:
        return
    try:
        # parser package lives one level deeper now; keep debug output in the
        # original sao_auto/ directory (was os.path.dirname(__file__)).
        debug_path = os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            'packet_debug.jsonl')
        row = {
            'ts': round(time.time(), 3),
            'tag': tag,
            **payload,
        }
        with open(debug_path, 'a', encoding='utf-8') as f:
            f.write(json.dumps(row, ensure_ascii=False) + '\n')
    except Exception:
        pass


# UUID helpers


def _is_player(uuid: int) -> bool:
    return bool(_CY_COMBAT.is_player_uuid(uuid))


def _is_monster(uuid: int) -> bool:
    return bool(_CY_COMBAT.is_monster_uuid(uuid))


_MONSTER_HINT_ATTR_IDS = (
    frozenset({
        AttrType.ID,
        AttrType.HP,
        AttrType.MAX_HP,
        AttrType.MONSTER_SEASON_LEVEL,
        AttrType.MAX_EXTINCTION,
        AttrType.EXTINCTION,
        AttrType.MAX_STUNNED,
        AttrType.STUNNED,
        AttrType.IN_OVERDRIVE,
        AttrType.IS_LOCK_STUNNED,
        AttrType.STOP_BREAKING_TICKING,
        AttrType.BREAKING_STAGE,
        AttrType.SHIELD_LIST,
    })
    | AttrType._MONSTER_EXTENDED_IDS
)


def _attrs_look_monster_like(ac) -> bool:
    """Return True for non-player entities carrying monster/combat HP attrs.

    v2.4.33: pb2 decoding stays in Python (the AttrCollection wrapper is a
    Python-only proto object), but the membership test is in cython now.
    """
    if not ac or not getattr(ac, 'Attrs', None):
        return False
    attr_ids: list = []
    for attr in ac.Attrs:
        try:
            attr_ids.append(int(attr.Id or 0))
        except Exception:
            continue
    return bool(_CY_PACKET.attrs_match_monster_hint(attr_ids, _MONSTER_HINT_ATTR_IDS))


def _combat_damage_amount(value, lucky_value, actual_value, hp_lessen, shield_lessen) -> int:
    """Resolve display/stat damage using the same broad fallbacks as upstream counters."""
    return int(_CY_COMBAT.combat_damage_amount(
        value, lucky_value, actual_value, hp_lessen, shield_lessen))


def _compute_damage_key(owner_id: int, damage_source: int, owner_level: int,
                        hit_event_id: int) -> int:
    return int(_CY_PACKET.compute_damage_key(owner_id, damage_source, owner_level, hit_event_id))


def _uuid_to_uid(uuid: int) -> int:
    return int(_CY_COMBAT.uuid_to_uid(uuid))



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
    """Pick the sane representation from dirty-stream energy payload.

    v2.4.33: cython.
    """
    return _CY_PACKET.decode_dirty_energy_value(raw_u32, raw_f32, stamina_max)


def _is_sane_attr_stamina_max(value: int) -> bool:
    # Current observed self STA caps stay around 1200. Values like 1350/1500
    # and the larger 2100/3100 spikes are not stable enough to trust.
    return bool(_CY_PACKET.is_sane_attr_stamina_max(value))


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
    """v2.4.33: cython."""
    return int(_CY_PACKET.level_extra_source_priority(source))


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

    v2.4.33: cython.
    """
    return int(_CY_PACKET.normalize_season_medal_level(raw_level))


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


def _decode_resource_value_map(resource_ids, resources) -> Dict[int, int]:
    return _CY_PACKET.decode_resource_value_map(resource_ids, resources)


def _decode_packed_varints(raw: bytes) -> list:
    return _CY_PACKET.decode_packed_varints(raw or b'')


def _extract_scene_basic_id_from_attrs(attrs) -> int:
    if not attrs or not getattr(attrs, 'Attrs', None):
        return 0
    for attr in attrs.Attrs:
        try:
            attr_id = int(attr.Id or 0)
        except Exception:
            continue
        if attr_id != AttrType.SCENE_BASIC_ID:
            continue
        raw_data = getattr(attr, 'RawData', b'') or b''
        if not raw_data:
            continue
        scene_id = _decode_int32_from_raw(raw_data)
        if scene_id > 0:
            return scene_id
    return 0


def _pick_stamina_resource_id(player: PlayerData) -> int:
    resource_values = getattr(player, 'resource_values', {}) or {}
    energy_info_map = getattr(player, 'energy_info_map', {}) or {}
    total_limit = max(0, int(getattr(player, 'energy_limit', 0) or 0))
    total_limit += max(0, int(getattr(player, 'extra_energy_limit', 0) or 0))
    return int(_CY_PACKET.pick_stamina_resource_id(
        resource_values, energy_info_map, total_limit))


def _refresh_stamina_resource(player: PlayerData) -> bool:
    picked_resource_id = _pick_stamina_resource_id(player)
    if picked_resource_id <= 0 or picked_resource_id == int(getattr(player, 'stamina_resource_id', 0) or 0):
        return False
    player.stamina_resource_id = picked_resource_id
    return True
