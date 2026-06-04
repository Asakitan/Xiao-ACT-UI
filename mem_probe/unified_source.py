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
import threading
from typing import Any, Callable, Optional

from mem_probe.il2cpp.mem_state_bridge import MemStateBridge
from mem_probe.il2cpp.mem_self_state_provider import MemSelfStateProvider
from mem_probe import cy_memscan as _cy_memscan

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
        settings: Any = None,
        enable_extended: bool = True,
        **_: Any,
    ) -> None:
        self.state_mgr = state_mgr
        self.mode = str(mode or "auto").strip().lower() or "auto"
        self.settings = settings
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
        self._deferred = False
        self._defer_reason = ""
        self._trigger_count = 0
        self._last_trigger = ""
        self._last_trigger_context: dict[str, Any] = {}
        self._start_lock = threading.RLock()
        self._policy = self._build_policy()

        self._bridge = MemStateBridge(
            state_mgr=state_mgr,
            dps_tracker=dps_tracker,
            dps_overlay=dps_overlay,
            hp_overlay=hp_overlay,
            auto_key_engine=auto_key_engine,
            boss_raid_engine=boss_raid_engine,
            packet_bridge=packet_bridge,
            enable_extended=enable_extended,
            auto_scan_enabled=bool(self._policy["auto_scan_enabled"]),
            poll_interval=float(self._policy["auto_scan_interval_s"]),
            allow_static_fallback=bool(self._policy["allow_static_fallback"]),
            max_scan_regions_mb=int(self._policy["max_scan_regions_mb"]),
            on_log=self._on_bridge_log,
        )

    # ───────── public API expected by PacketBridge ─────────

    def start(self, *, defer: Optional[bool] = None) -> bool:
        """Start or arm the underlying read-only memory self-state bridge."""
        if self._started:
            return True
        if not self._policy["start_allowed"]:
            self._last_error = str(self._policy["fallback_reason"] or "memory policy denied startup")
            self._notify_status("error", self._last_error)
            return False
        defer_start = bool(self._policy.get("defer_until_tcp_scene", False) if defer is None else defer)
        if defer_start and self.mode != "memory":
            self._deferred = True
            self._defer_reason = "waiting_for_tcp_scene_or_full_sync"
            self._last_error = ""
            self._notify_status("deferred", "")
            return True
        return self.start_bridge(trigger="start", context={})

    def start_bridge(self, *, trigger: str = "", context: Optional[dict] = None) -> bool:
        """Idempotently start the heavy MemStateBridge after a TCP trigger."""
        with self._start_lock:
            if self._started:
                return True
            if not self._policy["start_allowed"]:
                self._last_error = str(self._policy["fallback_reason"] or "memory policy denied startup")
                self._notify_status("error", self._last_error)
                return False
            self._trigger_count += 1
            self._last_trigger = str(trigger or "manual")
            self._last_trigger_context = dict(context or {})
            self._deferred = False
            self._defer_reason = ""
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
        self._deferred = False
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
            "deferred": bool(self._deferred),
            "defer_reason": self._defer_reason,
            "trigger_count": int(self._trigger_count),
            "last_trigger": self._last_trigger,
            "last_trigger_context": dict(self._last_trigger_context),
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
            "policy": self._policy_health(),
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
        self._update_tcp_name_cache(payload)
        cb = self.on_self_update
        if callable(cb):
            try:
                cb(payload)
            except Exception:
                pass

    def _update_tcp_name_cache(self, payload: dict) -> None:
        bridge = self.packet_bridge
        cache = getattr(bridge, "tcp_name_cache", None) if bridge is not None else None
        if cache is None:
            return
        endpoint = ""
        try:
            endpoint_fn = getattr(bridge, "_tcp_endpoint", None)
            if callable(endpoint_fn):
                endpoint = str(endpoint_fn() or "")
        except Exception:
            endpoint = ""
        try:
            cache.apply_mem_self_snapshot(
                payload,
                endpoint=endpoint,
                context={
                    "source": "UnifiedDataSource",
                    "trigger": self._last_trigger,
                    "deferred_start": bool(self._last_trigger and self._last_trigger != "start"),
                },
            )
        except Exception:
            pass

    def _build_policy(self) -> dict:
        interval = self._float_setting(
            "mem_auto_scan_interval_s",
            getattr(MemSelfStateProvider, "POLL_INTERVAL", 0.5),
        )
        if interval <= 0:
            interval = getattr(MemSelfStateProvider, "POLL_INTERVAL", 0.5)
        interval = max(0.1, min(float(interval), 30.0))
        require_admin = self._bool_setting("mem_require_admin", False)
        admin_ok = (not require_admin) or self._is_windows_admin()
        auto_scan_enabled = self._bool_setting("mem_auto_scan_enabled", True)
        scan_safe_mode = self.mode in ("auto", "hybrid")
        allow_full_heap_scan = self._bool_setting("mem_allow_full_heap_scan", self.mode == "memory")
        default_scan_cap_mb = 1024 if scan_safe_mode else 0
        max_scan_regions_mb = self._int_setting("mem_max_scan_regions_mb", default_scan_cap_mb)
        if max_scan_regions_mb < 0:
            max_scan_regions_mb = 0
        if scan_safe_mode and not allow_full_heap_scan and max_scan_regions_mb <= 0:
            max_scan_regions_mb = default_scan_cap_mb
        allow_static_fallback = self._bool_setting("mem_allow_static_fallback", self.mode == "memory")
        show_risk_warning = self._bool_setting("mem_show_risk_warning", True)
        defer_until_tcp_scene = self._bool_setting("mem_defer_until_tcp_scene", self.mode in ("auto", "hybrid"))
        start_on_scene = self._bool_setting("mem_start_on_scene", False)
        start_on_full_sync = self._bool_setting("mem_start_on_full_sync", True)
        fallback_reason = ""
        if not auto_scan_enabled:
            fallback_reason = "memory auto scan disabled by mem_auto_scan_enabled"
        elif require_admin and not admin_ok:
            fallback_reason = "administrator privileges required by mem_require_admin"
        return {
            "auto_scan_enabled": bool(auto_scan_enabled),
            "auto_scan_interval_s": round(float(interval), 3),
            "require_admin": bool(require_admin),
            "admin_ok": bool(admin_ok),
            "max_scan_regions_mb": int(max_scan_regions_mb),
            "allow_full_heap_scan": bool(allow_full_heap_scan),
            "allow_static_fallback": bool(allow_static_fallback),
            "cy_memscan": _cy_memscan.backend_info(),
            "show_risk_warning": bool(show_risk_warning),
            "defer_until_tcp_scene": bool(defer_until_tcp_scene),
            "start_on_scene": bool(start_on_scene),
            "start_on_full_sync": bool(start_on_full_sync),
            "start_allowed": not bool(fallback_reason),
            "fallback_reason": fallback_reason,
        }

    def _policy_health(self) -> dict:
        out = dict(self._policy)
        try:
            bridge_policy = self._bridge.policy_status()
            if isinstance(bridge_policy, dict):
                out.update(bridge_policy)
        except Exception:
            pass
        return out

    def _setting_value(self, key: str, default: Any = None) -> Any:
        settings = self.settings
        if settings is None:
            return default
        getter = getattr(settings, "get", None)
        if callable(getter):
            try:
                return getter(key, default)
            except Exception:
                return default
        if isinstance(settings, dict):
            return settings.get(key, default)
        return default

    def _bool_setting(self, key: str, default: bool) -> bool:
        value = self._setting_value(key, default)
        if isinstance(value, bool):
            return value
        if isinstance(value, (int, float)):
            return bool(value)
        text = str(value).strip().lower()
        if text in ("1", "true", "yes", "on", "enabled"):
            return True
        if text in ("0", "false", "no", "off", "disabled"):
            return False
        return bool(default)

    def _float_setting(self, key: str, default: float) -> float:
        try:
            return float(self._setting_value(key, default))
        except Exception:
            return float(default)

    def _int_setting(self, key: str, default: int) -> int:
        try:
            return int(float(self._setting_value(key, default)))
        except Exception:
            return int(default)

    @staticmethod
    def _is_windows_admin() -> bool:
        try:
            import ctypes
            return bool(ctypes.windll.shell32.IsUserAnAdmin())
        except Exception:
            return False


__all__ = ["UnifiedDataSource"]
