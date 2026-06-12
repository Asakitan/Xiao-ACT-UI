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


def _assert_plugin_manager_payloads_are_guarded() -> None:
    plugin_manager = _read_web("plugin_manager.html")
    forbidden = (
        "var plugins = Array.isArray(data.plugins) ? data.plugins : [];",
        "var bus = data.event_bus || {};",
        "var logs = Array.isArray(plugin.logs) ? plugin.logs.slice(-5) : [];",
        "esc((plugin.game_ids || []).join(', ') || '-')",
        "var panels = (data && data.panels || []).filter(function (p) { return p.available && !p.hidden; });",
    )
    for snippet in forbidden:
        if snippet in plugin_manager:
            raise AssertionError("plugin manager payload guard missing for: " + snippet)
    required = (
        "function objectValue(value)",
        "function listItems(value)",
        "function objectItems(value)",
        "plugin = objectValue(plugin);",
        "var logs = listItems(plugin.logs).slice(-5);",
        "esc(listItems(plugin.game_ids).join(', ') || '-')",
        "data = objectValue(data);",
        "var plugins = objectItems(data.plugins);",
        "var bus = objectValue(data.event_bus);",
        "var panels = objectItems(objectValue(data).panels).filter(function (p) { return p.available && !p.hidden; });",
    )
    for snippet in required:
        if snippet not in plugin_manager:
            raise AssertionError("missing plugin manager payload guard snippet: " + snippet)


