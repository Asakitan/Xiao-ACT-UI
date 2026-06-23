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
from .runtime import act_plugin_action, act_plugin_disable, act_plugin_enable, render_overlays


def _workspace_root() -> Path:
    return Path(__file__).resolve().parents[3]


def _state(payload: dict[str, Any]) -> dict[str, Any]:
    result = payload.get("result") if isinstance(payload.get("result"), dict) else {}
    state = result.get("state") if isinstance(result.get("state"), dict) else {}
    return dict(state or {})


def _require_action(owner: Any, plugin_id: str, action_id: str,
                    payload: dict[str, Any], stage: str) -> tuple[dict[str, Any] | None, dict[str, Any]]:
    result = act_plugin_action(owner, action_id, payload, plugin_id=plugin_id)
    if not result.get("ok"):
        return {"ok": False, "plugin_id": plugin_id, "stage": stage, "result": result}, {}
    return None, _state(result)


def _plugin_timer_active(manager: PluginManager, plugin_id: str) -> bool:
    return bool(manager._timers.get(plugin_id) or {})


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
    owner._act_plugin_manager = manager
    records = {record.plugin_id: record for record in manager.discover()}
    required = (
        "script_world_clock_angelscript",
        "script_stickwoman_csharp",
        "script_flappy_emma",
        "script_snake_lua",
    )
    missing = [plugin_id for plugin_id in required if plugin_id not in records]
    if missing:
        return {
            "ok": False,
            "missing": missing,
            "plugins": sorted(records),
        }

    summaries: dict[str, dict[str, Any]] = {}
    for index, plugin_id in enumerate(required):
        enabled = act_plugin_enable(owner, plugin_id)
        if not enabled.get("ok"):
            return {"ok": False, "plugin_id": plugin_id, "stage": "enable", "result": enabled}
        try:
            failure, state = _require_action(
                owner, plugin_id, "script.overlay.set_enabled", {"enabled": True}, "start")
            if failure:
                return failure
            failure, state = _require_action(owner, plugin_id, "script.state", {}, "state")
            if failure:
                return failure
            if state.get("overlay_enabled") is not True:
                return {"ok": False, "plugin_id": plugin_id, "stage": "state_enabled", "state": state}
            if state.get("surface") != "unioverlay":
                return {"ok": False, "plugin_id": plugin_id, "stage": "surface", "state": state}
            if state.get("draggable") is not True:
                return {"ok": False, "plugin_id": plugin_id, "stage": "draggable", "state": state}

            target_pos = {"x": 111 + index, "y": 222 + index, "z": 33 + index}
            failure, positioned = _require_action(
                owner, plugin_id, "script.overlay.set_position", target_pos, "position")
            if failure:
                return failure
            for axis, expected in target_pos.items():
                if int(float(positioned.get(axis, -999999))) != expected:
                    return {
                        "ok": False,
                        "plugin_id": plugin_id,
                        "stage": "position_state",
                        "axis": axis,
                        "expected": expected,
                        "state": positioned,
                    }
            partial_target = {"x": target_pos["x"] + 10}
            failure, partial = _require_action(
                owner, plugin_id, "script.overlay.set_position", partial_target, "position_partial")
            if failure:
                return failure
            if int(float(partial.get("x", -999999))) != partial_target["x"]:
                return {"ok": False, "plugin_id": plugin_id, "stage": "position_partial_x", "state": partial}
            for axis in ("y", "z"):
                if int(float(partial.get(axis, -999999))) != target_pos[axis]:
                    return {
                        "ok": False,
                        "plugin_id": plugin_id,
                        "stage": "position_partial_preserve",
                        "axis": axis,
                        "state": partial,
                    }
            positioned = partial

            if plugin_id == "script_world_clock_angelscript":
                if state.get("timer_active") is not True or not _plugin_timer_active(manager, plugin_id):
                    return {"ok": False, "plugin_id": plugin_id, "stage": "clock_timer_active", "state": state}
                if abs(float(state.get("tick_interval") or 0.0) - 1.0) > 0.0001:
                    return {"ok": False, "plugin_id": plugin_id, "stage": "clock_tick_interval", "state": state}
                if int(state.get("render_count") or 0) < 1:
                    return {"ok": False, "plugin_id": plugin_id, "stage": "clock_render_count", "state": state}
                refresh = act_plugin_action(
                    owner, "script.clock.refresh", {}, plugin_id=plugin_id)
                refresh_state = _state(refresh)
                if int(refresh_state.get("render_count") or 0) <= int(positioned.get("render_count") or 0):
                    return {
                        "ok": False,
                        "plugin_id": plugin_id,
                        "stage": "clock_refresh_render",
                        "before": positioned,
                        "after": refresh_state,
                    }
                redraw = act_plugin_action(
                    owner, "script.clock.redraw", {}, plugin_id=plugin_id)
                redraw_state = _state(redraw)
                redraw_result = redraw.get("result") if isinstance(redraw.get("result"), dict) else {}
                if redraw_result.get("redraw_changed") is not False:
                    return {"ok": False, "plugin_id": plugin_id, "stage": "clock_redraw_cache_flag", "result": redraw}
                if int(redraw_state.get("cache_hit_count") or 0) <= int(refresh_state.get("cache_hit_count") or 0):
                    return {
                        "ok": False,
                        "plugin_id": plugin_id,
                        "stage": "clock_redraw_cache_hit",
                        "before": refresh_state,
                        "after": redraw_state,
                    }
                state = dict(redraw_state)

            if plugin_id == "script_flappy_emma":
                if not state.get("level") or not state.get("pipe_speed") or not state.get("gap_half"):
                    return {"ok": False, "plugin_id": plugin_id, "stage": "difficulty_state", "state": state}
                if state.get("started") is not False:
                    return {"ok": False, "plugin_id": plugin_id, "stage": "ready_state", "state": state}
                if state.get("timer_active") is not False or _plugin_timer_active(manager, plugin_id):
                    return {"ok": False, "plugin_id": plugin_id, "stage": "flappy_ready_timer_idle", "state": state}
                action = act_plugin_action(owner, "script.game.flap", {}, plugin_id=plugin_id)
                action_state = _state(action)
                if action_state.get("started") is not True:
                    return {"ok": False, "plugin_id": plugin_id, "stage": "flap_started", "state": action_state}
                if action_state.get("timer_active") is not True or not _plugin_timer_active(manager, plugin_id):
                    return {"ok": False, "plugin_id": plugin_id, "stage": "flappy_flap_timer_active", "state": action_state}
                queued_action = act_plugin_action(owner, "script.game.flap", {}, plugin_id=plugin_id)
                queued_state = _state(queued_action)
                if int(queued_state.get("queued_flaps") or 0) != 1:
                    return {"ok": False, "plugin_id": plugin_id, "stage": "flappy_buffered_flap_queued", "state": queued_state}
                tick_action = act_plugin_action(owner, "script.game.tick", {}, plugin_id=plugin_id)
                tick_state = _state(tick_action)
                if int(tick_state.get("frame_count") or 0) < 1:
                    return {"ok": False, "plugin_id": plugin_id, "stage": "flappy_tick_frame_count", "state": tick_state}
                if int(tick_state.get("queued_flaps") or 0) != 0:
                    return {"ok": False, "plugin_id": plugin_id, "stage": "flappy_buffered_flap_consumed", "state": tick_state}
                if int(tick_state.get("render_count") or 0) <= int(queued_state.get("render_count") or 0):
                    return {
                        "ok": False,
                        "plugin_id": plugin_id,
                        "stage": "flappy_tick_render_count",
                        "before": queued_state,
                        "after": tick_state,
                    }
                paused_action = act_plugin_action(
                    owner, "script.game.pause", {"paused": True}, plugin_id=plugin_id)
                paused_state = _state(paused_action)
                if paused_state.get("paused") is not True:
                    return {"ok": False, "plugin_id": plugin_id, "stage": "flappy_pause_state", "state": paused_state}
                if paused_state.get("timer_active") is not False or _plugin_timer_active(manager, plugin_id):
                    return {"ok": False, "plugin_id": plugin_id, "stage": "flappy_pause_timer_idle", "state": paused_state}
                action = act_plugin_action(
                    owner, "script.game.pause", {"paused": False}, plugin_id=plugin_id)
                action_state = _state(action)
                if action_state.get("timer_active") is not True or not _plugin_timer_active(manager, plugin_id):
                    return {"ok": False, "plugin_id": plugin_id, "stage": "flappy_resume_timer_active", "state": action_state}
            else:
                action: dict[str, Any] = {"ok": True}
            if plugin_id == "script_snake_lua":
                if not state.get("level") or not state.get("tick_interval"):
                    return {"ok": False, "plugin_id": plugin_id, "stage": "difficulty_state", "state": state}
                if state.get("timer_active") is not True or not _plugin_timer_active(manager, plugin_id):
                    return {"ok": False, "plugin_id": plugin_id, "stage": "snake_running_timer_active", "state": state}
                paused_action = act_plugin_action(
                    owner, "script.game.pause", {"paused": True}, plugin_id=plugin_id)
                paused_state = _state(paused_action)
                if paused_state.get("paused") is not True:
                    return {"ok": False, "plugin_id": plugin_id, "stage": "snake_pause_state", "state": paused_state}
                if paused_state.get("timer_active") is not False or _plugin_timer_active(manager, plugin_id):
                    return {"ok": False, "plugin_id": plugin_id, "stage": "snake_pause_timer_idle", "state": paused_state}
                resume_action = act_plugin_action(
                    owner, "script.game.pause", {"paused": False}, plugin_id=plugin_id)
                resume_state = _state(resume_action)
                if resume_state.get("timer_active") is not True or not _plugin_timer_active(manager, plugin_id):
                    return {"ok": False, "plugin_id": plugin_id, "stage": "snake_resume_timer_active", "state": resume_state}
                queued_first = _state(act_plugin_action(
                    owner, "script.game.direction", {"direction": "up"}, plugin_id=plugin_id))
                queued_second = _state(act_plugin_action(
                    owner, "script.game.direction", {"direction": "left"}, plugin_id=plugin_id))
                if queued_second.get("pending_turns") != ["up", "left"]:
                    return {
                        "ok": False,
                        "plugin_id": plugin_id,
                        "stage": "snake_buffered_turns_queue",
                        "first": queued_first,
                        "second": queued_second,
                    }
                tick_one = _state(act_plugin_action(
                    owner, "script.game.tick", {}, plugin_id=plugin_id))
                if tick_one.get("direction") != "up" or tick_one.get("next_direction") != "left":
                    return {"ok": False, "plugin_id": plugin_id, "stage": "snake_buffered_turns_first_tick", "state": tick_one}
                tick_two = _state(act_plugin_action(
                    owner, "script.game.tick", {}, plugin_id=plugin_id))
                if tick_two.get("direction") != "left":
                    return {"ok": False, "plugin_id": plugin_id, "stage": "snake_buffered_turns_second_tick", "state": tick_two}
                action = act_plugin_action(
                    owner, "script.game.direction", {"direction": "down"}, plugin_id=plugin_id)
            elif plugin_id == "script_stickwoman_csharp":
                if state.get("timer_active") is not True or not _plugin_timer_active(manager, plugin_id):
                    return {"ok": False, "plugin_id": plugin_id, "stage": "stickwoman_timer_active", "state": state}
                if abs(float(state.get("tick_interval") or 0.0) - 0.05) > 0.0001:
                    return {"ok": False, "plugin_id": plugin_id, "stage": "stickwoman_tick_interval", "state": state}
                action = act_plugin_action(
                    owner, "script.avatar.action", {"name": "walk"}, plugin_id=plugin_id)
                action_state = _state(action)
                if action_state.get("action") != "walk":
                    return {"ok": False, "plugin_id": plugin_id, "stage": "stickwoman_action", "state": action_state}
                if int(action_state.get("render_count") or 0) < 1:
                    return {"ok": False, "plugin_id": plugin_id, "stage": "stickwoman_render_count", "state": action_state}
                spec_build_count = int(action_state.get("spec_build_count") or 0)
                redraw = act_plugin_action(owner, "script.model3d.redraw", {}, plugin_id=plugin_id)
                redraw_state = _state(redraw)
                if int(redraw_state.get("spec_build_count") or 0) != spec_build_count:
                    return {
                        "ok": False,
                        "plugin_id": plugin_id,
                        "stage": "stickwoman_spec_cache_reuse",
                        "before": action_state,
                        "after": redraw_state,
                    }
                if int(redraw_state.get("render_count") or 0) <= int(action_state.get("render_count") or 0):
                    return {
                        "ok": False,
                        "plugin_id": plugin_id,
                        "stage": "stickwoman_redraw_count",
                        "before": action_state,
                        "after": redraw_state,
                    }
                action_state = redraw_state
                state = dict(state)
                state["action"] = action_state.get("action")
                state["timer_active"] = action_state.get("timer_active")
                state["tick_interval"] = action_state.get("tick_interval")
                state["tick_count"] = action_state.get("tick_count")
                state["render_count"] = action_state.get("render_count")
                state["spec_build_count"] = action_state.get("spec_build_count")
                overlays = render_overlays(owner, "unioverlay").get("overlays") or []
                overlay_text = json.dumps(overlays, ensure_ascii=False, sort_keys=True, default=str)
                if "keyframes" not in overlay_text or '"time"' not in overlay_text:
                    return {
                        "ok": False,
                        "plugin_id": plugin_id,
                        "stage": "stickwoman_overlay_keyframes",
                        "overlays": overlays,
                    }
            if not action.get("ok"):
                return {"ok": False, "plugin_id": plugin_id, "stage": "game_action", "result": action}
            summaries[plugin_id] = {
                "surface": state.get("surface"),
                "level": state.get("level"),
                "pipe_speed": state.get("pipe_speed"),
                "gap_half": state.get("gap_half"),
                "tick_interval": state.get("tick_interval"),
                "timer_active": state.get("timer_active"),
                "draggable": state.get("draggable"),
                "x": positioned.get("x"),
                "y": positioned.get("y"),
                "z": positioned.get("z"),
                "action": state.get("action"),
                "tick_count": state.get("tick_count"),
                "render_count": state.get("render_count"),
                "spec_build_count": state.get("spec_build_count"),
                "cache_hit_count": state.get("cache_hit_count"),
                "pending_turns": state.get("pending_turns"),
                "queued_flaps": state.get("queued_flaps"),
                "frame_count": state.get("frame_count"),
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
