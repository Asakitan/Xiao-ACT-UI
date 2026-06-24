# -*- coding: utf-8 -*-
"""Abstract base for multi-language script runtimes and the bridge proxy."""

from __future__ import annotations

import os
import abc
import json
import traceback
from types import ModuleType, SimpleNamespace
from typing import Any, Callable, Mapping, Optional, TYPE_CHECKING

if TYPE_CHECKING:
    from act_platform.plugins import PluginContext, PluginRecord


def resolve_local_script_path(plugin_base: str, relative_path: str) -> str:
    """Resolve a plugin-relative script path, enforcing sandbox containment.

    Used by Emma/Lua/AngelScript runtimes to let a plugin ``import``/``dofile``
    sibling script files (``.emma``/``.lua``/``.as``) from its own directory.
    The resolved path must stay inside ``plugin_base``; otherwise a ValueError
    is raised so plugins cannot escape their directory via ``..`` traversal.
    """
    base = os.path.abspath(plugin_base)
    target = os.path.abspath(os.path.join(base, str(relative_path or "")))
    if os.path.commonpath([base, target]) != base:
        raise ValueError(f"load_script path must stay inside the plugin directory: {relative_path!r}")
    if not os.path.isfile(target):
        raise FileNotFoundError(f"load_script target not found: {relative_path!r}")
    return target


class ScriptRuntime(abc.ABC):
    """Language-specific runtime that loads foreign scripts as plugin modules."""

    @property
    @abc.abstractmethod
    def language(self) -> str: ...

    @property
    def engine_name(self) -> str:
        return self.language

    @abc.abstractmethod
    def load_script(self, entry_path: str, record: "PluginRecord",
                    ctx: "PluginContext") -> ModuleType:
        """Load *entry_path* and return a Python ``ModuleType``-like object.

        The returned module must expose any lifecycle hooks the script defines
        (``on_load``, ``on_enable``, ``on_disable``, ``on_unload``) as
        regular callable attributes so ``PluginManager._call_hook`` works
        unchanged.
        """

    def unload_script(self, record: "PluginRecord") -> None:
        """Optional cleanup when the plugin unloads."""


class ScriptModule(ModuleType):
    """Thin Python module wrapper around a foreign-language plugin.

    ``PluginManager._call_hook`` reads ``getattr(module, hook_name)``
    and calls it. This class stores callables discovered from the
    foreign runtime and exposes them as attributes.
    """

    def __init__(self, name: str, language: str, entry_path: str) -> None:
        super().__init__(name)
        self.__file__ = entry_path
        self._language = language
        self._hooks: dict[str, Callable[..., Any]] = {}

    def set_hook(self, name: str, fn: Callable[..., Any]) -> None:
        self._hooks[name] = fn

    def __getattr__(self, name: str) -> Any:
        hooks = object.__getattribute__(self, "_hooks")
        if name in hooks:
            return hooks[name]
        raise AttributeError(f"script module has no attribute {name!r}")


class ContextProxy:
    """Flat proxy of :class:`PluginContext` for foreign-language consumption.

    Lua/AngelScript/Emma scripts see this object as ``ctx`` and can call
    any method directly. Type marshalling between the host language and
    Python is handled by the specific runtime; this proxy guarantees
    that only JSON-safe data crosses the boundary when needed.
    """

    _PASSTHROUGH_METHODS = (
        "log", "subscribe", "subscribe_once", "unsubscribe",
        "on_damage", "on_heal", "on_skill", "on_boss",
        "on_snapshot", "on_encounter_finalized",
        "emit", "get_snapshot", "snapshot_value", "time", "recent_events",
        "get_setting", "setting", "set_setting", "set_defaults",
        "register_parser_adapter", "register_exporter",
        "register_formatter", "register_trigger_type",
        "register_report_view", "register_timer",
        "register_ui_panel", "register_render_hook",
        "set_overlay", "clear_overlay",
        "register_hotkey", "register_menu_category",
        "register_menu_surface", "register_action_handler",
        "register_engine", "register_data_source",
        "request_redraw", "open_window",
        "open_file",
        "set_interval", "set_timeout", "clear_timer", "run_on_ui",
        "notify", "dismiss_notify", "toast",
        "ensure_requirements", "load_local",
        "get_engine", "require_engine", "call_engine", "call_runtime",
    )

    _PASSTHROUGH_PROPS = (
        "plugin_id", "path", "web_path", "assets_path",
        "event_bus", "owner", "mem",
    )

    def __init__(self, ctx: "PluginContext") -> None:
        object.__setattr__(self, "_ctx", ctx)

    @property
    def ui(self):
        return object.__getattribute__(self, "_ctx").ui

    @property
    def engine(self):
        return object.__getattribute__(self, "_ctx").engine

    def __getattr__(self, name: str) -> Any:
        ctx = object.__getattribute__(self, "_ctx")
        if name in self._PASSTHROUGH_PROPS:
            return getattr(ctx, name)
        if name in self._PASSTHROUGH_METHODS:
            return getattr(ctx, name)
        raise AttributeError(f"PluginContext has no attribute {name!r}")

    def __repr__(self) -> str:
        ctx = object.__getattribute__(self, "_ctx")
        return f"<ContextProxy plugin_id={ctx.plugin_id!r}>"


def wrap_callback(fn: Any, language: str, log_fn: Optional[Callable] = None) -> Callable:
    """Wrap a foreign-language callable so exceptions are caught and logged."""

    def _wrapper(*args: Any, **kwargs: Any) -> Any:
        try:
            return fn(*args, **kwargs)
        except Exception as exc:
            msg = f"[{language}] callback error: {exc}"
            if callable(log_fn):
                log_fn(msg)
            else:
                traceback.print_exc()
            return None

    _wrapper.__name__ = getattr(fn, "__name__", f"<{language}_callback>")
    _wrapper.__qualname__ = _wrapper.__name__
    return _wrapper


def to_json_safe(value: Any) -> Any:
    """Recursively coerce a value to JSON-safe Python types."""
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    if isinstance(value, Mapping):
        return {str(k): to_json_safe(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [to_json_safe(v) for v in value]
    try:
        return json.loads(json.dumps(value, default=str))
    except (TypeError, ValueError):
        return str(value)
