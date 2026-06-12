# -*- coding: utf-8 -*-
"""Regression tests for DPS overlay payload numeric guards."""

from __future__ import annotations

import math
import unittest
from types import SimpleNamespace

import _bootstrap  # noqa: F401

from PIL import Image, ImageDraw

from gui_modules.sao_gui_dps import DpsOverlay, _RowState


class _FakeSettings:
    def __init__(self, values: dict) -> None:
        self.values = dict(values)

    def get(self, key: str, default=None):
        return self.values.get(key, default)


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

    def test_init_bad_position_setting_preserves_other_layout_settings(self) -> None:
        settings = _FakeSettings({
            "dps_ov_x": "bad-x",
            "dps_ov_y": 321,
            "dps_detail_w": 640,
            "dps_detail_h": 500,
            "dps_minimized": True,
            "panel_themes": {},
        })

        panel = DpsOverlay(root=None, settings=settings)

        self.assertEqual(panel._y, 321)
        self.assertEqual(panel._detail_w, 640)
        self.assertEqual(panel._detail_h, 500)
        self.assertTrue(panel._minimized)

    def test_bad_scroll_state_does_not_abort_rows_or_skill_scroll(self) -> None:
        panel = DpsOverlay.__new__(DpsOverlay)
        panel._current_tab = "damage"
        panel._view_mode = "live"
        panel._scroll_offset = "bad-offset"
        panel._scroll_offset_report = "bad-report-offset"
        panel._rows = {}
        panel._act_snapshot = None
        panel._visible = False
        panel._win = None
        panel._disp_total_damage = 0.0
        panel._disp_total_heal = 0.0
        row = _RowState(11)
        row.disp_damage = 100.0
        panel._rows[row.uid] = row

        rows = panel._build_view_rows()
        self.assertEqual(len(rows), 1)
        self.assertEqual(panel._current_scroll_offset(), 0)

        panel._set_scroll_offset("bad-value")
        self.assertEqual(panel._scroll_offset, 0)

        panel._skill_max_scroll = object()
        panel._skill_scroll_target = object()
        panel._scroll_skills(1)
        self.assertEqual(panel._skill_scroll_target, 0.0)

    def test_hit_fx_decimal_string_ids_reach_panel_and_row(self) -> None:
        panel = DpsOverlay(root=None, settings=None)
        panel._visible = True
        panel._gpu_managed = True
        panel._win = object()
        panel._registered = True

        panel._ingest_live_snapshot({
            "encounter_active": True,
            "entities": [{"uid": 11, "name": "Kirito", "damage_total": 100}],
            "hit_fx": {"seq": "2.0", "uid": "11.0", "tier": "mega"},
        })

        self.assertEqual(panel._last_fx_seq, 2)
        self.assertEqual(panel._panel_fx_tier, "mega")
        self.assertEqual(panel._rows[11].fx_tier, "mega")

    def test_decimal_string_self_uid_marks_existing_row(self) -> None:
        panel = DpsOverlay.__new__(DpsOverlay)
        row = _RowState(11)
        panel._rows = {11: row}
        panel._self_uid = 0

        panel.set_self_uid("11.0")

        self.assertEqual(panel._self_uid, 11)
        self.assertTrue(row.is_self)

    def test_bad_detail_size_state_does_not_abort_compute_size(self) -> None:
        panel = DpsOverlay.__new__(DpsOverlay)
        panel._detail_mode = True
        panel._detail_w = object()
        panel._detail_h = "bad-height"

        size = panel._compute_size()

        self.assertEqual(size, (panel.DETAIL_DEFAULT_W, panel.DETAIL_DEFAULT_H))
        self.assertEqual(panel._detail_w, panel.DETAIL_DEFAULT_W)
        self.assertEqual(panel._detail_h, panel.DETAIL_DEFAULT_H)

    def test_bad_skill_scroll_animation_state_does_not_abort_tick_math(self) -> None:
        panel = DpsOverlay.__new__(DpsOverlay)
        panel._detail_visible = True
        panel._skill_scroll_disp = object()
        panel._skill_scroll_target = 96.0
        panel._fade_alpha = 0.0
        panel._fade_target = 0.0
        panel._fade_from = 0.0
        panel._fade_start = 0.0
        panel._fade_duration = 1.0
        panel._disp_total_damage = 0.0
        panel._target_total_damage = 0.0
        panel._disp_total_dps = 0.0
        panel._target_total_dps = 0.0
        panel._disp_total_heal = 0.0
        panel._target_total_heal = 0.0
        panel._disp_total_hps = 0.0
        panel._target_total_hps = 0.0
        panel._disp_elapsed = 0.0
        panel._target_elapsed = 0.0
        panel._rows = {}
        panel._panel_fx_tier = ""

        self.assertTrue(panel._is_animating())
        self.assertTrue(panel._advance_animations(1.0))
        self.assertGreater(panel._skill_scroll_disp, 0.0)

    def test_bad_compose_signature_numeric_state_still_dirty_skips(self) -> None:
        panel = DpsOverlay.__new__(DpsOverlay)
        panel._is_animating = lambda: False  # type: ignore[method-assign]
        panel._rows = {}
        panel._detail_visible = False
        panel._disp_elapsed = "bad-elapsed"
        panel._target_total_damage = object()
        panel._target_total_dps = "nan"
        panel._target_total_heal = float("inf")
        panel._target_total_hps = "bad-hps"
        panel._view_mode = "live"
        panel._current_tab = "damage"
        panel._detail_mode = False
        panel._detail_uid = "11.0"
        panel._minimized = False
        panel._panel_notice = ""
        panel._panel_notice_until = 0.0
        panel._detail_w = object()
        panel._detail_h = "bad-height"
        panel._act_snapshot = None
        panel._fade_alpha = object()

        sig = panel._compose_signature(1.0)

        self.assertIsNotNone(sig)
        self.assertEqual(sig[0:5], (0, 0, 0, 0, 0))
        self.assertEqual(sig[9], 11)
        self.assertEqual(sig[12], panel.DETAIL_DEFAULT_W)
        self.assertEqual(sig[13], panel.DETAIL_DEFAULT_H)
        self.assertEqual(sig[16], 0.0)

    def test_gpu_event_bad_numeric_state_does_not_abort(self) -> None:
        panel = DpsOverlay.__new__(DpsOverlay)
        panel._x = object()
        panel._y = "bad-y"

        ev = panel._gpu_event(object(), "bad-local-y", delta="bad-delta")

        self.assertEqual((ev.x, ev.y, ev.x_root, ev.y_root, ev.delta), (0, 0, 0, 0, 0))

    def test_mouse_wheel_string_delta_still_scrolls(self) -> None:
        panel = DpsOverlay.__new__(DpsOverlay)
        panel._detail_visible = False
        seen = []
        panel._scroll = lambda direction: seen.append(direction)  # type: ignore[method-assign]

        panel._on_mouse_wheel(SimpleNamespace(delta="120.0"))

        self.assertEqual(seen, [-1])

    def test_resize_drag_decimal_state_updates_detail_size(self) -> None:
        panel = DpsOverlay.__new__(DpsOverlay)
        panel._list_drag_active = False
        panel._resize_active = True
        panel._drag_moved = False
        panel._resize_start_root = ("10.0", "20.0")
        panel._resize_start_size = ("640.0", "500.0")
        panel._detail_w = "640.0"
        panel._detail_h = "500.0"
        panel._shell_cache = None
        panel._last_compose_sig = None
        panel._gpu_managed = False
        panel._gpu_window = None
        panel._win = None
        panel._visible = False

        panel._on_drag_move(SimpleNamespace(x_root="70.0", y_root="80.0"))

        self.assertEqual(panel._detail_w, 700)
        self.assertEqual(panel._detail_h, 560)
        self.assertTrue(panel._drag_moved)


if __name__ == "__main__":
    unittest.main()
