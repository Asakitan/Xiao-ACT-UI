# -*- coding: utf-8 -*-
"""Regression coverage for bundled ACT example plugins."""

from __future__ import annotations

import _bootstrap  # noqa: F401

import os
import unittest

from act_platform.event_bus import EventBus
from act_platform.plugins import PluginManager


class ActPluginExamplesTests(unittest.TestCase):
    def test_star_basic_report_plugin_exercises_once_and_unsubscribe(self) -> None:
        plugin_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "plugins"))
        bus = EventBus()
        emitted = []
        bus.subscribe("plugin_report_summary", emitted.append, owner_id="test")
        manager = PluginManager(
            plugin_dirs=[plugin_root],
            event_bus=bus,
            snapshot_provider=lambda: {
                "encounter": {"id": "enc-1"},
                "live": {"total_damage": 250000},
            },
        )
        manager.discover()

        self.assertTrue(manager.load_plugin("star_basic_report_plugin"), manager.status())
        record = manager._records["star_basic_report_plugin"]

        bus.publish("encounter_started", {"encounter_id": "enc-1"}, source_name="test", source_kind="unit")
        bus.publish("encounter_started", {"encounter_id": "enc-2"}, source_name="test", source_kind="unit")
        bus.publish("act_snapshot", {"live": {"total_damage": 10}}, source_name="test", source_kind="unit")
        bus.publish("act_snapshot", {"live": {"total_damage": 20}}, source_name="test", source_kind="unit")
        bus.publish(
            "damage",
            {"damage": 150000, "actor": "Kirito", "skill": "Horizontal Square"},
            source_name="test",
            source_kind="unit",
        )
        bus.publish(
            "encounter_finalized",
            {"summary": {"encounter_id": "enc-1", "duration_s": 12, "total_damage": 250000}},
            source_name="test",
            source_kind="unit",
        )

        logs = "\n".join(record.logs)
        bus_snapshot = bus.snapshot()

        self.assertIn("first encounter observed: enc-1", logs)
        self.assertNotIn("first encounter observed: enc-2", logs)
        self.assertIn("warmup snapshot listener removed", logs)
        self.assertEqual(bus_snapshot["topics"].get("act_snapshot"), 0)
        self.assertEqual([event["payload"]["kind"] for event in emitted], ["big_hit", "encounter_finalized"])
        self.assertEqual(emitted[0]["payload"]["damage"], 150000)
        self.assertEqual(emitted[1]["payload"]["big_hits"], 1)


if __name__ == "__main__":
    unittest.main()
