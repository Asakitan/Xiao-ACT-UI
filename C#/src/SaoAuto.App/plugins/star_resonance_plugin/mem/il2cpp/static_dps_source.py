"""static_dps_source - 主程序使用的高层接口.

封装:
  - 资源加载 (优先 bundle, 回退到完整 script.json + dump_cs_index)
  - 实例缓存 (避免每次 200s 全堆扫)
  - 高层数据 API: get_self_snapshot() -> SelfSnapshot

主程序集成示例:
    from plugins.star_resonance_plugin.mem.il2cpp.static_dps_source import StaticDpsSource
    src = StaticDpsSource()             # 自动选 bundle / 完整 dump
    snap = src.get_self_snapshot()      # 第一次 ~200s, 之后 <1ms
    print(snap.uid, snap.cur_hp, snap.max_hp)
"""
from __future__ import annotations

import os
import sys
import threading
import time
from dataclasses import dataclass
from typing import Optional

_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from plugins.star_resonance_plugin.mem.il2cpp.static_resolver import StaticResolver, open_resolver
from plugins.star_resonance_plugin.mem.il2cpp.bundle_loader import open_resolver_from_bundle
from plugins.star_resonance_plugin.mem.il2cpp.instance_cache import get_or_find_self
from plugins.star_resonance_plugin.mem.il2cpp.bundle_store import find_bundle_for_running_game


_HERE = os.path.dirname(os.path.abspath(__file__))
_DEFAULT_BUNDLE = os.path.join(_HERE, "_cache", "bundle.json")


@dataclass
class SkillCD:
    skill_id: int          # SkillLevelId (含等级)
    begin_ms: int          # SkillBeginTime (服务器时间 ms)
    duration_ms: int       # Duration (本次 CD 总长 ms)
    valid_cd_ms: int       # ValidCDTime
    charge_count: int      # 剩余充能次数
    cd_type: int           # SkillCDType (0=普通, 1=充能型可能)

    def remaining_ms(self, now_ms: int) -> int:
        """剩余 CD 毫秒 (now_ms = 服务器时间). 0 表示 ready."""
        if self.duration_ms <= 0 or self.begin_ms <= 0:
            return 0
        end = self.begin_ms + self.duration_ms
        return max(0, end - now_ms)

    def is_ready(self, now_ms: int) -> bool:
        return self.remaining_ms(now_ms) == 0 or self.charge_count > 0


@dataclass
class SelfSnapshot:
    uid: int
    cur_hp: int
    max_hp: int
    char_serialize_obj: int
    user_fight_attr_obj: int
    fetched_at: float
    # 扩展字段 (可选; 调用者需显式走 get_extended_snapshot 获取)
    is_dead: int = 0
    origin_energy: float = 0.0
    profession_id: int = 0
    char_name: str = ""
    resources: dict = None     # {resource_id: count}
    skill_cds: list = None     # List[SkillCD]
    # 身份/进度 (Phase 1)
    level_base: int = 0        # RoleLevel.Level
    season_exp: int = 0        # RoleLevel.AccumulateExp
    season_medal_level: int = 0  # SeasonMedalInfo.CoreHoleInfo.HoleLevel
    fight_point: int = 0       # CharBase.FightPoint
    # 体力 (Phase 1)
    energy_limit: int = 0      # EnergyItem.EnergyLimit
    extra_energy_limit: int = 0  # EnergyItem.ExtraEnergyLimit
    # 场景 (CharSerialize.SceneData.MapId)
    scene_map_id: int = 0


class StaticDpsSource:
    """主程序对接口."""

    SELF_CLASS = "Zproto.CharSerialize"
    SELF_SENTINEL_FIELD = "Attr"
    SELF_SENTINEL_CLASS = "Zproto.UserFightAttr"

    def __init__(self, bundle_path: Optional[str] = None,
                 dump_id: str = "ef9ef95a"):
        self.bundle_path = bundle_path or _DEFAULT_BUNDLE
        self.dump_id = dump_id
        self._sr: Optional[StaticResolver] = None
        # 后台扫描状态
        self._scan_lock = threading.Lock()
        self._scan_thread: Optional[threading.Thread] = None
        self._last_snapshot: Optional[SelfSnapshot] = None
        self._scan_in_progress: bool = False
        self._scan_failed_at: float = 0.0
        self._sr_lock = threading.Lock()  # StarProcess 不是线程安全的

    @property
    def sr(self) -> StaticResolver:
        if self._sr is None:
            # 1. 优先从 bundle_store 按 game_key 自动找
            store_hit = find_bundle_for_running_game()
            if store_hit:
                bundle_path, key, _ga_path = store_hit
                print(f"[dps-source] using bundle from store: {os.path.basename(bundle_path)} "
                      f"(game_key={key[:16]}...)", file=sys.stderr)
                self._sr = open_resolver_from_bundle(bundle_path)
            elif os.path.isfile(self.bundle_path):
                self._sr = open_resolver_from_bundle(self.bundle_path)
            else:
                self._sr = open_resolver(self.dump_id)
        return self._sr

    def get_self_snapshot(self, force_rescan: bool = False) -> Optional[SelfSnapshot]:
        sr = self.sr
        with self._sr_lock:
            hit = get_or_find_self(sr, self.SELF_CLASS,
                                   self.SELF_SENTINEL_FIELD,
                                   self.SELF_SENTINEL_CLASS,
                                   force_rescan=force_rescan)
            if not hit:
                return None
            obj, attr = hit
            uid = sr.read_field(obj, self.SELF_CLASS, "CharId")
            cur = sr.read_field(attr, self.SELF_SENTINEL_CLASS, "CurHp")
            mx = sr.read_field(attr, self.SELF_SENTINEL_CLASS, "MaxHp")
        if uid is None or cur is None or mx is None:
            return None
        snap = SelfSnapshot(
            uid=uid, cur_hp=cur, max_hp=mx,
            char_serialize_obj=obj, user_fight_attr_obj=attr,
            fetched_at=time.time(),
        )
        self._last_snapshot = snap
        return snap

    # ───────── 异步扫描 ─────────

    def get_self_snapshot_nowait(self) -> Optional[SelfSnapshot]:
        """非阻塞: 缓存命中直接返回; 未命中则启动后台扫描并立即返回 None.

        典型用法 (主循环里轮询):
            snap = src.get_self_snapshot_nowait()
            if snap is None:
                ...显示 "正在扫描..."
            else:
                ...用 snap 数据
        """
        sr = self.sr
        # 先尝试用现有缓存命中 (不触发扫描)
        with self._sr_lock:
            try:
                # 复用 get_or_find_self 的校验逻辑: 强制不扫,直接读 cache
                from plugins.star_resonance_plugin.mem.il2cpp.instance_cache import _load, _key, _DEFAULT_CACHE
                cache = _load(_DEFAULT_CACHE)
                e = cache.get(_key(self.SELF_CLASS, self.SELF_SENTINEL_FIELD))
                kp = sr.resolve_klass(self.SELF_CLASS)
                skp = sr.resolve_klass(self.SELF_SENTINEL_CLASS)
                if e and kp and skp and e.get("pid") == sr.pm.pid \
                        and e.get("ga_base") == sr.ga \
                        and e.get("klass_ptr") == kp:
                    obj = e["obj"]
                    if sr.pm.read_u64(obj) == kp:
                        sf_off = sr.dci.field_offset(self.SELF_CLASS,
                                                     self.SELF_SENTINEL_FIELD)
                        if sf_off is not None:
                            sp = sr.pm.read_u64(obj + sf_off)
                            if sp and sr.pm.read_u64(sp) == skp:
                                uid = sr.read_field(obj, self.SELF_CLASS, "CharId")
                                cur = sr.read_field(sp, self.SELF_SENTINEL_CLASS, "CurHp")
                                mx = sr.read_field(sp, self.SELF_SENTINEL_CLASS, "MaxHp")
                                if uid is not None and cur is not None and mx is not None:
                                    snap = SelfSnapshot(
                                        uid=uid, cur_hp=cur, max_hp=mx,
                                        char_serialize_obj=obj,
                                        user_fight_attr_obj=sp,
                                        fetched_at=time.time(),
                                    )
                                    self._last_snapshot = snap
                                    return snap
            except Exception:
                pass

        # 缓存未命中 → 启动后台扫描 (若未在跑)
        self._kick_background_scan()
        return None

    def _kick_background_scan(self) -> None:
        with self._scan_lock:
            if self._scan_in_progress:
                return
            # 失败后 30s 才允许下次重试
            if self._scan_failed_at and time.time() - self._scan_failed_at < 30:
                return
            self._scan_in_progress = True

        def _worker():
            try:
                # blocking scan (会写 cache)
                self.get_self_snapshot(force_rescan=False)
                with self._scan_lock:
                    self._scan_failed_at = 0.0
            except Exception:
                with self._scan_lock:
                    self._scan_failed_at = time.time()
            finally:
                with self._scan_lock:
                    self._scan_in_progress = False
                    self._scan_thread = None

        t = threading.Thread(target=_worker, name="static-dps-scan", daemon=True)
        self._scan_thread = t
        t.start()

    @property
    def scan_in_progress(self) -> bool:
        with self._scan_lock:
            return self._scan_in_progress

    @property
    def last_snapshot(self) -> Optional[SelfSnapshot]:
        return self._last_snapshot

    # ───────── 扩展读取 (Boss Raid + AutoKey 用) ─────────

    def fill_extended(self, snap: SelfSnapshot) -> SelfSnapshot:
        """在现有 snap 上补充 SkillCD/Resources/Profession/Name 等扩展信息."""
        if snap is None:
            return snap
        sr = self.sr
        attr = snap.user_fight_attr_obj
        char = snap.char_serialize_obj
        with self._sr_lock:
            try:
                snap.is_dead = sr.read_field(attr, self.SELF_SENTINEL_CLASS, "IsDead") or 0
                snap.origin_energy = sr.read_field(attr, self.SELF_SENTINEL_CLASS, "OriginEnergy") or 0.0
                # CharBase -> Name / FightPoint / InitProfessionId
                cbase = sr.read_ptr_field(char, self.SELF_CLASS, "CharBase")
                if cbase:
                    name_off = sr.dci.field_offset("Zproto.CharBaseInfo", "Name")
                    if name_off is not None:
                        name_ptr = sr.pm.read_u64(cbase + name_off)
                        snap.char_name = sr.read_string(name_ptr) or ""
                    snap.fight_point = sr.read_field(cbase, "Zproto.CharBaseInfo", "FightPoint") or 0
                    snap.profession_id = sr.read_field(cbase, "Zproto.CharBaseInfo", "InitProfessionId") or 0
                # ProfessionList.CurProfessionId 才是当前生效职业 (覆盖 cosmetic)
                plist = sr.read_ptr_field(char, self.SELF_CLASS, "ProfessionList")
                if plist:
                    cur_pid = sr.read_field(plist, "Zproto.ProfessionList", "CurProfessionId") or 0
                    if cur_pid > 0:
                        snap.profession_id = cur_pid
                # RoleLevel
                rlvl = sr.read_ptr_field(char, self.SELF_CLASS, "RoleLevel")
                if rlvl:
                    snap.level_base = sr.read_field(rlvl, "Zproto.RoleLevel", "Level") or 0
                    snap.season_exp = sr.read_field(rlvl, "Zproto.RoleLevel", "CurLevelExp") or 0
                # SeasonMedalInfo.CoreHoleInfo.HoleLevel
                smi = sr.read_ptr_field(char, self.SELF_CLASS, "SeasonMedalInfo")
                if smi:
                    core = sr.read_ptr_field(smi, "Zproto.SeasonMedalInfo", "CoreHoleInfo")
                    if core:
                        snap.season_medal_level = sr.read_field(core, "Zproto.MedalHole", "HoleLevel") or 0
                # EnergyItem (CharSerialize.EnergyItem)
                eitem = sr.read_ptr_field(char, self.SELF_CLASS, "EnergyItem")
                if eitem:
                    snap.energy_limit = sr.read_field(eitem, "Zproto.EnergyItem", "EnergyLimit") or 0
                    snap.extra_energy_limit = sr.read_field(eitem, "Zproto.EnergyItem", "ExtraEnergyLimit") or 0
                # Resources / ResourceIds (zip) — both are u32 arrays
                ids = sr.read_repeated_field(attr, self.SELF_SENTINEL_CLASS, "ResourceIds", elem_size=4, max_count=64)
                vals = sr.read_repeated_field(attr, self.SELF_SENTINEL_CLASS, "Resources", elem_size=4, max_count=64)
                snap.resources = {int(rid): int(rv) for rid, rv in zip(ids, vals)}
                # SkillCDInfo list — IL2CPP 用了 heap-allocated elements;
                # array of u64 pointers, each points to one SkillCDInfo instance
                # (layout: +0x00 klass ptr, +0x10 SkillLevelId, +0x18 Begin, ...).
                cd_rf = sr.read_ptr_field(attr, self.SELF_SENTINEL_CLASS, "CdInfo")
                snap.skill_cds = []
                if cd_rf:
                    cd_ptrs = sr.read_repeated_elements(cd_rf, elem_size=8, max_count=512)
                    for ptr in cd_ptrs:
                        if not ptr:
                            continue
                        snap.skill_cds.append(SkillCD(
                            skill_id=sr.read_field(ptr, "Zproto.SkillCDInfo", "SkillLevelId") or 0,
                            begin_ms=sr.read_field(ptr, "Zproto.SkillCDInfo", "SkillBeginTime") or 0,
                            duration_ms=sr.read_field(ptr, "Zproto.SkillCDInfo", "Duration") or 0,
                            valid_cd_ms=sr.read_field(ptr, "Zproto.SkillCDInfo", "ValidCDTime") or 0,
                            charge_count=sr.read_field(ptr, "Zproto.SkillCDInfo", "ChargeCount") or 0,
                            cd_type=sr.read_field(ptr, "Zproto.SkillCDInfo", "SkillCDType") or 0,
                        ))
            except Exception as e:
                # 扩展字段读失败不应该影响基本 snap; 丢 partial
                if snap.skill_cds is None:
                    snap.skill_cds = []
                if snap.resources is None:
                    snap.resources = {}
        return snap

    def get_extended_snapshot(self, force_rescan: bool = False) -> Optional[SelfSnapshot]:
        """一次性拿到全字段 (HP + SkillCD + Resources + Profession)."""
        snap = self.get_self_snapshot(force_rescan=force_rescan)
        if snap:
            self.fill_extended(snap)
        return snap

    def get_extended_snapshot_nowait(self) -> Optional[SelfSnapshot]:
        snap = self.get_self_snapshot_nowait()
        if snap:
            self.fill_extended(snap)
        return snap

    def close(self):
        if self._sr is not None:
            try:
                self._sr.pm.close()
            except Exception:
                pass
            self._sr = None


def _selftest():
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("--bundle", default=None)
    p.add_argument("--known-uid", type=int, default=36668136)
    p.add_argument("--known-hp", type=int, default=None)
    p.add_argument("--repeat", type=int, default=2,
                   help="读 N 次, 第 2 次起应命中缓存")
    p.add_argument("--force", action="store_true")
    p.add_argument("--async-mode", action="store_true",
                   help="测试 get_self_snapshot_nowait + 后台扫描")
    args = p.parse_args()

    src = StaticDpsSource(bundle_path=args.bundle)
    try:
        if args.async_mode:
            print("[async] 第一次轮询 (缓存命中应直接返回, 否则启后台扫):")
            for i in range(15):
                snap = src.get_self_snapshot_nowait()
                if snap:
                    print(f"  poll #{i}: HP={snap.cur_hp}/{snap.max_hp} obj=0x{snap.char_serialize_obj:X}")
                    break
                else:
                    print(f"  poll #{i}: scanning={src.scan_in_progress}")
                time.sleep(1.0)
            return

        for i in range(args.repeat):
            t0 = time.time()
            snap = src.get_self_snapshot(force_rescan=args.force and i == 0)
            dt = time.time() - t0
            if snap is None:
                print(f"[#{i+1}] FAIL ({dt:.2f}s)")
                continue
            tag_uid = "OK" if snap.uid == args.known_uid else "FAIL"
            tag_hp = ""
            if args.known_hp is not None:
                tag_hp = " HP-OK" if snap.cur_hp == args.known_hp else " HP-FAIL"
            print(f"[#{i+1}] {dt*1000:.1f}ms  UID={snap.uid}({tag_uid}) "
                  f"HP={snap.cur_hp}/{snap.max_hp}{tag_hp}  obj=0x{snap.char_serialize_obj:X}")
    finally:
        src.close()


if __name__ == "__main__":
    _selftest()

