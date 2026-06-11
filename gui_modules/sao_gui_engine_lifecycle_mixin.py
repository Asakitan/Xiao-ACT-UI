# -*- coding: utf-8 -*-
"""
SAOPlayerGUIEngineLifecycleMixin — twelfth mixin extracted from
SAOPlayerGUI (round 53 of the sao_gui split refactor). 4 methods,
~350 lines.

The recognition + data engine bring-up + teardown + cache helpers.
These methods are the heaviest single block remaining in
SAOPlayerGUI's __init__ flow.

Methods:
  * _stop_recognition_engines — stops AutoKey, BossRaid, packet/vision
    engines + clears all references (hide-and-seek now lives in a plugin).
  * _reconfigure_data_engines (80 lines) — restarts the
    packet+vision engines for the current data source. Reads
    `mem_data_source` setting to pick TCP/memory/hybrid/auto path.
    Resolves DPS skill names through the shared runtime NameResolver.
  * _start_recognition (204 lines — the biggest method here) —
    full bring-up sequence:
      1. instantiate GameStateManager + CfgSettings + subscribe
         _on_game_state_update
      2. restore cached player_name, sound settings
      3. call _reconfigure_data_engines
      4. AutoKeyEngine + burst actions
      5. BossRaidEngine + BossAutoKeyLinkage with alert linkage
      6. all overlay windows (DPS, BossHP, HP, Alert, SkillFX, Buffs)
      7. 30 s cache-saver background thread
  * _persist_cached_identity_state (34 lines) — write current
    player_name / profession / level / season_exp / player_id /
    fight_point into the cfg_settings game_cache dict.

Round 53+ follow-up: DPS skill-name lookup now goes through the shared
NameResolver/TCP name_tables path instead of reading a standalone
skill_names.json bundle during engine startup.

Required SAOPlayerGUI attrs:
  * self.root, self.settings, self._cfg_settings_ref, self._state_mgr
  * self._cache_loop_stop (threading.Event)
  * self._username, self._profession, self._level, self._level_extra,
    self._season_exp, self._game_state, self._hp_display_name
  * self._auto_key_engine, self._boss_raid_engine,
    self._boss_autokey_linkage
  * self._recognition_engine, self._recognition_engines,
    self._recognition_active, self._packet_engine, self._vision_engine
  * self._dps_tracker, self._dps_overlay, self._dps_enabled,
    self._dps_visible
  * self._hp_overlay, self._hp_ov_visible, self._boss_hp_overlay,
    self._alert_overlay, self._skillfx_overlay,
    self._self_buff_overlay, self._boss_buff_overlay

Required SAOPlayerGUI methods (via MRO):
  * _on_game_state_update (State mixin)
  * _on_packet_damage, _on_monster_update, _on_boss_event,
    _on_scene_change, _send_linked_key (SAOPlayerGUI)
  * _load_autokey_burst_actions (Actions mixin)
  * _request_dps_live_snapshot, _request_dps_last_report,
    _reset_dps_tracker, _get_dps_last_report_available,
    _request_dps_entity_detail (DpsTheme mixin)
  * _show_entity_alert (Dialogs mixin)
  * _hp_overlay_on_click, _hp_overlay_on_menu (Dialogs / SAOPlayerGUI)
  * _should_show_sta_offline, _reset_sta_offline_state (SAOPlayerGUI)
  * _get_setting (SAOPlayerGUI)
"""

from __future__ import annotations

import os
from typing import Any, Optional

from engines.auto_key_engine import AutoKeyEngine
from engines.boss_autokey_linkage import BossAutoKeyLinkage
from engines.boss_raid_engine import BossRaidEngine
from engines.act_trigger_engine import ActTriggerEngine
from engines.dps_history import DpsHistoryStore
from engines.dps_tracker import DpsTracker
from engines.encounter_manager import EncounterManager
from act_platform.runtime import ensure_act_event_bus, ensure_act_plugin_manager, shutdown_act_plugin_manager
from utils.sao_sound import play_sound
from utils import sao_tts
from engines.mechanic_alert_controller import MechanicAlertController

