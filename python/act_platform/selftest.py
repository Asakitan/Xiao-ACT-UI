# -*- coding: utf-8 -*-
"""Selftest for ACT event bus and in-process Python plugin manager."""

from __future__ import annotations

import json
import os
import tempfile
from types import ModuleType

from .adapters import built_in_parser_adapters
from .event_bus import EventBus
from .plugins import PluginManager


PLUGIN_CODE = r'''
events = []

def on_load(ctx):
    ctx.log('loaded')
    ctx.subscribe('damage', lambda event: events.append(('damage', event['payload'].get('damage'))))
    ctx.subscribe('skill', lambda event: events.append(('skill', event['payload'].get('kind'))))
    ctx.subscribe('dungeon', lambda event: events.append(('dungeon', event['payload'].get('dungeon_id'))))
    ctx.subscribe('act_snapshot', lambda event: events.append(('snapshot', event['payload'].get('live', {}).get('total_damage', 0))))
    ctx.set_overlay('unioverlay', ctx.ui.panel('Demo Overlay', [ctx.ui.text('live')]))
    ctx.register_ui_panel('demo_panel', {'title': 'Demo Panel'}, lambda _p: ctx.ui.panel('Demo Panel', []))
    ctx.register_hotkey('demo', lambda: events.append(('hotkey', True)), 'CTRL+F12', 'Demo Hotkey')
    ctx.set_interval(lambda: events.append(('timer', True)), 60.0)

def on_disable():
    events.append(('disabled', True))
'''


def _write_demo_plugin(root: str) -> str:
    plugin_dir = os.path.join(root, "capture_demo")
    os.makedirs(plugin_dir, exist_ok=True)
    with open(os.path.join(plugin_dir, "plugin.json"), "w", encoding="utf-8") as fp:
        json.dump({
            "id": "capture_demo",
            "name": "Capture Demo",
            "version": "0.1.0",
            "entry": "plugin.py",
            "enabled": True,
            "permissions": ["event_subscribe"],
        }, fp, ensure_ascii=False, indent=2)
    with open(os.path.join(plugin_dir, "plugin.py"), "w", encoding="utf-8") as fp:
        fp.write(PLUGIN_CODE)
    return plugin_dir


def _write_script_menu_plugin(root: str) -> str:
    plugin_dir = os.path.join(root, "script_menu_demo")
    os.makedirs(plugin_dir, exist_ok=True)
    with open(os.path.join(plugin_dir, "plugin.json"), "w", encoding="utf-8") as fp:
        json.dump({
            "id": "script_menu_demo",
            "name": "Script Menu Demo",
            "version": "0.1.0",
            "entry": "plugin.lua",
            "language": "lua",
            "enabled": False,
            "settings_schema": {
                "overlay_enabled": {"type": "boolean", "default": True},
            },
            "sao_menu": {
                "name": "脚本演示",
                "icon_text": "▣",
                "script_label": "脚本演示",
                "priority": 25,
            },
        }, fp, ensure_ascii=False, indent=2)
    with open(os.path.join(plugin_dir, "plugin.lua"), "w", encoding="utf-8") as fp:
        fp.write("-- disabled manifest-only script menu selftest\n")
    return plugin_dir


def _write_failing_script_plugin(root: str) -> str:
    plugin_dir = os.path.join(root, "failing_script_demo")
    os.makedirs(plugin_dir, exist_ok=True)
    with open(os.path.join(plugin_dir, "plugin.json"), "w", encoding="utf-8") as fp:
        json.dump({
            "id": "failing_script_demo",
            "name": "Failing Script Demo",
            "version": "0.1.0",
            "entry": "plugin.lua",
            "language": "lua",
            "enabled": True,
        }, fp, ensure_ascii=False, indent=2)
    with open(os.path.join(plugin_dir, "plugin.lua"), "w", encoding="utf-8") as fp:
        fp.write("-- fake runtime raises after partial registration\n")
    return plugin_dir


