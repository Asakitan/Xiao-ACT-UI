# -*- coding: utf-8 -*-
"""Boss Raid → Auto Key linkage controller.

Listens to boss raid phase/timeline events and triggers mapped auto-key actions.
Supports cooldown, dedup, execution lock, and debug logging.
"""

import copy
import json
import threading
import time
import uuid
from typing import Any, Callable, Dict, List, Optional

# ════════════════════════════════════════
#  Helpers
# ════════════════════════════════════════

def _s(v: Any) -> str:
    return str(v or "").strip()


def _bool(v: Any, default: bool = False) -> bool:
    if isinstance(v, bool):
        return v
    if isinstance(v, str):
        t = v.strip().lower()
        return t in ("1", "true", "yes", "on")
    return bool(v) if isinstance(v, (int, float)) else default


def _float(v: Any, default: float = 0.0, lo: Optional[float] = None,
           hi: Optional[float] = None) -> float:
    try:
        r = float(v)
    except Exception:
        r = default
    if lo is not None:
        r = max(lo, r)
    if hi is not None:
        r = min(hi, r)
    return r


def _int(v: Any, default: int = 0, lo: Optional[int] = None,
         hi: Optional[int] = None) -> int:
    try:
        r = int(v)
    except Exception:
        r = default
    if lo is not None:
        r = max(lo, r)
    if hi is not None:
        r = min(hi, r)
    return r


def _new_id() -> str:
    return f"lnk_{uuid.uuid4().hex[:12]}"


# ════════════════════════════════════════
#  Data model
# ════════════════════════════════════════

TRIGGER_TYPES = (
    "phase_enter",      # boss raid enters a phase matching trigger_match
    "timeline_alert",   # timeline fires an alert matching trigger_match
    "breaking",         # boss enters breaking state
    "enrage",           # enrage timer reached
)


def make_default_mapping() -> Dict[str, Any]:
    return {
        "id": _new_id(),
        "enabled": True,
        "trigger_type": "phase_enter",
        "trigger_match": "",
        "action_key": "",        # key to press (e.g. "1", "Q", "SPACE")
        "action_label": "",      # human-readable label
        "press_mode": "tap",     # tap | hold
        "hold_ms": 80,
        "press_count": 1,
        "cooldown_s": 3.0,
    }


def normalize_mapping(raw: Any) -> Dict[str, Any]:
    src = raw if isinstance(raw, dict) else {}
    trigger = _s(src.get("trigger_type")).lower()
    if trigger not in TRIGGER_TYPES:
        trigger = "phase_enter"
    press_mode = _s(src.get("press_mode")).lower()
    if press_mode not in ("tap", "hold"):
        press_mode = "tap"
    return {
        "id": _s(src.get("id")) or _new_id(),
        "enabled": _bool(src.get("enabled"), True),
        "trigger_type": trigger,
        "trigger_match": _s(src.get("trigger_match")),
        "action_key": _s(src.get("action_key")).upper(),
        "action_label": _s(src.get("action_label")),
        "press_mode": press_mode,
        "hold_ms": _int(src.get("hold_ms"), 80, 0, 10000),
        "press_count": _int(src.get("press_count"), 1, 1, 20),
        "cooldown_s": _float(src.get("cooldown_s"), 3.0, 0.0, 600.0),
    }


def default_linkage_config() -> Dict[str, Any]:
    return {
        "enabled": False,
        "global_cooldown_s": 1.0,
        "debug_log": False,
        "mappings": [],
    }


def normalize_linkage_config(raw: Any) -> Dict[str, Any]:
    src = raw if isinstance(raw, dict) else {}
    mappings = []
    for item in (src.get("mappings") or []):
        if isinstance(item, dict):
            mappings.append(normalize_mapping(item))
    return {
        "enabled": _bool(src.get("enabled"), False),
        "global_cooldown_s": _float(src.get("global_cooldown_s"), 1.0, 0.0, 60.0),
        "debug_log": _bool(src.get("debug_log"), False),
        "mappings": mappings,
    }


def load_linkage_config(settings) -> Dict[str, Any]:
    return normalize_linkage_config(settings.get("boss_autokey_linkage", {}))


def save_linkage_config(settings, config: Dict[str, Any]) -> Dict[str, Any]:
    normalized = normalize_linkage_config(config)
    settings.set("boss_autokey_linkage", normalized)
    settings.save()
    return normalized


