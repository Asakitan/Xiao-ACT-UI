# -*- coding: utf-8 -*-
"""Regression tests for shared ACT combatant drilldown helpers."""

from __future__ import annotations

import _bootstrap  # noqa: F401

import json
from pathlib import Path
import time
import unittest
from unittest import mock

from act_platform import runtime
from gui_modules.sao_gui_combatant_drilldown import CombatantDrilldownPanel, _finite_int


class FakeTracker:
    def __init__(self) -> None:
        self.detail = {
            "uid": 1001,
            "name": "Kirito",
            "profession": "Sword",
            "damage": 2500,
            "damage_total": 2500,
            "heal": 120,
            "heal_total": 120,
            "dps": 1250,
            "hps": 60,
            "crit_rate": 0.25,
            "damage_pct": 0.625,
            "skills": [
                {"skill_id": 11, "skill_name": "Slash", "total": 1800, "hits": 3, "crit_rate": 0.333},
                {"skill_id": 12, "skill_name": "Potion", "heal_total": 120, "heal_hits": 1},
            ],
        }

    def get_entity_detail(self, uid: int):
        return self.detail if int(uid) == 1001 else None


class FakeOwner:
    def __init__(self) -> None:
        self._dps_tracker = FakeTracker()
        self._act_combatant_drilldown_state = {}


class FakeVar:
    def __init__(self, value: str = "") -> None:
        self.value = value

    def get(self) -> str:
        return self.value

    def set(self, value: object) -> None:
        self.value = str(value)


