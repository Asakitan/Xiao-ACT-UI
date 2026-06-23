# -*- coding: utf-8 -*-
"""Resource lifecycle probe for workspace-root script plugins.

The probe intentionally exercises the public runtime/action path.  It does not
import root plugin internals and it does not create, modify, or delete plugin
assets.  The output is JSON so real-window/manual runs can be compared with the
same fields as headless CI-style runs.
"""
from __future__ import annotations

import argparse
import ctypes
import gc
import json
import math
import os
import sys
import threading
import time
import tracemalloc
from pathlib import Path
from types import SimpleNamespace
from typing import Any

from .event_bus import EventBus
from .plugins import PluginManager
from .runtime import (
    act_plugin_action,
    act_plugin_disable,
    act_plugin_enable,
    render_overlays,
)
from .scripting import list_runtimes


REQUIRED_PLUGINS = (
    "script_world_clock_angelscript",
    "script_stickwoman_csharp",
    "script_flappy_emma",
    "script_snake_lua",
)

STATE_KEYS = (
    "overlay_enabled",
    "timer_active",
    "tick_interval",
    "tick_count",
    "render_count",
    "spec_build_count",
    "settings_read_count",
    "settings_refresh_count",
    "settings_snapshot_loaded",
    "animation_fps",
    "action",
    "phase",
    "surface",
    "x",
    "y",
    "z",
    "static_ops_count",
    "dynamic_ops_count",
    "cache_hit_count",
    "model_reload_nonce",
    "level",
    "score",
    "target_score",
    "progress",
    "won",
    "stage_clear",
)


class ProbeSettings:
    def __init__(self) -> None:
        self.data: dict[str, Any] = {}

    def get(self, key: str, default: Any = None) -> Any:
        return self.data.get(key, default)

    def set(self, key: str, value: Any) -> None:
        self.data[key] = value

    def save(self) -> None:
        return None


def _workspace_root() -> Path:
    return Path(__file__).resolve().parents[3]


def _safe_int(value: Any, default: int = 0) -> int:
    try:
        number = int(value)
    except Exception:
        return default
    return number if math.isfinite(float(number)) else default


def _state_from_action(result: Any) -> dict[str, Any]:
    payload = result if isinstance(result, dict) else {}
    body = payload.get("result") if isinstance(payload.get("result"), dict) else {}
    state = body.get("state") if isinstance(body.get("state"), dict) else {}
    if state:
        return dict(state)
    return {key: value for key, value in body.items() if key != "ok"}


def _compact_state(state: Any) -> dict[str, Any]:
    data = state if isinstance(state, dict) else {}
    compact = {key: data.get(key) for key in STATE_KEYS if key in data}
    for key in ("snake", "pipes", "pending_turns", "queued_flaps"):
        value = data.get(key)
        if isinstance(value, (list, tuple, dict)):
            compact[f"{key}_count"] = len(value)
        elif value is not None:
            compact[key] = value
    return compact


def _windows_process_metrics() -> dict[str, Any]:
    if os.name != "nt":
        return {
            "handle_count": None,
            "gdi_objects": None,
            "user_objects": None,
            "working_set_bytes": None,
            "private_usage_bytes": None,
        }

    out: dict[str, Any] = {
        "handle_count": None,
        "gdi_objects": None,
        "user_objects": None,
        "working_set_bytes": None,
        "private_usage_bytes": None,
    }
    try:
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.GetCurrentProcess.restype = wintypes.HANDLE
        kernel32.GetProcessHandleCount.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
        kernel32.GetProcessHandleCount.restype = wintypes.BOOL
        process = kernel32.GetCurrentProcess()
        count = wintypes.DWORD(0)
        if kernel32.GetProcessHandleCount(process, ctypes.byref(count)):
            out["handle_count"] = int(count.value)
    except Exception:
        pass
    try:
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        kernel32.GetCurrentProcess.restype = wintypes.HANDLE
        user32.GetGuiResources.argtypes = (wintypes.HANDLE, wintypes.DWORD)
        user32.GetGuiResources.restype = wintypes.DWORD
        process = kernel32.GetCurrentProcess()
        out["gdi_objects"] = int(user32.GetGuiResources(process, 0))
        out["user_objects"] = int(user32.GetGuiResources(process, 1))
    except Exception:
        pass
    try:
        from ctypes import wintypes

        class PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
            _fields_ = [
                ("cb", wintypes.DWORD),
                ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t),
                ("PrivateUsage", ctypes.c_size_t),
            ]

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        kernel32.GetCurrentProcess.restype = wintypes.HANDLE
        psapi.GetProcessMemoryInfo.argtypes = (
            wintypes.HANDLE,
            ctypes.POINTER(PROCESS_MEMORY_COUNTERS_EX),
            wintypes.DWORD,
        )
        psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
        counters = PROCESS_MEMORY_COUNTERS_EX()
        counters.cb = ctypes.sizeof(counters)
        process = kernel32.GetCurrentProcess()
        if psapi.GetProcessMemoryInfo(process, ctypes.byref(counters), counters.cb):
            out["working_set_bytes"] = int(counters.WorkingSetSize)
            out["private_usage_bytes"] = int(counters.PrivateUsage)
    except Exception:
        pass
    return out


