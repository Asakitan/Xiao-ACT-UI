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
        # entity-HP arm (MEM-driven boss/entity HP; additive, never fights live TCP)
        self._entity_enabled: bool = True
        self._entity_provider: Any = None
        self._entity_thread: Optional[threading.Thread] = None
        self._entity_stop = threading.Event()
        self._entity_interval: float = 1.0
        self.last_entities: list = []
        self.last_boss_mem: Optional[dict] = None
        self._named: dict = {}          # uuid -> resolved name (push to tracker once)
        self._last_scene_id: int = 0
        self._damage_reader: Any = None
        self.last_mem_damage: dict = {}   # uid(=uuid>>16) -> MEM damage total
        self._mem_dmg_logged: bool = False
        self._mem_dmg_gate_logged: bool = False
        self._mem_dmg_empty_logged: bool = False
        self._mem_dmg_err_logged: bool = False
        self._entity_diag_logged: bool = False
        # JSON name self-heal: TCP sends only template ids, so read the game's real NAME
        # from memory and overlay it into the runtime name cache when it differs.
        self._name_cache = None
        self._name_cache_checked: bool = False
        # base_id -> the entity's real NAME attr ('' when it carries only a template id,
        # which is most monsters). Read once per base_id (the index walk is not free) and
        # reused; also dedups the overlay write.
        self._mem_name_seen: dict = {}
        self._nr = None
        self._nr_tried = False

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
                # resolve display names from MEM: ZEntity.BaseId -> offline name table
                # (no TCP) and feed the uuid->name path the boss bar / drilldown read.
                if snap:
                    nr = self._name_resolver()
                    cache = self._mem_name_cache()
                    for e in snap:
                        uuid = int(e["uuid"])
                        bid = int(e.get("base_id") or 0)
                        cached = self._named.get(uuid)
                        if cached is None or cached[0] != bid:   # resolve+push once / per base_id
                            nm = (nr.monster(bid, default="") if (nr and bid) else "") or ""
                            # JSON self-heal: TCP sends only template ids, so when an entity DOES
                            # carry a real NAME attr in memory and it differs from our table, the
                            # memory name is the authority -> overlay it. Most monsters carry no
                            # NAME attr; players are skipped (suffix 640). Read once per base_id.
                            if (cache is not None and bid > 0 and (uuid & 0xFFFF) != 640
                                    and bid not in self._mem_name_seen):
                                try:
                                    real = prov.read_name(int(e.get("obj") or 0))
                                except Exception:
                                    real = ""
                                self._mem_name_seen[bid] = real
                                if real and real != nm:
                                    kind = "boss" if e.get("kind") == "boss" else "monster"
                                    try:
                                        cache.observe_name(kind, bid, real,
                                                            source="mem", confidence="mem")
                                    except Exception:
                                        pass
                                    print(f"[MemName] overlay {kind}#{bid}: "
                                          f"table={nm!r} -> mem={real!r}")
                            _seen = self._mem_name_seen.get(bid)
                            if _seen:                       # reuse the memory name for display
                                nm = _seen
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
                # scene name from memory (self CharSerialize.SceneData.MapId -> table)
                ls = getattr(self._provider, "last_snap", None)
                smid = int(getattr(ls, "scene_map_id", 0) or 0) if ls else 0
                if smid and smid != self._last_scene_id and self.state_mgr is not None:
                    self._last_scene_id = smid
                    nr = self._name_resolver()
                    nm = nr.dungeon(smid, default="") if nr else ""
                    if nm:
                        try:
                            self.state_mgr.update(dungeon_scene_id=smid, dungeon_name=nm)
                        except Exception:
                            pass
            except Exception:
                traceback.print_exc()
            self._entity_stop.wait(self._entity_interval)

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
            ds = str(getattr(self.packet_bridge, '_data_source_mode', '') or '').lower()
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

    def stop(self):
        self._entity_stop.set()
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
                self.state_mgr.update(**kw)
                self._log(f"[MemBridge] identity {kw}")
        except Exception:
            traceback.print_exc()

    def _on_profession(self, profession_id: int, char_name: str):
        self.last_profession_id = profession_id
        self.last_char_name = char_name
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
            fn = getattr(self.packet_bridge, 'set_mem_authoritative', None)
            if callable(fn):
                try:
                    fn(mode == 'memory')
                except Exception:
                    pass

    # ───────── 查询 ─────────

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

def _selftest():
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("--seconds", type=int, default=10)
    args = p.parse_args()

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

