# -*- coding: utf-8 -*-
# SAOPlayerGUIActionsMixin — sixth mixin extracted from SAOPlayerGUI
# (round 43 of the sao_gui split refactor). 17 methods, ~152 lines.
#
# Bundles the AutoKey + BossRaid action toggles and their config loaders.
#
# AutoKey cluster:
# * _toggle_auto_script — toggle the AutoKey engine on/off
# * _auto_key_settings_ref, _auto_key_author_snapshot — settings
# + author-info shims
# * _load_auto_key_config / _save_auto_key_config — config IO
# * _load_autokey_burst_actions / _save_autokey_burst_actions — burst
# action list IO
#
# BossRaid cluster:
# * _toggle_boss_raid — toggle the BossRaid engine
# * _boss_raid_next_phase — advance the raid phase
# * _boss_raid_settings_ref, _boss_raid_author_snapshot
# * _load_boss_raid_config / _save_boss_raid_config
#
# Required SAOPlayerGUI attrs:
# * self.root, self.settings, self._cfg_settings_ref
# * self._username, self._profession, self._game_state
# * self._auto_key_engine, self._boss_raid_engine
#
# Required SAOPlayerGUI methods (resolved via MRO):
# * _dismiss_sao_menu_for_panel, _refresh_menu_if_open  (Menu mixin)
# * _raise_panel_window  (SAOPlayerGUI)
# * _get_setting, _set_setting  (SAOPlayerGUI)

from __future__ import annotations

from typing import Any, Dict, List, Optional



class SAOPlayerGUIActionsMixin:
    # Mixin bundling the AutoKey + BossRaid action toggles + config IO.

    def _mechanics_editor_api(self) -> Dict[str, Any]:
        # 机制编辑器回调集 — 游戏插件覆盖此方法。
        return {}

    def _presynthesize_active_profile(self):
        # 预合成 boss profile — 游戏插件覆盖此方法。
        pass

    def _load_boss_reactions_state(self, scene_key=None, boss_base_id=None) -> Dict[str, Any]:
        # Boss 反应状态 — 游戏插件覆盖此方法。
        return {}

    def _save_boss_reaction(self, mapping: Dict[str, Any]) -> Any:
        # 保存 boss 反应映射 — 游戏插件覆盖此方法。
        return None

    def _set_linkage_field(self, field: str, value: Any) -> Any:
        # 设置联动字段 — 游戏插件覆盖此方法。
        return None

    def _toggle_auto_script(self, force_enabled=None):
        # 切换 AutoKey 脚本开关.
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
        # 切换 Boss Raid 引擎开关.
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
        # Boss Raid 下一阶段.
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
        # 双端诚实反馈: web autokey_editor.saveActions() 成功弹「已保存 N 条动作」、
        # 失败弹「保存失败」; Tk 此前只在引擎应用失败时给提示, 成功静默、持久化
        # 异常被上层 except 吞掉 → 与 web 不一致。补齐成功反馈 + 持久化失败反馈。
        normalized = list(actions or [])
        try:
            self._set_setting('autokey_burst_actions', normalized)
        except Exception as exc:
            self._show_entity_alert(
                'BURST SKILLS', f'保存失败: {exc}', display_time=4.0)
            return
        apply_err = None
        if self._auto_key_engine:
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
        else:
            self._show_entity_alert(
                'BURST SKILLS', f'已保存 {len(normalized)} 条动作',
                display_time=2.0)

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