from gui_modules.sao_gui_alert import AlertOverlay
from gui_modules.sao_gui_map_banner import MapBannerOverlay
from gui_modules.sao_gui_mech_banner import MechBannerOverlay
from gui_modules.sao_gui_bosshp import BossHpOverlay
from gui_modules.sao_gui_buffmon import SelfBuffOverlay, BossBuffOverlay
from gui_modules.sao_gui_dps import DpsOverlay
from gui_modules.sao_gui_hp import HpOverlay
from gui_modules.sao_gui_skillfx import BurstReadyOverlay


class SAOPlayerGUIEngineLifecycleMixin:
    """Mixin bundling recognition+data engine bring-up + teardown +
    cache persistence."""

    def _stop_recognition_engines(self):
        """停止所有识别/数据引擎."""
        if getattr(self, '_mem_bridge', None):
            try: self._mem_bridge.stop()
            except Exception: pass
            self._mem_bridge = None
        if getattr(self, '_auto_key_engine', None):
            try: self._auto_key_engine.stop()
            except Exception: pass
            self._auto_key_engine = None
        if getattr(self, '_boss_raid_engine', None):
            try: self._boss_raid_engine.stop()
            except Exception: pass
            self._boss_raid_engine = None
        # Plugins (incl. the hide_seek_plugin that now owns the hide-and-seek
        # engine) are unloaded here; their on_unload stops any owned engines.
        shutdown_act_plugin_manager(self)
        engines = list(getattr(self, '_recognition_engines', []) or [])
        if not engines and self._recognition_engine:
            engines = [self._recognition_engine]
        for engine in engines:
            try: engine.stop()
            except Exception: pass
        self._recognition_engines = []
        self._recognition_engine = None
        self._packet_engine = None
        self._vision_engine = None
        self._reset_sta_offline_state()

    def _reconfigure_data_engines(self):
        """重启 packet/vision 引擎以匹配当前数据源配置."""
        if not getattr(self, '_cfg_settings_ref', None) or not getattr(self, '_state_mgr', None):
            return
        self._stop_recognition_engines()

        engines = []
        try:
            from net.packet_bridge import PacketBridge
            # Phase 7: Entity menu reads `mem_data_source` preference
            # (key intentionally distinct from legacy 'data_source' which is
            # a packet-vs-vision toggle). Values: 'tcp' (default) | 'memory' |
            # 'hybrid' | 'auto'. Routes through mem_probe.UnifiedDataSource
            # for non-tcp modes so Entity menu gets the exact same dataset
            # the webview menu does.
            try:
                _data_source_mode = str(
                    self._cfg_settings_ref.get('mem_data_source', 'tcp') or 'tcp'
                ).lower()
            except Exception:
                _data_source_mode = 'tcp'
            packet_engine = PacketBridge(self._state_mgr, self._cfg_settings_ref,
                                         on_damage=self._on_packet_damage,
                                         on_monster_update=self._on_monster_update,
                                         on_boss_event=self._on_boss_event,
                                         on_scene_change=self._on_scene_change,
                                         on_skill_event=self._on_skill_event,
                                         on_dungeon_event=self._on_dungeon_event,
                                         data_source=_data_source_mode,
                                         plugin_manager=ensure_act_plugin_manager(self, load=True),
                                         event_bus=ensure_act_event_bus(self))
            packet_engine.start()
            engines.append(packet_engine)
            self._packet_engine = packet_engine
            print(f'[SAO Entity] Packet bridge started '
                  f'(data_source={_data_source_mode!r})')
        except Exception as e:
            import traceback
            print(f'[SAO Entity] Packet bridge FAILED: {e}')
            traceback.print_exc()
            self._packet_engine = None

        # DPS Tracker
        try:
            self._dps_history_store = DpsHistoryStore()
            self._dps_tracker = DpsTracker()
            self._encounter_mgr = EncounterManager()
            try:
                _act_rules = self._cfg_settings_ref.get('act_trigger_rules', []) or []
            except Exception:
                _act_rules = []
            self._act_trigger_engine = ActTriggerEngine(_act_rules)
            self._dps_tracker.register_finalized_hook(self._on_dps_report_finalized)
            ensure_act_event_bus(self)
            ensure_act_plugin_manager(self, load=True)
            # Wire the tracker into the packet bridge so the (deferred, hybrid) memory
            # source can push the MEM damage table / per-skill breakdown into the DPS
            # panel. The mem source is created lazily on the first TCP scene trigger,
            # which happens after this point, so it picks up the tracker.
            try:
                if getattr(self, '_packet_engine', None) is not None:
                    self._packet_engine.set_dps_tracker(self._dps_tracker)
            except Exception:
                pass
            print('[SAO Entity] DPS tracker initialized')
        except Exception as e:
            print(f'[SAO Entity] DPS tracker init failed: {e}')
            self._dps_history_store = None
            self._dps_tracker = None
            self._encounter_mgr = None
            self._act_trigger_engine = None

        try:
            from vision.recognition import RecognitionEngine
            vision_engine = RecognitionEngine(self._state_mgr, self._cfg_settings_ref)
            vision_engine.start()
            engines.append(vision_engine)
            self._vision_engine = vision_engine
            print('[SAO Entity] Recognition engine started (window vision)')
        except Exception as e:
            import traceback
            print(f'[SAO Entity] Recognition engine FAILED: {e}')
            traceback.print_exc()
            self._vision_engine = None

        self._recognition_engines = engines
        self._recognition_engine = engines[0] if engines else None
        self._recognition_active = bool(engines)

    def _start_recognition(self):
        """启动游戏数据引擎 (抓包 + 纯识图 + AutoKey + BossRaid)."""
        try:
            from engines.game_state import GameStateManager
            from config import SettingsManager as CfgSettings

            self._state_mgr = GameStateManager()
            cfg_settings = CfgSettings()
            self._cfg_settings_ref = cfg_settings

            # v2.1.18: 必须先 subscribe 再 load_cache, 否则 load_cache 内部对订阅者
            # 的初始通知会因为 _on_game_state_update 还没注册而被丢弃, 导致 entity 面板
            # 启动时显示不出从 webview 切过来时持久化的角色名/等级/HP 等.
            try:
                self._state_mgr.subscribe(self._on_game_state_update)
            except Exception:
                pass

            # 加载上次缓存的游戏状态 (立即显示)
            self._state_mgr.load_cache(cfg_settings)

            # Restore sound settings
            try:
                from utils.sao_sound import set_sound_enabled, set_sound_volume
                _snd_on = cfg_settings.get('sound_enabled', True)
                _snd_vol = cfg_settings.get('sound_volume', 70)
                set_sound_enabled(bool(_snd_on) if _snd_on is not None else True)
                set_sound_volume(int(_snd_vol) if _snd_vol is not None else 70)
            except Exception:
                pass

            # 用缓存名替换默认 "Player"
            cached_name = self._state_mgr.state.player_name
            if cached_name:
                self._username = cached_name
                disp = cached_name
                if len(disp) > 10:
                    disp = disp[:9] + '…'
                self._hp_display_name = disp
                print(f'[SAO Entity] 从缓存加载角色名: {cached_name}')

            self._reconfigure_data_engines()

            # AutoKey Engine
            self._auto_key_engine = AutoKeyEngine(
                self._state_mgr,
                self._cfg_settings_ref,
                extra_gate=lambda: bool(getattr(self, '_recognition_active', False)),
            )
            self._auto_key_engine.start()
            try:
                self._auto_key_engine.set_burst_actions(self._load_autokey_burst_actions())
            except Exception:
                pass

            # Boss Raid Engine + AutoKey Linkage
            self._boss_autokey_linkage = BossAutoKeyLinkage(
                self._cfg_settings_ref,
                send_key=self._send_linked_key,
                on_log=lambda msg: print(msg),
                foreground_gate=self._auto_key_engine.is_game_foreground,
            )

            def _on_boss_alert_with_linkage(title, message):
                print(f'[SAO Entity] Boss Alert: {title} — {message}')
                if self._alert_overlay:
                    try: self._alert_overlay.show_alert(title, message)
                    except Exception: pass
                if self._boss_autokey_linkage:
                    try:
                        self._boss_autokey_linkage.on_boss_raid_alert(title, message)
                    except Exception:
                        pass

            self._boss_raid_engine = BossRaidEngine(
                self._state_mgr,
                self._cfg_settings_ref,
                on_alert=_on_boss_alert_with_linkage,
                on_sound=lambda name: play_sound(name),
                on_boss_action=self._on_boss_action_with_gate,
                on_mechanic=self._on_mechanic_event,
            )

            # ── 初始化 ULW 覆盖层 ──
            # DPS panel mirrors webview: stays hidden on startup regardless of
            # the `dps_enabled` setting. Combat/report triggers bring it up.
            self._dps_enabled = bool(self._get_setting('dps_enabled', True))
            self._dps_visible = False
            try:
                self._dps_overlay = DpsOverlay(
                    self.root,
                    self._cfg_settings_ref,
                    request_live_snapshot=self._request_dps_live_snapshot,
                    show_last_report=self._request_dps_last_report,
                    reset_dps=self._reset_dps_tracker,
                    has_last_report=self._get_dps_last_report_available,
                    request_entity_detail=self._request_dps_entity_detail,
                    list_history=self._request_dps_history,
                    export_last_report=self._export_last_dps_report,
                    alert=self._show_entity_alert,
                )
                self._dps_overlay.set_report_available(
                    self._get_dps_last_report_available())
                print('[SAO Entity] DPS overlay initialized (hidden)')
            except Exception as e:
                print(f'[SAO Entity] DPS overlay init failed: {e}')
                self._dps_overlay = None

            try:
                self._boss_hp_overlay = BossHpOverlay(self.root, self._cfg_settings_ref)
                print('[SAO Entity] Boss HP overlay initialized')
            except Exception as e:
                print(f'[SAO Entity] Boss HP overlay init failed: {e}')
                self._boss_hp_overlay = None

            self._hp_ov_visible = bool(self._get_setting('hp_ov_enabled', True))
            try:
                self._hp_overlay = HpOverlay(
                    self.root, self._cfg_settings_ref,
                    on_click=self._hp_overlay_on_click,
                    on_menu=self._hp_overlay_on_menu,
                )
                if self._hp_ov_visible:
                    self._hp_overlay.show()
                # Push cached game state to HP overlay immediately
                if self._state_mgr:
                    try:
                        _gs = self._state_mgr.state
                        if _gs.hp_max > 0:
                            _hp_lv = int(_gs.level_base or 1)
                            _hp_lv_extra = int(getattr(_gs, 'level_extra', 0) or 0)
                            if _hp_lv_extra > 0 and _hp_lv > 0:
                                _hp_lv_text = f'{_hp_lv}(+{_hp_lv_extra})'
                            else:
                                _hp_lv_text = str(_hp_lv)
                            self._hp_overlay.update_hp(
                                _gs.hp_current, _gs.hp_max, _hp_lv_text)
                        if _gs.stamina_max > 0:
                            self._hp_overlay.update_sta(
                                _gs.stamina_current, _gs.stamina_max)
                        self._hp_overlay.set_sta_offline(
                            self._should_show_sta_offline(_gs)
                        )
                        if _gs.player_name:
                            self._hp_overlay.set_player_info({
                                'name': _gs.player_name,
                                'profession': _gs.profession_name or '',
                                'uid': _gs.player_id or '',
                            })
                    except Exception:
                        pass
                print('[SAO Entity] Player HP overlay initialized')
            except Exception as e:
                print(f'[SAO Entity] Player HP overlay init failed: {e}')
                self._hp_overlay = None

            try:
                self._alert_overlay = AlertOverlay(self.root, self._cfg_settings_ref)
                print('[SAO Entity] Alert overlay initialized')
            except Exception as e:
                print(f'[SAO Entity] Alert overlay init failed: {e}')
                self._alert_overlay = None

            try:
                self._map_banner_overlay = MapBannerOverlay(self.root, self._cfg_settings_ref)
                print('[SAO Entity] Map banner overlay initialized')
            except Exception as e:
                print(f'[SAO Entity] Map banner overlay init failed: {e}')
                self._map_banner_overlay = None

            try:
                self._mech_banner_overlay = MechBannerOverlay(self.root, self._cfg_settings_ref)
                self._mech_alert_controller = MechanicAlertController(
                    on_banner=self._mech_banner_overlay.show_mechanic,
                    banner_enabled_fn=lambda: bool(
                        self._get_setting('mech_banner_enabled', True)),
                )
                sao_tts.set_tts_enabled(bool(self._get_setting('tts_enabled', True)))
                sao_tts.set_tts_volume(int(self._get_setting('tts_volume', 80) or 80))
                print('[SAO Entity] Mech banner overlay + alert controller initialized')
            except Exception as e:
                print(f'[SAO Entity] Mech banner overlay init failed: {e}')
                self._mech_banner_overlay = None
                self._mech_alert_controller = None

            try:
                self._skillfx_overlay = BurstReadyOverlay(self.root, self._cfg_settings_ref)
                print('[SAO Entity] SkillFX (Burst) overlay initialized')
            except Exception as e:
                print(f'[SAO Entity] SkillFX overlay init failed: {e}')
                self._skillfx_overlay = None

            try:
                _bm_on = bool(self._get_setting('buffmon_enabled', True))
                self._self_buff_overlay = SelfBuffOverlay(
                    self.root, self._cfg_settings_ref, hp_overlay=self._hp_overlay)
                self._self_buff_overlay.set_enabled(_bm_on)
                self._boss_buff_overlay = BossBuffOverlay(
                    self.root, self._cfg_settings_ref,
                    boss_hp_overlay=self._boss_hp_overlay)
                self._boss_buff_overlay.set_enabled(_bm_on)
                print('[SAO Entity] BuffMon overlays (self/boss) initialized')
            except Exception as e:
                print(f'[SAO Entity] BuffMon overlay init failed: {e}')
                self._self_buff_overlay = None
                self._boss_buff_overlay = None

            # 启动定时缓存保存 (每30秒)
            import threading as _thr
            _stop_evt = self._cache_loop_stop
            def _cache_loop():
                while not _stop_evt.is_set():
                    _stop_evt.wait(30)
                    if _stop_evt.is_set():
                        break
                    try:
                        self._persist_cached_identity_state(save_now=False)
                        self._state_mgr.save_cache(self._cfg_settings_ref)
                    except Exception:
                        pass
            _thr.Thread(target=_cache_loop, daemon=True, name='cache_saver').start()

            # ── Memory bridge: live read-only inspect of Star.exe to push
            # self UID / HP / level / season_exp / fight_point / profession /
            # skill CDs into GameState. TCP path remains authoritative; this
            # is a fast-path that covers the "started after login" gap.
            try:
                if getattr(self, '_packet_engine', None) is not None:
                    self._mem_bridge = None
                    # hybrid: the live MemStateBridge is built inside PacketBridge's
                    # mem source -- forward the boss raid engine so the boss-action
                    # feed (cast_skill_id edge) reaches on_mem_boss_action.
                    try:
                        self._packet_engine.set_boss_raid_engine(
                            getattr(self, '_boss_raid_engine', None))
                    except Exception:
                        pass
                    print('[MemBridge] deferred to PacketBridge TCP scene/full-sync trigger')
                else:
                    from mem_probe.il2cpp.mem_state_bridge import MemStateBridge
                    self._mem_bridge = MemStateBridge(
                        state_mgr=self._state_mgr,
                        dps_tracker=getattr(self, '_dps_tracker', None),
                        dps_overlay=getattr(self, '_dps_overlay', None),
                        hp_overlay=getattr(self, '_hp_overlay', None),
                        auto_key_engine=getattr(self, '_auto_key_engine', None),
                        boss_raid_engine=getattr(self, '_boss_raid_engine', None),
                        packet_bridge=getattr(self, '_packet_engine', None),
                        dump_id='fdc7111b',
                        enable_extended=True,
                        on_log=lambda m: print(f'[MemBridge] {m}'),
                    )
                    if not self._mem_bridge.start():
                        self._mem_bridge = None
            except Exception as _mb_exc:
                import traceback
                print(f'[MemBridge] init failed: {_mb_exc}')
                traceback.print_exc()
                self._mem_bridge = None

        except Exception as e:
            print(f'[SAO Entity] Data engine failed: {e}')
            import traceback; traceback.print_exc()
            self._recognition_active = False

    def _on_mechanic_event(self, evt):
        """Engine mechanic event → TTS + top banner (controller may lag engine
        creation during bring-up; events before it exists are dropped)."""
        controller = getattr(self, '_mech_alert_controller', None)
        if controller is not None:
            try:
                controller.on_mechanic(evt)
            except Exception:
                pass

    def _on_boss_action_with_gate(self, action):
        """Gate the memory boss-action feed before driving the auto-key linkage.

        Mirrors the AutoKeyEngine gate: only react while recognition is active and
        the player is alive (suppress auto-dodge / offense while dead)."""
        if not bool(getattr(self, '_recognition_active', False)):
            return
        gs = getattr(self._state_mgr, 'state', None) if getattr(self, '_state_mgr', None) else None
        if gs is not None and bool(getattr(gs, 'self_dead', False)):
            return
        linkage = getattr(self, '_boss_autokey_linkage', None)
        if linkage is not None:
            try:
                linkage.on_boss_action(action)
            except Exception:
                pass
        self._maybe_directional_dodge(action)
        self._kick_numbered_zone_capture(action)

    def get_numbered_zone_summary(self):
        """编号圈(1/2/3)统计供面板读取: 每个圈的序号/组/命中名单/存活时长。
        无追踪器(没遇到机制圈)时返回 []。纯读取, 不触发内存扫描。"""
        t = getattr(self, '_numbered_zone_tracker', None)
        return t.summary() if t is not None else []

    def _kick_numbered_zone_capture(self, action):
        """机制触发时, 在机制窗口内短时(默认12s @4Hz)采样活动区域喂编号圈追踪器:
        按出现顺序编号 + 用区域成员(entitiesIdInZone_=服务端命中判定)做精确归属。
        纯读取无按键, 不受躲避总开关限制(统计安全); 仅前台采样。窗口内重复触发只延长
        截止时间不另起线程。"""
        import time as _t
        import threading as _th
        try:
            if not bool(getattr(self, '_recognition_active', False)):
                return
            tracker = getattr(self, '_numbered_zone_tracker', None)
            if tracker is None:
                from engines.numbered_zone_tracker import NumberedZoneTracker
                tracker = NumberedZoneTracker()
                self._numbered_zone_tracker = tracker
            self._nz_capture_until = _t.time() + 12.0
            if getattr(self, '_nz_capture_thread', None) is not None \
                    and self._nz_capture_thread.is_alive():
                return                       # 已在采样, 只延长窗口
            ctx = getattr(self, '_dodge_context', None)
            if ctx is None:
                director = self._get_auto_dodge_director()   # 建 ctx (只读 mem 源)
                ctx = getattr(self, '_dodge_context', None)
            if ctx is None:
                return
            fg = getattr(getattr(self, '_auto_key_engine', None),
                         'is_game_foreground', None)

            def _loop():
                seen = set()
                while _t.time() < getattr(self, '_nz_capture_until', 0.0):
                    try:
                        if fg is None or fg():
                            snap = ctx.snapshot_zones()
                            now = _t.time()
                            new = tracker.observe(snap, now=now)
                            for z in new:
                                seen.add(z.zone_uuid)
                                print('[编号圈] 第%d个(组%d内#%d) uuid=%d 成员%d人'
                                      % (z.seq, z.group_id, z.seq_in_group,
                                         z.zone_uuid, len(z.members_now)))
                            tracker.prune(now)
                    except Exception:
                        pass
                    _t.sleep(0.25)
            th = _th.Thread(target=_loop, daemon=True)
            self._nz_capture_thread = th
            th.start()
        except Exception:
            pass

    def _get_auto_dodge_director(self):
        """Lazy AutoDodgeDirector (定向躲避): 自带只读 mem 源, 相机基/玩家位/boss位
        三个已逆向读取器收口。首次创建会开一个只读进程句柄 (与 ACE 兼容)。"""
        director = getattr(self, '_auto_dodge_director', None)
        if director is not None:
            return director
        try:
            from engines.auto_dodge_director import AutoDodgeDirector
            from engines.boss_autokey_linkage import load_linkage_config
            from mem_probe.il2cpp.mem_dodge_context import DodgeContext
            from mem_probe.il2cpp.static_dps_source import StaticDpsSource
            ctx = DodgeContext(StaticDpsSource())
            fg = getattr(getattr(self, '_auto_key_engine', None),
                         'is_game_foreground', None)

            def gate():
                # 前台门 AND 躲避总开关: F12 翻 dodge_enabled 后, 延迟期/在途的闭环
                # 下一 tick _blocked() 即停 (修审计 P2 急停竞态)
                try:
                    if fg is not None and not fg():
                        return False
                    return bool(load_linkage_config(self._cfg_settings_ref)
                                .get('dodge_enabled', True))
                except Exception:
                    return True
            director = AutoDodgeDirector(
                self._send_key_event,
                get_cam_basis=ctx.get_cam_basis,
                get_player_pos=ctx.get_player_pos,
                gate=gate)
            self._auto_dodge_director = director
            self._dodge_context = ctx
            # 后台预热相机/实体冷定位堆扫(~20s), 否则第一次躲避会迟到
            import threading as _th
            _th.Thread(target=ctx.prewarm, daemon=True).start()
        except Exception as e:
            print(f'[Dodge] director init failed: {e}')
            self._auto_dodge_director = None
        return self._auto_dodge_director

    def _maybe_directional_dodge(self, action):
        """机制躲避带 direction 时, 把人物按 WASD 挪开 (主开关
        directional_dodge_enabled, 默认关; 与 linkage dodge_enabled / F12 共用急停)。"""
        try:
            if not bool(self._get_setting('directional_dodge_enabled', False)):
                return
            from engines.boss_autokey_linkage import load_linkage_config
            if not bool(load_linkage_config(self._cfg_settings_ref).get('dodge_enabled', True)):
                return
            inline = action.get('mechanic_dodge') if isinstance(action, dict) else None
            if not isinstance(inline, dict):
                return
            direction = str(inline.get('direction') or '').strip()
            if not direction:
                return
            director = self._get_auto_dodge_director()
            if director is None:
                return
            wait_s = float(action.get('dodge_wait_s') or 0.0)
            boss_base_id = int(action.get('boss_base_id') or 0)

            def _go():
                ctx = self._dodge_context
                if direction.startswith('away'):
                    # 闭环精准出圈: 进循环前定位一次危险 obj (O(N)), 之后每 tick 只读
                    # 该 obj 的坐标 (O(1)), 不再每 tick 全实体堆读 (修审计性能 P2)
                    try:
                        obj = ctx.lock_danger_obj(direction, boss_base_id)
                    except Exception:
                        obj = 0
                    danger0 = None
                    get_danger = None
                    if obj:
                        danger0 = ctx.read_obj_pos(obj)
                        get_danger = lambda _o=obj: ctx.read_obj_pos(_o)
                    # 零误差出圈判据: 游戏自己的区域成员(玩家离开AOE圈即停), 距离仅兜底
                    is_clear = None
                    try:
                        is_clear = ctx.make_zone_exit_check()
                    except Exception:
                        is_clear = None
                    director.dodge(inline, danger_pos=danger0,
                                   get_danger_pos=get_danger, is_clear=is_clear)
                else:
                    director.dodge(inline)
            import threading as _th
            if wait_s > 0.05:
                t = _th.Timer(wait_s, _go)
                t.daemon = True
                t.start()
            else:
                _go()
        except Exception as e:
            print(f'[Dodge] directional dodge failed: {e}')

    # ────────────────────────────────────────────
    #  SkillFX / Burst Mode Ready helpers
    # ────────────────────────────────────────────

    def _persist_cached_identity_state(self, save_now: bool = False):
        settings = getattr(self, '_cfg_settings_ref', None)
        if not settings:
            return
        cache = dict(settings.get('game_cache', {}) or {})
        name = str(getattr(self, '_username', '') or '').strip()
        profession = str(getattr(self, '_profession', '') or '').strip()
        level_base = int(getattr(self, '_level', 0) or 0)
        level_extra = int(getattr(self, '_level_extra', 0) or 0)
        season_exp = int(getattr(self, '_season_exp', 0) or 0)
        if name:
            cache['player_name'] = name
        if profession:
            cache['profession_name'] = profession
        if level_base > 0:
            cache['level_base'] = level_base
        if level_extra > 0:
            cache['level_extra'] = level_extra
        if season_exp > 0:
            cache['season_exp'] = season_exp
        gs = getattr(self, '_game_state', None)
        uid = str(getattr(gs, 'player_id', '') or '').strip() if gs is not None else ''
        if uid:
            cache['player_id'] = uid
        fight_point = int(getattr(gs, 'fight_point', 0) or 0) if gs is not None else 0
        if fight_point > 0:
            cache['fight_point'] = fight_point
        settings.set('game_cache', cache)
        if save_now:
            try:
                settings.save()
            except Exception:
                pass

