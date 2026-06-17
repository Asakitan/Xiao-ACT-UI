# -*- coding: utf-8 -*-
"""Regression coverage for Entity fisheye + floating panel interaction."""

from __future__ import annotations

import os
import sys
import threading
import time
import unittest
from types import SimpleNamespace

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gui_modules.sao_gui_fisheye_mixin import SAOPlayerGUIFisheyeMixin
from gui_modules.sao_gui_float_hp_mixin import SAOPlayerGUIFloatHpMixin
from gui_modules.sao_gui_float_handlers_mixin import SAOPlayerGUIFloatHandlersMixin


class _Root:
    def __init__(self) -> None:
        self.after_calls = []

    def after(self, delay, callback=None):
        self.after_calls.append((delay, callback))
        return f"after-{len(self.after_calls)}"


class _Panel:
    def __init__(self, visible: bool = True) -> None:
        self.visible = visible

    def is_visible(self) -> bool:
        return self.visible


class _Menu:
    def __init__(self, visible: bool) -> None:
        self.visible = visible


class _FisheyeOverlay:
    def __init__(self) -> None:
        self.fade_requests = 0
        self.fade_forces = []
        self.gpu_win = _GpuWin()

    def _request_fadeout(self, force=False) -> None:
        self.fade_requests += 1
        self.fade_forces.append(bool(force))


class _GpuWin:
    def __init__(self) -> None:
        self.click_through_values = []

    def set_click_through(self, value: bool) -> None:
        self.click_through_values.append(bool(value))


class _Layer:
    def __init__(self) -> None:
        self.destroy_count = 0

    def winfo_exists(self) -> bool:
        return True

    def destroy(self) -> None:
        self.destroy_count += 1


class _Pump:
    def __init__(self) -> None:
        self._thread = None
        self.tk_posts = []
        self.exec_calls = 0

    def post_to_tk(self, fn) -> None:
        self.tk_posts.append(fn)

    def exec_on_pump(self, fn, timeout=5.0):
        self.exec_calls += 1
        return fn()


class _StopGpuWin:
    def __init__(self, pump: _Pump) -> None:
        self._pump = pump
        self._win = object()
        self._hwnd = 0
        self.destroy_count = 0

    def destroy(self) -> None:
        self.destroy_count += 1


class _Presenter:
    def __init__(self) -> None:
        self.release_count = 0

    def release(self) -> None:
        self.release_count += 1


class _Worker:
    def __init__(self, alive: bool = False) -> None:
        self._alive = alive
        self.join_calls = []

    def is_alive(self) -> bool:
        return self._alive

    def join(self, timeout=None) -> None:
        self.join_calls.append(timeout)
        self._alive = False


class _FisheyeOwner(SAOPlayerGUIFisheyeMixin):
    def __init__(self) -> None:
        self.root = _Root()
        self._destroyed = False
        self._panels_hidden = False
        self._sao_menu = _Menu(False)
        self._fisheye_ov = _FisheyeOverlay()
        self._fisheye_hit_layer = None
        self._fisheye_close_suppress_until = 0.0
        self._sao_panel_transition_until = 0.0
        self._plugin_panel = None
        self._plugin_detached_panels = {}
        self.prepared_for_panel = 0
        self.stopped = 0
        self.clickthrough_values = []
        self.hit_layer_destroyed = 0
        self.raised_panels = []
        self.close_fx_count = 0

    def _prepare_fisheye_backdrop_for_panels(self) -> None:
        self.prepared_for_panel += 1

    def _play_fisheye_backdrop_close_fx(self) -> None:
        self.close_fx_count += 1

    def _stop_fisheye_overlay(self) -> None:
        self.stopped += 1

    def _destroy_fisheye_hit_layer(self) -> None:
        self.hit_layer_destroyed += 1

    def _set_fisheye_hit_layer_clickthrough(self, enabled: bool) -> None:
        self.clickthrough_values.append(bool(enabled))

    def _raise_panel_window(self, panel) -> None:
        self.raised_panels.append(panel)


class _StopOwner(SAOPlayerGUIFisheyeMixin):
    def __init__(self) -> None:
        self.root = _Root()
        self._fisheye_hit_layer = _Layer()
        self._fisheye_close_suppress_until = 0.0
        self._destroyed = False
        self._pump = _Pump()
        self._gpu_win = _StopGpuWin(self._pump)
        self._presenter = _Presenter()
        self._worker = _Worker(alive=True)
        self._running_ref = [True]
        self._fisheye_ov = SimpleNamespace(
            gpu_win=self._gpu_win,
            presenter=self._presenter,
            _worker_thread=self._worker,
            _running_ref=self._running_ref,
        )


class _Win:
    def __init__(self) -> None:
        self.topmost_values = []
        self.lift_count = 0
        self.focus_count = 0
        self._state = 'normal'

    def winfo_exists(self) -> bool:
        return True

    def state(self) -> str:
        return self._state

    def attributes(self, key, value=None):
        if key == '-topmost' and value is not None:
            self.topmost_values.append(bool(value))

    def lift(self) -> None:
        self.lift_count += 1

    def focus_force(self) -> None:
        self.focus_count += 1


class _PanelWithWin:
    def __init__(self) -> None:
        self._win = _Win()


class _RaiseOwner(SAOPlayerGUIFloatHandlersMixin):
    def __init__(self) -> None:
        self.root = _Root()


class _HpOverlay:
    def __init__(self) -> None:
        self.raise_count = 0

    def raise_topmost(self) -> None:
        self.raise_count += 1
        raise AssertionError('closing blur must not touch HP GPU window')


