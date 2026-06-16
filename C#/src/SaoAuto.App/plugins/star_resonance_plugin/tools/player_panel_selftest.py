# -*- coding: utf-8 -*-
"""Regression tests for SAOPlayerPanel live-state update paths."""

from __future__ import annotations

import _bootstrap  # noqa: F401

import unittest

from gui_modules.sao_player_panel import SAOPlayerPanel


def _panel(active: bool = True, gpu: bool = True):
    panel = SAOPlayerPanel.__new__(SAOPlayerPanel)
    panel._active = active
    panel._gpu_managed = gpu
    panel._gpu_required = gpu
    panel._target_w = 240
    panel._top_h = 240
    panel._username = "Old"
    panel._profession = "Warrior"
    panel._level = 1
    panel._level_extra = 0
    panel._season_exp = 0
    panel._sta_hp = (10, 100)
    panel._sta_sta = (20, 100)
    panel.calls = []
    panel._dispatch_gpu_paint = lambda: panel.calls.append(
        ("gpu", panel._username, panel._sta_hp, panel._sta_sta)
    )
    panel._redraw_top = lambda w, h: panel.calls.append(
        ("tk", w, h, panel._username, panel._sta_hp, panel._sta_sta)
    )
    return panel


class PlayerPanelUpdateTests(unittest.TestCase):
    def test_update_vitals_repaints_active_gpu_without_level_change(self) -> None:
        panel = _panel()

        changed = panel.update_vitals((11, 100), (21, 100))

        self.assertTrue(changed)
        self.assertEqual(panel._sta_hp, (11, 100))
        self.assertEqual(panel._sta_sta, (21, 100))
        self.assertEqual(panel.calls, [("gpu", "Old", (11, 100), (21, 100))])

    def test_update_profile_repaints_active_gpu_without_level_change(self) -> None:
        panel = _panel()

        changed = panel.update_profile("New", "Mage")

        self.assertTrue(changed)
        self.assertEqual(panel._username, "New")
        self.assertEqual(panel._profession, "Mage")
        self.assertEqual(panel.calls, [("gpu", "New", (10, 100), (20, 100))])

    def test_deferred_updates_are_painted_by_next_level_refresh(self) -> None:
        panel = _panel()

        panel.update_profile("New", "Mage", repaint=False)
        panel.update_vitals((30, 100), (40, 100), repaint=False)
        panel.update_level(2, 0, 5)

        self.assertEqual(panel.calls, [("gpu", "New", (30, 100), (40, 100))])

    def test_update_vitals_repaints_active_tk_fallback(self) -> None:
        panel = _panel(gpu=False)

        panel.update_vitals((12, 100), (22, 100))

        self.assertEqual(
            panel.calls,
            [("tk", 240, 240, "Old", (12, 100), (22, 100))],
        )


if __name__ == "__main__":
    unittest.main()
