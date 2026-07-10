# -*- coding: utf-8 -*-
# SAOPlayerGUIEngineLifecycleMixin — twelfth mixin extracted from
# SAOPlayerGUI (round 53 of the sao_gui split refactor). 4 methods,
# ~350 lines.
#
# The recognition + data engine bring-up + teardown + cache helpers.
# These methods are the heaviest single block remaining in
# SAOPlayerGUI's __init__ flow.
#
# Methods:
# * _stop_recognition_engines — stops AutoKey, BossRaid, packet/vision
# engines + clears all references (hide-and-seek now lives in a plugin).
# * _reconfigure_data_engines (80 lines) — restarts the
# packet+vision engines for the current data source. Reads
# `mem_data_source` setting to pick TCP/memory/hybrid/auto path.
# Resolves DPS skill names through the shared runtime NameResolver.
# * _start_recognition (204 lines — the biggest method here) —
# full bring-up sequence:
# 1. instantiate GameStateManager + CfgSettings + subscribe
# _on_game_state_update
# 2. restore cached player_name, sound settings
# 3. call _reconfigure_data_engines
# 4. AutoKeyEngine + burst actions
# 5. BossRaidEngine + BossAutoKeyLinkage with alert linkage
# 6. all overlay windows (DPS, BossHP, HP, Alert, SkillFX, Buffs)
# 7. 30 s cache-saver background thread
# * _persist_cached_identity_state (34 lines) — write current
# player_name / profession / level / season_exp / player_id /
# fight_point into the cfg_settings game_cache dict.
#
# Round 53+ follow-up: DPS skill-name lookup now goes through the shared
# NameResolver/TCP name_tables path instead of reading a standalone
# skill_names.json bundle during engine startup.
#
# Required SAOPlayerGUI attrs:
# * self.root, self.settings, self._cfg_settings_ref, self._state_mgr
# * self._cache_loop_stop (threading.Event)
# * self._username, self._profession, self._level, self._level_extra,
# self._season_exp, self._game_state, self._hp_display_name
# * self._auto_key_engine, self._boss_raid_engine,
# self._boss_autokey_linkage
# * self._recognition_engine, self._recognition_engines,
# self._recognition_active, self._packet_engine, self._vision_engine
# * self._dps_tracker, self._dps_overlay, self._dps_enabled,
# self._dps_visible
# * self._hp_overlay, self._hp_ov_visible, self._boss_hp_overlay,
# self._alert_overlay, self._skillfx_overlay,
# self._self_buff_overlay, self._boss_buff_overlay
#
# Required SAOPlayerGUI methods (via MRO):
# * _on_game_state_update (State mixin)
# * _on_packet_damage, _on_monster_update, _on_boss_event,
# _on_scene_change, _send_linked_key (SAOPlayerGUI)
# * _load_autokey_burst_actions (Actions mixin)
# * _request_dps_live_snapshot, _request_dps_last_report,
# _reset_dps_tracker, _get_dps_last_report_available,
# _request_dps_entity_detail (DpsTheme mixin)
# * _show_entity_alert (Dialogs mixin)
# * _hp_overlay_on_click, _hp_overlay_on_menu (Dialogs / SAOPlayerGUI)
# * _should_show_sta_offline, _reset_sta_offline_state (SAOPlayerGUI)
# * _get_setting (SAOPlayerGUI)

from __future__ import annotations

import os
from typing import Any, Optional

from act_platform.runtime import ensure_act_event_bus, ensure_act_plugin_manager, shutdown_act_plugin_manager


