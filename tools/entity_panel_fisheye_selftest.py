# -*- coding: utf-8 -*-
"""Regression coverage for Entity fisheye + floating panel interaction."""

from __future__ import annotations

import os
import sys
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gui_modules.sao_gui_fisheye_mixin import SAOPlayerGUIFisheyeMixin
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

    def _request_fadeout(self) -> None:
        self.fade_requests += 1


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
        for attr in self._FISHEYE_PANEL_ATTRS:
            setattr(self, attr, None)
        self._plugin_detached_panels = {}
        self.prepared_for_panel = 0
        self.stopped = 0
        self.clickthrough_values = []

    def _prepare_fisheye_backdrop_for_panels(self) -> None:
        self.prepared_for_panel += 1

    def _stop_fisheye_overlay(self) -> None:
        self.stopped += 1

    def _set_fisheye_hit_layer_clickthrough(self, enabled: bool) -> None:
        self.clickthrough_values.append(bool(enabled))


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

    def test_backdrop_click_request_fades_fisheye_without_stopping_panel(self) -> None:
        owner = _FisheyeOwner()
        owner._act_aggregate_panel = _Panel(True)

        owner._request_fisheye_backdrop_close()

        self.assertEqual(owner._fisheye_ov.fade_requests, 1)
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


if __name__ == "__main__":
    unittest.main()
