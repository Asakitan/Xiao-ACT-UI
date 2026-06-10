# -*- coding: utf-8 -*-
"""
SAOPlayerGUIPacketCallbacksMixin — thirteenth mixin extracted from
SAOPlayerGUI (round 55 of the sao_gui split refactor). 9 methods,
~358 lines.

The packet event callback surface — these are the functions the
PacketBridge calls back into when it observes a game event:

  * _send_linked_key — boss-raid alert -> AutoKey key press. Uses
    Win32 SendInput via auto_key_engine VK_NAME_MAP. The hop the
    BossAutoKeyLinkage takes when it converts a 'boss alert' event
    into a synthesised keypress.
  * _on_packet_damage(event) — fires for every player-vs-monster
    damage tick. Updates the boss-HP target lock and the cached
    last-damage timestamp; this is what keeps the boss HP bar
    visible during a fight.
  * _is_dead_state(gs) — cython-backed predicate (player dead?).
  * _bump_boss_hp_target_hold(reason) — refresh the recent-targets
    map for the current locked target. Called on revive / scene
    re-entry to prevent the bar from fading immediately after
    death.
  * _boss_monster_usable(monster) — predicate + UUID-reuse revive
    side effect (cython core, plus the in-Python is_dead/last_update
    write because we don't want the monster handle leaking into
    native code).
  * _sync_boss_hp_revive_hold(gs) — detects dead → alive transition
    and bumps the target hold.
  * _on_monster_update(monster_data) — packet observed a monster
    update; refresh _bb_recent_targets for the target if the
    monster is usable.
  * _on_boss_event(event) — packet observed a boss-raid event;
    delegates to the boss_raid_engine.
  * _on_scene_change(scene_event) — packet observed a scene
    transition; arm pending combat reset + clear caches.

Required SAOPlayerGUI attrs:
  * self._packet_engine, self._boss_raid_engine,
    self._boss_autokey_linkage
  * self._bb_recent_targets, self._bb_last_target_uuid,
    self._bb_last_damage_ts, self._last_boss_hp_push_sig
  * self._last_dead_state
  * self._alert_overlay, self.root

Required SAOPlayerGUI methods (via MRO):
  * _arm_pending_combat_reset, _refresh_session_players_panel,
    _push_packet_overlays (and other helpers SAOPlayerGUI defines
    inline)
  * _get_setting / _set_setting (SAOPlayerGUI)
"""

from __future__ import annotations

import threading
import time
from typing import Any, Optional

import _sao_cy_uihelpers as _CY_UI  # type: ignore[import-not-found]

from act_platform.runtime import enrich_action_log_event, publish_owner_event, should_record_owner_combat_event
from engines.combat_analytics import boss_state_from_monster_update
from tools.tablekit.combat_preparse import enrich_boss_event, enrich_dungeon_event, enrich_monster_event, enrich_skill_event


