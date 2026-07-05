# -*- coding: utf-8 -*-
# Regression tests for game window title matching.

from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from utils.window_locator import _keyword_matches_title
from utils.window_locator import WindowLocator
from utils.window_locator import _matches_process_name
import utils.window_locator as window_locator


class WindowLocatorTitleMatchTests(unittest.TestCase):
    def test_star_keyword_does_not_match_link_start_editor_title(self) -> None:
        title = "link_start.py - SAO-UI - Visual Studio Code - Insiders".lower()
        self.assertFalse(_keyword_matches_title("Star", title))

    def test_star_keyword_matches_standalone_word(self) -> None:
        self.assertTrue(_keyword_matches_title("Star", "Star - game client".lower()))
        self.assertTrue(_keyword_matches_title("Star", "[Star] 星痕共鸣".lower()))

    def test_star_keyword_does_not_match_embedded_ascii_word(self) -> None:
        self.assertFalse(_keyword_matches_title("Star", "restart helper".lower()))
        self.assertFalse(_keyword_matches_title("Star", "StarResonanceDpsAnalysis".lower()))

    def test_chinese_keyword_keeps_substring_matching(self) -> None:
        self.assertTrue(_keyword_matches_title("星痕共鸣", "正在运行 - 星痕共鸣".lower()))

    def test_process_name_matching_is_case_insensitive(self) -> None:
        self.assertTrue(_matches_process_name("Star.EXE", ["star.exe"]))

    def test_process_match_is_preferred_over_title_fallback(self) -> None:
        windows = [
            (1, "Star - Visual Studio Code - Insiders", (0, 0, 1920, 1032)),
            (2, "Untitled", (100, 100, 2020, 1180)),
        ]

        def fake_process_name(hwnd: int) -> str:
            return "star.exe" if hwnd == 2 else "code - insiders.exe"

        locator = WindowLocator(keywords=["Star"], process_names=["star.exe"])
        locator._log_once = False
        with mock.patch.object(window_locator, "_enum_windows", return_value=windows), \
                mock.patch.object(window_locator, "_get_process_name", side_effect=fake_process_name):
            hwnd, title, _rect = locator.find_target_window()

        self.assertEqual(hwnd, 2)
        self.assertEqual(title, "Untitled")


if __name__ == "__main__":
    unittest.main()
