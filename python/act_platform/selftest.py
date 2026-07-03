# -*- coding: utf-8 -*-
"""Selftest for ACT event bus and in-process Python plugin manager."""

from __future__ import annotations

import json
import os
import shutil
import tempfile
import traceback
from types import ModuleType

from .adapters import built_in_parser_adapters
from .event_bus import EventBus
from .plugins import PluginManager
from .scripting.angel_runtime import _AngelScriptInterpreter

_FAILURE_DETAIL_LINE_LIMIT = 80
_FINAL_FAILURE_POINT_HEADING = "FAILED CHECK POINTS (final):"


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
            "name": "脚本菜单演示",
            "description": "中文脚本菜单演示",
            "version": "0.1.0",
            "entry": "plugin.lua",
            "language": "lua",
            "enabled": False,
            "settings_schema": {
                "overlay_enabled": {"type": "boolean", "default": True, "description": "显示叠加层"},
                "detail_mode": {
                    "type": "string",
                    "default": "seconds",
                    "description": "刷新细节",
                    "enum": ["seconds", "minute"],
                    "enum_labels": {
                        "seconds": "秒级刷新",
                        "minute": "分钟刷新",
                    },
                    "options": [
                        {"id": "seconds", "value": "seconds", "label": "秒级", "description": "每秒刷新"},
                        {"id": "minute", "value": "minute", "label": "分钟级", "description": "每分钟刷新"},
                    ],
                },
            },
            "sao_menu": {
                "name": "脚本演示",
                "icon_text": "▣",
                "script_label": "脚本演示",
                "priority": 25,
                "actions": [
                    {"id": "script.state", "label": "读取状态"},
                ],
            },
            "locales": {
                "en": {
                    "sao_menu": {
                        "name": "Generic Script Demo",
                    },
                },
                "en-US": {
                    "name": "Script Menu Demo",
                    "description": "Localized script menu demo",
                    "settings_schema": {
                        "overlay_enabled": {
                            "description": "Show overlay",
                            "default": False,
                        },
                        "detail_mode": {
                            "description": "Refresh detail",
                            "default": "minute",
                            "enum_labels": {
                                "seconds": "Seconds",
                                "minute": "Minute",
                                "frame": "Frame",
                            },
                            "options": {
                                "seconds": {
                                    "label": "Seconds",
                                    "description": "Refresh every second",
                                    "value": "frames",
                                },
                                "minute": {
                                    "label": "Minute",
                                    "description": "Refresh every minute",
                                },
                                "frame": {
                                    "label": "Frame",
                                },
                            },
                        },
                    },
                    "sao_menu": {
                        "name": "Script Demo",
                        "script_label": "Script Demo",
                        "actions": {
                            "script.state": {"label": "Read state"},
                        },
                        "priority": 99,
                        "surface": "other_surface",
                    },
                },
                "en-GB": {
                    "sao_menu": {
                        "name": "Script Demo UK",
                    },
                },
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


def _write_action_script_two_plugin(root: str) -> str:
    plugin_dir = os.path.join(root, "action_script_two_demo")
    os.makedirs(plugin_dir, exist_ok=True)
    with open(os.path.join(plugin_dir, "plugin.json"), "w", encoding="utf-8") as fp:
        json.dump({
            "id": "action_script_two_demo",
            "name": "Action Script Two Demo",
            "version": "0.1.0",
            "entry": "plugin.lua",
            "language": "lua",
            "enabled": True,
        }, fp, ensure_ascii=False, indent=2)
    with open(os.path.join(plugin_dir, "plugin.lua"), "w", encoding="utf-8") as fp:
        fp.write("-- fake runtime shares the cached lua engine\n")
    return plugin_dir


def _write_hot_script_plugin(root: str) -> str:
    plugin_dir = os.path.join(root, "hot_script_demo")
    os.makedirs(plugin_dir, exist_ok=True)
    with open(os.path.join(plugin_dir, "plugin.json"), "w", encoding="utf-8") as fp:
        json.dump({
            "id": "hot_script_demo",
            "name": "Hot Script Demo",
            "version": "0.1.0",
            "entry": "plugin.lua",
            "language": "lua",
            "enabled": True,
            "settings_schema": {
                "overlay_enabled": {"type": "boolean", "default": False},
            },
            "sao_menu": {
                "name": "热加载脚本",
                "icon_text": "◷",
                "script_label": "热加载脚本",
                "priority": 24,
            },
        }, fp, ensure_ascii=False, indent=2)
    with open(os.path.join(plugin_dir, "plugin.lua"), "w", encoding="utf-8") as fp:
        fp.write("-- hot discovery must not load this script\n")
    return plugin_dir


HOT_REMOVE_CODE = r'''
def on_load(ctx):
    ctx.set_overlay('unioverlay', ctx.ui.panel('Hot Remove', [ctx.ui.text('live')]))
    ctx.register_action_handler(lambda action, payload: {'action': action})
'''


class FakeSettings:
    def __init__(self, data=None) -> None:
        self.data = dict(data or {})
        self.save_count = 0

    def get(self, key, default=None):
        return self.data.get(key, default)

    def set(self, key, value) -> None:
        self.data[key] = value

    def save(self) -> None:
        self.save_count += 1


def _write_hot_remove_plugin(root: str) -> str:
    plugin_dir = os.path.join(root, "hot_remove_demo")
    os.makedirs(plugin_dir, exist_ok=True)
    with open(os.path.join(plugin_dir, "plugin.json"), "w", encoding="utf-8") as fp:
        json.dump({
            "id": "hot_remove_demo",
            "name": "Hot Remove Demo",
            "version": "0.1.0",
            "entry": "plugin.py",
            "enabled": True,
        }, fp, ensure_ascii=False, indent=2)
    with open(os.path.join(plugin_dir, "plugin.py"), "w", encoding="utf-8") as fp:
        fp.write(HOT_REMOVE_CODE)
    return plugin_dir


def _write_lazy_panel_plugin(root: str) -> str:
    plugin_dir = os.path.join(root, "lazy_panel_demo")
    os.makedirs(plugin_dir, exist_ok=True)
    with open(os.path.join(plugin_dir, "plugin.json"), "w", encoding="utf-8") as fp:
        json.dump({
            "id": "lazy_panel_demo",
            "name": "Lazy Panel Demo",
            "version": "0.1.0",
            "entry": "plugin.py",
            "enabled": True,
            "capabilities": [{"id": "ui_panels", "title": "Lazy Panel"}],
        }, fp, ensure_ascii=False, indent=2)
    with open(os.path.join(plugin_dir, "plugin.py"), "w", encoding="utf-8") as fp:
        fp.write("def on_load(ctx):\n    raise RuntimeError('ui panel listing should not load me')\n")
    return plugin_dir


def run_selftest() -> dict:
    angel = _AngelScriptInterpreter()
    angel.execute_source(r'''
string token = "";
dictionary@ state()
{
    return {"active": token != "", "sum": 1 + 2, "label": "x" + "y"};
}
''')
    angel_state = angel.get_function("state")()
    assert isinstance(angel_state, dict), angel_state
    assert angel_state == {"active": False, "sum": 3, "label": "xy"}, angel_state

    bus = EventBus()
    direct_events = []
    bus.subscribe("damage", direct_events.append, owner_id="selftest")
    bus.publish("damage", {"damage": 7}, source_name="selftest", source_kind="unit")
    assert direct_events and direct_events[0]["payload"]["damage"] == 7, direct_events

    assert built_in_parser_adapters() == [], built_in_parser_adapters()

    workspace_cutegirl_contract = False
    workspace_base = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir))
    workspace_cutegirl_manifest = os.path.abspath(
        os.path.join(workspace_base, os.pardir, os.pardir, "plugins", "script_cutegirl_csharp", "plugin.json")
    )
    if os.path.isfile(workspace_cutegirl_manifest):
        # 对照当前 Unity/VRM 桌宠 (Xiao-ACT-peto) 的 manifest 契约。
        # 旧 Tomurai FBX 版的 avatar_contract 字段随插件重写移除, 运行时
        # 从未消费过该字段, 动作/手势/表情现走引擎侧 IPC 不经 manifest。
        from .runtime import default_plugin_dirs

        workspace_settings = FakeSettings({"act_plugin_locale": "zh-CN"})
        workspace_bus = EventBus()
        workspace_manager = PluginManager(
            plugin_dirs=default_plugin_dirs(workspace_base),
            event_bus=workspace_bus,
            settings=workspace_settings,
        )
        workspace_manager.discover()
        cutegirl_record = workspace_manager._records.get("script_cutegirl_csharp")
        assert cutegirl_record is not None, workspace_manager.status()
        assert cutegirl_record.language == "csharp", cutegirl_record
        assert cutegirl_record.entry == "plugin.cs", cutegirl_record
        assert not cutegirl_record.last_error, cutegirl_record
        assert os.path.abspath(cutegirl_record.path).endswith(
            os.path.join("plugins", "script_cutegirl_csharp")
        ), cutegirl_record.path
        assert "act_platform>=1.0" in cutegirl_record.requires, cutegirl_record.requires
        assert "runtime_feature:unioverlay" in cutegirl_record.requires, cutegirl_record.requires
        cutegirl_schema = cutegirl_record.localized_settings_schema("zh-CN")
        assert cutegirl_schema.get("overlay_enabled", {}).get("default") is True, cutegirl_schema
        assert cutegirl_schema.get("target_fps", {}).get("default") == 90, cutegirl_schema
        assert cutegirl_schema.get("model_path", {}).get("default") == "", cutegirl_schema
        cutegirl_menu = cutegirl_record.localized_sao_menu("zh-CN")
        assert cutegirl_menu.get("surface") == "unioverlay", cutegirl_menu
        assert cutegirl_menu.get("setting") == "overlay_enabled", cutegirl_menu
        cutegirl_record.enabled = True
        menu_entry = next(
            item for item in workspace_manager.list_script_menu_entries("zh-CN")
            if item.get("id") == "script_cutegirl_csharp"
        )
        assert menu_entry.get("surface") == "unioverlay", menu_entry
        assert menu_entry.get("overlay_enabled") is True, menu_entry
        assert menu_entry.get("menu", {}).get("name") == "桌宠", menu_entry
        workspace_cutegirl_contract = True

    with tempfile.TemporaryDirectory(prefix="act_plugin_selftest_") as root:
        _write_demo_plugin(root)
        _write_script_menu_plugin(root)
        _write_failing_script_plugin(root)
        _write_action_script_plugin(root)
        settings = FakeSettings({"act_plugin_locale": "zh-CN"})
        manager = PluginManager(plugin_dirs=[root], event_bus=bus, settings=settings)
        lifecycle_events = []
        bus.subscribe("plugin_lifecycle", lifecycle_events.append, owner_id="selftest_lifecycle")
        manager.discover()
        manifest_status = manager.status()
        script_record = next(
            item for item in manifest_status.get("plugins", [])
            if item.get("id") == "script_menu_demo"
        )
        assert manifest_status.get("locale") == "zh-CN", manifest_status
        assert script_record.get("name") == "脚本菜单演示", script_record
        assert script_record.get("description") == "中文脚本菜单演示", script_record
        assert script_record.get("sao_menu", {}).get("name") == "脚本演示", script_record
        assert script_record.get("settings_schema", {}).get("overlay_enabled", {}).get("description") == "显示叠加层", script_record
        zh_detail = script_record.get("settings_schema", {}).get("detail_mode", {})
        assert zh_detail.get("enum_labels", {}).get("seconds") == "秒级刷新", script_record
        assert zh_detail.get("options", [{}])[0].get("label") == "秒级", script_record
        assert script_record.get("sao_menu", {}).get("actions", [{}])[0].get("label") == "读取状态", script_record
        settings.data["act_plugin_locale"] = "en-US"
        localized_status = manager.status()
        localized_record = next(
            item for item in localized_status.get("plugins", [])
            if item.get("id") == "script_menu_demo"
        )
        assert localized_status.get("locale") == "en-US", localized_status
        assert localized_record.get("name") == "Script Menu Demo", localized_record
        assert localized_record.get("default_name") == "脚本菜单演示", localized_record
        assert localized_record.get("description") == "Localized script menu demo", localized_record
        assert localized_record.get("sao_menu", {}).get("name") == "Script Demo", localized_record
        assert localized_record.get("sao_menu", {}).get("priority") == 25.0, localized_record
        assert localized_record.get("sao_menu", {}).get("surface") != "other_surface", localized_record
        assert localized_record.get("settings_schema", {}).get("overlay_enabled", {}).get("description") == "Show overlay", localized_record
        assert localized_record.get("settings_schema", {}).get("overlay_enabled", {}).get("default") is True, localized_record
        localized_detail = localized_record.get("settings_schema", {}).get("detail_mode", {})
        assert localized_detail.get("description") == "Refresh detail", localized_record
        assert localized_detail.get("default") == "seconds", localized_record
        assert localized_detail.get("enum") == ["seconds", "minute"], localized_record
        assert localized_detail.get("enum_labels", {}).get("seconds") == "Seconds", localized_record
        assert localized_detail.get("enum_labels", {}).get("minute") == "Minute", localized_record
        assert "frame" not in localized_detail.get("enum_labels", {}), localized_record
        localized_options = localized_detail.get("options") or []
        assert localized_options[0].get("label") == "Seconds", localized_record
        assert localized_options[0].get("description") == "Refresh every second", localized_record
        assert localized_options[0].get("value") == "seconds", localized_record
        assert localized_options[1].get("label") == "Minute", localized_record
        assert len(localized_options) == 2, localized_record
        assert localized_record.get("sao_menu", {}).get("actions", [{}])[0].get("label") == "Read state", localized_record
        settings.data["act_plugin_locale"] = "en-GB"
        gb_record = next(
            item for item in manager.status().get("plugins", [])
            if item.get("id") == "script_menu_demo"
        )
        assert gb_record.get("name") == "Script Menu Demo", gb_record
        assert gb_record.get("sao_menu", {}).get("name") == "Script Demo UK", gb_record
        settings.data["act_plugin_locale"] = "C.UTF-8"
        c_locale_status = manager.status()
        assert c_locale_status.get("locale") == "zh-CN", c_locale_status
        settings.data["act_plugin_locale"] = "zh-CN"
        assert manager.load_plugin("capture_demo"), manager.status()
        assert manager.render_registry.status().get("overlay_count") == 1, manager.render_registry.status()
        invalidate_events = []
        invalidate_token = bus.subscribe(
            "plugin_ui_invalidate", invalidate_events.append, owner_id="selftest_overlay_dedupe")
        current_overlay = manager.render_registry.overlays("unioverlay")[0]["spec"]
        before_overlay_status = manager.render_registry.status()
        manager._set_overlay("capture_demo", "unioverlay", current_overlay)
        after_same_status = manager.render_registry.status()
        assert after_same_status.get("overlay_unchanged_count") == (
            before_overlay_status.get("overlay_unchanged_count", 0) + 1
        ), after_same_status
        assert not invalidate_events, invalidate_events
        changed_overlay = dict(current_overlay)
        changed_overlay["title"] = "Demo Overlay Changed"
        manager._set_overlay("capture_demo", "unioverlay", changed_overlay)
        assert len(invalidate_events) == 1, invalidate_events
        assert invalidate_events[0].get("payload", {}).get("reason") == "overlay_set", invalidate_events
        bus.unsubscribe(invalidate_token)
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
        owner.settings = settings
        from .runtime import (
            act_plugin_action,
            act_plugin_menu,
            act_plugin_script_menus,
            act_plugin_status,
            act_plugin_ui_panels,
            render_overlays,
        )

        from . import scripting
        original_create_runtime = scripting._create_runtime
        original_runtime_cache = dict(scripting._runtimes)
        runtime_create_calls = []

        def _unexpected_runtime_create(language):
            runtime_create_calls.append(language)
            raise RuntimeError("status must not instantiate script runtimes")

        scripting._runtimes.clear()
        scripting._create_runtime = _unexpected_runtime_create
        try:
            lazy_status = manager.status()
        finally:
            scripting._create_runtime = original_create_runtime
            scripting._runtimes.clear()
            scripting._runtimes.update(original_runtime_cache)
        assert not runtime_create_calls, runtime_create_calls
        assert lazy_status.get("script_runtimes", {}).get("lua", {}).get("loaded") is False, lazy_status

        script_menus = act_plugin_script_menus(owner)
        assert script_menus.get("ok"), script_menus
        assert not any(
            item.get("id") == "script_menu_demo"
            for item in script_menus.get("items", [])
        ), script_menus
        manager._records["script_menu_demo"].enabled = True
        settings.data["act_plugin_locale"] = "en-US"
        enabled_script_menus = act_plugin_script_menus(owner)
        script_item = next(
            item for item in enabled_script_menus.get("items", [])
            if item.get("id") == "script_menu_demo"
        )
        assert script_item.get("enabled") is True, script_item
        assert script_item.get("overlay_enabled") is True, script_item
        assert script_item.get("name") == "Script Menu Demo", script_item
        assert script_item.get("menu", {}).get("name") == "Script Demo", script_item
        assert script_item.get("menu", {}).get("actions", [{}])[0].get("label") == "Read state", script_item
        assert script_item.get("menu", {}).get("priority") == 25.0, script_item
        assert script_item.get("surface") == "unioverlay", script_item
        manager.set_plugin_setting("script_menu_demo", "overlay_enabled", False)
        settings.data["act_plugin_enabled"] = {"script_menu_demo": True}
        restarted_manager = PluginManager(plugin_dirs=[root], event_bus=bus, settings=settings)
        restarted_manager.discover()
        restarted_entries = restarted_manager.list_script_menu_entries("en-US")
        restarted_item = next(
            item for item in restarted_entries
            if item.get("id") == "script_menu_demo"
        )
        assert restarted_item.get("enabled") is True, restarted_item
        assert restarted_item.get("overlay_enabled") is False, restarted_item
        assert restarted_item.get("loaded") is False, restarted_item
        assert restarted_manager._records["script_menu_demo"].loaded is False, restarted_manager.status()
        assert settings.save_count >= 1, settings.save_count
        settings.data["act_plugin_locale"] = "zh-CN"
        manager._records["script_menu_demo"].enabled = False
        menu_summary = act_plugin_menu(owner)
        assert not manager._records["failing_script_demo"].loaded, manager.status()
        assert not manager._records["action_script_demo"].loaded, manager.status()
        assert not any(
            item.get("id") == "script_menu_demo"
            for item in menu_summary.get("script_menus", [])
        ), menu_summary.get("script_menus")

        hot_script_dir = _write_hot_script_plugin(root)
        hot_script_menus = act_plugin_script_menus(owner)
        assert any(
            item.get("id") == "hot_script_demo"
            for item in hot_script_menus.get("items", [])
        ), hot_script_menus
        assert manager._records["hot_script_demo"].loaded is False, manager.status()
        hot_manifest = os.path.join(hot_script_dir, "plugin.json")
        with open(hot_manifest, "r", encoding="utf-8") as fp:
            hot_data = json.load(fp)
        hot_data["sao_menu"]["name"] = "热加载脚本已更新"
        hot_data["settings_schema"]["overlay_enabled"]["description"] = "更新后的热加载描述"
        with open(hot_manifest, "w", encoding="utf-8") as fp:
            json.dump(hot_data, fp, ensure_ascii=False, indent=2)
        manager.sync_discovery(force=True)
        hot_updated_menus = act_plugin_script_menus(owner)
        hot_updated = next(
            item for item in hot_updated_menus.get("items", [])
            if item.get("id") == "hot_script_demo"
        )
        assert hot_updated.get("menu", {}).get("name") == "热加载脚本已更新", hot_updated
        assert manager._records["hot_script_demo"].loaded is False, manager.status()
        assert manager._records["hot_script_demo"].localized_settings_schema().get(
            "overlay_enabled", {}).get("description") == "更新后的热加载描述", manager.status()
        assert any(
            ev.get("payload", {}).get("plugin_id") == "hot_script_demo"
            and ev.get("payload", {}).get("action") == "manifest_changed"
            for ev in lifecycle_events
        ), lifecycle_events

        hot_remove_dir = _write_hot_remove_plugin(root)
        manager.sync_discovery(force=True)
        assert manager.load_plugin("hot_remove_demo"), manager.status()
        assert manager.render_registry.status().get("overlay_count") == 1, manager.render_registry.status()
        shutil.rmtree(hot_remove_dir)
        removed_overlays = render_overlays(owner, "unioverlay")
        assert removed_overlays.get("ok"), removed_overlays
        assert removed_overlays.get("overlays") == [], removed_overlays
        assert "hot_remove_demo" not in manager._records, manager.status()
        assert manager.render_registry.status().get("overlay_count") == 0, manager.render_registry.status()
        assert any(
            ev.get("payload", {}).get("plugin_id") == "hot_remove_demo"
            and ev.get("payload", {}).get("action") == "forgotten"
            for ev in lifecycle_events
        ), lifecycle_events

        _write_lazy_panel_plugin(root)
        lazy_panels = act_plugin_ui_panels(owner)
        assert lazy_panels.get("ok"), lazy_panels
        assert "lazy_panel_demo" in manager._records, manager.status()
        assert manager._records["lazy_panel_demo"].loaded is False, manager.status()
        assert manager._records["lazy_panel_demo"].failures == 0, manager.status()
        assert not any(
            panel.get("plugin_id") == "lazy_panel_demo"
            for panel in lazy_panels.get("panels", [])
        ), lazy_panels

        class FakeLuaRuntime:
            def __init__(self) -> None:
                self.loaded = []
                self.unloaded = []
                self.disposed = 0

            def load_script(self, _entry_path, record, ctx):
                self.loaded.append(record.plugin_id)
                if record.plugin_id in {"action_script_demo", "action_script_two_demo"}:
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

            def dispose(self) -> None:
                self.disposed += 1

        _write_action_script_two_plugin(root)
        manager.sync_discovery(force=True)
        original_runtime_cache = dict(scripting._runtimes)
        original_create_runtime = scripting._create_runtime
        fake_runtime = FakeLuaRuntime()
        runtime_create_calls = []

        def _unexpected_runtime_create(language):
            runtime_create_calls.append(language)
            raise RuntimeError("unload must not instantiate script runtimes")

        scripting._runtimes.clear()
        scripting._runtimes["lua"] = fake_runtime
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
            action_two_result = act_plugin_action(
                owner,
                "script.overlay.set_enabled",
                {"enabled": True},
                plugin_id="action_script_two_demo",
            )
            assert action_two_result.get("ok"), action_two_result
            assert fake_runtime.loaded == ["action_script_demo", "action_script_two_demo"], fake_runtime.loaded
            assert manager._records["action_script_two_demo"].loaded, manager.status()
            assert scripting.runtime_loaded("lua"), scripting.list_runtimes()
            assert manager.forget_plugin("action_script_demo"), manager.status()
            assert fake_runtime.unloaded[-1:] == ["action_script_demo"], fake_runtime.unloaded
            assert scripting.runtime_loaded("lua"), scripting.list_runtimes()
            assert fake_runtime.disposed == 0, fake_runtime.disposed
            assert manager.forget_plugin("action_script_two_demo"), manager.status()
            assert fake_runtime.unloaded[-1:] == ["action_script_two_demo"], fake_runtime.unloaded
            assert not scripting.runtime_loaded("lua"), scripting.list_runtimes()
            assert fake_runtime.disposed == 1, fake_runtime.disposed
            assert not manager._records["failing_script_demo"].loaded, manager.status()
            scripting._runtimes["lua"] = fake_runtime
            assert not manager.load_plugin("failing_script_demo"), manager.status()
            assert fake_runtime.unloaded[-1:] == ["failing_script_demo"], fake_runtime.unloaded
            assert not scripting.runtime_loaded("lua"), scripting.list_runtimes()
            assert fake_runtime.disposed == 2, fake_runtime.disposed
            assert not manager.dispatch_plugin_action(
                "script.overlay.set_enabled",
                {"enabled": False},
                plugin_id="action_script_demo",
            ).get("ok")
            scripting._create_runtime = _unexpected_runtime_create
            manager._records["failing_script_demo"].script_runtime_active = True
            manager._unload_script_runtime(manager._records["failing_script_demo"])
            assert not runtime_create_calls, runtime_create_calls
            assert manager._records["failing_script_demo"].script_runtime_active is False
        finally:
            scripting._create_runtime = original_create_runtime
            scripting._runtimes.clear()
            scripting._runtimes.update(original_runtime_cache)
        assert fake_runtime.loaded == [
            "action_script_demo",
            "action_script_two_demo",
            "failing_script_demo",
        ], fake_runtime.loaded
        assert fake_runtime.unloaded == [
            "action_script_demo",
            "action_script_two_demo",
            "failing_script_demo",
        ], fake_runtime.unloaded
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
        assert any(
            ev.get("payload", {}).get("plugin_id") == "action_script_demo"
            and ev.get("payload", {}).get("action") == "forgotten"
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
        "workspace_cutegirl_contract": workspace_cutegirl_contract,
    }


