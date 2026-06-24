# -*- coding: utf-8 -*-
"""Lua scripting runtime via *lupa* (LuaJIT / Lua 5.x binding).

Plugin entry is a ``.lua`` file that defines lifecycle functions::

    -- plugin.lua
    function on_load(ctx)
        ctx:log("Hello from Lua!")
        ctx:register_ui_panel("my_panel", {title = "Lua Panel"},
            function(payload)
                return ctx.ui.panel("Lua Demo", {
                    ctx.ui.text("Hello from Lua!"),
                    ctx.ui.button("Click", "click_action"),
                })
            end,
            function(action_id, payload)
                ctx:log("action: " .. tostring(action_id))
                return {ok = true}
            end
        )
    end

    function on_unload()
        -- cleanup
    end

Lua tables map to Python dicts via lupa's automatic conversion.
Python objects (``ctx``, ``ctx.ui``) are accessible from Lua with
colon-syntax method calls (``ctx:method(...)``).

Requires: ``pip install lupa``
"""

from __future__ import annotations

import os
import sys
from types import ModuleType
from typing import Any, Callable, TYPE_CHECKING

from .base import ContextProxy, ScriptModule, ScriptRuntime, wrap_callback, resolve_local_script_path

if TYPE_CHECKING:
    from act_platform.plugins import PluginContext, PluginRecord

_lupa = None
_LuaRuntime_cls = None


def _ensure_lupa():
    global _lupa, _LuaRuntime_cls
    if _lupa is not None:
        return
    try:
        import lupa as _lupa_mod
        _lupa = _lupa_mod
        _LuaRuntime_cls = _lupa.LuaRuntime
    except ImportError:
        raise RuntimeError(
            "Lua runtime requires the 'lupa' package. "
            "Install with: pip install lupa"
        )


