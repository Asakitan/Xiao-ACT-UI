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
import shutil
import tempfile
from typing import Any, Mapping

from config import DEFAULT_HOTKEYS


_HERE = os.path.dirname(os.path.abspath(__file__))  # …/sao_auto/gui_modules
_SAO_AUTO_DIR = os.path.dirname(_HERE)              # …/sao_auto
CONFIG_FILE = os.path.join(_SAO_AUTO_DIR, 'settings.json')


def _json_safe(value: Any) -> Any:
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, Mapping):
        return {str(k): _json_safe(v) for k, v in value.items() if not callable(v)}
    if isinstance(value, (list, tuple, set)):
        return [_json_safe(v) for v in value if not callable(v)]
    try:
        return json.loads(json.dumps(value, ensure_ascii=False, default=str))
    except Exception:
        return str(value)


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

    def _backup_corrupt_file(self, exc):
        try:
            if not os.path.exists(CONFIG_FILE):
                return
            backup = CONFIG_FILE + '.corrupt'
            if os.path.exists(backup):
                backup = f'{CONFIG_FILE}.corrupt.{os.getpid()}'
            shutil.copy2(CONFIG_FILE, backup)
            print(f'[Settings] Invalid settings JSON backed up to {backup}: {exc}')
        except Exception as backup_exc:
            print(f'[Settings] Invalid settings JSON; backup failed: {backup_exc} ({exc})')

    def load(self):
        dirty = False
        try:
            if os.path.exists(CONFIG_FILE):
                with open(CONFIG_FILE, 'r', encoding='utf-8') as f:
                    loaded = json.load(f)
                if not isinstance(loaded, dict):
                    raise ValueError('settings root must be an object')
                self.settings.update(loaded)
        except Exception as exc:
            self._backup_corrupt_file(exc)
        if self.settings.get('ui_mode') == 'sao':
            self.settings['ui_mode'] = 'entity'
            dirty = True
        if self._prune_legacy_settings():
            dirty = True
        if dirty:
            self.save()

    def save(self):
        tmp_path = ''
        try:
            self._prune_legacy_settings()
            safe_settings = _json_safe(self.settings)
            if not isinstance(safe_settings, dict):
                raise ValueError('settings root must be an object')
            blob = json.dumps(safe_settings, indent=2, ensure_ascii=False)
            dir_name = os.path.dirname(CONFIG_FILE) or os.getcwd()
            os.makedirs(dir_name, exist_ok=True)
            with tempfile.NamedTemporaryFile(
                mode='w', dir=dir_name, delete=False,
                encoding='utf-8', suffix='.tmp.json',
            ) as tmp:
                tmp.write(blob)
                tmp.flush()
                os.fsync(tmp.fileno())
                tmp_path = tmp.name
            os.replace(tmp_path, CONFIG_FILE)
            self.settings = safe_settings
        except Exception as exc:
            print(f'[Settings] Save failed: {exc} (path={CONFIG_FILE}); previous settings kept')
            try:
                if tmp_path and os.path.exists(tmp_path):
                    os.remove(tmp_path)
            except Exception:
                pass

    def get(self, key, default=None):
        return self.settings.get(key, default)

    def set(self, key, value):
        self.settings[key] = value
        self.save()
