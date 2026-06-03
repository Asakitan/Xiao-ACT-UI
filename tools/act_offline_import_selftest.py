# -*- coding: utf-8 -*-
"""Regression coverage for normalized ACT offline imports."""

from __future__ import annotations

import json
import os
import tempfile
import unittest

from act_platform import runtime
from act_replay.events import damage_event, dungeon_event
from act_replay.harness import ActReplayHarness
from act_replay.importer import import_normalized_file, load_normalized_import
from engines.dps_history import DpsHistoryStore


SELF_UID = 36668136


class FakeOwner:
    def __init__(self, store=None):
        if store is not None:
            self._dps_history_store = store
        self.shown = None

    def _show_dps_last_report(self, report=None):
        self.shown = report
        return bool(report)


def _demo_damage_event(amount: int = 1234) -> dict:
    return damage_event(
        attacker_uid=SELF_UID,
        attacker_uuid=(SELF_UID << 16) | 640,
        attacker_is_self=True,
        target_uuid=987654321064,
        target_is_monster=True,
        target_is_combat_target=True,
        skill_id=1101,
        skill_key=1101,
        damage=amount,
    )


class ActOfflineImportTests(unittest.TestCase):
    def test_json_object_import_replays_damage(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_import_json_") as root:
            path = os.path.join(root, "encounter.json")
            with open(path, "w", encoding="utf-8") as fp:
                json.dump({
                    "self_uid": SELF_UID,
                    "events": [
                        dungeon_event("sync_dungeon_data", dungeon_id=42001, scene_uuid=155001),
                        _demo_damage_event(4321),
                    ],
                }, fp, ensure_ascii=False)

            summary = import_normalized_file(path)
            self.assertTrue(summary["ok"], summary)
            self.assertEqual(summary["format"], "json")
            self.assertEqual(summary["self_uid"], SELF_UID)
            self.assertEqual(summary["event_count"], 2)

            harness = ActReplayHarness(source_probe={"data_source": "offline_import"})
            harness.set_self_uid(summary["self_uid"])
            snapshot = harness.replay(summary["events"])

        self.assertEqual(snapshot["live"]["total_damage"], 4321)
        self.assertEqual((snapshot["render_spec"]["sources"]["summary"] or {}).get("data_source"), "offline_import")

    def test_jsonl_import_reads_meta_and_events(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_import_jsonl_") as root:
            path = os.path.join(root, "encounter.jsonl")
            with open(path, "w", encoding="utf-8") as fp:
                fp.write(json.dumps({"kind": "meta", "self_uid": SELF_UID}, ensure_ascii=False) + "\n")
                fp.write(json.dumps(_demo_damage_event(2222), ensure_ascii=False) + "\n")

            self_uid, events = load_normalized_import(path)

        self.assertEqual(self_uid, SELF_UID)
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]["damage"], 2222)

    def test_unsupported_import_format_reports_error(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_import_bad_") as root:
            path = os.path.join(root, "encounter.txt")
            with open(path, "w", encoding="utf-8") as fp:
                fp.write("not an import")

            summary = import_normalized_file(path)

        self.assertFalse(summary["ok"])
        self.assertEqual(summary["event_count"], 0)
        self.assertIn("unsupported", summary["message"])

    def test_runtime_import_persists_replayed_report_to_history(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_import_persist_") as root:
            path = os.path.join(root, "encounter.json")
            history_path = os.path.join(root, "history.json")
            store = DpsHistoryStore(path=history_path, limit=5)
            owner = FakeOwner(store)
            with open(path, "w", encoding="utf-8") as fp:
                json.dump({
                    "self_uid": SELF_UID,
                    "events": [
                        dungeon_event("sync_dungeon_data", dungeon_id=42001, scene_uuid=155001),
                        _demo_damage_event(7654),
                    ],
                }, fp, ensure_ascii=False)

            result = runtime.act_offline_import_file(owner, path, show=True)
            latest = store.latest_report()

        self.assertTrue(result["ok"], result)
        self.assertTrue(result["persisted"], result)
        self.assertTrue(result["shown"], result)
        self.assertEqual(result["event_count"], 2)
        self.assertEqual(result["report"]["report_reason"], "offline_import")
        self.assertEqual(result["report"]["source_kind"], "offline_import")
        self.assertEqual(result["history_item"]["report_reason"], "offline_import")
        self.assertEqual(result["history_item"]["total_damage"], 7654)
        self.assertEqual(latest["total_damage"], 7654)
        self.assertEqual(result["status"]["storage_status"]["count"], 1)
        self.assertEqual((result["snapshot"]["render_spec"]["sources"]["summary"] or {}).get("data_source"), "offline_import")

    def test_runtime_import_reports_missing_history_store_when_persisting(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_import_missing_store_") as root:
            path = os.path.join(root, "encounter.json")
            with open(path, "w", encoding="utf-8") as fp:
                json.dump({"self_uid": SELF_UID, "events": [_demo_damage_event(1111)]}, fp)

            result = runtime.act_offline_import_file(FakeOwner(), path)

        self.assertFalse(result["ok"])
        self.assertFalse(result["persisted"])
        self.assertEqual(result["preview"]["total_damage"], 1111)
        self.assertIn("DPS history is not initialized", result["message"])

    def test_runtime_import_can_replay_without_persisting(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_import_replay_only_") as root:
            path = os.path.join(root, "encounter.json")
            with open(path, "w", encoding="utf-8") as fp:
                json.dump({"self_uid": SELF_UID, "events": [_demo_damage_event(3333)]}, fp)

            result = runtime.act_offline_import_file(FakeOwner(), path, persist=False)

        self.assertTrue(result["ok"], result)
        self.assertFalse(result["persisted"])
        self.assertEqual(result["preview"]["total_damage"], 3333)
        self.assertEqual(result["status"], {})


if __name__ == "__main__":
    unittest.main()
