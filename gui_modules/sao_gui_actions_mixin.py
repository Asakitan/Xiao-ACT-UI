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
            )
        self._bossraid_panel.toggle()
        self.root.after(120, lambda: self._raise_panel_window(self._bossraid_panel))

    def _load_boss_reactions_state(self) -> Dict[str, Any]:
        """Data contract for the BossRaid panel's Boss 反应 tab (mem-driven editor)."""
        from engines.boss_autokey_linkage import build_boss_reactions_state
        return build_boss_reactions_state(
            self._cfg_settings_ref,
            getattr(self, '_boss_raid_engine', None),
            getattr(self, '_state_mgr', None))

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
            try:
                self._auto_key_engine.set_burst_actions(normalized)
            except Exception:
                pass
            try:
                self._auto_key_engine.invalidate()
            except Exception:
                pass

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
