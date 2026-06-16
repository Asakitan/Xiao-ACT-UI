# -*- coding: utf-8 -*-
"""Regression coverage for ACT plugin capability metadata."""

from __future__ import annotations

import _bootstrap  # noqa: F401

import json
import os
import tempfile
import unittest

from act_platform.plugins import PluginManager


PLUGIN_CODE = """
def on_load(ctx):
    ctx.log('loaded capability demo')
"""


class ActPluginCapabilityTests(unittest.TestCase):
    def _write_plugin(self, root: str) -> None:
        plugin_dir = os.path.join(root, "capability_demo")
        os.makedirs(plugin_dir, exist_ok=True)
        with open(os.path.join(plugin_dir, "plugin.json"), "w", encoding="utf-8") as fp:
            json.dump({
                "id": "capability_demo",
                "name": "Capability Demo",
                "version": "0.2.0",
                "entry": "plugin.py",
                "enabled": True,
                "capabilities": [
                    "plugin_manager",
                    {"id": "triggers_timers", "title": "Trigger authoring"},
                    {"capability_id": "export", "actions": ["save"]},
                    "bad id with spaces",
                    {"id": ""},
                ],
            }, fp, ensure_ascii=False, indent=2)
        with open(os.path.join(plugin_dir, "plugin.py"), "w", encoding="utf-8") as fp:
            fp.write(PLUGIN_CODE)

    def test_manifest_capabilities_are_exposed_in_status(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_plugin_caps_") as root:
            self._write_plugin(root)
            manager = PluginManager(plugin_dirs=[root])
            manager.discover()
            status = manager.load_all()

        self.assertTrue(status["ok"])
        plugin = status["plugins"][0]
        self.assertEqual(plugin["id"], "capability_demo")
        self.assertEqual(plugin["capability_ids"], [
            "plugin_manager",
            "triggers_timers",
            "export",
        ])
        self.assertEqual(plugin["capabilities"][1]["id"], "triggers_timers")
        self.assertEqual(plugin["capabilities"][1]["title"], "Trigger authoring")
        self.assertEqual(plugin["capabilities"][2]["actions"], ["save"])
        self.assertEqual(status["capabilities"]["plugin_manager"], ["capability_demo"])
        self.assertEqual(status["capabilities"]["triggers_timers"], ["capability_demo"])
        self.assertEqual(status["capabilities"]["export"], ["capability_demo"])


if __name__ == "__main__":
    unittest.main()
