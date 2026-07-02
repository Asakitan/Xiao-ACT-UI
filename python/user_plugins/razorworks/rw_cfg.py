# -*- coding: utf-8 -*-
"""RW module — settings persistence.

Saves all feature parameters to rw_config.json next to plugin.py.
Loads on startup, saves on change or explicit save action.
Merges with defaults so new settings auto-appear after updates.
"""

from __future__ import annotations

import json
import os
from typing import Any, Dict, Optional

_SETTINGS_FILE = "rw_config.json"

DEFAULTS: Dict[str, Any] = {
    # General
    "sensitivity": 1.0,
    "tick_s": 0.008,
    "viewport_w": 1920,
    "viewport_h": 1080,

    # ESP
    "show_health": True,
    "show_skeleton": True,
    "show_distance": True,
    "show_snapline": False,
    "show_head": True,
    "show_box": True,
    "show_hp_bar": True,
    "show_fov_circle": False,

    # Tracker
    "tk_on": False,
    "tk_fov": 5.0,
    "tk_smooth": 0.25,
    "tk_spd": 1.0,
    "tk_over": 0.0,
    "tk_curve": "ease_out",
    "tk_jit": 0.06,
    "tk_off": 0.0,
    "tk_react": 180,
    "tk_hr": 0.72,
    "tk_missj": 0.4,
    "tk_strat": "crosshair",
    "tk_pri": "head,chest",
    "tk_sec": "neck,stomach",
    "tk_maxd": 0,
    "tk_vis": "both",

    # RCS
    "cp_on": False,
    "cp_str": 1.0,
    "cp_hstr": 1.0,
    "cp_start": 1,
    "cp_max": 0,
    "cp_alpha": 0.5,
    "cp_ramp": 4,
    "cp_hr": 1.0,
    "cp_j": 0.0,

    # Reactor
    "rt_on": False,
    "rt_seed": False,
    "rt_dmin": 160,
    "rt_dmax": 280,
    "rt_cd": 80,
    "rt_chance": 0.65,
    "rt_spread": 0.8,
    "rt_hr": 0.75,
    "rt_bmax": 0,
    "rt_bcd": 500,
    "rt_pri": "head",
    "rt_sec": "neck,chest",
    "rt_vis": "both",

    # Timeshift
    "bt_on": False,
    "bt_win": 50.0,
    "st_on": False,
    "ip_on": False,
    "ip_delay": 0.0,
    "xp_on": False,
    "xp_ahead": 20.0,
    "xp_conf": 0.3,

    # Visibility
    "vis_mp": True,
    "vis_ec": 5,

    # Knife/Prox-Z
    "px_k": False,
    "px_z": False,
    "px_kb": 0,
    "px_zb": 0,

    # Key binds (VK codes, 0=always) + mode (hold/toggle)
    "tk_bind": 0,
    "tk_bind_mode": "hold",
    "rt_bind": 0,
    "rt_bind_mode": "hold",
    "px_kb_mode": "hold",
    "px_zb_mode": "hold",

    # Hop
    "hop_on": False,

    # Nade helper
    "gd_on": False,
    "gd_auto": False,
    "gd_prox": 128.0,

    # Radar (overlay on game radar)
    "map_on": False,
    "map_px": 256,  # legacy key, kept for backward-compat; no live effect (see rw_map.py)
    "map_hs": 1.0,
    "map_rs": 0.4,
    "map_override": False,  # when True, map_hs/map_rs are passed as explicit overrides

    # Pattern scan
    "use_sig": True,

    # Cloak (kernel-level concealment)
    "cloak_on": False,
}


class CfgMgr:
    """Load/save settings to JSON file, merge with defaults."""

    def __init__(self, plugin_dir: str) -> None:
        self._path = os.path.join(plugin_dir, _SETTINGS_FILE)
        self._data: Dict[str, Any] = dict(DEFAULTS)
        self._dirty = False

    def load(self) -> None:
        if not os.path.isfile(self._path):
            return
        try:
            with open(self._path, "r", encoding="utf-8") as f:
                saved = json.load(f)
            if isinstance(saved, dict):
                self._data.update(saved)
        except Exception:
            pass

    def save(self) -> None:
        try:
            with open(self._path, "w", encoding="utf-8") as f:
                json.dump(self._data, f, indent=2, ensure_ascii=False)
            self._dirty = False
        except Exception:
            pass

    def get(self, key: str, default: Any = None) -> Any:
        return self._data.get(key, DEFAULTS.get(key, default))

    def set(self, key: str, value: Any) -> None:
        if self._data.get(key) != value:
            self._data[key] = value
            self._dirty = True

    def set_many(self, updates: Dict[str, Any]) -> None:
        for k, v in updates.items():
            self.set(k, v)

    def save_if_dirty(self) -> None:
        if self._dirty:
            self.save()

    def reset_to_defaults(self) -> None:
        self._data = dict(DEFAULTS)
        self._dirty = True

    def all_settings(self) -> Dict[str, Any]:
        return dict(self._data)

    @property
    def dirty(self) -> bool:
        return self._dirty