class _LuaProxy:
    """Wraps PluginContext for Lua consumption.

    lupa makes Python objects callable from Lua by default, but
    we add convenience helpers for table↔dict conversion and
    ensure the ``ui`` builder works from Lua (it returns Python
    dicts which are already Lua-table compatible via lupa).
    """

    def __init__(self, ctx: "PluginContext", lua_runtime) -> None:
        self._ctx = ctx
        self._lua = lua_runtime
        self.ui = ctx.ui
        self.engine = ctx.engine
        self.plugin_id = ctx.plugin_id
        self.path = ctx.path
        self.web_path = ctx.web_path
        self.assets_path = ctx.assets_path

    def _to_python(self, val: Any) -> Any:
        if _lupa is not None and _lupa.lua_type(val) == "table":
            return _table_to_python(val)
        return val

    def _wrap_lua_callback(self, fn: Any) -> Callable:
        if not callable(fn):
            return fn
        lua = self._lua

        def _py_cb(*args, **kwargs):
            result = fn(*args, **kwargs)
            return self._to_python(result)
        return _py_cb

    def log(self, message: Any) -> None:
        self._ctx.log(str(message or ""))

    def subscribe(self, topic: str, callback) -> str:
        return self._ctx.subscribe(str(topic), self._wrap_lua_callback(callback))

    def subscribe_once(self, topic: str, callback) -> str:
        return self._ctx.subscribe_once(str(topic), self._wrap_lua_callback(callback))

    def unsubscribe(self, token: str) -> bool:
        return self._ctx.unsubscribe(str(token))

    def on_damage(self, callback) -> str:
        return self._ctx.on_damage(self._wrap_lua_callback(callback))

    def on_heal(self, callback) -> str:
        return self._ctx.on_heal(self._wrap_lua_callback(callback))

    def on_skill(self, callback) -> str:
        return self._ctx.on_skill(self._wrap_lua_callback(callback))

    def on_boss(self, callback) -> str:
        return self._ctx.on_boss(self._wrap_lua_callback(callback))

    def on_snapshot(self, callback) -> str:
        return self._ctx.on_snapshot(self._wrap_lua_callback(callback))

    def on_encounter_finalized(self, callback) -> str:
        return self._ctx.on_encounter_finalized(self._wrap_lua_callback(callback))

    def emit(self, topic: str, payload=None) -> dict:
        p = self._to_python(payload) if payload is not None else None
        return self._ctx.emit(str(topic), p)

    def get_snapshot(self) -> dict:
        return self._ctx.get_snapshot()

    def snapshot_value(self, path: str, default=None):
        return self._ctx.snapshot_value(str(path), default)

    def time(self) -> float:
        return self._ctx.time()

    def recent_events(self, limit=20, topic="") -> list:
        return self._ctx.recent_events(int(limit), str(topic or ""))

    def get_setting(self, key: str, default=None):
        return self._ctx.get_setting(str(key), default)

    def setting(self, key: str, default=None):
        return self._ctx.setting(str(key), default)

    def set_setting(self, key: str, value=None) -> None:
        self._ctx.set_setting(str(key), self._to_python(value))

    def set_defaults(self, defaults) -> None:
        d = self._to_python(defaults)
        if isinstance(d, dict):
            self._ctx.set_defaults(d)

    def register_ui_panel(self, panel_id: str, metadata=None,
                          render=None, on_action=None) -> dict:
        meta = self._to_python(metadata) if metadata is not None else None

        def _render(payload=None):
            if callable(render):
                result = render(payload)
                return self._to_python(result)
            return {"version": 1, "title": "", "nodes": []}

        def _on_action(action_id, payload=None):
            if callable(on_action):
                result = on_action(str(action_id), payload)
                return self._to_python(result)
            return {"ok": True}

        return self._ctx.register_ui_panel(
            str(panel_id), meta,
            _render if callable(render) else None,
            _on_action if callable(on_action) else None,
        )

    def register_render_hook(self, surface: str, callback, priority=0.0) -> str:
        return self._ctx.register_render_hook(
            str(surface), self._wrap_lua_callback(callback), float(priority))

    def set_overlay(self, surface: str, spec) -> dict:
        return self._ctx.set_overlay(str(surface), self._to_python(spec))

    def clear_overlay(self, surface=None) -> None:
        self._ctx.clear_overlay(str(surface) if surface else None)

    def register_hotkey(self, hotkey_id: str, callback,
                        default_key="", label="") -> str:
        return self._ctx.register_hotkey(
            str(hotkey_id), self._wrap_lua_callback(callback),
            str(default_key or ""), str(label or ""))

    def register_engine(self, name: str, engine) -> None:
        self._ctx.register_engine(str(name), engine)

    def register_data_source(self, source_id: str, metadata=None,
                             start=None, stop=None) -> dict:
        meta = self._to_python(metadata) if metadata is not None else None
        return self._ctx.register_data_source(
            str(source_id), meta,
            self._wrap_lua_callback(start) if callable(start) else None,
            self._wrap_lua_callback(stop) if callable(stop) else None,
        )

    def register_parser_adapter(self, adapter_id, metadata=None, handler=None):
        meta = self._to_python(metadata) if metadata is not None else None
        h = self._wrap_lua_callback(handler) if callable(handler) else None
        return self._ctx.register_parser_adapter(str(adapter_id), meta, h)

    def register_exporter(self, exporter_id, metadata=None, handler=None):
        meta = self._to_python(metadata) if metadata is not None else None
        h = self._wrap_lua_callback(handler) if callable(handler) else None
        return self._ctx.register_exporter(str(exporter_id), meta, h)

    def register_formatter(self, formatter_id, metadata=None, handler=None):
        meta = self._to_python(metadata) if metadata is not None else None
        h = self._wrap_lua_callback(handler) if callable(handler) else None
        return self._ctx.register_formatter(str(formatter_id), meta, h)

    def register_trigger_type(self, trigger_type, metadata=None, handler=None):
        meta = self._to_python(metadata) if metadata is not None else None
        h = self._wrap_lua_callback(handler) if callable(handler) else None
        return self._ctx.register_trigger_type(str(trigger_type), meta, h)

    def register_report_view(self, view_id, metadata=None, handler=None):
        meta = self._to_python(metadata) if metadata is not None else None
        h = self._wrap_lua_callback(handler) if callable(handler) else None
        return self._ctx.register_report_view(str(view_id), meta, h)

    def register_timer(self, timer_id, metadata=None, handler=None):
        meta = self._to_python(metadata) if metadata is not None else None
        h = self._wrap_lua_callback(handler) if callable(handler) else None
        return self._ctx.register_timer(str(timer_id), meta, h)

    def register_menu_category(self, name, icon, builder, priority=0.0):
        return self._ctx.register_menu_category(
            str(name), str(icon or ""),
            self._wrap_lua_callback(builder), float(priority))

    def register_menu_surface(self, surface_id, descriptor, priority=0.0):
        d = self._to_python(descriptor) if descriptor is not None else {}
        return self._ctx.register_menu_surface(str(surface_id), d, float(priority))

    def register_action_handler(self, handler):
        return self._ctx.register_action_handler(self._wrap_lua_callback(handler))

    def request_redraw(self, surface="", reason="") -> dict:
        return self._ctx.request_redraw(str(surface or ""), str(reason or ""))

    def open_window(self, panel_id="", width=0, height=0) -> dict:
        return self._ctx.open_window(str(panel_id or ""), int(width), int(height))

    def set_interval(self, callback, seconds) -> str:
        return self._ctx.set_interval(self._wrap_lua_callback(callback), float(seconds))

    def set_timeout(self, callback, seconds) -> str:
        return self._ctx.set_timeout(self._wrap_lua_callback(callback), float(seconds))

    def clear_timer(self, token) -> bool:
        return self._ctx.clear_timer(str(token))

    def run_on_ui(self, callback) -> None:
        self._ctx.run_on_ui(self._wrap_lua_callback(callback))

    def notify(self, title, message, duration_s=60.0, kind="plugin") -> bool:
        return self._ctx.notify(str(title), str(message), float(duration_s), str(kind))

    def dismiss_notify(self) -> bool:
        return self._ctx.dismiss_notify()

    def toast(self, message) -> bool:
        return self._ctx.toast(str(message))

    def get_engine(self, name, default=None):
        return self._ctx.get_engine(str(name), default)

    def require_engine(self, name):
        return self._ctx.require_engine(str(name))

    def call_engine(self, engine_name, method, *args, **kwargs):
        return self._ctx.call_engine(str(engine_name), str(method), *args, **kwargs)

    def call_runtime(self, action, *args, **kwargs):
        return self._ctx.call_runtime(str(action), *args, **kwargs)

    def ensure_requirements(self, install=True) -> dict:
        return self._ctx.ensure_requirements(bool(install))

    def load_local(self, relative_path):
        return self._ctx.load_local(str(relative_path))

    @property
    def mem(self):
        return self._ctx.mem

    @property
    def event_bus(self):
        return self._ctx.event_bus

    @property
    def owner(self):
        return self._ctx.owner


