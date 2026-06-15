# -*- coding: utf-8 -*-
"""Regression tests for local ACT report/history HTTP API."""

from __future__ import annotations

import _bootstrap  # noqa: F401

import os
import tempfile
import unittest
from unittest import mock

from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from engines import dps_history
from server import app as server_app


class ActServerApiTests(unittest.TestCase):
    def _client_with_store(self):
        tmp = tempfile.TemporaryDirectory(prefix="act_server_api_")
        self.addCleanup(tmp.cleanup)
        store = dps_history.DpsHistoryStore(path=os.path.join(tmp.name, "history.json"), limit=10)
        store.add_report({
            "encounter_id": "enc-api",
            "report_reason": "api-test",
            "elapsed_s": 30,
            "total_damage": 300,
            "entities": [{"uid": 1, "name": "Kirito", "damage_total": 300}],
            "actions": [{
                "topic": "damage",
                "time_ms": 1000,
                "payload": {"actor_name": "Kirito", "target_name": "Boss", "skill_name": "Slash", "damage": 300},
                "source_name": "fixture",
                "source_kind": "replay",
            }],
        })
        client = TestClient(server_app.app)
        return client, store

    def test_act_health_and_latest_report_contract(self) -> None:
        client, store = self._client_with_store()
        with mock.patch.object(server_app, "_act_history_store", return_value=store):
            health = client.get("/api/act/health")
            latest = client.get("/api/act/reports/latest")

        self.assertEqual(health.status_code, 200)
        self.assertTrue(health.json()["ok"])
        self.assertIn("sqlite", health.json()["storage_status"])
        self.assertEqual(latest.status_code, 200)
        self.assertTrue(latest.json()["ok"])
        self.assertEqual(latest.json()["report"]["encounter_id"], "enc-api")

    def test_act_history_and_action_rows_contract(self) -> None:
        client, store = self._client_with_store()
        with mock.patch.object(server_app, "_act_history_store", return_value=store):
            history = client.get("/api/act/reports?limit=5")
            loaded = client.get("/api/act/reports/0")
            actions = client.get("/api/act/actions?limit=5&encounter_id=enc-api")

        self.assertEqual(history.status_code, 200)
        self.assertEqual(history.json()["items"][0]["encounter_id"], "enc-api")
        self.assertEqual(history.json()["cursor"]["limit"], 5)
        self.assertEqual(loaded.status_code, 200)
        self.assertEqual(loaded.json()["report"]["entities"][0]["name"], "Kirito")
        self.assertEqual(actions.status_code, 200)
        self.assertEqual(actions.json()["items"][0]["topic"], "damage")
        self.assertEqual(actions.json()["items"][0]["skill_name"], "Slash")

    def test_remote_act_read_requires_explicit_enablement(self) -> None:
        client, store = self._client_with_store()
        with mock.patch.object(server_app, "_act_history_store", return_value=store):
            response = client.get("/api/act/health", headers={"host": "192.0.2.10:9983"})

        self.assertEqual(response.status_code, 403)

    def test_act_report_websocket_snapshot_and_actions(self) -> None:
        client, store = self._client_with_store()
        with mock.patch.object(server_app, "_act_history_store", return_value=store):
            with client.websocket_connect("/ws/act/reports") as ws:
                initial = ws.receive_json()
                ws.send_json({"type": "actions", "limit": 5, "encounter_id": "enc-api"})
                actions = ws.receive_json()

        self.assertEqual(initial["type"], "snapshot")
        self.assertEqual(initial["latest"]["encounter_id"], "enc-api")
        self.assertEqual(actions["type"], "actions")
        self.assertEqual(actions["items"][0]["topic"], "damage")

    def test_remote_act_websocket_requires_explicit_enablement(self) -> None:
        client, store = self._client_with_store()
        with mock.patch.object(server_app, "_act_history_store", return_value=store):
            with self.assertRaises(WebSocketDisconnect):
                with client.websocket_connect("/ws/act/reports", headers={"host": "192.0.2.10:9983"}):
                    pass


if __name__ == "__main__":
    unittest.main()
