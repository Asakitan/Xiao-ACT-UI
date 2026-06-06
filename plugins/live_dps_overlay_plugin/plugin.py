# -*- coding: utf-8 -*-
"""Flagship UI-redraw example plugin.

Demonstrates the full redraw surface of the SAO plugin engine:

* ``register_ui_panel`` — a declarative DPS board that appears in the plugin
  manager "Panels" tab on BOTH the Entity (Tk) and WebView renderers.
* ``request_redraw`` — called on every ``act_snapshot`` so the board repaints
  live during combat.
* ``set_overlay`` / ``clear_overlay`` — an opt-in compact overlay pinned onto
  the DPS meter surface (toggled by a button in the board).
* engine read access — pulls the live snapshot straight from the shared engine
  state via ``ctx.get_snapshot()``.

It is intentionally read-only (no engine writes), so it is safe to keep enabled.
"""

_ctx = None


def _fmt(value):
    try:
        n = float(value or 0)
    except Exception:
        return "0"
    for unit, scale in (("B", 1e9), ("M", 1e6), ("K", 1e3)):
        if abs(n) >= scale:
            return f"{n / scale:.2f}{unit}"
    return f"{int(n)}"


def on_load(ctx):
    global _ctx
    _ctx = ctx
    ctx.set_defaults({"top_n": 8, "pin_overlay": False})
    ctx.log("live_dps_overlay loaded")
    ctx.register_ui_panel(
        "live_dps",
        {
            "title": "Live DPS board",
            "description": "Live encounter DPS leaderboard with boss HP, redrawn on every snapshot.",
            "route": "plugin://live_dps_overlay_plugin/board",
        },
        render=_render,
        on_action=_on_action,
    )
    ctx.on_snapshot(_on_snapshot)
    ctx.on_encounter_finalized(lambda _e: ctx.request_redraw("live_dps"))


def on_enable():
    if _ctx:
        _ctx.log("live_dps_overlay enabled")


def on_disable():
    if _ctx:
        _ctx.clear_overlay("dps")
        _ctx.log("live_dps_overlay disabled")


def on_unload():
    if _ctx:
        _ctx.clear_overlay("dps")


def _board_spec(compact=False):
    snap = _ctx.get_snapshot() if _ctx else {}
    rs = snap.get("render_spec") or {}
    totals = rs.get("totals") or {}
    boss = rs.get("boss") or {}
    rows = rs.get("rows") or []
    top_n = max(1, int(_ctx.setting("top_n", 8) if _ctx else 8))
    top = sorted(rows, key=lambda r: r.get("damage", 0), reverse=True)[: (5 if compact else top_n)]
    table_rows = [{
        "rank": r.get("rank"),
        "name": r.get("name") or "?",
        "dps": _fmt(r.get("dps")),
        "pct": f"{(r.get('damage_pct') or 0) * 100:.0f}%",
        "self": bool(r.get("is_self")),
    } for r in top]

    ui = _ctx.ui
    nodes = []
    if boss.get("active"):
        nodes.append(ui.bar(
            "BOSS HP", pct=float(boss.get("hp_pct") or 0.0), color="bad",
            caption=f"{_fmt(boss.get('current_hp'))} / {_fmt(boss.get('total_hp'))}"))
    nodes.append(ui.row([
        ui.badge("LIVE" if (snap.get("live") or {}).get("encounter_active") else "IDLE",
                 "ok" if (snap.get("live") or {}).get("encounter_active") else "muted"),
        ui.kv("Total", _fmt(totals.get("damage"))),
        ui.kv("DPS", _fmt(totals.get("dps"))),
        ui.kv("Elapsed", f"{float(totals.get('elapsed_s') or 0):.0f}s"),
    ]))
    nodes.append(ui.table(
        columns=[
            {"key": "rank", "title": "#", "align": "right"},
            {"key": "name", "title": "Player"},
            {"key": "dps", "title": "DPS", "align": "right"},
            {"key": "pct", "title": "%", "align": "right"},
        ],
        rows=table_rows, highlight_key="self"))
    if not compact:
        pinned = bool(_ctx.setting("pin_overlay", False))
        nodes.append(ui.row([
            ui.button("刷新 Refresh", action="refresh"),
            ui.button("取消钉住 Unpin" if pinned else "钉到DPS悬浮窗 Pin",
                      action="toggle_pin", style="primary"),
        ]))
    return _ctx.ui.panel("Live DPS", nodes)


def _render(_payload=None):
    return _board_spec(compact=False)


def _on_action(action_id, _payload=None):
    if not _ctx:
        return {"ok": False}
    if action_id == "toggle_pin":
        pinned = not bool(_ctx.setting("pin_overlay", False))
        _ctx.set_setting("pin_overlay", pinned)
        if pinned:
            _ctx.set_overlay("dps", _board_spec(compact=True))
        else:
            _ctx.clear_overlay("dps")
        _ctx.log(f"pin_overlay={pinned}")
    return _board_spec(compact=False)


def _on_snapshot(_event):
    if not _ctx:
        return
    _ctx.request_redraw("live_dps")
    if bool(_ctx.setting("pin_overlay", False)):
        _ctx.set_overlay("dps", _board_spec(compact=True))