class ActCombatantDrilldownRuntimeTests(unittest.TestCase):
    def test_html_sections_are_scrollable(self) -> None:
        html = (Path(__file__).resolve().parents[1] / "web" / "act_combatant_drilldown.html").read_text(encoding="utf-8")

        self.assertIn("height:max(270px, calc(100vh - 230px))", html)
        self.assertIn(".section { min-height:0; padding:10px; overflow:auto; }", html)
        self.assertNotIn(".section { min-height:270px; padding:10px; overflow:hidden; }", html)

    def test_combatant_status_contains_parity_fields(self) -> None:
        status = runtime.act_combatant_drilldown_status(FakeOwner(), combatant_id=1001)

        self.assertTrue(status["ok"])
        self.assertEqual(status["combatant_id"], "1001")
        self.assertEqual(status["summary"]["name"], "Kirito")
        self.assertEqual(len(status["skills"]), 2)
        self.assertIn("incoming", status)
        self.assertIn("outgoing", status)
        self.assertIn("encounter_id", status)
        json.dumps(status, ensure_ascii=False)

    def test_combatant_status_sorts_skills_by_activity(self) -> None:
        status = runtime.act_combatant_drilldown_status(FakeOwner(), combatant_id=1001)
        skills = status["skills"]

        self.assertEqual(skills[0]["name"], "Slash")
        self.assertEqual(skills[0]["amount"], 1800)
        self.assertEqual(skills[1]["kind"], "heal")
        self.assertEqual(skills[1]["amount"], 120)

    def test_combatant_filter_and_focus_state(self) -> None:
        owner = FakeOwner()
        filtered = runtime.act_combatant_drilldown_filter(owner, combatant_id=1001, query="slash")
        focused = runtime.act_combatant_drilldown_focus_target(owner, combatant_id=1001, target_id="boss-7")
        after = runtime.act_combatant_drilldown_status(owner)

        self.assertEqual([skill["name"] for skill in filtered["skills"]], ["Slash"])
        self.assertEqual(focused["filters"]["focus_target"], "boss-7")
        self.assertEqual(after["combatant_id"], "1001")
        self.assertEqual(after["filters"]["query"], "slash")

    def test_combatant_back_clears_selection(self) -> None:
        owner = FakeOwner()
        runtime.act_combatant_drilldown_status(owner, combatant_id=1001)
        status = runtime.act_combatant_drilldown_back(owner)

        self.assertEqual(status["combatant_id"], "")
        self.assertEqual(status["summary"], {})
        self.assertEqual(status["skills"], [])

    def test_missing_combatant_is_safe(self) -> None:
        status = runtime.act_combatant_drilldown_status(FakeOwner(), combatant_id=9999)

        self.assertFalse(status["ok"])
        self.assertEqual(status["summary"], {})
        self.assertTrue(status["errors"])

    def test_entity_signature_tracks_rendered_summary_skill_and_side_fields(self) -> None:
        base = {
            "combatant_id": "1001",
            "summary": {"name": "Kirito", "damage": 2500, "heal": 120, "dps": 1250, "crit_rate": 0.25, "damage_pct": 0.625},
            "skills": [{"skill_id": 11, "name": "Slash", "amount": 1800, "hits": 3, "kind": "damage", "crit_rate": 0.333}],
            "outgoing": [{"kind": "damage", "name": "Boss", "amount": 900}],
            "filters": {"query": "", "focus_target": ""},
        }
        summary_changed = dict(base, summary=dict(base["summary"], dps=1300, crit_rate=0.5, damage_pct=0.7))
        skill_changed = dict(base, skills=[dict(base["skills"][0], crit_rate=0.667)])
        side_changed = dict(base, outgoing=[{"kind": "damage", "name": "Boss", "amount": 1200}])

        sig = CombatantDrilldownPanel._signature(base)

        self.assertNotEqual(sig, CombatantDrilldownPanel._signature(summary_changed))
        self.assertNotEqual(sig, CombatantDrilldownPanel._signature(skill_changed))
        self.assertNotEqual(sig, CombatantDrilldownPanel._signature(side_changed))

    def test_tk_numeric_rendering_uses_finite_helpers(self) -> None:
        source = (Path(__file__).resolve().parents[1] / "gui_modules" / "sao_gui_combatant_drilldown.py").read_text(encoding="utf-8")

        self.assertNotIn("int(skill.get('amount') or 0)", source)
        self.assertIn("_finite_int(skill.get('amount'), 0, lo=0)", source)
        self.assertEqual(_finite_int(float("nan"), 7, lo=0), 7)
        self.assertEqual(CombatantDrilldownPanel._fmt(float("nan")), "0")
        self.assertEqual(CombatantDrilldownPanel._fmt(float("inf")), "0")
        self.assertEqual(CombatantDrilldownPanel._pct(float("inf")), "0.0%")
        self.assertEqual(CombatantDrilldownPanel._pct(2), "100.0%")

    def test_tk_status_counts_ignore_malformed_skill_and_error_payloads(self) -> None:
        panel = CombatantDrilldownPanel.__new__(CombatantDrilldownPanel)
        panel._combatant_var = FakeVar("")
        panel._summary_var = FakeVar("")
        panel._status_var = FakeVar("")
        panel._rows = None

        panel._render_status({
            "ok": True,
            "combatant_id": "1001",
            "summary": {"name": "Kirito", "damage": 1200},
            "skills": "bad",
            "filters": {},
            "errors": "oops",
        })

        self.assertIn("0 SKILLS", panel._summary_var.get())
        self.assertIn("errors=0", panel._status_var.get())

    def test_tk_refresh_cache_reuses_only_same_request_parameters(self) -> None:
        panel = CombatantDrilldownPanel.__new__(CombatantDrilldownPanel)
        panel.owner = FakeOwner()
        panel._combatant_var = FakeVar("1001")
        panel._query_var = FakeVar("slash")
        panel._focus_var = FakeVar("boss-7")
        panel._last_status = {"ok": True, "summary": {"name": "cached"}}
        panel._last_refresh_at = time.time()
        panel._last_request_key = ("1001", "slash", "boss-7")
        rendered: list[dict] = []
        panel._render_status = lambda status: rendered.append(dict(status))

        with mock.patch("gui_modules.sao_gui_combatant_drilldown.act_combatant_drilldown_status") as status_fn:
            cached = panel.refresh()

        self.assertEqual(cached["summary"]["name"], "cached")
        self.assertEqual(rendered[-1]["summary"]["name"], "cached")
        status_fn.assert_not_called()

        panel._query_var.set("heal")
        status_payload = {"ok": True, "summary": {"name": "fresh"}, "skills": [], "incoming": [], "outgoing": [], "filters": {"query": "heal", "focus_target": "boss-7"}}
        with mock.patch("gui_modules.sao_gui_combatant_drilldown.act_combatant_drilldown_status", return_value=status_payload) as status_fn:
            refreshed = panel.refresh()

        self.assertEqual(refreshed["summary"]["name"], "fresh")
        self.assertEqual(panel._last_request_key, ("1001", "heal", "boss-7"))
        status_fn.assert_called_once_with(panel.owner, combatant_id="1001", query="heal", focus_target="boss-7")


if __name__ == "__main__":
    unittest.main()
