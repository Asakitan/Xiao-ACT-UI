# -*- coding: utf-8 -*-
# Regression coverage for high-freedom ACT plugin engine access.

from __future__ import annotations

import _bootstrap  # noqa: F401

import json
import os
import tempfile
import unittest

from act_platform.plugins import PluginManager


PLUGIN_CODE = r'''
events = []


def on_load(ctx):
    events.append(("owner", ctx.owner is not None))
    events.append(("available", tuple(ctx.engine.available())))
    events.append(("tracker", ctx.get_engine("dps_tracker").name))
    events.append(("history", ctx.require_engine("history_store").name))
    events.append(("call_engine", ctx.call_engine("dps_tracker", "summary")))
    events.append(("owner_attr", ctx.engine.owner_attr("custom_value")))
    events.append(("call_owner", ctx.engine.call_owner("owner_method", "hello")))
    events.append(("runtime", ctx.call_runtime("report_status")["ok"]))
    ctx.engine.set_owner_attr("plugin_touched", True)
'''


class FakeEngine:
    def __init__(self, name: str) -> None:
        self.name = name

    def summary(self) -> str:
        return f"summary:{self.name}"


class FakeOwner:
    def __init__(self) -> None:
        self.custom_value = "owner-custom"
        self._dps_tracker = FakeEngine("tracker")
        self._dps_history_store = FakeEngine("history")
        self._act_trigger_engine = FakeEngine("trigger")

    def owner_method(self, value: str) -> str:
        return f"owner:{value}"


def _write_plugin(root: str) -> None:
    plugin_dir = os.path.join(root, "engine_demo")
    os.makedirs(plugin_dir, exist_ok=True)
    with open(os.path.join(plugin_dir, "plugin.json"), "w", encoding="utf-8") as fp:
        json.dump({
            "id": "engine_demo",
            "name": "Engine Demo",
            "version": "0.1.0",
            "entry": "plugin.py",
            "enabled": True,
            "permissions": ["engine_access"],
        }, fp, ensure_ascii=False, indent=2)
    with open(os.path.join(plugin_dir, "plugin.py"), "w", encoding="utf-8") as fp:
        fp.write(PLUGIN_CODE)


class ActPluginEngineAccessTests(unittest.TestCase):
    def test_plugin_context_exposes_high_freedom_engine_access(self) -> None:
        owner = FakeOwner()
        with tempfile.TemporaryDirectory(prefix="act_plugin_engine_") as root:
            _write_plugin(root)
            manager = PluginManager(
                plugin_dirs=[root],
                owner_provider=lambda: owner,
                snapshot_provider=lambda: {"live": {"total_damage": 1}},
            )
            manager.discover()
            self.assertTrue(manager.load_plugin("engine_demo"), manager.status())
            status = manager.status()
            module = manager._records["engine_demo"].module
            events = list(getattr(module, "events", [])) if module else []

        available = dict(events)["available"]
        self.assertIn("owner", available)
        self.assertIn("dps_tracker", available)
        self.assertIn("history_store", available)
        self.assertIn("trigger_engine", available)
        self.assertIn(("owner", True), events)
        self.assertIn(("tracker", "tracker"), events)
        self.assertIn(("history", "history"), events)
        self.assertIn(("call_engine", "summary:tracker"), events)
        self.assertIn(("owner_attr", "owner-custom"), events)
        self.assertIn(("call_owner", "owner:hello"), events)
        self.assertIn(("runtime", False), events)
        self.assertTrue(owner.plugin_touched)
        self.assertTrue(status["engine_access"]["trusted_in_process"])
        self.assertTrue(status["engine_access"]["owner_available"])
        self.assertTrue(status["engine_access"]["handles"]["dps_tracker"]["available"])


if __name__ == "__main__":
    unittest.main()