def _assert_drilldown_numbers_are_clamped() -> None:
    combatant = _read_web("act_combatant_drilldown.html")
    skill = _read_web("act_skill_drilldown.html")
    raw_pct = "function pct(v) { return (Number(v || 0) * 100).toFixed(1) + '%'; }"
    if raw_pct in combatant:
        raise AssertionError("combatant drilldown percentages must clamp invalid/out-of-range values")
    if raw_pct in skill:
        raise AssertionError("skill drilldown percentages must clamp invalid/out-of-range values")
    if "limit: args[3] || 80" in skill:
        raise AssertionError("skill drilldown fallback payload must clamp limit before bridge calls")
    if "const max = Math.max(1, ...skills.map(x => Number(x.amount || 0)));" in combatant:
        raise AssertionError("combatant drilldown skill max must ignore invalid/negative amounts")
    if "const w = Math.round(Number(sk.amount || 0) / max * 100);" in combatant:
        raise AssertionError("combatant drilldown skill bars must clamp width to 0..100")
    for snippet in (
        "const s = status.summary || {}; const skills = status.skills || []; const incoming = status.incoming || []; const outgoing = status.outgoing || [];",
        "outgoing.slice(0, 5).forEach(x => sideRows.push",
        "incoming.slice(0, 5).forEach(x => sideRows.push",
    ):
        if snippet in combatant:
            raise AssertionError("combatant drilldown must guard list/object payloads: " + snippet)
    for snippet in (
        "const s = status.summary || {}; const refs = status.timeline_refs || []; const filters = status.filters || {};",
        "refs.length ? refs.map((ref, idx) =>",
        "JSON.stringify(ref.payload || {})",
    ):
        if snippet in skill:
            raise AssertionError("skill drilldown must guard list/object payloads: " + snippet)
    for snippet in (
        "function finiteNum",
        "function clamp01",
        "function barPct",
        "function objectValue(v)",
        "function objectItems(v)",
        "const s = objectValue(status.summary);",
        "const skills = objectItems(status.skills);",
        "const incoming = objectItems(status.incoming);",
        "const outgoing = objectItems(status.outgoing);",
        "const max = Math.max(1, ...skills.map(x => nonNeg(x.amount)));",
        "const w = barPct(sk.amount, max);",
    ):
        if snippet not in combatant:
            raise AssertionError("missing safe combatant drilldown numeric snippet: " + snippet)
    for snippet in (
        "function finiteNum",
        "function safeLimit",
        "limit: safeLimit(args[3])",
        "function clamp01",
        "function pct(v)",
        "function objectValue(v)",
        "function objectItems(v)",
        "const s = objectValue(status.summary);",
        "const refs = objectItems(status.timeline_refs);",
        "const filters = objectValue(status.filters);",
        "const payload = objectValue(ref.payload);",
    ):
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
    for snippet in (
        "history_limit: numericArg(args[0], 20)",
        "limit: numericArg(args[0], 20)",
    ):
        if snippet in offline or snippet in report:
            raise AssertionError("ACT history/report limits must clamp fallback payload values: " + snippet)
    for label, source in (("offline import", offline), ("report export", report)):
        if "function safeHistoryIndex" not in source:
            raise AssertionError(f"{label} must provide safeHistoryIndex")
        if "function safeLimit" not in source:
            raise AssertionError(f"{label} must provide safeLimit")
    if "OfflineImport.loadHistory(' + safeHistoryIndex(index, i) + ')" not in offline:
        raise AssertionError("offline import history onclick must use safeHistoryIndex")
    if "load_history_report', [safeHistoryIndex(index, 0), true]" not in offline:
        raise AssertionError("offline import loadHistory API call must use safeHistoryIndex")
    if "history_limit: safeLimit(args[0], 20, 200)" not in offline:
        raise AssertionError("offline import status payload must clamp history_limit")
    for snippet in (
        "limit: safeLimit(args[0], 20, 200), fmt: String(args[1] || 'json')",
        "limit: safeLimit(args[0], 20, 200), query: String(args[1] || '')",
    ):
        if snippet not in report:
            raise AssertionError("report export payload must clamp limits: " + snippet)
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
        "limit: args[1] || 120",
        "time_range_ms: args[4] || 0",
        "time_range_ms: args[0] || 0, limit: args[1] || 120",
        "limit: args[2] || 120",
        "return String(value || '0');",
        "return (Array.isArray(points) ? points : []).map(p => {",
        "const point = (p && typeof p === 'object') ? p : {};",
    )
    for snippet in raw_number_snippets:
        if snippet in graph:
            raise AssertionError("graph timeseries must normalize numeric input before rendering/calling APIs: " + snippet)
    for snippet in (
        "const rawPoints = (((status.series || {})[metric] || {}).points || []);",
        "const metricDef = (status.metrics || []).find(m => m.id === metric) || { label: metric };",
        "document.getElementById('legend').innerHTML = (status.metrics || []).map(m =>",
        "status = status || {};",
    ):
        if snippet in graph:
            raise AssertionError("graph timeseries must guard list/object payloads: " + snippet)
    for snippet in (
        "function finiteNum",
        "function safeLimit",
        "const raw = value == null || value === '' ? 120 : value;",
        "function safeMs",
        "function safeZoom",
        "function normalizedPoints",
        "function objectValue(value)",
        "function objectItems(value)",
        "const n = finiteNum(value, 0);",
        "return objectItems(points).map(point => {",
        "limit: safeLimit(args[1])",
        "time_range_ms: safeZoom(args[4])",
        "return { time_range_ms: safeZoom(args[0]), limit: safeLimit(args[1]) };",
        "limit: safeLimit(args[2])",
        "status = objectValue(status);",
        "const series = objectValue(status.series);",
        "const metricSeries = objectValue(series[metric]);",
        "const rawPoints = metricSeries.points;",
        "const points = normalizedPoints(rawPoints);",
        "const metrics = objectItems(status.metrics);",
        "const metricDef = metrics.find(m => m.id === metric) || { label: metric };",
        "document.getElementById('legend').innerHTML = metrics.map(m =>",
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
        "var rows = data.rows || [];",
        "var groups = data.grouped_rows || [];",
        "var rows = group.rows || [];",
        "String((data.errors || []).length)",
    ):
        if snippet in action_log:
            raise AssertionError("action log must clamp numeric inputs and guard list payloads: " + snippet)
    for snippet in (
        "return isFinite(n) ? n : fallback;",
        "[80, Number(windowInput.value || 8), entity.value || null]",
        "var rows = data.rows || [];",
        "var summary = data.summary || {};",
        "var death = data.death || {};",
        "var win = data.window || {};",
        "String((data.errors || []).length)",
    ):
        if snippet in death_recap:
            raise AssertionError("death recap must clamp numeric inputs and guard list payloads: " + snippet)
    for snippet in (
        "function nonNegIntArg",
        "function safePageOffset",
        "function safeCursorMs",
        "pageOffset = safePageOffset(page.offset || cursorState.offset || pageOffset || 0);",
        "[pageLimit, query.value || '', topic.value || '', safeCursorMs(cursor.value)",
        "[safeCursorMs(cursor.value), pageLimit",
        "function listItems(value)",
        "function objectItems(value)",
        "var rows = objectItems(data.rows);",
        "var groups = objectItems(data.grouped_rows);",
        "var rows = objectItems(group.rows);",
        "var errorCount = listItems(data.errors).length;",
    ):
        if snippet not in action_log:
            raise AssertionError("missing safe action log numeric/list snippet: " + snippet)
    for snippet in (
        "function positiveNumberArg",
        "function safeWindowSeconds",
        "[80, safeWindowSeconds(windowInput.value), entity.value || null]",
        "function listItems(value)",
        "function objectItems(value)",
        "function objectValue(value)",
        "var rows = objectItems(data.rows);",
        "var summary = objectValue(data.summary);",
        "var death = objectValue(data.death);",
        "var win = objectValue(data.window);",
        "var errorCount = listItems(data.errors).length;",
    ):
        if snippet not in death_recap:
            raise AssertionError("missing safe death recap numeric/list snippet: " + snippet)


