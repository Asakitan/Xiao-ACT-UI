# -*- coding: utf-8 -*-
"""Trusted engine-access plugin example.

This example is intentionally disabled by default because it demonstrates the
high-freedom in-process API. Copy it into user_plugins/ and enable it when you
want to experiment with direct SAO Auto engine handles.
"""

_ctx = None


def on_load(ctx):
    global _ctx
    _ctx = ctx
    ctx.log("engine_access_demo loaded; available=%s" % ",".join(ctx.engine.available()))
    ctx.register_report_view("engine_access_diagnostics", {
        "title": "Engine access diagnostics",
        "description": "Lists trusted in-process engine handles and selected runtime status payloads.",
        "route": "plugin://engine_access_demo_plugin/diagnostics",
        "payload_fields": ["handles", "report_status", "data_source_health"],
    }, handler=_diagnostics)
    ctx.on_snapshot(_on_snapshot)
    ctx.on_encounter_finalized(_on_encounter_finalized)


def on_enable():
    if _ctx:
        _ctx.log("engine_access_demo enabled")


def on_disable():
    if _ctx:
        _ctx.log("engine_access_demo disabled")


def on_unload():
    if _ctx:
        _ctx.log("engine_access_demo unloaded")


def _diagnostics(_payload=None):
    if not _ctx:
        return {"ok": False, "message": "plugin context unavailable"}
    return {
        "ok": True,
        "handles": _ctx.engine.handles(),
        "report_status": _ctx.call_runtime("report_status"),
        "data_source_health": _ctx.call_runtime("data_source_health"),
    }


def _on_snapshot(_event):
    if not _ctx:
        return
    tracker = _ctx.get_engine("dps_tracker")
    history = _ctx.get_engine("history_store")
    trigger_engine = _ctx.get_engine("trigger_engine")
    _ctx.log(
        "snapshot handles: tracker=%s history=%s trigger=%s" % (
            type(tracker).__name__ if tracker else "missing",
            type(history).__name__ if history else "missing",
            type(trigger_engine).__name__ if trigger_engine else "missing",
        )
    )


def _on_encounter_finalized(event):
    if not _ctx:
        return
    payload = event.get("payload") or {}
    report_status = _ctx.call_runtime("report_status")
    _ctx.log(
        "finalized via engine API: encounter=%s report_ok=%s" % (
            payload.get("encounter_id") or payload.get("id") or "unknown",
            report_status.get("ok"),
        )
    )
