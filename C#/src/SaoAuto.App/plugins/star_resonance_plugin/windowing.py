# -*- coding: utf-8 -*-
"""Window-location helpers owned by the Star Resonance plugin."""

from __future__ import annotations

from utils.window_locator import WindowLocator

from .sr_config import GAME_PROCESS_NAMES, GAME_WINDOW_KEYWORDS


def create_window_locator() -> WindowLocator:
    return WindowLocator(
        keywords=GAME_WINDOW_KEYWORDS,
        process_names=GAME_PROCESS_NAMES,
    )


def get_window_rect():
    return create_window_locator().get_rect()
