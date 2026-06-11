# -*- coding: utf-8 -*-
"""Regression tests for shared ACT data-source health helpers."""

from __future__ import annotations

import _bootstrap  # noqa: F401

import json
import unittest
from unittest import mock

from act_platform import runtime
from gui_modules.sao_gui_data_source_health import DataSourceHealthPanel


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
            "parser_adapter_selection": {
                "requested_id": "demo_live_packet",
                "selected_id": "star_resonance_tcp",
                "mode": "builtin",
                "fallback_reason": "plugin parser adapter demo_live_packet does not support packet source",
            },
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
        self.assertEqual(payload["sources"]["packet"]["parser_adapter_selection"]["requested_id"], "demo_live_packet")
        self.assertEqual(payload["sources"]["summary"]["parser_adapter_id"], "star_resonance_tcp")
        self.assertIn("does not support packet", payload["sources"]["summary"]["parser_adapter_fallback_reason"])
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

    def test_panel_refresh_does_not_reuse_recent_diagnose_payload(self) -> None:
        panel = DataSourceHealthPanel.__new__(DataSourceHealthPanel)
        panel.owner = object()
        panel._last_status = {}
        panel._last_refresh_at = 0.0
        panel._last_request_key = ()
        rendered: list[dict] = []
        panel._render_status = lambda status: rendered.append(dict(status))

        diagnose_payload = {"ok": False, "status": "missing", "sources": {"summary": {}}, "diagnostics": [{"level": "error", "message": "missing"}]}
        with mock.patch("gui_modules.sao_gui_data_source_health.act_data_source_diagnose", return_value=diagnose_payload) as diagnose_fn:
            diagnosed = panel.diagnose()

        self.assertEqual(diagnosed["status"], "missing")
        self.assertEqual(panel._last_request_key, ("diagnose",))
        diagnose_fn.assert_called_once_with(panel.owner)

        health_payload = {"ok": True, "status": "running", "sources": {"summary": {"data_source": "hybrid"}}, "latency_ms": 1, "last_event_ms": 2, "errors": []}
        with mock.patch("gui_modules.sao_gui_data_source_health.act_data_source_health", return_value=health_payload) as health_fn:
            refreshed = panel.refresh()

        self.assertEqual(refreshed["status"], "running")
        self.assertEqual(panel._last_request_key, ("health",))
        self.assertEqual(rendered[-1]["status"], "running")
        health_fn.assert_called_once_with(panel.owner)

        with mock.patch("gui_modules.sao_gui_data_source_health.act_data_source_health") as health_fn:
            cached = panel.refresh()

        self.assertEqual(cached["status"], "running")
        health_fn.assert_not_called()

    def test_panel_destroy_resets_render_signatures(self) -> None:
        panel = DataSourceHealthPanel.__new__(DataSourceHealthPanel)
        panel._win = None
        panel._list = None
        panel._diag = None
        panel._last_sources_sig = "sources-stale"
        panel._last_diag_sig = "diag-stale"

        panel.destroy()

        self.assertEqual(panel._last_sources_sig, "")
        self.assertEqual(panel._last_diag_sig, "")

    def test_sources_signature_tracks_displayed_requested_mode_and_uptime(self) -> None:
        base = {
            "packet": {
                "data_source": "hybrid",
                "status": "running",
                "running": True,
                "requested_mode": "packet",
                "uptime_s": 1,
            }
        }
        requested_changed = {"packet": dict(base["packet"], requested_mode="hybrid")}
        uptime_changed = {"packet": dict(base["packet"], uptime_s=2)}

        sig = DataSourceHealthPanel._sources_signature(base)

        self.assertNotEqual(sig, DataSourceHealthPanel._sources_signature(requested_changed))
        self.assertNotEqual(sig, DataSourceHealthPanel._sources_signature(uptime_changed))


if __name__ == "__main__":
    unittest.main()
