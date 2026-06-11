# -*- coding: utf-8 -*-
"""mem_zone_reader - 读 AOE 危险区(ZoneEnt)的**成员判定**, 精准出圈零误差.

★精确不靠估算半径(主人: 范围错了会害死人): 服务端自己在 ZoneComp.entitiesIdInZone_
(ZList<long uuid>) 里维护"谁在这个区域内"——game 自己说玩家在不在圈里。精准出圈=
玩家 uuid 从危险区成员列表消失即停, 而不是算半径+距离(半径在配置/native里逆不干净)。

链路(全 auto-offset, 用活对象 klass 字段表): ZoneEnt(zoneDict_) → compList_ 里 ZoneComp
→ entitiesIdInZone_(ZList) → items_/size_ → long uuid[]; ZoneEnt.zoneType_ 分危险/安全。
实测: 玩家站区域内时其 uuid 确在 entitiesIdInZone_ 列表里(size=1 ids=[player_uuid])。
"""
from __future__ import annotations

from typing import Dict, List, Optional, Set

_MINP, _MAXP = 0x10000, 0x7FFF_FFFF_FFFF

ZONE_DICT_OFF = 0xA8 - 0x18      # ZEntityMgr.zoneDict_ 实测 0x90
ENT_COMPLIST_OFF = 0x60         # ZEntity.compList_ (ZComponent[])
ENT_ZONETYPE_OFF = 0x118        # ZoneEnt.zoneType_ (fallback)
ENT_UUID_OFF = 0xC0             # ZEntity.Uuid (fallback)
ZONECOMP_ENTSINZONE_OFF = 0x70  # ZoneComp.entitiesIdInZone_ (fallback, auto-offset 优先)
# ZList<T> 布局: 前有 recyclePooledObj_(bool), items_@0x18, size_@0x20
ZLIST_ITEMS_OFF = 0x18
ZLIST_SIZE_OFF = 0x20
ARRAY_ELEMS_OFF = 0x20          # 托管数组首元素偏移


class ZoneReader:
    def __init__(self, dps_source):
        self._src = dps_source
        self._pm = dps_source.sr.pm
        self._zonecomp_klass = 0
        self._off_entsinzone = 0
        self._off_uuid = 0

    def _kname(self, obj: int) -> str:
        try:
            kp = self._pm.read_u64(obj)
            np = self._pm.read_u64(kp + 0x10)
            b = self._pm.read_bytes(np, 64)
            s = b.split(b"\x00", 1)[0]
            return s.decode("ascii", "replace") if s else ""
        except Exception:
            return ""

    def _resolve_entsinzone_off(self, zonecomp: int) -> int:
        """从活 ZoneComp 对象的 klass 字段表解 entitiesIdInZone_ 偏移 (auto-offset)。"""
        if self._off_entsinzone:
            return self._off_entsinzone
        try:
            from mem_probe.il2cpp.live_field_resolver import LiveFieldResolver
            kp = self._pm.read_u64(zonecomp)
            lfr = LiveFieldResolver(self._pm)
            fmap = lfr._field_map(kp)
            off = fmap.get("entitiesIdInZone_")
            self._off_entsinzone = int(off) if off else ZONECOMP_ENTSINZONE_OFF
        except Exception:
            self._off_entsinzone = ZONECOMP_ENTSINZONE_OFF
        return self._off_entsinzone

    def _zone_comp(self, zone_obj: int) -> int:
        cl = self._pm.read_u64(zone_obj + ENT_COMPLIST_OFF) or 0
        if not (_MINP <= cl <= _MAXP):
            return 0
        for i in range(16):
            cp = self._pm.read_u64(cl + ARRAY_ELEMS_OFF + i * 8) or 0
            if _MINP <= cp <= _MAXP and self._kname(cp) == "ZoneComp":
                return cp
        return 0

    def _read_member_uuids(self, zonecomp: int) -> List[int]:
        off = self._resolve_entsinzone_off(zonecomp)
        zlist = self._pm.read_u64(zonecomp + off) or 0
        if not (_MINP <= zlist <= _MAXP):
            return []
        items = self._pm.read_u64(zlist + ZLIST_ITEMS_OFF) or 0
        size = self._pm.read_i32(zlist + ZLIST_SIZE_OFF) or 0
        if not (_MINP <= items <= _MAXP) or not (0 <= size <= 256):
            return []
        out = []
        for j in range(min(size, 64)):
            v = self._pm.read_i64(items + ARRAY_ELEMS_OFF + j * 8)
            if v:
                out.append(int(v))
        return out

    def zones_containing(self, mgr_addr: int, player_uuid: int,
                         zone_dict_off: int = 0x90) -> List[Dict]:
        """返回玩家当前所在的全部区域 [{zone_uuid, zone_type, members_n}]。"""
        out = []
        try:
            from mem_probe.il2cpp.mem_entity_mgr import EntityMgrReader
            emr = EntityMgrReader(self._src)
            d = self._pm.read_u64(mgr_addr + zone_dict_off) or 0
            for key, zone in emr._read_dict_entries(d, max_entries=64):
                zc = self._zone_comp(zone)
                if not zc:
                    continue
                members = self._read_member_uuids(zc)
                if player_uuid in members:
                    zt = self._pm.read_i32(zone + ENT_ZONETYPE_OFF)
                    out.append({"zone_uuid": int(key), "zone_obj": int(zone),
                                "zone_type": zt, "members_n": len(members)})
        except Exception:
            pass
        return out

    def player_zone_uuids(self, mgr_addr: int, player_uuid: int,
                          zone_dict_off: int = 0x90) -> Set[int]:
        return {z["zone_uuid"] for z in
                self.zones_containing(mgr_addr, player_uuid, zone_dict_off)}


__all__ = ["ZoneReader"]
