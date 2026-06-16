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

from plugins.star_resonance_plugin.net.tcp_name_cache import (
    TcpNameCache,
    build_index_from_live_rows,
    runtime_cache_path,
    sanitize_shared_cache,
    shared_cache_path,
)


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
        self.assertEqual(data["names"]["by_kind"]["player_skill"]["1522"]["text"], "滋养")
        self.assertNotIn("skill", data["names"]["by_kind"])
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
                    "text": "滋养",
                    "confidence": "high",
                    "primary_match": {"id_space": "skill_id", "id": 1522, "source": "tcp_alias"},
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
        entry = by_kind["player_skill"]["1522"]
        self.assertEqual(entry["text"], "滋养")
        dumped = json.dumps(entry, ensure_ascii=False)
        self.assertNotIn("string_obj", dumped)
        self.assertNotIn("table_element_base", dumped)
        self.assertNotIn("context", entry)
        self.assertNotIn("sources", entry)
        self.assertNotIn("updated_at", entry)
        self.assertNotIn("skill", by_kind)
        self.assertNotIn("unknown", by_kind)

    def test_default_runtime_cache_is_separate_from_shared_asset(self) -> None:
        self.assertNotEqual(os.path.abspath(runtime_cache_path()), os.path.abspath(shared_cache_path()))
        self.assertTrue(os.path.basename(runtime_cache_path()).endswith(".local.json"))

    def test_sanitize_shared_cache_strips_local_and_provenance_fields(self) -> None:
        dirty = {
            "schema_version": 1,
            "updated_at": "2026-06-04T21:23:23Z",
            "endpoints": {"d239471c:2131": {"ip": "210.57.71.28", "seen_count": 3}},
            "names": {
                "by_kind": {
                    "skill": {
                        "2414": {
                            "text": "神圣壁垒",
                            "confidence": "high",
                            "sources": ["StarResonanceDps/DataTools/Data/CN/BuffTable.json"],
                            "endpoints": ["d239471c:2131"],
                            "updated_at": "2026-06-04T21:23:23Z",
                            "context": {"player_uid": 36668136, "player_uuid": 2403082961536},
                        },
                        "1": {"text": "login", "confidence": "medium"},
                    },
                    "player": {"36668136": {"text": "咲"}},
                }
            },
        }
        clean = sanitize_shared_cache(dirty)
        dumped = json.dumps(clean, ensure_ascii=False)
        retained_texts = []
        for bucket in clean["names"]["by_kind"].values():
            retained_texts.extend(str(entry.get("text") or "") for entry in bucket.values())
        self.assertIn("神圣壁垒", retained_texts)
        for forbidden in (
            "endpoints", "player", "sources", "context", "updated_at", "seen_count",
            "player_uid", "player_uuid", "210.57.71.28", "d239471c:2131", "StarResonanceDps/", "login",
        ):
            self.assertNotIn(forbidden, dumped)

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
                 mock.patch.object(name_tables, "_TCP_PREPARSE_CACHE", path), \
                 mock.patch.object(name_tables, "_TCP_PREPARSE_LOCAL_CACHE", os.path.join(td, "missing.local.json")):
                resolver = name_tables.NameResolver()
                self.assertEqual(resolver.skill(2414), "神圣壁垒")
                self.assertEqual(resolver.resolve("player", 36668136, default=""), "")

    def test_name_resolver_reuses_tcp_preparse_cache_until_file_changes(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = os.path.join(td, "cache.json")

            def write_skill(text: str, mtime: int) -> None:
                with open(path, "w", encoding="utf-8") as f:
                    json.dump({"names": {"by_kind": {"skill": {"987654321": {"text": text}}}}}, f, ensure_ascii=False)
                os.utime(path, (mtime, mtime))

            write_skill("神圣壁垒", 1)
            from tools.tablekit import name_tables
            original_json_load = json.load
            with mock.patch.object(name_tables, "_SOURCES", {}), \
                 mock.patch.object(name_tables, "_TCP_PREPARSE_CACHE", path), \
                 mock.patch.object(name_tables, "_TCP_PREPARSE_LOCAL_CACHE", os.path.join(td, "missing.local.json")):
                name_tables.names.reload()
                resolver = name_tables.NameResolver()
                with mock.patch.object(name_tables.json, "load", wraps=original_json_load) as mocked_load:
                    self.assertEqual(resolver.skill(987654321), "神圣壁垒")
                    self.assertEqual(resolver.skill(987654321), "神圣壁垒")
                    self.assertEqual(mocked_load.call_count, 1)

                    write_skill("二段名字", 2)
                    self.assertEqual(resolver.skill(987654321), "二段名字")
                    self.assertEqual(mocked_load.call_count, 2)
                name_tables.names.reload()

    def test_name_resolver_merges_ignored_local_cache_text_only(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            shared = os.path.join(td, "shared.json")
            local = os.path.join(td, "local.json")
            with open(shared, "w", encoding="utf-8") as f:
                json.dump({"names": {"by_kind": {"skill": {"2414": {"text": "神圣壁垒"}}}}}, f, ensure_ascii=False)
            with open(local, "w", encoding="utf-8") as f:
                json.dump({
                    "endpoints": {"01020304:1234": {"ip": "1.2.3.4"}},
                    "names": {"by_kind": {"skill": {"9999": {"text": "本地技能", "context": {"endpoint": "01020304:1234"}}}}},
                }, f, ensure_ascii=False)
            from tools.tablekit import name_tables
            with mock.patch.object(name_tables, "_SOURCES", {"skill": []}), \
                 mock.patch.object(name_tables, "_TCP_PREPARSE_CACHE", shared), \
                 mock.patch.object(name_tables, "_TCP_PREPARSE_LOCAL_CACHE", local):
                resolver = name_tables.NameResolver()
                self.assertEqual(resolver.skill(2414), "神圣壁垒")
                self.assertEqual(resolver.skill(9999), "本地技能")

    def test_runtime_name_table_assets_are_consumed_by_name_resolver(self) -> None:
        from tools.tablekit import name_tables

        # Runtime resolver assets are the per-kind id->name tables plus the shared
        # preparse cache. Exclude ignored local runtime caches (*.local.json) and
        # hand-curated meta tables consumed by dedicated loaders rather than the
        # NameResolver (element.json -> tools.tablekit.element_meta).
        non_resolver_assets = {"element.json"}
        static_cache_path = getattr(name_tables, "_STATIC_CACHE_PATH", "")
        if not static_cache_path:
            non_resolver_assets.add("static_id_name_cache.json")
        runtime_assets = {
            name for name in os.listdir(name_tables._EXTRACTED)
            if name.endswith(".json")
            and not name.startswith("live_")
            and not name.endswith(".local.json")
            and name not in non_resolver_assets
        }
        resolver_assets = {
            fname
            for sources in name_tables._SOURCES.values()
            for folder, fname in sources
            if os.path.abspath(folder) == os.path.abspath(name_tables._EXTRACTED)
        }
        resolver_assets.add(os.path.basename(name_tables._TCP_PREPARSE_CACHE))
        if static_cache_path:
            resolver_assets.add(os.path.basename(static_cache_path))

        self.assertFalse(sorted(runtime_assets - resolver_assets))

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
        self.assertFalse(os.path.exists(os.path.join(out_dir, "skill.json")))

    def test_hybrid_name_tables_materializes_refined_semantic_tables(self) -> None:
        from tools.tablekit.hybrid_name_tables import update_runtime_tables

        out_dir = os.path.join(self.tmp, "refined_tables")
        result = update_runtime_tables(self.path, output_dir=out_dir)

        for kind in ("player_skill", "monster_skill", "environment_skill", "ultimate_skill", "roguelike_affix", "profession_skill", "scripted_skill", "virtual_skill", "boss_mechanic_skill", "client_effect_skill", "interaction_skill", "companion_skill", "projectile_skill", "passive_skill", "test_skill", "system_skill", "profession_skill_buff"):
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
        self.assertFalse(os.path.exists(os.path.join(out_dir, "skill.json")))
        self.assertFalse(os.path.exists(os.path.join(out_dir, "boss_status.json")))

    def test_full_rebuild_is_self_contained_without_neighbour_repo(self) -> None:
        """A full rebuild must rely ONLY on our project (committed tables + parse
        cache): with the neighbour StarResonanceDps/resonance dirs pointed at an
        empty path, it must not empty our tables (no classifier degradation)."""
        from tools.tablekit import hybrid_name_tables
        import shutil

        out_dir = os.path.join(self.tmp, "self_contained")
        os.makedirs(out_dir, exist_ok=True)
        for fn in os.listdir(hybrid_name_tables._NAME_TABLES):
            if fn.endswith(".json"):
                shutil.copy2(os.path.join(hybrid_name_tables._NAME_TABLES, fn), os.path.join(out_dir, fn))
        empty = os.path.join(self.tmp, "no_neighbour")
        os.makedirs(empty, exist_ok=True)
        with mock.patch.object(hybrid_name_tables, "_DATATOOLS_CN", empty), \
             mock.patch.object(hybrid_name_tables, "_RESONANCE_CONFIG", empty), \
             mock.patch.object(hybrid_name_tables, "_RESONANCE_METER", empty):
            result = hybrid_name_tables.update_runtime_tables(output_dir=out_dir)

        self.assertNotIn("skill", result["kinds"])
        # Missing neighbour must not collapse our classified tables.
        self.assertGreaterEqual(result["kinds"]["player_buff"]["written"], 1000)
        self.assertGreaterEqual(result["kinds"]["buff"]["written"], 1000)
        self.assertGreaterEqual(result["kinds"]["system_skill"]["written"], 1000)
        self.assertFalse(os.path.exists(os.path.join(out_dir, "skill.json")))


    def test_overlay_cache_overrides_existing_names_only(self) -> None:
        from tools.tablekit import hybrid_name_tables as H

        out_dir = os.path.join(self.tmp, "overlay_tables")
        os.makedirs(out_dir, exist_ok=True)
        # Existing on-disk table carries a stale neighbouring name plus an entry we
        # never parsed from memory.
        with open(os.path.join(out_dir, "player_buff.json"), "w", encoding="utf-8") as f:
            json.dump({"2207130": "音浪烈焰", "999": "无关条目"}, f, ensure_ascii=False)
        cache = TcpNameCache(self.path, autosave_interval_s=0)
        cache.observe_name("player_buff", 2207130, "音浪烈火", source="mem", confidence="high")
        cache.save(force=True)

        result = H.overlay_cache_into_existing_tables(self.path, output_dir=out_dir)
        with open(os.path.join(out_dir, "player_buff.json"), "r", encoding="utf-8") as f:
            table = json.load(f)
        # Our memory-parsed name wins the conflict; unrelated ids stay untouched.
        self.assertEqual(table["2207130"], "音浪烈火")
        self.assertEqual(table["999"], "无关条目")
        self.assertEqual(result["kinds"]["player_buff"]["changed"], 1)

    def test_hybrid_clean_text_drops_denylisted_tokens(self) -> None:
        from tools.tablekit import hybrid_name_tables as H

        for token in ("login", "LOGIN", "logout", "unknown", "none", "null"):
            self.assertEqual(H._clean_text(token), "", token)
        self.assertEqual(H._clean_text("巴哈马尔高原"), "巴哈马尔高原")


if __name__ == "__main__":
    unittest.main(verbosity=2)
