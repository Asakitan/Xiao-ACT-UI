# -*- coding: utf-8 -*-
"""CS2 ESP plugin — read-only entity overlay.

This is a **read-only** plugin. It uses Engine A (``ENGINE_READONLY``)
through :mod:`mem_probe.rt_io`, reads the target process memory, builds
an overlay spec, and submits it via ``ctx.set_overlay``. It never calls
``write``, ``request_capability`` with write/input flags, or any input
control API.

Scope: authorized / offline / bot / self-hosted server testing only.
The plugin ships disabled by default (``plugin.json`` ``enabled:false``)
and refuses to start until ``offsets.json`` contains complete, real RVAs
for the target game version.

Lifecycle mirrors ``hide_seek_plugin/plugin.py``: a UI panel with
start/stop/reload/clear actions, a customizable toggle hotkey, a
throttled timer loop, and owner-agnostic notifications.
"""

from __future__ import annotations

import os
import time
from typing import Any, Optional

from .cs2_offsets import OffsetConfig, default_config, load_config
from .cs2_project import DEFAULT_VIEWPORT, build_overlay_spec, filter_enemies
from .cs2_reader import EngineAReader

PANEL_ID = "cs2_esp"
SURFACE = "cs2_esp"
ALERT_KIND = "cs2_esp"
DEFAULT_TICK_S = 0.05  # 20 Hz; conservative for read-only overlay
DEFAULT_HOTKEY = "CTRL+F7"

_ctx = None
_reader: Optional[EngineAReader] = None
_config: Optional[OffsetConfig] = None
_timer_token = ""
_running = False
_last_status: dict = {}
_last_snapshot: dict = {}
_last_spec: dict = {}
_last_tick_ts = 0.0


def on_load(ctx):
    global _ctx, _config
    _ctx = ctx
    _config = load_config(ctx.path) or default_config()
    _register_alert_kind()
    ctx.register_ui_panel(
        PANEL_ID,
        {"title": "CS2 ESP (Read-only)",
         "description": "Engine A 只读实体覆盖层。需先填入 offsets.json 真实 RVA。"},
        render=_render,
        on_action=_on_action,
    )
    ctx.register_hotkey("toggle", _toggle, default_key=DEFAULT_HOTKEY, label="CS2 ESP 开关")
    ctx.log(f"cs2_esp_plugin loaded (idle, config_version={_config.config_version!r})")


def on_disable():
    _stop("disabled")


def on_unload():
    _stop("unloaded")
    _unregister_alert_kind()


# ── controls ────────────────────────────────────────────────────────────
def _toggle():
    if _running():
        _stop("hotkey")
    else:
        _start()


def _start() -> bool:
    global _reader, _timer_token, _running
    if _ctx is None:
        return False
    if not _ensure_config_ok():
        return False
    use_scan = _bool_setting("use_pattern_scan", True)
    _reader = EngineAReader(_config, use_pattern_scan=use_scan)
    if not _reader.ensure_engine():
        _fail(f"引擎 A 加载失败: {_reader._last_error or 'unknown'}")
        return False
    pid = _reader.find_process()
    if pid <= 0:
        _fail(f"未找到进程 {_config.target_process!r}")
        return False
    if not _reader.attach(pid):
        _fail(f"attach 失败 (pid={pid}): {_reader._last_error or 'unknown'}")
        return False
    _running = True
    tick_s = float(_ctx.get_setting("tick_s", DEFAULT_TICK_S) or DEFAULT_TICK_S)
    tick_s = max(0.02, min(1.0, tick_s))
    _timer_token = _ctx.set_interval(_tick, tick_s)
    _ctx.log(f"cs2_esp started (pid={pid}, tick={tick_s:.3f}s)")
    _ctx.notify("CS2 ESP", f"已启动 (pid={pid})", duration_s=4.0, kind=ALERT_KIND)
    _refresh_panel()
    return True


