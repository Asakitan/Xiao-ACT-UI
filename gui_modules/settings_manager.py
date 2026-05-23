# -*- coding: utf-8 -*-
"""
SettingsManager — sao_gui's settings.json persistence layer.

Extracted from sao_gui.py (round 29 of the split refactor). This is the
SAO Entity-mode `SettingsManager`; the codebase also has separate
``config.SettingsManager`` (used by automation/recognition) and
``sao_webview.SettingsManager`` (used by the WebView UI). They share
the same ``settings.json`` file on disk in dev mode, but the three
classes have slightly different APIs (atomic save, legacy-key pruning
sets, etc.) so we keep them as distinct classes.

CONFIG_FILE is resolved relative to the package's parent directory
(``sao_auto/``) — matching the original path computation in sao_gui.
"""

from __future__ import annotations

import json
import os

from config import DEFAULT_HOTKEYS


_HERE = os.path.dirname(os.path.abspath(__file__))  # …/sao_auto/gui_modules
_SAO_AUTO_DIR = os.path.dirname(_HERE)              # …/sao_auto
CONFIG_FILE = os.path.join(_SAO_AUTO_DIR, 'settings.json')


# ══════════════════════════════════════════════════════════
#  Settings Manager (与 gui.py 共享 settings.json)
# ══════════════════════════════════════════════════════════
class SettingsManager:
    _LEGACY_KEYS = (
        'last_file', 'speed', 'transpose', 'chord_mode',
        'show_piano', 'show_viz', 'show_control',
        'piano_x', 'piano_y', 'viz_x', 'viz_y',
        'control_x', 'control_y',
    )

    def __init__(self):
        self.settings = {
            'hotkeys': DEFAULT_HOTKEYS.copy(),
            'ui_mode': 'entity',
        }
        self.load()

    def _prune_legacy_settings(self):
        dirty = False
        for legacy_key in self._LEGACY_KEYS:
            if legacy_key in self.settings:
                self.settings.pop(legacy_key, None)
                dirty = True
        return dirty

    def load(self):
        dirty = False
        try:
            if os.path.exists(CONFIG_FILE):
                with open(CONFIG_FILE, 'r', encoding='utf-8') as f:
                    self.settings.update(json.load(f))
        except Exception:
            pass
        if self.settings.get('ui_mode') == 'sao':
            self.settings['ui_mode'] = 'entity'
            dirty = True
        if self._prune_legacy_settings():
            dirty = True
        if dirty:
            self.save()

    def save(self):
        try:
            self._prune_legacy_settings()
            with open(CONFIG_FILE, 'w', encoding='utf-8') as f:
                json.dump(self.settings, f, indent=2, ensure_ascii=False)
        except Exception:
            pass

    def get(self, key, default=None):
        return self.settings.get(key, default)

    def set(self, key, value):
        self.settings[key] = value
        self.save()