def _assert_timeline_speed_is_normalized() -> None:
    timeline = _read_web("act_timeline_vcr.html")
    for snippet in (
        "limit: numericArg(args[0], 80)",
        "delta_ms: numericArg(args[0], 1000)",
        "cursor_ms: numericArg(args[0], 0)",
        "return isFinite(n) ? n : fallback;",
        "speed: numericArg(args[0], 1)",
        "Number(speed.value || 1)",
    ):
        if snippet in timeline:
            raise AssertionError("timeline VCR must clamp speed inputs before API calls: " + snippet)
    for snippet in (
        "function clampIntArg(value, fallback, lo, hi)",
        "function safeLimit(value)",
        "function safeDeltaMs(value)",
        "function safeCursorMs(value)",
        "if (value == null || value === '') return fallback;",
        "limit: safeLimit(args[0])",
        "function safeSpeed",
        "delta_ms: safeDeltaMs(args[0])",
        "cursor_ms: safeCursorMs(args[0])",
        "speed: safeSpeed(args[0])",
        "apiCall('play_timeline', [safeSpeed(speed.value)])",
        "apiCall('set_timeline_speed', [safeSpeed(speed.value)])",
    ):
        if snippet not in timeline:
            raise AssertionError("missing safe timeline speed snippet: " + snippet)


