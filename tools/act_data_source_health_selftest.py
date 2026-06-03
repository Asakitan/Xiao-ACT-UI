# -*- coding: utf-8 -*-
"""Regression tests for shared ACT data-source health helpers."""

from __future__ import annotations

import json
import unittest

from act_platform import runtime


class FakePacketEngine:
    def __init__(self):
        self._last_update_t = 1000.0
        self._last_capture_raw_seen_ts = 1001.25
        self._data_source_mode = "hybrid"

    def health(self):
        return {
            "data_source": "hybrid",
            "running": True,
            "error_msg": "",
            "mem": {
                "data_source": "unified",
                "requested_mode": "hybrid",
                "running": True,
                "alive": True,
                "is_memory_active": True,
                "status": "running",
                "last_error": "",
                "watchers": {
                    "self": "memory_first",
                    "boss": "tcp_fallback",
                },
                "self": {"uid": 123, "hp": 80, "max_hp": 100},
            },
        }


class FakeOwner:
    def __init__(self):
        self._packet_engine = FakePacketEngine()
        self._recognition_active = True
        self._cfg_settings_ref = {"mem_data_source": "hybrid"}


class ActDataSourceHealthTests(unittest.TestCase):
    def test_health_payload_contains_parity_fields(self) -> None:
        payload = runtime.act_data_source_health(FakeOwner(), now=1002.0)

        self.assertTrue(payload["ok"])
        self.assertEqual(payload["status"], "running")
        self.assertIn("packet", payload["sources"])
        self.assertIn("memory", payload["sources"])
        self.assertIn("summary", payload["sources"])
        self.assertTrue(payload["sources"]["summary"]["hybrid"])
        self.assertEqual(payload["sources"]["packet"]["data_source"], "hybrid")
        self.assertEqual(payload["sources"]["memory"]["watchers"]["self"], "memory_first")
        self.assertEqual(payload["latency_ms"], 750)
        self.assertEqual(payload["last_event_ms"], 2000)
        self.assertEqual(payload["errors"], [])
        json.dumps(payload, ensure_ascii=False)

    def test_diagnose_reports_missing_engine_without_throwing(self) -> None:
        owner = object()
        result = runtime.act_data_source_diagnose(owner, now=1.0)

        self.assertFalse(result["ok"])
        self.assertEqual(result["status"], "missing")
        self.assertTrue(result["errors"])
        self.assertTrue(any(item["level"] == "error" for item in result["diagnostics"]))


if __name__ == "__main__":
    unittest.main()
