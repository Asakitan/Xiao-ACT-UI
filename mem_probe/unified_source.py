# -*- coding: utf-8 -*-
"""Unified memory/TCP data source entry point.

This module is intentionally conservative. It provides the missing
`UnifiedDataSource` surface expected by `net.packet_bridge.PacketBridge`,
while delegating read-only memory self-state work to `MemStateBridge`.

Current scope:
- self player identity / HP / stamina / skill CD / resources via memory-first
  `MemStateBridge` with its built-in fallback behavior.
- boss, monster, scene, and damage streams remain PacketBridge/TCP-owned until
  their memory readers are proven live-safe.

The class does not create another PacketBridge, so hybrid/auto startup cannot
recurse.
"""

from __future__ import annotations

import time
from typing import Any, Callable, Optional

from mem_probe.il2cpp.mem_state_bridge import MemStateBridge

StatusCallback = Callable[[str, str], None]
SelfCallback = Callable[[dict], None]


class UnifiedDataSource:
    """Small fail-safe adapter used by PacketBridge memory/hybrid/auto modes.

    Parameters mirror the older PacketBridge expectation so this can be used as
    a drop-in data source. Unsupported callback streams are retained for future
    expansion and reported as TCP-owned in `health()`.
    """

    def __init__(
        self,
        state_mgr: Any,
        mode: str = "auto",
        on_self_update: Optional[SelfCallback] = None,
        on_damage: Optional[Callable[..., None]] = None,
        on_monster_update: Optional[Callable[..., None]] = None,
        on_boss_event: Optional[Callable[..., None]] = None,
        on_scene_change: Optional[Callable[..., None]] = None,
        on_status_change: Optional[StatusCallback] = None,
        packet_bridge: Any = None,
        dps_tracker: Any = None,
        dps_overlay: Any = None,
        hp_overlay: Any = None,
        auto_key_engine: Any = None,
        boss_raid_engine: Any = None,
        enable_extended: bool = True,
        **_: Any,
    ) -> None:
        self.state_mgr = state_mgr
        self.mode = str(mode or "auto").strip().lower() or "auto"
        self.packet_bridge = packet_bridge
        self.on_self_update = on_self_update
        self.on_damage = on_damage
        self.on_monster_update = on_monster_update
        self.on_boss_event = on_boss_event
        self.on_scene_change = on_scene_change
        self.on_status_change = on_status_change
        self._started = False
        self._started_at = 0.0
        self._last_error = ""
        self._last_status = "init"
        self._last_snapshot_sig = None

        self._bridge = MemStateBridge(
            state_mgr=state_mgr,
            dps_tracker=dps_tracker,
            dps_overlay=dps_overlay,
            hp_overlay=hp_overlay,
            auto_key_engine=auto_key_engine,
            boss_raid_engine=boss_raid_engine,
            packet_bridge=packet_bridge,
            enable_extended=enable_extended,
            on_log=self._on_bridge_log,
        )

    # ───────── public API expected by PacketBridge ─────────

    def start(self) -> bool:
        """Start the underlying read-only memory self-state bridge."""
        if self._started:
            return True
        self._notify_status("starting", "")
        try:
            ok = bool(self._bridge.start())
        except Exception as exc:
            self._last_error = str(exc)
            self._notify_status("error", self._last_error)
            return False
        if not ok:
            self._last_error = getattr(self._bridge, "last_error", "") or "MemStateBridge failed to start"
            self._notify_status("error", self._last_error)
            return False
        self._started = True
        self._started_at = time.time()
        self._last_error = ""
        self._notify_status("running", "")
        self._emit_self_update_if_changed()
        return True

    def stop(self) -> None:
        """Stop the underlying bridge. Never raises."""
        try:
            self._bridge.stop()
        except Exception as exc:
            self._last_error = str(exc)
        self._started = False
        self._notify_status("stopped", self._last_error)

    def health(self) -> dict:
        """Return a JSON-serializable health snapshot."""
        mode = str(getattr(self._bridge, "mode", "") or self._last_status or "init")
        err = str(getattr(self._bridge, "last_error", "") or self._last_error or "")
        snap = self._safe_snapshot_dict()
        alive = bool(self._started and getattr(self._bridge, "_provider", None) is not None)
        return {
            "data_source": "unified",
            "requested_mode": self.mode,
            "mode": mode,
            "running": bool(self._started),
            "started": bool(self._started),
            "alive": alive,
            "is_memory_active": bool(getattr(self._bridge, "is_memory_active", False)),
            "status": self._last_status,
            "last_error": err,
            "started_at": self._started_at,
            "uptime_s": round(max(0.0, time.time() - self._started_at), 3) if self._started_at else 0.0,
            "watchers": {
                "self": "memory_first",
                "combat": "tcp_fallback",
                "entity": "tcp_fallback",
                "boss": "tcp_fallback",
                "scene": "tcp_fallback",
            },
            "self": {
                "uid": int(getattr(self._bridge, "last_uid", 0) or 0),
                "hp": int(getattr(self._bridge, "last_hp", 0) or 0),
                "max_hp": int(getattr(self._bridge, "last_max_hp", 0) or 0),
                "profession_id": int(getattr(self._bridge, "last_profession_id", 0) or 0),
                "name": str(getattr(self._bridge, "last_char_name", "") or ""),
                "skill_cd_count": int(getattr(self._bridge, "last_skill_cd_count", 0) or 0),
                "is_dead": bool(getattr(self._bridge, "last_is_dead", False)),
            },
            "snapshot_available": bool(snap),
        }

    # ───────── internals ─────────

    def _on_bridge_log(self, message: str) -> None:
        msg = str(message or "")
        if "err=" in msg or "failed" in msg.lower():
            self._last_error = msg
        if "mode='memory'" in msg or 'mode="memory"' in msg:
            self._notify_status("running", "")
        self._emit_self_update_if_changed()

    def _notify_status(self, status: str, error: str = "") -> None:
        self._last_status = str(status or "")
        self._last_error = str(error or "")
        cb = self.on_status_change
        if callable(cb):
            try:
                cb(self._last_status, self._last_error)
            except Exception:
                pass

    def _safe_snapshot_dict(self) -> dict:
        try:
            snap = self._bridge.snapshot()
        except Exception:
            return {}
        if snap is None:
            return {}
        if isinstance(snap, dict):
            return dict(snap)
        data = {}
        for key in (
            "uid", "char_id", "player_id", "cur_hp", "hp", "max_hp",
            "profession_id", "char_name", "name", "skill_cds", "resources",
            "is_dead", "stamina", "stamina_max",
        ):
            try:
                if hasattr(snap, key):
                    value = getattr(snap, key)
                    if isinstance(value, (str, int, float, bool, type(None), list, tuple, dict)):
                        data[key] = value
                    else:
                        data[key] = str(value)
            except Exception:
                pass
        return data

    def _emit_self_update_if_changed(self) -> None:
        cb = self.on_self_update
        if not callable(cb):
            return
        payload = self._safe_snapshot_dict()
        if not payload:
            payload = {
                "uid": int(getattr(self._bridge, "last_uid", 0) or 0),
                "hp": int(getattr(self._bridge, "last_hp", 0) or 0),
                "max_hp": int(getattr(self._bridge, "last_max_hp", 0) or 0),
                "profession_id": int(getattr(self._bridge, "last_profession_id", 0) or 0),
                "name": str(getattr(self._bridge, "last_char_name", "") or ""),
            }
        sig = repr(sorted((str(k), repr(v)) for k, v in payload.items()))
        if sig == self._last_snapshot_sig:
            return
        self._last_snapshot_sig = sig
        try:
            cb(payload)
        except Exception:
            pass


__all__ = ["UnifiedDataSource"]
