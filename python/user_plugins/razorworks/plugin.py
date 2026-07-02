# -*- coding: utf-8 -*-
"""Razorworks analysis plugin — overlay + data pipeline.

Engine A data source (read-only). Engine E I/O (kernel HID).
Feature toggles in config; all default off except overlay.
"""

from __future__ import annotations

import os
import sys

# Bootstrap package context: the platform loader uses spec_from_file_location
# with a flat module name, so relative imports fail. Fix by setting __package__
# and ensuring the parent directory is on sys.path.
_dir = os.path.dirname(os.path.abspath(__file__))
_pkg = os.path.basename(_dir)
if not __package__:
    __package__ = _pkg
    _parent = os.path.dirname(_dir)
    if _parent not in sys.path:
        sys.path.insert(0, _parent)

import random
import time
from typing import Any, Optional

from .rw_track import TrackState
from .rw_prox import ProxState, read_weapon_id
from .rw_io import (
    click, click_with_delay, input_mode, key_tap, move_mouse,
)
from .rw_cloak import get_hider, hide as _kernel_hide, unhide as _kernel_unhide
from .rw_enc import label as _L
from .rw_bind import is_bind_active, vk_name
from .rw_ctx import Metrics
from .rw_guide import GuideHelper
from .rw_schema import SchemaConfig, default_config, load_config
from .rw_proj import DEFAULT_VIEWPORT, build_overlay_spec, filter_enemies
from .rw_map import build_radar_overlay, refresh_radar_convars
from .rw_src import DataSrc
from .rw_comp import CompState
from .rw_cfg import CfgMgr
from .rw_tempo import TempoEngine
from .rw_react import ReactState
from .rw_sight import combined_visibility
from .rw_cyber import CyberMenuController

VK_SPACE = 0x20

PANEL_ID = "rw_panel"
SURFACE = "rw_layer"
ALERT_KIND = "rw_layer"
DEFAULT_TICK_S = 0.008  # 125 Hz for tracker responsiveness
DEFAULT_HOTKEY = "CTRL+F7"

_ctx = None
_reader: Optional[DataSrc] = None
_config: Optional[SchemaConfig] = None
_timer_token = ""
_running = False
_last_status: dict = {}
_last_snapshot: dict = {}
_last_spec: dict = {}

# Feature states
_tracker = TrackState()
_comp = CompState()
_reactor = ReactState()
_proximity = ProxState()
_metrics = Metrics()
_ts = TempoEngine()
_nade = GuideHelper()
_settings: Optional[CfgMgr] = None
_cyber: Optional[CyberMenuController] = None
_legacy_expanded = False
_visible_edge_count = 5


def _tog(key):
    if _settings:
        _settings.set(key, not _settings.get(key, False))
        _settings.save_if_dirty()
    _refresh()


def _dot(key, default=False):
    return '◉' if _bool(key, default) else '○'


def _open(pid):
    if _ctx:
        _ctx.open_window(panel_id=pid)


def _toggle_legacy_expanded():
    global _legacy_expanded
    _legacy_expanded = not _legacy_expanded


def _build_legacy_items() -> list:
    """The original 13 quick-toggle/open-panel items, unchanged behavior —
    folded under the "Legacy Menu" accordion header in _build_menu_items()."""
    items = []
    items.append({'icon': '👁', 'label': 'ESP  ⚙',
                  'command': lambda: _open("rw_esp"), 'close_menu_before': True})
    items.append({'icon': _dot("tk_on"), 'label': f'{_L("aim")}  ⚙',
                  'command': lambda: _open("rw_tk"), 'close_menu_before': True})
    items.append({'icon': _dot("cp_on"), 'label': f'{_L("rcs")}  ⚙',
                  'command': lambda: _open("rw_cp"), 'close_menu_before': True})
    items.append({'icon': _dot("rt_on"), 'label': f'{_L("trg")}  ⚙',
                  'command': lambda: _open("rw_rt"), 'close_menu_before': True})
    items.append({'icon': '⏱', 'label': f'{_L("ts")}  ⚙',
                  'command': lambda: _open("rw_ts"), 'close_menu_before': True})
    items.append({'icon': _dot("vis_mp", True), 'label': '边缘扫描',
                  'command': lambda: _tog("vis_mp"), 'keep_menu_open': True})
    items.append({'icon': '🗡', 'label': '近战  ⚙',
                  'command': lambda: _open("rw_wpn"), 'close_menu_before': True})
    items.append({'icon': _dot("hop_on"), 'label': _L('hop'),
                  'command': lambda: _tog("hop_on"), 'keep_menu_open': True})
    items.append({'icon': '💣', 'label': f'{_L("nad")}  ⚙',
                  'command': lambda: _open("rw_gd"), 'close_menu_before': True})
    items.append({'icon': _dot("map_on"), 'label': _L('rad'),
                  'command': lambda: _tog("map_on"), 'keep_menu_open': True})
    items.append({'icon': '🛡', 'label': f'{_L("clk")}  ⚙',
                  'command': lambda: _open("rw_clk"), 'close_menu_before': True})
    items.append({'icon': '💾', 'label': '保存',
                  'command': lambda: _settings.save() if _settings else None})
    items.append({'icon': '↺', 'label': '重置',
                  'command': lambda: (_settings.reset_to_defaults(), _settings.save()) if _settings else None})
    return items


def _build_menu_items() -> list:
    """Child bar: Start/Stop + collapsible Legacy Menu accordion + Cyber Menu."""
    items = []
    items.append({'icon': '▶' if not _running else '⏹',
                  'label': 'Start' if not _running else 'Stop',
                  'command': _start if not _running else lambda: _stop("menu")})
    items.append({'icon': '─', 'label': '──────────'})

    arrow = '▾' if _legacy_expanded else '▸'
    items.append({'icon': '📋', 'label': f'{arrow} Legacy Menu',
                  'command': _toggle_legacy_expanded, 'keep_menu_open': True})
    if _legacy_expanded:
        items.extend(_build_legacy_items())

    cyber_open = _cyber.is_open() if _cyber else False
    items.append({'icon': '◉' if cyber_open else '✦',
                  'label': 'Cyber Menu' + (' 运行中' if cyber_open else ''),
                  'command': _cyber.toggle if _cyber else (lambda: None),
                  'keep_menu_open': True})
    return items


