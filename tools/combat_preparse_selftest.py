# -*- coding: utf-8 -*-
"""Selftest for shared combat preparse/enrichment facts.

Run from ``sao_auto``:

    python -m tools.combat_preparse_selftest
"""

from __future__ import annotations

import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from tools.tablekit.combat_preparse import (  # noqa: E402
    enrich_boss_event,
    enrich_dungeon_event,
    enrich_monster_event,
    enrich_skill_event,
)
from engines.auto_key_engine import AutoKeyEngine, normalize_condition  # noqa: E402
from engines.boss_raid_engine import BossRaidEngine, normalize_phase_trigger  # noqa: E402
from engines.combat_analytics import build_act_render_spec  # noqa: E402
from engines.game_state import GameStateManager  # noqa: E402
from net.packet_bridge import PacketBridge  # noqa: E402


class CombatPreparseTests(unittest.TestCase):
    def test_skill_event_resolves_role_and_sub_profession(self) -> None:
        fact = enrich_skill_event({
            "kind": "client_use",
            "skill_level_id": 240601,
            "caster_uid": 36668136,
        })

        self.assertEqual(fact["skill_id"], 2406)
        self.assertEqual(fact["skill_role"], "sub_profession_skill")
        self.assertEqual(fact["profession_id"], 12)
        self.assertEqual(fact["sub_profession"], "光盾")
        self.assertTrue(fact["display_name"])

    def test_dungeon_event_resolves_name_and_target_summary(self) -> None:
        fact = enrich_dungeon_event({
            "kind": "sync_dungeon_data",
            "scene_uuid": 42001,
            "dungeon_difficulty": 3,
            "targets": [{"target_id": 1302101, "nums": 1, "complete": 0}],
        })

        self.assertEqual(fact["dungeon_id"], 42001)
        self.assertEqual(fact["dungeon_difficulty"], 3)
        self.assertEqual(fact["target_count"], 1)
        self.assertIn("display_name", fact)

    def test_boss_event_maps_mechanic_family(self) -> None:
        fact = enrich_boss_event({"event_type": 47, "host_uuid": 123, "buff_uuid": 456})

        self.assertEqual(fact["boss_mechanic_key"], "shield_broken")
        self.assertEqual(fact["boss_mechanic_label"], "护盾破裂")
        self.assertEqual(fact["trigger_family"], "shield")
        self.assertEqual(fact["host_uuid"], 123)

    def test_bossraid_semantic_trigger_advances_phase(self) -> None:
        state = GameStateManager()
        engine = BossRaidEngine(state, settings={})
        profile = {
            "profile_name": "semantic boss",
            "phases": [
                {"name": "P1", "trigger": {"type": "manual", "value": 0}, "timelines": []},
                {"name": "P2", "trigger": {"type": "boss_mechanic", "value": "shield_broken"}, "timelines": []},
            ],
        }

        self.assertEqual(normalize_phase_trigger({"type": "boss_mechanic", "value": "shield_broken"})["value"], "shield_broken")
        engine.start(profile)
        try:
            engine.on_boss_event({"event_type": 47, "host_uuid": 123})
            status = engine.get_status()

            self.assertEqual(status["phase_idx"], 1)
            self.assertEqual(status["last_boss_mechanic_key"], "shield_broken")
            self.assertEqual(state.state.last_boss_event["combat_fact"]["trigger_family"], "shield")
        finally:
            engine.stop()

    def test_monster_event_summarizes_combat_mechanics(self) -> None:
        fact = enrich_monster_event({
            "uuid": 999,
            "template_id": 1301,
            "name": "测试Boss",
            "hp": 50,
            "max_hp": 100,
            "shield_active": True,
            "shield_pct": 0.4,
            "breaking_stage": 0,
            "in_overdrive": True,
            "buff_list": [{"buff_id": 1}],
        })

        self.assertEqual(fact["monster_id"], 1301)
        self.assertEqual(fact["monster_name"], "测试Boss")
        self.assertEqual(fact["hp_pct"], 0.5)
        self.assertEqual(fact["buff_count"], 1)
        self.assertIn("shield", fact["mechanics"])
        self.assertIn("breaking", fact["mechanics"])
        self.assertIn("overdrive", fact["mechanics"])

    def test_autokey_conditions_match_combat_facts(self) -> None:
        state = GameStateManager()
        state.update(
            dungeon_id=42001,
            dungeon_name="测试副本",
            last_skill_event={"skill_id": 2406, "skill_name": "先锋追击", "skill_role": "sub_profession_skill"},
            last_boss_event={"event_type": 47, "boss_mechanic_key": "shield_broken", "trigger_family": "shield"},
        )
        engine = AutoKeyEngine(state, settings={})
        action = {
            "slot_index": 1,
            "conditions": [
                normalize_condition({"type": "dungeon_is", "value": "测试副本"}),
                normalize_condition({"type": "last_skill_is", "value": "2406"}),
                normalize_condition({"type": "boss_mechanic_is", "value": "shield_broken"}),
                normalize_condition({"type": "boss_mechanic_family_is", "value": "shield"}),
            ],
        }

        self.assertTrue(engine._conditions_match(action, state.state, {}))

    def test_packet_bridge_enriches_skill_dungeon_monster_and_boss_events(self) -> None:
        state = GameStateManager()
        seen = {"skill": None, "dungeon": None, "monster": None, "boss": None}
        bridge = PacketBridge(
            state,
            settings={},
            on_skill_event=lambda event: seen.__setitem__("skill", dict(event)),
            on_dungeon_event=lambda event: seen.__setitem__("dungeon", dict(event)),
            on_monster_update=lambda event: seen.__setitem__("monster", dict(event)),
            on_boss_event=lambda event: seen.__setitem__("boss", dict(event)),
        )
        bridge._tcp_name_cache = None
        bridge.tcp_name_cache = None

        bridge._on_parser_skill_event({"kind": "client_use", "skill_level_id": 240601})
        bridge._on_parser_dungeon_event({"kind": "sync_dungeon_data", "scene_uuid": 42001})
        bridge._on_parser_monster_update({"uuid": 1, "template_id": 1301, "name": "测试Boss", "shield_active": True})
        bridge._on_parser_boss_event({"event_type": 47, "host_uuid": 1})

        self.assertEqual(seen["skill"]["skill_id"], 2406)
        self.assertEqual(seen["skill"]["skill_role"], "sub_profession_skill")
        self.assertIn("combat_fact", seen["dungeon"])
        self.assertIn("shield", seen["monster"].get("mechanics") or [])
        self.assertEqual(seen["boss"]["boss_mechanic_key"], "shield_broken")
        self.assertEqual(state.state.last_boss_event["trigger_family"], "shield")

    def test_act_render_spec_exposes_combat_fact_shortcuts(self) -> None:
        spec = build_act_render_spec(
            live={"encounter_active": True, "total_damage": 100, "elapsed_s": 1.0},
            context={
                "dungeon_id": 42001,
                "dungeon_name": "测试副本",
                "last_skill_event": {"combat_fact": {"skill_id": 2406, "skill_name": "先锋追击", "skill_role": "sub_profession_skill"}},
                "last_boss_event": {"combat_fact": {"event_type": 47, "boss_mechanic_key": "shield_broken", "boss_mechanic_label": "护盾破裂", "trigger_family": "shield"}},
            },
        )

        ctx = spec["context"]
        self.assertEqual(ctx["last_skill_id"], 2406)
        self.assertEqual(ctx["last_skill_role"], "sub_profession_skill")
        self.assertEqual(ctx["last_boss_mechanic_key"], "shield_broken")
        self.assertEqual(ctx["last_boss_trigger_family"], "shield")


if __name__ == "__main__":
    unittest.main(verbosity=2)