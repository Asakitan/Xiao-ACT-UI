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
        cache.save(force=True)

        with open(self.path, "r", encoding="utf-8") as f:
            data = json.load(f)
        endpoint = data["endpoints"]["01020304:1234"]
        self.assertEqual(endpoint["ip"], "1.2.3.4")
        self.assertEqual(endpoint["port"], 1234)
        self.assertEqual(data["names"]["by_kind"]["skill"]["1522"]["text"], "滋养")
        dumped = json.dumps(data, ensure_ascii=False)
        self.assertNotIn("raw_payload", dumped)
        self.assertNotIn("packet_bytes", dumped)

    def test_fallback_labels_are_not_recorded_as_names(self) -> None:
        cache = TcpNameCache(self.path, autosave_interval_s=0)
        cache.observe_name("monster", 11008, "怪物#11008", source="tcp", confidence="high")
        self.assertNotIn("monster", cache.snapshot()["names"]["by_kind"])

    def test_build_index_from_live_rows_filters_confidence_and_id_space(self) -> None:
        index = build_index_from_live_rows({
            "rows": [
                {
                    "text": "神圣壁垒",
                    "confidence": "high",
                    "primary_match": {"id_space": "skill_id", "id": 2414, "source": "tcp_alias"},
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
        self.assertEqual(by_kind["skill"]["2414"]["text"], "神圣壁垒")
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
                 mock.patch.object(name_tables, "_LIVE_ACT_MATCHES", os.path.join(td, "missing.json")), \
                 mock.patch.object(name_tables, "_TCP_PREPARSE_CACHE", path):
                resolver = name_tables.NameResolver()
                self.assertEqual(resolver.skill(2414), "神圣壁垒")
                self.assertEqual(resolver.resolve("player", 36668136, default=""), "")


if __name__ == "__main__":
    unittest.main(verbosity=2)
