# -*- coding: utf-8 -*-
"""Regression tests for ACT semantic aggregation helpers."""

from __future__ import annotations

import _bootstrap  # noqa: F401

import unittest
from unittest import mock

from plugins.star_resonance_plugin.engines.act_aggregate import (
    aggregate_damage_by_dungeon,
    aggregate_damage_by_monster,
    aggregate_damage_by_skill,
    build_act_aggregate_summary,
    build_timeline_clusters,
    normalize_act_event_row,
)
from act_platform.event_bus import EventBus
from act_platform.runtime import act_aggregate_status
from gui_modules.sao_gui_act_aggregate import ActAggregatePanel


class _FakeVar:
    def __init__(self, value: str = "") -> None:
        self.value = value

    def get(self) -> str:
        return self.value

    def set(self, value: object) -> None:
        self.value = str(value)


def _events():
    return [
        {
            "id": "evt-1",
            "time_ms": 100,
            "topic": "damage",
            "source": "packet",
            "actor": "Kirito",
            "target": "Wolf",
            "label": "Slash",
            "value": 1200,
            "payload": {
                "skill_id": 110048200100,
                "base_skill_id": 1004820,
                "skill_name": "Slash",
                "target_uid": 5001,
                "monster_id": 3001,
                "monster_name": "Wolf",
                "dungeon_id": 77,
                "dungeon_name": "North Cave",
                "damage": 1200,
            },
        },
        {
            "id": "evt-2",
            "time_ms": 700,
            "topic": "damage",
            "source": "packet",
            "actor": "Asuna",
            "target": "Wolf",
            "label": "Pierce",
            "value": 800,
            "payload": {
                "skill_id": 2200,
                "skill_name": "Pierce",
                "target_uid": 5001,
                "monster_id": 3001,
                "monster_name": "Wolf",
                "dungeon_id": 77,
                "dungeon_name": "North Cave",
                "damage": 800,
            },
        },
        {
            "id": "evt-3",
            "time_ms": 1700,
            "topic": "damage",
            "source": "memory",
            "actor": "Kirito",
            "target": "Boss",
            "label": "Slash",
            "value": 2400,
            "payload": {
                "skill_id": 110048200100,
                "base_skill_id": 1004820,
                "skill_name": "Slash",
                "target_uid": 9001,
                "monster_id": 900,
                "monster_name": "Boss",
                "dungeon_id": 77,
                "dungeon_name": "North Cave",
                "damage": 2400,
            },
        },
        {
            "id": "evt-4",
            "time_ms": 1900,
            "topic": "heal",
            "source": "packet",
            "actor": "Yui",
            "target": "Kirito",
            "label": "Recover",
            "value": 500,
            "payload": {
                "skill_id": 3300,
                "skill_name": "Recover",
                "dungeon_id": 77,
                "dungeon_name": "North Cave",
                "heal": 500,
            },
        },
    ]