def _assert_timeline_and_aggregate_lists_are_guarded() -> None:
    timeline = _read_web("act_timeline_vcr.html")
    aggregate = _read_web("act_aggregate.html")
    for snippet in (
        "data = data || {};",
        "var events = data.events || [];",
        "String((data.errors || []).length)",
        "JSON.stringify(ev.payload || {})",
    ):
        if snippet in timeline:
            raise AssertionError("timeline VCR must guard list/object payloads: " + snippet)
    for snippet in (
        "var vals = g[p[0]] || [];",
        "(g.sources || []).forEach(function (s)",
        "['来源', (g.sources || []).map(function (s) { return ActText.source(s); })]",
        "(g.rows || []).slice(0, 6).forEach(function (r)",
        "items = (items || []).filter(function (it) { return it && typeof it === 'object'; });",
        "var pts = (item.points || []).map(function (p) { return num(p && p.value); });",
        "html += section(gb, '按 ' + dimLabel + ' 聚合', '', dimAccent, data.groups || [], dimValueKey);",
        "((data.errors || []).length)",
    ):
        if snippet in aggregate:
            raise AssertionError("aggregate dashboard must guard list/object payloads: " + snippet)
    for snippet in (
        "function listItems(value)",
        "function objectItems(value)",
        "function objectValue(value)",
        "data = objectValue(data);",
        "var events = objectItems(data.events);",
        "var errorCount = listItems(data.errors).length;",
        "var payload = objectValue(ev.payload);",
    ):
        if snippet not in timeline:
            raise AssertionError("missing safe timeline list/object snippet: " + snippet)
    for snippet in (
        "function listItems(value)",
        "function objectItems(value)",
        "function objectValue(value)",
        "var vals = listItems(g[p[0]]);",
        "listItems(g.sources).forEach(function (s)",
        "['来源', listItems(g.sources).map(function (s) { return ActText.source(s); })]",
        "objectItems(g.rows).slice(0, 6).forEach(function (r)",
        "items = objectItems(items);",
        "var pts = objectItems(item.points).map(function (p) { return num(p.value); });",
        "var groups = objectItems(data.groups);",
        "var errorCount = listItems(data.errors).length;",
    ):
        if snippet not in aggregate:
            raise AssertionError("missing safe aggregate list/object snippet: " + snippet)
    for snippet in (
        "(counts.rows || 0) + ' 次事件'",
        "String(counts.skills || 0)",
        "String(counts.monsters || 0)",
        "String(counts.dungeons || 0)",
        "(g.count || 0) + 'x'",
        "(graph && graph.row_count || 0)",
        "(s.count || 0)",
        "(it.count || 0)",
    ):
        if snippet in aggregate:
            raise AssertionError("aggregate numeric counts must use finite display guards: " + snippet)
    for snippet in (
        "function countText(value)",
        "countText(counts.rows) + ' 次事件'",
        "countText(counts.skills)",
        "countText(counts.monsters)",
        "countText(counts.dungeons)",
        "countText(g.count) + 'x</span>'",
        "countText(graph && graph.row_count)",
        "countText(s.count)",
        "countText(it.count)",
    ):
        if snippet not in aggregate:
            raise AssertionError("missing safe aggregate count snippet: " + snippet)


def _assert_report_and_offline_lists_are_guarded() -> None:
    report = _read_web("act_report_export.html")
    offline = _read_web("act_offline_import.html")
    for snippet in (
        "rows.map(function (row, idx)",
        "items.slice(0, 12).map(function (item, idx)",
        "payload = payload || {};",
        "var p = payload.preview || {};",
        "var rows = p.top_rows || [];",
        "String((payload.storage_status || {}).count || (payload.history || []).length || 0)",
        "renderHistory(payload.history || []);",
        "(!rows.length && !(payload.history || []).length)",
        "payload.history = historyPayload.encounters || [];",
        "payload.storage_status = historyPayload.storage_status || payload.storage_status || {};",
    ):
        if snippet in report:
            raise AssertionError("report export must guard list/object payloads: " + snippet)
    for snippet in (
        "result = result || {};",
        "var pv = result.preview || {};",
        "(result.report || {}).encounter_id",
        "var rows = (history && history.encounters) || [];",
        "data = data || {};",
        "var last = data.last_result || {};",
        "renderHistory(data.history || {});",
    ):
        if snippet in offline:
            raise AssertionError("offline import must guard list/object payloads: " + snippet)
    for snippet in (
        "function listItems(value)",
        "function objectItems(value)",
        "function objectValue(value)",
        "rows = objectItems(rows);",
        "row = objectValue(row);",
        "items = objectItems(items);",
        "item = objectValue(item);",
        "payload = objectValue(payload);",
        "var p = objectValue(payload.preview);",
        "var rows = objectItems(p.top_rows);",
        "var historyItems = objectItems(payload.history);",
        "var storage = objectValue(payload.storage_status);",
        "document.getElementById('sum-count').textContent = String(storage.count || historyItems.length || 0);",
        "renderHistory(historyItems);",
        "if (!payload.ok && (!rows.length && !historyItems.length))",
        "historyPayload = objectValue(historyPayload);",
        "payload.history = objectItems(historyPayload.encounters);",
        "payload.storage_status = objectValue(historyPayload.storage_status || payload.storage_status);",
    ):
        if snippet not in report:
            raise AssertionError("missing safe report export list/object snippet: " + snippet)
    for snippet in (
        "function listItems(value)",
        "function objectItems(value)",
        "function objectValue(value)",
        "result = objectValue(result);",
        "var pv = objectValue(result.preview);",
        "var report = objectValue(result.report);",
        "var rows = objectItems(objectValue(history).encounters);",
        "item = objectValue(item);",
        "data = objectValue(data);",
        "var last = objectValue(data.last_result);",
        "renderHistory(data.history);",
    ):
        if snippet not in offline:
            raise AssertionError("missing safe offline import list/object snippet: " + snippet)


