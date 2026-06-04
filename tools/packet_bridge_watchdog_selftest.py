# -*- coding: utf-8 -*-
"""Focused PacketBridge watchdog regression tests."""
from __future__ import annotations

import os
import sys
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from net import packet_bridge


class _FakeThread:
    def __init__(self, alive: bool = True):
        self._alive = bool(alive)

    def is_alive(self) -> bool:
        return self._alive


class _FakeCapture:
    def __init__(self, *, raw: int, game: int, alive: bool = True):
        self.stats = {"raw_frames": raw, "complete_game_frames": game}
        self._thread = _FakeThread(alive)
        self.force_reconnects: list[str] = []
        self.restart_requests: list[str] = []
        self.stop_count = 0
        self.start_count = 0

    def force_reconnect(self, reason: str = "watchdog") -> bool:
        self.force_reconnects.append(reason)
        return True

    def request_restart(self, reason: str = "watchdog") -> bool:
        self.restart_requests.append(reason)
        return True

    def stop(self) -> None:
        self.stop_count += 1

    def start(self) -> None:
        self.start_count += 1


def _bridge_with_capture(cap: _FakeCapture):
    bridge = packet_bridge.PacketBridge.__new__(packet_bridge.PacketBridge)
    bridge._capture = cap
    bridge._last_capture_watchdog_ts = 0.0
    bridge._last_capture_restart_ts = 0.0
    bridge._last_capture_raw_seen = cap.stats["raw_frames"]
    bridge._last_capture_raw_seen_ts = 95.0
    bridge._last_capture_game_seen = cap.stats["complete_game_frames"]
    bridge._last_capture_game_seen_ts = 98.0
    bridge._last_packet_frame_t = 99.0
    bridge._server_found_printed = True
    return bridge


class PacketBridgeWatchdogTests(unittest.TestCase):
    def test_player_idle_does_not_force_reconnect_when_game_frames_are_active(self) -> None:
        cap = _FakeCapture(raw=100, game=20, alive=True)
        bridge = _bridge_with_capture(cap)
        with mock.patch.object(packet_bridge.time, "time", return_value=100.0):
            bridge._maybe_recover_capture_idle(20.0)
        self.assertEqual(cap.force_reconnects, [])
        self.assertEqual(cap.restart_requests, [])

    def test_no_game_frames_can_force_reconnect_after_long_stall(self) -> None:
        cap = _FakeCapture(raw=100, game=20, alive=True)
        bridge = _bridge_with_capture(cap)
        bridge._last_capture_raw_seen_ts = 60.0
        bridge._last_capture_game_seen_ts = 60.0
        bridge._last_packet_frame_t = 60.0
        with mock.patch.object(packet_bridge.time, "time", return_value=100.0):
            bridge._maybe_recover_capture_idle(40.0)
        self.assertTrue(cap.force_reconnects)
        self.assertIn("no_game_frames_40s", cap.force_reconnects[0])

    def test_dead_capture_thread_restarts_capture(self) -> None:
        cap = _FakeCapture(raw=100, game=20, alive=False)
        bridge = _bridge_with_capture(cap)
        with mock.patch.object(packet_bridge.time, "time", return_value=100.0):
            bridge._maybe_recover_capture_idle(40.0)
        self.assertEqual(cap.stop_count, 1)
        self.assertEqual(cap.start_count, 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
