# -*- coding: utf-8 -*-
"""CS2 combat plugin — ESP + Aimbot + RCS + Triggerbot + Visibility.

Uses Engine A (read-only) for all memory reads. Input simulation
prefers Engine E kernel-mode HID injection, falls back to SendInput.

Feature toggles in plugin settings; all features default off except ESP.
"""

from __future__ import annotations

import random
import time
from typing import Any, Optional

from .cs2_aimbot import AimbotState
from .cs2_input import (
    click_with_delay, input_mode, key_tap, move_mouse,
)
from .cs2_local import HitStats
from .cs2_offsets import OffsetConfig, default_config, load_config
from .cs2_project import DEFAULT_VIEWPORT, build_overlay_spec, filter_enemies
from .cs2_reader import EngineAReader
from .cs2_rcs import AutoStopState, RCSState
from .cs2_triggerbot import TriggerbotState

PANEL_ID = "cs2_combat"
SURFACE = "cs2_esp"
ALERT_KIND = "cs2_esp"
DEFAULT_TICK_S = 0.008  # 125 Hz for aimbot responsiveness
DEFAULT_HOTKEY = "CTRL+F7"

_ctx = None
_reader: Optional[EngineAReader] = None
_config: Optional[OffsetConfig] = None
_timer_token = ""
_running = False
_last_status: dict = {}
_last_snapshot: dict = {}
_last_spec: dict = {}

# Feature states
_aimbot = AimbotState()
_rcs = RCSState()
_autostop = AutoStopState()
_triggerbot = TriggerbotState()
_hitstats = HitStats()


def on_load(ctx):
    global _ctx, _config
    _ctx = ctx
    _config = load_config(ctx.path) or default_config()
    _register_alert_kind()
    ctx.register_ui_panel(
        PANEL_ID,
        {"title": "CS2 Combat",
         "description": "ESP + Aimbot + RCS + Triggerbot (Engine A 读 / Engine E 输入)"},
        render=_render,
        on_action=_on_action,
    )
    ctx.register_hotkey("toggle", _toggle, default_key=DEFAULT_HOTKEY, label="CS2 开关")
    ctx.log(f"cs2_combat loaded (idle, v={_config.config_version!r})")


def on_disable():
    _stop("disabled")


def on_unload():
    _stop("unloaded")
    _unregister_alert_kind()


# ── controls ────────────────────────────────────────────────────────────

def _toggle():
    if _running:
        _stop("hotkey")
    else:
        _start()


def _start() -> bool:
    global _reader, _timer_token, _running
    if _ctx is None or _running:
        return _running
    if not _ensure_config_ok():
        return False
    use_scan = _bool("use_pattern_scan", True)
    _reader = EngineAReader(_config, use_pattern_scan=use_scan)
    if not _reader.ensure_engine():
        _fail(f"引擎 A 加载失败: {_reader._last_error or '?'}")
        _reader = None
        return False
    pid = _reader.find_process()
    if pid <= 0:
        _fail(f"未找到进程 {_config.target_process!r}")
        _reader = None
        return False
    if not _reader.attach(pid):
        _fail(f"attach 失败 (pid={pid}): {_reader._last_error or '?'}")
        _reader = None
        return False

    _sync_features()
    if input_mode() == "unavailable":
        _ctx.log("cs2: kernel input unavailable, combat features disabled")
        _aimbot.enabled = False
        _rcs.enabled = False
        _triggerbot.enabled = False
    _running = True
    base_tick = max(0.004, min(1.0, float(_ctx.get_setting("tick_s", DEFAULT_TICK_S) or DEFAULT_TICK_S)))
    _timer_token = _ctx.set_interval(_tick, base_tick)
    _ctx.log(f"cs2_combat started (pid={pid}, tick={base_tick*1000:.0f}ms, input={input_mode()})")
    _ctx.notify("CS2", f"已启动 (pid={pid})", duration_s=4.0, kind=ALERT_KIND)
    _refresh()
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
    _aimbot.reset()
    _rcs.reset()
    _triggerbot.reset()
    if _ctx is not None:
        try:
            _ctx.clear_overlay(SURFACE)
        except Exception:
            pass
        _ctx.log(f"cs2_combat stopped ({reason})")
    _reader = None
    _refresh()


