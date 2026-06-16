"""mem_self_state_provider - 用内存替代 TCP 提供 self 状态.

后台线程定期读 StaticDpsSource.get_self_snapshot_nowait():
  - UID 变化 → 触发回调 (主程序据此更新 dps_tracker.set_self_uid 等)
  - HP/MaxHp 变化 → 触发回调

TCP 回退策略:
  - 内存源连续 N 次失败 (>5s) → 切到 TCP 模式 (set_provider("tcp")), 让原 packet
    parser 接管. 仅当用户/主程序明确调用时才会切回内存.
  - 默认模式: "memory" (启动时尝试内存, 失败提示用户).

主程序集成:
    from plugins.star_resonance_plugin.mem.il2cpp.mem_self_state_provider import MemSelfStateProvider
    provider = MemSelfStateProvider(
        on_uid_change=lambda uid: dps_tracker.set_self_uid(uid),
        on_hp_change=lambda cur,mx: hp_overlay.update(cur,mx),
        on_status_change=lambda mode,err: ui.set_data_source(mode,err),
    )
    provider.start()
    ...
    provider.stop()
"""
from __future__ import annotations

import os
import sys
import threading
import time
import traceback
from typing import Callable, Optional

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(_HERE)))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from plugins.star_resonance_plugin.mem.il2cpp.static_dps_source import StaticDpsSource, SelfSnapshot
from plugins.star_resonance_plugin.mem.il2cpp.mem_state_anchor import AnchorMemoryReader, AnchorPack
from mem_probe import cy_memscan as _cy_memscan


