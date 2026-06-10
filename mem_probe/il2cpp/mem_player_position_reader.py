# -*- coding: utf-8 -*-
"""mem_player_position_reader - 玩家/实体世界坐标读取 (read-only, auto-offset).

链路 (全部按名解析, dump/literal 兜底, 补丁自愈):
  ZEntity.moveModuleComp_   -> MoveModuleComp*           (+0x88 fallback)
  MoveModuleComp.lastPosition_ -> Vector3 (3×float32)    (+0x98 fallback)

活体实证 (game 2022.3.59f1 / IL2CPP): PlayerEnt.moveModuleComp_ 指向 MoveModuleComp,
其 lastPosition_ = (x, y_height, z), 与 ZStateMoveComp.oldGoPosition_ 同值互证。

坐标系: X/Z 为水平面, Y 为高度。两点水平距离 = hypot(dx, dz)。

用法:
    rd = PlayerPositionReader(dps_source)
    pos = rd.read_entity_pos(entity_obj_addr)   # -> (x, y, z) 或 None
"""
from __future__ import annotations

import math
import struct
from typing import Dict, Optional, Tuple

from mem_probe.il2cpp.live_field_resolver import LiveFieldResolver

ENTITY_CLASS = "Panda.ZGame.ZEntity"
MOVEMOD_CLASS = "MoveModuleComp"

# 活体实证 fallback (auto-offset 解析失败时用)
ENT_MOVEMOD_OFF = 0x88        # ZEntity.moveModuleComp_
MOVEMOD_LASTPOS_OFF = 0x98    # MoveModuleComp.lastPosition_ (Vector3)

_MIN_PTR = 0x10000
_MAX_PTR = 0x7FFF_FFFF_FFFF

Vec3 = Tuple[float, float, float]


def horiz_dist(a: Vec3, b: Vec3) -> float:
    """水平面 (X/Z) 距离, 忽略高度 Y。"""
    return math.hypot(a[0] - b[0], a[2] - b[2])


class PlayerPositionReader:
    """实体世界坐标读取 (玩家/boss/队友通用), 偏移按名 auto-offset 一次后缓存。"""

    def __init__(self, dps_source):
        self._src = dps_source
        self._pm = dps_source.sr.pm
        self._lfr = LiveFieldResolver(
            self._pm, klass_resolver=getattr(dps_source.sr, "resolve_klass", None))
        self._off: Dict[str, int] = {}

    def _resolve(self) -> Dict[str, int]:
        if self._off:
            return self._off
        ent_mm = self._lfr.field_offset(ENTITY_CLASS, "moveModuleComp_")
        mm_pos = self._lfr.field_offset(MOVEMOD_CLASS, "lastPosition_")
        self._off = {
            "ent_movemod": int(ent_mm) if ent_mm else ENT_MOVEMOD_OFF,
            "movemod_pos": int(mm_pos) if mm_pos else MOVEMOD_LASTPOS_OFF,
        }
        return self._off

    def _read_vec3(self, addr: int) -> Optional[Vec3]:
        if not (_MIN_PTR <= addr <= _MAX_PTR):
            return None
        b = self._pm.read_bytes(addr, 12)
        if not b or len(b) != 12:
            return None
        x, y, z = struct.unpack("<3f", b)
        # NaN / 离谱值过滤 (世界坐标在 ±1e5 内)
        for v in (x, y, z):
            if v != v or abs(v) > 1e5:
                return None
        return (x, y, z)

    def read_entity_pos(self, entity_obj: int) -> Optional[Vec3]:
        """读一个 ZEntity 对象的当前世界坐标 (x, y_height, z)。"""
        if not (_MIN_PTR <= (entity_obj or 0) <= _MAX_PTR):
            return None
        off = self._resolve()
        mm = self._pm.read_u64(entity_obj + off["ent_movemod"]) or 0
        if not (_MIN_PTR <= mm <= _MAX_PTR):
            return None
        return self._read_vec3(mm + off["movemod_pos"])


__all__ = ["PlayerPositionReader", "horiz_dist", "Vec3"]
