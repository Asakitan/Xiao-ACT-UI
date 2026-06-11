# -*- coding: utf-8 -*-
"""Regression tests for shared ACT skill drilldown helpers."""

from __future__ import annotations

import _bootstrap  # noqa: F401

import json
from pathlib import Path
import time
import unittest
from unittest import mock

from act_platform import runtime
from act_platform.runtime import ensure_act_event_bus
from gui_modules.sao_gui_skill_drilldown import SkillDrilldownPanel, _finite_int


class FakeTracker:
    def __init__(self) -> None:
        self.detail = {
            "uid": 1001,
            "name": "Kirito",
            "skills": [
                {
                    "skill_id": 11,
                    "source_skill_id": 110048200100,
                    "base_skill_id": 1004820,
                    "skill_level_id": 11,
                    "skill_name": "Slash",
                    "total": 1800,
                    "hits": 3,
                    "casts": 2,
                    "crit_rate": 0.333,
                    "timeline_refs": [{"time_ms": 1000, "event_id": "evt-1"}],
                },
                {
                    "skill_id": 12,
                    "skill_name": "Potion",
                    "heal_total": 120,
                    "heal_hits": 1,
                    "casts": 1,
                },
            ],
        }

    def get_entity_detail(self, uid: int):
        return self.detail if int(uid) == 1001 else None


class FakeOwner:
    def __init__(self) -> None:
        self._dps_tracker = FakeTracker()


class FakeVar:
    def __init__(self, value: str = "") -> None:
        self.value = value

    def get(self) -> str:
        return self.value

    def set(self, value: object) -> None:
        self.value = str(value)


