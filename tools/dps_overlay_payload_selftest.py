# -*- coding: utf-8 -*-
"""Regression tests for DPS overlay payload numeric guards."""

from __future__ import annotations

import math
import unittest

import _bootstrap  # noqa: F401

from PIL import Image, ImageDraw

from gui_modules.sao_gui_dps import DpsOverlay, _RowState


class DpsOverlayPayloadTests(unittest.TestCase):
    def _hidden_panel(self) -> DpsOverlay:
        panel = DpsOverlay.__new__(DpsOverlay)
        panel._last_report = None
        panel._has_last_report_cb = None
        panel._report_available = False
        panel._visible = False
        panel._gpu_managed = False
        panel._hwnd = 0
        panel._last_snapshot = None
        panel._target_total_damage = 0.0
        panel._target_total_dps = 0.0
        panel._target_total_heal = 0.0
        panel._target_total_hps = 0.0
        panel._target_elapsed = 0.0
        panel._encounter_active = False
        return panel

    def test_live_snapshot_bad_totals_do_not_abort_ingest(self) -> None:
        panel = self._hidden_panel()

        panel._ingest_live_snapshot({
            "total_damage": "not-a-number",
            "total_dps": float("inf"),
            "total_heal": object(),
            "total_hps": "nan",
            "elapsed_s": "oops",
            "entities": [],
        })

        self.assertEqual(panel._target_total_damage, 0.0)
        self.assertEqual(panel._target_total_dps, 0.0)
        self.assertEqual(panel._target_total_heal, 0.0)
        self.assertEqual(panel._target_total_hps, 0.0)
        self.assertEqual(panel._target_elapsed, 0.0)

    def test_row_state_bad_entity_metrics_fall_back_to_zero(self) -> None:
        row = _RowState(7)

        row.update_targets({
            "fight_point": "bad",
            "damage_total": "broken",
            "mem_damage_total": object(),
            "mem_dps": float("inf"),
            "heal_total": "nan",
            "damage_pct": "oops",
            "dps": object(),
            "hps": "invalid",
            "bar_pct": float("nan"),
        })

        self.assertEqual(row.damage_total, 0)
        self.assertEqual(row.mem_damage_total, 0)
        self.assertEqual(row.mem_dps, 0)
        self.assertEqual(row.heal_total, 0)
        self.assertEqual(row.damage_pct, 0.0)
        self.assertEqual(row.target_damage, 0.0)
        self.assertEqual(row.target_dps, 0.0)
        self.assertEqual(row.target_heal, 0.0)
        self.assertEqual(row.target_hps, 0.0)
        self.assertEqual(row.target_bar_pct, 0.0)

    def test_report_rows_bad_metrics_do_not_blank_list(self) -> None:
        panel = DpsOverlay.__new__(DpsOverlay)
        panel._current_tab = "damage"
        panel._view_mode = "report"
        panel._scroll_offset_report = 0
        panel._self_uid = 0
        panel._last_report = {
            "total_damage": "bad-total",
            "entities": [
                {
                    "uid": "bad-uid",
                    "name": "Broken",
                    "damage_total": "bad-damage",
                    "dps": object(),
                    "fight_point": "bad-fp",
                    "mem_damage_total": "bad-mem-total",
                    "mem_dps": float("inf"),
                },
                {"uid": 42, "name": "Valid", "damage_total": 1200, "dps": 100},
            ],
        }

        rows = panel._build_view_rows()

        self.assertEqual(len(rows), 2)
        bad = next(row for row in rows if row["name"] == "Broken")
        self.assertEqual(bad["uid"], 0)
        self.assertEqual(bad["amount"], 0.0)
        self.assertEqual(bad["rate"], 0.0)
        self.assertEqual(bad["fight_point"], 0)
        self.assertEqual(bad["mem_damage_total"], 0)
        self.assertEqual(bad["mem_dps"], 0)
        self.assertTrue(all(math.isfinite(float(row["amount"])) for row in rows))

    def test_act_rows_bad_metrics_do_not_blank_list(self) -> None:
        panel = DpsOverlay.__new__(DpsOverlay)
        panel._current_tab = "damage"
        panel._view_mode = "live"
        panel._scroll_offset = 0
        panel._self_uid = 0
        panel._act_snapshot = {"render_spec": {"totals": {"damage": "bad-total"}}}
        panel._act_render_rows = lambda: [  # type: ignore[method-assign]
            {
                "uid": "bad-uid",
                "name": "Broken ACT",
                "damage": "bad-damage",
                "dps": object(),
                "damage_pct": "bad-pct",
                "fight_point": "bad-fp",
                "mem_damage_total": "bad-mem-total",
                "mem_dps": "nan",
            },
            {"uid": 84, "name": "Valid ACT", "damage": 2400, "dps": 200},
        ]

        rows = panel._build_view_rows()

        self.assertEqual(len(rows), 2)
        bad = next(row for row in rows if row["name"] == "Broken ACT")
        self.assertEqual(bad["uid"], 0)
        self.assertEqual(bad["amount"], 0.0)
        self.assertEqual(bad["rate"], 0.0)
        self.assertEqual(bad["fight_point"], 0)
        self.assertEqual(bad["mem_damage_total"], 0)
        self.assertEqual(bad["mem_dps"], 0)

    def test_list_frame_ignores_bad_row_uid_for_click_regions(self) -> None:
        panel = DpsOverlay.__new__(DpsOverlay)
        panel._row_click_regions = []
        panel._view_mode = "live"
        panel._scroll_offset = 0
        panel._build_view_rows = lambda: [{"uid": "bad-uid"}]  # type: ignore[method-assign]
        panel._draw_scroll_affordance = lambda *args, **kwargs: None  # type: ignore[method-assign]
        panel._draw_row = lambda *args, **kwargs: None  # type: ignore[method-assign]

        img = Image.new("RGBA", (140, 100), (0, 0, 0, 0))
        draw = ImageDraw.Draw(img, "RGBA")
        panel._draw_list_frame(draw, img, 0, 0, 140, 100)

        self.assertEqual(panel._row_click_regions, [])

    def test_detail_report_bad_uid_does_not_block_valid_entity(self) -> None:
        panel = DpsOverlay.__new__(DpsOverlay)
        panel._view_mode = "report"
        panel._detail_uid = 42
        panel._last_report = {
            "entities": [
                {"uid": "bad-uid", "name": "Broken"},
                {"uid": 42, "name": "Valid Detail"},
            ],
        }

        entity = panel._get_detail_entity()

        self.assertIsNotNone(entity)
        self.assertEqual(entity["name"], "Valid Detail")

    def test_detail_view_bad_stats_and_skills_do_not_abort_render(self) -> None:
        panel = DpsOverlay.__new__(DpsOverlay)
        panel._view_mode = "report"
        panel._detail_mode = False
        panel._detail_uid = 42
        panel._skill_scroll_target = 0.0
        panel._skill_scroll_disp = 0.0
        panel._skill_content_h = 0
        panel._skill_view_h = 0
        panel._skill_max_scroll = 0.0
        panel._last_report = {
            "entities": [
                {
                    "uid": 42,
                    "name": "Bad Detail",
                    "profession": "Sword",
                    "fight_point": "bad-fp",
                    "damage_total": object(),
                    "dps": "nan",
                    "heal_total": "bad-heal",
                    "hps": float("inf"),
                    "crit_rate": "bad-crit",
                    "max_hit": object(),
                    "damage_hits": "bad-hits",
                    "elapsed_s": "bad-time",
                    "skills": [
                        {
                            "skill_name": "Broken Skill",
                            "total": "bad-total",
                            "heal_total": object(),
                            "hits": "bad-hit-count",
                            "heal_hits": "bad-heal-hits",
                            "crit_rate": "bad-skill-crit",
                        },
                        {
                            "skill_name": "Valid Skill",
                            "total": 300,
                            "hits": 3,
                            "crit_rate": 0.25,
                        },
                    ],
                },
            ],
        }

        img = Image.new("RGBA", (620, 360), (0, 0, 0, 0))
        draw = ImageDraw.Draw(img, "RGBA")
        panel._draw_detail_view(draw, img, 0, 0, 620, 360)

        self.assertGreaterEqual(panel._skill_content_h, 0)

    def test_act_badge_bad_version_preserves_visible_context(self) -> None:
        panel = DpsOverlay.__new__(DpsOverlay)
        panel._view_mode = "live"
        panel._act_snapshot = {
            "render_spec": {
                "version": "bad-version",
                "mode": "live",
                "sources": {"summary": {"data_source": "mem"}},
                "boss": {"hp_source": "packet"},
            },
        }

        badge = panel._act_badge_text()

        self.assertIn("ACT V1", badge)
        self.assertIn("LIVE", badge)
        self.assertIn("MEM", badge)
        self.assertIn("BOSS PACKET", badge)

    def test_panel_notice_bad_seconds_and_expiry_do_not_abort(self) -> None:
        panel = DpsOverlay.__new__(DpsOverlay)
        panel._visible = False
        panel._win = None
        panel._panel_notice = ""
        panel._panel_notice_until = 0.0
        panel._last_compose_sig = None

        panel._set_panel_notice("Hello", seconds="bad-seconds")
        self.assertEqual(panel._panel_notice_text(), "Hello")

        panel._panel_notice_until = object()
        self.assertEqual(panel._panel_notice_text(), "")


if __name__ == "__main__":
    unittest.main()