def _loaded_runtimes(runtime_status: dict[str, Any], *, include_python: bool) -> list[str]:
    return sorted(
        language
        for language, meta in runtime_status.items()
        if (include_python or language != "python")
        and isinstance(meta, dict)
        and bool(meta.get("loaded"))
    )


def _manager_metrics(manager: PluginManager) -> dict[str, Any]:
    status = manager.status()
    runtime_status = list_runtimes()
    return {
        "plugin_count": _safe_int(status.get("plugin_count")),
        "active_count": _safe_int(status.get("active_count")),
        "overlay_count": _safe_int(manager.render_registry.status().get("overlay_count")),
        "timer_count": sum(len(bucket or {}) for bucket in manager._timers.values()),
        "timer_plugins": sorted(pid for pid, bucket in manager._timers.items() if bucket),
        "loaded_runtimes": _loaded_runtimes(runtime_status, include_python=True),
        "loaded_script_runtimes": _loaded_runtimes(runtime_status, include_python=False),
        "script_runtimes": runtime_status,
    }


def _sample(manager: PluginManager, label: str, *, tracemalloc_enabled: bool) -> dict[str, Any]:
    gc.collect()
    current = peak = None
    if tracemalloc_enabled:
        current, peak = tracemalloc.get_traced_memory()
    return {
        "label": label,
        "time": time.time(),
        "process_time": time.process_time(),
        "thread_count": len(threading.enumerate()),
        "python_allocated_blocks": getattr(sys, "getallocatedblocks", lambda: None)(),
        "tracemalloc_current_bytes": current,
        "tracemalloc_peak_bytes": peak,
        "process": _windows_process_metrics(),
        "manager": _manager_metrics(manager),
    }


def _plugin_state(owner: Any, plugin_id: str) -> dict[str, Any]:
    result = act_plugin_action(owner, "script.state", {}, plugin_id=plugin_id)
    return {
        "ok": bool(result.get("ok")) if isinstance(result, dict) else False,
        "state": _compact_state(_state_from_action(result)),
        "error": "" if isinstance(result, dict) and result.get("ok") else str(result),
    }


def _compact_result(result: Any) -> dict[str, Any]:
    data = result if isinstance(result, dict) else {}
    status = data.get("status") if isinstance(data.get("status"), dict) else {}
    compact = {
        "ok": bool(data.get("ok")),
        "message": str(data.get("message") or ""),
        "errors": list(data.get("errors") or []) if isinstance(data.get("errors"), list) else [],
    }
    if status:
        compact["status"] = {
            "plugin_count": _safe_int(status.get("plugin_count")),
            "active_count": _safe_int(status.get("active_count")),
            "overlay_count": _safe_int(
                (status.get("render") if isinstance(status.get("render"), dict) else {}).get("overlay_count")
            ),
        }
    return compact