def _table_to_python(table) -> Any:
    """Recursively convert a Lua table to Python dict/list."""
    if _lupa is None or _lupa.lua_type(table) != "table":
        return table
    keys = list(table.keys())
    if keys and all(isinstance(k, (int, float)) for k in keys):
        sorted_keys = sorted(keys)
        if sorted_keys == list(range(1, len(sorted_keys) + 1)):
            return [_table_to_python(table[k]) for k in sorted_keys]
    out = {}
    for k in keys:
        v = table[k]
        pk = str(k)
        if _lupa.lua_type(v) == "table":
            out[pk] = _table_to_python(v)
        else:
            out[pk] = v
    return out


class LuaRuntime(ScriptRuntime):
    """Load ``.lua`` plugin scripts via lupa."""

    def __init__(self) -> None:
        _ensure_lupa()
        self._engines: dict[str, Any] = {}

    @property
    def language(self) -> str:
        return "lua"

    @property
    def engine_name(self) -> str:
        return f"lupa ({getattr(_lupa, 'LUA_VERSION', '?')})"

    def load_script(self, entry_path: str, record: "PluginRecord",
                    ctx: "PluginContext") -> ModuleType:
        lua = _LuaRuntime_cls(
            unpack_returned_tuples=True,
            encoding="utf-8",
            source_encoding="utf-8",
        )
        self._engines[record.plugin_id] = lua

        with open(entry_path, "r", encoding="utf-8") as fp:
            source = fp.read()

        proxy = _LuaProxy(ctx, lua)
        lua.globals()["python"] = __builtins__ if isinstance(__builtins__, dict) else vars(__builtins__)
        lua.globals()["require_python"] = __import__

        plugin_base = os.path.abspath(str(record.path))

        def _lua_load_script(relative_path, *, _lua=lua, _base=plugin_base):
            target = resolve_local_script_path(_base, relative_path)
            with open(target, "r", encoding="utf-8") as fp:
                more_source = fp.read()
            _lua.execute(more_source)
            return True

        lua.globals()["load_script"] = _lua_load_script
        lua.globals()["dofile"] = _lua_load_script
        lua.globals()["import"] = _lua_load_script
        proxy.load_script = _lua_load_script

        lua.execute(source)
        g = lua.globals()

        module = ScriptModule(
            f"act_plugin_lua_{record.plugin_id}",
            "lua", entry_path,
        )

        for hook_name in ("on_load", "on_enable", "on_disable", "on_unload"):
            lua_fn = g[hook_name] if hook_name in g else None
            if lua_fn is not None and callable(lua_fn):
                if hook_name == "on_load":
                    def _on_load(_ctx_ignored=None, *, _p=proxy, _fn=lua_fn):
                        _fn(_p)
                    module.set_hook("on_load", _on_load)
                else:
                    module.set_hook(hook_name, wrap_callback(lua_fn, "lua", ctx.log))

        return module

    def unload_script(self, record: "PluginRecord") -> None:
        self._engines.pop(record.plugin_id, None)
