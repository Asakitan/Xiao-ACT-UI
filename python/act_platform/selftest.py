# -*- coding: utf-8 -*-
"""Selftest for ACT event bus and in-process Python plugin manager."""

from __future__ import annotations

import json
import os
import tempfile

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


def run_selftest() -> dict:
    bus = EventBus()
    direct_events = []
    bus.subscribe("damage", direct_events.append, owner_id="selftest")
    bus.publish("damage", {"damage": 7}, source_name="selftest", source_kind="unit")
    assert direct_events and direct_events[0]["payload"]["damage"] == 7, direct_events

    assert built_in_parser_adapters() == [], built_in_parser_adapters()

    with tempfile.TemporaryDirectory(prefix="act_plugin_selftest_") as root:
        _write_demo_plugin(root)
        manager = PluginManager(plugin_dirs=[root], event_bus=bus)
        manager.discover()
        assert manager.load_plugin("capture_demo"), manager.status()

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
        status = manager.status()

    return {
        "ok": True,
        "event_bus_contract": True,
        "plugin_manager_contract": True,
        "parser_adapter_contract": "plugin_owned",
        "replay_event_bus_contract": True,
        "published": bus.snapshot().get("published"),
        "plugin_count": status.get("plugin_count"),
        "active_count_after_disable": status.get("active_count"),
        "captured_events": len(captured),
        "built_in_parser_adapters": len(built_in_parser_adapters()),
    }


def main() -> int:
    print(json.dumps(run_selftest(), ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
