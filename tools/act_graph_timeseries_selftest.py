# -*- coding: utf-8 -*-
"""Regression tests for shared ACT graph/timeseries helpers."""

from __future__ import annotations

import _bootstrap  # noqa: F401

import json
import time
import unittest
from unittest import mock

from act_platform import runtime
from act_platform.runtime import ensure_act_event_bus
from gui_modules.sao_gui_graph_timeseries import GraphTimeseriesPanel


class FakeOwner:
    pass


class FakeVar:
    def __init__(self, value: str = "") -> None:
        self.value = value

    def get(self) -> str:
        return self.value

    def set(self, value: object) -> None:
        self.value = str(value)


class ActGraphTimeseriesRuntimeTests(unittest.TestCase):
    def _owner_with_events(self) -> FakeOwner:
        owner = FakeOwner()
        bus = ensure_act_event_bus(owner)
        bus.publish("damage", {"timestamp": 100.0, "attacker": "Kirito", "damage": 1000}, source_name="tcp", source_kind="packet")
        bus.publish("heal", {"timestamp": 101.0, "name": "Asuna", "heal": 250}, source_name="tcp", source_kind="packet")
        bus.publish("damage", {"timestamp": 102.0, "attacker": "Kirito", "damage_total": 1500}, source_name="tcp", source_kind="packet")
        bus.publish("boss", {"timestamp": 103.0, "message": "Boss HP", "boss_hp_pct": 0.72}, source_name="memory", source_kind="runtime")
        return owner

    def test_graph_status_contains_parity_fields(self) -> None:
        owner = self._owner_with_events()
        status = runtime.act_graph_timeseries_status(owner, metric="damage", limit=20)

        self.assertTrue(status["ok"])
        self.assertEqual(status["selected_metric"], "damage")
        self.assertIn("damage", status["series"])
        self.assertIn("heal", status["series"])
        self.assertIn("event_count", status["series"])
        self.assertGreaterEqual(status["time_range_ms"], 3000)
        self.assertEqual(status["filters"]["query"], "")
        self.assertIn("encounter_id", status)
        json.dumps(status, ensure_ascii=False)

    def test_graph_damage_series_is_cumulative_newest_events_sorted_by_time(self) -> None:
        owner = self._owner_with_events()
        status = runtime.act_graph_timeseries_status(owner, metric="damage", limit=20)
        points = status["series"]["damage"]["points"]

        self.assertEqual([point["value"] for point in points], [1000.0, 1000.0, 2500.0, 2500.0])
        self.assertEqual([point["time_ms"] for point in points], sorted(point["time_ms"] for point in points))

    def test_graph_controls_update_shared_state(self) -> None:
        owner = self._owner_with_events()
        selected = runtime.act_graph_timeseries_select_metric(owner, metric="heal")
        zoomed = runtime.act_graph_timeseries_zoom(owner, time_range_ms=1500)
        filtered = runtime.act_graph_timeseries_filter(owner, topic="damage", query="Kirito")
        after = runtime.act_graph_timeseries_status(owner)

        self.assertEqual(selected["selected_metric"], "heal")
        self.assertEqual(zoomed["time_range_ms"], 1500)
        self.assertEqual(filtered["filters"]["topic"], "damage")
        self.assertEqual(after["filters"]["query"], "Kirito")

    def test_graph_export_returns_json_payload(self) -> None:
        owner = self._owner_with_events()
        exported = runtime.act_graph_timeseries_export(owner, metric="event_count", limit=3)
        data = json.loads(exported["text"])

        self.assertTrue(exported["ok"])
        self.assertEqual(exported["selected_metric"], "event_count")
        self.assertIn("series", data)
        self.assertLessEqual(len(data["series"]["event_count"]["points"]), 3)

    def test_empty_graph_is_safe(self) -> None:
        status = runtime.act_graph_timeseries_status(FakeOwner())

        self.assertTrue(status["ok"])
        self.assertEqual(status["series"]["event_count"]["points"], [])
        self.assertEqual(status["time_range_ms"], 0)

    def test_entity_signature_tracks_rendered_filter_and_status_fields(self) -> None:
        points = [{"time_ms": 1000, "topic": "damage", "value": 100, "row_id": "r1"}]
        base = {
            "time_range_ms": 0,
            "row_count": 1,
            "encounter_id": "live",
            "filters": {"query": "", "topic": ""},
            "errors": [],
        }
        query_changed = dict(base, filters={"query": "boss", "topic": ""})
        rows_changed = dict(base, row_count=2)
        errors_changed = dict(base, errors=["late packet"])

        sig = GraphTimeseriesPanel._series_signature("damage", points, base)

        self.assertNotEqual(sig, GraphTimeseriesPanel._series_signature("damage", points, query_changed))
        self.assertNotEqual(sig, GraphTimeseriesPanel._series_signature("damage", points, rows_changed))
        self.assertNotEqual(sig, GraphTimeseriesPanel._series_signature("damage", points, errors_changed))

    def test_tk_refresh_cache_reuses_only_same_request_parameters(self) -> None:
        panel = GraphTimeseriesPanel.__new__(GraphTimeseriesPanel)
        panel.owner = FakeOwner()
        panel._metric_var = FakeVar("damage")
        panel._query_var = FakeVar("Kirito")
        panel._topic_var = FakeVar("damage")
        panel._zoom_var = FakeVar("1500")
        panel._last_status = {"ok": True, "selected_metric": "damage", "series": {"cached": True}}
        panel._last_refresh_at = time.time()
        panel._last_request_key = ("damage", "Kirito", "damage", 1500)
        rendered: list[dict] = []
        panel._render_status = lambda status: rendered.append(dict(status))

        with mock.patch("gui_modules.sao_gui_graph_timeseries.act_graph_timeseries_status") as status_fn:
            cached = panel.refresh()

        self.assertEqual(cached["series"], {"cached": True})
        self.assertEqual(rendered[-1]["series"], {"cached": True})
        status_fn.assert_not_called()

        panel._metric_var.set("heal")
        status_payload = {"ok": True, "selected_metric": "heal", "series": {"fresh": True}, "filters": {"query": "Kirito", "topic": "damage"}}
        with mock.patch("gui_modules.sao_gui_graph_timeseries.act_graph_timeseries_status", return_value=status_payload) as status_fn:
            refreshed = panel.refresh()

        self.assertEqual(refreshed["selected_metric"], "heal")
        self.assertEqual(panel._last_request_key, ("heal", "Kirito", "damage", 1500))
        status_fn.assert_called_once_with(
            panel.owner,
            metric="heal",
            limit=120,
            query="Kirito",
            topic="damage",
            time_range_ms=1500,
        )


if __name__ == "__main__":
    unittest.main()
