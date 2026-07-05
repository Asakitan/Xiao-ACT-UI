# -*- coding: utf-8 -*-
# Regression tests for Star Resonance panel text helpers.

from __future__ import annotations

import _bootstrap  # noqa: F401

import unittest
from unittest import mock

from plugins.star_resonance_plugin.panels import panel_text


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
            mock.patch("plugins.star_resonance_plugin.panels.panel_text.tk.Frame", return_value=_Widget()),
            mock.patch("plugins.star_resonance_plugin.panels.panel_text.status_badge", side_effect=_badge),
        ):
            panel_text.source_badges(object(), [
                {"source": "packet", "count": "bad"},
                {"source": "memory", "count": float("nan")},
            ])

        self.assertEqual(labels, ["封包 · 0", "内存 · 0"])


if __name__ == "__main__":
    unittest.main()