def on_load(ctx):
    global _ctx, _config, _settings, _cyber
    _ctx = ctx
    _config = load_config(ctx.path) or default_config()
    _settings = CfgMgr(ctx.path)
    _settings.load()
    nades_dir = os.path.join(ctx.path, "nades")
    _nade.load_builtin(nades_dir)
    _register_alert_kind()
    _cyber = CyberMenuController(lambda: _ctx, _cyber_snapshot, _apply_cyber_action)

    # Primary overview panel
    ctx.register_ui_panel(PANEL_ID,
        {"title": "Razorworks", "primary": True},
        render=_render, on_action=_on_action)

    # Detached sub-panels (hidden, opened via ctx.open_window)
    ctx.register_ui_panel("rw_esp",
        {"title": "ESP", "hidden": True, "width": 520, "height": 400},
        render=_render_esp, on_action=_on_sub_action)
    ctx.register_ui_panel("rw_tk",
        {"title": _L('aim'), "hidden": True, "width": 560, "height": 620},
        render=_render_tracker, on_action=_on_sub_action)
    ctx.register_ui_panel("rw_cp",
        {"title": _L('rcs'), "hidden": True, "width": 520, "height": 460},
        render=_render_comp, on_action=_on_sub_action)
    ctx.register_ui_panel("rw_rt",
        {"title": _L('trg'), "hidden": True, "width": 560, "height": 580},
        render=_render_reactor, on_action=_on_sub_action)
    ctx.register_ui_panel("rw_ts",
        {"title": _L('ts'), "hidden": True, "width": 520, "height": 400},
        render=_render_timeshift, on_action=_on_sub_action)
    ctx.register_ui_panel("rw_wpn",
        {"title": "Proximity", "hidden": True, "width": 520, "height": 400},
        render=_render_weapon, on_action=_on_sub_action)
    ctx.register_ui_panel("rw_gd",
        {"title": _L('nad'), "hidden": True, "width": 560, "height": 460},
        render=_render_guide, on_action=_on_sub_action)
    ctx.register_ui_panel("rw_clk",
        {"title": _L('clk'), "hidden": True, "width": 520, "height": 360},
        render=_render_cloak, on_action=_on_sub_action)

    ctx.register_menu_category("Razorworks", "⬢", _build_menu_items)
    ctx.register_hotkey("toggle", _toggle, default_key=DEFAULT_HOTKEY, label=_L('on'))
    ctx.log(f"rw loaded (idle, v={_config.config_version!r})")


def on_disable():
    _stop("disabled")
    if _cyber:
        _cyber.shutdown()


def on_unload():
    _stop("unloaded")
    if _cyber:
        _cyber.shutdown()
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
    use_scan = _bool("use_sig", True)
    _reader = DataSrc(_config, use_sig=use_scan)
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
        _ctx.log("cs2: kernel input unavailable, analysis features disabled")
        _tracker.enabled = False
        _comp.enabled = False
        _reactor.enabled = False
    if _bool("cloak_on", False):
        cr = _kernel_hide()
        _ctx.log(f"rw cloak: {cr}")

    _running = True
    base_tick = max(0.004, min(1.0, float(_ctx.get_setting("tick_s", DEFAULT_TICK_S) or DEFAULT_TICK_S)))
    _timer_token = _ctx.set_interval(_tick, base_tick)
    _ctx.log(f"rw started (pid={pid}, tick={base_tick*1000:.0f}ms, input={input_mode()})")
    _ctx.notify("RW", f"{_L('run')} (pid={pid})", duration_s=4.0, kind=ALERT_KIND)
    _refresh()
    return True


def _stop(reason: str = "manual") -> None:
    global _reader, _timer_token, _running
    _kernel_unhide()
    if _ctx is not None and _timer_token:
        try:
            _ctx.clear_timer(_timer_token)
        except Exception:
            pass
    _timer_token = ""
    if _reader is not None:
        _reader.detach()
    _running = False
    _tracker.reset()
    _comp.reset()
    _reactor.reset()
    _ts.reset()
    if _ctx is not None:
        try:
            _ctx.clear_overlay(SURFACE)
        except Exception:
            pass
        _ctx.log(f"rw_panel stopped ({reason})")
    if _settings:
        _settings.save_if_dirty()
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


_sync_counter = 0


def _sync_features():
    """Read feature toggles — only every 8th tick to save IOCTL calls."""
    global _sync_counter, _visible_edge_count
    _sync_counter += 1
    if _sync_counter & 7:  # skip 7 of 8 ticks
        return
    sens = _float("sensitivity", 1.0)
    _visible_edge_count = _int("vis_ec", 5)

    # Tracker
    _tracker.enabled = _bool("tk_on", False)
    _tracker.fov = _float("tk_fov", 5.0)
    _tracker.strategy = _str("tk_strat", "crosshair")
    _tracker.primary_bones = _str("tk_pri", "head").split(",")
    _tracker.secondary_bones = _str("tk_sec", "neck,chest").split(",")
    _tracker.smooth = _float("tk_smooth", 0.25)
    _tracker.speed_scale = _float("tk_spd", 1.0)
    _tracker.overshoot = _float("tk_over", 0.0)
    _tracker.curve = _str("tk_curve", "ease_out")
    _tracker.jitter = _float("tk_jit", 0.06)
    _tracker.random_offset = _float("tk_off", 0.0)
    _tracker.reaction_ms = _int("tk_react", 180)
    _tracker.hit_rate = _float("tk_hr", 0.72)
    _tracker.miss_jitter = _float("tk_missj", 0.4)
    _tracker.max_distance = _float("tk_maxd", 0)
    _tracker.visible_only = _str("tk_vis", "both") != "off"
    _tracker.sensitivity = sens

    # RCS
    _comp.enabled = _bool("cp_on", False)
    _comp.strength = _float("cp_str", 1.0)
    _comp.h_strength = _float("cp_hstr", 1.0)
    _comp.start_bullet = _int("cp_start", 1)
    _comp.max_bullets = _int("cp_max", 0)
    _comp.ema_alpha = _float("cp_alpha", 0.5)
    _comp.ramp_bullets = _int("cp_ramp", 4)
    _comp.hit_rate = _float("cp_hr", 1.0)
    _comp.jitter = _float("cp_j", 0.0)
    _comp.sensitivity = sens

    # Reactor
    _reactor.enabled = _bool("rt_on", False)
    _reactor.seed_mode = _bool("rt_seed", False)
    _reactor.primary_bones = _str("rt_pri", "head").split(",")
    _reactor.secondary_bones = _str("rt_sec", "neck,chest").split(",")
    _reactor.delay_min_ms = _int("rt_dmin", 160)
    _reactor.delay_max_ms = _int("rt_dmax", 280)
    _reactor.cooldown_ms = _int("rt_cd", 80)
    _reactor.hitchance_min = _float("rt_chance", 0.65)
    _reactor.spread_deg = _float("rt_spread", 0.8)
    _reactor.hit_rate = _float("rt_hr", 0.75)
    _reactor.burst_max = _int("rt_bmax", 0)
    _reactor.burst_cooldown_ms = _int("rt_bcd", 500)
    _reactor.visible_only = _str("rt_vis", "both") != "off"

    # Timeshift
    _ts.recall.enabled = _bool("bt_on", False)
    _ts.recall.max_window_ms = _float("bt_win", 50.0)
    _ts.selfmon.enabled = _bool("st_on", False)
    _ts.interp.enabled = _bool("ip_on", False)
    _ts.interp.render_delay_ms = _float("ip_delay", 0.0)
    _ts.extrap.enabled = _bool("xp_on", False)
    _ts.extrap.lookahead_ms = _float("xp_ahead", 20.0)
    _ts.extrap.min_confidence = _float("xp_conf", 0.3)

    # Auto-weapon
    _proximity.px_k = _bool("px_k", False)
    _proximity.px_z = _bool("px_z", False)

    # Nade helper
    _nade.enabled = _bool("gd_on", False)
    _nade.auto_mode = _bool("gd_auto", False)
    _nade.proximity = _float("gd_prox", 128.0)


