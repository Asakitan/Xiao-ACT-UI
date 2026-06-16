# -*- coding: utf-8 -*-
"""mem_boss_skill_state_reader - 读 boss 状态机当前出招 (ZStateSkillComp).

★为什么需要它 (主人: 距离兜底肯定错): 很多 boss 的预警圈不是可读的 ZoneEnt(在 ECS/技能
特效里), zoneDict_ 读不到 → 精准出圈无信号源。但 boss **正在出什么招**是状态机里实时可读的:
`ZStateSkillComp.curSkillId_`(当前技能id) + `curStageId_`(阶段)。这就是机制/预警圈的真正
驱动源——boss 出招=预警圈出现, 招结束=圈消失。

于是躲避停止条件可以从"距离"(错: 锥/线/扇形到圆心距离无意义)换成**"招式生命周期"**:
boss 出危险招时躲, curSkillId_ 变回待机(0/1)或换招即停。无论圈什么形状都对。

全 auto-offset: curSkillId_/curStageId_ 偏移从活 ZStateSkillComp 对象 klass 字段表解。
实测活体: boss 出招 skill=6920001(stage 0↔1)/6920012; 待机 curSkillId_∈{0,1}。
"""
from __future__ import annotations

from typing import Dict, Optional

from plugins.star_resonance_plugin.mem.il2cpp import auto_offsets as _ao

_MINP, _MAXP = 0x10000, 0x7FFF_FFFF_FFFF
ARRAY_ELEMS_OFF = 0x20               # Il2Cpp STRUCTURAL: array element ptrs start
SKILLCOMP_NAME = "ZStateSkillComp"
IDLE_SKILL_IDS = (0, 1)  # 待机/无技能


class BossSkillStateReader:
    def __init__(self, dps_source):
        self._src = dps_source
        self._pm = dps_source.sr.pm
        # init-time auto-offset resolution (Stage A only — never re-resolve in hot path).
        # ZEntity.compList_ is a managed field; resolved by name from the live dump or
        # bundle, falling back to the verified-live literal 0x60 when both are missing.
        ent = (_ao.resolve(dps_source, "Panda.ZGame.ZEntity",
                            {"off_ent_complist": ("compList_", 0x60)})
               if dps_source is not None else {})
        self._off_ent_complist = int(ent.get("off_ent_complist", 0x60))
        self._off_skill = 0
        self._off_stage = 0
        # (comp_ptr, klass_ptr) — klass ptr 用于 O(1) 快速验证(替代每 tick 3-RPM name 验证)
        self._comp_cache: Dict[int, tuple] = {}

    def _kname(self, obj: int) -> str:
        try:
            kp = self._pm.read_u64(obj)
            np = self._pm.read_u64(kp + 0x10)
            b = self._pm.read_bytes(np, 64)
            s = b.split(b"\x00", 1)[0]
            return s.decode("ascii", "replace") if s else ""
        except Exception:
            return ""

    def _read_klass_ptr(self, obj: int) -> int:
        try:
            return int(self._pm.read_u64(obj) or 0)
        except Exception:
            return 0

    def cached_comp(self, ent_obj: int) -> tuple:
        """返回缓存的 (comp_ptr, klass_ptr)，无缓存返回 (0, 0)。"""
        return self._comp_cache.get(ent_obj, (0, 0))

    def validate_comp_fast(self, comp: int, klass_val: int) -> bool:
        """O(1) klass 指针比较验证 (0 RPM — klass_val 由调用方 batch 读出)。
        klass ptr 变化(entity 池化复用)时返回 False, 调用方走完整 _skill_comp 重扫。"""
        for _k, (cp, kp) in self._comp_cache.items():
            if cp == comp:
                return kp != 0 and kp == klass_val
        return False

    def _skill_comp(self, ent_obj: int) -> int:
        # 热路径(12.5Hz)缓存 comp ptr; klass ptr 快验 → 失效时重扫
        entry = self._comp_cache.get(ent_obj)
        if entry:
            comp, klass = entry
            klass_now = self._read_klass_ptr(comp)
            if klass_now and klass_now == klass:
                return comp
            if klass_now and self._kname(comp) == SKILLCOMP_NAME:
                self._comp_cache[ent_obj] = (comp, klass_now)
                return comp
        cl = self._pm.read_u64(ent_obj + self._off_ent_complist) or 0
        if not (_MINP <= cl <= _MAXP):
            return 0
        for i in range(40):
            cp = self._pm.read_u64(cl + ARRAY_ELEMS_OFF + i * 8) or 0
            if _MINP <= cp <= _MAXP and self._kname(cp) == SKILLCOMP_NAME:
                kp = self._read_klass_ptr(cp)
                self._comp_cache[ent_obj] = (cp, kp)
                return cp
        return 0

    def _resolve_offsets(self, comp: int) -> None:
        if self._off_skill:
            return
        # auto-offset via auto_offsets.offset (live field table -> dump bundle -> literal)
        try:
            self._off_skill = int(_ao.offset(self._src, SKILLCOMP_NAME, "curSkillId_") or 0x40)
            self._off_stage = int(_ao.offset(self._src, SKILLCOMP_NAME, "curStageId_") or 0x64)
        except Exception:
            self._off_skill = 0x40
            self._off_stage = 0x64

    def ensure_offsets(self, comp: int = 0) -> None:
        """确保 _off_skill/_off_stage 已解析 (供外部 batch 路径调用)。"""
        self._resolve_offsets(comp)

    def read_skill_fields(self, comp: int) -> tuple:
        """直接读 skill/stage 字段, 不做 comp 验证 (调用方已验证)。返回 (skill_id, stage_id)。"""
        self._resolve_offsets(comp)
        sid = self._pm.read_i32(comp + self._off_skill)
        stage = self._pm.read_i32(comp + self._off_stage)
        return (int(sid) if sid is not None else 0, int(stage or 0))

    def read_skill_cast(self, ent_obj: int) -> Optional[Dict]:
        """返回 {skill_id, stage_id} (boss 当前出招); 待机/读不到返回 None。"""
        try:
            comp = self._skill_comp(ent_obj)
            if not comp:
                return None
            self._resolve_offsets(comp)
            sid = self._pm.read_i32(comp + self._off_skill)
            if sid is None or sid in IDLE_SKILL_IDS:
                return None
            stage = self._pm.read_i32(comp + self._off_stage)
            return {"skill_id": int(sid), "stage_id": int(stage or 0)}
        except Exception:
            return None

    def current_skill_id(self, ent_obj: int) -> int:
        c = self.read_skill_cast(ent_obj)
        return c["skill_id"] if c else 0


__all__ = ["BossSkillStateReader"]