class ActAggregateTests(unittest.TestCase):
    def test_normalize_live_row(self) -> None:
        row = normalize_act_event_row(_events()[0])
        self.assertEqual(row["topic"], "damage")
        self.assertEqual(row["skill_id"], 110048200100)
        self.assertEqual(row["monster_id"], 3001)
        self.assertEqual(row["dungeon_name"], "North Cave")
        self.assertEqual(row["damage"], 1200)

    def test_timeline_clusters_bucket_events(self) -> None:
        clusters = build_timeline_clusters(_events(), window_ms=1000)
        self.assertEqual(len(clusters), 2)
        self.assertEqual(clusters[0]["count"], 2)
        self.assertEqual(clusters[0]["damage"], 2000)
        self.assertEqual(clusters[1]["count"], 2)
        self.assertEqual(clusters[1]["heal"], 500)

    def test_skill_aggregation(self) -> None:
        groups = aggregate_damage_by_skill(_events())
        slash = next(item for item in groups if item["name"] == "Slash")
        self.assertEqual(slash["damage"], 3600)
        self.assertEqual(slash["count"], 2)
        self.assertIn("Kirito", slash["actors"])

    def test_monster_aggregation(self) -> None:
        groups = aggregate_damage_by_monster(_events())
        wolf = next(item for item in groups if item["name"] == "Wolf")
        self.assertEqual(wolf["damage"], 2000)
        self.assertEqual(wolf["count"], 2)
        boss = next(item for item in groups if item["name"] == "Boss")
        self.assertEqual(boss["damage"], 2400)

    def test_dungeon_aggregation(self) -> None:
        groups = aggregate_damage_by_dungeon(_events())
        self.assertEqual(len(groups), 1)
        self.assertEqual(groups[0]["name"], "North Cave")
        self.assertEqual(groups[0]["damage"], 4400)
        self.assertEqual(groups[0]["heal"], 500)

    def test_summary_shapes(self) -> None:
        summary = build_act_aggregate_summary(
            _events(),
            render_spec={"totals": {"damage": 5000, "heal": 700, "elapsed_s": 10}, "context": {"dungeon_name": "North Cave"}, "encounter": {"id": "enc-1"}},
        )
        self.assertEqual(summary["overview"]["damage"], 5000)
        self.assertEqual(summary["overview"]["heal"], 700)
        self.assertEqual(summary["overview"]["dungeon_name"], "North Cave")
        self.assertEqual(summary["raw_counts"]["rows"], 4)
        self.assertEqual(summary["raw_counts"]["skills"], 3)
        self.assertEqual(summary["source_mix"][0]["source"], "packet")
        self.assertEqual({item["name"] for item in summary["log_groups"]}, {"Slash", "Pierce", "Recover"})

    def test_summary_tolerates_bad_row_index(self) -> None:
        summary = build_act_aggregate_summary([
            {
                "id": "evt-bad-index",
                "index": "bad",
                "time_ms": 100,
                "topic": "damage",
                "source": "packet",
                "actor": "Kirito",
                "target": "Boss",
                "label": "Slash",
                "damage": 1200,
                "payload": {"skill_id": 110048200100, "monster_name": "Boss"},
            },
        ])

        self.assertEqual(summary["raw_counts"]["rows"], 1)
        self.assertEqual(summary["overview"]["damage"], 1200)
        self.assertEqual(summary["skill_damage"][0]["count"], 1)

    def test_runtime_aggregate_status_from_event_bus(self) -> None:
        class Owner:
            pass

        owner = Owner()
        owner._act_event_bus = EventBus(max_recent=20)
        for event in _events():
            payload = dict(event["payload"])
            payload["timestamp"] = event["time_ms"] / 1000.0
            owner._act_event_bus.publish(event["topic"], payload, source_name=event["source"], source_kind=event["source"])

        status = act_aggregate_status(owner, limit=20)

        self.assertTrue(status["ok"])
        self.assertEqual(status["raw_counts"]["action_rows"], 4)
        self.assertGreaterEqual(len(status["timeline_clusters"]), 2)
        self.assertGreaterEqual(len(status["skill_damage"]), 3)
        self.assertGreaterEqual(len(status["monster_damage"]), 2)

    def test_entity_signature_tracks_rendered_overview_group_and_graph_fields(self) -> None:
        panel = ActAggregatePanel.__new__(ActAggregatePanel)
        panel._expanded_groups = set()
        panel._expanded_rows = set()
        panel._query_var = _FakeVar("")
        panel._source_var = _FakeVar("live")
        base = {
            "raw_counts": {"rows": 4, "skills": 1, "monsters": 1, "dungeons": 1},
            "overview": {"damage": 1000, "heal": 0, "dps": 500, "hps": 0, "elapsed_s": 2, "dungeon_name": "North Cave", "mode": "live"},
            "source_mix": [{"source": "packet", "count": 3}],
            "group_by": "skill",
            "group_field": "",
            "groups": [{"key": "11", "name": "Slash", "damage": 1000, "count": 2, "duration_ms": 500, "targets": ["Wolf"], "sources": ["packet"]}],
            "graph": {"series": {"damage": {"points": [{"time_ms": 100, "value": 1000}]}}},
        }
        overview_changed = dict(base, overview=dict(base["overview"], dps=600, dungeon_name="South Cave"))
        group_changed = dict(base, groups=[dict(base["groups"][0], name="Slash II", targets=["Boss"])])
        graph_changed = dict(base, graph={"series": {"damage": {"points": [{"time_ms": 100, "value": 1500}]}}})

        sig = panel._signature(base)

        self.assertNotEqual(sig, panel._signature(overview_changed))
        self.assertNotEqual(sig, panel._signature(group_changed))
        self.assertNotEqual(sig, panel._signature(graph_changed))

    def test_refresh_cache_is_scoped_to_request_parameters(self) -> None:
        panel = ActAggregatePanel.__new__(ActAggregatePanel)
        panel.owner = object()
        panel._query_var = _FakeVar("boss")
        panel._source_var = _FakeVar("live")
        panel._group_by_var = _FakeVar("技能")
        panel._group_field_var = _FakeVar("")
        panel._last_status = {"ok": True, "stale": True}
        panel._last_refresh_at = __import__("time").time()
        panel._last_request_key = ("old", "live", "skill", "")
        panel._render_status = lambda _status: None

        with (
            mock.patch("gui_modules.sao_gui_act_aggregate.act_aggregate_status") as aggregate_status,
            mock.patch("gui_modules.sao_gui_act_aggregate.act_graph_timeseries_status", return_value={"ok": True}),
            mock.patch("gui_modules.sao_gui_act_aggregate.act_render_apply_hooks", return_value={"ok": False}),
        ):
            aggregate_status.return_value = {"ok": True, "overview": {}, "raw_counts": {}, "groups": []}
            panel.refresh()

        aggregate_status.assert_called_once()
        self.assertEqual(aggregate_status.call_args.kwargs["query"], "boss")
        self.assertEqual(panel._last_request_key, ("boss", "live", "skill", ""))

    def test_refresh_cache_reuses_same_request(self) -> None:
        panel = ActAggregatePanel.__new__(ActAggregatePanel)
        panel._query_var = _FakeVar("boss")
        panel._source_var = _FakeVar("live")
        panel._group_by_var = _FakeVar("技能")
        panel._group_field_var = _FakeVar("")
        panel._last_status = {"ok": True, "cached": True}
        panel._last_refresh_at = __import__("time").time()
        panel._last_request_key = ("boss", "live", "skill", "")
        rendered = []
        panel._render_status = lambda status: rendered.append(status)

        with mock.patch("gui_modules.sao_gui_act_aggregate.act_aggregate_status") as aggregate_status:
            self.assertEqual(panel.refresh(), {"ok": True, "cached": True})

        aggregate_status.assert_not_called()
        self.assertEqual(rendered, [{"ok": True, "cached": True}])

    def test_tk_render_status_normalizes_malformed_counts(self) -> None:
        panel = ActAggregatePanel.__new__(ActAggregatePanel)
        panel._summary_var = _FakeVar()
        panel._status_var = _FakeVar()
        panel._query_var = _FakeVar("boss")
        panel._rows = None

        panel._render_status({
            "overview": {},
            "raw_counts": {
                "rows": "oops",
                "skills": float("inf"),
                "monsters": float("nan"),
                "dungeons": "bad",
            },
            "errors": [],
            "source": "live",
        })

        self.assertEqual(panel._summary_var.value, "DMG 0 · DPS 0 · EVENTS 0 · GROUPS 0/0/0")

    def test_tk_fmt_normalizes_non_finite_values(self) -> None:
        self.assertEqual(ActAggregatePanel._fmt(float("nan")), "0")
        self.assertEqual(ActAggregatePanel._fmt(float("inf")), "0")
        self.assertEqual(ActAggregatePanel._fmt(float("-inf")), "0")


if __name__ == "__main__":
    unittest.main()
