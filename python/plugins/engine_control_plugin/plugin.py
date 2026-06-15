# -*- coding: utf-8 -*-
"""Trusted engine write-back example — call the whole engine, both directions.

A UI panel whose buttons reach the in-process engine handles and *mutate* live
state:

* ``auto_key_engine``  — start() / stop() / get_status()
* ``boss_raid_engine`` — get_status() / next_phase() / reset()

Every engine call is defensively guarded (handle may be missing when offline),
and the panel renders identically on Entity (Tk) and WebView.  Disabled by
default and gated behind the ``engine_access`` / ``engine_control`` permissions
because it writes to live engines.
"""

_ctx = None


def on_load(ctx):
    global _ctx
    _ctx = ctx
    ctx.register_ui_panel(
        "engine_control",
        {"title": "Engine control",
         "description": "Drive auto_key_engine / boss_raid_engine from a plugin panel."},
        render=_render,
        on_action=_on_action,
    )
    ctx.on_snapshot(lambda _e: ctx.request_redraw("engine_control"))
    ctx.log("engine_control loaded; handles=%s" % ",".join(ctx.engine.available()))


def on_disable():
    if _ctx:
        _ctx.log("engine_control disabled")


def _call(handle_name, method, *args):
    handle = _ctx.get_engine(handle_name) if _ctx else None
    if handle is None:
        return None, f"{handle_name} unavailable"
    fn = getattr(handle, method, None)
    if not callable(fn):
        return None, f"{handle_name}.{method} unavailable"
    try:
        return fn(*args), ""
    except Exception as exc:
        return None, str(exc)


def _status_text(handle_name, method="get_status"):
    handle = _ctx.get_engine(handle_name) if _ctx else None
    if handle is None:
        return "offline"
    fn = getattr(handle, method, None)
    if not callable(fn):
        return type(handle).__name__
    try:
        st = fn() or {}
    except Exception as exc:
        return f"err: {exc}"
    if isinstance(st, dict):
        keys = ("running", "active", "phase", "phase_name", "state", "enabled")
        parts = [f"{k}={st.get(k)}" for k in keys if k in st]
        return " ".join(parts) or "ok"
    return str(st)[:60]


def _render(_payload=None):
    ui = _ctx.ui
    ak = _ctx.get_engine("auto_key_engine") if _ctx else None
    boss = _ctx.get_engine("boss_raid_engine") if _ctx else None
    return ui.panel("Engine Control", [
        ui.section("AutoKey 引擎", [
            ui.kv("status", _status_text("auto_key_engine"),
                  style=("ok" if ak is not None else "bad")),
            ui.row([
                ui.button("Start", action="autokey_start", style="primary",
                          disabled=ak is None),
                ui.button("Stop", action="autokey_stop", style="danger",
                          disabled=ak is None),
            ]),
        ], accent="gold"),
        ui.section("Boss Raid 引擎", [
            ui.kv("status", _status_text("boss_raid_engine"),
                  style=("ok" if boss is not None else "bad")),
            ui.row([
                ui.button("Next phase", action="boss_next", disabled=boss is None),
                ui.button("Reset", action="boss_reset", style="danger", disabled=boss is None),
            ]),
        ], accent="bad"),
        ui.button("刷新 Refresh", action="refresh"),
        ui.text("此插件回写真实引擎状态，需 engine_access 权限，默认禁用。", style="muted"),
    ])


def _on_action(action_id, _payload=None):
    if not _ctx:
        return {"ok": False}
    msg = ""
    if action_id == "autokey_start":
        _, msg = _call("auto_key_engine", "start")
    elif action_id == "autokey_stop":
        _, msg = _call("auto_key_engine", "stop")
    elif action_id == "boss_next":
        _, msg = _call("boss_raid_engine", "next_phase")
    elif action_id == "boss_reset":
        _, msg = _call("boss_raid_engine", "reset")
    if msg:
        _ctx.log(f"{action_id}: {msg}")
    return _render()