def _assert_data_source_health_payloads_are_guarded() -> None:
    health = _read_web("data_source_health.html")
    for snippet in (
        "source = source || {};",
        "var watchers = source.watchers || {};",
        "var selection = source.parser_adapter_selection || {};",
        "var self = source.self || {};",
        "var items = payload.diagnostics || [];",
        "var errs = payload.errors || [];",
        "items = errs.map(function (err) { return { level: 'error', message: err }; });",
        "payload = payload || {};",
        "var sources = payload.sources || {};",
        "var summary = sources.summary || {};",
        "String((payload.errors || []).length)",
        "if (sources.packet) cards.push",
        "esc(source.uptime_s || 0) + 's'",
        "(payload.latency_ms || 0) + ' ms'",
        "(payload.last_event_ms || 0) + ' ms'",
    ):
        if snippet in health:
            raise AssertionError("data source health must guard payload shapes: " + snippet)
    for snippet in (
        "function listItems(value)",
        "function isObjectValue(value)",
        "function objectValue(value)",
        "function diagnosticItem(item, level)",
        "source = objectValue(source);",
        "var watchers = objectValue(source.watchers);",
        "var selection = objectValue(source.parser_adapter_selection);",
        "var self = objectValue(source.self);",
        "payload = objectValue(payload);",
        "var items = listItems(payload.diagnostics).map(function (item) { return diagnosticItem(item, 'info'); });",
        "var errs = listItems(payload.errors);",
        "items = errs.map(function (err) { return diagnosticItem(err, 'error'); });",
        "item = diagnosticItem(item, 'info');",
        "var sources = objectValue(payload.sources);",
        "var summary = objectValue(sources.summary);",
        "var errorCount = listItems(payload.errors).length;",
        "if (isObjectValue(sources.packet)) cards.push(renderSource('packet', sources.packet));",
        "function finiteNum(value, fallback)",
        "function metricText(value, suffix)",
        "esc(finiteNum(source.uptime_s, 0)) + 's</span></div>'",
        "metricText(payload.latency_ms, ' ms')",
        "metricText(payload.last_event_ms, ' ms')",
    ):
        if snippet not in health:
            raise AssertionError("missing safe data source health snippet: " + snippet)


