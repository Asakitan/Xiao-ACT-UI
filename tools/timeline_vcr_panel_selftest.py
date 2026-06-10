# -*- coding: utf-8 -*-
"""Regression tests for the Entity timeline/VCR render signatures."""

from __future__ import annotations

import unittest

from gui_modules.sao_gui_timeline_vcr import TimelineVcrPanel


def _event(**overrides):
    data = {
        "id": "evt-1",
        "topic": "damage",
        "time_ms": 100,
        "label": "Hit",
        "value": 10,
        "source": "live",
        "payload": {"amount": 10},
    }
    data.update(overrides)
    return data


class TimelineVcrPanelSignatureTests(unittest.TestCase):
    def _panel(self) -> TimelineVcrPanel:
        panel = TimelineVcrPanel.__new__(TimelineVcrPanel)
        panel._expanded_events = set()
        panel._last_events_sig = ""
        return panel

    def test_event_signature_tracks_rendered_source_and_payload(self) -> None:
        base = [_event()]
        source_changed = [_event(source="history")]
        payload_changed = [_event(payload={"amount": 99})]

        self.assertNotEqual(TimelineVcrPanel._events_signature(base), TimelineVcrPanel._events_signature(source_changed))
        self.assertNotEqual(TimelineVcrPanel._events_signature(base), TimelineVcrPanel._events_signature(payload_changed))

    def test_event_signature_tracks_count_beyond_render_limit(self) -> None:
        first80 = [
            _event(id=f"evt-{idx}", time_ms=idx, value=idx)
            for idx in range(80)
        ]
        plus_hidden = first80 + [_event(id="evt-80", topic="heal", time_ms=80, value=1)]

        self.assertNotEqual(TimelineVcrPanel._events_signature(first80), TimelineVcrPanel._events_signature(plus_hidden))

    def test_render_signature_tracks_vcr_state_and_expansion(self) -> None:
        panel = self._panel()
        base = {
            "events": [_event()],
            "cursor_ms": 100,
            "speed": 1.0,
            "playing": False,
            "encounter_id": "enc-1",
            "errors": [],
        }
        speed_changed = dict(base, speed=2.0)

        self.assertNotEqual(
            panel._render_signature(base, base["events"]),
            panel._render_signature(speed_changed, speed_changed["events"]),
        )

        before_expand = panel._render_signature(base, base["events"])
        panel._expanded_events.add("evt-1")
        after_expand = panel._render_signature(base, base["events"])
        self.assertNotEqual(before_expand, after_expand)

    def test_destroy_resets_render_cache(self) -> None:
        panel = self._panel()
        panel._win = None
        panel._events = None
        panel._last_events_sig = "stale"

        panel.destroy()

        self.assertEqual(panel._last_events_sig, "")


if __name__ == "__main__":
    unittest.main()
