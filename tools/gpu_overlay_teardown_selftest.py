# -*- coding: utf-8 -*-
"""Regression coverage for GPU overlay teardown ordering."""

from __future__ import annotations

import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gui_modules.sao_child_bar_gpu import ChildBarGpuPainter
from gui_modules.sao_child_bar_gpu import BarColors, _ChildBarSnapshot, _RowSnapshot
from gui_modules.sao_gui_bosshp import BossHpOverlay
from gui_modules.sao_gui_hp import HpOverlay
from gui_modules.sao_gui_menu_hud import MenuHudOverlay
from gui_modules.sao_left_info_gpu import (
    LeftInfoGpuPainter, PlayerPanelGpuPainter, SessionPlayersGpuPainter,
    _PlayerPanelSnapshot, _SessionPlayersSnapshot,
)
from gui_modules.sao_menu_bar_gpu import _ButtonSnapshot
from gui_modules.sao_menu_bar_gpu import MenuBarGpuPainter


class _Worker:
    def __init__(self, order):
        self.order = order

    def stop(self) -> None:
        self.order.append("worker.stop")


class _Presenter:
    def __init__(self, order):
        self.order = order
        self.release_count = 0

    def release(self) -> None:
        self.release_count += 1
        self.order.append("presenter.release")

    def render(self):
        return None


class _GpuWindow:
    def __init__(self, order):
        self.order = order
        self.destroy_count = 0

    def destroy(self) -> None:
        self.destroy_count += 1
        self.order.append("gpu_window.destroy")


class _Renderer:
    def __init__(self) -> None:
        self.reset_count = 0

    def reset(self) -> None:
        self.reset_count += 1


def _owned_pair():
    order = []
    return order, _Presenter(order), _GpuWindow(order)