def _assert_boss_hp_additional_units_are_safe() -> None:
    boss_hp = _read_web("boss_hp.html")
    for snippet in (
        "var hpPct = (u.hp_pct || 0);",
        "var breakPct = (u.extinction_pct || 1.0);",
        "'<span class=\"name\">' + name + '</span>'",
        "var pct = Math.max(0, Math.min(1, Number(data.hp_pct || 0)));",
        "String(data.hp_source || '') === 'packet' && Number(data.total_hp || 0) > 0",
        "var shieldPct = Math.max(0, Math.min(pct, Number(data.shield_pct || 0)));",
        "var _rawExt = Number(data.extinction || 0);",
        "var _rawMaxExt = Number(data.max_extinction || 0);",
        "var _extPct = Number(data.extinction_pct || 0);",
        "parseInt(data.breaking_stage, 10)",
        "var pct = Math.max(0.05, Math.min(1, Number(lastPct || _lastShieldPct || 0.08)));",
        "wave = Number(wave || 0);",
    ):
        if snippet in boss_hp:
            raise AssertionError("boss HP additional units must sanitize/clamp mini-unit rendering: " + snippet)
    for snippet in (
        "function _unitPct",
        "function _escHtml",
        "var hpPct = _unitPct(u.hp_pct, 0);",
        "var breakPct = _unitPct(u.extinction_pct, 1.0);",
        "'<span class=\"name\">' + _escHtml(name) + '</span>'",
        "function _nonNegNum",
        "function _clampInt",
        "var pct = _unitPct(data.hp_pct, 0);",
        "String(data.hp_source || '') === 'packet' && _nonNegNum(data.total_hp || 0, 0) > 0",
        "var shieldPct = Math.min(pct, _unitPct(data.shield_pct, 0));",
        "var _rawExt = _nonNegNum(data.extinction || 0, 0);",
        "var _rawMaxExt = _nonNegNum(data.max_extinction || 0, 0);",
        "var _extPct = _unitPct(data.extinction_pct, 0);",
        "var stage = (data.breaking_stage != null) ? _clampInt(data.breaking_stage, -1, -1, 999) : -1;",
        "var pct = Math.max(0.05, _unitPct(lastPct || _lastShieldPct || 0.08, 0.08));",
        "wave = _clampInt(wave, 0, 0, 4);",
    ):
        if snippet not in boss_hp:
            raise AssertionError("missing safe boss HP mini-unit snippet: " + snippet)


def _assert_dps_hit_fx_numbers_are_normalized() -> None:
    dps = _read_web("dps.html")
    for snippet in (
        "var seq = Number(fx.seq || 0);",
        "var tsMs = Number(fx.generated_at || 0) * 1000;",
    ):
        if snippet in dps:
            raise AssertionError("DPS hit FX must normalize sequence/timestamp numbers: " + snippet)
    for snippet in (
        "var seq = _nonNegInt(fx.seq, 0);",
        "var tsMs = _nonNegNum(fx.generated_at, 0) * 1000;",
    ):
        if snippet not in dps:
            raise AssertionError("missing safe DPS hit FX numeric snippet: " + snippet)


def _assert_trigger_timer_numbers_are_normalized() -> None:
    trigger_timer = _read_web("trigger_timer_manager.html")
    for snippet in (
        "if (!events || !events.length)",
        "recentList.innerHTML = events.slice(0, 12).map(function (ev) {",
        "[data && data.triggers, data && data.timers].forEach(function (source) {",
        "data = data || {};",
        "var ruleCount = Math.max(Number(data.rule_count != null ? data.rule_count : rules.length) || 0, rules.length);",
        "var timerCount = Number(data.timer_count != null ? data.timer_count : ((data.timers || []).length || 0)) || 0;",
        "esc(rule.threshold)",
        "esc(rule.cooldown_s || 0) + 's'",
        "String(data.last_reload_ms || 0)",
        "String((data.errors || []).length)",
        "renderRecent(data.recent || []);",
        "var count = (data.events || []).length;",
    ):
        if snippet in trigger_timer:
            raise AssertionError("trigger timer manager must normalize visible numeric state: " + snippet)
    for snippet in (
        "function objectValue(value)",
        "function listItems(value)",
        "function objectItems(value)",
        "function finiteNumber(value, fallback)",
        "function clampNumber(value, fallback, lo, hi)",
        "function clampInt(value, fallback, lo, hi)",
        "function numericText(value, fallback, hi)",
        "return String(Math.round(number * 100) / 100);",
        "rule = objectValue(rule);",
        "events = objectItems(events);",
        "[objectItems(data.triggers), objectItems(data.timers)].forEach(function (source) {",
        "data = objectValue(data);",
        "var timerRows = objectItems(data.timers);",
        "var ruleCount = Math.max(clampInt(data.rule_count, rules.length, 0, 999999), rules.length);",
        "var timerCount = clampInt(data.timer_count, timerRows.length, 0, 999999);",
        "var reloadMs = clampInt(data.last_reload_ms, 0, 0, 86400000);",
        "var errorCount = clampInt(listItems(data.errors).length, 0, 0, 999999);",
        "renderRecent(data.recent);",
        "esc(numericText(rule.threshold, 0))",
        "esc(numericText(rule.cooldown_s, 0, 86400)) + 's</span>",
        "var count = Array.isArray(data.events) ? clampInt(data.events.length, 0, 0, 999999) : 0;",
    ):
        if snippet not in trigger_timer:
            raise AssertionError("missing safe trigger timer numeric snippet: " + snippet)


