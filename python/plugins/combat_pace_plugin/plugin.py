# -*- coding: utf-8 -*-
"""Custom trigger_type + timer + UI panel example.

* ``register_trigger_type("dps_floor")`` — a plugin-defined trigger the ACT
  trigger engine can evaluate: it matches when the encounter is live but total
  DPS sits below a configured floor.
* ``register_timer("burst_window")`` — declares a reusable timer extension.
* ``register_ui_panel("pace")`` — shows the live pace and a manual burst-window
  countdown driven by a button.

Read-only; safe to keep enabled.
"""

import time

_ctx = None
_burst_deadline = 0.0


def _fmt(value):
    try:
        n = float(value or 0)
    except Exception:
        return "0"
    for unit, scale in (("M", 1e6), ("K", 1e3)):
        if abs(n) >= scale:
            return f"{n / scale:.2f}{unit}"
    return f"{int(n)}"


def on_load(ctx):
    global _ctx, _burst_deadline
    _ctx = ctx
    _burst_deadline = 0.0
    ctx.set_defaults({"dps_floor": 500000, "burst_window_s": 15})
    ctx.register_trigger_type(
        "dps_floor",
        {"label": "DPS floor", "schema": {"threshold": "integer"},
         "description": "Fires when live DPS drops below the threshold."},
        handler=_dps_floor,
    )
    ctx.register_timer(
        "burst_window",
        {"duration_s": int(ctx.setting("burst_window_s", 15)), "scope": "encounter",
         "label": "Burst window"},
    )
    ctx.register_ui_panel(
        "pace",
        {"title": "Combat pace", "description": "Live DPS vs floor and a manual burst-window timer."},
        render=_render,
        on_action=_on_action,
    )
    ctx.on_snapshot(lambda _e: ctx.request_redraw("pace"))
    ctx.log("combat_pace loaded")


def on_disable():
    if _ctx:
        _ctx.log("combat_pace disabled")


def _dps_floor(payload):
    rs = (payload or {}).get("render_spec") or {}
    rule = (payload or {}).get("rule") or {}
    totals = rs.get("totals") or {}
    try:
        floor = int(rule.get("threshold") or (_ctx.setting("dps_floor", 500000) if _ctx else 500000))
    except Exception:
        floor = 500000
    dps = totals.get("dps") or 0
    is_live = bool((rs.get("encounter") or {}).get("status") in ("active", "pending_reset")
                   or float(totals.get("elapsed_s") or 0) > 0)
    matched = bool(is_live and dps < floor)
    return {
        "matched": matched,
        "severity": "warn",
        "message": f"DPS {_fmt(dps)} < floor {_fmt(floor)}" if matched else "pace ok",
    }


def _render(_payload=None):
    snap = _ctx.get_snapshot() if _ctx else {}
    rs = snap.get("render_spec") or {}
    totals = rs.get("totals") or {}
    floor = int(_ctx.setting("dps_floor", 500000)) if _ctx else 500000
    dps = float(totals.get("dps") or 0)
    pct = max(0.0, min(1.0, dps / floor)) if floor else 0.0
    remaining = max(0.0, _burst_deadline - time.time())
    ui = _ctx.ui
    nodes = [
        ui.bar("DPS vs floor", pct=pct, color=("ok" if dps >= floor else "bad"),
               caption=f"{_fmt(dps)} / {_fmt(floor)}"),
    ]
    if remaining > 0:
        nodes.append(ui.bar("Burst window", pct=remaining / max(1, int(_ctx.setting("burst_window_s", 15))),
                            color="gold", caption=f"{remaining:.0f}s left"))
        nodes.append(ui.badge("BURST ACTIVE", "gold"))
    else:
        nodes.append(ui.text("Burst window idle", style="muted"))
    nodes.append(ui.button("开始爆发窗口 Start burst", action="start_burst", style="primary"))
    return ui.panel("Combat Pace", nodes)


def _on_action(action_id, _payload=None):
    global _burst_deadline
    if _ctx and action_id == "start_burst":
        _burst_deadline = time.time() + int(_ctx.setting("burst_window_s", 15))
        _ctx.emit("plugin_burst_window_started", {"duration_s": int(_ctx.setting("burst_window_s", 15))})
        _ctx.log("burst window started")
    return _render()
