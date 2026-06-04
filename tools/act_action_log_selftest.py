# -*- coding: utf-8 -*-
"""Regression tests for shared ACT action-log helpers."""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from act_platform import runtime
from act_platform.runtime import ensure_act_event_bus
from engines.dps_history import DpsHistoryStore


class FakeOwner:
    pass


class ActActionLogRuntimeTests(unittest.TestCase):
    def _owner_with_events(self) -> FakeOwner:
        owner = FakeOwner()
        bus = ensure_act_event_bus(owner)
        bus.publish("damage", {"timestamp": 100.0, "attacker": "Kirito", "damage": 1000}, source_name="tcp", source_kind="packet")
        bus.publish("skill", {"timestamp": 101.5, "skill": "Starburst Stream", "target": "Boss"}, source_name="tcp", source_kind="packet")
        bus.publish("boss", {"timestamp": 103.0, "event_type": 101, "message": "Boss phase"}, source_name="trigger", source_kind="runtime")
        return owner

    def test_action_log_status_contains_parity_fields(self) -> None:
        owner = self._owner_with_events()
        status = runtime.act_action_log_status(owner, limit=10)

        self.assertTrue(status["ok"])
        self.assertEqual([row["topic"] for row in status["rows"]], ["boss", "skill", "damage"])
        self.assertEqual(status["columns"][0]["key"], "time_ms")
        self.assertEqual(status["filters"]["query"], "")
        self.assertEqual(status["cursor"]["limit"], 10)
        self.assertEqual(status["analytics"]["total_rows"], 3)
        self.assertIn("topics", status["analytics"]["groups"])
        self.assertIn("encounter_id", status)
        json.dumps(status, ensure_ascii=False)

    def test_action_log_search_and_topic_filter_share_state(self) -> None:
        owner = self._owner_with_events()
        searched = runtime.act_action_log_search(owner, query="Starburst")
        filtered = runtime.act_action_log_filter(owner, topic="skill")
        after = runtime.act_action_log_status(owner)

        self.assertEqual(len(searched["rows"]), 1)
        self.assertEqual(searched["rows"][0]["topic"], "skill")
        self.assertEqual(len(filtered["rows"]), 1)
        self.assertEqual(filtered["filters"]["topic"], "skill")
        self.assertEqual(after["filters"]["topic"], "skill")

    def test_action_log_status_can_clear_filters(self) -> None:
        owner = self._owner_with_events()
        runtime.act_action_log_filter(owner, topic="skill", query="Starburst")

        cleared = runtime.act_action_log_status(owner, topic="", query="")

        self.assertEqual(cleared["filters"]["topic"], "")
        self.assertEqual(cleared["filters"]["query"], "")
        self.assertEqual(len(cleared["rows"]), 3)

    def test_action_log_jump_to_time_marks_nearest_row(self) -> None:
        owner = self._owner_with_events()
        status = runtime.act_action_log_jump_to_time(owner, cursor_ms=101500)

        self.assertEqual(status["cursor"]["time_ms"], 101500)
        self.assertEqual(status["cursor"]["nearest_row_id"], status["rows"][1]["id"])
        self.assertTrue(status["rows"][1]["is_cursor"])

    def test_action_log_copy_returns_json_payload(self) -> None:
        owner = self._owner_with_events()
        copied = runtime.act_action_log_copy(owner, limit=2, query="")
        data = json.loads(copied["text"])

        self.assertTrue(copied["ok"])
        self.assertIn("encounter_id", copied)
        self.assertEqual(len(data["rows"]), 2)
        self.assertIn("columns", data)

    def test_action_log_empty_bus_is_safe(self) -> None:
        status = runtime.act_action_log_status(FakeOwner())

        self.assertTrue(status["ok"])
        self.assertEqual(status["rows"], [])
        self.assertEqual(status["cursor"]["row_count"], 0)
        self.assertEqual(status["analytics"]["total_rows"], 0)

    def test_action_log_history_source_reads_sqlite_actions(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_action_history_") as root:
            store = DpsHistoryStore(
                path=os.path.join(root, "history.json"),
                archive_path=os.path.join(root, "history.jsonl"),
                sqlite_path=os.path.join(root, "history.sqlite3"),
            )
            store.add_report({
                "encounter_id": "enc-history",
                "completed_at": 200.0,
                "completed_local_time": "2026-06-03 12:00:00",
                "report_reason": "unit",
                "encounter_started_at": 100.0,
                "encounter_ended_at": 112.0,
                "elapsed_s": 12.0,
                "total_damage": 321,
                "total_damage_all": 321,
                "total_heal": 0,
                "total_dps": 26,
                "total_hps": 0,
                "entities": [],
                "actions": [{
                    "topic": "damage",
                    "time_ms": 1250,
                    "source_name": "archive",
                    "source_kind": "sqlite",
                    "payload": {
                        "action_type": "damage",
                        "actor_name": "Kirito",
                        "target_name": "Boss",
                        "skill_id": "starburst",
                        "skill_name": "Starburst Stream",
                        "damage": 321,
                    },
                }],
            })
            owner = FakeOwner()
            owner._dps_history_store = store

            status = runtime.act_action_log_status(
                owner,
                source="history",
                encounter_id="enc-history",
                query="Starburst",
                limit=10,
            )

        self.assertTrue(status["ok"])
        self.assertEqual(status["source"], "history")
        self.assertEqual(status["filters"]["encounter_id"], "enc-history")
        self.assertEqual(status["cursor"]["row_count"], 1)
        self.assertEqual(status["rows"][0]["label"], "Starburst Stream")
        self.assertEqual(status["rows"][0]["actor"], "Kirito")
        self.assertEqual(status["rows"][0]["encounter_id"], "enc-history")
        self.assertEqual(status["analytics"]["total_rows"], 1)
        self.assertIn("sqlite", status["storage_status"])

    def test_action_log_history_paginates_and_groups_rows(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_action_history_page_") as root:
            store = DpsHistoryStore(
                path=os.path.join(root, "history.json"),
                archive_path=os.path.join(root, "history.jsonl"),
                sqlite_path=os.path.join(root, "history.sqlite3"),
            )
            store.add_report({
                "encounter_id": "enc-page",
                "completed_at": 200.0,
                "completed_local_time": "2026-06-03 12:00:00",
                "report_reason": "unit",
                "encounter_started_at": 100.0,
                "encounter_ended_at": 112.0,
                "elapsed_s": 12.0,
                "total_damage": 444,
                "total_damage_all": 444,
                "total_heal": 45,
                "total_dps": 37,
                "total_hps": 3,
                "entities": [],
                "actions": [
                    {
                        "topic": "damage",
                        "time_ms": 1000,
                        "source_name": "archive",
                        "source_kind": "sqlite",
                        "payload": {
                            "action_type": "damage",
                            "actor_name": "Kirito",
                            "target_name": "Boss",
                            "skill_name": "Starburst Stream",
                            "damage": 321,
                        },
                    },
                    {
                        "topic": "damage",
                        "time_ms": 2000,
                        "source_name": "archive",
                        "source_kind": "sqlite",
                        "payload": {
                            "action_type": "damage",
                            "actor_name": "Kirito",
                            "target_name": "Boss",
                            "skill_name": "Linear",
                            "damage": 123,
                        },
                    },
                    {
                        "topic": "heal",
                        "time_ms": 3000,
                        "source_name": "archive",
                        "source_kind": "sqlite",
                        "payload": {
                            "action_type": "heal",
                            "actor_name": "Asuna",
                            "target_name": "Kirito",
                            "skill_name": "First Aid",
                            "heal": 45,
                        },
                    },
                ],
            })
            owner = FakeOwner()
            owner._dps_history_store = store

            first = runtime.act_action_log_status(owner, source="history", encounter_id="enc-page", limit=1)
            second = runtime.act_action_log_status(owner, source="history", encounter_id="enc-page", limit=1, offset=1)
            clamped = runtime.act_action_log_status(owner, source="history", encounter_id="enc-page", limit=1, offset=99)
            copied = runtime.act_action_log_copy(owner, limit=1, source="history", encounter_id="enc-page", offset=2)

        self.assertEqual(first["cursor"]["row_count"], 1)
        self.assertEqual(first["cursor"]["total_row_count"], 3)
        self.assertEqual(first["cursor"]["page_index"], 1)
        self.assertEqual(first["cursor"]["page_count"], 3)
        self.assertTrue(first["cursor"]["has_next"])
        self.assertEqual(first["rows"][0]["label"], "Starburst Stream")
        self.assertEqual(second["cursor"]["offset"], 1)
        self.assertTrue(second["cursor"]["has_previous"])
        self.assertEqual(second["rows"][0]["label"], "Linear")
        self.assertEqual(clamped["cursor"]["offset"], 2)
        self.assertEqual(clamped["rows"][0]["label"], "First Aid")
        self.assertEqual(first["analytics"]["total_rows"], 3)
        topics = {item["key"]: item["count"] for item in first["analytics"]["groups"]["topics"]}
        actors = {item["key"]: item["count"] for item in first["analytics"]["groups"]["actors"]}
        self.assertEqual(topics["damage"], 2)
        self.assertEqual(topics["heal"], 1)
        self.assertEqual(actors["Kirito"], 2)
        copied_payload = json.loads(copied["text"])
        self.assertEqual(copied_payload["analytics"]["total_rows"], 3)
        self.assertEqual(copied_payload["cursor"]["offset"], 2)
        self.assertEqual(copied_payload["rows"][0]["label"], "First Aid")

    def test_action_log_groups_same_name_monsters_and_preserves_uids(self) -> None:
        owner = FakeOwner()
        bus = ensure_act_event_bus(owner)
        bus.publish("damage", {
            "timestamp": 100.0,
            "attacker": "Kirito",
            "attacker_uid": 1001,
            "target_uuid": 90001,
            "target_name": "深渊兽",
            "monster_id": 7001,
            "skill_name": "星爆气流斩",
            "skill_id": 1101,
            "dungeon_name": "第七迷宫",
            "dungeon_id": 77,
            "damage": 1200,
        }, source_name="tcp", source_kind="packet")
        bus.publish("damage", {
            "timestamp": 101.0,
            "attacker": "Kirito",
            "attacker_uid": 1001,
            "target_uuid": 90002,
            "target_name": "深渊兽",
            "monster_id": 7001,
            "skill_name": "星爆气流斩",
            "skill_id": 1101,
            "dungeon_name": "第七迷宫",
            "dungeon_id": 77,
            "damage": 3400,
        }, source_name="tcp", source_kind="packet")
        bus.publish("skill", {
            "timestamp": 102.0,
            "actor": "Kirito",
            "actor_uid": 1001,
            "skill_name": "星爆气流斩",
            "skill_id": 1101,
            "dungeon_name": "第七迷宫",
            "dungeon_id": 77,
        }, source_name="tcp", source_kind="packet")
        bus.publish("dungeon", {
            "timestamp": 103.0,
            "dungeon_name": "第七迷宫",
            "dungeon_id": 77,
            "scene_id": 7788,
            "encounter_id": "enc-named-group",
        }, source_name="tcp", source_kind="packet")

        status = runtime.act_action_log_status(owner, limit=10)

        self.assertTrue(status["ok"])
        grouped = {item["key"]: item for item in status["grouped_rows"]}
        monster_group = grouped["monster:深渊兽"]
        self.assertEqual(monster_group["name"], "深渊兽")
        self.assertEqual(monster_group["count"], 2)
        self.assertEqual(monster_group["uid_count"], 2)
        self.assertIn(90001, monster_group["target_uids"])
        self.assertIn(90002, monster_group["target_uids"])
        self.assertIn(77, monster_group["dungeon_ids"])
        self.assertEqual(monster_group["total_value"], 4600.0)
        self.assertEqual([row["target_uid"] for row in monster_group["rows"]], [90002, 90001])
        self.assertEqual(monster_group["rows"][0]["payload"]["dungeon_name"], "第七迷宫")

        skill_group = next(item for item in status["grouped_rows"] if item["kind"] == "actor_skill")
        self.assertEqual(skill_group["name"], "Kirito · 星爆气流斩")
        self.assertIn(1101, skill_group["skill_ids"])
        dungeon_group = next(item for item in status["grouped_rows"] if item["kind"] == "dungeon")
        self.assertEqual(dungeon_group["name"], "第七迷宫")
        self.assertIn(77, dungeon_group["dungeon_ids"])

    def test_action_log_live_monster_update_names_later_damage(self) -> None:
        owner = FakeOwner()
        monster = runtime.enrich_action_log_event({
            "timestamp": 100.0,
            "uuid": 313131072,
            "name": "剑盾哥布林-共鸣",
            "template_id": 3000010,
            "hp": 1000,
            "max_hp": 1000,
        }, owner=owner, topic="monster")
        damage = runtime.enrich_action_log_event({
            "timestamp": 101.0,
            "attacker": "Kirito",
            "target_uuid": 313131072,
            "target_is_monster": True,
            "skill_id": 140301,
            "damage": 471829,
        }, owner=owner, topic="damage")
        bus = ensure_act_event_bus(owner)
        bus.publish("monster", monster, source_name="tcp", source_kind="packet")
        bus.publish("damage", damage, source_name="tcp", source_kind="packet")

        status = runtime.act_action_log_status(owner, limit=10)
        grouped = {item["key"]: item for item in status["grouped_rows"]}
        monster_group = grouped["monster:剑盾哥布林-共鸣"]
        self.assertEqual(monster_group["count"], 2)
        self.assertIn(313131072, monster_group["target_uids"])
        self.assertEqual(monster_group["rows"][0]["target"], "剑盾哥布林-共鸣")
        self.assertEqual(monster_group["rows"][0]["monster_id"], 3000010)

    def test_name_resolver_uses_supplemental_monster_tables(self) -> None:
        from tools.tablekit.name_tables import NameResolver

        resolver = NameResolver()
        self.assertEqual(resolver.monster(210107, default=""), "基础音响")
        self.assertEqual(resolver.monster(3000010, default=""), "剑盾哥布林-共鸣")
        self.assertEqual(resolver.dungeon(6007, default=""), "普通-哥布林巢穴")

    def test_generated_monster_fallback_is_not_name_resolution_evidence(self) -> None:
        owner = FakeOwner()
        bus = ensure_act_event_bus(owner)
        event = runtime.enrich_action_log_event({
            "timestamp": 100.0,
            "target_uuid": 334626880,
            "target_is_monster": True,
            "skill_id": 1550,
            "damage": 123,
        }, owner=owner, topic="damage")
        bus.publish("damage", event, source_name="tcp", source_kind="packet")

        status = runtime.act_action_log_status(owner, limit=10)
        group = status["grouped_rows"][0]

        self.assertEqual(group["kind"], "monster")
        self.assertEqual(group["name"], "怪物#334626880")
        self.assertEqual(group["rows"][0]["monster"], "")
        payload = group["rows"][0].get("payload") or {}
        self.assertEqual(payload.get("target_display"), "怪物#334626880")
        self.assertFalse(payload.get("monster_name"))
        self.assertNotIn("怪物#334626880", json.dumps(payload.get("name_resolution") or [], ensure_ascii=False))

    def test_numeric_target_text_is_not_used_as_monster_name(self) -> None:
        owner = FakeOwner()
        bus = ensure_act_event_bus(owner)
        event = runtime.enrich_action_log_event({
            "timestamp": 100.0,
            "target_uuid": 334626880,
            "target": "334626880",
            "target_is_monster": True,
            "skill_id": 1550,
            "damage": 123,
        }, owner=owner, topic="damage")
        bus.publish("damage", event, source_name="tcp", source_kind="packet")

        status = runtime.act_action_log_status(owner, limit=10)
        group = status["grouped_rows"][0]

        self.assertEqual(group["name"], "怪物#334626880")
        self.assertEqual(group["rows"][0]["monster"], "")
        self.assertEqual(group["rows"][0]["target"], "怪物#334626880")

    def test_dungeon_id_resolves_to_scene_name_in_groups(self) -> None:
        owner = FakeOwner()
        bus = ensure_act_event_bus(owner)
        event = runtime.enrich_action_log_event({
            "timestamp": 100.0,
            "dungeon_id": 6007,
            "scene_id": 6007,
        }, owner=owner, topic="dungeon")
        bus.publish("dungeon", event, source_name="tcp", source_kind="packet")

        status = runtime.act_action_log_status(owner, limit=10)
        group = status["grouped_rows"][0]

        self.assertEqual(group["kind"], "dungeon")
        self.assertEqual(group["name"], "普通-哥布林巢穴")
        self.assertIn(6007, group["dungeon_ids"])
        self.assertEqual(group["rows"][0]["dungeon"], "普通-哥布林巢穴")
        self.assertEqual(group["rows"][0]["payload"].get("dungeon_name"), "普通-哥布林巢穴")


if __name__ == "__main__":
    unittest.main()
