# -*- coding: utf-8 -*-
"""Render-hook interception example — modify or fully take over a surface.

This is the "截断原渲染" demo.  A render hook receives a surface's payload
*before* the host draws it and may:

* **skin**     — mutate the payload (here: tag the dungeon name + boss),
* **takeover** — set ``payload[OVERRIDE_KEY]`` to a full plugin spec, so the
  host renders the plugin's UI instead of the native one (WebView paints the
  override layer; Entity surfaces that honor overrides do the same).

The same hook is registered for the configured surface; a small UI panel lets
you flip between off / skin / takeover at runtime.  Disabled by default.
"""

from act_platform.render_hooks import OVERRIDE_KEY

_ctx = None
_hook_token = ""


def on_load(ctx):
    global _ctx, _hook_token
    _ctx = ctx
    ctx.set_defaults({"mode": "skin", "surface": "act_aggregate"})
    surface = str(ctx.setting("surface", "act_aggregate"))
    _hook_token = ctx.register_render_hook(surface, _hook, priority=50)
    ctx.register_ui_panel(
        "skin_control",
        {"title": "Render skin control",
         "description": "Toggle how this plugin intercepts the target surface's render."},
        render=_render_control,
        on_action=_on_action,
    )
    ctx.log(f"render_skin loaded; intercepting surface={surface}")


def on_disable():
    if _ctx:
        _ctx.log("render_skin disabled — surface restored")


def on_unload():
    if _ctx:
        _ctx.log("render_skin unloaded")


def _hook(surface, payload):
    """Intercept `surface`'s render payload."""
    mode = str(_ctx.setting("mode", "skin")) if _ctx else "skin"
    if mode == "off":
        return payload
    if mode == "takeover":
        payload[OVERRIDE_KEY] = _ctx.ui.panel("🔌 PLUGIN TAKEOVER", [
            _ctx.ui.text(f"surface '{surface}' is fully rendered by render_skin_plugin",
                         style="warn"),
            _ctx.ui.divider(),
            _ctx.ui.kv("mode", "takeover"),
            _ctx.ui.kv("intercepted at", surface),
            _ctx.ui.text("Switch back to 'skin' or 'off' in the plugin panel to restore the native UI.",
                         style="muted"),
        ])
        return payload
    # skin mode: lightly rewrite visible fields so the interception is obvious.
    try:
        if surface == "act_aggregate" and isinstance(payload.get("overview"), dict):
            ov = payload["overview"]
            name = str(ov.get("dungeon_name") or "live")
            if not name.startswith("🔌"):
                ov["dungeon_name"] = "🔌 " + name
        rs = payload.get("render_spec")
        if isinstance(rs, dict):
            title = str(rs.get("title") or "")
            if not title.startswith("🔌"):
                rs["title"] = "🔌 " + title
    except Exception:
        pass
    return payload


def _render_control(_payload=None):
    mode = str(_ctx.setting("mode", "skin")) if _ctx else "skin"
    surface = str(_ctx.setting("surface", "act_aggregate")) if _ctx else "act_aggregate"
    ui = _ctx.ui
    return ui.panel("Render Skin Control", [
        ui.kv("Target surface", surface),
        ui.kv("Current mode", mode, style=("ok" if mode != "off" else "muted")),
        ui.divider(),
        ui.row([
            ui.button("Off", action="mode:off", style=("primary" if mode == "off" else "default")),
            ui.button("Skin", action="mode:skin", style=("primary" if mode == "skin" else "default")),
            ui.button("Takeover", action="mode:takeover",
                      style=("danger" if mode == "takeover" else "default")),
        ]),
        ui.text("Skin rewrites the payload; Takeover replaces the whole surface with this plugin's spec.",
                style="muted"),
    ])


def _on_action(action_id, _payload=None):
    if _ctx and str(action_id or "").startswith("mode:"):
        _ctx.set_setting("mode", action_id.split(":", 1)[1])
        _ctx.request_redraw(_ctx.setting("surface", "act_aggregate"))
    return _render_control()
