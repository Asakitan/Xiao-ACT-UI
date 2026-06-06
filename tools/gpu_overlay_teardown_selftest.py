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
from gui_modules.sao_gui_bosshp import BossHpOverlay
from gui_modules.sao_gui_hp import HpOverlay
from gui_modules.sao_gui_menu_hud import MenuHudOverlay
from gui_modules.sao_left_info_gpu import (
    LeftInfoGpuPainter, PlayerPanelGpuPainter, SessionPlayersGpuPainter,
)
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


if __name__ == "__main__":
    unittest.main()
