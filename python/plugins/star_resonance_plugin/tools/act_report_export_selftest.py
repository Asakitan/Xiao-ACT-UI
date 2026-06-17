# -*- coding: utf-8 -*-
"""Regression tests for shared ACT report/export helpers."""

from __future__ import annotations

import _bootstrap  # noqa: F401

import json
import os
import tempfile
import unittest
import gzip
import zipfile
from xml.etree import ElementTree as ET
from unittest import mock

from act_platform import runtime
from plugins.star_resonance_plugin.engines import dps_history


class FakeHistoryStore:
    def __init__(self):
        self.report = {
            "elapsed_s": 12.5,
            "total_damage": 12000,
            "total_heal": 345,
            "total_dps": 960,
            "report_reason": "completed",
            "entities": [
                {"uid": 1, "name": "Kirito", "profession": "剑士", "damage_total": 8000, "dps": 640, "damage_pct": 0.666},
                {"uid": 2, "name": "Asuna", "profession": "细剑", "damage_total": 4000, "dps": 320, "damage_pct": 0.333},
            ],
        }
        self.exported: list[tuple[dict, str]] = []

    def latest_report(self):
        return dict(self.report)

    def list_reports(self, limit=20):
        return [dict(self.report) for _ in range(min(int(limit or 20), 2))]

    def export_report(self, report=None, fmt="json"):
        src = dict(report or self.report)
        self.exported.append((src, str(fmt)))
        return f"E:/tmp/act_report.{fmt}"


class FakeOwner:
    def __init__(self):
        self._dps_history_store = FakeHistoryStore()
        self._dps_tracker = None

    def _get_dps_act_snapshot(self, history_limit=20):
        return {
            "render_spec": {
                "encounter": {"id": "enc-1", "status": "report", "duration_s": 12.5},
                "totals": {"damage": 12000, "heal": 345, "dps": 960},
                "rows": [
                    {"rank": 1, "uid": 1, "name": "Kirito", "damage": 8000, "dps": 640, "damage_pct": 0.666},
                    {"rank": 2, "uid": 2, "name": "Asuna", "damage": 4000, "dps": 320, "damage_pct": 0.333},
                ],
            },
            "history": self._dps_history_store.list_reports(history_limit),
            "last_report": self._dps_history_store.latest_report(),
        }