def _enable_cycle_plugin(owner: Any, plugin_id: str, fps: float) -> dict[str, Any]:
    enabled = act_plugin_enable(owner, plugin_id)
    if not enabled.get("ok"):
        return {"ok": False, "plugin_id": plugin_id, "stage": "enable", "result": _compact_result(enabled)}
    if plugin_id == "script_stickwoman_csharp":
        fps_result = act_plugin_action(
            owner,
            "script.model3d.set_fps",
            {"fps": fps},
            plugin_id=plugin_id,
        )
        if not fps_result.get("ok"):
            return {"ok": False, "plugin_id": plugin_id, "stage": "set_fps", "result": _compact_result(fps_result)}
    overlay = act_plugin_action(
        owner,
        "script.overlay.set_enabled",
        {"enabled": True},
        plugin_id=plugin_id,
    )
    if not overlay.get("ok"):
        return {"ok": False, "plugin_id": plugin_id, "stage": "overlay_on", "result": _compact_result(overlay)}
    return {"ok": True, "plugin_id": plugin_id, "state": _compact_state(_state_from_action(overlay))}


def _disable_cycle_plugin(owner: Any, plugin_id: str) -> dict[str, Any]:
    overlay = act_plugin_action(
        owner,
        "script.overlay.set_enabled",
        {"enabled": False},
        plugin_id=plugin_id,
    )
    disabled = act_plugin_disable(owner, plugin_id)
    return {
        "ok": bool(disabled.get("ok")),
        "plugin_id": plugin_id,
        "overlay": _compact_result(overlay),
        "disable": _compact_result(disabled),
    }


def _assert_idle(manager: PluginManager, stage: str) -> list[str]:
    failures: list[str] = []
    metrics = _manager_metrics(manager)
    if metrics["overlay_count"] != 0:
        failures.append(f"{stage}: overlay_count={metrics['overlay_count']}")
    if metrics["timer_count"] != 0:
        failures.append(f"{stage}: timer_count={metrics['timer_count']} plugins={metrics['timer_plugins']}")
    if metrics["active_count"] != 0:
        failures.append(f"{stage}: active_count={metrics['active_count']}")
    if metrics["loaded_script_runtimes"]:
        failures.append(f"{stage}: loaded_script_runtimes={metrics['loaded_script_runtimes']}")
    return failures


def _result_failure(result: Any, stage: str) -> str:
    data = result if isinstance(result, dict) else {}
    plugin_id = str(data.get("plugin_id") or "?")
    message = str(data.get("stage") or data.get("message") or "")
    errors = data.get("errors")
    if not errors and isinstance(data.get("result"), dict):
        errors = data["result"].get("errors")
    if errors:
        message = "; ".join(str(item) for item in errors)
    return f"{stage}: {plugin_id} failed{': ' + message if message else ''}"