# ── tick ────────────────────────────────────────────────────────────────

def _tick() -> None:
    global _last_status, _last_snapshot, _last_spec
    if _ctx is None or not _running or _reader is None:
        return
    if not _reader.attached:
        _stop("detached")
        return

    # Gaussian jitter: skip ~5% + randomize next tick interval
    if random.random() < 0.05:
        return
    # Vary next tick timing to break IOCTL periodicity fingerprint
    if hasattr(_tick, '_jitter_counter'):
        _tick._jitter_counter += 1
        if _tick._jitter_counter % 16 == 0:
            try:
                jitter_s = random.gauss(0, 0.002)
                if abs(jitter_s) < 0.005:
                    import time as _tm
                    _tm.sleep(max(0, jitter_s + 0.003))
            except Exception:
                pass
    else:
        _tick._jitter_counter = 0

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
    enemies = filter_enemies(entities, local_team)

    enemy_map = {}
    now = time.perf_counter()
    alive = set()
    eye = va = None
    if local:
        fov_scale = local.get("fov_scale", 1.0) or 1.0
        _tracker.fov_scale = fov_scale
        _comp.fov_scale = fov_scale
        eye = local.get("eye_pos")
        va = local.get("view_angles")

    # Single pass: build enemy_map + feed timeshift + edge-scan visibility
    local_idx = snap.get("local_index", -1)
    do_vis = _bool("vis_mp", True) and eye and va
    ec = _visible_edge_count
    for e in enemies:
        idx = e["index"]
        enemy_map[idx] = e
        alive.add(idx)
        origin = e.get("origin")
        if origin:
            _ts.feed(idx, origin, head=e.get("head_pos"),
                     vel=e.get("velocity", (0, 0, 0)), now=now)
        # Timeshift resolve
        if eye and va:
            resolved = _ts.resolve_aim_target(idx, eye, va, now)
            if resolved:
                e["head_pos"] = resolved
        # Edge-scan visibility (done ONCE, results cached for tracker+reactor)
        if do_vis:
            vis, vis_pts = combined_visibility(e, local_idx, eye, va, ec)
            e["is_visible"] = vis
            e["visible_points"] = vis_pts

    _ts.feed_local(local, now)
    _ts.cleanup(alive, now)

    # Hit stats
    if local:
        _metrics.update(local.get("shots_fired", 0), enemies)

    # --- Hop ---
    if _bool("hop_on", False) and local:
        flags = local.get("flags", 0)
        if flags and (flags & 1):  # FL_ONGROUND
            key_tap(VK_SPACE, 8)

    # --- Tracker (bind-gated) ---
    aim_delta = None
    if is_bind_active(_int("tk_bind", 0), _str("tk_bind_mode", "hold")):
        aim_delta = _tracker.tick(local, enemies)

    # --- Auto-stop ---
    if _ts.selfmon.enabled and _tracker.has_target:
        if _ts.selfmon.should_autostop():
            for vk in _ts.selfmon.counter_keys():
                key_tap(vk, 20)

    if aim_delta:
        move_mouse(aim_delta[0], aim_delta[1])

    # --- RCS ---
    rcs_delta = _comp.tick(local)
    if rcs_delta and not aim_delta:
        move_mouse(rcs_delta[0], rcs_delta[1])

    # --- Reactor (bind-gated) ---
    if is_bind_active(_int("rt_bind", 0), _str("rt_bind_mode", "hold")):
        trig = _reactor.tick(local, enemy_map, all_enemies=enemies)
        if trig == "fire":
            click_with_delay(_reactor.delay_min_ms, _reactor.delay_max_ms)

    # --- Prox-K / Prox-Z (bind-gated, weapon-gated) ---
    if local and (_proximity.px_k or _proximity.px_z):
        weapon_id = read_weapon_id(
            _reader.read, local.get("pawn", 0),
            _config.entity_fields.get("m_pClippingWeapon", 0),
            _config.entity_fields.get("m_iItemDefinitionIndex", 0))
        if is_bind_active(_int("px_kb", 0), _str("px_kb_mode", "hold")) or \
           is_bind_active(_int("px_zb", 0), _str("px_zb_mode", "hold")):
            action = _proximity.tick(local, enemies, weapon_id)
            if action == "left":
                click("left", hold_ms=30)
            elif action == "right":
                click("right", hold_ms=50)

    # --- Nade helper ---
    if _nade.enabled and local and local.get("origin"):
        map_name = local.get("map_name", "")
        if map_name:
            lineup = _nade.find_nearest(map_name, local["origin"])
            if lineup and _nade.auto_mode:
                act = _nade.auto_tick(lineup, local.get("view_angles", (0, 0)))
                if act and "move" in act:
                    move_mouse(act["move"][0], act["move"][1])
                elif act and "throw" in act:
                    if act.get("duck"):
                        key_tap(0x11, 30)  # ctrl = duck
                    if act["throw"] == "jump":
                        key_tap(VK_SPACE, 10)
                    click("left", hold_ms=40)

    # --- Overlay overlay ---
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
        show_fov_circle=_bool("show_fov_circle", False) and _tracker.enabled,
        fov_radius=_tracker.fov,
    )
    # Append guide overlay to Overlay ops
    if _nade.enabled and _nade.active_lineup and local:
        vw, vh = viewport[0], viewport[1]
        nade_ops = _nade.guide_overlay_ops(
            _nade.active_lineup, local.get("origin", (0,0,0)),
            local.get("view_angles", (0,0)), vw, vh)
        spec["ops"].extend(nade_ops)

    # Append radar overlay (enemy dots on game's built-in radar)
    if _bool("map_on", False) and local and local.get("origin"):
        map_name = local.get("map_name", "")
        if map_name:
            refresh_radar_convars(_reader.read, _reader.module_base, 0)
            vw, vh = viewport
            override = _bool("map_override", False)
            radar_ops = build_radar_overlay(
                map_name, local["origin"],
                local.get("view_angles", (0, 0))[1], enemies,
                hud_scale=_float("map_hs", 1.0) if override else None,
                map_rs=_float("map_rs", 0.4) if override else None,
                screen_w=vw, screen_h=vh)
            spec["ops"].extend(radar_ops)

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
        "aim_target": _tracker.has_target,
        "rcs_active": _comp._active,
        "reactor_pending": _reactor._pending_fire,
        "input_mode": input_mode(),
        "accuracy": _metrics.display,
    }
    _refresh()


