# -*- coding: utf-8 -*-
"""Regression coverage for shared ACT death recap helpers."""

from __future__ import annotations

import _bootstrap  # noqa: F401

import json
import time
import unittest
from unittest import mock

from act_platform import runtime
from act_platform.events import make_event
from act_platform.runtime import ensure_act_event_bus
from plugins.star_resonance_plugin.panels.sao_gui_death_recap import DeathRecapPanel


SELF_UID = 36668136


class FakeOwner:
    pass


class FakeVar:
    def __init__(self, value: str = "") -> None:
        self.value = value

    def get(self) -> str:
        return self.value

    def set(self, value: object) -> None:
        self.value = str(value)


def _publish(owner: FakeOwner, topic: str, payload: dict, ts: float) -> None:
    bus = ensure_act_event_bus(owner)
    bus.publish(
        topic,
        event=make_event(
            topic,
            payload,
            source_name="test",
            source_kind="unit",
            observed_at=ts,
        ),
    )


class ActDeathRecapRuntimeTests(unittest.TestCase):
    def _owner_with_death(self) -> FakeOwner:
        owner = FakeOwner()
        _publish(owner, "damage", {"attacker": "Boss", "target": "Kirito", "target_uid": SELF_UID, "damage": 999}, 90.0)
        _publish(owner, "damage", {"attacker": "Boss", "target": "Kirito", "target_uid": SELF_UID, "damage": 1000}, 95.0)
        _publish(owner, "heal", {"actor": "Asuna", "target": "Kirito", "target_uid": SELF_UID, "heal": 250}, 97.0)
        _publish(owner, "shield", {"actor": "Support", "target": "Kirito", "target_uid": SELF_UID, "shield": 100}, 98.0)
        _publish(owner, "damage", {"attacker": "Boss", "target": "Kirito", "target_uid": SELF_UID, "damage": 700}, 99.0)
        _publish(owner, "self_state", {"uid": SELF_UID, "name": "Kirito", "hp": 0, "max_hp": 1000, "is_dead": True}, 100.0)
        _publish(owner, "damage", {"attacker": "Boss", "target": "Kirito", "target_uid": SELF_UID, "damage": 50}, 101.0)
        return owner

    def test_death_recap_summarizes_window_around_latest_death(self) -> None:
        owner = self._owner_with_death()

        status = runtime.act_death_recap_status(owner, window_s=5.0, limit=20)

        self.assertTrue(status["ok"], status)
        self.assertEqual(status["death"]["entity_id"], str(SELF_UID))
        self.assertEqual(status["death"]["name"], "Kirito")
        self.assertEqual(status["summary"]["incoming_damage"], 1750)
        self.assertEqual(status["summary"]["healing"], 250)
        self.assertEqual(status["summary"]["shield"], 100)
        self.assertEqual(status["summary"]["death_events"], 1)
        self.assertFalse(any(row["amount"] == 999 for row in status["rows"]))
        self.assertEqual([row["time_ms"] for row in status["rows"]], sorted(row["time_ms"] for row in status["rows"]))
        self.assertTrue(any(row["is_death"] and row["relative_ms"] == 0 for row in status["rows"]))
        json.dumps(status, ensure_ascii=False)

    def test_death_recap_copy_returns_json_payload(self) -> None:
        owner = self._owner_with_death()

        copied = runtime.act_death_recap_copy(owner, window_s=5.0)
        data = json.loads(copied["text"])

        self.assertTrue(copied["ok"], copied)
        self.assertEqual(data["summary"]["incoming_damage"], 1750)
        self.assertEqual(data["death"]["entity_id"], str(SELF_UID))

    def test_death_recap_empty_bus_is_safe(self) -> None:
        status = runtime.act_death_recap_status(FakeOwner())

        self.assertTrue(status["ok"])
        self.assertIsNone(status["death"])
        self.assertEqual(status["rows"], [])
        self.assertEqual(status["summary"]["event_count"], 0)

    def test_tk_refresh_cache_reuses_only_same_request_parameters(self) -> None:
        panel = DeathRecapPanel.__new__(DeathRecapPanel)
        panel.owner = FakeOwner()
        panel._entity_var = FakeVar("1001")
        panel._window_var = FakeVar("5.0")
        panel._last_status = {"ok": True, "summary": {"event_count": 1}, "rows": []}
        panel._last_refresh_at = time.time()
        panel._last_request_key = ("1001", 5.0)
        rendered: list[dict] = []
        panel._render_status = lambda status: rendered.append(dict(status))

        with mock.patch("gui_modules.sao_gui_death_recap.act_death_recap_status") as status_fn:
            cached = panel.refresh()

        self.assertEqual(cached["summary"]["event_count"], 1)
        self.assertEqual(rendered[-1]["summary"]["event_count"], 1)
        status_fn.assert_not_called()

        panel._window_var.set("12.5")
        status_payload = {"ok": True, "summary": {"event_count": 2}, "death": None, "rows": []}
        with mock.patch("gui_modules.sao_gui_death_recap.act_death_recap_status", return_value=status_payload) as status_fn:
            refreshed = panel.refresh()

        self.assertEqual(refreshed["summary"]["event_count"], 2)
        self.assertEqual(panel._last_request_key, ("1001", 12.5))
        status_fn.assert_called_once_with(panel.owner, limit=80, window_s=12.5, entity_id="1001")

    def test_render_signature_tracks_visible_summary_death_window_and_row_fields(self) -> None:
        row = {
            "id": "row-1",
            "time_ms": 95000,
            "relative_ms": -5000,
            "kind": "incoming_damage",
            "amount": 1000,
            "actor": "Boss",
            "target": "Kirito",
            "label": "Cleave",
            "is_death": False,
            "payload": {"skill": "Cleave"},
        }
        status = {
            "summary": {"incoming_damage": 1000, "healing": 250, "shield": 100, "death_events": 1},
            "death": {"name": "Kirito", "entity_id": str(SELF_UID), "time_ms": 100000},
            "encounter_id": "enc-1",
            "window": {"before_ms": 5000},
            "errors": [],
            "rows": [row],
        }
        changed_visible = {
            **status,
            "summary": {"incoming_damage": 1500, "healing": 250, "shield": 100, "death_events": 1},
            "death": {"name": "Asuna", "entity_id": "1002", "time_ms": 100500},
            "window": {"before_ms": 8000},
            "rows": [{**row, "actor": "Boss Phase 2", "target": "Asuna", "label": "Fatal Cleave"}],
        }
        changed_payload = {
            **status,
            "rows": [{**row, "payload": {"skill": "Cleave", "raw": "changed"}}],
        }

        self.assertNotEqual(
            DeathRecapPanel._render_signature(status, status["rows"], set()),
            DeathRecapPanel._render_signature(changed_visible, changed_visible["rows"], set()),
        )
        self.assertNotEqual(
            DeathRecapPanel._render_signature(status, status["rows"], {"row-1"}),
            DeathRecapPanel._render_signature(changed_payload, changed_payload["rows"], {"row-1"}),
        )

    def test_tk_render_status_ignores_malformed_rows_shape(self) -> None:
        panel = DeathRecapPanel.__new__(DeathRecapPanel)
        panel._summary_var = FakeVar()
        panel._status_var = FakeVar()
        panel._rows = None

        panel._render_status({
            "rows": "not-a-list",
            "summary": {"incoming_damage": 0, "healing": 0},
            "death": None,
            "encounter_id": "enc-1",
        })

        self.assertEqual(panel._summary_var.value, "0 EVENTS · DMG 0 · HEAL 0")

    def test_tk_render_status_normalizes_malformed_summary_numbers(self) -> None:
        panel = DeathRecapPanel.__new__(DeathRecapPanel)
        panel._summary_var = FakeVar()
        panel._status_var = FakeVar()
        panel._rows = None

        panel._render_status({
            "rows": [],
            "summary": {"incoming_damage": "oops", "healing": float("inf")},
            "death": None,
            "encounter_id": "enc-1",
        })

        self.assertEqual(panel._summary_var.value, "0 EVENTS · DMG 0 · HEAL 0")


if __name__ == "__main__":
    unittest.main()
