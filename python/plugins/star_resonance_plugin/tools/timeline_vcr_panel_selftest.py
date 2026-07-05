# -*- coding: utf-8 -*-
# Regression tests for the Entity timeline/VCR render signatures.

from __future__ import annotations

import _bootstrap  # noqa: F401

import time
import unittest
from unittest import mock

from plugins.star_resonance_plugin.panels.sao_gui_timeline_vcr import TimelineVcrPanel


class FakeVar:
    def __init__(self, value: str = "") -> None:
        self.value = value

    def get(self) -> str:
        return self.value

    def set(self, value: object) -> None:
        self.value = str(value)


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

    def test_refresh_cache_reuses_only_same_query(self) -> None:
        panel = self._panel()
        panel.owner = object()
        panel._query_var = FakeVar("damage")
        panel._last_status = {"ok": True, "events": [{"id": "cached"}]}
        panel._last_refresh_at = time.time()
        panel._last_request_key = ("damage",)
        rendered: list[dict] = []
        panel._render_status = lambda status: rendered.append(dict(status))

        with mock.patch("plugins.star_resonance_plugin.panels.sao_gui_timeline_vcr.act_timeline_status") as status_fn:
            cached = panel.refresh()

        self.assertEqual(cached["events"][0]["id"], "cached")
        self.assertEqual(rendered[-1]["events"][0]["id"], "cached")
        status_fn.assert_not_called()

        panel._query_var.set("heal")
        status_payload = {"ok": True, "events": [{"id": "fresh"}], "filters": {"query": "heal"}}
        with mock.patch("plugins.star_resonance_plugin.panels.sao_gui_timeline_vcr.act_timeline_status", return_value=status_payload) as status_fn:
            refreshed = panel.refresh()

        self.assertEqual(refreshed["events"][0]["id"], "fresh")
        self.assertEqual(panel._last_request_key, ("heal",))
        status_fn.assert_called_once_with(panel.owner, limit=80, query="heal")

    def test_play_and_set_speed_normalize_non_finite_input(self) -> None:
        panel = self._panel()
        panel.owner = object()
        panel._apply_result = lambda result, _message: dict(result)  # type: ignore[method-assign]

        panel._speed_var = FakeVar("nan")
        with mock.patch("plugins.star_resonance_plugin.panels.sao_gui_timeline_vcr.act_timeline_play", return_value={"ok": True}) as play_fn:
            panel.play()
        play_fn.assert_called_once_with(panel.owner, speed=1.0)

        panel._speed_var.set("inf")
        with mock.patch("plugins.star_resonance_plugin.panels.sao_gui_timeline_vcr.act_timeline_set_speed", return_value={"ok": True}) as speed_fn:
            panel.set_speed()
        speed_fn.assert_called_once_with(panel.owner, speed=1.0)

    def test_fmt_normalizes_non_finite_values(self) -> None:
        self.assertEqual(TimelineVcrPanel._fmt(float("nan")), "0")
        self.assertEqual(TimelineVcrPanel._fmt(float("inf")), "0")
        self.assertEqual(TimelineVcrPanel._fmt(float("-inf")), "0")


if __name__ == "__main__":
    unittest.main()
