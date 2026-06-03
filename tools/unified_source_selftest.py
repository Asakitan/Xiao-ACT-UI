# -*- coding: utf-8 -*-
"""Small contract tests for mem_probe.unified_source.UnifiedDataSource."""

from __future__ import annotations

import types
import unittest
from unittest import mock

from mem_probe import unified_source


class FakeMemStateBridge:
    def __init__(self, **kwargs):
        self.kwargs = kwargs
        self.mode = "init"
        self.last_error = ""
        self.last_uid = 0
        self.last_hp = 0
        self.last_max_hp = 0
        self.last_profession_id = 0
        self.last_char_name = ""
        self.last_skill_cd_count = 0
        self.last_is_dead = False
        self._provider = None
        self._snap = types.SimpleNamespace(
            uid=36668136,
            hp=120,
            max_hp=200,
            profession_id=1,
            name="tester",
            skill_cds=[{"slot": 1}],
        )
        self.stopped = False

    def start(self) -> bool:
        self.mode = "memory"
        self.last_uid = 36668136
        self.last_hp = 120
        self.last_max_hp = 200
        self.last_profession_id = 1
        self.last_char_name = "tester"
        self.last_skill_cd_count = 1
        self._provider = object()
        on_log = self.kwargs.get("on_log")
        if callable(on_log):
            on_log("[FakeMemBridge] mode='memory'")
        return True

    def stop(self) -> None:
        self.stopped = True
        self._provider = None

    def snapshot(self):
        return self._snap

    @property
    def is_memory_active(self) -> bool:
        return self.mode == "memory"


class UnifiedSourceContractTests(unittest.TestCase):
    def test_start_health_and_callbacks_are_json_safe(self) -> None:
        statuses = []
        self_updates = []
        packet_bridge = object()
        with mock.patch.object(unified_source, "MemStateBridge", FakeMemStateBridge):
            source = unified_source.UnifiedDataSource(
                state_mgr=object(),
                mode="hybrid",
                packet_bridge=packet_bridge,
                on_status_change=lambda status, error="": statuses.append((status, error)),
                on_self_update=self_updates.append,
            )
            self.assertIs(source._bridge.kwargs["packet_bridge"], packet_bridge)
            self.assertTrue(source.start())
            health = source.health()
            source.stop()

        self.assertIn(("starting", ""), statuses)
        self.assertIn(("running", ""), statuses)
        self.assertEqual(statuses[-1], ("stopped", ""))
        self.assertEqual(self_updates[0]["uid"], 36668136)
        self.assertEqual(health["data_source"], "unified")
        self.assertEqual(health["requested_mode"], "hybrid")
        self.assertEqual(health["mode"], "memory")
        self.assertTrue(health["running"])
        self.assertTrue(health["alive"])
        self.assertTrue(health["is_memory_active"])
        self.assertEqual(health["watchers"]["self"], "memory_first")
        self.assertEqual(health["watchers"]["boss"], "tcp_fallback")
        self.assertEqual(health["self"]["uid"], 36668136)
        self.assertTrue(health["snapshot_available"])

    def test_failed_start_reports_error_without_throwing(self) -> None:
        class FailingBridge(FakeMemStateBridge):
            def start(self) -> bool:
                self.last_error = "boom"
                return False

        statuses = []
        with mock.patch.object(unified_source, "MemStateBridge", FailingBridge):
            source = unified_source.UnifiedDataSource(
                state_mgr=object(),
                mode="memory",
                on_status_change=lambda status, error="": statuses.append((status, error)),
            )
            self.assertFalse(source.start())
            health = source.health()

        self.assertEqual(statuses[-1], ("error", "boom"))
        self.assertFalse(health["running"])
        self.assertEqual(health["last_error"], "boom")


if __name__ == "__main__":
    unittest.main()
