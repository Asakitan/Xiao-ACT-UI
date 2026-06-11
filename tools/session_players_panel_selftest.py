# -*- coding: utf-8 -*-
"""Regression tests for the Entity session players panel update signature."""

from __future__ import annotations

import _bootstrap  # noqa: F401

import unittest

from gui_modules.sao_session_players_panel import (
    SAOSessionPlayersPanel,
    _session_rows_render_signature,
)


def _row(power: str):
    return {
        "uid": "1",
        "name": "Alice",
        "fight_power": power,
        "is_self": False,
    }


class SessionPlayersPanelSignatureTests(unittest.TestCase):
    def test_render_signature_tracks_power_text_without_numeric_value(self) -> None:
        self.assertNotEqual(
            _session_rows_render_signature([_row("100")]),
            _session_rows_render_signature([_row("200")]),
        )

    def test_force_update_does_not_return_on_same_signature(self) -> None:
        rows = [_row("100")]
        panel = SAOSessionPlayersPanel.__new__(SAOSessionPlayersPanel)
        panel._rows_sig = _session_rows_render_signature(rows)
        panel._rows_data = ["old"]
        panel._rendered_count = 99
        panel._rows_self_uid = ""
        panel._gpu_managed = True
        panel._first_visible_row = 4
        panel._visible_row_count = 7
        panel._cached_screen_xy = "cached"
        panel.calls = []
        panel._dispatch_gpu_paint = lambda: panel.calls.append("paint")
        panel._queue_gpu_drain = lambda *args, **kwargs: panel.calls.append("drain")

        panel.update_rows(rows, force=True)

        self.assertEqual(panel._rendered_count, 0)
        self.assertEqual(panel._first_visible_row, 0)
        self.assertIsNone(panel._cached_screen_xy)
        self.assertEqual(panel.calls, ["paint", "drain"])


if __name__ == "__main__":
    unittest.main()
