# -*- coding: utf-8 -*-
"""Regression coverage for Entity menu motion-blur z-order handling."""

from __future__ import annotations

import os
import sys
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gui_modules.sao_gui_float_chrome_mixin import SAOPlayerGUIFloatChromeMixin
from sao_theme.popup_menu import SAOPopUpMenu


class _Menu:
    def __init__(self, visible: bool = True) -> None:
        self.visible = visible
        self.raise_count = 0

    def _raise_to_top(self) -> None:
        self.raise_count += 1


class _Owner(SAOPlayerGUIFloatChromeMixin):
    def __init__(self, menu=None) -> None:
        self._sao_menu = menu


class _Panel:
    def __init__(self, visible: bool = True) -> None:
        self.visible = visible


class _Overlay:
    def __init__(self) -> None:
        self.topmost_values = []
        self.lift_count = 0

    def winfo_exists(self) -> bool:
        return True

    def attributes(self, key, value=None):
        if key == '-topmost' and value is not None:
            self.topmost_values.append(value)

    def lift(self) -> None:
        self.lift_count += 1


class _PanelOwner(SAOPlayerGUIFloatChromeMixin):
    def __init__(self) -> None:
        self.panels = [_Panel(True), _Panel(False)]
        self.raised = []

    def _iter_fisheye_panels(self):
        return iter(self.panels)

    def _is_fisheye_panel_visible(self, panel) -> bool:
        return bool(getattr(panel, 'visible', False))

    def _raise_panel_window(self, panel) -> None:
        self.raised.append(panel)


class MenuMotionBlurTests(unittest.TestCase):
    def test_open_blur_reraises_visible_sao_menu(self) -> None:
        menu = _Menu(visible=True)
        owner = _Owner(menu)

        owner._raise_sao_menu_above_motion_blur()

        self.assertEqual(menu.raise_count, 1)

    def test_open_blur_ignores_hidden_or_missing_menu(self) -> None:
        hidden = _Menu(visible=False)

        _Owner(hidden)._raise_sao_menu_above_motion_blur()
        _Owner(None)._raise_sao_menu_above_motion_blur()

        self.assertEqual(hidden.raise_count, 0)

    def test_closing_blur_reraises_visible_fisheye_panels(self) -> None:
        owner = _PanelOwner()

        owner._raise_fisheye_panels_above_motion_blur()

        self.assertEqual(owner.raised, [owner.panels[0]])

    def test_motion_blur_marker_keeps_short_settle_window(self) -> None:
        owner = _Owner(None)

        owner._mark_motion_blur_active(ttl=0.2)
        self.assertEqual(owner._motion_blur_active_count, 1)
        self.assertGreater(owner._motion_blur_active_until, time.time())

        owner._clear_motion_blur_active(settle=0.05)
        self.assertEqual(owner._motion_blur_active_count, 0)
        self.assertGreater(owner._motion_blur_active_until, time.time())

    def test_popup_menu_raise_to_top_lifts_overlay(self) -> None:
        menu = object.__new__(SAOPopUpMenu)
        overlay = _Overlay()
        menu._overlay = overlay

        menu._raise_to_top()

        self.assertEqual(overlay.topmost_values, [True])
        self.assertEqual(overlay.lift_count, 1)


if __name__ == "__main__":
    unittest.main()
