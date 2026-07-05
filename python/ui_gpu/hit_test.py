# Cursor → (kind, idx) hit testing for the popup GLFW window.

from __future__ import annotations

from typing import List, Optional, Tuple

from . import composer, menu_bar_layout, child_bar_layout
import _sao_cy_uihelpers as _CY_UI  # type: ignore[import-not-found]

# kind constants
KIND_MENU_BTN = 'menu'
KIND_CHILD_ROW = 'child'
KIND_BACKGROUND = 'background'


class HitTester:
    def __init__(self) -> None:
        self._regions: List[Tuple[Tuple[int, int, int, int], str, int]] = []
        self._bounds: Tuple[int, int, int, int] = (0, 0, 0, 0)

    def update(self, state, reserved_rows: int = 0) -> None:
        self._regions.clear()
        win_w, win_h = composer.window_size_reserved(state, int(reserved_rows or 0))
        self._bounds = (0, 0, int(win_w), int(win_h))
        menu_x, menu_y = composer.menu_origin(state)
        child_x, child_y = composer.child_origin(state)
        # Menu buttons
        for rect, idx in menu_bar_layout.hit_rects(state, menu_x, menu_y):
            self._regions.append((rect, KIND_MENU_BTN, idx))
        # Child rows
        for rect, idx in child_bar_layout.hit_rects(state, child_x, child_y):
            self._regions.append((rect, KIND_CHILD_ROW, idx))

    def pick(self, x: float, y: float) -> Optional[Tuple[str, int]]:
        return _CY_UI.popup_pick_hit(self._regions, self._bounds, x, y)
