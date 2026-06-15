"""mem_state_bridge - 把 MemSelfStateProvider 接到现有 game_state / dps_tracker.

主程序集成 (在 sao_gui 初始化 packet_engine + dps_tracker 之后):

    from mem_probe.il2cpp.mem_state_bridge import MemStateBridge
    self._mem_bridge = MemStateBridge(
        state_mgr=self._state_mgr,        # game_state.GameStateManager
        dps_tracker=self._dps_tracker,    # 可 None
    )
    self._mem_bridge.start()

设计要点:
  - 内存源失败时自动转 TCP (MemSelfStateProvider 内置)
  - 任意子组件 (dps_tracker / state_mgr) 缺失都不会崩
  - 全部回调内部 try/except, 避免回调异常影响内存源主循环
  - 读不到 bundle 时 start() 不抛, 只把 mode 标 'error' 并打日志
"""
from __future__ import annotations

import os
import sys
import threading
import time
import traceback
from typing import Any, Callable, Optional

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(_HERE)))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from mem_probe.il2cpp.mem_self_state_provider import MemSelfStateProvider
from mem_probe.il2cpp.mem_state_anchor import AnchorMemoryReader, AnchorPack
from mem_probe.il2cpp.field_authority import FieldAuthority, ProbeReason, Source


class MemStateBridge:
    """把内存读出的 self 状态 push 到 game_state / dps_tracker."""

    def __init__(self,
                 state_mgr: Any = None,
                 dps_tracker: Any = None,
                 dps_overlay: Any = None,
                 hp_overlay: Any = None,
                 auto_key_engine: Any = None,
                 boss_raid_engine: Any = None,
                 packet_bridge: Any = None,
                 dump_id: str = "ef9ef95a",
                 enable_extended: bool = True,
                 auto_scan_enabled: bool = True,
                 poll_interval: Optional[float] = None,
                 allow_static_fallback: bool = True,
                 max_scan_regions_mb: int = 0,
                 on_log: Optional[Callable[[str], None]] = None):
        self.state_mgr = state_mgr
        self.dps_tracker = dps_tracker
        self.dps_overlay = dps_overlay
        self.hp_overlay = hp_overlay
        self.auto_key_engine = auto_key_engine
        self.boss_raid_engine = boss_raid_engine
        self.packet_bridge = packet_bridge
        self._on_log = on_log or (lambda m: print(m))
        self._provider: Optional[MemSelfStateProvider] = None
        self._dump_id = dump_id
        self._enable_extended = enable_extended
        self._auto_scan_enabled = bool(auto_scan_enabled)
        self._poll_interval = poll_interval
        self._allow_static_fallback = bool(allow_static_fallback)
        self._max_scan_regions_mb = int(max_scan_regions_mb or 0)

        # 状态 (供外部查询)
        self.mode: str = "init"
        self.last_error: str = ""
        self.last_uid: int = 0
        self.last_hp: int = 0
        self.last_max_hp: int = 0
        self.last_profession_id: int = 0
        self.last_char_name: str = ""
        self.last_skill_cd_count: int = 0
        self.last_resources: dict = {}
        self.last_is_dead: bool = False
        # Phase 5: granular per-field ownership. PacketBridge consumes this map so
        # one bad memory reader no longer flips the whole subsystem to TCP.
        self._field_authority = FieldAuthority()
        if self.packet_bridge is not None:
            fn = getattr(self.packet_bridge, 'set_field_authority', None)
            if callable(fn):
                try:
                    fn(self._field_authority)
                except Exception:
                    pass
        # entity-HP arm (MEM-driven boss/entity HP; additive, never fights live TCP)
        self._entity_enabled: bool = True
        self._entity_provider: Any = None
        self._entity_thread: Optional[threading.Thread] = None
        self._entity_stop = threading.Event()
        self._entity_interval: float = 1.0
        # Cadence policy: memory mode keeps the full 1s O(N) entity scan (mem is the
        # primary source). In a TCP-primary mode (hybrid/auto) the snapshot+damage
        # harvest is only a SUPPLEMENT (names / boss locate / cross-check), so it backs
        # off to _entity_interval_slow once a boss is located + base acquired — the O(1)
        # boss loop carries the live boss bar. This is the crowd / 20-player lag fix:
        # the per-tick O(N) GIL-holding reads stop scaling with entity/player count.
        self._entity_interval_fast: float = 1.0   # cold-start / memory-mode cadence
        self._entity_interval_slow: float = 4.0   # hybrid steady-state harvest cadence
        self.last_entities: list = []
        self.last_boss_mem: Optional[dict] = None
        # sticky "correct base acquired" flag: once a non-empty entity snapshot is
        # read (proves ZEntityMgr base is correct + entities readable), this latches
        # True for the session and never resets. It is the signal the boss-bar feeder
        # uses to switch boss-break ownership from TCP to MEM in hybrid/auto/memory.
        self._base_acquired: bool = False
        # MEM-authoritative boss break snapshot (stage + extinction gauge%), refreshed
        # per entity tick. Shield is NOT here — shield always comes from TCP.
        self.last_boss_break: Optional[dict] = None
        # boss-action feed (cast_skill_id edge-detect + state) -> bossraid/autokey
        self._boss_action_tracker: Any = None
        self.last_boss_actions: list = []
        self._boss_cast_thread: Optional[threading.Thread] = None
        self._boss_cast_stop = threading.Event()
        self._boss_cast_interval: float = 0.08   # ~12.5Hz boss-only cast poll (low latency)
        # In a TCP-primary mode the O(1) boss loop owns the boss bar / break; throttle
        # its state_mgr push + break recompute to ~3.5Hz (the UI pulls last_boss_break
        # at 20Hz, so this is ample and far fresher than the old 1Hz entity loop).
        self._boss_fast_push_min_dt: float = 0.28
        self._last_boss_fast_push: float = 0.0
        self._named: dict = {}          # uuid -> resolved name (push to tracker once)
        self._last_scene_pushed: tuple = (0, "")
        # live map/scene display-name reader (SceneTable + localization pool); heavy
        # one-shot build runs on a background thread, then lookups are an O(1) cached dict.
        self._map_reader: Any = None
        self._map_reader_built: bool = False
        self._map_reader_busy: bool = False
        self._damage_reader: Any = None
        self.last_mem_damage: dict = {}   # uid(=uuid>>16) -> MEM damage total
        self._mem_dmg_logged: bool = False
        self._mem_dmg_gate_logged: bool = False
        self._mem_dmg_empty_logged: bool = False
        self._mem_dmg_err_logged: bool = False
        self._entity_diag_logged: bool = False
        # JSON name self-heal: TCP sends only template ids, so read the game's real NAME
        # from memory and overlay it into the runtime name cache when it differs.
        # Shared TcpNameCache (PacketBridge owns it); the route-2 table walk overlays
        # authoritative id->name into it. Lazily fetched via _mem_name_cache().
        self._name_cache = None
        self._name_cache_checked: bool = False
        self._name_overlay_writer = None
        self._nr = None
        self._nr_tried = False
        # nameplate name harvest: read the game's *rendered* over-head name from the UI
        # nameplate widgets (uuid->name) and overlay it where the offline JSON is wrong
        # or missing. The displayed name isn't on the entity/combat-row, only the UI.
        # The first sweep is heavy (~8s, full private heap) so it runs in a one-shot
        # background thread; warm sweeps reuse a region hint (~0.1s). Each base is
        # resolved once, then never harvested again (no ongoing cost).
        self._np_reader: Any = None
        self._np_resolved_bases: set = set()
        self._np_harvest_busy: bool = False
        self._np_bootstrapped: bool = False   # swept npcs at least once (snap has none)
        self._np_last_harvest: float = 0.0
        self._np_harvest_interval: float = 8.0    # min gap between sweeps
        self._np_periodic_interval: float = 60.0  # idle re-sweep (warm ~0.1s) for new mobs/npcs

    # ───────── public ─────────

    def start(self) -> bool:
        """启动后台线程. 失败返回 False (不抛). 成功返回 True."""
        if self._provider is not None:
            return True
        try:
            self._provider = MemSelfStateProvider(
                on_uid_change=self._on_uid,
                on_hp_change=self._on_hp,
                on_status_change=self._on_status,
                on_skill_cd_change=self._on_skill_cd,
                on_resources_change=self._on_resources,
                on_profession_change=self._on_profession,
                on_dead_change=self._on_dead,
                on_identity_change=self._on_identity,
                on_stamina_change=self._on_stamina,
                anchor_source=self._build_anchor_pack,
                enable_extended=self._enable_extended,
                auto_scan_enabled=self._auto_scan_enabled,
                poll_interval=self._poll_interval,
                allow_static_fallback=self._allow_static_fallback,
                max_scan_regions_mb=self._max_scan_regions_mb,
                dump_id=self._dump_id,
            )
            self._provider.start()
            self._log("[MemBridge] started (memory-first, TCP fallback enabled)")
            if self._entity_enabled:
                self._entity_stop.clear()
                self._entity_thread = threading.Thread(
                    target=self._entity_loop, name="mem-entity-hp", daemon=True)
                self._entity_thread.start()
            # direct print -- on_log is swallowed in hybrid; confirm start + dps_tracker wiring
            print(f"[MemBridge] start(): entity_enabled={self._entity_enabled} "
                  f"dps_tracker={'ok' if self.dps_tracker else 'None'}")
            return True
        except Exception as e:
            self._log(f"[MemBridge] start failed: {e}")
            traceback.print_exc()
            self._provider = None
            return False

    # ───────── data-source cadence (TCP-primary supplement vs memory-primary) ─────────

    def _mode(self) -> str:
        return str(getattr(self.packet_bridge, "_data_source_mode", "") or "").lower()

    def _mem_is_supplement(self) -> bool:
        """True when memory is NOT the primary source (TCP-primary modes: hybrid /
        auto / tcp). Mirrors dps_tracker.set_mem_primary(ds == 'memory'): in these
        modes mem only supplements TCP, so its O(N) work backs off."""
        return self._mode() != "memory"

    def _next_entity_interval(self) -> float:
        """Entity-loop sleep. Memory mode keeps the full 1s scan. A TCP-primary mode
        backs the O(N) snapshot+damage harvest off to the slow interval once a boss is
        located and the base is acquired — but stays fast while cold so boss-locate /
        base acquisition (which flips boss-break ownership to MEM) isn't delayed; a boss
        despawn (last_boss_mem→None) auto-re-tightens to fast."""
        if not self._mem_is_supplement():
            return self._entity_interval_fast
        cold = (self.last_boss_mem is None) or (not self._base_acquired)
        return self._entity_interval_fast if cold else self._entity_interval_slow

    def _boss_obj_is(self, prov, obj: int, uuid: int) -> bool:
        """O(1) guard that the cached boss obj still hosts `uuid` (pool reuse between
        slow snapshots would otherwise let read_combat report a recycled occupant's HP
        as the boss). Can't verify (no pm) → don't block (status quo); read error →
        treat stale and yield to the slow loop's relocation."""
        try:
            pm = getattr(prov, "_pm", None)
            if pm is None:
                return True
            emr = getattr(prov, "_emr", None)
            off = int(getattr(emr, "off_ent_uuid", 0xC0)) if emr is not None else 0xC0
            return int(pm.read_u64(obj + off) or 0) == int(uuid)
        except Exception:
            return False

    def _boss_fast_push(self, boss: dict, c: dict) -> None:
        """O(1) boss bar / break from the read_combat dict the cast loop already has.
        Keeps the cached boss dict fresh between slow snapshots, and (throttled ~3.5Hz)
        recomputes last_boss_break + pushes boss HP — self-gated identically to the slow
        entity loop (writes boss_hp_source='memory' only when TCP isn't owning the bar).
        Supplement-mode only; in memory mode the 1Hz entity loop owns these."""
        for k in ("cur_hp", "max_hp", "hp_pct", "breaking_stage", "extinction",
                  "max_extinction", "stun", "max_stun", "stop_breaking_ticking"):
            if c.get(k) is not None:
                boss[k] = c[k]
        now = time.monotonic()
        if (now - self._last_boss_fast_push) < self._boss_fast_push_min_dt:
            return
        self._last_boss_fast_push = now
        bs = c.get("breaking_stage")
        ext = c.get("extinction")
        mext = c.get("max_extinction")
        pct = (ext / mext) if (ext and mext and mext > 0) else 0.0
        if pct == 0.0:
            stun = c.get("stun")
            mstun = c.get("max_stun")
            if mstun and mstun > 0 and stun is not None:
                pct = max(0.0, min(1.0, stun / mstun))
        sbt = c.get("stop_breaking_ticking")
        if sbt is None:
            sbt = boss.get("stop_breaking_ticking", False)
        self.last_boss_break = {
            "breaking_stage": int(bs) if isinstance(bs, int) else -1,
            "extinction_pct": max(0.0, min(1.0, pct)),
            "has_break_data": bool(isinstance(bs, int) and bs >= 0),
            "stop_breaking_ticking": bool(sbt),
        }
        if self.state_mgr is None or not c.get("max_hp"):
            return
        st = getattr(self.state_mgr, "state", None)
        src_now = str(getattr(st, "boss_hp_source", "none") or "none")
        if src_now not in ("none", "memory", "estimate"):
            return
        cur = int(c.get("cur_hp") or 0)
        mx = int(c.get("max_hp") or 0)
        hp_pct = c.get("hp_pct")
        hp_pct = float(hp_pct) if hp_pct is not None else (cur / mx if mx > 0 else 0.0)
        upd = dict(boss_current_hp=cur, boss_total_hp=mx,
                   boss_hp_est_pct=max(0.0, min(1.0, hp_pct)),
                   boss_hp_source="memory")
        if isinstance(bs, int):
            upd["boss_breaking_stage"] = bs
        try:
            self.state_mgr.update(**upd)
        except Exception:
            pass

    def _entity_loop(self):
        """Poll ZEntityMgr for live entity/boss HP and fill the boss bar pre-pull.

        Additive only: pushes boss HP with source='memory' ONLY when TCP is not
        actively owning the boss bar (source in none/memory/estimate), so it never
        overrides a live packet HP during combat. Surfaces ``last_entities`` for
        plugins/UI (all visible mobs, real-time, including off-screen/pre-pull).
        """
        prov = None
        while not self._entity_stop.is_set():
            try:
                if prov is None:
                    p = self._provider
                    src = getattr(p, "_src", None) if p is not None else None
                    # NOTE: do NOT require self.last_uid > 0. In hybrid the self-state
                    # path may never resolve (full-heap scan disabled outside memory mode),
                    # but ZEntityMgr / DamageDataMgr locate independently (best-by-count +
                    # klass sentinel). Start as soon as the StaticDpsSource is available.
                    if src is not None:
                        from mem_probe.il2cpp.mem_entity_provider import MemEntityProvider
                        prov = MemEntityProvider(src, self_uid=int(self.last_uid or 0))
                        self._entity_provider = prov
                        try:
                            from mem_probe.il2cpp.mem_damage_reader import MemDamageReader
                            self._damage_reader = MemDamageReader(src)
                        except Exception as _dr_exc:
                            self._damage_reader = None
                            print(f"[MemBridge.entity] MemDamageReader init failed: {_dr_exc}")
                        # boss-action tracker: edge-detect cast_skill_id on the live
                        # entity snapshot (reuses prov._ecr / prov._pm; never blocks).
                        try:
                            from mem_probe.il2cpp.mem_boss_action_reader import (
                                BossActionTracker, BossDurationProbe)
                            # on_event left unset: the bridge forwards explicitly from
                            # both loops (fast = low-latency cast edges; 1Hz = offensive
                            # windows) so a record is never double-forwarded.
                            _prov_src = getattr(prov, "_src", None)
                            self._boss_action_tracker = BossActionTracker(
                                prov._ecr, pm=prov._pm,
                                name_resolver=self._name_resolver(),
                                duration_probe=BossDurationProbe(
                                    prov._pm,
                                    resolver=(_prov_src.sr if _prov_src is not None else None)))
                            self._boss_cast_stop.clear()
                            self._boss_cast_thread = threading.Thread(
                                target=self._boss_cast_loop, name="mem-boss-cast", daemon=True)
                            self._boss_cast_thread.start()
                        except Exception as _ba_exc:
                            self._boss_action_tracker = None
                            print(f"[MemBridge.entity] BossActionTracker init failed: {_ba_exc}")
                        # direct print -- bridge on_log is swallowed in hybrid (see _poll_mem_damage)
                        print(f"[MemBridge.entity] loop active: src=ok damage_reader="
                              f"{'ok' if self._damage_reader else 'None'} dps_tracker="
                              f"{'ok' if self.dps_tracker else 'None'} last_uid={self.last_uid}")
                    else:
                        if not self._entity_diag_logged:
                            self._entity_diag_logged = True
                            print(f"[MemBridge.entity] waiting on StaticDpsSource "
                                  f"(provider={'set' if p is not None else 'None'}, _src=None)")
                        self._entity_stop.wait(self._entity_interval)
                        continue
                prov.set_self_uid(int(self.last_uid or 0))
                # MEM damage table first (fast DamageDataMgr locate) so the DPS badge shows
                # before the slow first ZEntityMgr best-by-count scan in snapshot().
                self._poll_mem_damage()
                snap = prov.snapshot()
                self.last_entities = snap
                if snap and not self._base_acquired:
                    # non-empty snapshot -> ZEntityMgr base is correct and entities
                    # are readable. Latch sticky (never resets for this bridge).
                    self._base_acquired = True
                    self._maybe_build_break_cache()
                # resolve display names from MEM: ZEntity.BaseId -> offline name table
                # (no TCP) and feed the uuid->name path the boss bar / drilldown read.
                if snap:
                    # authoritative over-head name from the UI nameplate (overlays the
                    # JSON when wrong/missing); runs in the background, sets _named for
                    # corrected bases so the loop below picks the corrected name up.
                    self._maybe_harvest_nameplates(snap)
                    nr = self._name_resolver()
                    for e in snap:
                        uuid = int(e["uuid"])
                        bid = int(e.get("base_id") or 0)
                        cached = self._named.get(uuid)
                        if cached is None or cached[0] != bid:   # resolve+push once / per base_id
                            # bosses live in the boss table, not monster -> try boss first
                            nm = ((nr.boss(bid, default="") or nr.monster(bid, default=""))
                                  if (nr and bid) else "") or ""
                            self._named[uuid] = (bid, nm)
                            if nm and self.dps_tracker is not None:
                                try:
                                    self.dps_tracker.update_monster_info(uuid, nm)
                                except Exception:
                                    pass
                        else:
                            nm = cached[1]
                        if nm:
                            e["name"] = nm
                    if len(self._named) > 1024:
                        self._named.clear()
                boss = max(snap, key=lambda e: e["max_hp"]) if snap else None
                self.last_boss_mem = boss
                # MEM-authoritative boss break (stage + extinction gauge%); shield
                # stays TCP-only and is never derived here.
                if boss:
                    bs = boss.get("breaking_stage")
                    ext = boss.get("extinction")
                    mext = boss.get("max_extinction")
                    pct = (ext / mext) if (ext and mext and mext > 0) else 0.0
                    if pct == 0.0:
                        stun = boss.get("stun")
                        mstun = boss.get("max_stun")
                        if mstun and mstun > 0 and stun is not None:
                            pct = max(0.0, min(1.0, stun / mstun))
                    self.last_boss_break = {
                        "breaking_stage": int(bs) if isinstance(bs, int) else -1,
                        "extinction_pct": max(0.0, min(1.0, pct)),
                        "has_break_data": bool(isinstance(bs, int) and bs >= 0),
                        "stop_breaking_ticking": bool(boss.get("stop_breaking_ticking")),
                    }
                else:
                    self.last_boss_break = None
                # boss-action feed: rich per-tick edge-detect + duration upgrade.
                # Forward the boss's current record every tick so offensive windows
                # (breaking/overdrive/stun) reach the linkage even without a cast edge;
                # the fast loop handles low-latency cast-start separately.
                if self._boss_action_tracker is not None:
                    try:
                        self.last_boss_actions = self._boss_action_tracker.update(snap, boss)
                        if boss:
                            buuid = int(boss.get("uuid") or 0)
                            brec = next((r for r in self.last_boss_actions
                                         if int(r.get("boss_uuid") or 0) == buuid), None)
                            if brec is not None:
                                self._on_boss_action_event(brec)
                    except Exception:
                        traceback.print_exc()
                if boss and self.state_mgr is not None:
                    st = getattr(self.state_mgr, "state", None)
                    src_now = str(getattr(st, "boss_hp_source", "none") or "none")
                    if src_now in ("none", "memory", "estimate"):
                        upd = dict(
                            boss_current_hp=int(boss["cur_hp"]),
                            boss_total_hp=int(boss["max_hp"]),
                            boss_hp_est_pct=max(0.0, min(1.0, float(boss["hp_pct"]))),
                            boss_hp_source="memory",
                        )
                        bs = boss.get("breaking_stage")
                        if isinstance(bs, int):
                            upd["boss_breaking_stage"] = bs
                        try:
                            self.state_mgr.update(**upd)
                        except Exception:
                            pass
                # scene name from memory: prefer the live SceneTable display name (the
                # top-left UI map name, e.g. "协会活动中心") read via SceneConfigMgr.
                # curSceneId_ — CharSerialize.SceneData is null in many scenes.
                if self.state_mgr is not None:
                    smid, nm = self._current_scene_named()
                    if smid and (smid, nm) != self._last_scene_pushed:
                        self._last_scene_pushed = (smid, nm)
                        upd = {"dungeon_scene_id": smid}
                        if nm:
                            upd["dungeon_name"] = nm
                        try:
                            self.state_mgr.update(**upd)
                        except Exception:
                            pass
            except Exception:
                traceback.print_exc()
            self._entity_stop.wait(self._next_entity_interval())

    def _mem_name_cache(self):
        """The shared TcpNameCache (PacketBridge owns it) used as the name overlay sink.
        Lazily fetched once; None when unavailable (then the self-heal simply no-ops)."""
        if not self._name_cache_checked:
            self._name_cache_checked = True
            self._name_cache = getattr(self.packet_bridge, "tcp_name_cache", None)
        return self._name_cache

    def _poll_mem_damage(self) -> None:
        """Read the game's DamageDataMgr table -> dps_tracker (cross-check badge / memory
        mode). Independent of the entity snapshot, so it runs BEFORE the slow first
        ZEntityMgr scan and the MEM badge appears within ~1-2s. uid = playerUuid >> 16
        (== CharSerialize.CharId; dps_tracker keys entities by uuid>>16)."""
        # NOTE: print() directly (not self._log) -- in hybrid the bridge's on_log is
        # UnifiedDataSource._on_bridge_log, which swallows messages. Direct prints are
        # the only way the user sees what this path does. One-shot per outcome.
        if self._damage_reader is None or self.dps_tracker is None:
            if not self._mem_dmg_gate_logged:
                self._mem_dmg_gate_logged = True
                print(f"[MemBridge.dmg] blocked: damage_reader="
                      f"{'ok' if self._damage_reader else 'None'} dps_tracker="
                      f"{'ok' if self.dps_tracker else 'None'}")
            return
        try:
            totals = self._damage_reader.read_player_totals()
            if not totals:
                if not self._mem_dmg_empty_logged:
                    self._mem_dmg_empty_logged = True
                    print("[MemBridge.dmg] read_player_totals() empty -- "
                          "DamageDataMgr not located yet or no combat damage recorded.")
                return
            md = {int(u) >> 16: int(v) for u, v in totals.items()}
            self.last_mem_damage = md
            self.dps_tracker.set_mem_damage(md)
            ds = self._mode()
            self.dps_tracker.set_mem_primary(ds == 'memory')
            try:
                skills = {}
                for u, _v in sorted(totals.items(), key=lambda x: -x[1])[:6]:
                    sd = self._damage_reader.read_player_skill_damage(int(u))
                    if sd:
                        skills[int(u) >> 16] = sd
                if skills:
                    self.dps_tracker.set_mem_skill_damage(skills)
            except Exception:
                pass
            if not self._mem_dmg_logged:
                self._mem_dmg_logged = True
                top = max(totals.items(), key=lambda x: x[1])
                print(f"[MemBridge] MEM damage table live: {len(totals)} players; "
                      f"top uuid={top[0]} (uid={top[0] >> 16}) = {top[1]:,}; "
                      f"mode={ds!r} mem_primary={ds == 'memory'}")
        except Exception as exc:
            if not self._mem_dmg_err_logged:
                self._mem_dmg_err_logged = True
                import traceback
                traceback.print_exc()
                print(f"[MemBridge.dmg] read failed: {exc}")

    def _on_boss_action_event(self, rec: dict) -> None:
        """Forward a boss cast/state edge to the boss raid engine (skill_id-accurate)."""
        eng = self.boss_raid_engine
        if eng is None:
            return
        fn = getattr(eng, "on_mem_boss_action", None)
        if callable(fn):
            try:
                fn(rec)
            except Exception:
                traceback.print_exc()

    def _boss_cast_loop(self):
        """Low-latency boss-only cast poll (~12.5Hz).

        Three-phase nogil batch: all RPM calls go through Cython read_u64_many /
        read_u32_many / read_combat_batch so the GIL is never held during cross-
        process reads.  The UI thread (Tk mainloop) is never blocked by this loop.
        """
        while not self._boss_cast_stop.is_set():
            try:
                tr = self._boss_action_tracker
                boss = self.last_boss_mem
                prov = self._entity_provider
                if tr is not None and boss and prov is not None:
                    obj = int(boss.get("obj") or 0)
                    uuid = int(boss.get("uuid") or 0)
                    if obj and uuid:
                        self._boss_cast_tick(obj, uuid, boss, prov, tr)
            except Exception:
                pass
            self._boss_cast_stop.wait(self._boss_cast_interval)

    def _boss_cast_tick(self, obj: int, uuid: int, boss: dict, prov, tr) -> None:
        """Single boss-cast tick with batched nogil RPM reads.

        Phase 1: read_boss_combat(obj) — cached fast path (9 RPMs hot / ~26 cold)
        Phase 2: read_u64_many([uuid_addr, sm_addr, comp_addr]) — nogil batch
        Phase 3: read_u32_many([curstate_addr, skill_addr, stage_addr]) — nogil batch
        """
        _MINP, _MAXP = 0x10000, 0x7FFF_FFFF_FFFF
        ecr = getattr(prov, "_ecr", None)
        pm = getattr(prov, "_pm", None)
        if ecr is None or pm is None:
            return

        # ── Phase 1: boss combat with obj-pointer caching ──
        c = ecr.read_boss_combat(obj)
        if c is None:
            return

        # ── Phase 2: batch u64 reads (uuid + sm + optional comp klass) ──
        emr = getattr(prov, "_emr", None)
        off_uuid = int(getattr(emr, "off_ent_uuid", 0xC0)) if emr is not None else 0xC0
        probe = getattr(tr, "_probe", None)
        off_sm = int(getattr(probe, "off_statemachine", 0x70)) if probe is not None else 0x70

        ss = self._boss_skill_state()
        comp, comp_klass = ss.cached_comp(obj) if ss is not None else (0, 0)

        aux_addrs = [obj + off_uuid, obj + off_sm]
        if comp:
            aux_addrs.append(comp)          # klass ptr at comp+0x0
        aux = pm.read_u64_many(aux_addrs)
        if aux is None or len(aux) < 2:
            return

        # uuid validation
        if int(aux[0] or 0) != uuid:
            return

        sm_ptr = int(aux[1] or 0)
        klass_now = int(aux[2] or 0) if comp and len(aux) > 2 else 0

        # comp klass fast validation (0 RPM — value already in aux)
        comp_ok = (comp != 0 and comp_klass != 0
                   and klass_now != 0 and klass_now == comp_klass)

        # supplement modes: push boss bar / break from combat dict
        if self._mem_is_supplement():
            self._boss_fast_push(boss, c)

        skill = int(c.get("cast_skill_id") or 0)

        # ── Phase 3: batch u32 reads (curstate + conditional skill fields) ──
        seq_addrs = []
        seq_keys = []       # track what's at each index

        if probe is not None and _MINP <= sm_ptr <= _MAXP:
            off_curstate = int(getattr(probe, "off_sm_curstate", 0x20))
            seq_addrs.append(sm_ptr + off_curstate)
            seq_keys.append("actor")

        if not skill and comp_ok and ss is not None:
            ss.ensure_offsets(comp)
            if ss._off_skill:
                seq_addrs.append(comp + ss._off_skill)
                seq_keys.append("skill")
            if ss._off_stage:
                seq_addrs.append(comp + ss._off_stage)
                seq_keys.append("stage")

        actor = None
        if seq_addrs:
            seq = pm.read_u32_many(seq_addrs)
            if seq and len(seq) == len(seq_addrs):
                for i, key in enumerate(seq_keys):
                    v = seq[i]
                    if v is None:
                        continue
                    iv = int(v)
                    if iv >= (1 << 31):
                        iv -= (1 << 32)
                    if key == "actor":
                        actor = iv
                    elif key == "skill":
                        if iv and iv not in (0, 1):
                            skill = iv

        rec = tr.update_fast(uuid, skill, actor, obj=obj)
        if rec is not None:
            self._on_boss_action_event(rec)

    def stop(self):
        self._entity_stop.set()
        self._boss_cast_stop.set()
        if self._boss_cast_thread:
            try:
                self._boss_cast_thread.join(timeout=2.0)
            except Exception:
                pass
            self._boss_cast_thread = None
        self._boss_action_tracker = None
        if self._entity_thread:
            try:
                self._entity_thread.join(timeout=2.0)
            except Exception:
                pass
            self._entity_thread = None
        self._entity_provider = None
        if self._provider:
            try:
                self._provider.stop()
            except Exception:
                pass
            self._provider = None

    def force_mode(self, mode: str):
        """'tcp' 或 'memory' — 主程序可强制切换."""
        if self._provider:
            self._provider.force_mode(mode)

    def _name_resolver(self):
        """Lazily fetch the app's offline id->name resolver (cached; None if absent)."""
        if not self._nr_tried:
            self._nr_tried = True
            try:
                from tools.tablekit.name_tables import names as _NR
                self._nr = _NR
            except Exception:
                self._nr = None
        return self._nr

    def _current_scene_named(self):
        """(scene_id, localized_name) for the current scene. Prefers the live
        SceneTable name via SceneConfigMgr.curSceneId_ (reliable); the reader's first
        build is heavy (heap scans) so it runs once on a background thread, until then
        we fall back to the provider's scene id + the offline dungeon table."""
        # The TCP/provider-known scene id (read straight from CharSerialize.SceneData
        # by the self provider) is the most specific anchor for the SceneConfigMgr
        # value-scan: passing it turns an N-way known-id set scan into a single-needle
        # search. Harmless when 0 (reader falls back to the full known-id set).
        ls = getattr(self._provider, "last_snap", None)
        smid = int(getattr(ls, "scene_map_id", 0) or 0) if ls else 0
        r = self._map_reader
        if r is not None and r.ready:
            sid = r.current_scene_id(hint_scene_id=smid)
            if sid:
                return sid, r.name_for_scene(sid)
        if not self._map_reader_built and not self._map_reader_busy:
            self._map_reader_busy = True
            threading.Thread(target=self._build_map_reader, name="mem-mapname",
                             daemon=True).start()
        nr = self._name_resolver()
        nm = (nr.dungeon(smid, default="") if (nr and smid) else "")
        return smid, nm

    def _boss_skill_state(self):
        """Lazy BossSkillStateReader: boss ZStateSkillComp.curSkillId_ 当 cast_skill_id
        attr 不可靠时的兜底出招源 (comp ptr 缓存, 热路径 O(1))。无源时 None。"""
        r = getattr(self, "_boss_skill_reader", None)
        if r is not None:
            return r
        if getattr(self, "_boss_skill_reader_tried", False):
            return None
        self._boss_skill_reader_tried = True
        try:
            src = getattr(self._entity_provider, "_src", None) \
                or getattr(self._provider, "_src", None)
            if src is None or getattr(src, "sr", None) is None:
                return None
            from mem_probe.il2cpp.mem_boss_skill_state_reader import BossSkillStateReader
            self._boss_skill_reader = BossSkillStateReader(src)
            return self._boss_skill_reader
        except Exception:
            return None

    def _build_map_reader(self) -> None:
        try:
            src = getattr(self._entity_provider, "_src", None) \
                or getattr(self._provider, "_src", None)
            if src is None or getattr(src, "sr", None) is None:
                return
            from mem_probe.il2cpp.mem_map_name_reader import MapNameReader
            r = MapNameReader(src)
            if r.build():
                self._map_reader = r
                print(f"[MemBridge] map-name reader ready: {len(r._scene_names)} scenes")
        except Exception:
            traceback.print_exc()
        finally:
            self._map_reader_built = True
            self._map_reader_busy = False

    def _harvest_pm(self):
        """The shared StarProcess handle for the nameplate sweep (or None)."""
        pm = getattr(self._entity_provider, "_pm", None)
        if pm is not None:
            return pm
        src = getattr(self._provider, "_src", None)
        sr = getattr(src, "sr", None)
        return getattr(sr, "pm", None)

    def _maybe_build_break_cache(self) -> None:
        """One-shot background build of the full MonsterTable BreakingContinueTime cache."""
        try:
            import threading
            from engines.break_time_lookup import build_full_cache, _cache
            if len(_cache) > 50:
                return
            threading.Thread(target=build_full_cache, name="break-cache-build", daemon=True).start()
        except Exception:
            pass

    def _maybe_harvest_nameplates(self, snap) -> None:
        """Trigger a (throttled, background) nameplate name harvest.

        Fires when a visible monster base_id isn't confirmed yet, once to bootstrap
        NPCs (the snap carries no npcs), and on a slow idle timer to catch new
        mobs/npcs. The over-head name is the authority the JSON should match; this
        corrects the JSON (e.g. 114 '木桩' -> '敌方木桩') and fills gaps (npc names),
        all auto-offset, no TCP.
        """
        try:
            snap_bases = {int(e.get("base_id") or 0) for e in snap}
            snap_bases.discard(0)
            now = time.time()
            unresolved = (bool(snap_bases - self._np_resolved_bases)
                          or not self._np_bootstrapped
                          or (now - self._np_last_harvest) >= self._np_periodic_interval)
            if (not unresolved or self._np_harvest_busy
                    or (now - self._np_last_harvest) < self._np_harvest_interval):
                return
            pm = self._harvest_pm()
            if pm is None:
                return
            self._np_harvest_busy = True
            self._np_last_harvest = now
            threading.Thread(target=self._run_nameplate_harvest, args=(pm,),
                             name="mem-nameplate", daemon=True).start()
        except Exception:
            traceback.print_exc()

    def _run_nameplate_harvest(self, pm) -> None:
        """Worker: enumerate monsters+npcs, sweep their nameplate widgets, and overlay
        the corrected/missing name -- routed to the 'monster' kind for mobs and the
        'npc' kind for NPCs (the plate-type field picks the NPC name over its title).
        Monster names are also pushed to the live panel; NPCs are JSON-only.
        """
        try:
            prov = self._entity_provider
            # no HP gate -- include non-combat NPCs that snapshot() filters out
            full = (prov.enumerate_ids(include_monsters=True, include_npcs=True)
                    if prov is not None else None)
            self._np_bootstrapped = True
            if not full:
                return
            uuid_to_base: dict = {}
            base_kind: dict = {}
            base_to_uuids: dict = {}
            for e in full:
                b = int(e.get("base_id") or 0)
                u = int(e.get("uuid") or 0)
                if b <= 0 or u <= 0:
                    continue
                uuid_to_base[u] = b
                base_kind[b] = str(e.get("kind") or "monster")
                base_to_uuids.setdefault(b, []).append(u)
            if self._np_reader is None:
                from mem_probe.il2cpp.mem_nameplate_reader import NameplateReader
                self._np_reader = NameplateReader(pm)
            res = self._np_reader.harvest(uuid_to_base)
            names = res.get("names") or {}
            nr = self._name_resolver()
            cache = self._mem_name_cache()
            writer = None
            if cache is not None:
                try:
                    if self._name_overlay_writer is None:
                        from tools.tablekit.live_overlay_writer import LiveNameOverlayWriter
                        self._name_overlay_writer = LiveNameOverlayWriter(cache, min_confirmations=1)
                    writer = self._name_overlay_writer
                except Exception:
                    writer = None
            for base, nm in names.items():
                if base in self._np_resolved_bases:
                    continue                                  # handled once; no re-overlay
                self._np_resolved_bases.add(base)
                if not nm:
                    continue
                kind = base_kind.get(base, "monster")
                if kind not in ("monster", "boss", "npc"):
                    kind = "monster"
                try:
                    json_nm = (nr.resolve(kind, base, default="") if nr else "") or ""
                except Exception:
                    json_nm = ""
                if nm == json_nm:
                    continue                                  # JSON already correct
                # persist the authoritative game name into the overlay cache
                if writer is not None:
                    try:
                        writer.observe(kind, base, nm, context={"source": "nameplate"})
                    except Exception:
                        pass
                # combat entities also go to the live panel (npcs aren't shown there)
                if kind in ("monster", "boss"):
                    for u in base_to_uuids.get(base, []):
                        self._named[u] = (base, nm)
                        if self.dps_tracker is not None:
                            try:
                                self.dps_tracker.update_monster_info(u, nm)
                            except Exception:
                                pass
                print(f"[MemBridge.nameplate] {kind} base_id={base} "
                      f"{('JSON='+json_nm) if json_nm else '(JSON gap)'} -> '{nm}' overlaid")
        except Exception:
            traceback.print_exc()
        finally:
            self._np_harvest_busy = False

    def _build_anchor_pack(self) -> AnchorPack:
        """Build a semantic anchor pack from the live PacketBridge parser."""
        parser = getattr(self.packet_bridge, '_parser', None) if self.packet_bridge is not None else None
        if parser is None:
            return AnchorPack()
        return AnchorMemoryReader.build_anchor_from_parser(parser)

    # ───────── 回调实现 ─────────

    def _log(self, msg: str):
        try:
            self._on_log(msg)
        except Exception:
            pass

    def _on_uid(self, uid: int):
        self.last_uid = uid
        self._field_authority.record_success('identity', source=Source.MEMORY)
        self._log(f"[MemBridge] uid={uid}")
        # game_state.player_id 是 str
        if self.state_mgr is not None:
            try:
                self.state_mgr.update(player_id=str(uid))
            except Exception:
                traceback.print_exc()
        # dps_tracker
        if self.dps_tracker is not None:
            try:
                self.dps_tracker.set_self_uid(uid)
            except Exception:
                traceback.print_exc()
        if self.dps_overlay is not None:
            try:
                self.dps_overlay.set_self_uid(uid)
            except Exception:
                traceback.print_exc()

    def _on_hp(self, cur_hp: int, max_hp: int):
        self.last_hp = cur_hp
        self.last_max_hp = max_hp
        self._field_authority.record_success('hp', source=Source.MEMORY)
        # 主推 game_state — 所有面板订阅 GameState 即可
        if self.state_mgr is not None:
            try:
                pct = (float(cur_hp) / float(max_hp)) if max_hp > 0 else 1.0
                pct = max(0.0, min(1.0, pct))
                self.state_mgr.update(hp_current=int(cur_hp),
                                      hp_max=int(max_hp),
                                      hp_pct=pct)
            except Exception:
                traceback.print_exc()

    def _on_stamina(self, cur: int, total: int):
        # OriginEnergy 字段不是 runtime stamina (实际 stamina 在 EnergyItem.EnergyInfo
        # MapField 内, 当前未实现). 因此这里只在合理范围内推; 否则等 Phase 2 MapField.
        if self.state_mgr is None:
            return
        if total <= 0 or cur < 0 or cur > total * 4:
            return  # 数据可疑, 跳过避免污染 GameState
        try:
            self._field_authority.record_success('stamina', source=Source.MEMORY)
            pct = (float(cur) / float(total)) if total > 0 else 1.0
            pct = max(0.0, min(1.0, pct))
            self.state_mgr.update(stamina_current=int(cur),
                                  stamina_max=int(total),
                                  stamina_pct=pct,
                                  stamina_offline=False)
        except Exception:
            traceback.print_exc()

    def _on_identity(self, ident: dict):
        """level_base / season_exp / season_medal_level / fight_point."""
        if self.state_mgr is None:
            return
        try:
            kw = {}
            if ident.get('level_base'):
                kw['level_base'] = int(ident['level_base'])
            if ident.get('season_exp') is not None:
                kw['season_exp'] = int(ident['season_exp'])
            if ident.get('fight_point'):
                kw['fight_point'] = int(ident['fight_point'])
            # season_medal_level 不是 GameState 字段; 跳过
            if kw:
                if 'level_base' in kw:
                    self._field_authority.record_success('level', source=Source.MEMORY)
                self._field_authority.record_success('identity', source=Source.MEMORY)
                self.state_mgr.update(**kw)
                self._log(f"[MemBridge] identity {kw}")
        except Exception:
            traceback.print_exc()

    def _on_profession(self, profession_id: int, char_name: str):
        self.last_profession_id = profession_id
        self.last_char_name = char_name
        self._field_authority.record_success('profession', source=Source.MEMORY)
        if char_name:
            self._field_authority.record_success('name', source=Source.MEMORY)
            self._field_authority.record_success('identity', source=Source.MEMORY)
        self._log(f"[MemBridge] profession={profession_id} name={char_name!r}")
        if self.state_mgr is not None:
            try:
                kw = {"profession_id": int(profession_id)}
                # 解析职业名 (尽力而为, 失败不阻塞)
                try:
                    from packet_parser import PROFESSION_NAMES  # type: ignore
                    pn = PROFESSION_NAMES.get(int(profession_id), '')
                    if pn:
                        kw["profession_name"] = pn
                except Exception:
                    pass
                if char_name:
                    kw["player_name"] = char_name
                self.state_mgr.update(**kw)
            except Exception:
                traceback.print_exc()

    def _on_skill_cd(self, cds: list):
        self.last_skill_cd_count = len(cds)
        if cds:
            self._field_authority.record_success('skills', source=Source.MEMORY)
        # 把 SkillCD list 转成 GameState.skill_slots 格式 (HUD/SkillFX/AutoKey 都用这个)
        if self.state_mgr is not None:
            try:
                from mem_probe.il2cpp.mem_skill_slots import convert as _conv
                slots = _conv(cds, self.last_profession_id, server_time_offset_ms=None)
                if slots:
                    self.state_mgr.update(skill_slots=slots)
            except Exception:
                traceback.print_exc()
        # 兼容旧接口: 如果 autokey 引擎实现了直接吃 SkillCD 的方法, 也喂一份
        if self.auto_key_engine is not None:
            for fn_name in ("on_skill_cds_update", "update_skill_cds", "set_skill_cds"):
                fn = getattr(self.auto_key_engine, fn_name, None)
                if callable(fn):
                    try:
                        fn(cds)
                    except Exception:
                        traceback.print_exc()
                    break

    def _on_resources(self, resources: dict):
        self.last_resources = dict(resources or {})

    def _on_dead(self, is_dead: bool):
        self.last_is_dead = bool(is_dead)
        self._log(f"[MemBridge] dead={is_dead}")
        if self.boss_raid_engine is not None:
            fn = getattr(self.boss_raid_engine, "on_self_dead_change", None)
            if callable(fn):
                try:
                    fn(is_dead)
                except Exception:
                    traceback.print_exc()

    @staticmethod
    def _classify_probe_failure(mode: str, err: str) -> ProbeReason:
        """Map provider status text to the Phase-5 typed failure reason.

        The current provider still reports status as `(mode, err)` strings, so this
        bridge performs a conservative text classification until the provider emits
        typed ProbeFailure values directly. Importantly, scan-in-progress and missing
        process are NOT counted as field failures by FieldAuthority.
        """
        text = f"{mode or ''} {err or ''}".lower()
        if "scan" in text and ("progress" in text or "in_progress" in text or "running" in text):
            return ProbeReason.SCAN_IN_PROGRESS
        if "process" in text or "openprocess" in text or "star.exe" in text or "pid" in text:
            return ProbeReason.PROCESS_MISSING
        if "anchor" in text or "relocate" in text or "klass" in text:
            return ProbeReason.ANCHOR_INVALID
        if "layout" in text or "sanity" in text or "drift" in text or "plaus" in text:
            return ProbeReason.LAYOUT_DRIFT
        return ProbeReason.SNAPSHOT_NONE

    def _on_status(self, mode: str, err: str):
        self.mode = mode
        self.last_error = err
        if err:
            self._log(f"[MemBridge] mode={mode!r} err={err!r}")
        else:
            self._log(f"[MemBridge] mode={mode!r}")
        if self.state_mgr is not None:
            try:
                self.state_mgr.update(data_source=str(mode or ''))
            except Exception:
                pass
        # 切换 packet_bridge 的权威源状态
        if self.packet_bridge is not None:
            fn_fa = getattr(self.packet_bridge, 'set_field_authority', None)
            if callable(fn_fa):
                try:
                    fn_fa(self._field_authority)
                except Exception:
                    pass
            fn = getattr(self.packet_bridge, 'set_mem_authoritative', None)
            if callable(fn):
                try:
                    fn(mode == 'memory')
                except Exception:
                    pass
        if mode == 'memory':
            return
        # Phase 5: degrade fields independently based on the typed failure reason.
        # A scan in progress or missing process does not count as failure, by policy.
        reason = self._classify_probe_failure(mode, err)
        for comp in ('hp', 'level', 'skills', 'identity', 'profession', 'name'):
            self._field_authority.record_failure(comp, reason)

    # ───────── 查询 ─────────

    def base_acquired(self) -> bool:
        """True once a non-empty entity snapshot has been read (correct base latched)."""
        return self._base_acquired

    def boss_break(self) -> Optional[dict]:
        """Latest MEM boss break {breaking_stage, extinction_pct, has_break_data}, or None."""
        return self.last_boss_break

    def snapshot(self):
        """返回内存源的最新原始 SelfSnapshot (含 skill_cds/resources 等)."""
        if self._provider is None:
            return None
        return self._provider.last_snap

    def policy_status(self) -> dict:
        provider = self._provider
        out = {
            "provider_auto_scan_enabled": bool(self._auto_scan_enabled),
            "provider_poll_interval_s": self._poll_interval,
            "provider_allow_static_fallback": bool(self._allow_static_fallback),
            "provider_max_scan_regions_mb": int(self._max_scan_regions_mb),
        }
        if provider is not None:
            fn = getattr(provider, "policy_status", None)
            if callable(fn):
                try:
                    provider_out = fn()
                    if isinstance(provider_out, dict):
                        out.update(provider_out)
                except Exception:
                    pass
        return out

    @property
    def is_memory_active(self) -> bool:
        return self.mode == "memory"


