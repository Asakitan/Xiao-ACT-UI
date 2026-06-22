# -*- coding: utf-8 -*-
"""Regression tests for Entity menu ACT summary labels."""

from __future__ import annotations

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from contextlib import ExitStack, contextmanager
import unittest
from unittest import mock

import gui_modules.sao_gui_menu_mixin as menu_mixin
from gui_modules.sao_gui_menu_mixin import SAOPlayerGUIMenuMixin
import plugins.star_resonance_plugin.plugin as sr_plugin


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


class _FakeRoot:
    def after(self, _delay, _callback=None):
        return 'after-id'

    def after_cancel(self, _after_id) -> None:
        return None


class _FakePopUpMenu:
    def __init__(self, root, icons, children, **kwargs) -> None:
        self.root = root
        self.icons = icons
        self.child_menus = children
        self.kwargs = kwargs
        self.visible = False
        self.bound = False

    def bind_events(self) -> None:
        self.bound = True


class _MenuHarness(SAOPlayerGUIMenuMixin):
    def __init__(self) -> None:
        self.settings = _FakeSettings()
        self.root = _FakeRoot()
        self._float = _FakeFloat()
        self._cfg_settings_ref = None
        self._recognition_active = False
        self._panels_hidden = False
        self._act_plugin_manager = None
        self._menu_left_stack = None
        self._session_players = {}
        self._sao_menu = None
        self._menu_icons = []
        self._menu_refresh_after_id = None
        self._menu_refresh_force = False
        self._menu_children_cache = None
        self._menu_children_cache_sig = None
        self._last_menu_refresh_sig = None
        self._last_menu_refresh_sig_time = 0.0
        self._sao_menu_close_pending = False
        self._fisheye_close_suppress_until = 0.0
        self._destroyed = False
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


@contextmanager
def _patched_sr_ctx(owner):
    old_ctx = sr_plugin._ctx
    sr_plugin._ctx = type('Ctx', (), {'engine': type('Engine', (), {'owner': owner})()})()
    try:
        yield
    finally:
        sr_plugin._ctx = old_ctx


@contextmanager
def _patched_sr_runtime(harness: _MenuHarness):
    import act_platform.runtime as runtime

    patches = {
        'act_trigger_status': lambda _owner, **_kwargs: {'rule_count': 0, 'timer_count': 0},
        'act_data_source_health': lambda _owner, **_kwargs: {'status': 'ok', 'sources': {'summary': {'data_source': 'tcp'}}},
        'act_report_status': lambda _owner, **_kwargs: {'ok': False, 'preview': {}, 'storage_status': {'count': 0}},
        'act_timeline_status': lambda _owner, **_kwargs: harness._act_statuses.get('timeline', {'ok': True, 'events': [], 'playing': False}),
        'act_action_log_status': lambda _owner, **_kwargs: harness._act_statuses.get('action_log', {'ok': True, 'rows': [], 'grouped_rows': [], 'cursor': {}, 'filters': {}}),
        'act_aggregate_status': lambda _owner, **_kwargs: harness._act_statuses.get('aggregate', {'ok': False, 'raw_counts': {}}),
        'act_death_recap_status': lambda _owner, **_kwargs: {'ok': False, 'summary': {}},
        'act_graph_timeseries_status': lambda _owner, **_kwargs: harness._act_statuses.get('graph', {'ok': False, 'selected_metric': 'damage', 'row_count': 0}),
        'act_combatant_drilldown_status': lambda _owner, **_kwargs: {'ok': False, 'combatant_id': '', 'skills': []},
        'act_skill_drilldown_status': lambda _owner, **_kwargs: {'ok': False, 'skill_id': '', 'timeline_refs': []},
    }
    with ExitStack() as stack:
        for name, value in patches.items():
            stack.enter_context(mock.patch.object(runtime, name, value))
        yield


def _sr_act_labels(harness: _MenuHarness) -> list[str]:
    with _patched_sr_ctx(harness), _patched_sr_runtime(harness):
        return [str(item.get('label') or '') for item in sr_plugin._build_act_items()]


