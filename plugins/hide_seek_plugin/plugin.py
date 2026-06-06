# -*- coding: utf-8 -*-
"""Auto Hide & Seek — self-contained example plugin.

This was originally wired directly into the main program (Entity mixin + WebView
bridge, an F11 toggle and a persistent alert).  It is now a **self-contained
plugin** that ships its own CV automation engine (``hide_seek_engine.py``) and
drives it entirely through the plugin engine, exercising a lot of the plugin↔host
surface:

* ``ctx.load_local("hide_seek_engine.py")`` — load the bundled engine module
  (its own ``import cv2 / numpy / config / utils.window_locator`` still resolve
  from the shared main-program process — "从主程序获取依赖").
* ``ctx.get_engine("window_locator")`` — reuse the host's shared WindowLocator.
* ``ctx.set_interval`` — the 50 s persistent-alert refresh loop.
* ``ctx.notify`` / ``ctx.dismiss_notify`` — the owner-agnostic alert that works
  on both Entity and WebView.
* ``register_ui_panel`` — start/stop control + live status, shown in the plugin
  panel surface and reachable from the plugin popup menu.

The automation only runs when the user toggles it on (same opt-in behaviour as
the old F11 switch); loading the plugin is cheap (the heavy ``cv2`` import is
deferred until start).
"""

REFRESH_S = 50.0  # re-show the persistent alert before its 60 s auto-hide

_ctx = None
_engine = None
_refresh_token = ""
_engine_class = None


def on_load(ctx):
    global _ctx, _engine, _refresh_token, _engine_class
    _ctx = ctx
    _engine = None
    _refresh_token = ""
    _engine_class = None
    ctx.register_ui_panel(
        "hide_seek",
        {"title": "Auto Hide & Seek",
         "description": "CV 模板匹配自动躲猫猫，启停开关。"},
        render=_render,
        on_action=_on_action,
    )
    # Customizable hotkey — toggles the automation; rebindable in the keybinding
    # editor. Demonstrates the plugin custom-hotkey capability.
    ctx.register_hotkey("toggle", _toggle, default_key="F12", label="自动躲猫猫开关")
    ctx.log("hide_seek_plugin loaded (idle)")


def _toggle():
    if _running():
        _stop("hotkey")
    elif _ctx is not None:
        try:
            _start()
        except Exception as exc:
            _ctx.log(f"hotkey start failed: {exc}")


def on_disable():
    _stop("disabled")


def on_unload():
    _stop("unloaded")


# ── engine plumbing ──────────────────────────────────────────────────────────
def _get_engine_class():
    global _engine_class
    if _engine_class is None:
        # Bundled with the plugin; imports cv2/numpy/config from the host process.
        module = _ctx.load_local("hide_seek_engine.py")
        _engine_class = module.HideSeekEngine
    return _engine_class


def _get_locator():
    locator = _ctx.get_engine("window_locator")  # host's shared _locator if present
    if locator is not None:
        return locator
    from utils.window_locator import WindowLocator  # main-program dependency
    return WindowLocator()


def _running():
    return bool(_engine is not None and getattr(_engine, "running", False))


def _start():
    global _engine, _refresh_token
    if _running():
        return
    cls = _get_engine_class()
    _engine = cls(locator=_get_locator(), on_status=_on_status)
    _engine.start()
    _ctx.notify("AUTO HIDE & SEEK", "自动躲猫猫已启动", duration_s=60.0, kind="hide_seek")
    _refresh_token = _ctx.set_interval(_refresh_alert, REFRESH_S)
    _ctx.request_redraw("hide_seek")
    _ctx.log("hide_seek started")


def _stop(reason=""):
    global _engine, _refresh_token
    if _refresh_token and _ctx:
        _ctx.clear_timer(_refresh_token)
        _refresh_token = ""
    engine = _engine
    _engine = None
    if engine is not None:
        try:
            engine.stop()
        except Exception:
            pass
    if _ctx:
        _ctx.dismiss_notify()
        _ctx.request_redraw("hide_seek")
        _ctx.log(f"hide_seek stopped ({reason})")


def _refresh_alert():
    # Keep the persistent alert visible; NEVER touch the engine thread (a
    # detection phase can idle for minutes — interrupting it would reset it).
    if _ctx and _running():
        _ctx.notify("AUTO HIDE & SEEK", "自动躲猫猫运行中", duration_s=60.0, kind="hide_seek")
        _ctx.request_redraw("hide_seek")


def _on_status(message, step):
    if _ctx and message:
        _ctx.log(f"step={step}: {message}")
        _ctx.request_redraw("hide_seek")


# ── UI panel ─────────────────────────────────────────────────────────────────
def _render(_payload=None):
    ui = _ctx.ui
    running = _running()
    step = int(getattr(_engine, "current_step", 0) or 0) if _engine is not None else 0
    return ui.panel("Auto Hide & Seek", [
        ui.row([
            ui.badge("运行中 RUNNING" if running else "已停止 IDLE", "ok" if running else "muted"),
            ui.kv("当前步骤 Step", str(step)),
        ]),
        ui.text("CV 模板匹配自动点击；模板/locator/识别均从主程序依赖获取。", style="muted"),
        ui.button("停止 Stop" if running else "启动 Start", action="toggle",
                  style="danger" if running else "primary"),
    ])


def _on_action(action_id, _payload=None):
    if action_id == "toggle":
        if _running():
            _stop("panel")
        else:
            try:
                _start()
            except Exception as exc:
                if _ctx:
                    _ctx.log(f"start failed: {exc}")
                    _ctx.notify("AUTO HIDE & SEEK", "启动失败：检查游戏窗口/模板资源",
                                duration_s=4.0, kind="hide_seek")
    return _render()