# ── sub-panel renderers (detached windows) ─────────────────────────────


def _render_esp(_payload=None) -> dict:
    ui = _ctx.ui
    cols = [{"key": "n", "title": "功能"}, {"key": "s", "title": "", "align": "right"}]
    rows = []
    for key, label, dfl in [
        ("show_skeleton", "骨骼", True), ("show_hp_bar", "血条", True),
        ("show_distance", "距离", True), ("show_head", "头点", True),
        ("show_box", "框体", True), ("show_snapline", "拉线", False),
        ("show_fov_circle", "FOV圈", False),
    ]:
        rows.append({"n": label, "s": "●" if _bool(key, dfl) else "○"})
    nodes = [
        ui.table(cols, rows),
        ui.divider(),
    ]
    for key, label, dfl in [
        ("show_skeleton", "骨骼", True), ("show_hp_bar", "血条", True),
        ("show_distance", "距离", True), ("show_head", "头点", True),
        ("show_box", "框体", True), ("show_snapline", "拉线", False),
        ("show_fov_circle", "FOV圈", False),
    ]:
        nodes.append(ui.button(f"{'关闭' if _bool(key, dfl) else '开启'} {label}",
                               f"toggle_{key}"))
    return ui.panel("ESP", nodes)


def _sl(label, key, lo, hi, step, color="cyan"):
    """Native slider — drag to adjust, no panel redraw needed."""
    ui = _ctx.ui
    cur = _float(key, lo) if isinstance(lo, float) else float(_int(key, int(lo)))
    return ui.slider(label=label, id=key, value=cur,
                     lo=lo, hi=hi, step=step, color=color)


def _render_tracker(_payload=None) -> dict:
    ui = _ctx.ui
    on = _bool("tk_on", False)
    st = _last_status or {}
    strats = {"crosshair": "准星最近", "distance": "距离最近", "health": "血量最低"}
    nodes = [
        ui.row([ui.badge("ON" if on else "OFF", "ok" if on else "muted"),
                ui.badge("LOCKED" if st.get("aim_target") else "---",
                         "gold" if st.get("aim_target") else "muted"),
                ui.badge(strats.get(_tracker.strategy, "?"), "accent")]),
        _sl("FOV", "tk_fov", 0.5, 30.0, 0.5),
        _sl("平滑", "tk_smooth", 0.01, 1.0, 0.01, fmt=".2f"),
        _sl("速度", "tk_spd", 0.1, 3.0, 0.1),
        _sl("抖动", "tk_jit", 0.0, 0.5, 0.01, fmt=".2f"),
        _sl("偏移", "tk_off", 0.0, 2.0, 0.05, fmt=".2f"),
        _sl("反应 ms", "tk_react", 0, 500, 10, fmt=".0f"),
        _sl("命中率", "tk_hr", 0.0, 1.0, 0.01, color="gold", fmt=".0%"),
        _sl("过冲", "tk_over", 0.0, 0.5, 0.01, fmt=".2f"),
        ui.divider(),
        ui.row([ui.kv("优先", _str("tk_pri", "head,chest")),
                ui.button("↻", "cycle_tk_pri")]),
        ui.row([ui.kv("次要", _str("tk_sec", "neck,stomach")),
                ui.button("↻", "cycle_tk_sec")]),
        ui.row([ui.kv("可见", _str("tk_vis", "both")),
                ui.button("↻", "cycle_tk_vis")]),
        ui.divider(),
        ui.kv("绑定键", vk_name(_int("tk_bind", 0))),
        ui.row([ui.button("点击采集按键" if _capture_target != "tk_bind" else "▶ 按任意键...",
                           "capture_tk_bind", style="primary" if _capture_target == "tk_bind" else "default"),
                ui.button("清除", "clear_tk_bind", style="ghost"),
                ui.button(f"模式: {'按住' if _str('tk_bind_mode', 'hold') == 'hold' else '切换'}",
                           "cycle_tk_bind_mode")]),
        ui.divider(),
        ui.row([ui.button("开启" if not on else "关闭", "toggle_tk_on"),
                ui.button("策略 ↻", "cycle_strat")]),
    ]
    return ui.panel(_L('aim_s'), nodes)


def _render_comp(_payload=None) -> dict:
    ui = _ctx.ui
    on = _bool("cp_on", False)
    st = _last_status or {}
    nodes = [
        ui.row([ui.badge("ON" if on else "OFF", "ok" if on else "muted"),
                ui.badge("补偿中" if st.get("rcs_active") else "待机",
                         "gold" if st.get("rcs_active") else "muted")]),
        _sl("纵向强度", "cp_str", 0.0, 1.5, 0.05, fmt=".0%"),
        _sl("横向强度", "cp_hstr", 0.0, 1.5, 0.05, fmt=".0%"),
        _sl("EMA α", "cp_alpha", 0.05, 1.0, 0.05, fmt=".2f"),
        ui.row([ui.kv("起始弹", ""), ui.input(id="cp_start", value=str(_int("cp_start", 1)),
                input_type="number", width=50)]),
        ui.row([ui.kv("最大弹 (0=不限)", ""), ui.input(id="cp_max", value=str(_int("cp_max", 0)),
                input_type="number", width=50)]),
        ui.row([ui.kv("渐入弹数", ""), ui.input(id="cp_ramp", value=str(_int("cp_ramp", 4)),
                input_type="number", width=50)]),
        ui.divider(),
        ui.row([ui.button("开启" if not on else "关闭", "toggle_cp_on"),
                ui.button("应用文本", "apply_cp", style="primary")]),
    ]
    return ui.panel(_L('rcs_s'), nodes)


