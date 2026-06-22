# -*- coding: utf-8 -*-
"""Smoke checks for workspace-root script plugins.

This intentionally exercises the public PluginManager/action path instead of
importing plugin internals.  The workspace-root ``plugins`` directory is not
part of the ``sao_auto`` git repository, so the test skips cleanly when that
directory is absent.
"""
from __future__ import annotations

import json
from pathlib import Path
from types import SimpleNamespace
from typing import Any

from .plugins import PluginManager
from .runtime import act_plugin_action, act_plugin_disable, act_plugin_enable


def _workspace_root() -> Path:
    return Path(__file__).resolve().parents[3]


def _state(payload: dict[str, Any]) -> dict[str, Any]:
    result = payload.get("result") if isinstance(payload.get("result"), dict) else {}
    state = result.get("state") if isinstance(result.get("state"), dict) else {}
    return dict(state or {})


def run_selftest() -> dict[str, Any]:
    root_plugins = _workspace_root() / "plugins"
    if not root_plugins.is_dir():
        return {
            "ok": True,
            "skipped": True,
            "reason": f"root plugin directory not found: {root_plugins}",
        }

    owner = SimpleNamespace(root=None)
    manager = PluginManager(plugin_dirs=[str(root_plugins)], owner_provider=lambda: owner)
    owner.act_plugin_manager = manager
    records = {record.plugin_id: record for record in manager.discover()}
    required = ("script_flappy_emma", "script_snake_lua")
    missing = [plugin_id for plugin_id in required if plugin_id not in records]
    if missing:
        return {
            "ok": False,
            "missing": missing,
            "plugins": sorted(records),
        }

    summaries: dict[str, dict[str, Any]] = {}
    for plugin_id in required:
        enabled = act_plugin_enable(owner, plugin_id)
        if not enabled.get("ok"):
            return {"ok": False, "plugin_id": plugin_id, "stage": "enable", "result": enabled}
        try:
            started = act_plugin_action(
                owner, "script.overlay.set_enabled", {"enabled": True}, plugin_id=plugin_id)
            if not started.get("ok"):
                return {"ok": False, "plugin_id": plugin_id, "stage": "start", "result": started}
            state_payload = act_plugin_action(owner, "script.state", {}, plugin_id=plugin_id)
            if not state_payload.get("ok"):
                return {"ok": False, "plugin_id": plugin_id, "stage": "state", "result": state_payload}
            state = _state(state_payload)
            if state.get("overlay_enabled") is not True:
                return {"ok": False, "plugin_id": plugin_id, "stage": "state_enabled", "state": state}
            if plugin_id == "script_flappy_emma":
                if not state.get("level") or not state.get("pipe_speed") or not state.get("gap_half"):
                    return {"ok": False, "plugin_id": plugin_id, "stage": "difficulty_state", "state": state}
                if state.get("started") is not False:
                    return {"ok": False, "plugin_id": plugin_id, "stage": "ready_state", "state": state}
                action = act_plugin_action(owner, "script.game.flap", {}, plugin_id=plugin_id)
                action_state = _state(action)
                if action_state.get("started") is not True:
                    return {"ok": False, "plugin_id": plugin_id, "stage": "flap_started", "state": action_state}
            else:
                if not state.get("level") or not state.get("tick_interval"):
                    return {"ok": False, "plugin_id": plugin_id, "stage": "difficulty_state", "state": state}
                action = act_plugin_action(
                    owner, "script.game.direction", {"direction": "down"}, plugin_id=plugin_id)
            if not action.get("ok"):
                return {"ok": False, "plugin_id": plugin_id, "stage": "game_action", "result": action}
            summaries[plugin_id] = {
                "level": state.get("level"),
                "pipe_speed": state.get("pipe_speed"),
                "gap_half": state.get("gap_half"),
                "tick_interval": state.get("tick_interval"),
                "draggable": state.get("draggable"),
            }
        finally:
            act_plugin_action(
                owner, "script.overlay.set_enabled", {"enabled": False}, plugin_id=plugin_id)
            act_plugin_disable(owner, plugin_id)

    return {
        "ok": True,
        "plugins": summaries,
        "timers_empty": not bool(manager._timers),
        "overlay_count": manager.render_registry.status().get("overlay_count"),
    }


if __name__ == "__main__":
    print(json.dumps(run_selftest(), ensure_ascii=False, indent=2, sort_keys=True))
