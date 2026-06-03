# -*- coding: utf-8 -*-
"""Regression tests for shared ACT report/export helpers."""

from __future__ import annotations

import json
import unittest

from act_platform import runtime


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
        self.assertEqual(status["formats"], ["json", "csv"])
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

    def test_missing_history_store_is_reported_without_throwing(self) -> None:
        status = runtime.act_report_status(object())
        exported = runtime.act_report_export(object())

        self.assertFalse(status["ok"])
        self.assertIn("errors", status)
        self.assertFalse(exported["ok"])
        self.assertIn("DPS history", exported["message"])


if __name__ == "__main__":
    unittest.main()