# ───────── selftest ─────────

def _unit_selftest() -> int:
    """No-process unit checks for the TCP-primary-supplement cadence + O(1) boss push
    (run: python mem_probe/il2cpp/mem_state_bridge.py --unit)."""
    passed = [0]
    failed = [0]

    def ck(name, cond):
        if cond:
            passed[0] += 1
            print(f"  [ok] {name}")
        else:
            failed[0] += 1
            print(f"  [FAIL] {name}")

    class _State:
        def __init__(self, src="none"):
            self.boss_hp_source = src

    class _GS:
        def __init__(self, src="none"):
            self.state = _State(src)
            self.updates = []

        def update(self, **kw):
            self.updates.append(kw)

    class _PB:
        def __init__(self, mode):
            self._data_source_mode = mode

    class _PM:
        def __init__(self, uuid):
            self._uuid = uuid

        def read_u64(self, addr):
            return self._uuid

    class _EMR:
        off_ent_uuid = 0xC0

    class _Prov:
        def __init__(self, uuid):
            self._pm = _PM(uuid)
            self._emr = _EMR()

    print("[cadence gating]")
    br = MemStateBridge(state_mgr=_GS(), packet_bridge=_PB("memory"))
    br.last_boss_mem = {"uuid": 1}
    br._base_acquired = True
    ck("memory -> fast(1.0) regardless of boss", br._next_entity_interval() == br._entity_interval_fast)
    br.packet_bridge._data_source_mode = "hybrid"
    ck("hybrid+boss+base -> slow(4.0)", br._next_entity_interval() == br._entity_interval_slow)
    br.last_boss_mem = None
    ck("hybrid+no boss -> fast (cold)", br._next_entity_interval() == br._entity_interval_fast)
    br.last_boss_mem = {"uuid": 1}
    br._base_acquired = False
    ck("hybrid+boss+!base -> fast (cold)", br._next_entity_interval() == br._entity_interval_fast)

    print("[fast boss push]")
    gs = _GS(src="none")
    br2 = MemStateBridge(state_mgr=gs, packet_bridge=_PB("hybrid"))
    boss = {"uuid": 7, "obj": 0x1000}
    c = {"cur_hp": 300, "max_hp": 1000, "hp_pct": 0.3,
         "breaking_stage": 2, "extinction": 50, "max_extinction": 100}
    br2._boss_fast_push(boss, c)
    ck("boss dict HP refreshed in place", boss.get("cur_hp") == 300 and boss.get("breaking_stage") == 2)
    ck("last_boss_break stage", br2.last_boss_break and br2.last_boss_break["breaking_stage"] == 2)
    ck("last_boss_break ext% = 0.5", abs(br2.last_boss_break["extinction_pct"] - 0.5) < 1e-6)
    ck("hp push when src=none", any(u.get("boss_hp_source") == "memory" for u in gs.updates))
    ck("hp push value", gs.updates and gs.updates[-1].get("boss_current_hp") == 300)
    n0 = len(gs.updates)
    br2._boss_fast_push(boss, c)
    ck("throttled: no second push", len(gs.updates) == n0)
    br2._last_boss_fast_push = 0.0
    br2._boss_fast_push(boss, c)
    ck("pushes again after throttle window", len(gs.updates) == n0 + 1)

    gs2 = _GS(src="packet")   # TCP owns the bar
    br3 = MemStateBridge(state_mgr=gs2, packet_bridge=_PB("hybrid"))
    br3._boss_fast_push({"uuid": 7}, dict(c))
    ck("no push when src=packet (TCP owns)",
       not any(u.get("boss_hp_source") == "memory" for u in gs2.updates))

    print("[staleness guard]")
    br4 = MemStateBridge(packet_bridge=_PB("hybrid"))
    ck("guard true on uuid match", br4._boss_obj_is(_Prov(0x55), 0x1000, 0x55) is True)
    ck("guard false on uuid mismatch", br4._boss_obj_is(_Prov(0x99), 0x1000, 0x55) is False)

    print("[mode helpers]")
    ck("hybrid is supplement", MemStateBridge(packet_bridge=_PB("hybrid"))._mem_is_supplement() is True)
    ck("auto is supplement", MemStateBridge(packet_bridge=_PB("auto"))._mem_is_supplement() is True)
    ck("memory is primary", MemStateBridge(packet_bridge=_PB("memory"))._mem_is_supplement() is False)

    print(f"\n{passed[0]} passed, {failed[0]} failed")
    return 1 if failed[0] else 0


def _selftest():
    import argparse
    import sys
    p = argparse.ArgumentParser()
    p.add_argument("--seconds", type=int, default=10)
    p.add_argument("--unit", action="store_true", help="no-process unit checks")
    args = p.parse_args()
    if args.unit:
        sys.exit(_unit_selftest())

    class _StubGS:
        def __init__(self):
            self.kw = {}
        def update(self, **kw):
            self.kw.update(kw)
            print(f"[gs.update] {kw}")

    class _StubDps:
        def set_self_uid(self, uid):
            print(f"[dps.set_self_uid] {uid}")

    gs = _StubGS()
    dps = _StubDps()
    br = MemStateBridge(state_mgr=gs, dps_tracker=dps)
    if not br.start():
        print("FAIL: bridge did not start")
        return
    try:
        for i in range(args.seconds):
            time.sleep(1)
            print(f"  tick {i+1} mode={br.mode} uid={br.last_uid} "
                  f"hp={br.last_hp}/{br.last_max_hp} cds={br.last_skill_cd_count}")
    finally:
        br.stop()


if __name__ == "__main__":
    _selftest()

