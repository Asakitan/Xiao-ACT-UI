# -*- coding: utf-8 -*-
"""Contract tests for net.tcp_name_cache."""
from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from net.tcp_name_cache import TcpNameCache, build_index_from_live_rows


class TcpNameCacheTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="tcp-name-cache-")
        self.path = os.path.join(self.tmp, "cache.json")

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_endpoint_and_name_persist_without_raw_payload(self) -> None:
        cache = TcpNameCache(self.path, autosave_interval_s=0)
        cache.observe_endpoint("01020304:1234", source="tcp")
        cache.observe_name("skill", 1522, "滋养", source="tcp", confidence="high", endpoint="01020304:1234")
        cache.observe_name("boss_mechanic", 47, "护盾破裂", source="tcp", confidence="high", endpoint="01020304:1234")
        cache.observe_name("boss", 1301, "测试Boss", source="tcp", confidence="high", endpoint="01020304:1234")
        cache.save(force=True)

        with open(self.path, "r", encoding="utf-8") as f:
            data = json.load(f)
        endpoint = data["endpoints"]["01020304:1234"]
        self.assertEqual(endpoint["ip"], "1.2.3.4")
        self.assertEqual(endpoint["port"], 1234)
        self.assertEqual(data["names"]["by_kind"]["skill"]["1522"]["text"], "滋养")
        self.assertEqual(data["names"]["by_kind"]["boss_mechanic"]["47"]["text"], "护盾破裂")
        self.assertEqual(data["names"]["by_kind"]["boss"]["1301"]["text"], "测试Boss")
        dumped = json.dumps(data, ensure_ascii=False)
        self.assertNotIn("raw_payload", dumped)
        self.assertNotIn("packet_bytes", dumped)

    def test_fallback_labels_are_not_recorded_as_names(self) -> None:
        cache = TcpNameCache(self.path, autosave_interval_s=0)
        cache.observe_name("monster", 11008, "怪物#11008", source="tcp", confidence="high")
        cache.observe_name("monster_skill", 1004820, "怪物技能#1004820", source="tcp", confidence="high")
        cache.observe_name("environment_skill", 1006507, "环境技能#1006507", source="tcp", confidence="high")
        cache.observe_name("player_skill", 1201, "玩家技能#1201", source="tcp", confidence="high")
        cache.observe_name("ultimate_skill", 1713, "幻想技能#1713", source="tcp", confidence="high")
        cache.observe_name("profession_skill_buff", 55302, "职业技能Buff#55302", source="tcp", confidence="high")
        self.assertNotIn("monster", cache.snapshot()["names"]["by_kind"])
        self.assertNotIn("monster_skill", cache.snapshot()["names"]["by_kind"])
        self.assertNotIn("environment_skill", cache.snapshot()["names"]["by_kind"])
        self.assertNotIn("player_skill", cache.snapshot()["names"]["by_kind"])
        self.assertNotIn("ultimate_skill", cache.snapshot()["names"]["by_kind"])
        self.assertNotIn("profession_skill_buff", cache.snapshot()["names"]["by_kind"])

    def test_build_index_from_live_rows_filters_confidence_and_id_space(self) -> None:
        index = build_index_from_live_rows({
            "rows": [
                {
                    "text": "神圣壁垒",
                    "confidence": "high",
                    "primary_match": {"id_space": "skill_id", "id": 2414, "source": "tcp_alias"},
                    "runtime": {
                        "string_obj": "0x111",
                        "table_element_base": "0x222",
                        "allLocalizationString_index": 123,
                        "anchor_status": "text_aligned_pointer_table_fallback",
                    },
                },
                {
                    "text": "低可信技能",
                    "confidence": "low",
                    "primary_match": {"id_space": "skill_id", "id": 9999, "source": "tcp_alias"},
                },
                {
                    "text": "未知空间",
                    "confidence": "high",
                    "primary_match": {"id_space": "unknown", "id": 1},
                },
            ]
        })
        by_kind = index["names"]["by_kind"]
        entry = by_kind["skill"].get("2414") or by_kind["profession_skill"]["2414"]
        self.assertEqual(entry["text"], "神圣壁垒")
        dumped = json.dumps(entry, ensure_ascii=False)
        self.assertNotIn("string_obj", dumped)
        self.assertNotIn("table_element_base", dumped)
        self.assertEqual(entry["context"]["allLocalizationString_index"], 123)
        self.assertEqual(entry["context"]["anchor_status"], "text_aligned_pointer_table_fallback")
        self.assertNotIn("9999", by_kind["skill"])
        self.assertNotIn("unknown", by_kind)

    def test_mem_snapshot_updates_player_cache(self) -> None:
        cache = TcpNameCache(self.path, autosave_interval_s=0)
        cache.apply_mem_self_snapshot({
            "uid": 36668136,
            "char_name": "咲",
            "level_base": 60,
            "cur_hp": 244207,
            "max_hp": 244207,
            "profession_id": 5,
            "fight_point": 53406,
            "skill_cds": [{"skill_level_id": 152230, "name": "滋养"}],
        }, endpoint="01020304:1234")
        player = cache.snapshot()["names"]["by_kind"]["player"]["36668136"]
        self.assertEqual(player["name"], "咲")
        self.assertEqual(player["level"], 60)
        self.assertEqual(player["hp"], 244207)
        self.assertEqual(player["profession_id"], 5)
        self.assertEqual(player["profession_name"], "森语者")
        self.assertEqual(player["fight_point"], 53406)
        self.assertEqual(player["skills"][0]["skill_level_id"], 152230)
        self.assertNotIn("skill", cache.snapshot()["names"]["by_kind"])

    def test_dict_skill_cache_records_keys_not_timestamp_values(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = os.path.join(td, "cache.json")
            cache = TcpNameCache(path, autosave_interval_s=0)
            cache.observe_player(
                36668136,
                name="咲",
                skills={152230: {"remaining_ms": 1234}, 2414: 1700000000.0},
                endpoint="01020304:1234",
            )
            skills = cache.snapshot()["names"]["by_kind"]["player"]["36668136"]["skills"]
        level_ids = {int(row.get("skill_level_id") or 0) for row in skills}
        self.assertIn(152230, level_ids)
        self.assertIn(2414, level_ids)
        self.assertNotIn(1700000000, level_ids)

    def test_name_resolver_reads_tcp_preparse_cache(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = os.path.join(td, "cache.json")
            with open(path, "w", encoding="utf-8") as f:
                json.dump({
                    "names": {
                        "by_kind": {
                            "skill": {"2414": {"text": "神圣壁垒"}},
                            "player": {"36668136": {"text": "咲"}},
                        }
                    }
                }, f, ensure_ascii=False)
            from tools.tablekit import name_tables
            with mock.patch.object(name_tables, "_SOURCES", {"skill": []}), \
                 mock.patch.object(name_tables, "_TCP_PREPARSE_CACHE", path):
                resolver = name_tables.NameResolver()
                self.assertEqual(resolver.skill(2414), "神圣壁垒")
                self.assertEqual(resolver.resolve("player", 36668136, default=""), "")

    def test_save_callback_can_reload_name_resolver(self) -> None:
        seen = []
        cache = TcpNameCache(self.path, autosave_interval_s=0, on_save=lambda path: seen.append(path))
        cache.observe_name("skill", 2414, "神圣壁垒", source="tcp", confidence="high")

        self.assertEqual(seen, [self.path])

    def test_hybrid_name_tables_build_runtime_outputs(self) -> None:
        cache = TcpNameCache(self.path, autosave_interval_s=0)
        cache.observe_name("monster", 999001, "测试怪物", source="tcp", confidence="high")
        cache.observe_name("monster_skill", 1004820, "肉鸽大秘境肉山平A", source="tcp", confidence="high")
        cache.observe_name("environment_skill", 1006507, "吸引怪物-黯影堡垒专用", source="tcp", confidence="high")
        cache.observe_name("player_skill", 9998, "测试玩家技能", source="tcp", confidence="high")
        cache.observe_name("boss", 1301, "测试Boss", source="tcp", confidence="high")
        cache.observe_name("boss_mechanic", 47, "护盾破裂", source="tcp", confidence="high")
        cache.observe_name("dungeon", 42001, "测试副本", source="tcp", confidence="high")
        cache.observe_name("skill", 9999, "技能#9999", source="tcp", confidence="high")

        from tools.tablekit.hybrid_name_tables import update_runtime_tables
        out_dir = os.path.join(self.tmp, "tables")
        result = update_runtime_tables(self.path, output_dir=out_dir)

        self.assertGreaterEqual(result["kinds"]["boss_mechanic"]["written"], 1)
        with open(os.path.join(out_dir, "monster.json"), "r", encoding="utf-8") as f:
            monster = json.load(f)
        with open(os.path.join(out_dir, "boss.json"), "r", encoding="utf-8") as f:
            boss = json.load(f)
        with open(os.path.join(out_dir, "boss_mechanic.json"), "r", encoding="utf-8") as f:
            mechanic = json.load(f)
        with open(os.path.join(out_dir, "skill.json"), "r", encoding="utf-8") as f:
            skill = json.load(f)
        with open(os.path.join(out_dir, "monster_skill.json"), "r", encoding="utf-8") as f:
            monster_skill = json.load(f)
        with open(os.path.join(out_dir, "environment_skill.json"), "r", encoding="utf-8") as f:
            environment_skill = json.load(f)
        with open(os.path.join(out_dir, "player_skill.json"), "r", encoding="utf-8") as f:
            player_skill = json.load(f)
        self.assertEqual(monster["999001"], "测试怪物")
        self.assertEqual(monster_skill["1004820"], "肉鸽大秘境肉山平A")
        self.assertEqual(environment_skill["1006507"], "吸引怪物-黯影堡垒专用")
        self.assertEqual(player_skill["9998"], "测试玩家技能")
        self.assertEqual(boss["1301"], "测试Boss")
        self.assertEqual(mechanic["47"], "护盾破裂")
        self.assertNotIn("9999", skill)

    def test_hybrid_name_tables_materializes_refined_semantic_tables(self) -> None:
        from tools.tablekit.hybrid_name_tables import update_runtime_tables

        out_dir = os.path.join(self.tmp, "refined_tables")
        result = update_runtime_tables(self.path, output_dir=out_dir)

        for kind in ("player_skill", "monster_skill", "environment_skill", "ultimate_skill", "roguelike_affix", "profession_skill", "scripted_skill", "virtual_skill", "boss_mechanic_skill", "profession_skill_buff"):
            with self.subTest(kind=kind):
                self.assertIn(kind, result["kinds"])
                with open(os.path.join(out_dir, f"{kind}.json"), "r", encoding="utf-8") as f:
                    table = json.load(f)
                self.assertIsInstance(table, dict)
        with open(os.path.join(out_dir, "monster_skill.json"), "r", encoding="utf-8") as f:
            monster_skill = json.load(f)
        with open(os.path.join(out_dir, "environment_skill.json"), "r", encoding="utf-8") as f:
            environment_skill = json.load(f)
        self.assertIn("1004820", monster_skill)
        self.assertIn("1006507", environment_skill)
        self.assertFalse(os.path.exists(os.path.join(out_dir, "boss_status.json")))

    def test_hybrid_name_tables_materializes_static_skill_and_buff_sources(self) -> None:
        from tools.tablekit import hybrid_name_tables

        out_dir = os.path.join(self.tmp, "tables")
        datatools = os.path.join(self.tmp, "DataTools", "CN")
        resonance = os.path.join(self.tmp, "resonance")
        assets = os.path.join(self.tmp, "assets")
        os.makedirs(datatools, exist_ok=True)
        os.makedirs(resonance, exist_ok=True)
        os.makedirs(assets, exist_ok=True)
        with open(os.path.join(datatools, "SkillTable.json"), "w", encoding="utf-8") as f:
            json.dump({"1001": {"Id": 1001, "Name": "静态技能"}}, f, ensure_ascii=False)
        with open(os.path.join(datatools, "BuffTable.json"), "w", encoding="utf-8") as f:
            json.dump({"2001": {"Id": 2001, "Name": "静态Buff", "NameDesign": "静态Buff设计"}}, f, ensure_ascii=False)
        with open(os.path.join(resonance, "BuffName.json"), "w", encoding="utf-8") as f:
            json.dump([{"Id": 2002, "NameDesign": "列表Buff"}], f, ensure_ascii=False)
        with open(os.path.join(assets, "skill_names.json"), "w", encoding="utf-8") as f:
            json.dump({"1002": "旧技能表"}, f, ensure_ascii=False)

        with mock.patch.object(hybrid_name_tables, "_DATATOOLS_CN", datatools), \
             mock.patch.object(hybrid_name_tables, "_RESONANCE_CONFIG", resonance), \
             mock.patch.object(hybrid_name_tables, "_ASSETS", assets):
            result = hybrid_name_tables.update_runtime_tables(self.path, output_dir=out_dir)

        self.assertEqual(result["kinds"]["skill"]["written"], 2)
        self.assertGreaterEqual(result["kinds"]["buff"]["written"], 2)
        with open(os.path.join(out_dir, "skill.json"), "r", encoding="utf-8") as f:
            skill = json.load(f)
        with open(os.path.join(out_dir, "buff.json"), "r", encoding="utf-8") as f:
            buff = json.load(f)
        self.assertEqual(skill["1001"], "静态技能")
        self.assertEqual(skill["1002"], "旧技能表")
        self.assertEqual(buff["2001"], "静态Buff设计")
        self.assertEqual(buff["2002"], "列表Buff")
        self.assertNotIn("1002", buff)


if __name__ == "__main__":
    unittest.main(verbosity=2)
