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

Round-61 note: the SAOPlayerGUI mixin re-exports were removed from
this __init__.py. They introduced a circular import once sao_theme
started importing gui_modules.sao_menu_hud (sao_theme → __init__ →
mixin → sao_theme). The mixins are sao_gui.py-internal and are
imported via their full dotted path, so the re-exports here were
unused convenience. Only the standalone helper classes that are
actually imported as `from gui_modules import X` stay.
"""

from gui_modules.sao_hotkey_manager import SAOHotkeyManager
from gui_modules.settings_manager import SettingsManager

__all__ = [
    'SAOHotkeyManager',
    'SettingsManager',
]
