# -*- coding: utf-8 -*-
"""Regression coverage for first-party ACT parser adapters."""

from __future__ import annotations

import json
import os
import tempfile
import unittest

from act_platform.adapters import (
    ParserAdapterMetadata,
    PluginParserAdapter,
    StarResonanceParserAdapter,
    built_in_parser_adapters,
    create_builtin_parser_adapter,
    create_plugin_parser_adapter,
    plugin_parser_adapters,
)
from act_platform.plugins import PluginManager
from act_platform.runtime import ensure_act_event_bus
from engines.game_state import GameStateManager
from net.packet_bridge import PacketBridge


PLUGIN_PARSER_CODE = r'''
calls = []

def _parser_handler(payload):
    op = payload.get("operation")
    calls.append(op)
    if op == "start":
        return {"started": True}
    if op == "stop":
        return {"stopped": True}
    if op == "parse_packet":
        frame = payload.get("frame") or b""
        return {"frame_len": len(frame), "frame_len_field": payload.get("frame_len")}
    if op == "parse_log_line":
        return {
            "events": [{
                "topic": "damage",
                "payload": {"line": payload.get("line")},
            }]
        }
    if op == "import_file":
        return {"ok": True, "path": payload.get("path"), "events": []}
    if op == "normalize_event":
        return {
            "topic": payload.get("topic"),
            "payload": payload.get("payload") or {},
            "source": {
                "name": "demo_parser",
                "kind": payload.get("source_kind"),
                "game_id": "demo_game",
                "parser_id": "demo_parser",
                "confidence": payload.get("confidence"),
            },
        }
    return {"ignored": op}

def on_load(ctx):
    ctx.register_parser_adapter("demo_parser", {
        "title": "Demo Parser",
        "display_name": "Demo Parser",
        "game_id": "demo_game",
        "supported_locales": ["en-US"],
        "source_kinds": ["packet", "log", "file"],
        "time_budget_ms": 20,
    }, handler=_parser_handler)
'''


LIVE_PACKET_PARSER_CODE = r'''
def _packet_parser(payload):
    op = payload.get("operation")
    if op == "start":
        return {"started": True}
    if op == "stop":
        return {"stopped": True}
    if op == "parse_packet":
        return [
            {
                "topic": "damage",
                "payload": {
                    "damage": payload.get("frame_len"),
                    "attacker": "plugin",
                    "target": "dummy",
                },
            }
        ]
    return {}

def on_load(ctx):
    ctx.register_parser_adapter("demo_live_packet", {
        "title": "Demo Live Packet Parser",
        "display_name": "Demo Live Packet Parser",
        "game_id": "demo_game",
        "source_kinds": ["packet"],
        "time_budget_ms": 20,
    }, handler=_packet_parser)
'''


LIVE_LOG_ONLY_PARSER_CODE = r'''
def _log_parser(payload):
    return {}

def on_load(ctx):
    ctx.register_parser_adapter("demo_log_only", {
        "title": "Demo Log Parser",
        "game_id": "demo_game",
        "source_kinds": ["log"],
    }, handler=_log_parser)
'''


class DictSettings(dict):
    def get(self, key, default=None):
        return super().get(key, default)


def _write_plugin(root: str, plugin_id: str, code: str) -> PluginManager:
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
        fp.write(code)
    manager = PluginManager(plugin_dirs=[root])
    manager.discover()
    return manager