def _reload():
    global _config
    if _ctx is None:
        return
    was = _running
    if was:
        _stop("reload")
    _config = load_config(_ctx.path) or default_config()
    _ctx.log(f"cs2 config reloaded (v={_config.config_version!r})")
    _refresh()
    if was and _ensure_config_ok():
        _start()


def _sync_features():
    """Read feature toggles from plugin settings into state objects."""
    sens = _float("sensitivity", 1.0)

    # Aimbot
    _aimbot.enabled = _bool("aimbot_enabled", False)
    _aimbot.fov = _float("aimbot_fov", 5.0)
    _aimbot.smooth = _float("aimbot_smooth", 0.25)
    _aimbot.speed_scale = _float("aimbot_speed", 1.0)
    _aimbot.use_bezier = _bool("aimbot_bezier", True)
    _aimbot.bezier_pull = _float("aimbot_bezier_pull", 0.35)
    _aimbot.jitter = _float("aimbot_jitter", 0.08)
    _aimbot.random_offset = _float("aimbot_offset", 0.0)
    _aimbot.reaction_ms = _int("aimbot_reaction_ms", 0)
    _aimbot.hit_rate = _float("aimbot_hit_rate", 1.0)
    _aimbot.miss_jitter = _float("aimbot_miss_jitter", 0.5)
    _aimbot.bone = _str("aimbot_bone", "head")
    _aimbot.max_distance = _float("aimbot_max_distance", 0)
    _aimbot.visible_only = _bool("aimbot_visible_only", True)
    _aimbot.sensitivity = sens

    # Aimbot (new fields)
    _aimbot.overshoot = _float("aimbot_overshoot", 0.0)
    _aimbot.curve = _str("aimbot_curve", "ease_out")

    # RCS
    _rcs.enabled = _bool("rcs_enabled", False)
    _rcs.strength = _float("rcs_strength", 1.0)
    _rcs.h_strength = _float("rcs_h_strength", 1.0)
    _rcs.start_bullet = _int("rcs_start_bullet", 1)
    _rcs.max_bullets = _int("rcs_max_bullets", 0)
    _rcs.ema_alpha = _float("rcs_ema_alpha", 0.5)
    _rcs.ramp_bullets = _int("rcs_ramp_bullets", 4)
    _rcs.hit_rate = _float("rcs_hit_rate", 1.0)
    _rcs.jitter = _float("rcs_jitter", 0.0)
    _rcs.sensitivity = sens

    # Auto-stop
    _autostop.enabled = _bool("autostop_enabled", False)
    _autostop.speed_threshold = _float("autostop_threshold", 10.0)

    # Triggerbot
    _triggerbot.enabled = _bool("triggerbot_enabled", False)
    _triggerbot.seed_mode = _bool("triggerbot_seed", False)
    _triggerbot.delay_min_ms = _int("triggerbot_delay_min", 50)
    _triggerbot.delay_max_ms = _int("triggerbot_delay_max", 150)
    _triggerbot.cooldown_ms = _int("triggerbot_cooldown", 60)
    _triggerbot.hitchance_min = _float("triggerbot_hitchance", 0.65)
    _triggerbot.spread_deg = _float("triggerbot_spread", 0.8)
    _triggerbot.hitbox_radius = _float("triggerbot_hitbox_r", 3.6)
    _triggerbot.hit_rate = _float("triggerbot_hit_rate", 1.0)
    _triggerbot.burst_max = _int("triggerbot_burst_max", 0)
    _triggerbot.burst_cooldown_ms = _int("triggerbot_burst_cooldown", 500)
    _triggerbot.visible_only = _bool("triggerbot_visible_only", True)


# ── tick ────────────────────────────────────────────────────────────────

