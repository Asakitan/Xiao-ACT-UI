# -*- coding: utf-8 -*-
"""Regression tests for ACT semantic aggregation helpers."""

from __future__ import annotations

import unittest

from engines.act_aggregate import (
    aggregate_damage_by_dungeon,
    aggregate_damage_by_monster,
    aggregate_damage_by_skill,
    build_act_aggregate_summary,
    build_timeline_clusters,
    normalize_act_event_row,
)
from act_platform.event_bus import EventBus
from act_platform.runtime import act_aggregate_status


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


if __name__ == "__main__":
    unittest.main()
