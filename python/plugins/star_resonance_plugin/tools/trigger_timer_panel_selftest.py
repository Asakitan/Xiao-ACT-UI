# -*- coding: utf-8 -*-
# Regression tests for the Entity trigger/timer manager renderer.

from __future__ import annotations

import _bootstrap  # noqa: F401

from pathlib import Path
import unittest

from plugins.star_resonance_plugin.panels.sao_gui_trigger_timer_manager import TriggerTimerManagerPanel, _format_number


class FakeVar:
    def __init__(self) -> None:
        self.value = ""

    def set(self, value: object) -> None:
        self.value = str(value)


class FakeChild:
    def __init__(self, parent: "FakeList") -> None:
        self.parent = parent

    def destroy(self) -> None:
        self.parent.destroy_count += 1
        if self in self.parent.children:
            self.parent.children.remove(self)


class FakeList:
    def __init__(self) -> None:
        self.children: list[FakeChild] = []
        self.destroy_count = 0

    def add_child(self) -> None:
        self.children.append(FakeChild(self))

    def winfo_children(self) -> list[FakeChild]:
        return list(self.children)


class TriggerTimerPanelRenderTests(unittest.TestCase):
    def _panel(self) -> TriggerTimerManagerPanel:
        panel = TriggerTimerManagerPanel.__new__(TriggerTimerManagerPanel)
        panel._summary_var = FakeVar()
        panel._status_var = FakeVar()
        panel._list = FakeList()
        panel._last_render_sig = ""
        panel.rendered_rules = []
        panel.empty_count = 0
        panel.recent_count = 0
        panel._render_rule = lambda rule: panel.rendered_rules.append(str(rule.get("id") or ""))
        panel._render_empty = lambda: setattr(panel, "empty_count", panel.empty_count + 1)
        panel._render_recent = lambda _status: setattr(panel, "recent_count", panel.recent_count + 1)
        return panel

    def test_timer_only_status_is_rendered_instead_of_empty_state(self) -> None:
        panel = self._panel()
        panel._list.add_child()

        panel._render_status({
            "ok": True,
            "message": "OK",
            "triggers": [],
            "timers": [{"id": "elapsed_gate", "type": "elapsed_s", "enabled": True}],
            "recent": [],
        })

        self.assertEqual(panel.rendered_rules, ["elapsed_gate"])
        self.assertEqual(panel.empty_count, 0)
        self.assertEqual(panel._summary_var.value, "1 RULES / 1 TIMERS")

    def test_timer_rows_are_not_duplicated_when_already_in_triggers(self) -> None:
        rows = TriggerTimerManagerPanel._display_rules({
            "triggers": [{"id": "elapsed_gate", "type": "elapsed_s"}],
            "timers": [{"id": "elapsed_gate", "type": "elapsed_s"}],
        })

        self.assertEqual([row["id"] for row in rows], ["elapsed_gate"])

    def test_unchanged_status_skips_destroy_and_rerender(self) -> None:
        panel = self._panel()
        panel._list.add_child()
        status = {
            "ok": True,
            "message": "OK",
            "triggers": [{"id": "damage_gate", "type": "damage_total", "enabled": True}],
            "timers": [],
            "recent": [{"rule_id": "damage_gate", "message": "hit"}],
        }

        panel._render_status(status)
        panel._list.add_child()
        panel._render_status(dict(status))

        self.assertEqual(panel._list.destroy_count, 1)
        self.assertEqual(panel.rendered_rules, ["damage_gate"])
        self.assertEqual(panel.recent_count, 1)

    def test_recent_event_change_invalidates_render_signature(self) -> None:
        panel = self._panel()
        base = {
            "ok": True,
            "message": "OK",
            "triggers": [{"id": "damage_gate", "type": "damage_total", "enabled": True}],
            "timers": [],
            "recent": [{"rule_id": "damage_gate", "message": "hit"}],
        }

        panel._render_status(base)
        changed = dict(base)
        changed["recent"] = [{"rule_id": "damage_gate", "message": "hit again"}]
        panel._render_status(changed)

        self.assertEqual(panel.rendered_rules, ["damage_gate", "damage_gate"])
        self.assertEqual(panel.recent_count, 2)

    def test_rule_numeric_text_filters_non_finite_values(self) -> None:
        source = (Path(__file__).resolve().parents[1] / "gui_modules" / "sao_gui_trigger_timer_manager.py").read_text(encoding="utf-8")
        panel = TriggerTimerManagerPanel.__new__(TriggerTimerManagerPanel)

        self.assertNotIn("threshold={rule.get('threshold')}", source)
        self.assertNotIn("cooldown={rule.get('cooldown_s') or 0}s", source)
        self.assertIn("threshold = _format_number(rule.get('threshold'), 0, lo=0)", source)
        self.assertEqual(_format_number(float("nan"), 5, lo=0), "5")
        self.assertEqual(_format_number(float("inf"), 6, lo=0), "6")

        text = panel._format_rule({
            "id": "bad",
            "type": "damage_total",
            "threshold": float("nan"),
            "cooldown_s": float("inf"),
            "match": "",
            "severity": "warn",
        })
        self.assertIn("threshold=0", text)
        self.assertIn("cooldown=0s", text)
        self.assertNotIn("nan", text.lower())
        self.assertNotIn("infinity", text.lower())


if __name__ == "__main__":
    unittest.main()