class _MotionBlurOwner(SAOPlayerGUIFloatHpMixin):
    def __init__(self) -> None:
        self._hp_overlay = _HpOverlay()
        self.raised_panels = []

    def _raise_panel_window(self, panel) -> None:
        self.raised_panels.append(panel)

    def _iter_fisheye_panels(self):
        return [getattr(self, '_act_aggregate_panel', None)]

    def _is_fisheye_panel_visible(self, panel) -> bool:
        return panel is not None


class EntityPanelFisheyeTests(unittest.TestCase):
    def test_act_panels_keep_fisheye_alive_and_release_backdrop_input(self) -> None:
        owner = _FisheyeOwner()
        owner._act_aggregate_panel = _Panel(True)

        self.assertTrue(owner._any_panel_open())
        owner._maybe_stop_fisheye()

        self.assertEqual(owner.prepared_for_panel, 1)
        self.assertEqual(owner.stopped, 0)

    def test_panel_transition_defers_fisheye_stop_until_panel_can_show(self) -> None:
        owner = _FisheyeOwner()
        owner._sao_panel_transition_until = time.time() + 1.0

        owner._maybe_stop_fisheye()

        self.assertEqual(owner.prepared_for_panel, 1)
        self.assertEqual(owner.stopped, 0)
        self.assertTrue(any(delay == 180 for delay, _callback in owner.root.after_calls))

    def test_menu_mode_keeps_backdrop_interactive(self) -> None:
        owner = _FisheyeOwner()
        owner._sao_menu = _Menu(True)

        owner._maybe_stop_fisheye()

        self.assertEqual(owner.clickthrough_values, [False])
        self.assertEqual(owner.prepared_for_panel, 0)
        self.assertEqual(owner.stopped, 0)

    def test_backdrop_click_request_forces_animated_fisheye_exit(self) -> None:
        owner = _FisheyeOwner()
        owner._act_aggregate_panel = _Panel(True)

        owner._request_fisheye_backdrop_close()

        self.assertEqual(owner._fisheye_ov.fade_requests, 1)
        self.assertEqual(owner._fisheye_ov.fade_forces, [True])
        self.assertEqual(owner._fisheye_ov.gpu_win.click_through_values, [True])
        self.assertEqual(owner.hit_layer_destroyed, 1)
        self.assertEqual(owner.close_fx_count, 1)
        self.assertEqual(owner.raised_panels, [owner._act_aggregate_panel])
        self.assertEqual(owner.stopped, 0)
        self.assertTrue(owner._any_panel_open())

    def test_raise_panel_reasserts_topmost_after_panel_show_demote(self) -> None:
        owner = _RaiseOwner()
        panel = _PanelWithWin()

        owner._raise_panel_window(panel)
        for _delay, callback in list(owner.root.after_calls):
            callback()

        self.assertEqual(panel._win.topmost_values, [True, True, True])
        self.assertEqual(panel._win.focus_count, 1)
        self.assertEqual([delay for delay, _ in owner.root.after_calls], [260, 520])

    def test_closing_motion_blur_does_not_touch_hp_gpu_window(self) -> None:
        owner = _MotionBlurOwner()
        owner._act_aggregate_panel = _Panel(True)

        owner._raise_fisheye_panels_above_motion_blur()

        self.assertEqual(owner._hp_overlay.raise_count, 0)
        self.assertEqual(owner.raised_panels, [owner._act_aggregate_panel])

    def test_stop_fisheye_called_from_pump_thread_marshals_to_tk(self) -> None:
        owner = _StopOwner()
        owner._pump._thread = threading.current_thread()

        owner._stop_fisheye_overlay()

        self.assertIsNotNone(owner._fisheye_ov)
        self.assertEqual(owner._fisheye_hit_layer.destroy_count, 0)
        self.assertEqual(len(owner._pump.tk_posts), 1)
        self.assertEqual(owner._gpu_win.destroy_count, 0)

    def test_final_close_waits_for_fisheye_gpu_shutdown(self) -> None:
        owner = _StopOwner()
        layer = owner._fisheye_hit_layer

        owner._stop_fisheye_overlay(wait=True)

        self.assertIsNone(owner._fisheye_ov)
        self.assertFalse(owner._running_ref[0])
        self.assertEqual(layer.destroy_count, 1)
        self.assertEqual(owner._worker.join_calls, [2.0])
        self.assertEqual(owner._pump.exec_calls, 0)
        self.assertEqual(owner._presenter.release_count, 0)
        self.assertEqual(owner._gpu_win.destroy_count, 1)

    def test_stop_fisheye_releases_presenter_without_gpu_window(self) -> None:
        owner = _StopOwner()
        owner._fisheye_ov.gpu_win = None

        owner._stop_fisheye_overlay(wait=True)

        self.assertEqual(owner._presenter.release_count, 1)

    def test_stale_fadeout_stop_does_not_destroy_new_fisheye_overlay(self) -> None:
        owner = _StopOwner()
        stale = owner._fisheye_ov
        new_running = [True]
        new_overlay = SimpleNamespace(
            gpu_win=None,
            presenter=_Presenter(),
            _worker_thread=None,
            _running_ref=new_running,
        )
        owner._fisheye_ov = new_overlay

        owner._stop_fisheye_overlay(wait=True, expected=stale)

        self.assertIs(owner._fisheye_ov, new_overlay)
        self.assertTrue(new_running[0])
        self.assertEqual(owner._gpu_win.destroy_count, 0)
        self.assertEqual(owner._presenter.release_count, 0)


if __name__ == "__main__":
    unittest.main()
