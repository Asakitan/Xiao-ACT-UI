# -*- coding: utf-8 -*-
"""
SAOPlayerGUIActionsMixin — sixth mixin extracted from SAOPlayerGUI
(round 43 of the sao_gui split refactor). 17 methods, ~152 lines.

Bundles the AutoKey + BossRaid "action" toggles + their config
loaders + their panel-toggle handlers. Both clusters share the same
shape (toggle_panel + toggle_detail_panel + toggle_engine +
config-loader + config-saver + author-snapshot), so they ride
together as a single mixin.

AutoKey cluster:
  * _toggle_autokey_panel, _toggle_autokey_detail_panel — open/close
    the floating panels (quick + detail editor)
  * _toggle_auto_script — toggle the AutoKey engine on/off
  * _auto_key_settings_ref, _auto_key_author_snapshot — settings
    + author-info shims
  * _load_auto_key_config / _save_auto_key_config — config IO
  * _load_autokey_burst_actions / _save_autokey_burst_actions — burst
    action list IO

BossRaid cluster:
  * _toggle_bossraid_panel, _toggle_bossraid_detail_panel — same shape
  * _toggle_boss_raid — toggle the BossRaid engine
  * _boss_raid_next_phase — advance the raid phase
  * _boss_raid_settings_ref, _boss_raid_author_snapshot
  * _load_boss_raid_config / _save_boss_raid_config

Required SAOPlayerGUI attrs:
  * self.root, self.settings, self._cfg_settings_ref
  * self._username, self._profession, self._game_state
  * self._auto_key_engine, self._boss_raid_engine
  * self._autokey_panel, self._autokey_detail_panel,
    self._bossraid_panel, self._bossraid_detail_panel

Required SAOPlayerGUI methods (resolved via MRO):
  * _dismiss_sao_menu_for_panel, _refresh_menu_if_open  (Menu mixin)
  * _raise_panel_window  (SAOPlayerGUI)
  * _get_setting, _set_setting  (SAOPlayerGUI)
"""

from __future__ import annotations

from typing import Any, Dict, List, Optional

from engines.auto_key_engine import (
    load_auto_key_config,
    save_auto_key_config,
    snapshot_author_from_state,
)
from engines.boss_raid_engine import (
    load_boss_raid_config,
    save_boss_raid_config,
)
from gui_modules.sao_gui_autokey import AutoKeyPanel
from gui_modules.sao_gui_bossraid import BossRaidPanel
from gui_modules.sao_gui_profile_editors import AutoKeyDetailPanel, BossRaidDetailPanel