def _tick() -> None:
    global _last_status, _last_snapshot, _last_spec
    if _ctx is None or not _running or _reader is None:
        return
    if not _reader.attached:
        _stop("detached")
        return

    # Skip ~5% of ticks randomly to break perfect periodicity
    if random.random() < 0.05:
        return

    _sync_features()
    started = time.perf_counter()
    snap = _reader.snapshot()
    _last_snapshot = snap

    if snap.get("error"):
        try:
            _ctx.clear_overlay(SURFACE)
        except Exception:
            pass
        _last_status = {**_reader.status(), "last_error": snap["error"],
                        "tick_ms": int((time.perf_counter() - started) * 1000)}
        _refresh()
        return

    matrix = snap.get("matrix")
    entities = snap.get("entities") or []
    local = snap.get("local_player")
    local_team = local.get("team", 0) if local else 0
    # Randomise entity read order to avoid deterministic access cadence
    if entities:
        random.shuffle(entities)
    enemies = filter_enemies(entities, local_team)

    # Build enemy index for triggerbot
    enemy_map = {e["index"]: e for e in enemies}

    # FOV scale from local player
    fov_scale = 1.0
    if local:
        fov_scale = local.get("fov_scale", 1.0) or 1.0
        _aimbot.fov_scale = fov_scale
        _rcs.fov_scale = fov_scale

    # Hit stats
    if local:
        _hitstats.update(local.get("shots_fired", 0), enemies)

    # --- Aimbot ---
    aim_delta = _aimbot.tick(local, enemies)

    # --- Auto-stop (before aimbot move) ---
    if _autostop.enabled and _aimbot.has_target and local:
        if _autostop.should_stop(local):
            keys = _autostop.get_counter_keys(local)
            for vk in keys:
                key_tap(vk, _autostop.tap_ms)

    if aim_delta:
        move_mouse(aim_delta[0], aim_delta[1])

    # --- RCS ---
    rcs_delta = _rcs.tick(local)
    if rcs_delta and not aim_delta:
        move_mouse(rcs_delta[0], rcs_delta[1])

    # --- Triggerbot ---
    trig = _triggerbot.tick(local, enemy_map, all_enemies=enemies)
    if trig == "fire":
        click_with_delay(
            _triggerbot.delay_min_ms,
            _triggerbot.delay_max_ms,
        )

    # --- ESP overlay ---
    viewport = _viewport()
    local_origin = local.get("origin") if local else None
    spec = build_overlay_spec(
        enemies, matrix, viewport,
        local_origin=local_origin,
        show_health=_bool("show_health", True),
        show_skeleton=_bool("show_skeleton", True),
        show_distance=_bool("show_distance", True),
        show_snapline=_bool("show_snapline", False),
        show_head=_bool("show_head", True),
        show_box=_bool("show_box", True),
        show_hp_bar=_bool("show_hp_bar", True),
        show_fov_circle=_bool("show_fov_circle", False) and _aimbot.enabled,
        fov_radius=_aimbot.fov,
    )
    try:
        _ctx.set_overlay(SURFACE, spec)
    except Exception:
        try:
            _ctx.clear_overlay(SURFACE)
        except Exception:
            pass

    _last_status = {
        **_reader.status(),
        "entity_count": len(enemies),
        "tick_ms": int((time.perf_counter() - started) * 1000),
        "last_error": "",
        "aim_target": _aimbot.has_target,
        "rcs_active": _rcs._active,
        "triggerbot_pending": _triggerbot._pending_fire,
        "input_mode": input_mode(),
        "accuracy": _hitstats.display,
    }
    _refresh()


# ── panel ───────────────────────────────────────────────────────────────

