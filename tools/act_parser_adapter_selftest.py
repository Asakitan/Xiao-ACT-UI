# -*- coding: utf-8 -*-
"""Regression coverage for first-party ACT parser adapters."""

from __future__ import annotations

import unittest

from act_platform.adapters import (
    ParserAdapterMetadata,
    StarResonanceParserAdapter,
    built_in_parser_adapters,
    create_builtin_parser_adapter,
)


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


if __name__ == "__main__":
    unittest.main()
