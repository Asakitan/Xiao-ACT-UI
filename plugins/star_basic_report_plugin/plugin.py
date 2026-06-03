# -*- coding: utf-8 -*-
"""Basic ACT report plugin example for Star Resonance.

This bundled example intentionally sticks to the stable PluginContext surface:
settings, decorator/shortcut subscriptions, subscribe_once, explicit
unsubscribe, snapshot reads, and plugin-originated events.
"""

_ctx = None
_damage_token = ""
_warmup_token = ""
_snapshot_count = 0
_big_hit_count = 0


def on_load(ctx):
    global _ctx, _damage_token, _warmup_token, _snapshot_count, _big_hit_count
    _ctx = ctx
    _damage_token = ""
    _warmup_token = ""
    _snapshot_count = 0
    _big_hit_count = 0

    ctx.set_defaults({
        "damage_threshold": 100000,
        "warmup_snapshots": 2,
        "emit_big_hit_events": True,
    })
    ctx.log("star_basic_report_plugin loaded")

    ctx.subscribe_once("encounter_started", _on_first_encounter_started)
    _warmup_token = ctx.subscribe("act_snapshot", _on_warmup_snapshot)
    _damage_token = ctx.on_damage(_on_damage)
    ctx.on_encounter_finalized(_on_encounter_finalized)


def on_enable():
    if _ctx:
        _ctx.log("star_basic_report_plugin enabled")


def on_disable():
    _unsubscribe_runtime_tokens("disabled")
    if _ctx:
        _ctx.log("star_basic_report_plugin disabled")


def on_unload():
    _unsubscribe_runtime_tokens("unloaded")
    if _ctx:
        _ctx.log("star_basic_report_plugin unloaded")


def _unsubscribe_runtime_tokens(reason):
    global _damage_token, _warmup_token
    if not _ctx:
        return
    if _damage_token and _ctx.unsubscribe(_damage_token):
        _ctx.log(f"{reason}: damage listener removed")
    _damage_token = ""
    if _warmup_token and _ctx.unsubscribe(_warmup_token):
        _ctx.log(f"{reason}: warmup snapshot listener removed")
    _warmup_token = ""


def _on_first_encounter_started(event):
    payload = event.get("payload") or {}
    encounter_id = payload.get("encounter_id") or payload.get("id") or "unknown"
    if _ctx:
        _ctx.log(f"first encounter observed: {encounter_id}")


def _on_warmup_snapshot(event):
    global _snapshot_count, _warmup_token
    _snapshot_count += 1
    warmup_limit = _setting_int("warmup_snapshots", 2, minimum=1)
    if _snapshot_count < warmup_limit:
        return
    total_damage = _ctx.snapshot_value("live.total_damage", 0) if _ctx else 0
    encounter_id = _ctx.snapshot_value("encounter.id", "live") if _ctx else "live"
    if _ctx:
        _ctx.log(
            f"warmup complete after {_snapshot_count} snapshots: "
            f"encounter={encounter_id} total_damage={total_damage}"
        )
        if _warmup_token and _ctx.unsubscribe(_warmup_token):
            _ctx.log("warmup snapshot listener removed")
    _warmup_token = ""


def _on_damage(event):
    global _big_hit_count
    payload = event.get("payload") or {}
    damage = _to_int(payload.get("damage") or payload.get("value"))
    threshold = _setting_int("damage_threshold", 100000, minimum=1)
    if damage < threshold:
        return
    _big_hit_count += 1
    actor = payload.get("actor") or payload.get("source") or payload.get("attacker") or "unknown"
    skill = payload.get("skill") or payload.get("skill_name") or payload.get("skill_id") or "unknown"
    if _ctx:
        _ctx.log(f"big hit #{_big_hit_count}: actor={actor} skill={skill} damage={damage}")
        if bool(_ctx.setting("emit_big_hit_events", True)):
            _ctx.emit("plugin_report_summary", {
                "kind": "big_hit",
                "plugin_id": _ctx.plugin_id,
                "actor": actor,
                "skill": skill,
                "damage": damage,
                "threshold": threshold,
                "count": _big_hit_count,
            })


def _on_encounter_finalized(event):
    payload = event.get("payload") or {}
    summary = payload.get("summary") if isinstance(payload.get("summary"), dict) else payload
    encounter_id = summary.get("encounter_id") or summary.get("id") or "unknown"
    duration_s = summary.get("duration_s") or summary.get("duration") or 0
    total_damage = summary.get("total_damage") or summary.get("damage") or 0
    if _ctx:
        _ctx.log(
            f"encounter summary: id={encounter_id} "
            f"duration={duration_s}s damage={total_damage} big_hits={_big_hit_count}"
        )
        _ctx.emit("plugin_report_summary", {
            "kind": "encounter_finalized",
            "plugin_id": _ctx.plugin_id,
            "encounter_id": encounter_id,
            "duration_s": duration_s,
            "total_damage": total_damage,
            "big_hits": _big_hit_count,
        })


def _setting_int(key, default, minimum=0):
    value = _ctx.setting(key, default) if _ctx else default
    try:
        number = int(value)
    except Exception:
        number = int(default)
    return max(int(minimum), number)


def _to_int(value):
    try:
        return int(value)
    except Exception:
        return 0
