# -*- coding: utf-8 -*-
"""Selftest for shared combat preparse/enrichment facts.

Run from ``sao_auto``:

    python -m tools.combat_preparse_selftest
"""

from __future__ import annotations

import os
import sys
import json
import tempfile
import unittest
from unittest import mock

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
from net import packet_bridge as packet_bridge_module  # noqa: E402


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

    def test_packet_bridge_skill_name_prefers_name_resolver(self) -> None:
        with mock.patch.object(packet_bridge_module._NAME_RESOLVER, "skill", side_effect=lambda skill_id, default="": "统一技能名" if int(skill_id) == 2414 else default):
            packet_bridge_module._SKILL_NAMES = {2414: "本地旧技能名"}
            self.assertEqual(packet_bridge_module._get_skill_name(2414), "统一技能名")

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

    def test_boss_event_label_prefers_name_resolver(self) -> None:
        from tools.tablekit import name_tables

        with mock.patch.object(name_tables.names, "boss_mechanic", side_effect=lambda event_type, default="": "统一机制名" if int(event_type) == 47 else default):
            fact = enrich_boss_event({"event_type": 47, "host_uuid": 123})

        self.assertEqual(fact["boss_mechanic_label"], "统一机制名")

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

    def test_name_table_effect_audit_reports_mapping_health(self) -> None:
        from tools.tablekit.name_table_effect_audit import audit

        with tempfile.TemporaryDirectory() as td:
            full = os.path.join(td, "full.json")
            matched = os.path.join(td, "matched.json")
            cache = os.path.join(td, "cache.json")
            correspondence = os.path.join(td, "corr.json")
            with open(full, "w", encoding="utf-8") as f:
                json.dump({"anchor": {"status": "text_aligned_pointer_table_fallback"}, "rows": [{"index": 0, "text": "神圣壁垒", "string_obj": "0x1"}]}, f, ensure_ascii=False)
            with open(matched, "w", encoding="utf-8") as f:
                json.dump({"rows": [{"text": "神圣壁垒", "confidence": "high", "primary_match": {"id_space": "skill_id", "id": 2414}, "runtime": {"anchor_status": "text_aligned_pointer_table_fallback", "string_obj": "0x1"}}]}, f, ensure_ascii=False)
            with open(cache, "w", encoding="utf-8") as f:
                json.dump({"endpoints": {}, "names": {"by_kind": {"skill": {"2414": {"text": "神圣壁垒", "context": {"allLocalizationString_index": 0}}}}}}, f, ensure_ascii=False)
            with open(correspondence, "w", encoding="utf-8") as f:
                json.dump({"summary": {"tcp_matched_entry_count": 1}}, f, ensure_ascii=False)

            result = audit(full, matched, cache, correspondence)

        self.assertEqual(result["full_string_pool"]["nonempty_text_count"], 1)
        self.assertEqual(result["matched_rows"]["by_id_space"]["skill_id"], 1)
        self.assertEqual(result["tcp_preparse_cache"]["kind_counts"]["skill"], 1)
        self.assertEqual(result["volatile_address_keys"]["cache"], {})
        self.assertEqual(result["volatile_address_keys"]["matched"]["string_obj"], 1)

    def test_name_table_effect_audit_works_without_large_correspondence(self) -> None:
        from tools.tablekit.name_table_effect_audit import audit

        with tempfile.TemporaryDirectory() as td:
            cache = os.path.join(td, "cache.json")
            with open(cache, "w", encoding="utf-8") as f:
                json.dump({"endpoints": {}, "names": {"by_kind": {"boss_mechanic": {"47": {"text": "护盾破裂"}}}}}, f, ensure_ascii=False)

            result = audit(os.path.join(td, "missing_full.json"), os.path.join(td, "missing_matched.json"), cache, os.path.join(td, "missing_corr.json"))

        self.assertEqual(result["tcp_preparse_cache"]["kind_counts"]["boss_mechanic"], 1)
        self.assertEqual(result["correspondence"]["status"], "not_generated_runtime_optional")


if __name__ == "__main__":
    unittest.main(verbosity=2)