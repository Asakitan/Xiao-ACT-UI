# -*- coding: utf-8 -*-
"""mem_camera_reader - 读 gameplay 相机朝向 (read-only, auto-offset).

WASD 是相机相对的: 按 W 角色朝"相机前向投影到地面"移动。要把"往世界某方向撤"
翻成按键, 必须知道相机前向/右向的世界 XZ 向量。

链路 (活体实证 game 2022.3.59f1):
  CameraManager(堆扫, brain_字段→CinemachineBrain 校验)
    brain_ → CinemachineBrain
      <CurrentCameraState>k__BackingField (活字段表偏移, 内联 Cinemachine CameraState 结构)
        → 结构内扫单位四元数 RawOrientation (自识别: |q|≈1 + 真实俯角 + 非轴对齐)

相机前向 = q·(0,0,1), 右向 = q·(1,0,0), 各自投影到 XZ 平面归一化。
结构内字段偏移随版本漂移, 故用"自识别四元数扫描"而非写死偏移 → 补丁自愈。
"""
from __future__ import annotations

import math
import struct
import time
from typing import Optional, Tuple

from plugins.star_resonance_plugin.mem.il2cpp.live_field_resolver import LiveFieldResolver

CAMMGR_CLASS = "Panda.ZGame.CameraManager"
BRAIN_CLASS = "CinemachineBrain"
CAMMGR_BRAIN_OFF = 0x10          # CameraManager.brain_ (fallback)
BRAIN_CAMSTATE_OFF = 0xB0        # CinemachineBrain.<CurrentCameraState>k__BackingField (fallback)
CAMSTATE_SCAN_LEN = 0x180        # 内联 CameraState 结构扫描长度

_MIN_PTR = 0x10000
_MAX_PTR = 0x7FFF_FFFF_FFFF
_KLASS_LO = 0x10000000           # 元数据/klass 区下限 (排除堆扫巧合命中)
_KLASS_HI = 0x42000000

Vec2 = Tuple[float, float]


def _quat_forward_xz(q) -> Optional[Vec2]:
    """相机前向 q·(0,0,1) 投影到 XZ 平面归一化 (x, z)。"""
    qx, qy, qz, qw = q
    fx = 2.0 * (qx * qz + qw * qy)
    fz = 1.0 - 2.0 * (qx * qx + qy * qy)
    m = math.hypot(fx, fz)
    if m < 1e-4:
        return None
    return (fx / m, fz / m)


def _quat_right_xz(q) -> Optional[Vec2]:
    """相机右向 q·(1,0,0) 投影到 XZ 平面归一化 (x, z)。"""
    qx, qy, qz, qw = q
    rx = 1.0 - 2.0 * (qy * qy + qz * qz)
    rz = 2.0 * (qx * qz - qw * qy)
    m = math.hypot(rx, rz)
    if m < 1e-4:
        return None
    return (rx / m, rz / m)


def _quat_pitch_deg(q) -> float:
    qx, qy, qz, qw = q
    return math.degrees(math.asin(max(-1.0, min(1.0, 2.0 * (qw * qx - qy * qz)))))