def _render_reactor(_payload=None) -> dict:
    ui = _ctx.ui
    on = _bool("rt_on", False)
    st = _last_status or {}
    nodes = [
        ui.row([ui.badge("ON" if on else "OFF", "ok" if on else "muted"),
                ui.badge("Hitchance" if _reactor.seed_mode else "经典", "accent"),
                ui.badge("FIRE" if st.get("reactor_pending") else "---",
                         "bad" if st.get("reactor_pending") else "muted")]),
        _sl("延迟 min ms", "rt_dmin", 0, 500, 10, fmt=".0f"),
        _sl("延迟 max ms", "rt_dmax", 50, 800, 10, fmt=".0f"),
        _sl("冷却 ms", "rt_cd", 0, 300, 10, fmt=".0f"),
        _sl("命中率", "rt_hr", 0.0, 1.0, 0.01, color="gold", fmt=".0%"),
        _sl("Hitchance ≥", "rt_chance", 0.0, 1.0, 0.05, fmt=".0%"),
        _sl("散布锥", "rt_spread", 0.0, 3.0, 0.1, fmt=".1f"),
        ui.row([ui.kv("连发 (0=不限)", str(_int("rt_bmax", 0))),
                ui.button("+", "inc_rt_bmax"), ui.button("-", "dec_rt_bmax")]),
        ui.divider(),
        ui.row([ui.kv("优先", _str("rt_pri", "head")),
                ui.button("↻", "cycle_rt_pri")]),
        ui.row([ui.kv("次要", _str("rt_sec", "neck,chest")),
                ui.button("↻", "cycle_rt_sec")]),
        ui.row([ui.kv("可见", _str("rt_vis", "both")),
                ui.button("↻", "cycle_rt_vis")]),
        ui.divider(),
        ui.kv("绑定键", vk_name(_int("rt_bind", 0))),
        ui.row([ui.button("点击采集按键" if _capture_target != "rt_bind" else "▶ 按任意键...",
                           "capture_rt_bind", style="primary" if _capture_target == "rt_bind" else "default"),
                ui.button("清除", "clear_rt_bind", style="ghost"),
                ui.button(f"模式: {'按住' if _str('rt_bind_mode', 'hold') == 'hold' else '切换'}",
                           "cycle_rt_bind_mode")]),
        ui.divider(),
        ui.row([ui.button("开启" if not on else "关闭", "toggle_rt_on"),
                ui.button("Hitchance ↻", "toggle_rt_seed")]),
    ]
    return ui.panel(_L('trg_s'), nodes)


def _render_timeshift(_payload=None) -> dict:
    ui = _ctx.ui
    cols = [{"key": "sys", "title": "系统"},
            {"key": "st", "title": ""},
            {"key": "val", "title": "参数"}]
    rows = [
        {"sys": "延迟补偿", "st": "●" if _ts.recall.enabled else "○",
         "val": f"{_ts.recall.max_window_ms:.0f}ms" if _ts.recall.enabled else "-"},
        {"sys": "自跟踪", "st": "●" if _ts.selfmon.enabled else "○",
         "val": "-"},
        {"sys": "插值", "st": "●" if _ts.interp.enabled else "○",
         "val": f"{_ts.interp.render_delay_ms:.0f}ms" if _ts.interp.enabled else "-"},
        {"sys": "外推", "st": "●" if _ts.extrap.enabled else "○",
         "val": f"{_ts.extrap.lookahead_ms:.0f}ms" if _ts.extrap.enabled else "-"},
    ]
    nodes = [
        ui.table(cols, rows),
        ui.kv("跟踪", f"{len(_ts._timelines)} 实体"),
        ui.divider(),
        ui.row([ui.button("延迟补偿 ↻", "toggle_bt_on"),
                ui.button("自跟踪 ↻", "toggle_st_on")]),
        ui.row([ui.button("插值 ↻", "toggle_ip_on"),
                ui.button("外推 ↻", "toggle_xp_on")]),
    ]
    return ui.panel(_L('ts'), nodes)


def _render_weapon(_payload=None) -> dict:
    ui = _ctx.ui
    kn = _bool("px_k", False)
    zs = _bool("px_z", False)
    nodes = [
        ui.row([ui.badge("Knife " + ("ON" if kn else "OFF"), "gold" if kn else "muted"),
                ui.badge("Zeus " + ("ON" if zs else "OFF"), "accent" if zs else "muted")]),
        ui.kv("Knife 绑定", vk_name(_int("px_kb", 0))),
        ui.row([ui.button("点击采集" if _capture_target != "px_kb" else "▶ 按任意键...",
                           "capture_px_kb", style="primary" if _capture_target == "px_kb" else "default"),
                ui.button("清除", "clear_px_kb", style="ghost"),
                ui.button(f"{'按住' if _str('px_kb_mode', 'hold') == 'hold' else '切换'}",
                           "cycle_px_kb_bind_mode")]),
        ui.kv("Zeus 绑定", vk_name(_int("px_zb", 0))),
        ui.row([ui.button("点击采集" if _capture_target != "px_zb" else "▶ 按任意键...",
                           "capture_px_zb", style="primary" if _capture_target == "px_zb" else "default"),
                ui.button("清除", "clear_px_zb", style="ghost"),
                ui.button(f"{'按住' if _str('px_zb_mode', 'hold') == 'hold' else '切换'}",
                           "cycle_px_zb_bind_mode")]),
        ui.divider(),
        ui.row([ui.button("Knife ↻", "toggle_px_k"),
                ui.button("Zeus ↻", "toggle_px_z")]),
    ]
    return ui.panel("Proximity", nodes)


def _render_guide(_payload=None) -> dict:
    ui = _ctx.ui
    on = _bool("gd_on", False)
    auto = _bool("gd_auto", False)
    lu = _nade.active_lineup
    nodes = [
        ui.row([ui.badge("ON" if on else "OFF", "ok" if on else "muted"),
                ui.badge("自动" if auto else "引导", "accent" if auto else "muted")]),
        ui.kv("数据", f"{_nade.lineup_count} lineups / {_nade.map_count} 地图"),
        ui.kv("当前", lu.name if lu else "-"),
        ui.divider(),
        ui.row([ui.button("开启" if not on else "关闭", "toggle_gd_on"),
                ui.button("模式 ↻", "toggle_gd_auto")]),
    ]
    return ui.panel(_L('nad'), nodes)


def _render_cloak(_payload=None) -> dict:
    ui = _ctx.ui
    on = _bool("cloak_on", False)
    ck = get_hider().status()
    nodes = [
        ui.row([ui.badge("ON" if on else "OFF", "ok" if on else "muted"),
                ui.badge("DKOM" if ck.get("dkom") else "-",
                         "accent" if ck.get("dkom") else "muted"),
                ui.badge("CID" if ck.get("cid") else "-",
                         "accent" if ck.get("cid") else "muted"),
                ui.badge("PEB" if ck.get("peb") else "-",
                         "accent" if ck.get("peb") else "muted")]),
        ui.divider(),
        ui.row([ui.button("开启" if not on else "关闭", "toggle_cloak_on")]),
    ]
    return ui.panel(_L('clk'), nodes)