def _failure_detail_tail(detail: str) -> list[str]:
    lines = str(detail or "").splitlines()
    if len(lines) <= _FAILURE_DETAIL_LINE_LIMIT:
        return lines
    omitted = len(lines) - _FAILURE_DETAIL_LINE_LIMIT
    return [f"... omitted {omitted} earlier detail lines ...", *lines[-_FAILURE_DETAIL_LINE_LIMIT:]]


def _failure_point_reason(exc: BaseException) -> str:
    message = str(exc).strip()
    if message:
        first_line = message.splitlines()[0].strip()
        if first_line:
            return first_line
    return f"{type(exc).__name__} without detail"


def _print_final_failed_check_points(exc: BaseException, detail: str) -> None:
    print()
    print("=" * 50)
    print(_FINAL_FAILURE_POINT_HEADING)
    print(f"  1. {type(exc).__name__}: {_failure_point_reason(exc)}")
    detail_lines = _failure_detail_tail(detail)
    if detail_lines:
        print("     detail:")
        for line in detail_lines:
            print(f"       {line}")


def main() -> int:
    try:
        print(json.dumps(run_selftest(), ensure_ascii=False, indent=2))
        return 0
    except Exception as exc:
        print(json.dumps({
            "ok": False,
            "failure_points_deferred": True,
        }, ensure_ascii=False, indent=2))
        _print_final_failed_check_points(exc, traceback.format_exc().rstrip())
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
