# -*- coding: utf-8 -*-
# Regression tests for live Tk panel render signatures and numeric guards.

from __future__ import annotations

import unittest
from unittest import mock

import _bootstrap  # noqa: F401

from plugins.star_resonance_plugin.panels.sao_gui_autokey import AutoKeyPanel
from plugins.star_resonance_plugin.panels.sao_gui_bossraid import BossRaidPanel


class TkLivePanelSignatureTests(unittest.TestCase):
    def _bossraid_panel(self) -> BossRaidPanel:
        panel = BossRaidPanel.__new__(BossRaidPanel)
        panel._current_tab = "entities"
        panel._cfg = {"profiles": []}
        panel._status = {
            "state": "running",
            "phase_idx": 0,
            "elapsed_s": 12.0,
            "dps": 1000,
            "entities": [
                {
                    "uuid": 9001,
                    "name": "Alpha",
                    "role": "boss",
                    "hp_pct": 0.5,
                    "damage_dealt": 1234,
                    "shield_active": False,
                    "breaking_stage": 0,
                    "in_overdrive": False,
                }
            ],
        }
        return panel

    def test_bossraid_entity_signature_tracks_visible_name(self) -> None:
        panel = self._bossraid_panel()
        base = panel._build_signature()
        panel._status["entities"][0]["name"] = "Beta"

        self.assertNotEqual(base, panel._build_signature())

    def test_bossraid_entity_signature_is_stable_for_non_finite_numbers(self) -> None:
        panel = self._bossraid_panel()
        entity = panel._status["entities"][0]
        entity["hp_pct"] = "nan"
        entity["damage_dealt"] = "inf"
        entity["breaking_stage"] = "-inf"
        panel._status["elapsed_s"] = "nan"
        panel._status["dps"] = "inf"

        first = panel._build_signature()
        second = panel._build_signature()

        self.assertEqual(first, second)
        self.assertNotIn("nan", repr(first).lower())
        self.assertNotIn("inf", repr(first).lower())

    def test_bossraid_memory_summary_tolerates_bad_counts(self) -> None:
        class _Widget:
            def pack(self, *args, **kwargs):
                return None

        panel = self._bossraid_panel()
        panel._rx_container = _Widget()

        with (
            mock.patch("plugins.star_resonance_plugin.panels.sao_gui_bossraid.tk.Label", return_value=_Widget()),
            mock.patch("plugins.star_resonance_plugin.panels.sao_gui_bossraid.panel_font", return_value=("Segoe UI", 8)),
        ):
            panel._render_boss_summary({
                "skill_count": "bad",
                "mechanic_count": float("nan"),
                "hp_line_count": "bad",
                "approx_duration_ms": "bad",
            })

    def test_autokey_slot_normalization_clamps_non_finite_numbers(self) -> None:
        panel = AutoKeyPanel.__new__(AutoKeyPanel)

        slots = panel._normalize_slots(
            [
                {
                    "index": "1",
                    "skill_name": "Burst",
                    "state": "cooldown",
                    "cooldown_pct": "nan",
                    "remaining_ms": "inf",
                    "charge_count": "bad",
                },
                {
                    "slot_index": "2",
                    "name": "Ready",
                    "cooldown_pct": 2.5,
                    "remaining_ms": -100,
                    "charge_count": 3,
                },
            ]
        )

        self.assertEqual(slots[0]["cooldown_pct"], 0.0)
        self.assertEqual(slots[0]["remaining_ms"], 0.0)
        self.assertEqual(slots[0]["charge_count"], 0)
        self.assertEqual(slots[1]["cooldown_pct"], 1.0)
        self.assertEqual(slots[1]["remaining_ms"], 0.0)
        self.assertEqual(slots[1]["charge_count"], 3)


if __name__ == "__main__":
    unittest.main()