_capture_target: str = ""

# Presets shared by every "cycle_<key>" action, whether it comes from a
# native Tk sub-panel button or the Cyber Menu dashboard.
_CYCLE_PRESETS = {
    "tk_strat": ["crosshair", "distance", "health"],
    "tk_pri": ["head", "head,chest", "head,neck,chest", "chest", "chest,stomach"],
    "rt_pri": ["head", "head,chest", "head,neck,chest", "chest", "chest,stomach"],
    "tk_sec": ["neck,chest", "neck,stomach", "chest,stomach,pelvis", "stomach", "neck"],
    "rt_sec": ["neck,chest", "neck,stomach", "chest,stomach,pelvis", "stomach", "neck"],
    "tk_vis": ["both", "spotted", "multipoint", "off"],
    "rt_vis": ["both", "spotted", "multipoint", "off"],
    "tk_curve": ["ease_out", "linear", "ease_in", "overshoot"],
    "vis_ec": [3, 5, 9],
}

# Step/lo/hi for every "inc_/dec_<key>" (and Cyber Menu stepper) action.
_NUDGE_STEPS = {
    "tk_fov": (0.5, 0.5, 30.0), "tk_smooth": (0.01, 0.01, 1.0),
    "tk_spd": (0.1, 0.1, 3.0), "tk_jit": (0.01, 0.0, 0.5),
    "tk_off": (0.05, 0.0, 2.0), "tk_react": (10, 0, 500),
    "tk_hr": (0.01, 0.0, 1.0), "tk_over": (0.01, 0.0, 0.5),
    "cp_str": (0.05, 0.0, 1.5), "cp_hstr": (0.05, 0.0, 1.5),
    "cp_alpha": (0.05, 0.05, 1.0),
    "cp_start": (1, 1, 99), "cp_max": (1, 0, 99), "cp_ramp": (1, 0, 30),
    "rt_dmin": (10, 0, 500), "rt_dmax": (10, 50, 800),
    "rt_cd": (10, 0, 300), "rt_hr": (0.01, 0.0, 1.0),
    "rt_chance": (0.05, 0.0, 1.0), "rt_spread": (0.1, 0.0, 3.0),
    "rt_bmax": (1, 0, 30),
}


def _cycle_value(key: str) -> None:
    opts = _CYCLE_PRESETS.get(key)
    if not opts or not _settings:
        return
    cur = _settings.get(key, opts[0])
    idx = (opts.index(cur) + 1) % len(opts) if cur in opts else 0
    _settings.set(key, opts[idx])
    _settings.save_if_dirty()


def _nudge_value(key: str, direction: int) -> None:
    if not _settings:
        return
    step, lo, hi = _NUDGE_STEPS.get(key, (1, 0, 9999))
    cur = _settings.get(key, lo)
    d = step if direction >= 0 else -step
    val = cur + d
    if isinstance(lo, float):
        val = round(max(lo, min(hi, val)), 4)
    else:
        val = max(lo, min(hi, int(round(val))))
    _settings.set(key, val)
    _settings.save_if_dirty()


def _cycle_bind_mode(key: str) -> None:
    if not _settings:
        return
    cur = _str(key, "hold")
    _settings.set(key, "toggle" if cur == "hold" else "hold")
    _settings.save_if_dirty()


def _clear_bind(key: str) -> None:
    if _settings:
        _settings.set(key, 0)
        _settings.save_if_dirty()


def _start_bind_capture(bind_key: str) -> None:
    global _capture_target
    _capture_target = bind_key
    if _ctx:
        _ctx.notify("RW", f"按任意键/鼠标键绑定 {bind_key}...", duration_s=5.0)
    import threading
    threading.Thread(target=_capture_key_async, args=(bind_key,), daemon=True).start()


def _set_value(key: str, value: Any) -> None:
    if value is None or not _settings:
        return
    old = _settings.get(key)
    if isinstance(old, int):
        _settings.set(key, int(round(float(value))))
    else:
        _settings.set(key, float(value))


def _on_sub_action(action_id: str, _payload=None) -> Any:
    """Shared action handler for all detached sub-panels."""
    payload = _payload or {}
    inputs = payload.get("inputs", {})

    if action_id.startswith("toggle_"):
        _tog(action_id[7:])

    elif action_id == "cycle_strat":
        _cycle_value("tk_strat")

    elif action_id.startswith("cycle_") and action_id.endswith("_pri"):
        _cycle_value(action_id[6:])

    elif action_id.startswith("cycle_") and action_id.endswith("_sec"):
        _cycle_value(action_id[6:])

    elif action_id.startswith("cycle_") and action_id.endswith("_vis"):
        _cycle_value(action_id[6:])

    elif action_id.startswith("inc_") or action_id.startswith("dec_"):
        key = action_id[4:]
        _nudge_value(key, 1 if action_id.startswith("inc_") else -1)

    elif action_id.startswith("cycle_") and action_id.endswith("_bind_mode"):
        _cycle_bind_mode(action_id[6:])

    elif action_id.startswith("clear_"):
        _clear_bind(action_id[6:])

    elif action_id.startswith("capture_"):
        _start_bind_capture(action_id[8:])

    elif action_id.startswith("slide_"):
        _set_value(action_id[6:], payload.get("value"))
        return {"ok": True}

    elif action_id.startswith("apply_"):
        prefix = action_id[6:]  # "tk", "cp", "rt"
        if _settings and inputs:
            for k, v in inputs.items():
                if not v and v != "0":
                    continue
                old = _settings.get(k)
                try:
                    if isinstance(old, float):
                        _settings.set(k, float(v))
                    elif isinstance(old, int):
                        _settings.set(k, int(float(v)))
                    else:
                        _settings.set(k, str(v))
                except (ValueError, TypeError):
                    pass
            _settings.save_if_dirty()
            if _ctx:
                _ctx.notify("RW", _L('sav'), duration_s=1.5)

    elif action_id == "save":
        if _settings:
            _settings.save()

    _refresh()
    return {"ok": True}


