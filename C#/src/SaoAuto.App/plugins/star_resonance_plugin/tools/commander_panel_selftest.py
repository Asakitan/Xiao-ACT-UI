# -*- coding: utf-8 -*-
"""Regression tests for Commander Tk data propagation and render signatures."""

from __future__ import annotations

import unittest
from unittest import mock

import _bootstrap  # noqa: F401

from plugins.star_resonance_plugin.panels.sao_gui_commander import CommanderPanel
from gui_modules.sao_gui_panels_mixin import SAOPlayerGUIPanelsMixin


def _bad_commander_data() -> dict:
    return {
        "team_id": "bad-team",
        "leader_uid": "leader-x",
        "dungeon_id": "inf",
        "self_uid": "self-x",
        "members": [
            {
                "uid": "member-x",
                "name": "Alice",
                "profession": "Tank",
                "fight_point": "nan",
                "level": "bad",
                "is_self": True,
                "is_leader": True,
                "hp": "nan",
                "max_hp": "inf",
                "skill_slots": [
                    {
                        "index": "slot-x",
                        "state": "cooldown",
                        "cooldown_pct": "nan",
                        "remaining_ms": "inf",
                    }
                ],
            }
        ],
    }


class _FakeCommanderPanel:
    def __init__(self) -> None:
        self.updates: list[dict] = []

    def is_visible(self) -> bool:
        return True

    def update(self, data: dict) -> None:
        self.updates.append(data)


class _FakePacketEngine:
    def __init__(self, data: dict) -> None:
        self.data = data

    def get_commander_data(self) -> dict:
        return self.data


class CommanderPanelTests(unittest.TestCase):
    def test_panel_signature_tolerates_non_finite_live_values(self) -> None:
        panel = CommanderPanel.__new__(CommanderPanel)
        panel._body = object()
        panel._canvas = None
        panel._active_tab = "team"
        panel._data = _bad_commander_data()
        panel._last_signature = None

        with (
            mock.patch("plugins.star_resonance_plugin.panels.sao_gui_commander.clear_frame"),
            mock.patch.object(CommanderPanel, "_render_team_tab", lambda self: None),
        ):
            panel._render_if_needed(force=False)
            first = panel._last_signature
            panel._render_if_needed(force=False)

        self.assertEqual(first, panel._last_signature)
        self.assertNotIn("nan", repr(first).lower())
        self.assertNotIn("inf", repr(first).lower())

    def test_push_commander_data_does_not_drop_bad_numeric_payloads(self) -> None:
        owner = SAOPlayerGUIPanelsMixin.__new__(SAOPlayerGUIPanelsMixin)
        owner._commander_panel = _FakeCommanderPanel()
        owner._packet_engine = _FakePacketEngine(_bad_commander_data())
        owner._last_commander_push_sig = None

        owner._push_commander_data()
        owner._push_commander_data()

        self.assertEqual(len(owner._commander_panel.updates), 1)
        self.assertEqual(owner._commander_panel.updates[0]["members"][0]["name"], "Alice")


if __name__ == "__main__":
    unittest.main()