def _stop(reason: str = "manual") -> None:
    global _reader, _timer_token, _running
    if _ctx is not None and _timer_token:
        try:
            _ctx.clear_timer(_timer_token)
        except Exception:
            pass
    _timer_token = ""
    if _reader is not None:
        _reader.detach()
    _running = False
    if _ctx is not None:
        try:
            _ctx.clear_overlay(SURFACE)
        except Exception:
            pass
        _ctx.log(f"cs2_esp stopped ({reason})")
        _ctx.notify("CS2 ESP", f"已停止 ({reason})", duration_s=3.0, kind=ALERT_KIND)
    _reader = None
    _refresh_panel()


def _reload_config() -> bool:
    global _config
    if _ctx is None:
        return False
    was_running = _running
    if was_running:
        _stop("reload")
    _config = load_config(_ctx.path) or default_config()
    _ctx.log(f"cs2_esp config reloaded (version={_config.config_version!r})")
    _refresh_panel()
    if was_running and _ensure_config_ok():
        _start()
    return True


def _clear_overlay() -> None:
    if _ctx is None:
        return
    try:
        _ctx.clear_overlay(SURFACE)
    except Exception:
        pass
    _refresh_panel()


def _ensure_config_ok() -> bool:
    if _config is None or not _config.is_complete():
        miss = _config.missing() if _config else []
        _fail(f"offsets.json 不完整: {', '.join(miss) or '未知字段'}; 请填入真实 RVA")
        return False
    return True


def _fail(message: str) -> None:
    global _last_status
    _last_status = {**_last_status, "last_error": message}
    if _ctx is not None:
        _ctx.log(f"cs2_esp: {message}")
        _ctx.notify("CS2 ESP 错误", message, duration_s=6.0, kind=ALERT_KIND)
        _refresh_panel()


def _running() -> bool:
    return bool(_running)


# ── tick ────────────────────────────────────────────────────────────────
def _tick() -> None:
    global _last_status, _last_snapshot, _last_spec, _last_tick_ts
    if _ctx is None or not _running or _reader is None:
        return
    if not _reader.attached:
        _stop("detached")
        return
    started = time.perf_counter()
    snap = _reader.snapshot()
    _last_tick_ts = time.time()
    _last_snapshot = snap
    if snap.get("error"):
        # Fail-closed: clear overlay, keep trying next tick unless attach lost.
        try:
            _ctx.clear_overlay(SURFACE)
        except Exception:
            pass
        _last_status = {**_reader.status(), "last_error": snap["error"],
                        "last_tick_ms": int((time.perf_counter() - started) * 1000)}
        _refresh_panel()
        return
    matrix = snap.get("matrix")
    entities = snap.get("entities") or []
    viewport = _viewport()
    enemies = filter_enemies(entities, _local_team_or_default())
    spec = build_overlay_spec(enemies, matrix, viewport,
                              show_health=_bool_setting("show_health", True))
    # Dirty-signature: skip overlay update when entity count + matrix hash match.
    sig = (len(enemies), _matrix_sig(matrix), viewport)
    if _last_spec.get("_sig") != sig:
        spec["_sig"] = sig
        _last_spec = spec
        try:
            _ctx.set_overlay(SURFACE, spec)
        except Exception as exc:
            _last_status = {**_reader.status(), "last_error": f"set_overlay: {exc}"}
            _refresh_panel()
            return
    _last_status = {**_reader.status(),
                    "entity_count": len(enemies),
                    "last_tick_ms": int((time.perf_counter() - started) * 1000),
                    "last_error": ""}
    _refresh_panel()


def _viewport() -> tuple:
    if _ctx is None:
        return DEFAULT_VIEWPORT
    w = int(_ctx.get_setting("viewport_w", DEFAULT_VIEWPORT[0]) or DEFAULT_VIEWPORT[0])
    h = int(_ctx.get_setting("viewport_h", DEFAULT_VIEWPORT[1]) or DEFAULT_VIEWPORT[1])
    return (max(1, w), max(1, h))