def _apply_cyber_action(msg: dict) -> None:
    """Dispatch an action sent by the Cyber Menu dashboard subprocess.

    Routes through the same shared functions the native Tk sub-panels use
    (_tog/_cycle_value/_nudge_value/_set_value/bind helpers) so both UIs
    share one settings state, not two.
    """
    kind = msg.get("kind")
    key = msg.get("key")
    if kind == "set":
        _set_value(key, msg.get("value"))
    elif kind == "toggle":
        _tog(key)
    elif kind == "cycle":
        _cycle_value(key)
    elif kind == "nudge":
        _nudge_value(key, msg.get("dir", 1))
    elif kind == "capture_bind":
        _start_bind_capture(key)
    elif kind == "clear_bind":
        _clear_bind(key)
    elif kind == "cycle_bind_mode":
        _cycle_bind_mode(key)
    elif kind == "command":
        name = msg.get("name")
        if name == "start":
            _start()
        elif name == "stop":
            _stop("cyber")
        elif name == "reload":
            _reload()
        elif name == "save":
            if _settings:
                _settings.save()
        elif name == "reset":
            if _settings:
                _settings.reset_to_defaults()
                _settings.save()
        elif name == "reset_stats":
            _metrics.reset()
    _refresh()


def _cyber_snapshot() -> dict:
    """Build the JSON state pushed to the Cyber Menu dashboard every tick."""
    st = _last_status or {}
    cfg = _config or default_config()
    if _running:
        status = "LIVE"
    elif cfg.is_complete():
        status = "READY"
    else:
        status = "INCOMPLETE"

    overview = {
        "status": status,
        "tick_ms": st.get("tick_ms"),
        "entity_count": st.get("entity_count", 0),
        "input_mode": input_mode(),
        "accuracy": _metrics.accuracy,
        "accuracy_display": _metrics.display,
    }
    groups = {
        "aim": {
            "tk_on": _bool("tk_on", False), "tk_fov": _float("tk_fov", 5.0),
            "tk_smooth": _float("tk_smooth", 0.25), "tk_spd": _float("tk_spd", 1.0),
            "tk_jit": _float("tk_jit", 0.06), "tk_off": _float("tk_off", 0.0),
            "tk_react": _int("tk_react", 180), "tk_hr": _float("tk_hr", 0.72),
            "tk_over": _float("tk_over", 0.0), "tk_curve": _str("tk_curve", "ease_out"),
            "tk_missj": _float("tk_missj", 0.4), "tk_maxd": _float("tk_maxd", 0),
            "tk_pri": _str("tk_pri", "head,chest"), "tk_sec": _str("tk_sec", "neck,stomach"),
            "tk_vis": _str("tk_vis", "both"), "tk_strat": _str("tk_strat", "crosshair"),
            "tk_bind": _int("tk_bind", 0), "tk_bind_label": vk_name(_int("tk_bind", 0)),
            "tk_bind_mode": _str("tk_bind_mode", "hold"),
        },
        "rcs": {
            "cp_on": _bool("cp_on", False), "cp_str": _float("cp_str", 1.0),
            "cp_hstr": _float("cp_hstr", 1.0), "cp_alpha": _float("cp_alpha", 0.5),
            "cp_start": _int("cp_start", 1), "cp_max": _int("cp_max", 0),
            "cp_ramp": _int("cp_ramp", 4), "cp_hr": _float("cp_hr", 1.0),
            "cp_j": _float("cp_j", 0.0),
        },
        "trigger": {
            "rt_on": _bool("rt_on", False), "rt_seed": _bool("rt_seed", False),
            "rt_dmin": _int("rt_dmin", 160), "rt_dmax": _int("rt_dmax", 280),
            "rt_cd": _int("rt_cd", 80), "rt_hr": _float("rt_hr", 0.75),
            "rt_chance": _float("rt_chance", 0.65), "rt_spread": _float("rt_spread", 0.8),
            "rt_bmax": _int("rt_bmax", 0), "rt_bcd": _int("rt_bcd", 500),
            "rt_pri": _str("rt_pri", "head"), "rt_sec": _str("rt_sec", "neck,chest"),
            "rt_vis": _str("rt_vis", "both"), "rt_bind": _int("rt_bind", 0),
            "rt_bind_label": vk_name(_int("rt_bind", 0)), "rt_bind_mode": _str("rt_bind_mode", "hold"),
        },
        "esp": {
            "show_health": _bool("show_health", True), "show_hp_bar": _bool("show_hp_bar", True),
            "show_skeleton": _bool("show_skeleton", True), "show_distance": _bool("show_distance", True),
            "show_head": _bool("show_head", True), "show_box": _bool("show_box", True),
            "show_snapline": _bool("show_snapline", False), "show_fov_circle": _bool("show_fov_circle", False),
        },
        "timeshift": {
            "bt_on": _bool("bt_on", False), "bt_win": _float("bt_win", 50.0),
            "st_on": _bool("st_on", False), "ip_on": _bool("ip_on", False),
            "ip_delay": _float("ip_delay", 0.0), "xp_on": _bool("xp_on", False),
            "xp_ahead": _float("xp_ahead", 20.0), "xp_conf": _float("xp_conf", 0.3),
        },
        "proximity": {
            "px_k": _bool("px_k", False), "px_z": _bool("px_z", False),
            "px_kb": _int("px_kb", 0), "px_kb_label": vk_name(_int("px_kb", 0)),
            "px_kb_mode": _str("px_kb_mode", "hold"),
            "px_zb": _int("px_zb", 0), "px_zb_label": vk_name(_int("px_zb", 0)),
            "px_zb_mode": _str("px_zb_mode", "hold"),
        },
        "nade": {
            "gd_on": _bool("gd_on", False), "gd_auto": _bool("gd_auto", False),
            "gd_prox": _float("gd_prox", 128.0),
        },
        "radar": {
            "map_on": _bool("map_on", False), "map_override": _bool("map_override", False),
            "map_hs": _float("map_hs", 1.0), "map_rs": _float("map_rs", 0.4),
        },
        "cloak": {
            "cloak_on": _bool("cloak_on", False),
        },
        "system": {
            "sensitivity": _float("sensitivity", 1.0), "vis_mp": _bool("vis_mp", True),
            "vis_ec": _int("vis_ec", 5), "use_sig": _bool("use_sig", True),
        },
    }
    return {
        "running": _running,
        "overview": overview,
        "groups": groups,
        "capture_target": _capture_target,
    }


