# -*- coding: utf-8 -*-
"""Static layout regression checks for ACT WebView panels."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
WEB_DIR = ROOT / "web"


def _read_web(name: str) -> str:
    return (WEB_DIR / name).read_text(encoding="utf-8")


def _css_block(source: str, selector: str) -> str:
    match = re.search(rf"{re.escape(selector)}\s*\{{(?P<body>.*?)\}}", source, re.S)
    if not match:
        raise AssertionError(f"missing CSS block: {selector}")
    return match.group("body")


def _px_property(block: str, prop: str) -> int:
    match = re.search(rf"\b{re.escape(prop)}\s*:\s*(\d+)px\b", block)
    if not match:
        raise AssertionError(f"missing px property: {prop}")
    return int(match.group(1))


def _property_value(block: str, prop: str) -> str:
    match = re.search(rf"\b{re.escape(prop)}\s*:\s*([^;]+);", block)
    if not match:
        raise AssertionError(f"missing CSS property: {prop}")
    return match.group(1).strip()


def _assert_graph_points_scroll() -> None:
    html = _read_web("act_graph_timeseries.html")
    table = _css_block(html, ".table")
    max_height = _px_property(table, "max-height")
    overflow = _property_value(table, "overflow")
    rendered_rows = re.search(r"points\.slice\(-(?P<count>\d+)\)", html)
    if not rendered_rows:
        raise AssertionError("graph timeseries must declare rendered point row count")
    row_count = int(rendered_rows.group("count"))
    if row_count < 6:
        raise AssertionError("graph timeseries should keep the latest 6 point rows visible")
    if max_height < 148:
        raise AssertionError(
            f"graph points max-height {max_height}px is too small for {row_count} rows"
        )
    if overflow == "hidden":
        raise AssertionError("graph points table must scroll instead of clipping rows")


def _assert_selector_scroll(file_name: str, selector: str, label: str) -> None:
    html = _read_web(file_name)
    side = _css_block(html, selector)
    overflow = _property_value(side, "overflow")
    if overflow == "hidden":
        raise AssertionError(f"{label} side panel must scroll instead of clipping controls")
    if overflow not in {"auto", "scroll"}:
        raise AssertionError(f"unexpected {label} side overflow: {overflow}")


def _assert_side_scroll(file_name: str, label: str) -> None:
    _assert_selector_scroll(file_name, ".side", label)


def _assert_inline_js_args_are_escaped() -> None:
    combatant = _read_web("act_combatant_drilldown.html")
    graph = _read_web("act_graph_timeseries.html")
    plugin_manager = _read_web("plugin_manager.html")
    trigger_timer = _read_web("trigger_timer_manager.html")
    if "openSkill(\\''+esc(String(sid))+'\\')" in combatant:
        raise AssertionError("combatant skill onclick must not interpolate skill_id through a quoted JS string")
    if "openLog(' + ms + \",\\'\" + (p.topic || '') + \"\\')\"" in graph:
        raise AssertionError("graph point onclick must not interpolate topic through a quoted JS string")
    bad_json_arg = "return JSON.stringify(String(value == null ? '' : value));"
    if bad_json_arg in plugin_manager:
        raise AssertionError("plugin manager jsArg must HTML-escape JSON args before inline onclick")
    if bad_json_arg in trigger_timer:
        raise AssertionError("trigger timer jsArg must HTML-escape JSON args before inline onclick")
    if "function jsArg" not in combatant or "openSkill('+jsArg(sid)+')" not in combatant:
        raise AssertionError("combatant drilldown must encode skill_id with jsArg before wiring onclick")
    if "function jsArg" not in graph or "openLog(' + ms + ',' + jsArg(p.topic || '') + ')" not in graph:
        raise AssertionError("graph timeseries must encode topic with jsArg before wiring onclick")
    safe_json_arg = "return esc(JSON.stringify(String(value == null ? '' : value)));"
    if safe_json_arg not in plugin_manager:
        raise AssertionError("plugin manager must encode plugin ids with escaped jsArg")
    if safe_json_arg not in trigger_timer:
        raise AssertionError("trigger timer must encode rule ids with escaped jsArg")


def _assert_drilldown_numbers_are_clamped() -> None:
    combatant = _read_web("act_combatant_drilldown.html")
    skill = _read_web("act_skill_drilldown.html")
    raw_pct = "function pct(v) { return (Number(v || 0) * 100).toFixed(1) + '%'; }"
    if raw_pct in combatant:
        raise AssertionError("combatant drilldown percentages must clamp invalid/out-of-range values")
    if raw_pct in skill:
        raise AssertionError("skill drilldown percentages must clamp invalid/out-of-range values")
    if "const max = Math.max(1, ...skills.map(x => Number(x.amount || 0)));" in combatant:
        raise AssertionError("combatant drilldown skill max must ignore invalid/negative amounts")
    if "const w = Math.round(Number(sk.amount || 0) / max * 100);" in combatant:
        raise AssertionError("combatant drilldown skill bars must clamp width to 0..100")
    for snippet in (
        "function finiteNum",
        "function clamp01",
        "function barPct",
        "const max = Math.max(1, ...skills.map(x => nonNeg(x.amount)));",
        "const w = barPct(sk.amount, max);",
    ):
        if snippet not in combatant:
            raise AssertionError("missing safe combatant drilldown numeric snippet: " + snippet)
    for snippet in ("function finiteNum", "function clamp01", "function pct(v)"):
        if snippet not in skill:
            raise AssertionError("missing safe skill drilldown numeric snippet: " + snippet)


def _assert_act_dynamic_classes_and_widths_are_safe() -> None:
    timeline = _read_web("act_timeline_vcr.html")
    aggregate = _read_web("act_aggregate.html")
    if "'<article class=\"event ' + esc(ev.topic || '')" in timeline:
        raise AssertionError("timeline VCR must not use raw event topic text as a CSS class")
    if "function eventTopicClass" not in timeline:
        raise AssertionError("timeline VCR must classify event topics through a safe class helper")
    if "'<article class=\"event' + eventTopicClass(ev.topic || '')" not in timeline:
        raise AssertionError("timeline VCR event row must use the safe topic class helper")
    if "(ratio * 100).toFixed(1)" in aggregate:
        raise AssertionError("aggregate bar widths must clamp ratios before rendering CSS width")
    if "function pctWidth" not in aggregate:
        raise AssertionError("aggregate renderer must provide a clamped width helper")
    if "style=\"width:' + pctWidth(ratio) + '%;" not in aggregate:
        raise AssertionError("aggregate bars must render width through pctWidth(ratio)")


def _assert_history_indexes_are_normalized() -> None:
    offline = _read_web("act_offline_import.html")
    report = _read_web("act_report_export.html")
    if "OfflineImport.loadHistory(' + Number(index) + ')" in offline:
        raise AssertionError("offline import history onclick must not pass raw Number(index)")
    if "Number(index || 0)" in offline:
        raise AssertionError("offline import loadHistory must normalize history index before API call")
    if "Number.isFinite(Number(item._history_index)) ? Number(item._history_index) : idx" in report:
        raise AssertionError("report export history index must clamp finite non-negative integers")
    for label, source in (("offline import", offline), ("report export", report)):
        if "function safeHistoryIndex" not in source:
            raise AssertionError(f"{label} must provide safeHistoryIndex")
    if "OfflineImport.loadHistory(' + safeHistoryIndex(index, i) + ')" not in offline:
        raise AssertionError("offline import history onclick must use safeHistoryIndex")
    if "load_history_report', [safeHistoryIndex(index, 0), true]" not in offline:
        raise AssertionError("offline import loadHistory API call must use safeHistoryIndex")
    if "var historyIndex = safeHistoryIndex(item._history_index, idx);" not in report:
        raise AssertionError("report export history rows must use safeHistoryIndex")


def _assert_graph_numbers_and_jump_topic_are_normalized() -> None:
    graph = _read_web("act_graph_timeseries.html")
    raw_number_snippets = (
        "Math.min(...points.map(p => Number(p.time_ms || 0)))",
        "Math.max(...points.map(p => Number(p.time_ms || 0)))",
        "Math.max(1, ...points.map(p => Number(p.value || 0)))",
        "Math.min(0, ...points.map(p => Number(p.value || 0)))",
        "Number(p.time_ms || 0)",
        "Number(p.value || 0)",
        "Number(document.getElementById('zoom').value || 0)",
        "Number(ms) || 0, 'live', ''",
    )
    for snippet in raw_number_snippets:
        if snippet in graph:
            raise AssertionError("graph timeseries must normalize numeric input before rendering/calling APIs: " + snippet)
    for snippet in (
        "function finiteNum",
        "function safeMs",
        "function safeZoom",
        "function normalizedPoints",
        "const points = normalizedPoints(rawPoints);",
        "const t0 = points.length ? Math.min.apply(null, points.map(p => p._time_ms)) : 0;",
        "await callApi('show_action_log_at', 'act.action_log.jump_to_time', safeMs(ms), 'live', '', safeTopic(topic));",
        "topic: String(args[3] || '')",
    ):
        if snippet not in graph:
            raise AssertionError("missing safe graph timeseries snippet: " + snippet)


def _assert_action_log_and_death_recap_numbers_are_normalized() -> None:
    action_log = _read_web("act_action_log.html")
    death_recap = _read_web("act_death_recap.html")
    for snippet in (
        "return isFinite(n) ? n : fallback;",
        "pageOffset = Number(page.offset || cursorState.offset || pageOffset || 0);",
        "[pageLimit, query.value || '', topic.value || '', Number(cursor.value || 0)",
        "[Number(cursor.value || 0), pageLimit",
    ):
        if snippet in action_log:
            raise AssertionError("action log must clamp cursor/offset/limit numeric inputs: " + snippet)
    for snippet in (
        "return isFinite(n) ? n : fallback;",
        "[80, Number(windowInput.value || 8), entity.value || null]",
    ):
        if snippet in death_recap:
            raise AssertionError("death recap must clamp window numeric inputs: " + snippet)
    for snippet in (
        "function nonNegIntArg",
        "function safePageOffset",
        "function safeCursorMs",
        "pageOffset = safePageOffset(page.offset || cursorState.offset || pageOffset || 0);",
        "[pageLimit, query.value || '', topic.value || '', safeCursorMs(cursor.value)",
        "[safeCursorMs(cursor.value), pageLimit",
    ):
        if snippet not in action_log:
            raise AssertionError("missing safe action log numeric snippet: " + snippet)
    for snippet in (
        "function positiveNumberArg",
        "function safeWindowSeconds",
        "[80, safeWindowSeconds(windowInput.value), entity.value || null]",
    ):
        if snippet not in death_recap:
            raise AssertionError("missing safe death recap numeric snippet: " + snippet)


def main() -> int:
    _assert_graph_points_scroll()
    _assert_side_scroll("act_action_log.html", "action log")
    _assert_side_scroll("act_report_export.html", "report export")
    _assert_side_scroll("act_death_recap.html", "death recap")
    _assert_side_scroll("data_source_health.html", "data source health")
    _assert_side_scroll("trigger_timer_manager.html", "trigger timer manager")
    _assert_selector_scroll("plugin_manager.html", ".side-panel", "plugin manager")
    _assert_inline_js_args_are_escaped()
    _assert_drilldown_numbers_are_clamped()
    _assert_act_dynamic_classes_and_widths_are_safe()
    _assert_history_indexes_are_normalized()
    _assert_graph_numbers_and_jump_topic_are_normalized()
    _assert_action_log_and_death_recap_numbers_are_normalized()
    print("OK ACT web layout: graph points and side panels are scrollable")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FAIL ACT web layout selftest: {exc}", file=sys.stderr)
        raise SystemExit(1)