class MemSelfStateProvider:
    POLL_INTERVAL = 0.5   # 内存读取频率 (s) - 缓存命中后只算 read_field, <1ms
    EXT_POLL_EVERY = 1    # 每 N 个 tick 拉一次扩展快照 (skill CDs); 0=不拉
    FAIL_THRESHOLD = 10   # 连续失败 N 次 → 切 TCP 模式

    def __init__(self,
                 on_uid_change: Optional[Callable[[int], None]] = None,
                 on_hp_change: Optional[Callable[[int, int], None]] = None,
                 on_status_change: Optional[Callable[[str, str], None]] = None,
                 on_skill_cd_change: Optional[Callable[[list], None]] = None,
                 on_resources_change: Optional[Callable[[dict], None]] = None,
                 on_profession_change: Optional[Callable[[int, str], None]] = None,
                 on_dead_change: Optional[Callable[[bool], None]] = None,
                 on_identity_change: Optional[Callable[[dict], None]] = None,
                 on_stamina_change: Optional[Callable[[int, int], None]] = None,
                 anchor_source: Optional[Callable[[], AnchorPack]] = None,
                 enable_extended: bool = True,
                 auto_scan_enabled: bool = True,
                 poll_interval: Optional[float] = None,
                 allow_static_fallback: bool = True,
                 max_scan_regions_mb: int = 0,
                 dump_id: str = "ef9ef95a"):
        self.on_uid_change = on_uid_change
        self.on_hp_change = on_hp_change
        self.on_status_change = on_status_change
        self.on_skill_cd_change = on_skill_cd_change
        self.on_resources_change = on_resources_change
        self.on_profession_change = on_profession_change
        self.on_dead_change = on_dead_change
        self.on_identity_change = on_identity_change
        self.on_stamina_change = on_stamina_change
        self.anchor_source = anchor_source
        self.enable_extended = enable_extended
        self.auto_scan_enabled = bool(auto_scan_enabled)
        try:
            self.poll_interval = max(0.1, min(float(poll_interval or self.POLL_INTERVAL), 30.0))
        except Exception:
            self.poll_interval = float(self.POLL_INTERVAL)
        self.allow_static_fallback = bool(allow_static_fallback)
        self.max_scan_regions_mb = max(0, int(max_scan_regions_mb or 0))
        self.dump_id = dump_id

        self._src: Optional[StaticDpsSource] = None
        self._anchor_reader: Optional[AnchorMemoryReader] = None
        self._thread: Optional[threading.Thread] = None
        self._stop_evt = threading.Event()
        self._lock = threading.Lock()

        # 状态
        self.mode: str = "init"   # init | memory | tcp | error
        self.last_error: str = ""
        self.last_snap: Optional[SelfSnapshot] = None
        self._last_uid: int = 0
        self._last_hp: int = 0
        self._last_mx: int = 0
        self._consecutive_fails: int = 0
        # 扩展状态 (仅用于判变化)
        self._last_skill_sig: tuple = ()       # (skill_id, ready_bool) sorted
        self._last_resources: dict = {}
        self._last_profession_id: int = 0
        self._last_char_name: str = ""
        self._last_dead: int = 0
        self._last_identity: tuple = (0, 0, 0, 0)  # (level, season_exp, season_medal, fight_point)
        self._last_stamina: tuple = (-1, -1)         # (energy_current, energy_total)
        self._tick_no: int = 0
        self._last_anchor_strength: str = "none"
        self._last_anchor_skip_reason: str = ""

    # ───────── public ─────────

    def start(self):
        if self._thread and self._thread.is_alive():
            return
        self._stop_evt.clear()
        self._thread = threading.Thread(
            target=self._loop, name="mem-self-state", daemon=True
        )
        self._thread.start()

    def stop(self, join_timeout: float = 2.0):
        self._stop_evt.set()
        if self._thread:
            self._thread.join(timeout=join_timeout)
        if self._src:
            self._src.close()
            self._src = None
        self._anchor_reader = None

    def force_mode(self, mode: str):
        """主程序可调来强制切到 'tcp' 或 'memory'."""
        with self._lock:
            self.mode = mode
            self._consecutive_fails = 0
        self._notify_status()

    # ───────── internal ─────────

    def _set_mode(self, mode: str, err: str = ""):
        with self._lock:
            if self.mode == mode and self.last_error == err:
                return
            self.mode = mode
            self.last_error = err
        self._notify_status()

    def _notify_status(self):
        if self.on_status_change:
            try:
                self.on_status_change(self.mode, self.last_error)
            except Exception:
                pass

    def _loop(self):
        if not self.auto_scan_enabled:
            self._set_mode("tcp", "memory auto scan disabled")
            while not self._stop_evt.is_set():
                self._stop_evt.wait(self.poll_interval)
            return

        # 第一次启动: 创建 source (打开 GA, 加载 bundle)
        try:
            self._src = StaticDpsSource(dump_id=self.dump_id)
            _ = self._src.sr  # 触发懒加载
            self._anchor_reader = AnchorMemoryReader(
                self._src.sr.pm,
                max_scan_regions_mb=self.max_scan_regions_mb,
                resolver=self._src.sr,   # auto-offset: resolve proto layouts by name
            )
            self._set_mode("memory")
        except Exception as e:
            self._set_mode("error", f"init: {e}")
            traceback.print_exc()
            return

        while not self._stop_evt.is_set():
            try:
                # 当 force 到 tcp 时, 不读内存, 只睡眠
                if self.mode == "tcp":
                    time.sleep(self.poll_interval)
                    continue

                snap = self._get_snapshot_nowait()
                if snap is None:
                    # 缓存未命中, 后台扫描中
                    self._consecutive_fails += 1
                    if self._consecutive_fails >= self.FAIL_THRESHOLD \
                            and not self._src.scan_in_progress:
                        # 扫完了还是 None → 真失败
                        self._set_mode("tcp",
                                       "memory snapshot unavailable, fallback")
                else:
                    self._consecutive_fails = 0
                    if self.mode != "memory":
                        self._set_mode("memory")
                    # 全字段: 拉 ext (skill CDs / resources / name / profession)
                    self._tick_no += 1
                    if self.enable_extended and self.EXT_POLL_EVERY > 0 \
                            and (self._tick_no % self.EXT_POLL_EVERY == 0):
                        try:
                            self._src.fill_extended(snap)
                        except Exception:
                            traceback.print_exc()
                    self._dispatch(snap)
            except Exception as e:
                self._consecutive_fails += 1
                if self._consecutive_fails >= self.FAIL_THRESHOLD:
                    self._set_mode("tcp", f"poll exc: {e}")
            self._stop_evt.wait(self.poll_interval)

    def _get_snapshot_nowait(self) -> Optional[SelfSnapshot]:
        """Prefer TCP-anchor memory lookup, then fall back to static cache.

        The anchor path is only attempted when the PacketParser has a strong
        semantic pack (uid + level/profession/skills).  It never writes to the
        target process and it reuses the same StarProcess handle as the static
        source.
        """
        if self.anchor_source and self._anchor_reader and self._src:
            try:
                anchor = self.anchor_source() or AnchorPack()
                if int(getattr(anchor, 'uid', 0) or 0) > 0:
                    if not anchor.is_strong():
                        self._last_anchor_strength = "weak"
                        self._last_anchor_skip_reason = "weak_anchor_no_scan"
                    else:
                        self._last_anchor_strength = "strong"
                        self._last_anchor_skip_reason = ""
                        resolved = self._anchor_reader.find_self(anchor)
                        if resolved:
                            data = self._anchor_reader.read_self_snapshot(resolved)
                            uid = int(data.get('uid') or 0)
                            cur = int(data.get('cur_hp') or 0)
                            mx = int(data.get('max_hp') or 0)
                            if uid > 0 and mx > 0:
                                snap = SelfSnapshot(
                                    uid=uid,
                                    cur_hp=cur,
                                    max_hp=mx,
                                    char_serialize_obj=resolved.char_serialize_obj,
                                    user_fight_attr_obj=resolved.user_fight_attr_obj,
                                    fetched_at=time.time(),
                                )
                                snap.is_dead = int(data.get('is_dead') or 0)
                                snap.origin_energy = float(data.get('origin_energy') or 0.0)
                                snap.profession_id = int(data.get('profession_id') or data.get('init_profession_id') or 0)
                                snap.char_name = str(data.get('name') or '')
                                snap.level_base = int(data.get('level_base') or 0)
                                snap.season_exp = int(data.get('season_exp') or 0)
                                snap.fight_point = int(data.get('fight_point') or 0)
                                snap.energy_limit = int(data.get('energy_limit') or 0)
                                snap.extra_energy_limit = int(data.get('extra_energy_limit') or 0)
                                snap.scene_map_id = int(data.get('scene_map_id') or 0)
                                try:
                                    self._src.fill_extended(snap)
                                except Exception:
                                    traceback.print_exc()
                                self._src._last_snapshot = snap
                                return snap
            except Exception:
                traceback.print_exc()
        if not self.allow_static_fallback:
            return None
        return self._src.get_self_snapshot_nowait() if self._src else None

    def _dispatch(self, snap: SelfSnapshot):
        self.last_snap = snap
        if snap.uid != self._last_uid:
            self._last_uid = snap.uid
            if self.on_uid_change:
                try:
                    self.on_uid_change(snap.uid)
                except Exception:
                    traceback.print_exc()
        if snap.cur_hp != self._last_hp or snap.max_hp != self._last_mx:
            self._last_hp = snap.cur_hp
            self._last_mx = snap.max_hp
            if self.on_hp_change:
                try:
                    self.on_hp_change(snap.cur_hp, snap.max_hp)
                except Exception:
                    traceback.print_exc()
        # 扩展字段变化检测 (仅在调用了 fill_extended 后 snap.skill_cds 不是 None)
        if snap.skill_cds is not None:
            now_ms = int(snap.fetched_at * 1000)
            sig = tuple(sorted(
                (cd.skill_id, cd.is_ready(now_ms))
                for cd in snap.skill_cds if cd.skill_id
            ))
            if sig != self._last_skill_sig:
                self._last_skill_sig = sig
                if self.on_skill_cd_change:
                    try:
                        self.on_skill_cd_change(list(snap.skill_cds))
                    except Exception:
                        traceback.print_exc()
        if snap.resources is not None and snap.resources != self._last_resources:
            self._last_resources = dict(snap.resources)
            if self.on_resources_change:
                try:
                    self.on_resources_change(dict(snap.resources))
                except Exception:
                    traceback.print_exc()
        if (snap.profession_id, snap.char_name) != (self._last_profession_id, self._last_char_name):
            self._last_profession_id = snap.profession_id
            self._last_char_name = snap.char_name
            if self.on_profession_change:
                try:
                    self.on_profession_change(snap.profession_id, snap.char_name)
                except Exception:
                    traceback.print_exc()
        if snap.is_dead != self._last_dead:
            self._last_dead = snap.is_dead
            if self.on_dead_change:
                try:
                    self.on_dead_change(bool(snap.is_dead))
                except Exception:
                    traceback.print_exc()
        # Identity (level / season_exp / season_medal / fight_point)
        ident = (int(snap.level_base or 0), int(snap.season_exp or 0),
                 int(snap.season_medal_level or 0), int(snap.fight_point or 0))
        if ident != self._last_identity and any(ident):
            self._last_identity = ident
            if self.on_identity_change:
                try:
                    self.on_identity_change({
                        'level_base': ident[0],
                        'season_exp': ident[1],
                        'season_medal_level': ident[2],
                        'fight_point': ident[3],
                    })
                except Exception:
                    traceback.print_exc()
        # Stamina: energy_current = OriginEnergy ; total = EnergyLimit + ExtraEnergyLimit
        e_cur = int(snap.origin_energy or 0)
        e_tot = int((snap.energy_limit or 0) + (snap.extra_energy_limit or 0))
        if (e_cur, e_tot) != self._last_stamina and e_tot > 0:
            self._last_stamina = (e_cur, e_tot)
            if self.on_stamina_change:
                try:
                    self.on_stamina_change(e_cur, e_tot)
                except Exception:
                    traceback.print_exc()

    def policy_status(self) -> dict:
        reader = self._anchor_reader
        region_mb = 0.0
        limited = False
        scan_mode = "none"
        scan_time_ms = 0.0
        scan_confidence = 0.0
        if reader is not None:
            try:
                region_mb = round(float(getattr(reader, "last_region_scan_bytes", 0) or 0) / (1024 * 1024), 3)
                limited = bool(getattr(reader, "last_region_scan_limited", False))
                scan_mode = str(getattr(reader, "last_scan_mode", "none") or "none")
                scan_time_ms = round(float(getattr(reader, "last_scan_time_s", 0.0) or 0.0) * 1000.0, 2)
                scan_confidence = round(float(getattr(reader, "last_confidence", 0.0) or 0.0), 4)
            except Exception:
                pass
        scan_in_progress = False
        if self._src is not None:
            try:
                scan_in_progress = bool(getattr(self._src, "scan_in_progress", False))
            except Exception:
                scan_in_progress = False
        return {
            "provider_auto_scan_enabled": bool(self.auto_scan_enabled),
            "provider_poll_interval_s": round(float(self.poll_interval), 3),
            "provider_allow_static_fallback": bool(self.allow_static_fallback),
            "provider_max_scan_regions_mb": int(self.max_scan_regions_mb),
            "provider_full_heap_scan": int(self.max_scan_regions_mb) <= 0,
            "provider_cy_memscan": _cy_memscan.backend_info(),
            "provider_anchor_strength": self._last_anchor_strength,
            "provider_anchor_skip_reason": self._last_anchor_skip_reason,
            "provider_scan_in_progress": scan_in_progress,
            "provider_region_scan_mb": region_mb,
            "provider_region_scan_limited": limited,
            "provider_scan_mode": scan_mode,
            "provider_scan_time_ms": scan_time_ms,
            "provider_scan_confidence": scan_confidence,
        }