def _capture_key_async(bind_key: str):
    """Background thread: poll GetAsyncKeyState for any key/mouse press."""
    global _capture_target
    import ctypes
    import time as _t
    user32 = ctypes.windll.user32

    # Wait for all buttons/keys to be released first (avoid catching the click)
    _t.sleep(0.3)
    release_deadline = _t.monotonic() + 2.0
    while _t.monotonic() < release_deadline:
        any_held = False
        for vk in range(0x01, 0x07):
            try:
                if user32.GetAsyncKeyState(vk) & 0x8000:
                    any_held = True
            except Exception:
                pass
        if not any_held:
            break
        _t.sleep(0.05)

    # Now capture the NEXT press
    deadline = _t.monotonic() + 8.0
    while _t.monotonic() < deadline and _capture_target == bind_key:
        for vk in list(range(0x01, 0x07)) + list(range(0x08, 0x100)):
            if vk in (0x03,):
                continue
            try:
                if user32.GetAsyncKeyState(vk) & 0x8000:
                    if _settings:
                        _settings.set(bind_key, vk)
                        _settings.save_if_dirty()
                    from .rw_bind import vk_name as _vkn
                    name = _vkn(vk)
                    if _ctx:
                        _ctx.notify("RW", f"{bind_key} → {name}", duration_s=2.5)
                    _capture_target = ""
                    _refresh()
                    return
            except Exception:
                pass
        _t.sleep(0.02)
    _capture_target = ""
    if _ctx:
        _ctx.notify("RW", "采集超时", duration_s=1.5)


# ── main panel ──────────────────────────────────────────────────────────

def _render(_payload=None) -> dict:
    ui = _ctx.ui if _ctx is not None else None
    if ui is None:
        return {"version": 1, "title": "Razorworks", "nodes": []}
    cfg = _config or default_config()
    st = _last_status or {}
    running = _running
    complete = cfg.is_complete()
    combat = cfg.combat_ready()
    err = str(st.get("last_error") or "")

    # ── Status header ──
    if running:
        s_badge = ui.badge("LIVE", "ok")
    elif complete:
        s_badge = ui.badge("READY", "accent")
    else:
        s_badge = ui.badge("INCOMPLETE", "bad")

    acc = _metrics.accuracy
    acc_color = "ok" if acc >= 0.5 else ("gold" if acc >= 0.25 else "bad")

    nodes = [
        ui.card("状态", [
            ui.row([s_badge,
                    ui.badge(f"{st.get('tick_ms', '-')}ms", "muted"),
                    ui.badge(f"{st.get('entity_count', 0)} 实体", "muted"),
                    ui.badge(input_mode().upper(), "accent")]),
            ui.bar("命中率", pct=acc, color=acc_color, caption=_metrics.display),
        ]),
    ]
    if err:
        nodes.append(ui.text(err, style="bad"))

    if not combat and complete:
        nodes.append(ui.text(f"缺: {', '.join(cfg.combat_missing())}", style="warn"))

    # ── Feature overview (compact badges + open-panel buttons) ──
    def _row(label, key, panel_id, on_key=None, accent="muted"):
        on = _bool(on_key, False) if on_key else None
        badges = []
        if on is not None:
            badges.append(ui.badge("ON" if on else "OFF", "ok" if on else "muted"))
        badges.append(ui.badge(label, accent))
        return ui.row(badges + [ui.button("⚙", f"open_{panel_id}")])

    nodes.append(_row("ESP", "show_box", "rw_esp", accent="cyan"))
    nodes.append(_row(_L('aim'), "tk_on", "rw_tk", on_key="tk_on", accent="gold"))
    nodes.append(_row(_L('rcs'), "cp_on", "rw_cp", on_key="cp_on", accent="cyan"))
    nodes.append(_row(_L('trg'), "rt_on", "rw_rt", on_key="rt_on", accent="warn"))
    nodes.append(_row(_L('ts'),  "bt_on", "rw_ts", accent="accent"))
    nodes.append(_row("近战",    "px_k",  "rw_wpn", accent="gold"))
    nodes.append(_row(_L('nad'), "gd_on", "rw_gd", on_key="gd_on", accent="cyan"))
    nodes.append(_row(_L('clk'), "cloak_on", "rw_clk", on_key="cloak_on", accent="bad"))

    # Inline simple toggles (no sub-panel)
    nodes.append(ui.row([
        ui.badge("边缘扫描" if _bool("vis_mp", True) else "Spotted",
                 "ok" if _bool("vis_mp", True) else "muted"),
        ui.badge(f"{_L('hop')} {'ON' if _bool('hop_on', False) else 'OFF'}",
                 "ok" if _bool("hop_on", False) else "muted"),
        ui.badge(f"{_L('rad')} {'ON' if _bool('map_on', False) else 'OFF'}",
                 "ok" if _bool("map_on", False) else "muted"),
    ]))

    # ── Key Binds (inline) ──
    nodes.append(ui.section(_L('key'), [
        ui.kv(_L('aim'), vk_name(_int("tk_bind", 0))),
        ui.kv(_L('trg'), vk_name(_int("rt_bind", 0))),
        ui.kv("K", vk_name(_int("px_kb", 0))),
        ui.kv("Z", vk_name(_int("px_zb", 0))),
    ], accent="muted"))

    # ── Controls ──
    nodes.append(ui.divider())
    nodes.append(ui.row([
        ui.button("启动" if not running else "停止",
                  "start" if not running else "stop",
                  style="primary" if not running else "danger"),
        ui.button("重载", "reload"),
        ui.button("清零", "reset_stats"),
    ]))
    nodes.append(ui.row([
        ui.button("保存", "save_settings", style="primary"),
        ui.button("重置", "reset_settings"),
        ui.button("清屏", "clear"),
    ]))

    return ui.panel("Razorworks", nodes)


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
        _metrics.reset()
    elif action_id == "save_settings":
        if _settings:
            _settings.save()
            if _ctx:
                _ctx.notify("RW", _L('sav'), duration_s=2.0, kind=ALERT_KIND)
    elif action_id == "reset_settings":
        if _settings:
            _settings.reset_to_defaults()
            _settings.save()
    elif action_id.startswith("open_"):
        pid = action_id[5:]
        if _ctx:
            _ctx.open_window(panel_id=pid)
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
        _ctx.notify(_L('err'), msg, duration_s=6.0, kind=ALERT_KIND)
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


def _g(key: str, default: Any = None) -> Any:
    """Read setting: persistent file → ctx fallback → default."""
    if _settings is not None:
        return _settings.get(key, default)
    if _ctx is not None:
        return _ctx.get_setting(key, default)
    return default


def _bool(key: str, default: bool) -> bool:
    val = _g(key, default)
    if isinstance(val, bool):
        return val
    if isinstance(val, str):
        return val.strip().lower() in ("1", "true", "yes", "on")
    return bool(val)


def _float(key: str, default: float) -> float:
    try:
        return float(_g(key, default) or default)
    except (TypeError, ValueError):
        return default


def _int(key: str, default: int) -> int:
    try:
        return int(_g(key, default) or default)
    except (TypeError, ValueError):
        return default


def _str(key: str, default: str) -> str:
    return str(_g(key, default) or default)


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