def _write_action_script_plugin(root: str) -> str:
    plugin_dir = os.path.join(root, "action_script_demo")
    os.makedirs(plugin_dir, exist_ok=True)
    with open(os.path.join(plugin_dir, "plugin.json"), "w", encoding="utf-8") as fp:
        json.dump({
            "id": "action_script_demo",
            "name": "Action Script Demo",
            "version": "0.1.0",
            "entry": "plugin.lua",
            "language": "lua",
            "enabled": True,
        }, fp, ensure_ascii=False, indent=2)
    with open(os.path.join(plugin_dir, "plugin.lua"), "w", encoding="utf-8") as fp:
        fp.write("-- fake runtime registers action lazily\n")
    return plugin_dir


def run_selftest() -> dict:
    bus = EventBus()
    direct_events = []
    bus.subscribe("damage", direct_events.append, owner_id="selftest")
    bus.publish("damage", {"damage": 7}, source_name="selftest", source_kind="unit")
    assert direct_events and direct_events[0]["payload"]["damage"] == 7, direct_events

    assert built_in_parser_adapters() == [], built_in_parser_adapters()

    with tempfile.TemporaryDirectory(prefix="act_plugin_selftest_") as root:
        _write_demo_plugin(root)
        _write_script_menu_plugin(root)
        _write_failing_script_plugin(root)
        _write_action_script_plugin(root)
        manager = PluginManager(plugin_dirs=[root], event_bus=bus)
        lifecycle_events = []
        bus.subscribe("plugin_lifecycle", lifecycle_events.append, owner_id="selftest_lifecycle")
        manager.discover()
        manifest_status = manager.status()
        script_record = next(
            item for item in manifest_status.get("plugins", [])
            if item.get("id") == "script_menu_demo"
        )
        assert script_record.get("sao_menu", {}).get("name") == "脚本演示", script_record
        assert manager.load_plugin("capture_demo"), manager.status()
        assert manager.render_registry.status().get("overlay_count") == 1, manager.render_registry.status()
        assert manager.list_ui_panels(), manager.list_ui_panels()
        assert manager.list_hotkeys(), manager.list_hotkeys()
        assert manager._timers.get("capture_demo"), manager._timers
        manager.discover()
        assert manager._records["capture_demo"].active, manager.status()
        assert manager.render_registry.status().get("overlay_count") == 1, manager.render_registry.status()
        assert manager.list_ui_panels(), manager.list_ui_panels()
        assert manager.list_hotkeys(), manager.list_hotkeys()
        assert manager._timers.get("capture_demo"), manager._timers

        bus.publish("dungeon", {"kind": "sync_dungeon_data", "dungeon_id": 42001}, source_name="selftest", source_kind="unit")
        bus.publish("skill", {"kind": "server_end", "skill_uuid": 777}, source_name="selftest", source_kind="unit")
        bus.publish("damage", {"damage": 1234}, source_name="selftest", source_kind="unit")
        snap = {"live": {"total_damage": 1234}}
        bus.publish("act_snapshot", snap, source_name="selftest", source_kind="unit")
        module = manager._records["capture_demo"].module
        captured = list(getattr(module, "events", [])) if module else []
        assert ("dungeon", 42001) in captured, captured
        assert ("skill", "server_end") in captured, captured
        assert ("damage", 1234) in captured, captured
        assert any(kind == "snapshot" and value >= 1234 for kind, value in captured), captured
        assert snap.get("live", {}).get("total_damage") == 1234, snap
        assert manager.disable_plugin("capture_demo"), manager.status()
        assert not manager._records["capture_demo"].active, manager.status()
        assert manager.render_registry.status().get("overlay_count") == 0, manager.render_registry.status()
        assert not manager.list_hotkeys(), manager.list_hotkeys()
        assert not manager._timers.get("capture_demo"), manager._timers
        assert any(
            ev.get("payload", {}).get("plugin_id") == "capture_demo"
            and ev.get("payload", {}).get("action") == "loaded"
            for ev in lifecycle_events
        ), lifecycle_events
        assert any(
            ev.get("payload", {}).get("plugin_id") == "capture_demo"
            and ev.get("payload", {}).get("action") == "unloaded"
            for ev in lifecycle_events
        ), lifecycle_events
        status = manager.status()
        owner = type("Owner", (), {})()
        owner._act_event_bus = bus
        owner._act_plugin_manager = manager
        from .runtime import act_plugin_action, act_plugin_menu, act_plugin_script_menus
        script_menus = act_plugin_script_menus(owner)
        assert script_menus.get("ok"), script_menus
        assert not any(
            item.get("id") == "script_menu_demo"
            for item in script_menus.get("items", [])
        ), script_menus
        manager._records["script_menu_demo"].enabled = True
        enabled_script_menus = act_plugin_script_menus(owner)
        script_item = next(
            item for item in enabled_script_menus.get("items", [])
            if item.get("id") == "script_menu_demo"
        )
        assert script_item.get("enabled") is True, script_item
        assert script_item.get("overlay_enabled") is True, script_item
        manager._records["script_menu_demo"].enabled = False
        menu_summary = act_plugin_menu(owner)
        assert not manager._records["failing_script_demo"].loaded, manager.status()
        assert not manager._records["action_script_demo"].loaded, manager.status()
        assert not any(
            item.get("id") == "script_menu_demo"
            for item in menu_summary.get("script_menus", [])
        ), menu_summary.get("script_menus")

        class FakeLuaRuntime:
            def __init__(self) -> None:
                self.loaded = []
                self.unloaded = []

            def load_script(self, _entry_path, record, ctx):
                self.loaded.append(record.plugin_id)
                if record.plugin_id == "action_script_demo":
                    module = ModuleType(f"act_plugin_lua_{record.plugin_id}")

                    def on_load(_ctx):
                        _ctx.register_action_handler(
                            lambda action, payload: {
                                "action": action,
                                "enabled": bool(payload.get("enabled")),
                            })

                    module.on_load = on_load
                    return module
                ctx.set_overlay("unioverlay", ctx.ui.panel("Failing Overlay", []))
                ctx.register_hotkey("fail", lambda: None, "CTRL+F11", "Fail Hotkey")
                ctx.set_interval(lambda: None, 60.0)
                raise RuntimeError("script load failed after partial registration")

            def unload_script(self, record) -> None:
                self.unloaded.append(record.plugin_id)

        from . import scripting
        original_get_runtime = scripting.get_runtime
        fake_runtime = FakeLuaRuntime()
        scripting.get_runtime = (
            lambda language: fake_runtime
            if str(language or "").lower() == "lua"
            else original_get_runtime(language)
        )
        try:
            action_result = act_plugin_action(
                owner,
                "script.overlay.set_enabled",
                {"enabled": True},
                plugin_id="action_script_demo",
            )
            assert action_result.get("ok"), action_result
            assert fake_runtime.loaded == ["action_script_demo"], fake_runtime.loaded
            assert manager._records["action_script_demo"].loaded, manager.status()
            assert not manager._records["failing_script_demo"].loaded, manager.status()
            assert not manager.load_plugin("failing_script_demo"), manager.status()
        finally:
            scripting.get_runtime = original_get_runtime
        assert fake_runtime.loaded == ["action_script_demo", "failing_script_demo"], fake_runtime.loaded
        assert fake_runtime.unloaded == ["failing_script_demo"], fake_runtime.unloaded
        assert manager.render_registry.status().get("overlay_count") == 0, manager.render_registry.status()
        assert not manager._timers.get("failing_script_demo"), manager._timers
        assert not any(
            item.get("plugin_id") == "failing_script_demo"
            for item in manager.list_hotkeys()
        ), manager.list_hotkeys()
        assert any(
            ev.get("payload", {}).get("plugin_id") == "failing_script_demo"
            and ev.get("payload", {}).get("action") == "load_failed"
            for ev in lifecycle_events
        ), lifecycle_events

    return {
        "ok": True,
        "event_bus_contract": True,
        "plugin_manager_contract": True,
        "parser_adapter_contract": "plugin_owned",
        "replay_event_bus_contract": True,
        "published": bus.snapshot().get("published"),
        "lifecycle_events": len(lifecycle_events),
        "plugin_count": status.get("plugin_count"),
        "active_count_after_disable": status.get("active_count"),
        "script_menu_count": len(enabled_script_menus.get("items", [])),
        "captured_events": len(captured),
        "built_in_parser_adapters": len(built_in_parser_adapters()),
    }


def main() -> int:
    print(json.dumps(run_selftest(), ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
