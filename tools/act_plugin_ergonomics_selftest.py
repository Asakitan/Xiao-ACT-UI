# -*- coding: utf-8 -*-
"""Regression coverage for ergonomic ACT Python plugin SDK helpers."""

from __future__ import annotations

import json
import os
import tempfile
import unittest

from act_platform.event_bus import EventBus
from act_platform.plugins import PluginManager


PLUGIN_CODE = r'''
events = []


def on_load(ctx):
    ctx.set_defaults({"threshold": 1000, "nested": {"enabled": True}})
    events.append(("setting", ctx.setting("threshold")))
    events.append(("missing", ctx.setting("missing", "fallback")))

    @ctx.on("damage")
    def _decorated_damage(event):
        events.append(("decorated_damage", event["payload"].get("damage")))

    ctx.on_damage(lambda event: events.append(("damage_helper", event["payload"].get("damage"))))
    ctx.on_snapshot(lambda event: events.append(("snapshot", ctx.snapshot_value("live.total_damage", 0))))
    ctx.subscribe_once("boss", lambda event: events.append(("boss_once", event["payload"].get("phase"))))
    token = ctx.subscribe("heal", lambda event: events.append(("heal", event["payload"].get("heal"))))
    events.append(("unsubscribed", ctx.unsubscribe(token)))
    ctx.emit("plugin_custom", {"value": ctx.snapshot_value("live.total_damage", 0)})
'''


class DictSettings:
    def __init__(self) -> None:
        self.data = {}
        self.save_count = 0

    def get(self, key, default=None):
        return self.data.get(key, default)

    def set(self, key, value):
        self.data[key] = value

    def save(self):
        self.save_count += 1


class ActPluginErgonomicsTests(unittest.TestCase):
    def _write_plugin(self, root: str) -> None:
        plugin_dir = os.path.join(root, "ergonomics_demo")
        os.makedirs(plugin_dir, exist_ok=True)
        with open(os.path.join(plugin_dir, "plugin.json"), "w", encoding="utf-8") as fp:
            json.dump({
                "id": "ergonomics_demo",
                "name": "Ergonomics Demo",
                "version": "0.3.0",
                "entry": "plugin.py",
                "enabled": True,
            }, fp, ensure_ascii=False, indent=2)
        with open(os.path.join(plugin_dir, "plugin.py"), "w", encoding="utf-8") as fp:
            fp.write(PLUGIN_CODE)

    def test_plugin_context_ergonomic_helpers(self) -> None:
        bus = EventBus()
        settings = DictSettings()
        with tempfile.TemporaryDirectory(prefix="act_plugin_erg_") as root:
            self._write_plugin(root)
            manager = PluginManager(
                plugin_dirs=[root],
                event_bus=bus,
                settings=settings,
                snapshot_provider=lambda: {"live": {"total_damage": 4321}},
            )
            manager.discover()
            custom_events = []
            bus.subscribe("plugin_custom", custom_events.append, owner_id="test")
            self.assertTrue(manager.load_plugin("ergonomics_demo"), manager.status())

            bus.publish("damage", {"damage": 77}, source_name="test", source_kind="unit")
            bus.publish("heal", {"heal": 88}, source_name="test", source_kind="unit")
            bus.publish("act_snapshot", {"live": {"total_damage": 4321}}, source_name="test", source_kind="unit")
            bus.publish("boss", {"phase": 1}, source_name="test", source_kind="unit")
            bus.publish("boss", {"phase": 2}, source_name="test", source_kind="unit")

            module = manager._records["ergonomics_demo"].module
            events = list(getattr(module, "events", [])) if module else []

        self.assertIn(("setting", 1000), events)
        self.assertIn(("missing", "fallback"), events)
        self.assertIn(("decorated_damage", 77), events)
        self.assertIn(("damage_helper", 77), events)
        self.assertIn(("snapshot", 4321), events)
        self.assertIn(("unsubscribed", True), events)
        self.assertNotIn(("heal", 88), events)
        self.assertEqual([item for item in events if item[0] == "boss_once"], [("boss_once", 1)])
        self.assertEqual(custom_events[0]["payload"]["value"], 4321)
        self.assertGreaterEqual(settings.save_count, 1)


if __name__ == "__main__":
    unittest.main()