class ActParserAdapterTests(unittest.TestCase):
    def test_built_in_star_adapter_metadata_is_serializable(self) -> None:
        adapters = built_in_parser_adapters()

        self.assertEqual(len(adapters), 1)
        self.assertEqual(adapters[0]["adapter_id"], "star_resonance_tcp")
        self.assertEqual(adapters[0]["game_id"], "star_resonance")
        self.assertEqual(adapters[0]["source_kinds"], ["packet"])
        self.assertEqual(adapters[0]["supported_locales"], ["zh-CN"])

    def test_metadata_from_mapping_accepts_plugin_shape(self) -> None:
        meta = ParserAdapterMetadata.from_mapping({
            "id": "demo_parser",
            "game_id": "demo_game",
            "title": "Demo Parser",
            "locales": ["en-US", "zh-CN"],
            "source_kinds": ["packet", "log"],
            "priority": "7",
        })

        self.assertEqual(meta.adapter_id, "demo_parser")
        self.assertEqual(meta.display_name, "Demo Parser")
        self.assertEqual(meta.supported_locales, ("en-US", "zh-CN"))
        self.assertEqual(meta.source_kinds, ("packet", "log"))
        self.assertEqual(meta.to_dict()["priority"], 7.0)

        fallback = ParserAdapterMetadata.from_mapping({"id": "fallback_parser", "priority": "high"})
        self.assertEqual(fallback.display_name, "fallback_parser")
        self.assertEqual(fallback.priority, 0.0)

    def test_star_adapter_wraps_packet_parser_and_health(self) -> None:
        adapter = create_builtin_parser_adapter("star_resonance_tcp")
        self.assertIsInstance(adapter, StarResonanceParserAdapter)
        parser = adapter.create_parser(on_self_update=lambda player: None, preferred_uid=42)

        adapter.start()
        adapter.parse_packet(b"\x00")
        adapter.set_subscribed_messages({"SyncContainerData"})
        health = adapter.health()

        self.assertIs(parser, adapter.parser)
        self.assertTrue(health["started"])
        self.assertTrue(health["parser_created"])
        self.assertEqual(health["adapter_id"], "star_resonance_tcp")
        self.assertEqual(health["packet_frames"], 1)
        self.assertEqual(parser._subscribed_messages, {"SyncContainerData"})
        self.assertEqual(adapter.get_alive_monsters(), [])
        self.assertEqual(adapter.get_players(), {})

        adapter.stop()
        self.assertFalse(adapter.health()["started"])

    def test_adapter_normalizes_events_with_parser_source(self) -> None:
        adapter = StarResonanceParserAdapter()

        event = adapter.normalize_event("damage", {"damage": 9}, confidence=0.5)

        self.assertEqual(event["topic"], "damage")
        self.assertEqual(event["payload"]["damage"], 9)
        self.assertEqual(event["source"]["name"], "star_resonance_tcp")
        self.assertEqual(event["source"]["game_id"], "star_resonance")
        self.assertEqual(event["source"]["parser_id"], "star_resonance_tcp")
        self.assertEqual(event["source"]["confidence"], 0.5)

    def test_plugin_parser_adapter_invokes_controlled_handler(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_plugin_parser_") as root:
            plugin_dir = os.path.join(root, "parser_demo")
            os.makedirs(plugin_dir, exist_ok=True)
            with open(os.path.join(plugin_dir, "plugin.json"), "w", encoding="utf-8") as fp:
                json.dump({
                    "id": "parser_demo",
                    "name": "Parser Demo",
                    "version": "0.1.0",
                    "entry": "plugin.py",
                    "enabled": True,
                }, fp, ensure_ascii=False, indent=2)
            with open(os.path.join(plugin_dir, "plugin.py"), "w", encoding="utf-8") as fp:
                fp.write(PLUGIN_PARSER_CODE)
            manager = PluginManager(plugin_dirs=[root])
            manager.discover()
            self.assertTrue(manager.load_plugin("parser_demo"), manager.status())

            metadata = plugin_parser_adapters(manager)
            adapter = create_plugin_parser_adapter(manager, "demo_parser")
            self.assertIsInstance(adapter, PluginParserAdapter)
            self.assertEqual(metadata[0]["adapter_id"], "demo_parser")
            self.assertEqual(metadata[0]["plugin_id"], "parser_demo")
            self.assertEqual(adapter.game_id, "demo_game")

            adapter.start()
            packet = adapter.parse_packet(b"abc")
            events = adapter.parse_log_line("hit 123")
            imported = adapter.import_file("sample.log")
            normalized = adapter.normalize_event("damage", {"damage": 33}, source_kind="log", confidence=0.75)
            health = adapter.health()

        self.assertTrue(adapter.started)
        self.assertTrue(packet["ok"], packet)
        self.assertEqual(packet["result"]["frame_len"], 3)
        self.assertEqual(packet["result"]["frame_len_field"], 3)
        self.assertEqual(events[0]["topic"], "damage")
        self.assertEqual(events[0]["payload"]["line"], "hit 123")
        self.assertTrue(imported["ok"], imported)
        self.assertEqual(imported["result"]["path"], "sample.log")
        self.assertEqual(normalized["source"]["parser_id"], "demo_parser")
        self.assertEqual(normalized["source"]["confidence"], 0.75)
        self.assertEqual(health["plugin_id"], "parser_demo")
        self.assertEqual(health["last_invocation"]["operation"], "normalize_event")

    def test_packet_bridge_selects_live_plugin_packet_adapter(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_live_parser_") as root:
            manager = _write_plugin(root, "live_parser_demo", LIVE_PACKET_PARSER_CODE)
            self.assertTrue(manager.load_plugin("live_parser_demo"), manager.status())
            owner = object()
            bus = ensure_act_event_bus(owner)
            seen: list[dict] = []
            bus.subscribe("damage", seen.append)
            bridge = PacketBridge(
                GameStateManager(),
                DictSettings({
                    "act_live_parser_adapter_id": "demo_live_packet",
                    "profession_skill_cache": {"12": {"1": "240130"}},
                }),
                plugin_manager=manager,
                event_bus=bus,
            )

            selected = bridge._create_live_parser_adapter(preferred_uid=0)
            selected.start()
            bridge._process_live_packet_frame(b"abc")
            health = bridge.health()

        self.assertEqual(selected.adapter_id, "demo_live_packet")
        self.assertIsNone(bridge._parser)
        self.assertEqual(len(seen), 1)
        self.assertEqual(seen[0]["schema_version"], 1)
        self.assertTrue(seen[0]["id"])
        self.assertEqual(seen[0]["topic"], "damage")
        self.assertEqual(seen[0]["payload"]["damage"], 3)
        self.assertEqual(seen[0]["source"]["kind"], "packet")
        self.assertEqual(seen[0]["source"]["parser_id"], "demo_live_packet")
        self.assertEqual(health["parser_adapter_selection"]["selected_id"], "demo_live_packet")
        self.assertEqual(health["parser_adapter_selection"]["mode"], "plugin")

    def test_packet_bridge_falls_back_when_live_plugin_is_not_packet_source(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_live_parser_fallback_") as root:
            manager = _write_plugin(root, "log_only_demo", LIVE_LOG_ONLY_PARSER_CODE)
            self.assertTrue(manager.load_plugin("log_only_demo"), manager.status())
            bridge = PacketBridge(
                GameStateManager(),
                DictSettings({"act_live_parser_adapter_id": "demo_log_only"}),
                plugin_manager=manager,
            )

            selected = bridge._create_live_parser_adapter(preferred_uid=0)
            health = bridge.health()

        self.assertIsInstance(selected, StarResonanceParserAdapter)
        self.assertEqual(selected.adapter_id, "star_resonance_tcp")
        self.assertEqual(health["parser_adapter_selection"]["selected_id"], "star_resonance_tcp")
        self.assertIn("does not support packet", health["parser_adapter_selection"]["fallback_reason"])


if __name__ == "__main__":
    unittest.main()
