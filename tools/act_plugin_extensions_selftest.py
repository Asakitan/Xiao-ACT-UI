# -*- coding: utf-8 -*-
"""Regression coverage for plugin extension registry helpers."""

from __future__ import annotations

import json
import os
import tempfile
import unittest

from act_platform.plugins import PluginManager


PLUGIN_CODE = r'''
registered = []


def _field_match_handler(payload):
    return {
        "matched": True,
        "message": "plugin field matched",
        "severity": "warn",
    }


def on_load(ctx):
    registered.append(ctx.register_parser_adapter("star_fixture", {
        "title": "Star fixture parser",
        "display_name": "Star fixture parser",
        "game_id": "star_resonance",
        "game_ids": ["star_resonance"],
        "supported_locales": ["zh-CN"],
        "source_kinds": ["fixture", "packet"],
        "priority": 5,
    }))
    registered.append(ctx.register_exporter("summary_json", {
        "title": "Summary JSON",
        "formats": ["json"],
        "payload_fields": ["total_damage"],
    }, handler=lambda report: report))
    registered.append(ctx.register_trigger_type("field_match", {
        "label": "Field match",
        "schema": {"field": "string", "match": "string"},
    }, handler=_field_match_handler))
    registered.append(ctx.register_report_view("compact_report", {
        "route": "plugin://extension_demo/compact",
        "actions": ["open", "copy"],
    }))
    registered.append(ctx.register_timer("burst_window", {
        "duration_s": 12,
        "scope": "encounter",
    }))
'''


