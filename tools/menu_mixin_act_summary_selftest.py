# -*- coding: utf-8 -*-
"""Regression tests for Entity menu ACT summary labels."""

from __future__ import annotations

import _bootstrap  # noqa: F401

from contextlib import contextmanager
import unittest

import gui_modules.sao_gui_menu_mixin as menu_mixin
from gui_modules.sao_gui_menu_mixin import SAOPlayerGUIMenuMixin


@contextmanager
def _patched_module_attr(name: str, value):
    old_value = getattr(menu_mixin, name)
    setattr(menu_mixin, name, value)
    try:
        yield
    finally:
        setattr(menu_mixin, name, old_value)


class _FakeSettings:
    def get(self, _key: str, default=None):
        return default

    def save(self) -> None:
        return None


class _FakeFloat:
    def attributes(self, name: str):
        if name == "-topmost":
            return False
        return None


class _MenuHarness(SAOPlayerGUIMenuMixin):
    def __init__(self) -> None:
        self.settings = _FakeSettings()
        self._float = _FakeFloat()
        self._cfg_settings_ref = None
        self._recognition_active = False
        self._panels_hidden = False
        self._menu_left_stack = None
        self._session_players = {}
        self._act_statuses: dict[str, dict] = {}
        self.alerts: list[tuple[str, str, float]] = []

    def __getattr__(self, name: str):
        if name.startswith("_"):
            return lambda *args, **kwargs: None
        raise AttributeError(name)

    def _get_setting(self, _key: str, default=None):
        return default

    def _get_mem_data_source(self):
        return "tcp"

    def _get_dps_last_report_available(self):
        return False

    def _normalize_watched_skill_slots(self, value):
        return list(value or [])

    def _build_plugin_menu_items(self):
        return []

    def _build_update_menu_label(self):
        return "检查更新"

    def _get_act_plugin_menu_status(self):
        return {"plugin_count": 0, "active_count": 0, "plugins": []}

    def _get_act_trigger_menu_status(self):
        return {"rule_count": 0, "timer_count": 0}

    def _get_act_data_source_menu_status(self):
        return {"status": "ok", "sources": {"summary": {"data_source": "tcp"}}}

    def _get_act_report_menu_status(self):
        return {"ok": False, "preview": {}, "storage_status": {"count": 0}}

    def _get_act_timeline_menu_status(self):
        return self._act_statuses.get("timeline", {"ok": True, "events": [], "playing": False})

    def _get_act_action_log_menu_status(self):
        return self._act_statuses.get("action_log", {"ok": True, "rows": [], "grouped_rows": [], "cursor": {}, "filters": {}})

    def _get_act_aggregate_menu_status(self):
        return self._act_statuses.get("aggregate", {"ok": False, "raw_counts": {}})

    def _get_act_death_recap_menu_status(self):
        return {"ok": False, "summary": {}}

    def _get_act_graph_timeseries_menu_status(self):
        return self._act_statuses.get("graph", {"ok": False, "selected_metric": "damage", "row_count": 0})

    def _get_act_combatant_menu_status(self):
        return {"ok": False, "combatant_id": "", "skills": []}

    def _get_act_skill_menu_status(self):
        return {"ok": False, "skill_id": "", "timeline_refs": []}

    def _show_entity_alert(self, title: str, message: str, display_time: float = 0.0, **_kwargs) -> None:
        self.alerts.append((title, message, display_time))


def _labels(children: dict, menu_name: str) -> list[str]:
    return [str(item.get("label") or "") for item in children.get(menu_name, [])]


class MenuMixinActSummaryTests(unittest.TestCase):
    def test_act_menu_counts_ignore_malformed_list_payloads(self) -> None:
        harness = _MenuHarness()
        harness._act_statuses["timeline"] = {"ok": True, "events": "not-a-list", "playing": False}
        harness._act_statuses["action_log"] = {"ok": True, "rows": "bad", "grouped_rows": "bad", "cursor": {}, "filters": {}}

        labels = _labels(harness._build_menu_children(), "ACT")

        self.assertIn("ACT时间线/VCR: READY/0", labels)
        self.assertIn("ACT行为日志: READY/0", labels)

    def test_act_menu_counts_normalize_malformed_numeric_payloads(self) -> None:
        harness = _MenuHarness()
        harness._act_statuses["aggregate"] = {
            "ok": True,
            "raw_counts": {"rows": "oops", "skills": float("inf"), "monsters": float("nan")},
        }
        harness._act_statuses["graph"] = {"ok": True, "selected_metric": "damage", "row_count": float("nan")}

        labels = _labels(harness._build_menu_children(), "ACT")

        self.assertIn("ACT聚合驾驶舱: EMPTY/0/0/0", labels)
        self.assertIn("ACT图表/曲线: READY/damage/0", labels)

    def test_plugin_menu_ignores_malformed_plugin_collection(self) -> None:
        harness = _MenuHarness()

        with _patched_module_attr("act_plugin_menu", lambda _self: {"plugins": "bad"}):
            labels = [
                str(item.get("label") or "")
                for item in SAOPlayerGUIMenuMixin._build_plugin_menu_items(harness)
            ]

        self.assertIn("无已启用面板插件 (去 Manage 启用)", labels)

    def test_plugin_menu_normalizes_malformed_hotkey_count(self) -> None:
        harness = _MenuHarness()

        def fake_plugin_menu(_self):
            return {
                "plugins": [
                    {
                        "id": "sample",
                        "label": "Sample Plugin",
                        "active": True,
                        "declares_panel": True,
                        "hotkey_count": "oops",
                    }
                ]
            }

        with _patched_module_attr("act_plugin_menu", fake_plugin_menu):
            labels = [
                str(item.get("label") or "")
                for item in SAOPlayerGUIMenuMixin._build_plugin_menu_items(harness)
            ]

        self.assertIn("Sample Plugin", labels)


if __name__ == "__main__":
    unittest.main()