class SAOPlayerGUIEngineLifecycleMixin:
    # Mixin bundling recognition+data engine bring-up + teardown +
    # cache persistence.

    def _stop_recognition_engines(self):
        # 停止所有识别/数据引擎.
        #
        # 契约: 返回 False 明确告知平台 "live producer 拒绝 rundown", 平台
        # _finalize_close 收到 False 会暂停后续 shutdown 让调用者 retry.
        # mem_bridge 是重点 — 它管着 kernel-side 物理内存写的 mutation
        # coordinator, 强行 destroy 会撞穿别人的 pa. mem_bridge.stop() 返回
        # False → 保留 _mem_bridge 引用 + 立即返 False 阻塞 shutdown.
        # 其他 engine (auto_key/boss_raid/vision/packet) 都是 user-mode
        # 生命周期, stop 失败不阻塞.
        if getattr(self, '_mem_bridge', None):
            stopped = False
            try:
                stopped = self._mem_bridge.stop() is not False
            except Exception:
                stopped = False
            if not stopped:
                return False
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
        return True

    def _reconfigure_data_engines(self):
        # 重启引擎。所有引擎/数据源/overlay 由插件 on_load 创建。
        self._stop_recognition_engines()
        ensure_act_event_bus(self)
        ensure_act_plugin_manager(self, load=True)
        self._recognition_engines = []
        self._recognition_engine = None
        self._recognition_active = bool(
            getattr(self, '_packet_engine', None) or getattr(self, '_vision_engine', None))

    def _start_recognition(self):
        # 启动平台引擎 + 加载游戏插件（游戏引擎/overlay 由插件 on_load 创建）。
        try:
            from config import SettingsManager as CfgSettings
            cfg_settings = CfgSettings()
            self._cfg_settings_ref = cfg_settings

            self._reconfigure_data_engines()

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
                        _sm = getattr(self, '_state_mgr', None)
                        if _sm:
                            _sm.save_cache(self._cfg_settings_ref)
                    except Exception:
                        pass
            _thr.Thread(target=_cache_loop, daemon=True, name='cache_saver').start()

        except Exception as e:
            print(f'[SAO Entity] Platform engine start failed: {e}')
            import traceback; traceback.print_exc()
            self._recognition_active = False

    def _on_mechanic_event(self, evt):
        # Engine mechanic event → TTS + top banner (controller may lag engine
        # creation during bring-up; events before it exists are dropped).
        controller = getattr(self, '_mech_alert_controller', None)
        if controller is not None:
            try:
                controller.on_mechanic(evt)
            except Exception:
                pass

    def _on_boss_action_with_gate(self, action):
        # Gate the memory boss-action feed before driving the auto-key linkage.
        #
        # Mirrors the AutoKeyEngine gate: only react while recognition is active and
        # the player is alive (suppress auto-dodge / offense while dead).
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
        # 编号圈(1/2/3)统计供面板读取: 每个圈的序号/组/命中名单/存活时长。
        # 无追踪器(没遇到机制圈)时返回 []。纯读取, 不触发内存扫描。
        t = getattr(self, '_numbered_zone_tracker', None)
        return t.summary() if t is not None else []

    def _kick_numbered_zone_capture(self, action):
        # 机制触发时, 在机制窗口内短时(默认12s @4Hz)采样活动区域喂编号圈追踪器:
        # 按出现顺序编号 + 用区域成员(entitiesIdInZone_=服务端命中判定)做精确归属。
        # 纯读取无按键, 不受躲避总开关限制(统计安全); 仅前台采样。窗口内重复触发只延长
        # 截止时间不另起线程。
        import time as _t
        import threading as _th
        try:
            if not bool(getattr(self, '_recognition_active', False)):
                return
            tracker = getattr(self, '_numbered_zone_tracker', None)
            if tracker is None:
                return
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
        # Lazy AutoDodgeDirector (定向躲避): 自带只读 mem 源, 相机基/玩家位/boss位
        # 三个已逆向读取器收口。首次创建会开一个只读进程句柄 (与 ACE 兼容)。
        return getattr(self, '_auto_dodge_director', None)

    def _maybe_directional_dodge(self, action):
        # 机制躲避带 direction 时, 把人物按 WASD 挪开 (主开关
        # directional_dodge_enabled, 默认关; 与 linkage dodge_enabled / F12 共用急停)。
        try:
            if not bool(self._get_setting('directional_dodge_enabled', False)):
                return
            linkage = getattr(self, '_boss_autokey_linkage', None)
            _dodge_on = True
            if linkage is not None:
                _lc_fn = getattr(linkage, 'load_config', None)
                if callable(_lc_fn):
                    try:
                        _dodge_on = bool(_lc_fn(self._cfg_settings_ref).get('dodge_enabled', True))
                    except Exception:
                        pass
            if not _dodge_on:
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
                    # ★停止判据(主人: 不要距离兜底, 肯定错): 任一满足即停, 无距离 —
                    #   ① 几何范围判定: 机制壳子填了"实际范围"→ 玩家出范围即停(最精确)
                    #   ② 区域成员判定: 玩家离开 AOE 圈 (圈是 ZoneEnt 的 boss, 零误差)
                    #   ③ 招式生命周期: boss 不再出触发本次躲避的招 (圈是 ECS 的 boss)
                    geo_clear = zone_clear = skill_ended = None
                    try:
                        geom = (inline.get('geometry') if isinstance(inline, dict) else None) or {}
                        # 范围中心: center=self 快照起手玩家位; 否则 Boss/危险位
                        if str(geom.get('center')) == 'self':
                            _c0 = ctx.get_player_pos()
                            get_center = lambda _p=_c0: _p
                        else:
                            get_center = get_danger
                        if get_center is not None:
                            geo_clear = ctx.make_geometry_clear_check(geom, get_center)
                    except Exception:
                        geo_clear = None
                    try:
                        zone_clear = ctx.make_zone_exit_check()
                    except Exception:
                        zone_clear = None
                    try:
                        skill_ended = ctx.make_skill_ended_check(boss_base_id)
                    except Exception:
                        skill_ended = None

                    def _is_clear():
                        if geo_clear is not None and geo_clear():
                            return True
                        if zone_clear is not None and zone_clear():
                            return True
                        if skill_ended is not None and skill_ended():
                            return True
                        return False
                    # 全没有(极少)才退 None→exit_margin 距离最后兜底
                    is_clear = _is_clear if (geo_clear or zone_clear or skill_ended) else None
                    director.dodge(inline, danger_pos=danger0,
                                   get_danger_pos=get_danger, is_clear=is_clear)
                elif direction.startswith('goto') or direction.startswith('walk'):
                    self._run_auto_walk(director, ctx, inline, direction)
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

    def _run_auto_walk(self, director, ctx, inline, direction):
        # 自动走位(走向目标, 与躲避的远离相反)。独立总开关 auto_walk_enabled 默认关
        # (走位比躲避更激进的 bot 行为); 前台门 + F12 与躲避共用。
        #
        # goto_teammate          靠拢最近队友 (抱团/分摊机制)
        # goto_point:x,z         走向固定世界坐标点
        # goto_circle            走向当前第一个编号圈
        # walk_sequence          按编号顺序走完全部编号圈 (进圈靠区域成员判定推进)
        # ★安全: 目标位置读不到 → director.walk_to 内部直接停, 绝不盲走。
        try:
            if ctx is None or director is None:
                return                 # 审查 #12: 引擎已停/ctx 失效 → 不走
            if not bool(self._get_setting('auto_walk_enabled', False)):
                return
            # 审查 #8: 走位中关掉「自动走位」总开关即时停 (每 tick 复查)
            walk_gate = lambda: bool(self._get_setting('auto_walk_enabled', False))
            arrive_m = float(inline.get('arrive_m') or 2.0)   # 审查 #13: 统一默认 2.0
            move_ms = int(inline.get('move_ms') or 8000)
            if direction.startswith('goto_teammate'):
                obj = ctx.lock_nearest_teammate()
                if not obj:
                    print('[走位] 没找到可靠队友坐标, 取消靠拢')
                    return
                director.walk_to(lambda _o=obj: ctx.read_obj_pos(_o),
                                 arrive_m=arrive_m, max_ms=move_ms,
                                 extra_gate=walk_gate, label='靠拢队友')
            elif direction.startswith('goto_point'):
                try:
                    x, z = [float(v) for v in direction.split(':', 1)[1].split(',')[:2]]
                except Exception:
                    print('[走位] goto_point 坐标解析失败:', direction)
                    return
                py = (ctx.get_player_pos() or (0, 0, 0))[1]
                director.walk_to(lambda: (x, py, z), arrive_m=arrive_m,
                                 max_ms=move_ms, extra_gate=walk_gate, label='走向指定点')
            elif direction.startswith('goto_circle') or direction.startswith('walk_sequence'):
                self._walk_numbered_sequence(director, ctx, inline, walk_gate,
                                             single=direction.startswith('goto_circle'))
        except Exception as e:
            print(f'[走位] auto-walk failed: {e}')

    def _walk_numbered_sequence(self, director, ctx, inline, walk_gate=None,
                                single=False):
        # 按编号顺序走完编号圈: 一个闭环走向当前目标圈; 玩家进圈(区域成员判定)→推进到
        # 下一个; 全部走完或超时即停。圈位置取自追踪器(炸圈 DamagePos 补), 未知则该 tick 停。
        tracker = getattr(self, '_numbered_zone_tracker', None)
        if tracker is None:
            print('[走位] 编号圈追踪器未就绪(尚未遇到机制圈)')
            return
        state = {'idx': 0}

        def _my_uuid():
            # 每次实时读 (auto-offset): 死亡/复活换 uuid 时不会卡在旧值
            try:
                pm = ctx._src.sr.pm
                me = ctx._player()
                return pm.read_i64(me + ctx._off_uuid()) if me else 0
            except Exception:
                return 0

        def current_target():
            seq = tracker.active_sequence()
            i = state['idx']
            if i >= len(seq):
                return None
            return seq[i].pos       # None(位置未知) → walk_to 停, 不盲走

        def is_arrived():
            seq = tracker.active_sequence()
            i = state['idx']
            if i >= len(seq):
                return True
            z = seq[i]
            if z.pos is None:
                return False        # 位置未知就不算到位, 不提前推进 (审查 #14)
            my_uuid = _my_uuid()
            if my_uuid and my_uuid in z.members_now:
                print('[走位] 已进编号圈#%d(组%d), 推进下一个' % (z.seq_in_group, z.group_id))
                state['idx'] = i + 1
                return single or state['idx'] >= len(seq)
            return False
        director.walk_to(current_target, arrive_m=float(inline.get('arrive_m') or 2.0),
                         max_ms=int(inline.get('move_ms') or 12000),
                         is_arrived=is_arrived, extra_gate=walk_gate,
                         label='走向编号圈' if single else '按序走完编号圈')

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

