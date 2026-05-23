# -*- coding: utf-8 -*-
"""
gui_modules — extracted sub-modules of sao_gui.py.

v3.2.0 round 25+: sao_gui.py grew to 9682 lines (top-level monolith
holding 12 classes, the biggest being SAOPlayerGUI at ~7800 lines).
The user asked for a folder-based refactor that:
  - keeps the program working end-to-end (entry points + imports
    must still resolve),
  - reduces sao_gui.py size,
  - groups related classes/helpers into focused submodules.

This package collects extracted widgets / managers / helpers. Each
extraction is a separate /loop round so we can verify imports + run
smoke tests before moving on.
"""

from gui_modules.sao_hotkey_manager import SAOHotkeyManager
from gui_modules.sao_session_players_panel import SAOSessionPlayersPanel
from gui_modules.sao_player_panel import SAOPlayerPanel
from gui_modules.sao_menu_left_stack import SAOMenuLeftStack
from gui_modules.settings_manager import SettingsManager
from gui_modules.sao_gui_session_mixin import SAOPlayerGUISessionMixin
from gui_modules.sao_gui_state_mixin import SAOPlayerGUIStateMixin
from gui_modules.sao_gui_menu_mixin import SAOPlayerGUIMenuMixin
from gui_modules.sao_gui_fisheye_mixin import SAOPlayerGUIFisheyeMixin
from gui_modules.sao_gui_actions_mixin import SAOPlayerGUIActionsMixin
from gui_modules.sao_gui_engine_toggles_mixin import SAOPlayerGUIEngineTogglesMixin
from gui_modules.sao_gui_dps_theme_mixin import SAOPlayerGUIDpsThemeMixin
from gui_modules.sao_gui_panels_mixin import SAOPlayerGUIPanelsMixin

__all__ = [
    'SAOHotkeyManager',
    'SAOSessionPlayersPanel',
    'SAOPlayerPanel',
    'SAOMenuLeftStack',
    'SettingsManager',
    'SAOPlayerGUISessionMixin',
    'SAOPlayerGUIStateMixin',
    'SAOPlayerGUIMenuMixin',
    'SAOPlayerGUIFisheyeMixin',
    'SAOPlayerGUIActionsMixin',
    'SAOPlayerGUIEngineTogglesMixin',
    'SAOPlayerGUIDpsThemeMixin',
    'SAOPlayerGUIPanelsMixin',
]