class SAOPlayerGUIActionsMixin:
    """Mixin bundling the AutoKey + BossRaid action toggles + config IO."""

    def _toggle_autokey_panel(self):
        """打开/关闭 AutoKey 配置面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._autokey_panel:
            self._autokey_panel = AutoKeyPanel(
                master=self.root,
                load_fn=self._load_auto_key_config,
                save_fn=self._save_auto_key_config,
                engine_ref=lambda: self._auto_key_engine,
                on_toggle=self._toggle_auto_script,
                author_fn=getattr(self, '_auto_key_author_snapshot', None),
                load_burst_actions=self._load_autokey_burst_actions,
                save_burst_actions=self._save_autokey_burst_actions,
            )
        self._autokey_panel.toggle()
        self.root.after(120, lambda: self._raise_panel_window(self._autokey_panel))

    def _toggle_bossraid_panel(self):
        """打开/关闭 BossRaid 配置面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._bossraid_panel:
            self._bossraid_panel = BossRaidPanel(
                master=self.root,
                load_fn=self._load_boss_raid_config,
                save_fn=self._save_boss_raid_config,
                engine_ref=lambda: self._boss_raid_engine,
                on_toggle=self._toggle_boss_raid,
                on_start=self._toggle_boss_raid,
                on_next=self._boss_raid_next_phase,
                on_reset=lambda: (
                    self._boss_raid_engine.reset() if self._boss_raid_engine else None
                ),
                load_reactions_fn=self._load_boss_reactions_state,
                save_reaction_fn=self._save_boss_reaction,
                mechanics_api=self._mechanics_editor_api(),
            )
        self._bossraid_panel.toggle()
        self.root.after(120, lambda: self._raise_panel_window(self._bossraid_panel))

    def _mechanics_editor_api(self) -> Dict[str, Any]:
        """机制编辑器回调集 (简单面板 + 详细面板共用同一契约)。"""
        from engines import boss_mechanics_state as bms

        def _load(scene_key=None, boss_base_id=None, profile_id=None):
            return bms.build_mechanics_state(
                self._cfg_settings_ref,
                getattr(self, '_boss_raid_engine', None),
                getattr(self, '_state_mgr', None),
                scene_key=scene_key, boss_base_id=boss_base_id,
                profile_id=profile_id)

        def _hot_apply():
            bms.hot_apply_to_engine(self._cfg_settings_ref,
                                    getattr(self, '_boss_raid_engine', None))

        def _save_mech(mech, profile_id=None):
            cfg = bms.upsert_mechanic(self._cfg_settings_ref, mech,
                                      profile_id=profile_id)
            self._presynthesize_active_profile()
            _hot_apply()
            return cfg

        def _delete_mech(mid, profile_id=None):
            cfg = bms.delete_mechanic(self._cfg_settings_ref, mid,
                                      profile_id=profile_id)
            _hot_apply()
            return cfg

        def _create_from_skill(sid, nm='', dur=None, profile_id=None):
            cfg = bms.create_mechanic_from_skill(self._cfg_settings_ref, sid, nm, dur,
                                                 profile_id=profile_id)
            _hot_apply()
            return cfg

        def _bind(mid, sid, profile_id=None):
            cfg = bms.bind_skill_to_mechanic(self._cfg_settings_ref, mid, sid,
                                             profile_id=profile_id)
            _hot_apply()
            return cfg

        def _unbind(mid, sid, profile_id=None):
            cfg = bms.unbind_skill_from_mechanic(self._cfg_settings_ref, mid, sid,
                                                 profile_id=profile_id)
            _hot_apply()
            return cfg

        _TTS_FEEDBACK = {'played': 'TTS已播', 'queued': 'TTS排队中',
                         'synthesizing': 'TTS合成中(首次稍候再试)',
                         'disabled': 'TTS未启用或静音', 'error': 'TTS失败'}

        def _test(mech, kinds):
            # 每种试发都给出可见结果 — 沉默的失败表现为「点了没反应」
            kinds = tuple(kinds or ())
            msgs = []
            controller = getattr(self, '_mech_alert_controller', None)
            if {'tts', 'banner'} & set(kinds):
                if controller is None:
                    msgs.append('提醒控制器未就绪')
                else:
                    res = controller.test_mechanic(mech, kinds) or {}
                    if 'tts' in kinds:
                        msgs.append(_TTS_FEEDBACK.get(res.get('tts'),
                                                      str(res.get('tts') or 'TTS无结果')))
                    if 'banner' in kinds:
                        msgs.append('横幅已显示' if res.get('banner') else '横幅未显示')
            if 'dodge' in kinds:
                linkage = getattr(self, '_boss_autokey_linkage', None)
                inline = ((mech.get('dodge') or {}).get('inline')
                          if isinstance(mech, dict) else None)
                if linkage is None or not isinstance(inline, dict):
                    msgs.append('按键联动不可用')
                elif linkage.fire_mapping_test(inline):
                    msgs.append('按键已发到当前前台窗口')
                else:
                    msgs.append('按键未配置(无键/无序列)')
            msg = ' · '.join(msgs)
            if msg and self._alert_overlay:
                try:
                    self._alert_overlay.show_alert('机制试发', msg)
                except Exception:
                    pass
            return {'ok': True, 'message': msg}

        return {
            'load': _load,
            'save_mech': _save_mech,
            'delete_mech': _delete_mech,
            'test': _test,
            'set_master': lambda flags: bms.set_mechanics_master(self._cfg_settings_ref, flags),
            'create_from_skill': _create_from_skill,
            'bind': _bind,
            'unbind': _unbind,
            'search_catalog': bms.search_skill_catalog,
        }

    def _presynthesize_active_profile(self):
        controller = getattr(self, '_mech_alert_controller', None)
        if controller is None:
            return
        try:
            cfg = self._load_boss_raid_config()
            from engines.boss_raid_engine import active_profile
            profile = active_profile(cfg)
            if profile:
                controller.presynthesize_profile(profile)
        except Exception:
            pass

    def _load_boss_reactions_state(self, scene_key=None, boss_base_id=None) -> Dict[str, Any]:
        """Data contract for the BossRaid panel's Boss 反应 tab (mem-driven editor).
        `scene_key` selects which map/scene to browse (None = live scene);
        `boss_base_id` selects which boss within it (None = first/live)."""
        from engines.boss_autokey_linkage import build_boss_reactions_state
        return build_boss_reactions_state(
            self._cfg_settings_ref,
            getattr(self, '_boss_raid_engine', None),
            getattr(self, '_state_mgr', None),
            scene_key=scene_key,
            boss_base_id=boss_base_id)

    def _save_boss_reaction(self, mapping: Dict[str, Any]) -> Any:
        """Upsert one boss-reaction linkage mapping (boss_cast / offensive window)."""
        from engines.boss_autokey_linkage import upsert_mapping
        return upsert_mapping(self._cfg_settings_ref, mapping)

    def _toggle_autokey_detail_panel(self):
        """Open/close the full AutoKey profile editor."""
        self._dismiss_sao_menu_for_panel()
        if not self._autokey_detail_panel:
            self._autokey_detail_panel = AutoKeyDetailPanel(
                master=self.root,
                load_fn=self._load_auto_key_config,
                save_fn=self._save_auto_key_config,
                author_fn=getattr(self, '_auto_key_author_snapshot', None),
            )
        self._autokey_detail_panel.toggle()
        self.root.after(120, lambda: self._raise_panel_window(self._autokey_detail_panel))

    def _toggle_bossraid_detail_panel(self):
        """Open/close the full BossRaid profile editor."""
        self._dismiss_sao_menu_for_panel()
        if not self._bossraid_detail_panel:
            self._bossraid_detail_panel = BossRaidDetailPanel(
                master=self.root,
                load_fn=self._load_boss_raid_config,
                save_fn=self._save_boss_raid_config,
                author_fn=getattr(self, '_boss_raid_author_snapshot', None),
                load_reactions_fn=self._load_boss_reactions_state,
                save_reaction_fn=self._save_boss_reaction,
                mechanics_api=self._mechanics_editor_api(),
            )
        self._bossraid_detail_panel.toggle()
        self.root.after(120, lambda: self._raise_panel_window(self._bossraid_detail_panel))

    def _toggle_auto_script(self, force_enabled=None):
        """切换 AutoKey 脚本开关."""
        config = self._load_auto_key_config()
        if force_enabled is not None:
            config['enabled'] = bool(force_enabled)
        else:
            config['enabled'] = not bool(config.get('enabled', False))
        self._save_auto_key_config(config)
        state_text = 'ON' if config['enabled'] else 'OFF'
        print(f'[SAO Entity] AUTO KEY: {state_text}')
        self._refresh_menu_if_open()

    def _toggle_boss_raid(self, force_enabled=None):
        """切换 Boss Raid 引擎开关."""
        if not self._boss_raid_engine:
            return
        config = self._load_boss_raid_config()
        if force_enabled is not None:
            config['enabled'] = bool(force_enabled)
        else:
            config['enabled'] = not bool(config.get('enabled', False))
        self._save_boss_raid_config(config)
        state_text = 'ON' if config['enabled'] else 'OFF'
        print(f'[SAO Entity] BOSS RAID: {state_text}')
        self._refresh_menu_if_open()

    def _boss_raid_next_phase(self):
        """Boss Raid 下一阶段."""
        if not self._boss_raid_engine:
            return
        self._boss_raid_engine.next_phase()

    def _auto_key_settings_ref(self):
        return self._cfg_settings_ref or self.settings

    def _auto_key_author_snapshot(self):
        gs = getattr(self, '_game_state', None)
        if gs is not None:
            return snapshot_author_from_state(gs)
        return {'player_uid': '', 'player_name': self._username,
                'profession_id': 0, 'profession_name': self._profession}

    def _load_auto_key_config(self):
        ref = self._auto_key_settings_ref()
        return load_auto_key_config(ref, state_snapshot=self._auto_key_author_snapshot())

    def _save_auto_key_config(self, config):
        ref = self._auto_key_settings_ref()
        saved = save_auto_key_config(ref, config)
        if self._auto_key_engine:
            self._auto_key_engine.invalidate()
        return saved

    def _load_autokey_burst_actions(self):
        actions = self._get_setting('autokey_burst_actions', [])
        return list(actions or [])

    def _save_autokey_burst_actions(self, actions):
        normalized = list(actions or [])
        self._set_setting('autokey_burst_actions', normalized)
        if self._auto_key_engine:
            apply_err = None
            try:
                self._auto_key_engine.set_burst_actions(normalized)
            except Exception as exc:
                apply_err = exc
            try:
                self._auto_key_engine.invalidate()
            except Exception as exc:
                if apply_err is None:
                    apply_err = exc
            if apply_err is not None:
                self._show_entity_alert(
                    'BURST SKILLS', f'已保存, 但引擎应用失败: {apply_err}',
                    display_time=4.0)

    # ── BossRaid config helpers ──
    def _boss_raid_settings_ref(self):
        return self._cfg_settings_ref or self.settings

    def _boss_raid_author_snapshot(self):
        gs = getattr(self, '_game_state', None)
        if gs is not None:
            return snapshot_author_from_state(gs)
        return {'player_uid': '', 'player_name': self._username,
                'profession_id': 0, 'profession_name': self._profession}

    def _load_boss_raid_config(self):
        ref = self._boss_raid_settings_ref()
        return load_boss_raid_config(ref, state_snapshot=self._boss_raid_author_snapshot())

    def _save_boss_raid_config(self, config):
        ref = self._boss_raid_settings_ref()
        return save_boss_raid_config(ref, config)

    # ── Settings helper ──
