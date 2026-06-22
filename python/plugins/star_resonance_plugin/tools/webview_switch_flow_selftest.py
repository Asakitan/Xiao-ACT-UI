# -*- coding: utf-8 -*-
"""Regression check for WebView -> Entity UI switching.

The WebView host should spawn Entity directly after the exit animation instead
of waiting for ``webview.start()`` to return.
"""

from __future__ import annotations

import sys
from pathlib import Path


import _bootstrap  # noqa: F401


ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import sao_webview  # noqa: E402


class _DummyStop:
    def __init__(self) -> None:
        self.called = False

    def set(self) -> None:
        self.called = True


class _DummySettings:
    def __init__(self) -> None:
        self.values = {}
        self.save_count = 0

    def set(self, key: str, value) -> None:
        self.values[key] = value

    def save(self) -> None:
        self.save_count += 1


class _DummyWindow:
    def __init__(self) -> None:
        self.destroy_count = 0

    def destroy(self) -> None:
        self.destroy_count += 1


def _build_gui():
    gui = object.__new__(sao_webview.SAOWebViewGUI)
    settings = _DummySettings()
    calls = []
    hot_switches = []

    gui._exit_animating = False
    gui._pending_switch = None
    gui._recognition_active = True
    gui._cache_loop_stop = _DummyStop()
    gui._cfg_settings_ref = settings
    gui.settings = settings
    gui._menu_visible = False

    gui._stop_recognition_engines = lambda: calls.append("stop_recognition")
    gui._persist_cached_identity_state = lambda **kwargs: calls.append(("persist_identity", kwargs))
    gui._save_game_cache = lambda quiet=False: calls.append(("save_game_cache", quiet))
    gui._close_menu = lambda: calls.append("close_menu")
    gui._native_fade_window = lambda *args, **kwargs: calls.append(("fade", args[0]))
    gui._destroy_all_panels = lambda: calls.append("destroy_panels")
    gui._do_hot_switch = lambda target: hot_switches.append(target)

    for attr in (
        "hp_win",
        "menu_win",
        "alert_win",
        "skillfx_win",
        "plugin_manager_win",
        "boss_hp_win",
        "dps_win",
        "raid_editor_win",
        "autokey_editor_win",
        "commander_win",
        "trigger_timer_win",
        "data_source_health_win",
        "report_export_win",
        "offline_import_win",
        "timeline_vcr_win",
        "act_aggregate_win",
        "action_log_win",
        "death_recap_win",
        "graph_timeseries_win",
        "combatant_drilldown_win",
        "skill_drilldown_win",
    ):
        setattr(gui, attr, _DummyWindow())

    return gui, settings, calls, hot_switches


def main() -> int:
    surface_gui, _settings, _calls, _switches = _build_gui()
    win_a = _DummyWindow()
    win_b = _DummyWindow()
    surface_gui._plugin_surfaces = {"a": win_a, "b": win_b}
    surface_gui._plugin_surface_meta = {
        "a": {"plugin_id": "plug_a"},
        "b": {"plugin_id": "plug_b"},
    }
    surface_gui._plugin_surface_order = ["a", "b"]
    surface_gui._destroy_plugin_surfaces_for_plugin("plug_a")
    if win_a.destroy_count != 1 or win_b.destroy_count != 0:
        raise AssertionError("plugin surface teardown was not scoped by plugin_id")
    if surface_gui._plugin_surface_order != ["b"]:
        raise AssertionError(f"stale plugin surface order: {surface_gui._plugin_surface_order}")

    gui, settings, calls, hot_switches = _build_gui()
    sleeps = []
    original_sleep = sao_webview.time.sleep
    sao_webview.time.sleep = lambda seconds: sleeps.append(seconds)
    try:
        gui._transition_with_animation("entity")
    finally:
        sao_webview.time.sleep = original_sleep

    if hot_switches != ["entity"]:
        raise AssertionError(f"Entity hot switch was not executed synchronously: {hot_switches}")
    if settings.values.get("ui_mode") != "entity":
        raise AssertionError("ui_mode was not saved as entity before switching")
    if settings.save_count < 1:
        raise AssertionError("settings were not flushed before hot switch")
    if not gui._cache_loop_stop.called:
        raise AssertionError("cache loop stop signal was not set")
    if "destroy_panels" not in calls:
        raise AssertionError("panel teardown was skipped before hot switch")

    print("OK webview switch flow: Entity spawn path runs before returning to webview.start()")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FAIL webview switch flow selftest: {exc}", file=sys.stderr)
        raise SystemExit(1)
