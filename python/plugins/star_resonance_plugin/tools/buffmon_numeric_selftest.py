# -*- coding: utf-8 -*-
"""Numeric guard checks for BuffMon overlay row and payload handling."""

from __future__ import annotations

import _bootstrap  # noqa: F401

from pathlib import Path
import time
import unittest

from PIL import Image, ImageDraw

from plugins.star_resonance_plugin.panels.sao_gui_buffmon import BossBuffOverlay, SelfBuffOverlay, _BuffPanelBase, _finite_float, _finite_int


class BuffMonNumericTests(unittest.TestCase):
    def test_row_rendering_filters_bad_numeric_values(self) -> None:
        source = (Path(__file__).resolve().parents[1] / "gui_modules" / "sao_gui_buffmon.py").read_text(encoding="utf-8")

        self.assertNotIn("layer = int(row.get('layer', 0) or 0)", source)
        self.assertNotIn("applies = int(row.get('apply_count', 0) or 0)", source)
        self.assertNotIn("int(r.get('rem_s', -1) * 10)", source)
        self.assertEqual(_finite_float(float("nan"), -1.0), -1.0)
        self.assertEqual(_finite_int(float("inf"), 5, lo=0), 5)

        panel = _BuffPanelBase.__new__(_BuffPanelBase)
        img = Image.new("RGBA", (260, 90), (0, 0, 0, 0))
        draw = ImageDraw.Draw(img, "RGBA")
        panel._draw_row(draw, img, {
            "name": "Bad Buff Payload",
            "rem_s": float("nan"),
            "layer": float("inf"),
            "count": "bad",
            "apply_count": float("nan"),
            "uptime_pct": float("inf"),
        }, 0, 30, 220)
        self.assertTrue(panel._row_sig([{"rem_s": float("nan"), "layer": float("inf"), "count": "bad"}]))

    def test_row_signature_tracks_visible_uptime_and_apply_count(self) -> None:
        panel = _BuffPanelBase.__new__(_BuffPanelBase)
        base = [{
            "id": 100,
            "uuid": 1,
            "name": "Good Buff",
            "rem_s": 12.0,
            "layer": 1,
            "count": 1,
            "uptime_pct": 0.25,
            "apply_count": 1,
        }]
        uptime_changed = [dict(base[0], uptime_pct=0.75)]
        apply_count_changed = [dict(base[0], apply_count=3)]

        self.assertNotEqual(panel._row_sig(base), panel._row_sig(uptime_changed))
        self.assertNotEqual(panel._row_sig(base), panel._row_sig(apply_count_changed))

    def test_row_signature_tracks_dynamic_header_label(self) -> None:
        panel = BossBuffOverlay.__new__(BossBuffOverlay)
        rows = [{"id": 100, "uuid": 1, "name": "Good Buff", "rem_s": 12.0, "layer": 1, "count": 1}]
        panel._target_name = "Alpha"
        first = panel._row_sig(rows)
        panel._target_name = "Beta"

        self.assertNotEqual(first, panel._row_sig(rows))

    def test_self_buff_update_skips_bad_ids_and_keeps_good_rows(self) -> None:
        panel = SelfBuffOverlay.__new__(SelfBuffOverlay)
        panel._buff_cache = {}
        panel._server_offset_ms = 0.0
        panel._filter_mode = "all"
        panel._dbg_first_data = True
        panel._dbg_first_rows = True
        now_ms = int(time.time() * 1000)

        panel.update_buffs([
            {"id": "bad", "name": "skip me"},
            {
                "id": "100",
                "uuid": "bad",
                "name": "Good Buff",
                "begin_ms": now_ms,
                "duration_ms": 5000,
                "layer": "bad",
                "count": float("inf"),
            },
        ], server_offset_ms=float("nan"), uptime={
            "buffs": [
                {"buff_id": "bad", "uptime_pct": 0.5, "apply_count": 2},
                {"buff_id": "100", "uptime_pct": float("inf"), "apply_count": "bad"},
            ],
        })

        rows = panel.get_rows()
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["id"], 100)
        self.assertEqual(rows[0]["uuid"], 0)
        self.assertEqual(rows[0]["layer"], 0)
        self.assertEqual(rows[0]["count"], 0)
        self.assertEqual(rows[0]["uptime_pct"], -1.0)
        self.assertEqual(rows[0]["apply_count"], 0)


if __name__ == "__main__":
    unittest.main()
