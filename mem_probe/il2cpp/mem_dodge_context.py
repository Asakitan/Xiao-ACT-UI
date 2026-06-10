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
            self._cam_cache = self._camera().read_basis()
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