class ActPluginExtensionTests(unittest.TestCase):
    def _write_plugin(self, root: str) -> None:
        plugin_dir = os.path.join(root, "extension_demo")
        os.makedirs(plugin_dir, exist_ok=True)
        with open(os.path.join(plugin_dir, "plugin.json"), "w", encoding="utf-8") as fp:
            json.dump({
                "id": "extension_demo",
                "name": "Extension Demo",
                "version": "0.1.0",
                "entry": "plugin.py",
                "enabled": True,
            }, fp, ensure_ascii=False, indent=2)
        with open(os.path.join(plugin_dir, "plugin.py"), "w", encoding="utf-8") as fp:
            fp.write(PLUGIN_CODE)

    def test_plugin_context_registers_extension_metadata(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_plugin_ext_") as root:
            self._write_plugin(root)
            manager = PluginManager(plugin_dirs=[root])
            manager.discover()
            self.assertTrue(manager.load_plugin("extension_demo"), manager.status())
            status = manager.status()
            plugin = status["plugins"][0]

            self.assertEqual(status["extension_counts"], {
                "parser_adapters": 1,
                "exporters": 1,
                "trigger_types": 1,
                "report_views": 1,
                "timers": 1,
            })
            self.assertEqual(plugin["extension_count"], 5)
            self.assertEqual(plugin["extensions"]["parser_adapters"], ["star_fixture"])
            self.assertEqual(status["extensions"]["parser_adapters"][0]["game_id"], "star_resonance")
            self.assertEqual(status["extensions"]["parser_adapters"][0]["supported_locales"], ["zh-CN"])
            self.assertEqual(status["extensions"]["exporters"][0]["id"], "summary_json")
            self.assertEqual(status["extensions"]["exporters"][0]["formats"], ["json"])
            self.assertEqual(status["extensions"]["trigger_types"][0]["schema"]["field"], "string")
            invoked = manager.invoke_extension("trigger_types", "field_match", {
                "rule": {"id": "plugin_field"},
                "render_spec": {"totals": {"damage": 1}},
            })
            self.assertTrue(invoked["ok"], invoked)
            self.assertEqual(invoked["result"]["message"], "plugin field matched")

            self.assertTrue(manager.unload_plugin("extension_demo"))
            cleared = manager.status()
            self.assertEqual(cleared["extension_counts"]["exporters"], 0)
            self.assertEqual(cleared["extensions"]["parser_adapters"], [])

    def test_extension_handler_time_budget_records_failure(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_plugin_ext_slow_") as root:
            plugin_dir = os.path.join(root, "slow_demo")
            os.makedirs(plugin_dir, exist_ok=True)
            with open(os.path.join(plugin_dir, "plugin.json"), "w", encoding="utf-8") as fp:
                json.dump({
                    "id": "slow_demo",
                    "name": "Slow Demo",
                    "version": "0.1.0",
                    "entry": "plugin.py",
                    "enabled": True,
                }, fp, ensure_ascii=False, indent=2)
            with open(os.path.join(plugin_dir, "plugin.py"), "w", encoding="utf-8") as fp:
                fp.write(
                    'import time\n'
                    'def _slow(_payload):\n'
                    '    time.sleep(0.02)\n'
                    '    return True\n'
                    'def on_load(ctx):\n'
                    '    ctx.register_trigger_type("slow_gate", {"time_budget_ms": 1}, handler=_slow)\n'
                )
            manager = PluginManager(plugin_dirs=[root], max_failures=2)
            manager.discover()
            self.assertTrue(manager.load_plugin("slow_demo"), manager.status())

            result = manager.invoke_extension("trigger_types", "slow_gate", {"rule": {}}, time_budget_ms=1)
            status = manager.status()

        self.assertFalse(result["ok"], result)
        self.assertTrue(result["timed_out"])
        plugin = status["plugins"][0]
        self.assertEqual(plugin["failures"], 1)
        self.assertIn("exceeded budget", plugin["last_error"])

    def test_duplicate_extension_id_fails_second_plugin(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_plugin_ext_dupe_") as root:
            for plugin_id in ("first_demo", "second_demo"):
                plugin_dir = os.path.join(root, plugin_id)
                os.makedirs(plugin_dir, exist_ok=True)
                with open(os.path.join(plugin_dir, "plugin.json"), "w", encoding="utf-8") as fp:
                    json.dump({
                        "id": plugin_id,
                        "name": plugin_id,
                        "version": "0.1.0",
                        "entry": "plugin.py",
                        "enabled": True,
                    }, fp, ensure_ascii=False, indent=2)
                with open(os.path.join(plugin_dir, "plugin.py"), "w", encoding="utf-8") as fp:
                    fp.write('def on_load(ctx):\n    ctx.register_exporter("same_export", {"title": ctx.plugin_id})\n')
            manager = PluginManager(plugin_dirs=[root], max_failures=1)
            manager.discover()

            self.assertTrue(manager.load_plugin("first_demo"), manager.status())
            self.assertFalse(manager.load_plugin("second_demo"), manager.status())
            status = manager.status()

        self.assertEqual(status["extension_counts"]["exporters"], 1)
        self.assertEqual(status["extensions"]["exporters"][0]["plugin_id"], "first_demo")
        second = next(plugin for plugin in status["plugins"] if plugin["id"] == "second_demo")
        self.assertIn("extension id already registered", second["last_error"])

    def test_load_failure_clears_partial_extensions(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_plugin_ext_fail_") as root:
            plugin_dir = os.path.join(root, "failing_demo")
            os.makedirs(plugin_dir, exist_ok=True)
            with open(os.path.join(plugin_dir, "plugin.json"), "w", encoding="utf-8") as fp:
                json.dump({
                    "id": "failing_demo",
                    "name": "Failing Demo",
                    "version": "0.1.0",
                    "entry": "plugin.py",
                    "enabled": True,
                }, fp, ensure_ascii=False, indent=2)
            with open(os.path.join(plugin_dir, "plugin.py"), "w", encoding="utf-8") as fp:
                fp.write(
                    'def on_load(ctx):\n'
                    '    ctx.register_exporter("half_loaded", {"title": "Half loaded"})\n'
                    '    raise RuntimeError("boom after extension")\n'
                )
            manager = PluginManager(plugin_dirs=[root], max_failures=1)
            manager.discover()

            self.assertFalse(manager.load_plugin("failing_demo"), manager.status())
            status = manager.status()

        self.assertEqual(status["extension_counts"]["exporters"], 0)
        self.assertEqual(status["extensions"]["exporters"], [])
        plugin = next(plugin for plugin in status["plugins"] if plugin["id"] == "failing_demo")
        self.assertEqual(plugin["extension_count"], 0)
        self.assertIn("boom after extension", plugin["last_error"])


if __name__ == "__main__":
    unittest.main()