def _render(_payload=None) -> dict:
    ui = _ctx.ui if _ctx is not None else None
    if ui is None:
        return {"version": 1, "title": "CS2 Combat", "nodes": []}
    cfg = _config or default_config()
    st = _last_status or {}
    running = _running
    complete = cfg.is_complete()
    combat = cfg.combat_ready()
    err = str(st.get("last_error") or "")

    # Status badge
    if running:
        s_badge = ui.badge("LIVE", "ok")
    elif complete:
        s_badge = ui.badge("READY", "accent")
    else:
        s_badge = ui.badge("INCOMPLETE", "bad")

    # Accuracy bar
    acc = _hitstats.accuracy
    acc_color = "ok" if acc >= 0.5 else ("gold" if acc >= 0.25 else "bad")

    nodes = [
        # ── Header ──
        ui.row([s_badge,
                ui.badge(f"Tick {st.get('tick_ms', '-')}ms", "muted"),
                ui.badge(f"×{st.get('entity_count', 0)}", "muted"),
                ui.badge(input_mode().upper(), "accent")]),
        ui.spacer(4),

        # ── Accuracy ──
        ui.bar("命中率", pct=acc, color=acc_color, caption=_hitstats.display),
        ui.spacer(4),
    ]

    if err:
        nodes.append(ui.text(err, style="bad"))
        nodes.append(ui.spacer(4))

    if not combat and complete:
        nodes.append(ui.text(f"战斗功能缺: {', '.join(cfg.combat_missing())}", style="warn"))
        nodes.append(ui.spacer(4))

    # ── ESP ──
    esp_items = []
    for key, label in [("show_skeleton", "骨骼"), ("show_hp_bar", "血条"),
                        ("show_distance", "距离"), ("show_head", "头点"),
                        ("show_snapline", "拉线"), ("show_box", "框体")]:
        style = "ok" if _bool(key, True if key != "show_snapline" else False) else "muted"
        esp_items.append(ui.badge(label, style))
    nodes.append(ui.section("ESP", [ui.row(esp_items)]))

    # ── Aimbot ──
    aim_on = _bool("aimbot_enabled", False)
    aim_items = [
        ui.row([ui.badge("ON" if aim_on else "OFF", "ok" if aim_on else "muted"),
                ui.badge("LOCKED" if st.get("aim_target") else "---",
                         "gold" if st.get("aim_target") else "muted")]),
        ui.kv("FOV", f"{_aimbot.fov:.1f}°"),
        ui.kv("速度", f"smooth={_aimbot.smooth:.2f}  scale={_aimbot.speed_scale:.1f}x"),
        ui.kv("曲线", f"{'Overshoot' if _aimbot.overshoot > 0 else 'EaseOut'}  "
              f"overshoot={_aimbot.overshoot:.2f}"),
        ui.kv("人性化", f"jitter={_aimbot.jitter:.2f}°  "
              f"offset={_aimbot.random_offset:.2f}°  "
              f"react={_aimbot.reaction_ms}ms"),
        ui.kv("命中率", f"{_aimbot.hit_rate:.0%}"),
        ui.kv("目标", f"{_aimbot.bone}  {'仅可见' if _aimbot.visible_only else '全部'}"),
    ]
    nodes.append(ui.section("自瞄", aim_items))

    # ── RCS ──
    rcs_on = _bool("rcs_enabled", False)
    rcs_items = [
        ui.row([ui.badge("ON" if rcs_on else "OFF", "ok" if rcs_on else "muted"),
                ui.badge("补偿中" if st.get("rcs_active") else "待机",
                         "gold" if st.get("rcs_active") else "muted")]),
        ui.kv("强度", f"V={_rcs.strength:.0%}  H={_rcs.h_strength:.0%}"),
        ui.kv("弹药", f"第{_rcs.start_bullet + 1}发起  "
              f"{'共' + str(_rcs.max_bullets) + '发' if _rcs.max_bullets else '不限'}"),
        ui.kv("平滑", f"EMA α={_rcs.ema_alpha:.2f}  ramp={_rcs.ramp_bullets}发"),
        ui.kv("命中率", f"{_rcs.hit_rate:.0%}  jitter={_rcs.jitter:.2f}°"),
    ]
    nodes.append(ui.section("压枪", rcs_items))

    # ── Triggerbot ──
    trig_on = _bool("triggerbot_enabled", False)
    mode_label = "Hitchance" if _triggerbot.seed_mode else "经典"
    trig_items = [
        ui.row([ui.badge("ON" if trig_on else "OFF", "ok" if trig_on else "muted"),
                ui.badge(mode_label, "accent"),
                ui.badge("FIRE" if st.get("triggerbot_pending") else "---",
                         "bad" if st.get("triggerbot_pending") else "muted")]),
        ui.kv("延迟", f"{_triggerbot.delay_min_ms}–{_triggerbot.delay_max_ms}ms  "
              f"CD={_triggerbot.cooldown_ms}ms"),
        ui.kv("命中率", f"{_triggerbot.hit_rate:.0%}"),
    ]
    if _triggerbot.seed_mode:
        trig_items.extend([
            ui.kv("Hitchance", f"≥{_triggerbot.hitchance_min:.0%}"),
            ui.kv("散布锥", f"{_triggerbot.spread_deg:.2f}°  "
                  f"hitbox={_triggerbot.hitbox_radius:.1f}u"),
        ])
    if _triggerbot.burst_max > 0:
        trig_items.append(ui.kv("连发", f"{_triggerbot.burst_max}发/"
                                f"{_triggerbot.burst_cooldown_ms}ms CD"))
    nodes.append(ui.section("板机", trig_items))

    # ── Auto-stop ──
    as_on = _bool("autostop_enabled", False)
    nodes.append(ui.section("急停", [
        ui.row([ui.badge("ON" if as_on else "OFF", "ok" if as_on else "muted")]),
        ui.kv("阈值", f"{_autostop.speed_threshold:.0f} u/s"),
    ]))

    # ── Controls ──
    nodes.append(ui.divider())
    nodes.append(ui.row([
        ui.button("启动" if not running else "停止",
                  "start" if not running else "stop",
                  style="primary" if not running else "danger"),
        ui.button("重载", "reload"),
        ui.button("清除覆盖", "clear"),
        ui.button("清零统计", "reset_stats"),
    ]))

    return ui.panel("CS2 Combat", nodes)