def _assert_act_time_numbers_are_clamped() -> None:
    util = _read_web("act_panel_util.js")
    aggregate = _read_web("act_aggregate.html")
    for snippet in (
        "var ms = parseInt(timeMs, 10);",
        "var t = Math.max(0, parseFloat(ms) || 0) / 1000;",
        "var base = parseInt(baseMs, 10) || 0;",
        "var delta = (parseInt(timeMs, 10) || 0) - base;",
        "var d = parseInt(deltaMs, 10) || 0;",
    ):
        if snippet in util:
            raise AssertionError("ACT shared time helper must clamp finite numeric inputs: " + snippet)
    for snippet in (
        "function finiteNumber(value, fallback)",
        "function clampNumber(value, fallback, lo, hi)",
        "function clampInt(value, fallback, lo, hi)",
        "var ms = clampInt(timeMs, 0, 0, 8640000000000000);",
        "if (!isFinite(d.getTime())) return '--';",
        "var t = clampNumber(ms, 0, 0, Number.MAX_SAFE_INTEGER) / 1000;",
        "var base = clampInt(baseMs, 0, 0, 8640000000000000);",
        "var delta = clampInt(timeMs, 0, 0, 8640000000000000) - base;",
        "var d = clampInt(deltaMs, 0, -Number.MAX_SAFE_INTEGER, Number.MAX_SAFE_INTEGER);",
    ):
        if snippet not in util:
            raise AssertionError("missing safe ACT shared time numeric snippet: " + snippet)
    bad_span = "var span = parseInt(g.duration_ms || (Math.max(0, (g.last_time_ms || 0) - (g.first_time_ms || 0))), 10) || 0;"
    if bad_span in aggregate:
        raise AssertionError("aggregate group duration must not parse raw duration/time span")
    for snippet in (
        "var span = Math.max(0, num(g.duration_ms));",
        "if (!span) span = Math.max(0, num(g.last_time_ms) - num(g.first_time_ms));",
        "span = Math.min(span, 86400000);",
    ):
        if snippet not in aggregate:
            raise AssertionError("missing safe aggregate duration snippet: " + snippet)


def main() -> int:
    _assert_graph_points_scroll()
    _assert_side_scroll("act_action_log.html", "action log")
    _assert_side_scroll("act_report_export.html", "report export")
    _assert_side_scroll("act_death_recap.html", "death recap")
    _assert_side_scroll("data_source_health.html", "data source health")
    _assert_side_scroll("trigger_timer_manager.html", "trigger timer manager")
    _assert_selector_scroll("plugin_manager.html", ".side-panel", "plugin manager")
    _assert_inline_js_args_are_escaped()
    _assert_plugin_manager_payloads_are_guarded()
    _assert_drilldown_numbers_are_clamped()
    _assert_act_dynamic_classes_and_widths_are_safe()
    _assert_history_indexes_are_normalized()
    _assert_graph_numbers_and_jump_topic_are_normalized()
    _assert_action_log_and_death_recap_numbers_are_normalized()
    _assert_timeline_speed_is_normalized()
    _assert_timeline_and_aggregate_lists_are_guarded()
    _assert_report_and_offline_lists_are_guarded()
    _assert_data_source_health_payloads_are_guarded()
    _assert_boss_hp_additional_units_are_safe()
    _assert_dps_hit_fx_numbers_are_normalized()
    _assert_trigger_timer_numbers_are_normalized()
    _assert_act_time_numbers_are_clamped()
    print("OK ACT web layout: graph points and side panels are scrollable")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FAIL ACT web layout selftest: {exc}", file=sys.stderr)
        raise SystemExit(1)
