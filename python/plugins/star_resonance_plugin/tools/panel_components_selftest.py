# -*- coding: utf-8 -*-
"""Regression tests for shared SAO panel components."""

from __future__ import annotations

import _bootstrap  # noqa: F401

import unittest
from unittest import mock

from gui_modules import sao_panel_components


class _Widget:
    def pack(self, *args, **kwargs):
        return None


class PanelComponentTests(unittest.TestCase):
    def test_source_badges_tolerates_bad_counts(self) -> None:
        labels: list[str] = []

        def _badge(_parent, text, **_kwargs):
            labels.append(str(text))
            return _Widget()

        with (
            mock.patch("gui_modules.sao_panel_components.tk.Frame", return_value=_Widget()),
            mock.patch("gui_modules.sao_panel_components.status_badge", side_effect=_badge),
        ):
            sao_panel_components.source_badges(object(), [
                {"source": "packet", "count": "bad"},
                {"source": "memory", "count": float("nan")},
            ])

        self.assertEqual(labels, ["packet · 0", "内存 · 0"])


if __name__ == "__main__":
    unittest.main()