def _on_action(action_id: str, _payload=None) -> Any:
    if action_id == "start":
        _start()
    elif action_id == "stop":
        _stop("panel")
    elif action_id == "reload":
        _reload()
    elif action_id == "clear":
        if _ctx:
            try:
                _ctx.clear_overlay(SURFACE)
            except Exception:
                pass
    elif action_id == "reset_stats":
        _hitstats.reset()
    _refresh()
    return {"ok": True}


# ── helpers ─────────────────────────────────────────────────────────────

def _ensure_config_ok() -> bool:
    if _config is None or not _config.is_complete():
        miss = _config.missing() if _config else []
        _fail(f"offsets.json 不完整: {', '.join(miss)}; 请填入真实 RVA")
        return False
    return True


def _fail(msg: str):
    global _last_status
    _last_status = {**_last_status, "last_error": msg}
    if _ctx:
        _ctx.log(f"cs2: {msg}")
        _ctx.notify("CS2 错误", msg, duration_s=6.0, kind=ALERT_KIND)
    _refresh()


def _refresh():
    if _ctx:
        try:
            _ctx.request_redraw(PANEL_ID)
        except Exception:
            pass


def _viewport() -> tuple:
    if _ctx is None:
        return DEFAULT_VIEWPORT
    w = int(_ctx.get_setting("viewport_w", DEFAULT_VIEWPORT[0]) or DEFAULT_VIEWPORT[0])
    h = int(_ctx.get_setting("viewport_h", DEFAULT_VIEWPORT[1]) or DEFAULT_VIEWPORT[1])
    return (max(1, w), max(1, h))


def _bool(key: str, default: bool) -> bool:
    if _ctx is None:
        return default
    val = _ctx.get_setting(key, default)
    if isinstance(val, bool):
        return val
    if isinstance(val, str):
        return val.strip().lower() in ("1", "true", "yes", "on")
    return bool(val)


def _float(key: str, default: float) -> float:
    if _ctx is None:
        return default
    try:
        return float(_ctx.get_setting(key, default) or default)
    except (TypeError, ValueError):
        return default


def _int(key: str, default: int) -> int:
    if _ctx is None:
        return default
    try:
        return int(_ctx.get_setting(key, default) or default)
    except (TypeError, ValueError):
        return default


def _str(key: str, default: str) -> str:
    if _ctx is None:
        return default
    return str(_ctx.get_setting(key, default) or default)


def _on_off(key: str, default: bool) -> str:
    return "开" if _bool(key, default) else "关"


def _register_alert_kind():
    owner = getattr(_ctx, "owner", None) if _ctx else None
    fn = getattr(owner, "_register_persistent_alert_kind", None)
    if callable(fn):
        try:
            fn(ALERT_KIND)
        except Exception:
            pass


def _unregister_alert_kind():
    owner = getattr(_ctx, "owner", None) if _ctx else None
    fn = getattr(owner, "_unregister_persistent_alert_kind", None)
    if callable(fn):
        try:
            fn(ALERT_KIND)
        except Exception:
            pass