class ActReportExportTests(unittest.TestCase):
    def test_status_contains_export_contract_fields(self) -> None:
        status = runtime.act_report_status(FakeOwner(), limit=5)

        self.assertTrue(status["ok"])
        self.assertEqual(status["formats"], ["json", "csv", "html", "xml", "xml.gz", "xml.zip"])
        self.assertEqual(status["selected_format"], "json")
        self.assertEqual(status["encounter_id"], "enc-1")
        self.assertEqual(status["preview"]["total_damage"], 12000)
        self.assertEqual(status["preview"]["top_rows"][0]["name"], "Kirito")
        json.dumps(status, ensure_ascii=False)

    def test_export_and_copy_use_same_report_payload(self) -> None:
        owner = FakeOwner()
        exported = runtime.act_report_export(owner, fmt="csv")
        copied = runtime.act_report_copy(owner, fmt="json")

        self.assertTrue(exported["ok"])
        self.assertEqual(exported["selected_format"], "csv")
        self.assertTrue(exported["path"].endswith(".csv"))
        self.assertTrue(copied["ok"])
        self.assertIn("Kirito", copied["text"])
        self.assertEqual(owner._dps_history_store.exported[0][1], "csv")

    def test_html_export_format_is_available(self) -> None:
        owner = FakeOwner()
        exported = runtime.act_report_export(owner, fmt="html")

        self.assertTrue(exported["ok"])
        self.assertEqual(exported["selected_format"], "html")
        self.assertTrue(exported["path"].endswith(".html"))
        self.assertEqual(owner._dps_history_store.exported[0][1], "html")

    def test_xml_export_format_is_available(self) -> None:
        owner = FakeOwner()
        exported = runtime.act_report_export(owner, fmt="xml")

        self.assertTrue(exported["ok"])
        self.assertEqual(exported["selected_format"], "xml")
        self.assertTrue(exported["path"].endswith(".xml"))
        self.assertEqual(owner._dps_history_store.exported[0][1], "xml")

    def test_compressed_xml_export_formats_are_available(self) -> None:
        owner = FakeOwner()
        gzip_exported = runtime.act_report_export(owner, fmt="xml.gz")
        zip_exported = runtime.act_report_export(owner, fmt="xml.zip")

        self.assertTrue(gzip_exported["ok"])
        self.assertEqual(gzip_exported["selected_format"], "xml.gz")
        self.assertTrue(gzip_exported["path"].endswith(".xml.gz"))
        self.assertTrue(zip_exported["ok"])
        self.assertEqual(zip_exported["selected_format"], "xml.zip")
        self.assertTrue(zip_exported["path"].endswith(".xml.zip"))

    def test_history_store_writes_static_html_report(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_html_report_") as root:
            with mock.patch.object(dps_history, "DPS_HISTORY_EXPORT_DIR", root):
                store = dps_history.DpsHistoryStore(path=os.path.join(root, "history.json"))
                store.add_report(FakeHistoryStore().report)
                path = store.export_report(fmt="html")
                self.assertIsNotNone(path)
                with open(path or "", "r", encoding="utf-8") as fp:
                    text = fp.read()

        self.assertIn("<!doctype html>", text.lower())
        self.assertIn("SAO Auto ACT Report", text)
        self.assertIn("Kirito", text)
        self.assertIn("12,000", text)

    def test_history_store_writes_xml_report(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_xml_report_") as root:
            with mock.patch.object(dps_history, "DPS_HISTORY_EXPORT_DIR", root):
                store = dps_history.DpsHistoryStore(path=os.path.join(root, "history.json"))
                store.add_report(FakeHistoryStore().report)
                path = store.export_report(fmt="xml")
                self.assertIsNotNone(path)
                self.assertTrue(str(path).endswith(".xml"))
                tree = ET.parse(path or "")
                root_node = tree.getroot()

        self.assertEqual(root_node.tag, "sao_act_report")
        self.assertEqual(root_node.findtext("totals/total_damage"), "12000")
        names = [node.text for node in root_node.findall("combatants/combatant/name")]
        self.assertIn("Kirito", names)

    def test_history_store_writes_compressed_xml_reports(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_xml_compressed_report_") as root:
            with mock.patch.object(dps_history, "DPS_HISTORY_EXPORT_DIR", root):
                store = dps_history.DpsHistoryStore(path=os.path.join(root, "history.json"))
                store.add_report(FakeHistoryStore().report)
                gzip_path = store.export_report(fmt="xml.gz")
                zip_path = store.export_report(fmt="xml.zip")
                self.assertIsNotNone(gzip_path)
                self.assertIsNotNone(zip_path)
                with gzip.open(gzip_path or "", "rt", encoding="utf-8") as fp:
                    gzip_text = fp.read()
                with zipfile.ZipFile(zip_path or "", "r") as zf:
                    names = zf.namelist()
                    zip_text = zf.read(names[0]).decode("utf-8")

        self.assertTrue(str(gzip_path).endswith(".xml.gz"))
        self.assertTrue(str(zip_path).endswith(".xml.zip"))
        self.assertIn("sao_act_report", gzip_text)
        self.assertIn("sao_act_report", zip_text)

    def test_missing_history_store_is_reported_without_throwing(self) -> None:
        status = runtime.act_report_status(object())
        exported = runtime.act_report_export(object())

        self.assertFalse(status["ok"])
        self.assertIn("errors", status)
        self.assertFalse(exported["ok"])
        self.assertIn("DPS history", exported["message"])


if __name__ == "__main__":
    unittest.main()
