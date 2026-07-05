# -*- coding: utf-8 -*-
# Regression coverage for ACT Mini-Parse formatter helpers.

from __future__ import annotations

import json
import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from act_platform.runtime import act_mini_parse_copy, act_mini_parse_preview, act_mini_parse_status, act_report_copy


class _HistoryStore:
    path = "memory://history"

    def list_reports(self, _limit=20):
        return []


class _Owner:
    def __init__(self):
        self._dps_history_store = _HistoryStore()
        self._last_act_snapshot = {
            "render_spec": {
                "title": "Test Encounter",
                "encounter": {"id": "enc-1", "status": "live", "duration_s": 10},
                "totals": {"damage": 3000, "dps": 300, "heal": 100, "hps": 10},
                "rows": [
                    {"rank": 1, "uid": 1001, "name": "Kirito", "damage": 2000, "dps": 200, "heal": 0},
                    {"rank": 2, "uid": 1002, "name": "Asuna", "damage": 1000, "dps": 100, "heal": 100},
                ],
            }
        }
        self._last_dps_report = {
            "encounter_id": "enc-1",
            "total_damage": 3000,
            "total_dps": 300,
            "entities": self._last_act_snapshot["render_spec"]["rows"],
        }

    def _get_dps_act_snapshot(self, history_limit=20):
        return self._last_act_snapshot

    @property
    def _dps_tracker(self):
        class Tracker:
            def __init__(self, report):
                self._report = report

            def get_last_report(self):
                return self._report

        return Tracker(self._last_dps_report)


class ActMiniParseTests(unittest.TestCase):
    def test_status_lists_builtin_formatters(self) -> None:
        status = act_mini_parse_status(_Owner())

        self.assertTrue(status["ok"])
        formatter_ids = {row["id"] for row in status["formatters"]}
        self.assertIn("summary_table", formatter_ids)
        self.assertIn("chat_ranking", formatter_ids)
        self.assertIn("json", formatter_ids)

    def test_preview_and_copy_format_compact_text(self) -> None:
        owner = _Owner()

        preview = act_mini_parse_preview(owner, formatter_id="chat_ranking")
        copied = act_mini_parse_copy(owner, formatter_id="summary_table")

        self.assertTrue(preview["ok"], preview)
        self.assertIn("Kirito", preview["text"])
        self.assertIn("Asuna", preview["text"])
        self.assertTrue(copied["ok"], copied)
        self.assertFalse(copied["copied"])
        self.assertIn("Test Encounter", copied["text"])

    def test_legacy_report_copy_remains_json_payload(self) -> None:
        result = act_report_copy(_Owner(), fmt="json")
        payload = json.loads(result["text"])

        self.assertTrue(result["ok"], result)
        self.assertEqual(payload["format"], "json")
        self.assertEqual(payload["preview"]["top_rows"][0]["name"], "Kirito")


if __name__ == "__main__":
    unittest.main()