class GpuOverlayTeardownTests(unittest.TestCase):
    def _assert_window_owns_presenter(self, obj, presenter_attr: str) -> None:
        order, presenter, gpu_window = _owned_pair()
        if presenter_attr == "_gpu_presenter":
            obj._gpu_presenter = presenter
        else:
            obj._presenter = presenter
        obj._gpu_window = gpu_window
        obj._render_worker = _Worker(order)

        obj.destroy()

        self.assertIn("gpu_window.destroy", order)
        self.assertNotIn("presenter.release", order)
        self.assertEqual(gpu_window.destroy_count, 1)
        self.assertEqual(presenter.release_count, 0)
        self.assertIsNone(obj._gpu_window)
        self.assertIsNone(getattr(obj, presenter_attr))

    def test_hp_overlay_destroys_gpu_window_before_dropping_presenter(self) -> None:
        obj = object.__new__(HpOverlay)
        obj._registered = False
        obj._gpu_managed = True
        obj._hwnd = 123
        obj._win = None
        obj._gpu_drag_active = False
        obj._input_passthrough_enabled = None
        obj._input_region_mode = None
        obj._drag_input_ignored = False
        obj._visible = True
        obj._exiting = True
        obj._hide_after_exit = True

        self._assert_window_owns_presenter(obj, "_gpu_presenter")

    def test_bosshp_overlay_destroys_gpu_window_before_dropping_presenter(self) -> None:
        obj = object.__new__(BossHpOverlay)
        obj._registered = False
        obj._gpu_managed = True
        obj._hwnd = 123
        obj._win = None
        obj._gpu_drag_active = False
        obj._visible = True
        obj._exiting = True
        obj._hide_after_exit = True

        self._assert_window_owns_presenter(obj, "_gpu_presenter")

    def test_left_info_painters_leave_presenter_to_gpu_window_teardown(self) -> None:
        for cls in (LeftInfoGpuPainter, SessionPlayersGpuPainter, PlayerPanelGpuPainter):
            with self.subTest(cls=cls.__name__):
                obj = object.__new__(cls)
                obj._destroyed = False
                self._assert_window_owns_presenter(obj, "_presenter")

    def test_menu_painters_leave_presenter_to_gpu_window_teardown(self) -> None:
        child = object.__new__(ChildBarGpuPainter)
        child._destroyed = False
        self._assert_window_owns_presenter(child, "_presenter")

        bar = object.__new__(MenuBarGpuPainter)
        bar._destroyed = False
        bar._hover_idx = None
        bar._leave_cb = None
        bar._target_geom = (0, 0, 1, 1)
        self._assert_window_owns_presenter(bar, "_presenter")

        hud = object.__new__(MenuHudOverlay)
        hud._destroyed = False
        hud._hwnd = 0
        hud._win = None
        hud._visible = True
        hud._renderer = _Renderer()
        self._assert_window_owns_presenter(hud, "_gpu_presenter")

    def test_presenter_releases_when_gpu_window_was_never_created(self) -> None:
        order = []
        obj = object.__new__(ChildBarGpuPainter)
        obj._destroyed = False
        obj._render_worker = _Worker(order)
        obj._presenter = _Presenter(order)
        obj._gpu_window = None

        obj.destroy()

        self.assertIn("presenter.release", order)
        self.assertEqual(obj._presenter, None)

    def test_session_players_snapshot_tolerates_bad_numeric_fields(self) -> None:
        snap = _SessionPlayersSnapshot(
            rows=[("Alice", "1", "bad", True)],
            total="inf",
            self_uid=123,
            first_index="bad",
            w="nan",
            h=None,
            reveal="bad",
        )

        self.assertEqual(snap.total, 0)
        self.assertEqual(snap.first_index, 0)
        self.assertEqual(snap.w, 1)
        self.assertEqual(snap.h, 1)
        self.assertEqual(snap.reveal, 1.0)
        self.assertEqual(snap.rows[0], ("Alice", "1", "bad", True))

    def test_player_panel_snapshot_tolerates_bad_numeric_fields(self) -> None:
        snap = _PlayerPanelSnapshot(
            username="Kirito",
            level="nan",
            level_extra="bad",
            season_exp=float("inf"),
            hp=("bad", float("inf")),
            sta=(None, "bad"),
            shift_mode="burst",
            top_w="bad",
            top_h=float("nan"),
            bottom_w=-5,
            bottom_h=None,
            scan_phase=float("inf"),
        )

        self.assertEqual(snap.level, 0)
        self.assertEqual(snap.level_extra, 0)
        self.assertEqual(snap.season_exp, 0)
        self.assertEqual(snap.hp, (0, 0))
        self.assertEqual(snap.sta, (0, 0))
        self.assertEqual((snap.top_w, snap.top_h, snap.bottom_w, snap.bottom_h), (1, 1, 1, 1))
        self.assertEqual(snap.scan_phase, 0.0)

    def test_child_bar_snapshots_tolerate_bad_numeric_fields(self) -> None:
        row = _RowSnapshot(icon=">", label="Submenu", hover_t="bad", row_w="nan")
        colors = BarColors(
            "#010101",
            "#202020",
            "#ffffff",
            "#ffd27a",
            "#86dfff",
            "#f3af12",
            lambda a, _b, _t: a,
        )
        snap = _ChildBarSnapshot(
            line_w="bad",
            line_h=float("inf"),
            arrow_w=-5,
            fade_t=float("nan"),
            rows=[row],
            bg_hex="#010101",
            colors=colors,
        )

        self.assertEqual(row.hover_t, 0.0)
        self.assertEqual(row.row_w, 1)
        self.assertEqual((snap.line_w, snap.line_h, snap.arrow_w), (1, 1, 1))
        self.assertEqual(snap.fade_t, 0.0)
        self.assertEqual(snap.rows, [row])

    def test_menu_bar_button_snapshot_tolerates_bad_numeric_fields(self) -> None:
        snap = _ButtonSnapshot(size="nan", hover_t=float("inf"), active=True, icon="")

        self.assertEqual(snap.size, 1.0)
        self.assertEqual(snap.hover_t, 0.0)
        self.assertTrue(snap.active)
        self.assertEqual(snap.icon, "●")


if __name__ == "__main__":
    unittest.main()