class CameraReader:
    """相机前向/右向世界 XZ 基向量读取 (auto-offset + 自识别四元数扫描)。"""

    def __init__(self, dps_source):
        self._src = dps_source
        self._pm = dps_source.sr.pm
        self._lfr = LiveFieldResolver(
            self._pm, klass_resolver=getattr(dps_source.sr, "resolve_klass", None))
        self._cammgr = 0
        self._brain = 0
        self._brain_klass = 0
        self._off_brain = 0
        self._off_camstate = 0
        self._off_quat = None     # 缓存 RawOrientation 在 camstate 内的偏移 (位置锚定后)
        self._last_locate = 0.0
        self._hint_base = 0       # region base that last held CameraManager
        self._scratch = None      # reused scan buffer

    # ---- klass 名校验 ----
    def _kname(self, kp: int) -> str:
        try:
            np = self._pm.read_u64(kp + 0x10)
            if not (_MIN_PTR <= (np or 0) <= _MAX_PTR):
                return ""
            b = self._pm.read_bytes(np, 48)
            s = b.split(b"\x00", 1)[0]
            return s.decode("ascii") if s and all(32 <= c < 127 for c in s) else ""
        except Exception:
            return ""

    def _obj_kname(self, obj: int) -> str:
        try:
            kp = self._pm.read_u64(obj)
            return self._kname(kp) if _MIN_PTR <= (kp or 0) <= _MAX_PTR else ""
        except Exception:
            return ""

    def _resolve_offsets(self) -> None:
        if self._off_brain and self._off_camstate:
            return
        ob = self._lfr.field_offset(CAMMGR_CLASS, "brain_")
        oc = self._lfr.field_offset(BRAIN_CLASS, "CurrentCameraState")
        self._off_brain = int(ob) if ob else CAMMGR_BRAIN_OFF
        self._off_camstate = int(oc) if oc else BRAIN_CAMSTATE_OFF

    def _scan_region_for_cammgr(self, r, kp: int, _cy) -> bool:
        """Cython klass-sentinel scan of one private region (zero-copy scratch).
        Validates each hit's brain_ -> CinemachineBrain; sets self._cammgr/_brain."""
        pm = self._pm
        read_into = getattr(pm, "read_bytes_into", None)
        chunk = 16 * 1024 * 1024
        if self._scratch is None or len(self._scratch) < min(chunk, r.size):
            self._scratch = bytearray(min(chunk, r.size))
        off = 0
        while off < r.size:
            n = min(chunk, r.size - off)
            if read_into is not None:
                got = read_into(r.base + off, self._scratch, n)
                if got <= 0:
                    break
                mv = memoryview(self._scratch)[:got]
            else:
                blob = pm.read_bytes(r.base + off, n)
                if blob is None:
                    break
                mv = blob
                got = n
            if _cy is not None and hasattr(_cy, "find_aligned_u64"):
                hits = _cy.find_aligned_u64(mv, kp, 256)
            else:
                bb = bytes(mv)
                hits, s, kb = [], 0, kp.to_bytes(8, "little")
                while True:
                    i = bb.find(kb, s)
                    if i < 0:
                        break
                    s = i + 8
                    if (i & 0x7) == 0:
                        hits.append(i)
            for i in hits:
                obj = r.base + off + i
                brain = pm.read_u64(obj + self._off_brain) or 0
                if brain <= _KLASS_HI or not (_MIN_PTR <= brain <= _MAX_PTR):
                    continue
                if self._obj_kname(brain) == BRAIN_CLASS:
                    self._cammgr = obj
                    self._brain = brain
                    return True
            off += n
        return False

    def _locate(self) -> bool:
        """定位 CameraManager 实例并取 brain_ (校验 brain_ 指向 CinemachineBrain)。"""
        # 缓存有效性: cammgr 仍是 CameraManager 且 brain_ 仍是 CinemachineBrain
        if self._brain and self._obj_kname(self._brain) == BRAIN_CLASS:
            return True
        self._resolve_offsets()
        pm = self._pm
        # 进程级共享类索引: 一遍 GA 扫服务所有 reader + 按版本持久化 RVA 暖启动
        from plugins.star_resonance_plugin.mem.il2cpp.klass_index import resolve_klasses
        kp = int(resolve_klasses(pm, {CAMMGR_CLASS}, time_budget_s=40).get(CAMMGR_CLASS, 0) or 0)
        if not kp:
            return False
        # 堆扫定位是这里唯一的重计算(扫数百 MB 找 klass 指针, >>1e5 次比对)→ 必须走
        # cy_memscan.find_aligned_u64 (nogil/AVX2 ~16GB/s), 纯 Python blob.find 慢几十倍。
        # 只扫 private (klass 实例在托管堆, 不在 image/mapped), 并用 read_bytes_into+scratch
        # 零拷贝; 命中 region 记为 warm hint, 失效重定位先扫它。
        try:
            from mem_probe import cy_memscan as _cy
        except Exception:
            _cy = None
        # warm hint first
        if self._hint_base:
            for r in pm.iter_regions(only_readable=True, only_private=True):
                if r.base == self._hint_base:
                    if self._scan_region_for_cammgr(r, kp, _cy):
                        return True
                    break
        t0 = time.time()
        for r in pm.iter_regions(only_readable=True, only_private=True):
            if time.time() - t0 > 60:
                break
            if self._scan_region_for_cammgr(r, kp, _cy):
                self._hint_base = r.base
                return True
        return False

    @staticmethod
    def _is_unit_quat(q) -> bool:
        ss = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]
        return 0.96 < ss < 1.04

    def _find_orientation_quat(self, player_pos=None
                               ) -> Optional[Tuple[float, float, float, float]]:
        """定位 RawOrientation 四元数。靠四元数分量做启发式不稳(随相机角度剧变,
        yaw≈0 时只 2 个分量非零)。改用 **RawPosition 锚定**: 相机世界坐标必在玩家
        附近, 找到它→紧随其后(0x10~0x30 内)的单位四元数就是 RawOrientation。
        偏移找到后缓存, 之后直读 + 单位校验, 失效再重找 (auto-offset 补丁自愈)。"""
        base = self._brain + self._off_camstate
        buf = self._pm.read_bytes(base, CAMSTATE_SCAN_LEN)
        if not buf or len(buf) < 16:
            return None
        # 缓存命中: 直读上次找到的偏移
        if self._off_quat is not None and self._off_quat + 16 <= len(buf):
            q = struct.unpack_from("<4f", buf, self._off_quat)
            if self._is_unit_quat(q) and 1.0 <= abs(_quat_pitch_deg(q)) <= 85.0:
                return q
            self._off_quat = None      # 失效, 重找
        # 1) 找 RawPosition: 内联结构里最接近玩家(给了player_pos)的 vec3, Y 接近;
        #    没给 player_pos 时退而求其次找"像世界坐标"的 vec3 (|y|<2000 且有量级)
        best_pos_off = -1
        best_score = 1e18
        for off in range(0, len(buf) - 12, 4):
            v = struct.unpack_from("<3f", buf, off)
            if any(c != c for c in v):    # NaN
                continue
            if not (abs(v[0]) < 1e5 and abs(v[2]) < 1e5 and abs(v[1]) < 5000):
                continue
            if abs(v[0]) < 1 and abs(v[2]) < 1:
                continue
            if player_pos is not None:
                d = math.hypot(v[0] - player_pos[0], v[2] - player_pos[2]) \
                    + abs(v[1] - player_pos[1])
                if d < best_score and d < 120.0:   # 相机在玩家 ~120m 内
                    best_score, best_pos_off = d, off
            else:
                if best_pos_off < 0:
                    best_pos_off = off
        # 2) RawOrientation = RawPosition 之后 0x0C~0x30 内的单位四元数(俯角合理)
        if best_pos_off >= 0:
            for off in range(best_pos_off + 12, min(best_pos_off + 0x34, len(buf) - 16), 4):
                q = struct.unpack_from("<4f", buf, off)
                if self._is_unit_quat(q):
                    p = abs(_quat_pitch_deg(q))
                    if 1.0 <= p <= 85.0 and not all(abs(c) > 0.999 or abs(c) < 1e-3
                                                    for c in q):
                        self._off_quat = off
                        return q
        # 3) 兜底: 全结构扫第一个非平凡单位四元数(俯角合理)
        for off in range(0, len(buf) - 16, 4):
            q = struct.unpack_from("<4f", buf, off)
            if not self._is_unit_quat(q):
                continue
            nz = sum(1 for c in q if abs(c) > 0.02)
            p = abs(_quat_pitch_deg(q))
            if nz >= 2 and 1.0 <= p <= 85.0 and not any(abs(c) > 0.9999 for c in q):
                self._off_quat = off
                return q
        return None

    def read_basis(self, player_pos=None) -> Optional[dict]:
        """返回 {forward:(x,z), right:(x,z), yaw_deg} 世界 XZ 基, 或 None。
        传 player_pos 可用位置锚定稳健定位相机朝向四元数 (强烈建议传)。"""
        try:
            if not self._locate():
                return None
            q = self._find_orientation_quat(player_pos)
            if not q:
                return None
            fwd = _quat_forward_xz(q)
            rgt = _quat_right_xz(q)
            if not fwd or not rgt:
                return None
            return {
                "forward": fwd,
                "right": rgt,
                "yaw_deg": math.degrees(math.atan2(fwd[0], fwd[1])),
            }
        except Exception:
            return None


__all__ = ["CameraReader"]
