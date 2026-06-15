# -*- coding: utf-8 -*-
"""Minimal ACT plugin example.

This plugin is intentionally tiny: it demonstrates lifecycle hooks,
subscriptions, and safe logging without depending on game-specific code.
"""

_ctx = None
_snapshot_count = 0


def on_load(ctx):
    global _ctx
    _ctx = ctx
    ctx.set_defaults({"log_every_snapshots": 25})
    ctx.log("hello_act_plugin loaded")

    @ctx.on("act_snapshot")
    def _snapshot(event):
        _on_snapshot(event)

    ctx.on_encounter_finalized(_on_encounter_finalized)


def on_enable():
    if _ctx:
        _ctx.log("hello_act_plugin enabled")


def on_disable():
    if _ctx:
        _ctx.log("hello_act_plugin disabled")


def on_unload():
    if _ctx:
        _ctx.log("hello_act_plugin unloaded")


def _on_snapshot(event):
    global _snapshot_count
    _snapshot_count += 1
    every = int(_ctx.setting("log_every_snapshots", 25) if _ctx else 25)
    every = max(1, every)
    if _snapshot_count == 1 or _snapshot_count % every == 0:
        payload = event.get("payload") or {}
        player = payload.get("player") or payload.get("owner") or {}
        name = player.get("name") or player.get("player_name") or "unknown"
        hp = player.get("hp")
        max_hp = player.get("max_hp")
        if _ctx:
            _ctx.log(f"snapshot #{_snapshot_count}: player={name} hp={hp}/{max_hp}")


def _on_encounter_finalized(event):
    payload = event.get("payload") or {}
    summary = payload.get("summary") or payload
    duration = summary.get("duration_s") or summary.get("duration") or 0
    total_damage = summary.get("total_damage") or summary.get("damage") or 0
    if _ctx:
        _ctx.log(f"encounter finalized: duration={duration}s damage={total_damage}")