def build_linkage_state(config: Dict[str, Any],
                        engine_status: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    return {
        "enabled": _bool(config.get("enabled"), False),
        "global_cooldown_s": config.get("global_cooldown_s", 1.0),
        "debug_log": _bool(config.get("debug_log"), False),
        "mappings": copy.deepcopy(config.get("mappings", [])),
        "runtime": copy.deepcopy(engine_status or {}),
    }


# ════════════════════════════════════════
#  Runtime Controller
# ════════════════════════════════════════

class BossAutoKeyLinkage:
    """Bridges boss raid events to auto-key actions.

    Wraps the on_alert callback of the boss raid engine to intercept
    phase/timeline events and fire mapped keystrokes.
    """

    def __init__(self, settings,
                 send_key: Optional[Callable[[str, str, int, int], None]] = None,
                 on_log: Optional[Callable[[str], None]] = None):
        """
        Args:
            settings: SettingsManager instance
            send_key: callable(key, press_mode, hold_ms, press_count) → fires a keystroke
            on_log: callable(message) → debug log output
        """
        self._settings = settings
        self._send_key = send_key
        self._on_log = on_log

        self._lock = threading.Lock()
        self._last_fire: Dict[str, float] = {}   # mapping_id → last fire time
        self._global_last_fire: float = 0.0
        self._fire_count: int = 0

    def on_boss_raid_alert(self, title: str, message: str):
        """Called when boss raid engine fires an alert (phase change, timeline, enrage, etc.).

        Determines if the alert matches any configured mapping and fires the action.
        """
        config = load_linkage_config(self._settings)
        if not _bool(config.get("enabled"), False):
            return

        trigger_type, trigger_label = self._classify_alert(title, message)
        if not trigger_type:
            return

        self._debug(config, f"[Linkage] Alert: type={trigger_type} label='{trigger_label}' msg='{message}'")

        with self._lock:
            now = time.time()
            global_cd = _float(config.get("global_cooldown_s"), 1.0)
            if global_cd > 0 and (now - self._global_last_fire) < global_cd:
                self._debug(config, f"[Linkage] Skipped: global cooldown ({global_cd}s)")
                return

            for mapping in config.get("mappings", []):
                if not _bool(mapping.get("enabled"), True):
                    continue
                if _s(mapping.get("trigger_type")) != trigger_type:
                    continue
                match_pattern = _s(mapping.get("trigger_match"))
                if match_pattern and match_pattern.lower() not in trigger_label.lower():
                    continue
                key = _s(mapping.get("action_key"))
                if not key:
                    continue

                # Cooldown check
                mid = _s(mapping.get("id"))
                per_cooldown = _float(mapping.get("cooldown_s"), 3.0)
                last = self._last_fire.get(mid, 0.0)
                if per_cooldown > 0 and (now - last) < per_cooldown:
                    self._debug(config, f"[Linkage] Skipped '{mapping.get('action_label', key)}': cooldown ({per_cooldown}s)")
                    continue

                # Fire
                self._last_fire[mid] = now
                self._global_last_fire = now
                self._fire_count += 1

                press_mode = _s(mapping.get("press_mode")) or "tap"
                hold_ms = _int(mapping.get("hold_ms"), 80)
                press_count = _int(mapping.get("press_count"), 1)
                label = _s(mapping.get("action_label")) or key

                self._debug(config, f"[Linkage] FIRE: '{label}' key={key} mode={press_mode} count={press_count}")

                if self._send_key:
                    threading.Thread(
                        target=self._send_key,
                        args=(key, press_mode, hold_ms, press_count),
                        daemon=True,
                    ).start()
                break  # Only fire first matching mapping per alert

    def _classify_alert(self, title: str, message: str):
        """Determine trigger_type and label from an alert."""
        msg = _s(message)
        if not msg:
            return None, ""

        # Phase enter: "→ P2" or "▶ Profile — START"
        if msg.startswith("→ "):
            return "phase_enter", msg[2:].strip()
        if msg.startswith("▶ "):
            return "phase_enter", msg[2:].strip()

        # Enrage
        if "ENRAGE" in msg.upper():
            return "enrage", msg

        # Completed
        if "COMPLETED" in msg.upper():
            return None, ""  # Don't trigger on completion

        # Timeline alert — anything else from "Boss Raid" title
        if _s(title).lower() in ("boss raid",):
            return "timeline_alert", msg

        return None, ""

    def get_status(self) -> Dict[str, Any]:
        with self._lock:
            return {
                "total_fires": self._fire_count,
                "last_global_fire": self._global_last_fire,
            }

    def reset(self):
        with self._lock:
            self._last_fire.clear()
            self._global_last_fire = 0.0
            self._fire_count = 0

    def _debug(self, config: Dict[str, Any], msg: str):
        if _bool(config.get("debug_log"), False) and self._on_log:
            try:
                self._on_log(msg)
            except Exception:
                pass


__all__ = [
    "BossAutoKeyLinkage",
    "build_linkage_state",
    "default_linkage_config",
    "load_linkage_config",
    "make_default_mapping",
    "normalize_linkage_config",
    "normalize_mapping",
    "save_linkage_config",
]
