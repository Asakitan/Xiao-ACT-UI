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
    if "openSkill(\\''+esc(String(sid))+'\\')" in combatant:
        raise AssertionError("combatant skill onclick must not interpolate skill_id through a quoted JS string")
    if "openLog(' + ms + \",\\'\" + (p.topic || '') + \"\\')\"" in graph:
        raise AssertionError("graph point onclick must not interpolate topic through a quoted JS string")
    if "function jsArg" not in combatant or "openSkill('+jsArg(sid)+')" not in combatant:
        raise AssertionError("combatant drilldown must encode skill_id with jsArg before wiring onclick")
    if "function jsArg" not in graph or "openLog(' + ms + ',' + jsArg(p.topic || '') + ')" not in graph:
        raise AssertionError("graph timeseries must encode topic with jsArg before wiring onclick")


def main() -> int:
    _assert_graph_points_scroll()
    _assert_side_scroll("act_action_log.html", "action log")
    _assert_side_scroll("act_report_export.html", "report export")
    _assert_side_scroll("act_death_recap.html", "death recap")
    _assert_side_scroll("data_source_health.html", "data source health")
    _assert_side_scroll("trigger_timer_manager.html", "trigger timer manager")
    _assert_selector_scroll("plugin_manager.html", ".side-panel", "plugin manager")
    _assert_inline_js_args_are_escaped()
    print("OK ACT web layout: graph points and side panels are scrollable")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FAIL ACT web layout selftest: {exc}", file=sys.stderr)
        raise SystemExit(1)
