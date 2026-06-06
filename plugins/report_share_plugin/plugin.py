# -*- coding: utf-8 -*-
"""Exporter + formatter example.

* ``register_exporter("markdown_report")`` — turns an encounter payload into a
  Markdown battle report (invokable via ``invoke_extension('exporters', ...)``).
* ``register_formatter("share_line")`` — a compact one-line share string.
* ``register_ui_panel("report_share")`` — previews the latest encounter and
  builds the Markdown report on demand.

Read-only; safe to keep enabled.
"""

_ctx = None
_last_summary = {}


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
    ctx.register_exporter(
        "markdown_report",
        {"title": "Markdown battle report", "formats": ["markdown"],
         "payload_fields": ["render_spec", "last_report"]},
        handler=_export_markdown,
    )
    ctx.register_formatter(
        "share_line",
        {"title": "Share line", "formatter": "text", "payload_fields": ["render_spec"]},
        handler=_format_share_line,
    )
    ctx.register_ui_panel(
        "report_share",
        {"title": "Report share", "description": "Preview and export the latest encounter."},
        render=_render,
        on_action=_on_action,
    )
    ctx.on_encounter_finalized(_on_finalized)
    ctx.log("report_share loaded")


def on_disable():
    if _ctx:
        _ctx.log("report_share disabled")


def _rows_from(payload):
    rs = (payload or {}).get("render_spec") or (_ctx.get_snapshot().get("render_spec") if _ctx else {}) or {}
    return rs, (rs.get("totals") or {}), (rs.get("rows") or [])


def _export_markdown(payload):
    rs, totals, rows = _rows_from(payload)
    title = rs.get("title") or "Encounter"
    lines = [
        f"# {title}",
        "",
        f"- **Damage**: {_fmt(totals.get('damage'))}",
        f"- **DPS**: {_fmt(totals.get('dps'))}",
        f"- **Heal**: {_fmt(totals.get('heal'))}",
        f"- **Elapsed**: {float(totals.get('elapsed_s') or 0):.0f}s",
        "",
        "| # | Player | DPS | Damage | % |",
        "| - | ------ | --- | ------ | - |",
    ]
    for r in sorted(rows, key=lambda x: x.get("damage", 0), reverse=True)[:12]:
        lines.append(
            f"| {r.get('rank', '')} | {r.get('name', '?')} | {_fmt(r.get('dps'))} | "
            f"{_fmt(r.get('damage'))} | {(r.get('damage_pct') or 0) * 100:.0f}% |")
    return {"format": "markdown", "text": "\n".join(lines)}


def _format_share_line(payload):
    rs, totals, rows = _rows_from(payload)
    self_row = next((r for r in rows if r.get("is_self")), (rows[0] if rows else {}))
    return {"text": f"[{rs.get('title') or 'Encounter'}] DPS {_fmt(totals.get('dps'))} · "
                    f"me {_fmt((self_row or {}).get('dps'))} · {len(rows)} players"}


def _on_finalized(event):
    global _last_summary
    payload = event.get("payload") or {}
    _last_summary = payload.get("summary") if isinstance(payload.get("summary"), dict) else payload
    if _ctx:
        _ctx.request_redraw("report_share")


def _render(_payload=None):
    ui = _ctx.ui
    share = _format_share_line(None).get("text", "")
    return ui.panel("Report Share", [
        ui.kv("Latest", share),
        ui.divider(),
        ui.button("生成Markdown战报 Build report", action="build", style="primary"),
        ui.text("注册了 exporter(markdown_report) + formatter(share_line)，可被导出系统调用。", style="muted"),
    ])


def _on_action(action_id, _payload=None):
    ui = _ctx.ui
    if action_id == "build":
        md = _export_markdown(None).get("text", "")
        return ui.panel("Markdown Report", [
            ui.text(md, style="mono"),
            ui.button("返回 Back", action="back"),
        ])
    return _render()