class MenuMixinActSummaryTests(unittest.TestCase):
    def test_setup_sao_menu_creates_platform_menu_shell(self) -> None:
        harness = _MenuHarness()

        with (
            _patched_module_attr('SAOPopUpMenu', _FakePopUpMenu),
            _patched_module_attr('ensure_act_plugin_manager', lambda _owner, load=True: None),
            _patched_module_attr('act_plugin_menu_surfaces', lambda _owner, _surface_id: {'surfaces': []}),
            _patched_module_attr('act_plugin_script_menus', lambda _owner: {'items': []}),
        ):
            harness._setup_sao_menu()

        self.assertIsNotNone(harness._sao_menu)
        self.assertTrue(harness._sao_menu.bound)
        self.assertEqual([item['name'] for item in harness._menu_icons], ['控制', '工具', '插件', '皮肤', '关于'])
        self.assertEqual(
            _labels(harness._sao_menu.child_menus, '工具'),
            ['AI Editor (LLM)', 'Workshop', 'Process Selector'],
        )

    def test_platform_tool_menu_excludes_mem_scope(self) -> None:
        harness = _MenuHarness()

        labels = _labels(harness._build_menu_children(), "工具")

        self.assertEqual(labels, ["AI Editor (LLM)", "Workshop", "Process Selector"])

    def test_act_menu_counts_ignore_malformed_list_payloads(self) -> None:
        harness = _MenuHarness()
        harness._act_statuses["timeline"] = {"ok": True, "events": "not-a-list", "playing": False}
        harness._act_statuses["action_log"] = {"ok": True, "rows": "bad", "grouped_rows": "bad", "cursor": {}, "filters": {}}

        labels = _sr_act_labels(harness)

        self.assertIn("ACT时间线/VCR: READY/0", labels)
        self.assertIn("ACT行为日志: READY/0", labels)

    def test_act_menu_counts_normalize_malformed_numeric_payloads(self) -> None:
        harness = _MenuHarness()
        harness._act_statuses["aggregate"] = {
            "ok": True,
            "raw_counts": {"rows": "oops", "skills": float("inf"), "monsters": float("nan")},
        }
        harness._act_statuses["graph"] = {"ok": True, "selected_metric": "damage", "row_count": float("nan")}

        labels = _sr_act_labels(harness)

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

    def test_script_sao_menu_synthesizes_disabled_manifest_button(self) -> None:
        harness = _MenuHarness()

        def fake_script_menus(_self):
            return {
                "items": [
                    {
                        "id": "script_snake",
                        "enabled": False,
                        "active": False,
                        "overlay_enabled": False,
                        "menu": {"name": "贪吃蛇", "icon_text": "▣", "script_label": "贪吃蛇"},
                    }
                ]
            }

        with _patched_module_attr("act_plugin_script_menus", fake_script_menus):
            icons = harness._build_menu_icons()
            children = harness._build_menu_children()

        self.assertIn({"name": "贪吃蛇", "icon": "▣", "can_active": True}, icons)
        self.assertEqual(_labels(children, "贪吃蛇"), ["开启贪吃蛇"])

    def test_script_sao_menu_click_enables_dispatches_refresh_and_closes(self) -> None:
        harness = _MenuHarness()
        calls: list[tuple] = []
        harness._sao_menu = type("Menu", (), {"visible": True})()
        harness._refresh_menu_if_open = lambda force=False: calls.append(("refresh", force))
        harness._toggle_sao_menu = lambda allow_close=False: calls.append(("close", allow_close))

        entry = {
            "id": "script_clock",
            "enabled": False,
            "active": False,
            "overlay_enabled": True,
            "setting": "overlay_enabled",
            "surface": "unioverlay",
            "action_id": "script.overlay.set_enabled",
            "menu": {"name": "世界时钟", "icon_text": "◷", "script_label": "世界时钟"},
        }

        def fake_script_menus(_self):
            return {"items": [entry]}

        def fake_enable(_self, plugin_id):
            calls.append(("enable", plugin_id))
            return {"ok": True}

        def fake_action(_self, action_id, payload=None, plugin_id=""):
            calls.append(("action", action_id, dict(payload or {}), plugin_id))
            return {"ok": True}

        with (
            _patched_module_attr("act_plugin_script_menus", fake_script_menus),
            _patched_module_attr("act_plugin_enable", fake_enable),
            _patched_module_attr("act_plugin_action", fake_action),
        ):
            row = harness._build_script_plugin_menu_children()["世界时钟"][0]
            self.assertEqual(row["label"], "关闭世界时钟")
            result = row["command"]()

        self.assertEqual(result, {"ok": True})
        self.assertIn(("enable", "script_clock"), calls)
        self.assertIn(
            ("action", "script.overlay.set_enabled",
             {"enabled": False, "setting": "overlay_enabled", "surface": "unioverlay"},
             "script_clock"),
            calls,
        )
        self.assertIn(("refresh", True), calls)
        self.assertIn(("close", True), calls)
        self.assertTrue(harness._sao_menu_needs_rebuild)


if __name__ == "__main__":
    unittest.main()