def _local_team_or_default() -> int:
    if _ctx is None:
        return 0
    try:
        return int(_ctx.get_setting("local_team", 0) or 0)
    except (TypeError, ValueError):
        return 0


def _bool_setting(key: str, default: bool) -> bool:
    if _ctx is None:
        return default
    val = _ctx.get_setting(key, default)
    if isinstance(val, bool):
        return val
    if isinstance(val, str):
        return val.strip().lower() in ("1", "true", "yes", "on")
    return bool(val)


def _matrix_sig(matrix) -> tuple:
    if not matrix:
        return ()
    try:
        return tuple(round(float(v), 3) for v in matrix)
    except Exception:
        return (len(matrix) if matrix else 0,)


# ── panel ───────────────────────────────────────────────────────────────
def _render(_payload=None) -> dict:
    ui = _ctx.ui if _ctx is not None else None
    if ui is None:
        return {"version": 1, "title": "CS2 ESP", "nodes": []}
    cfg = _config or default_config()
    st = _last_status or {}
    snap = _last_snapshot or {}
    running = _running
    complete = cfg.is_complete()
    status_text = "运行中" if running else ("就绪" if complete else "配置不完整")
    scanned = getattr(_reader, "scanned_rvas", lambda: {})() if _reader is not None else {}
    scan_state = "已扫描" if scanned else ("未启用" if not _bool_setting("use_pattern_scan", True) else "待扫描")
    nodes = [
        ui.section("状态", [
            ui.kv("状态", status_text),
            ui.kv("引擎", "A (ENGINE_READONLY)"),
            ui.kv("实际 tier", str(st.get("tier") or "-")),
            ui.kv("目标进程", cfg.target_process),
            ui.kv("模块", cfg.module),
            ui.kv("PID", str(st.get("attached_pid") or "-")),
            ui.kv("模块基址", hex(st.get("module_base") or 0)),
            ui.kv("实体数", str(st.get("entity_count") or len(snap.get("entities") or []))),
            ui.kv("最近 tick", str(st.get("last_tick_ms") or "-") + " ms"),
            ui.kv("Pattern 扫描", scan_state),
            ui.kv("扫描到的 RVA", ", ".join(f"{k}={hex(v)}" for k, v in scanned.items()) or "无"),
            ui.kv("最近错误", str(st.get("last_error") or "")),
            ui.kv("配置版本", str(cfg.config_version or "")),
            ui.kv("缺字段", ", ".join(cfg.missing()) if not complete else "无"),
        ]),
        ui.section("控制", [
            ui.row([
                ui.button("启动", "start", style="primary" if not running else "default",
                          disabled=running),
                ui.button("停止", "stop", style="danger" if running else "default",
                          disabled=not running),
                ui.button("重载配置", "reload"),
                ui.button("清除覆盖层", "clear"),
            ]),
        ]),
    ]
    return ui.panel("CS2 ESP (Read-only)", nodes)


def _on_action(action_id: str, _payload=None) -> Any:
    action_id = str(action_id or "")
    if action_id == "start":
        _start()
    elif action_id == "stop":
        _stop("panel")
    elif action_id == "reload":
        _reload_config()
    elif action_id == "clear":
        _clear_overlay()
    return {"ok": True}


def _refresh_panel() -> None:
    if _ctx is None:
        return
    try:
        _ctx.request_redraw(PANEL_ID)
    except Exception:
        pass


# ── alert kind registration (mirrors hide_seek_plugin) ──────────────────
def _register_alert_kind() -> None:
    owner = getattr(_ctx, "owner", None) if _ctx is not None else None
    fn = getattr(owner, "_register_persistent_alert_kind", None)
    if callable(fn):
        try:
            fn(ALERT_KIND)
        except Exception:
            pass


def _unregister_alert_kind() -> None:
    owner = getattr(_ctx, "owner", None) if _ctx is not None else None
    fn = getattr(owner, "_unregister_persistent_alert_kind", None)
    if callable(fn):
        try:
            fn(ALERT_KIND)
        except Exception:
            pass