# ───────── selftest ─────────

def _selftest():
    events = []
    def on_uid(u): events.append(("uid", u)); print(f"[evt] uid={u}")
    def on_hp(c, m): events.append(("hp", c, m)); print(f"[evt] HP={c}/{m}")
    def on_status(mode, err): print(f"[status] mode={mode!r} err={err!r}")
    def on_cd(cds):
        events.append(("cd", len(cds)))
        print(f"[evt] skill_cds change: {len(cds)} entries")
    def on_res(r): events.append(("res", r)); print(f"[evt] resources={r}")
    def on_prof(pid, name): events.append(("prof", pid, name)); print(f"[evt] profession={pid} name={name!r}")
    def on_dead(d): events.append(("dead", d)); print(f"[evt] dead={d}")

    p = MemSelfStateProvider(
        on_uid_change=on_uid, on_hp_change=on_hp, on_status_change=on_status,
        on_skill_cd_change=on_cd, on_resources_change=on_res,
        on_profession_change=on_prof, on_dead_change=on_dead,
    )
    p.start()
    try:
        for i in range(8):
            time.sleep(1)
            ls = p.last_snap
            print(f"  tick {i+1} mode={p.mode} uid={ls.uid if ls else None} "
                  f"cds={len(ls.skill_cds) if (ls and ls.skill_cds) else 0}")
    finally:
        p.stop()
    print(f"\n events captured: {len(events)}")


if __name__ == "__main__":
    _selftest()