def run_probe(
    *,
    fps_values: tuple[float, ...] = (4.0, 15.0, 30.0),
    cycles: int = 1,
    dwell_s: float = 0.05,
    trace_memory: bool = True,
) -> dict[str, Any]:
    root_plugins = _workspace_root() / "plugins"
    if not root_plugins.is_dir():
        return {
            "ok": True,
            "skipped": True,
            "reason": f"root plugin directory not found: {root_plugins}",
        }

    settings = ProbeSettings()
    owner = SimpleNamespace(root=None, settings=settings, _cfg_settings_ref=settings)
    manager = PluginManager(
        plugin_dirs=[str(root_plugins)],
        event_bus=EventBus(),
        owner_provider=lambda: owner,
        settings=settings,
    )
    owner._act_plugin_manager = manager
    owner.act_plugin_manager = manager

    records = {record.plugin_id: record for record in manager.discover()}
    missing = [plugin_id for plugin_id in REQUIRED_PLUGINS if plugin_id not in records]
    if missing:
        return {
            "ok": False,
            "missing": missing,
            "plugins": sorted(records),
        }

    if trace_memory and not tracemalloc.is_tracing():
        tracemalloc.start()
    samples: list[dict[str, Any]] = []
    cycle_results: list[dict[str, Any]] = []
    failures: list[str] = []

    for plugin_id in REQUIRED_PLUGINS:
        act_plugin_disable(owner, plugin_id)
    failures.extend(_assert_idle(manager, "baseline"))
    samples.append(_sample(manager, "baseline", tracemalloc_enabled=trace_memory))

    for cycle in range(max(1, int(cycles))):
        for fps in fps_values:
            step = {"cycle": cycle + 1, "fps": float(fps), "enable": [], "states": {}, "disable": []}
            for plugin_id in REQUIRED_PLUGINS:
                enable_result = _enable_cycle_plugin(owner, plugin_id, float(fps))
                step["enable"].append(enable_result)
                if not enable_result.get("ok"):
                    failures.append(_result_failure(enable_result, f"cycle_{cycle + 1}_fps_{fps:g}_enable"))
            render_overlays(owner, "unioverlay")
            if dwell_s > 0:
                time.sleep(float(dwell_s))
            for plugin_id in REQUIRED_PLUGINS:
                state_result = _plugin_state(owner, plugin_id)
                step["states"][plugin_id] = state_result
                if not state_result.get("ok"):
                    failures.append(f"cycle_{cycle + 1}_fps_{fps:g}_state: {plugin_id} failed")
            samples.append(_sample(manager, f"active_cycle_{cycle + 1}_fps_{fps:g}", tracemalloc_enabled=trace_memory))
            for plugin_id in reversed(REQUIRED_PLUGINS):
                disable_result = _disable_cycle_plugin(owner, plugin_id)
                step["disable"].append(disable_result)
                if not disable_result.get("ok"):
                    failures.append(_result_failure(disable_result, f"cycle_{cycle + 1}_fps_{fps:g}_disable"))
                overlay_result = disable_result.get("overlay") if isinstance(disable_result, dict) else {}
                if isinstance(overlay_result, dict) and not overlay_result.get("ok"):
                    failures.append(f"cycle_{cycle + 1}_fps_{fps:g}_overlay_off: {plugin_id} failed")
            render_overlays(owner, "unioverlay")
            failures.extend(_assert_idle(manager, f"after_cycle_{cycle + 1}_fps_{fps:g}"))
            samples.append(_sample(manager, f"idle_cycle_{cycle + 1}_fps_{fps:g}", tracemalloc_enabled=trace_memory))
            cycle_results.append(step)

    failures.extend(_assert_idle(manager, "final"))
    final_sample = _sample(manager, "final", tracemalloc_enabled=trace_memory)
    samples.append(final_sample)

    baseline = samples[0]
    process_fields = ("handle_count", "gdi_objects", "user_objects", "working_set_bytes", "private_usage_bytes")
    process_delta = {
        key: (
            None
            if baseline["process"].get(key) is None or final_sample["process"].get(key) is None
            else int(final_sample["process"][key]) - int(baseline["process"][key])
        )
        for key in process_fields
    }
    return {
        "ok": not failures,
        "failures": failures,
        "root_plugins": str(root_plugins),
        "required_plugins": list(REQUIRED_PLUGINS),
        "cycles": int(cycles),
        "fps_values": [float(value) for value in fps_values],
        "dwell_s": float(dwell_s),
        "process_delta": process_delta,
        "samples": samples,
        "cycle_results": cycle_results,
    }


def _parse_fps_values(value: str) -> tuple[float, ...]:
    out: list[float] = []
    for item in str(value or "").split(","):
        item = item.strip()
        if not item:
            continue
        out.append(max(0.25, min(120.0, float(item))))
    return tuple(out or [4.0, 15.0, 30.0])


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fps", default="4,15,30", help="Comma-separated stickwoman FPS values.")
    parser.add_argument("--cycles", type=int, default=1, help="Enable/disable cycles per FPS value.")
    parser.add_argument("--dwell", type=float, default=0.05, help="Seconds to leave plugins active per step.")
    parser.add_argument("--no-tracemalloc", action="store_true", help="Disable Python allocation sampling.")
    args = parser.parse_args(argv)
    result = run_probe(
        fps_values=_parse_fps_values(args.fps),
        cycles=max(1, int(args.cycles)),
        dwell_s=max(0.0, float(args.dwell)),
        trace_memory=not bool(args.no_tracemalloc),
    )
    print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