class ActSkillDrilldownRuntimeTests(unittest.TestCase):
    def test_html_sections_are_scrollable(self) -> None:
        html = (Path(__file__).resolve().parents[1] / "web" / "act_skill_drilldown.html").read_text(encoding="utf-8")

        self.assertIn("height:max(275px, calc(100vh - 230px))", html)
        self.assertIn(".section { min-height:0; padding:10px; overflow:auto; }", html)
        self.assertNotIn(".section { min-height:275px; padding:10px; overflow:hidden; }", html)

    def _owner_with_events(self) -> FakeOwner:
        owner = FakeOwner()
        bus = ensure_act_event_bus(owner)
        bus.publish("skill", {"timestamp": 101.0, "attacker": "Kirito", "skill_id": 11, "skill_name": "Slash", "damage": 900}, source_name="tcp", source_kind="packet")
        bus.publish("damage", {"timestamp": 102.0, "attacker": "Kirito", "skill_id": 11, "skill_name": "Slash", "damage": 900}, source_name="tcp", source_kind="packet")
        bus.publish("damage", {"timestamp": 102.5, "attacker": "Kirito", "skill_id": 110048200100, "base_skill_id": 1004820, "skill_name": "Slash", "damage": 1}, source_name="tcp", source_kind="packet")
        bus.publish("heal", {"timestamp": 103.0, "attacker": "Kirito", "skill_id": 12, "skill_name": "Potion", "heal": 120}, source_name="tcp", source_kind="packet")
        return owner

    def test_skill_status_contains_parity_fields(self) -> None:
        status = runtime.act_skill_drilldown_status(self._owner_with_events(), combatant_id=1001, skill_id=11)

        self.assertTrue(status["ok"])
        self.assertEqual(status["combatant_id"], "1001")
        self.assertEqual(status["skill_id"], "11")
        self.assertEqual(status["summary"]["name"], "Slash")
        self.assertEqual(status["summary"]["base_skill_id"], "1004820")
        self.assertIn("110048200100", status["summary"]["candidate_skill_ids"])
        self.assertEqual(status["casts"], 2)
        self.assertEqual(status["hits"], 3)
        self.assertAlmostEqual(status["crit_rate"], 0.333)
        self.assertGreaterEqual(len(status["timeline_refs"]), 1)
        self.assertIn("encounter_id", status)
        json.dumps(status, ensure_ascii=False)

    def test_skill_filter_uses_timeline_refs(self) -> None:
        status = runtime.act_skill_drilldown_filter(self._owner_with_events(), combatant_id=1001, skill_id=11, query="damage")

        self.assertTrue(status["timeline_refs"])
        self.assertTrue(all("damage" in json.dumps(ref, ensure_ascii=False).lower() for ref in status["timeline_refs"]))

    def test_skill_copy_returns_json_payload(self) -> None:
        copied = runtime.act_skill_drilldown_copy(self._owner_with_events(), combatant_id=1001, skill_id=11)
        data = json.loads(copied["text"])

        self.assertTrue(copied["ok"])
        self.assertEqual(data["skill_id"], "11")
        self.assertIn("timeline_refs", data)

    def test_skill_status_matches_composite_source_key(self) -> None:
        status = runtime.act_skill_drilldown_status(self._owner_with_events(), combatant_id=1001, skill_id=110048200100)

        self.assertTrue(status["ok"])
        self.assertEqual(status["summary"]["name"], "Slash")
        self.assertGreaterEqual(len(status["timeline_refs"]), 1)

    def test_skill_status_matches_semantic_base_id(self) -> None:
        status = runtime.act_skill_drilldown_status(self._owner_with_events(), combatant_id=1001, skill_id=1004820)

        self.assertTrue(status["ok"])
        self.assertEqual(status["summary"]["name"], "Slash")

    def test_skill_back_clears_selection(self) -> None:
        owner = self._owner_with_events()
        runtime.act_skill_drilldown_status(owner, combatant_id=1001, skill_id=11)
        status = runtime.act_skill_drilldown_back(owner)

        self.assertEqual(status["combatant_id"], "")
        self.assertEqual(status["skill_id"], "")
        self.assertEqual(status["summary"], {})

    def test_missing_skill_is_safe(self) -> None:
        status = runtime.act_skill_drilldown_status(self._owner_with_events(), combatant_id=1001, skill_id=999)

        self.assertFalse(status["ok"])
        self.assertEqual(status["summary"], {})
        self.assertTrue(status["errors"])

    def test_entity_signature_tracks_rendered_summary_facts(self) -> None:
        base = {
            "combatant_id": "1001",
            "skill_id": "11",
            "summary": {"name": "Slash", "kind": "damage", "amount": 1800, "damage": 1800, "heal": 0},
            "casts": 2,
            "hits": 3,
            "crit_rate": 0.333,
            "timeline_refs": [{"id": "evt-1", "time_ms": 1000, "topic": "damage", "label": "Slash", "value": 900}],
            "filters": {"query": ""},
        }
        name_changed = dict(base, summary=dict(base["summary"], name="Slash II"))
        kind_changed = dict(base, summary=dict(base["summary"], kind="heal"))
        facts_changed = dict(base, summary=dict(base["summary"], damage=1700, heal=100))

        panel = SkillDrilldownPanel.__new__(SkillDrilldownPanel)
        panel._expanded_refs = set()
        sig = panel._signature(base)

        self.assertNotEqual(sig, panel._signature(name_changed))
        self.assertNotEqual(sig, panel._signature(kind_changed))
        self.assertNotEqual(sig, panel._signature(facts_changed))

    def test_tk_numeric_rendering_uses_finite_helpers(self) -> None:
        source = (Path(__file__).resolve().parents[1] / "gui_modules" / "sao_gui_skill_drilldown.py").read_text(encoding="utf-8")

        self.assertNotIn("int(status.get('casts') or 0)", source)
        self.assertNotIn("float(value or 0)", source)
        self.assertIn("casts = _finite_int(status.get('casts'), 0, lo=0)", source)
        self.assertEqual(_finite_int(float("nan"), 9, lo=0), 9)
        self.assertEqual(SkillDrilldownPanel._fmt(float("nan")), "0")
        self.assertEqual(SkillDrilldownPanel._fmt(float("inf")), "0")
        self.assertEqual(SkillDrilldownPanel._pct(float("inf")), "0.0%")
        self.assertEqual(SkillDrilldownPanel._pct(2), "100.0%")

    def test_tk_status_counts_ignore_malformed_ref_and_error_payloads(self) -> None:
        panel = SkillDrilldownPanel.__new__(SkillDrilldownPanel)
        panel._combatant_var = FakeVar("")
        panel._skill_var = FakeVar("")
        panel._summary_var = FakeVar("")
        panel._status_var = FakeVar("")
        panel._rows = None

        panel._render_status({
            "ok": True,
            "combatant_id": "1001",
            "skill_id": "11",
            "summary": {"name": "Slash", "amount": 1200},
            "timeline_refs": "bad",
            "casts": 1,
            "hits": 2,
            "filters": {},
            "errors": "oops",
        })

        self.assertIn("refs=0", panel._status_var.get())
        self.assertIn("errors=0", panel._status_var.get())

    def test_tk_refresh_cache_reuses_only_same_request_parameters(self) -> None:
        panel = SkillDrilldownPanel.__new__(SkillDrilldownPanel)
        panel.owner = FakeOwner()
        panel._combatant_var = FakeVar("1001")
        panel._skill_var = FakeVar("11")
        panel._query_var = FakeVar("damage")
        panel._last_status = {"ok": True, "summary": {"name": "cached"}}
        panel._last_refresh_at = time.time()
        panel._last_request_key = ("1001", "11", "damage")
        rendered: list[dict] = []
        panel._render_status = lambda status: rendered.append(dict(status))

        with mock.patch("gui_modules.sao_gui_skill_drilldown.act_skill_drilldown_status") as status_fn:
            cached = panel.refresh()

        self.assertEqual(cached["summary"]["name"], "cached")
        self.assertEqual(rendered[-1]["summary"]["name"], "cached")
        status_fn.assert_not_called()

        panel._skill_var.set("12")
        status_payload = {"ok": True, "summary": {"name": "fresh"}, "timeline_refs": [], "casts": 1, "hits": 1, "crit_rate": 0.0, "filters": {"query": "damage"}}
        with mock.patch("gui_modules.sao_gui_skill_drilldown.act_skill_drilldown_status", return_value=status_payload) as status_fn:
            refreshed = panel.refresh()

        self.assertEqual(refreshed["summary"]["name"], "fresh")
        self.assertEqual(panel._last_request_key, ("1001", "12", "damage"))
        status_fn.assert_called_once_with(panel.owner, combatant_id="1001", skill_id="12", query="damage")


if __name__ == "__main__":
    unittest.main()
