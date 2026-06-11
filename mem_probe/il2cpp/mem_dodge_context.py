# -*- coding: utf-8 -*-
"""mem_dodge_context - 定向躲避的活体上下文 (相机基 + 玩家位 + boss 危险位).

把三个已逆向的只读读取器收口成 AutoDodgeDirector 需要的回调, 全部节流 + 懒定位,
不在热路径反复堆扫:
  - 相机前向/右向世界 XZ 基   (mem_camera_reader.CameraReader, 定位后缓存)
  - 本地玩家世界坐标          (mem_player_position_reader, O(1))
  - boss 危险位 (best-effort)  boss 远程同步, lastPosition_ 可能为 0 → 返回 None 让
                              director 降级到后备相机相对方向 (不假装能读 boss 位)

只读, 与 ACE 兼容 (不拦读)。相机基 0.2s 节流, 玩家/boss 位每次直读 (O(1))。
"""
from __future__ import annotations

import time
from typing import Optional, Tuple

Vec3 = Tuple[float, float, float]


class DodgeContext:
    def __init__(self, dps_source):
        self._src = dps_source
        self._cam = None
        self._pos = None
        self._emr = None
        self._player_obj = 0
        self._cam_cache = None
        self._cam_ts = 0.0
        self._cam_ttl = 0.2

    # ---- lazy readers ----
    def _camera(self):
        if self._cam is None:
            from mem_probe.il2cpp.mem_camera_reader import CameraReader
            self._cam = CameraReader(self._src)
        return self._cam

    def _position(self):
        if self._pos is None:
            from mem_probe.il2cpp.mem_player_position_reader import PlayerPositionReader
            self._pos = PlayerPositionReader(self._src)
        return self._pos

    def _entity_mgr(self):
        if self._emr is None:
            from mem_probe.il2cpp.mem_entity_mgr import EntityMgrReader
            self._emr = EntityMgrReader(self._src)
        return self._emr

    def _player(self) -> int:
        """玩家实体对象 (ZEntityMgr.playerEnt_ @ +0x18), 缓存 + 失效重取。"""
        pm = self._src.sr.pm
        if self._player_obj:
            try:
                if pm.read_u64(self._player_obj):   # 仍可读
                    return self._player_obj
            except Exception:
                pass
        try:
            mgr = self._entity_mgr().locate(0)
            if mgr:
                self._player_obj = pm.read_u64(mgr + 0x18) or 0
        except Exception:
            self._player_obj = 0
        return self._player_obj

    # ---- director callbacks ----
    def get_cam_basis(self) -> Optional[dict]:
        now = time.time()
        if self._cam_cache is not None and (now - self._cam_ts) < self._cam_ttl:
            return self._cam_cache
        try:
            # 传玩家位 → 相机读取器用 RawPosition 锚定稳健定位朝向四元数(任意 yaw/360°)
            self._cam_cache = self._camera().read_basis(self.get_player_pos())
        except Exception:
            self._cam_cache = None
        self._cam_ts = now
        return self._cam_cache

    def get_player_pos(self) -> Optional[Vec3]:
        try:
            p = self._player()
            return self._position().read_entity_pos(p) if p else None
        except Exception:
            return None

    def prewarm(self) -> None:
        """后台预热: 把相机/实体管理器/玩家的冷定位堆扫(各~7s)挡在闭环外, 之后所有
        读都走缓存 O(1)。在 director 创建时起守护线程调一次即可。"""
        try:
            self.get_cam_basis()
        except Exception:
            pass
        try:
            self._entity_mgr().locate(0)
            self.get_player_pos()
        except Exception:
            pass

    def read_obj_pos(self, obj: int) -> Optional[Vec3]:
        """O(1) 读一个已知实体对象的世界坐标 (闭环精准出圈每 tick 调)。"""
        try:
            return self._position().read_entity_pos(obj) if obj else None
        except Exception:
            return None

    def _zone(self):
        if getattr(self, "_zone_reader", None) is None:
            from mem_probe.il2cpp.mem_zone_reader import ZoneReader
            self._zone_reader = ZoneReader(self._src)
        return self._zone_reader

    def make_zone_exit_check(self):
        """精准出圈(零误差): 用游戏自己的区域成员判定(ZoneComp.entitiesIdInZone_)。
        返回 is_clear() 回调——dodge 开始时快照玩家所在的全部区域(含 AOE 圈), 之后玩家
        从其中任一区域消失即视为'出圈'(离开了一个 AOE), 返回 True。不靠估算半径。
        玩家定位失败/无区域时返回的 is_clear 恒 False, 让闭环退到 exit_margin 距离兜底。"""
        try:
            mgr = self._entity_mgr().locate(0)
            pm = self._src.sr.pm
            player = pm.read_u64(mgr + 0x18) if mgr else 0
            puuid = pm.read_i64(player + 0xC0) if player else 0
            if not mgr or not puuid:
                return lambda: False
            zr = self._zone()
            start = zr.player_zone_uuids(mgr, puuid)
            if not start:
                return lambda: False     # 没在任何区域 → 无精确信号, 走距离兜底

            def _is_clear():
                try:
                    cur = zr.player_zone_uuids(mgr, puuid)
                    return bool(start - cur)   # 离开了≥1个起始区域=出了一个圈
                except Exception:
                    return False
            return _is_clear
        except Exception:
            return lambda: False

    def lock_danger_obj(self, direction: str, boss_base_id: int = 0) -> int:
        """进闭环前定位一次危险源实体对象地址 (O(N) 一次), 之后 read_obj_pos O(1)。
        away_boss→按 base_id 找 boss; away_nearest→最近敌对实体。失败返回 0。"""
        try:
            import math
            snap = self._entity_mgr().read(player_uuid=0, include_monsters=True)
            if not snap:
                return 0
            rd = self._position()
            cands = list(snap.bosses) + list(snap.monsters)
            if str(direction) == "away_boss":
                for es in snap.bosses:
                    if not boss_base_id or int(es.config_uuid or 0) == int(boss_base_id):
                        pos = rd.read_entity_pos(es.obj_addr)
                        if pos and any(abs(c) > 1e-4 for c in pos):
                            return es.obj_addr
                cands = list(snap.bosses) or cands
            pp = self.get_player_pos()
            best, best_d = 0, 1e18
            for es in cands:
                pos = rd.read_entity_pos(es.obj_addr)
                if not pos or not any(abs(c) > 1e-4 for c in pos):
                    continue
                if not pp:
                    return es.obj_addr
                dd = math.hypot(pp[0] - pos[0], pp[2] - pos[2])
                if 0.5 < dd < best_d:
                    best_d, best = dd, es.obj_addr
            return best
        except Exception:
            return 0

    def get_nearest_danger_pos(self) -> Optional[Vec3]:
        """最近敌对实体(boss/怪/召唤)世界坐标 — 远离最近威胁躲避用。

        boss/怪走 stateMoveComp_.oldGoPosition_ 干净可读 (战斗召唤的 marker 怪
        3000xxx 也在内); ZoneEnt/DummyEnt 的 AOE 圈在 ECS/native 无稳定偏移, 不计入。"""
        try:
            import math
            snap = self._entity_mgr().read(player_uuid=0, include_monsters=True)
            if not snap:
                return None
            pp = self.get_player_pos()
            if not pp:
                return None
            rd = self._position()
            best = None
            best_d = 1e18
            for es in list(snap.bosses) + list(snap.monsters):
                pos = rd.read_entity_pos(es.obj_addr)
                if not pos or not any(abs(c) > 1e-4 for c in pos):
                    continue
                dd = math.hypot(pp[0] - pos[0], pp[2] - pos[2])
                if 0.5 < dd < best_d:    # 排除站在自己身上的
                    best_d = dd
                    best = pos
            return best
        except Exception:
            return None

    def get_boss_pos(self, boss_base_id: int = 0) -> Optional[Vec3]:
        """best-effort boss 世界坐标。远程同步常不写 lastPosition_ → 可能 None。"""
        try:
            snap = self._entity_mgr().read(player_uuid=0)
            if not snap or not snap.bosses:
                return None
            rd = self._position()
            for es in snap.bosses:
                if boss_base_id and int(es.config_uuid or 0) != int(boss_base_id):
                    continue
                pos = rd.read_entity_pos(es.obj_addr)
                if pos and any(abs(c) > 1e-4 for c in pos):
                    return pos
            # 无指定 boss 或都读 0: 取第一个非零
            for es in snap.bosses:
                pos = rd.read_entity_pos(es.obj_addr)
                if pos and any(abs(c) > 1e-4 for c in pos):
                    return pos
        except Exception:
            pass
        return None


__all__ = ["DodgeContext"]
