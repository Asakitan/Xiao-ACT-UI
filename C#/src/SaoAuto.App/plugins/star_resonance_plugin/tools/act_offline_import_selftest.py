# -*- coding: utf-8 -*-
"""Regression coverage for normalized ACT offline imports."""

from __future__ import annotations

import json
import os
import tempfile
import unittest
import gzip
import zipfile

import _bootstrap  # noqa: F401

from act_platform import runtime
from act_platform.plugins import PluginManager
from act_replay.events import damage_event, dungeon_event
from act_replay.harness import ActReplayHarness
from act_replay.importer import import_normalized_file, load_normalized_import
from plugins.star_resonance_plugin.engines.dps_history import DpsHistoryStore, load_exported_report_file


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


PLUGIN_IMPORT_CODE = r'''
from act_replay.events import damage_event

SELF_UID = 36668136

def _plugin_import(payload):
    if payload.get("operation") != "import_file":
        return {"ok": True}
    path = str(payload.get("path") or "")
    if not path.endswith(".demoact"):
        return {"ok": False, "message": "unsupported demo import"}
    return {
        "ok": True,
        "format": "demoact",
        "source_path": path,
        "self_uid": SELF_UID,
        "events": [
            damage_event(
                attacker_uid=SELF_UID,
                attacker_uuid=(SELF_UID << 16) | 640,
                attacker_is_self=True,
                target_uuid=987654321064,
                target_is_monster=True,
                target_is_combat_target=True,
                skill_id=1101,
                skill_key=1101,
                damage=5555,
            )
        ],
    }

def on_load(ctx):
    ctx.register_parser_adapter("demo_file_importer", {
        "title": "Demo file importer",
        "game_id": "star_resonance",
        "source_kinds": ["file"],
        "priority": 20,
    }, handler=_plugin_import)
'''


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
            status = runtime.act_offline_import_status(owner)
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
        self.assertTrue(status["ok"], status)
        self.assertIn("json", status["accepted_formats"])
        self.assertIn("xml.gz", status["accepted_formats"])
        self.assertIn("xml.zip", status["accepted_formats"])
        self.assertEqual(status["status"], "imported")
        self.assertEqual(status["last_result"]["source_path"], path)
        self.assertEqual(status["history"]["storage_status"]["count"], 1)

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

    def test_runtime_import_falls_back_to_plugin_parser_adapter(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_import_plugin_") as root:
            plugin_dir = os.path.join(root, "demo_import_plugin")
            os.makedirs(plugin_dir, exist_ok=True)
            with open(os.path.join(plugin_dir, "plugin.json"), "w", encoding="utf-8") as fp:
                json.dump({
                    "id": "demo_import_plugin",
                    "name": "Demo Import Plugin",
                    "version": "0.1.0",
                    "entry": "plugin.py",
                    "enabled": True,
                }, fp, ensure_ascii=False, indent=2)
            with open(os.path.join(plugin_dir, "plugin.py"), "w", encoding="utf-8") as fp:
                fp.write(PLUGIN_IMPORT_CODE)
            path = os.path.join(root, "encounter.demoact")
            with open(path, "w", encoding="utf-8") as fp:
                fp.write("demo plugin import")
            history_path = os.path.join(root, "history.json")
            store = DpsHistoryStore(path=history_path, limit=5)
            manager = PluginManager(plugin_dirs=[root])
            manager.discover()
            owner = FakeOwner(store)
            owner._act_plugin_manager = manager

            result = runtime.act_offline_import_file(owner, path, show=False)

        self.assertTrue(result["ok"], result)
        self.assertEqual(result["importer"], "plugin_parser_adapter")
        self.assertEqual(result["parser_adapter_id"], "demo_file_importer")
        self.assertEqual(result["plugin_id"], "demo_import_plugin")
        self.assertEqual(result["format"], "demoact")
        self.assertEqual(result["event_count"], 1)
        self.assertEqual(result["history_item"]["total_damage"], 5555)
        self.assertEqual(result["preview"]["total_damage"], 5555)

    def test_runtime_import_can_persist_exported_xml_report(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_import_xml_report_") as root:
            source_store = DpsHistoryStore(path=os.path.join(root, "source_history.json"), limit=5)
            source_store.add_report({
                "encounter_id": "xml-encounter",
                "report_reason": "xml_source",
                "elapsed_s": 12,
                "total_damage": 9001,
                "total_heal": 123,
                "entities": [{"uid": SELF_UID, "name": "Kirito", "damage_total": 9001, "dps": 750}],
            })
            xml_path = source_store.export_report(fmt="xml")
            parsed = load_exported_report_file(xml_path or "")
            target_store = DpsHistoryStore(path=os.path.join(root, "target_history.json"), limit=5)
            owner = FakeOwner(target_store)

            result = runtime.act_offline_import_file(owner, xml_path or "", show=True)
            latest = target_store.latest_report()

        self.assertEqual(parsed["encounter_id"], "xml-encounter")
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["importer"], "exported_report")
        self.assertEqual(result["format"], "xml")
        self.assertEqual(result["event_count"], 0)
        self.assertTrue(result["persisted"])
        self.assertTrue(result["shown"])
        self.assertEqual(result["history_item"]["total_damage"], 9001)
        self.assertEqual(result["preview"]["total_damage"], 9001)
        self.assertEqual(latest["encounter_id"], "xml-encounter")

    def test_runtime_import_can_persist_compressed_xml_reports(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_import_compressed_xml_report_") as root:
            source_store = DpsHistoryStore(path=os.path.join(root, "source_history.json"), limit=5)
            source_store.add_report({
                "encounter_id": "xml-compressed-encounter",
                "report_reason": "xml_source",
                "elapsed_s": 12,
                "total_damage": 7777,
                "entities": [{"uid": SELF_UID, "name": "Kirito", "damage_total": 7777, "dps": 648}],
            })
            xml_path = source_store.export_report(fmt="xml") or ""
            with open(xml_path, "r", encoding="utf-8") as fp:
                xml_text = fp.read()
            gzip_path = os.path.join(root, "report.xml.gz")
            zip_path = os.path.join(root, "report.xml.zip")
            with gzip.open(gzip_path, "wt", encoding="utf-8") as fp:
                fp.write(xml_text)
            with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
                zf.writestr("report.xml", xml_text)
            target_store = DpsHistoryStore(path=os.path.join(root, "target_history.json"), limit=5)
            owner = FakeOwner(target_store)

            gzip_result = runtime.act_offline_import_file(owner, gzip_path, show=False)
            zip_result = runtime.act_offline_import_file(owner, zip_path, show=False)

        self.assertTrue(gzip_result["ok"], gzip_result)
        self.assertEqual(gzip_result["format"], "xml.gz")
        self.assertTrue(zip_result["ok"], zip_result)
        self.assertEqual(zip_result["format"], "xml.zip")


if __name__ == "__main__":
    unittest.main()