class SAOPlayerGUIPacketCallbacksMixin:
    """Mixin bundling packet event callbacks + boss-HP target helpers."""

    def _push_monster_boss_state_to_act(self, monster_data):
        """Mirror packet monster HP/break state into GameState for ACT."""
        try:
            updates = boss_state_from_monster_update(monster_data)
            if updates and getattr(self, '_state_mgr', None):
                self._state_mgr.update(**updates)
                self._push_dps_act_snapshot(throttle=True)
        except Exception:
            pass

    def _on_skill_event(self, event):
        """Normalized TCP skill lifecycle event → shared ACT context."""
        try:
            event = dict(event or {})
            fact = enrich_skill_event(event)
            event.setdefault('combat_fact', fact)
            if fact.get('skill_id') and not event.get('skill_id'):
                event['skill_id'] = fact.get('skill_id')
            if fact.get('skill_name') and not event.get('skill_name'):
                event['skill_name'] = fact.get('skill_name')
            event.setdefault('skill_role', fact.get('skill_role'))
            event.setdefault('sub_profession', fact.get('sub_profession'))
            self._last_skill_event = enrich_action_log_event(event, owner=self, topic='skill')
            if getattr(self, '_state_mgr', None):
                self._state_mgr.update(last_skill_event=self._last_skill_event)
            mgr = getattr(self, '_encounter_mgr', None)
            if mgr is not None:
                mgr.on_skill_event(self._last_skill_event)
            publish_owner_event(self, 'skill', self._last_skill_event, source_name='entity', source_kind='tcp')
        except Exception:
            pass

    def _on_dungeon_event(self, event):
        """Normalized TCP dungeon/scene event → shared ACT context."""
        try:
            event = dict(event or {})
            fact = enrich_dungeon_event(event)
            event.setdefault('combat_fact', fact)
            if fact.get('dungeon_name') and not event.get('dungeon_name'):
                event['dungeon_name'] = fact.get('dungeon_name')
            if fact.get('dungeon_id') and not event.get('dungeon_id'):
                event['dungeon_id'] = fact.get('dungeon_id')
            self._last_dungeon_event = event
            updates = {'last_dungeon_event': event}
            dungeon_id = int(event.get('dungeon_id') or 0)
            scene_id = int(event.get('scene_id') or event.get('scene_uuid')
                           or event.get('cur_map_id') or 0)
            difficulty = int(event.get('dungeon_difficulty')
                             or event.get('difficulty')
                             or event.get('level_id') or 0)
            if dungeon_id > 0:
                updates['dungeon_id'] = dungeon_id
                try:
                    from tools.tablekit.name_tables import names
                    resolved = event.get('dungeon_name') or names.dungeon(dungeon_id, default='')
                    if resolved:
                        updates['dungeon_name'] = resolved
                except Exception:
                    pass
            if scene_id > 0:
                updates['dungeon_scene_id'] = scene_id
            if difficulty > 0:
                updates['dungeon_difficulty'] = difficulty
            # 切换地图中央横幅: 解析地图名后, 延迟 3s 在屏幕中央淡入显示。
            # 覆盖所有场景切换 (副本 dungeon_id + 开放世界 scene_id), 取不到名则不弹。
            banner_name = (updates.get('dungeon_name') or event.get('dungeon_name') or '').strip()
            if not banner_name:
                _lookup = dungeon_id or scene_id
                if _lookup > 0:
                    try:
                        from tools.tablekit.name_tables import names
                        banner_name = (names.dungeon(_lookup, default='') or '').strip()
                    except Exception:
                        banner_name = ''
            if banner_name:
                self._schedule_map_banner(banner_name)
            elif dungeon_id or scene_id:
                # 诊断: 有 id 但查不到名 (帮助定位大地图等场景)
                print(f'[MapBanner][entity] no-name event kind={event.get("kind")!r} '
                      f'dungeon_id={dungeon_id} scene_id={scene_id}', flush=True)
            if getattr(self, '_state_mgr', None):
                self._state_mgr.update(**updates)
            mgr = getattr(self, '_encounter_mgr', None)
            if mgr is not None:
                event_for_mgr = dict(event)
                if 'dungeon_name' not in event_for_mgr and updates.get('dungeon_name'):
                    event_for_mgr['dungeon_name'] = updates.get('dungeon_name')
                mgr.on_dungeon_event(event_for_mgr)
            event = enrich_action_log_event(event, owner=self, topic='dungeon')
            publish_owner_event(self, 'dungeon', event, source_name='entity', source_kind='tcp')
            if scene_id > 0:
                publish_owner_event(self, 'scene', event, source_name='entity', source_kind='tcp')
        except Exception:
            pass

    def _schedule_map_banner(self, name: str):
        """检测到切换地图 → 延迟 3 秒后在屏幕中央淡入地图名 (Entity ULW)。

        去重改为时间窗 (5s): 同名且距上次调度 < 5s 视为同一次进图的重复抓包,
        吞掉防止狂闪; 超过 5s 再次进入同一场景会重新弹 (满足"第二次切入同场景
        也要刷出名字")。快速连切到别的图时取消上一个未触发的延迟, 只显示最新。
        本方法可能在抓包线程被调用, 故用 threading.Timer 计时, 真正渲染由
        MapBannerOverlay.show_banner 内部 root.after(0) 调度回主线程。
        """
        name = (name or '').strip()
        if not name:
            return
        overlay = getattr(self, '_map_banner_overlay', None)
        if overlay is None:
            return
        now = time.time()
        last = getattr(self, '_map_banner_last_name', '')
        last_ts = float(getattr(self, '_map_banner_last_ts', 0.0) or 0.0)
        if name == last and (now - last_ts) < 5.0:
            return
        self._map_banner_last_name = name
        self._map_banner_last_ts = now
        print(f'[MapBanner][entity] schedule name={name!r} (+3s)', flush=True)
        prev = getattr(self, '_map_banner_timer', None)
        if prev is not None:
            try:
                prev.cancel()
            except Exception:
                pass

        def _fire(n=name):
            ov = getattr(self, '_map_banner_overlay', None)
            if ov is not None:
                try:
                    ov.show_banner(n)
                except Exception:
                    pass

        t = threading.Timer(3.0, _fire)
        t.daemon = True
        self._map_banner_timer = t
        t.start()

    def _send_linked_key(self, key: str, press_mode: str = "tap",
                         hold_ms: int = 80, press_count: int = 1):
        """发送联动按键 (Boss→AutoKey linkage)。用 scancode (移动/Shift冲刺/E走等
        躲避键游戏只认扫描码, wVk 不响应)。"""
        try:
            from engines.auto_key_engine import VK_NAME_MAP, INPUT, KEYBDINPUT, INPUT_KEYBOARD, KEYEVENTF_KEYUP
            import ctypes as _ct
            KEYEVENTF_SCANCODE = 0x0008
            key = (key or "").strip().upper()
            vk = VK_NAME_MAP.get(key)
            if vk is None and len(key) == 1 and key.isalpha():
                vk = ord(key)
            if vk is None:
                return
            scan = _ct.windll.user32.MapVirtualKeyW(int(vk), 0)
            if not scan:
                return
            hold_s = max(0.015, hold_ms / 1000.0) if press_mode == "hold" else 0.015
            extra = _ct.c_ulong(0)
            for _ in range(max(1, press_count)):
                ki = KEYBDINPUT(wVk=0, wScan=int(scan), dwFlags=KEYEVENTF_SCANCODE,
                                time=0, dwExtraInfo=_ct.pointer(extra))
                ev = INPUT(type=INPUT_KEYBOARD, ki=ki)
                _ct.windll.user32.SendInput(1, _ct.byref(ev), _ct.sizeof(INPUT))
                time.sleep(hold_s)
                ki2 = KEYBDINPUT(wVk=0, wScan=int(scan),
                                 dwFlags=KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP, time=0,
                                 dwExtraInfo=_ct.pointer(extra))
                ev2 = INPUT(type=INPUT_KEYBOARD, ki=ki2)
                _ct.windll.user32.SendInput(1, _ct.byref(ev2), _ct.sizeof(INPUT))
                if press_count > 1:
                    time.sleep(0.04)
        except Exception as e:
            print(f"[Linkage] send_key error: {e}")

    def _send_key_event(self, key: str, down: bool):
        """低层按下/松开 (定向躲避按住 WASD 用)。

        ★关键: 游戏移动只认 **scancode (扫描码)** 不认 wVk (虚拟键)——用 wVk 发 WASD
        游戏完全不响应(实测)。这里用 MapVirtualKey 把 vk 转成 scancode, 加
        KEYEVENTF_SCANCODE 标志发送, WASD 才会真正驱动人物移动。"""
        try:
            from engines.auto_key_engine import (
                VK_NAME_MAP, INPUT, KEYBDINPUT, INPUT_KEYBOARD, KEYEVENTF_KEYUP)
            import ctypes as _ct
            KEYEVENTF_SCANCODE = 0x0008
            MAPVK_VK_TO_VSC = 0
            k = (key or "").strip().upper()
            vk = VK_NAME_MAP.get(k)
            if vk is None and len(k) == 1 and k.isalpha():
                vk = ord(k)
            if vk is None:
                return
            scan = _ct.windll.user32.MapVirtualKeyW(int(vk), MAPVK_VK_TO_VSC)
            if not scan:
                return
            flags = KEYEVENTF_SCANCODE | (0 if down else KEYEVENTF_KEYUP)
            extra = _ct.c_ulong(0)
            ki = KEYBDINPUT(wVk=0, wScan=int(scan), dwFlags=flags, time=0,
                            dwExtraInfo=_ct.pointer(extra))
            ev = INPUT(type=INPUT_KEYBOARD, ki=ki)
            _ct.windll.user32.SendInput(1, _ct.byref(ev), _ct.sizeof(INPUT))
        except Exception as e:
            print(f"[Dodge] key_event error: {e}")

    def _on_packet_damage(self, event):
        """Damage event callback from packet_parser → boss raid engine + DPS tracker."""
        event = self._normalize_damage_event_for_self(event)
        event = self._normalize_damage_event_target_for_entity(event)
        event = enrich_action_log_event(event, owner=self, topic='damage')
        publish_owner_event(self, 'damage', event, source_name='entity', source_kind='tcp')
        selective_decision = should_record_owner_combat_event(self, event)
        # Track self -> non-player combat target damage for boss bar target.
        # BossHP only displays later if packet_parser has usable HP data.
        try:
            _target_uuid_for_boss_hp = int(event.get('target_uuid', 0) or 0)
        except Exception:
            _target_uuid_for_boss_hp = 0
        _is_friendly_boss_hp_target = bool(
            _target_uuid_for_boss_hp
            and self._is_known_friendly_uid(_target_uuid_for_boss_hp)
        )
        # Hard guard: any UUID with the player suffix (0xFFFF == 640) is
        # a player, period — even if the "known friendly" cache hasn't
        # caught them yet (e.g. teammate joined the party milliseconds
        # before their first heal/buff event). Without this, party-heal
        # events leak the teammate's UUID into _bb_recent_targets and
        # the boss bar picks them by max_hp, showing a 50M+ "monster"
        # while the real boss is only ~1M. See user report 2026-05-24
        # ("怪物血量会把队友血量算进去").
        _target_is_player_suffix = bool(
            _target_uuid_for_boss_hp
            and (int(_target_uuid_for_boss_hp) & 0xFFFF) == 640
        )
        _is_self_combat_target = bool(
            event.get('attacker_is_self')
            and _target_uuid_for_boss_hp
            and not _is_friendly_boss_hp_target
            and not _target_is_player_suffix
            and (
                event.get('target_is_combat_target', False)
                or event.get('target_is_monster', False)
                or ('target_is_player' in event and not event.get('target_is_player', False))
            )
        )
        self._maybe_apply_pending_combat_reset(event, _is_self_combat_target)
        if self._boss_raid_engine:
            try: self._boss_raid_engine.on_damage_event(event)
            except Exception: pass
        if _is_self_combat_target:
            self._cancel_dps_idle_reset_after()
            target_uuid = event.get('target_uuid', 0)
            if target_uuid:
                damage_now = time.time()
                # ── Duplicate-UUID revive (v2.3.14) ─────────────────────
                # When a server reuses a monster UUID (mob respawn in
                # the same dungeon, or monster instance churn without a
                # NotifyStartPlayingDungeon), the parser still has the
                # old MonsterData with ``is_dead=True``. The recognition
                # loop filter below (``not getattr(_m, 'is_dead', False)``)
                # excludes it → BossHP panel never pops up. Damage
                # arriving for a UUID is proof that a live unit holds
                # that UUID right now, so clear the stale flag.
                _bridge = getattr(self, '_packet_engine', None)
                if _bridge is not None:
                    try:
                        _m = _bridge.get_monster(target_uuid)
                        if _m is not None and getattr(_m, 'is_dead', False):
                            _m.is_dead = False
                            _m.last_update = damage_now
                    except Exception:
                        pass
                self._bb_recent_targets[target_uuid] = damage_now
                self._bb_last_target_uuid = target_uuid
                self._bb_last_damage_ts = damage_now
                self._scene_hide_token = int(getattr(self, '_scene_hide_token', 0) or 0) + 1
                # Damage is also liveness. Some encounters lock HP while
                # still accepting hits, so do not let stable-HP hiding win.
                self._bb_last_hp_motion_ts = self._bb_last_damage_ts
                self._last_boss_hp_push_sig = None
                try:
                    if _bridge is not None:
                        _m = _bridge.get_monster(target_uuid)
                        if _m is not None:
                            self._push_monster_boss_state_to_act(
                                _m.to_dict() if hasattr(_m, 'to_dict') else _m)
                except Exception:
                    pass
                if bool(selective_decision.get('record', True)) and self._dps_tracker:
                    try: self._dps_tracker.set_boss_uuid(target_uuid)
                    except Exception: pass
        if bool(selective_decision.get('record', True)):
            if self._dps_tracker:
                try: self._dps_tracker.on_damage_event(event)
                except Exception: pass
            try:
                mgr = getattr(self, '_encounter_mgr', None)
                if mgr is not None:
                    mgr.on_damage_event(event)
            except Exception:
                pass
            try:
                self._push_dps_act_snapshot(throttle=True)
            except Exception:
                pass

    def _is_dead_state(self, gs) -> bool:
        if gs is None:
            return False
        # v2.4.31: predicate moved to cython.
        return bool(_CY_UI.is_dead_state(
            getattr(gs, 'hp_max', 0),
            getattr(gs, 'hp_current', 0),
            getattr(gs, 'hp_pct', 1.0),
        ))

    def _bump_boss_hp_target_hold(self, reason: str = ''):
        target_uuid = int(getattr(self, '_bb_last_target_uuid', 0) or 0)
        if not target_uuid:
            return
        bridge = getattr(self, '_packet_engine', None)
        if not bridge:
            return
        try:
            monster = bridge.get_monster(target_uuid)
            if not monster:
                return
            if not self._boss_monster_usable(monster):
                return
            now = time.time()
            self._bb_recent_targets[target_uuid] = now
            self._bb_last_damage_ts = now
            self._last_boss_hp_push_sig = None
        except Exception:
            pass

    def _boss_monster_usable(self, monster) -> bool:
        if not monster:
            return False
        # v2.4.31: pure predicate in cython; keep the side effect (UUID-reuse
        # revive) here so we don't pass the monster handle to native code.
        try:
            usable, revive = _CY_UI.boss_monster_usable(
                getattr(monster, 'hp', 0),
                getattr(monster, 'max_hp', 0),
                bool(getattr(monster, 'is_dead', False)),
            )
            if revive:
                monster.is_dead = False
                monster.last_update = time.time()
            return bool(usable)
        except Exception:
            return False

    def _sync_boss_hp_revive_hold(self, gs):
        dead_now = self._is_dead_state(gs)
        dead_prev = bool(getattr(self, '_last_dead_state', False))
        if (not dead_now) and dead_prev:
            self._bump_boss_hp_target_hold('revive')
        self._last_dead_state = dead_now

    def _on_monster_update(self, monster_data):
        """Monster update from packet_parser → boss raid engine + BossHP pretrack."""
        try:
            if isinstance(monster_data, dict):
                fact = enrich_monster_event(monster_data)
                monster_data.setdefault('combat_fact', fact)
                monster_data.setdefault('mechanics', fact.get('mechanics') or [])
                if fact.get('monster_name') and not monster_data.get('monster_name'):
                    monster_data['monster_name'] = fact.get('monster_name')
        except Exception:
            pass
        # ACT per-target cross-tab: feed resolved CN monster name to the tracker.
        try:
            if getattr(self, '_dps_tracker', None) is not None:
                _mu = int(monster_data.get('uuid', 0) or 0)
                _mn = str(monster_data.get('monster_name') or monster_data.get('name') or '')
                if _mu and _mn:
                    self._dps_tracker.update_monster_info(_mu, _mn)
        except Exception:
            pass
        publish_owner_event(self, 'monster', monster_data, source_name='entity', source_kind='tcp')
        if self._boss_raid_engine:
            try: self._boss_raid_engine.on_monster_update(monster_data)
            except Exception: pass
        try:
            _uuid = int(monster_data.get('uuid', 0) or 0)
            _max_hp = int(monster_data.get('max_hp', 0) or 0)
            _hp = int(monster_data.get('hp', 0) or 0)
            _is_dead = bool(monster_data.get('is_dead', False))
            if (_uuid and _uuid == int(getattr(self, '_bb_last_target_uuid', 0) or 0)
                    and (_max_hp > 0 or _hp > 0) and (not _is_dead or _hp > 0)):
                self._last_boss_hp_push_sig = None
                self._push_monster_boss_state_to_act(monster_data)
        except Exception:
            pass
        # Buff 监视器 (boss): 仅推当前锁定 boss target 的 buff_list
        try:
            ov = getattr(self, '_boss_buff_overlay', None)
            if ov is not None:
                _uuid = int(monster_data.get('uuid', 0) or 0)
                _target = int(getattr(self, '_bb_last_target_uuid', 0) or 0)
                if _uuid and _target and _uuid == _target:
                    if monster_data.get('is_dead', False):
                        ov.clear_target()
                    else:
                        raw_buffs = monster_data.get('buff_list', None) or []
                        # bridge 不走 monster 路, 这里现场 resolve buff 名字
                        from net.packet_bridge import _get_skill_name as _bm_name
                        packed = []
                        for b in raw_buffs:
                            if not isinstance(b, dict):
                                continue
                            bid = int(b.get('buff_id', 0) or 0)
                            if bid <= 0:
                                continue
                            packed.append({
                                'id': bid,
                                'uuid': int(b.get('buff_uuid', 0) or 0),
                                'begin_ms': int(b.get('begin_time', 0) or 0),
                                'duration_ms': int(b.get('duration', 0) or 0),
                                'layer': int(b.get('layer', 0) or 0),
                                'count': int(b.get('count', 0) or 0),
                                'name': _bm_name(bid),
                            })
                        # 把 server offset 一起带过去
                        try:
                            offset = float(getattr(self._state_mgr.state,
                                                   'server_time_offset_ms', 0.0) or 0.0)
                        except Exception:
                            offset = 0.0
                        ov.update_target(
                            _uuid, monster_data.get('name', ''), packed, offset)
                        # First-time debug print: 第一次成功推 boss buff
                        if not getattr(self, '_dbg_buffmon_boss_first', False):
                            self._dbg_buffmon_boss_first = True
                            try:
                                from config import BUFFMON_DEBUG as _bm_dbg
                            except Exception:
                                _bm_dbg = False
                            if _bm_dbg:
                                print(f'[SAO Entity] BuffMon boss FIRST push: '
                                      f'target_uuid={_uuid} name={monster_data.get("name","")!r} '
                                      f'buffs={len(packed)}', flush=True)
        except Exception:
            pass

    def _on_boss_event(self, event):
        """Boss buff/event callback from packet_parser → boss raid engine."""
        try:
            event = dict(event or {})
            fact = enrich_boss_event(event)
            event.setdefault('combat_fact', fact)
            event.setdefault('boss_mechanic_key', fact.get('boss_mechanic_key'))
            event.setdefault('boss_mechanic_label', fact.get('boss_mechanic_label'))
            event.setdefault('trigger_family', fact.get('trigger_family'))
            self._last_boss_event = event
            if getattr(self, '_state_mgr', None):
                self._state_mgr.update(last_boss_event=self._last_boss_event)
            self._push_dps_act_snapshot()
            publish_owner_event(self, 'boss', self._last_boss_event, source_name='entity', source_kind='tcp')
        except Exception:
            pass
        if self._boss_raid_engine:
            try: self._boss_raid_engine.on_boss_event(event)
            except Exception: pass

    def _on_scene_change(self, scene_event=None):
        """Packet parser scene/retry callback: clear DPS/BossHP encounter state."""
        _scene_kind = ''
        _scene_reason = ''
        _preserve_combat = False
        _reset_on_next_damage = False
        if isinstance(scene_event, dict):
            _scene_kind = str(scene_event.get('kind') or '')
            _scene_reason = str(scene_event.get('reason') or '')
            _preserve_combat = bool(scene_event.get('preserve_combat', False))
            _reset_on_next_damage = bool(scene_event.get('reset_on_next_damage', False))
        if _reset_on_next_damage:
            # Only defer the reset when a fight is genuinely in progress. If
            # the prior encounter is already idle (no recent self/party
            # outgoing damage), there is nothing live to protect — fall through
            # to the immediate hard reset below so stale combat data cannot be
            # kept alive by the post-scene grace window. Without this, after
            # repeatedly re-entering a dungeon the DPS panel could stay stuck
            # visible and never fade. (User-confirmed fix direction A.)
            _combat_live = False
            try:
                if self._dps_tracker is not None:
                    _combat_live = bool(self._dps_tracker.has_recent_damage(
                        self._combat_damage_timeout_s()))
            except Exception:
                _combat_live = False
            if _combat_live:
                self._arm_pending_combat_reset(scene_event)
                return
            # Idle prior fight → force the hard-reset path (skip preserve).
            _preserve_combat = False
        if _preserve_combat:
            # Same-dungeon layer/map transitions are common during long fights.
            # Keep the live encounter; only force the next overlay tick to refresh.
            print(
                f'[SAO Entity] ↔ 同副本软切换({ _scene_kind or "transition" }/{_scene_reason}) '
                    f'— 保留 BossHP 与 DPS',
                flush=True,
            )
            self._last_boss_hp_push_sig = None
            self._scene_damage_grace_until = max(
                float(getattr(self, '_scene_damage_grace_until', 0.0) or 0.0),
                time.time() + 10.0,
            )
            try:
                if self._dps_tracker:
                    self._dps_tracker.invalidate_snapshot_cache()
            except Exception:
                pass
            return

        _scene_change_ts = time.time()
        self._pending_combat_reset_after = 0.0
        self._pending_combat_reset_reason = ''
        self._scene_damage_grace_until = _scene_change_ts + 15.0
        print('[SAO Entity] ⚡ 场景/同副本重开 — 重置 BossHP 与 DPS 追踪', flush=True)
        self._bb_last_target_uuid = 0
        self._bb_last_damage_ts = 0.0
        self._bb_recent_targets = {}
        self._bb_last_hp_motion_sig = None
        self._bb_last_hp_motion_ts = 0.0
        self._last_boss_hp_push_sig = None

        try:
            if self._state_mgr:
                self._state_mgr.update(
                    boss_breaking_stage=-1,
                    boss_extinction_pct=0.0,
                    boss_current_hp=0,
                    boss_total_hp=0,
                    boss_hp_source='none',
                    boss_hp_est_pct=1.0,
                    boss_shield_active=False,
                    boss_shield_pct=0.0,
                    boss_in_overdrive=False,
                    boss_invincible=False,
                )
        except Exception:
            pass

        if self._dps_tracker:
            try:
                self._dps_tracker.reset()
                print('[SAO Entity] DPS tracker reset on scene change', flush=True)
            except Exception:
                pass
        try:
            mgr = getattr(self, '_encounter_mgr', None)
            if mgr is not None:
                mgr.reset('scene_change')
        except Exception:
            pass
        self._dps_visible = False
        self._dps_faded = False
        self._dps_mode = 'hidden'

        if self._boss_raid_engine:
            try:
                if getattr(self._boss_raid_engine, '_state', '') != 'running':
                    self._boss_raid_engine.reset()
            except Exception:
                pass

        # Hard scene reset clears encounter data, but keep overlay windows
        # ready for the first fresh damage packet. Destroying the DPS window
        # here races the post-transition rebuild path and can leave the panel
        # visually suppressed after repeated map switches.
        try:
            if self._boss_hp_overlay:
                self._boss_hp_overlay.update({'active': False})
        except Exception:
            pass
        try:
            if self._boss_buff_overlay:
                self._boss_buff_overlay.clear_target()
        except Exception:
            pass
        try:
            self._sync_dps_report_availability()
        except Exception:
            pass

        self._scene_hide_token = int(getattr(self, '_scene_hide_token', 0) or 0) + 1
        _hide_token = self._scene_hide_token

        def _hide_overlays():
            # Re-hide once more after settling. The deferred pass is for any
            # paint that managed to push between the immediate hide above and
            # the next overlay tick.
            if _hide_token != int(getattr(self, '_scene_hide_token', 0) or 0):
                return
            _has_new_self_damage = bool(self._bb_last_damage_ts > _scene_change_ts)
            _has_new_dps = False
            try:
                _has_new_dps = bool(self._dps_tracker and self._dps_tracker.has_recent_damage(1.0))
            except Exception:
                _has_new_dps = False
            try:
                if (self._boss_hp_overlay
                        and not _has_new_self_damage
                        and not bool(getattr(self, '_bb_recent_targets', None))):
                    self._boss_hp_overlay.update({'active': False})
            except Exception:
                pass
            # Do not call DpsOverlay.hide() from this deferred guard. A fresh
            # post-transition damage packet can rebuild the tracker before the
            # next overlay tick has flipped _dps_visible back to True; hiding
            # here races that first live update and can leave the DPS panel
            # visually suppressed after several map switches.
            try:
                self._sync_dps_report_availability()
            except Exception:
                pass

        try:
            self.root.after(120, _hide_overlays)
        except Exception:
            pass

