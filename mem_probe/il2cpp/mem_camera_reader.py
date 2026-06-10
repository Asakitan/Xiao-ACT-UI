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

from mem_probe.il2cpp.live_field_resolver import LiveFieldResolver

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
        self._last_locate = 0.0

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

    def _locate(self) -> bool:
        """定位 CameraManager 实例并取 brain_ (校验 brain_ 指向 CinemachineBrain)。"""
        # 缓存有效性: cammgr 仍是 CameraManager 且 brain_ 仍是 CinemachineBrain
        if self._brain and self._obj_kname(self._brain) == BRAIN_CLASS:
            return True
        self._resolve_offsets()
        pm = self._pm
        from mem_probe.il2cpp.auto_registration_locator import build_live_class_index
        idx = build_live_class_index(pm, {CAMMGR_CLASS}, time_budget_s=40)
        kp = int(idx.get(CAMMGR_CLASS, 0) or 0)
        if not kp:
            return False
        kb = kp.to_bytes(8, "little")
        t0 = time.time()
        for r in pm.iter_regions(only_readable=True):
            if time.time() - t0 > 60:
                break
            off, chunk = 0, 16 * 1024 * 1024
            while off < r.size:
                blob = pm.read_bytes(r.base + off, min(chunk, r.size - off))
                if blob is None:
                    break
                s = 0
                while True:
                    i = blob.find(kb, s)
                    if i < 0:
                        break
                    s = i + 8
                    if i & 0x7:
                        continue
                    obj = r.base + off + i
                    brain = pm.read_u64(obj + self._off_brain) or 0
                    if brain <= _KLASS_HI or not (_MIN_PTR <= brain <= _MAX_PTR):
                        continue
                    if self._obj_kname(brain) == BRAIN_CLASS:
                        self._cammgr = obj
                        self._brain = brain
                        return True
                off += chunk
        return False

    def _find_orientation_quat(self) -> Optional[Tuple[float, float, float, float]]:
        """在内联 CameraState 结构里扫真实相机朝向四元数 (自识别)。"""
        base = self._brain + self._off_camstate
        buf = self._pm.read_bytes(base, CAMSTATE_SCAN_LEN)
        if not buf or len(buf) < 16:
            return None
        best = None
        for off in range(0, len(buf) - 16, 4):
            q = struct.unpack_from("<4f", buf, off)
            ss = sum(c * c for c in q)
            if not (0.97 < ss < 1.03):
                continue
            # 排除平凡轴对齐 (默认/未初始化): 至少 3 个分量非零且无单一分量≈±1
            nz = sum(1 for c in q if abs(c) > 0.02)
            if nz < 3 or any(abs(c) > 0.995 for c in q):
                continue
            pitch = abs(_quat_pitch_deg(q))
            # gameplay 相机恒有真实俯角 (经验 5°~80°)
            if 3.0 <= pitch <= 80.0:
                best = q
                break
        return best

    def read_basis(self) -> Optional[dict]:
        """返回 {forward:(x,z), right:(x,z), yaw_deg} 世界 XZ 基, 或 None。"""
        try:
            if not self._locate():
                return None
            q = self._find_orientation_quat()
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
