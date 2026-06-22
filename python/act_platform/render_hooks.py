# -*- coding: utf-8 -*-
"""Render-hook + overlay registry shared by every SAO UI surface.

A *surface* is any addressable UI region — an Entity (Tk) overlay or a WebView
window.  Plugins gain three powers over a surface, all funneled through this
registry so Entity and WebView behave identically:

* **hooks**   — ``hook(surface, payload) -> payload | None`` runs in a priority
  chain *before* the host renders.  A hook may mutate the payload, return a new
  payload, or fully take over the surface by setting
  ``payload[OVERRIDE_KEY] = <ui_spec>`` (the host then renders that spec instead
  of its native content).
* **overlays** — a declarative ``ui_spec`` drawn in a plugin layer *on top of*
  the native content.
* (UI panels live in the plugin manager surface and are handled separately.)

The registry is deliberately host-agnostic: it owns no Tk/web objects, only
JSON-safe data and callables, so it can be unit-tested headless and reused by
both renderers.
"""

from __future__ import annotations

import threading
import time
from typing import Any, Callable, Dict, List, Mapping, Optional

from .ui_spec import normalize_ui_spec

# Surface where a hook setting ``payload[OVERRIDE_KEY]`` replaces native render.
OVERRIDE_KEY = "__plugin_override__"

# Wildcard surface — hooks/overlays registered here apply to every surface.
ALL_SURFACES = "*"

HookCallback = Callable[[str, dict], Optional[dict]]


def _surface_id(value: Any) -> str:
    text = str(value or "").strip()
    return text or ALL_SURFACES


class _Hook:
    __slots__ = ("token", "plugin_id", "surface", "callback", "priority",
                 "calls", "failures", "last_error")

    def __init__(self, token: str, plugin_id: str, surface: str,
                 callback: HookCallback, priority: float) -> None:
        self.token = token
        self.plugin_id = plugin_id
        self.surface = surface
        self.callback = callback
        self.priority = priority
        self.calls = 0
        self.failures = 0
        self.last_error = ""


class RenderHookRegistry:
    """Per-surface render hooks and overlay specs with isolation + cleanup."""

    def __init__(self, *, slow_hook_ms: float = 12.0) -> None:
        self._lock = threading.RLock()
        self._hooks: Dict[str, List[_Hook]] = {}
        self._overlays: Dict[str, Dict[str, dict]] = {}  # surface -> plugin_id -> spec
        self._seq = 0
        self._slow_hook_ms = max(0.0, float(slow_hook_ms or 0.0))

    # ── registration ─────────────────────────────────────────────────────
    def register_hook(self, plugin_id: str, surface: str, callback: HookCallback,
                      priority: float = 0.0) -> str:
        if not callable(callback):
            raise TypeError("render hook callback must be callable")
        with self._lock:
            self._seq += 1
            token = f"hook_{self._seq}"
            hook = _Hook(token, str(plugin_id or ""), _surface_id(surface), callback, float(priority or 0.0))
            self._hooks.setdefault(hook.surface, []).append(hook)
            # Highest priority first; stable within equal priority (insertion order).
            self._hooks[hook.surface].sort(key=lambda h: -h.priority)
            return token

    def unregister_hook(self, token: str) -> bool:
        token = str(token or "")
        with self._lock:
            for surface, hooks in list(self._hooks.items()):
                kept = [h for h in hooks if h.token != token]
                if len(kept) != len(hooks):
                    if kept:
                        self._hooks[surface] = kept
                    else:
                        self._hooks.pop(surface, None)
                    return True
        return False

    def set_overlay(self, plugin_id: str, surface: str, spec: Any) -> dict:
        normalized = normalize_ui_spec(spec)
        with self._lock:
            self._overlays.setdefault(_surface_id(surface), {})[str(plugin_id or "")] = normalized
        return normalized

    def clear_overlay(self, plugin_id: str, surface: Optional[str] = None) -> None:
        plugin_id = str(plugin_id or "")
        with self._lock:
            if surface is None:
                for bucket in self._overlays.values():
                    bucket.pop(plugin_id, None)
            else:
                bucket = self._overlays.get(_surface_id(surface))
                if bucket:
                    bucket.pop(plugin_id, None)

    def unregister_plugin(self, plugin_id: str) -> None:
        plugin_id = str(plugin_id or "")
        with self._lock:
            for surface, hooks in list(self._hooks.items()):
                kept = [h for h in hooks if h.plugin_id != plugin_id]
                if kept:
                    self._hooks[surface] = kept
                else:
                    self._hooks.pop(surface, None)
            for surface, bucket in list(self._overlays.items()):
                bucket.pop(plugin_id, None)
                if not bucket:
                    self._overlays.pop(surface, None)

    # ── execution ────────────────────────────────────────────────────────
    def _chain_for(self, surface: str) -> List[_Hook]:
        surface = _surface_id(surface)
        with self._lock:
            specific = self._hooks.get(surface, ())
            wild = self._hooks.get(ALL_SURFACES, ()) if surface != ALL_SURFACES else ()
            chain = list(specific) + list(wild)
        chain.sort(key=lambda h: -h.priority)
        return chain

    def apply(self, surface: str, payload: Any,
              on_error: Optional[Callable[[str, BaseException], None]] = None) -> dict:
        """Run the hook chain for ``surface`` and return the final payload dict.

        Hook errors are isolated: a failing hook is skipped (and reported via
        ``on_error``) without aborting the chain or the host render.
        """
        data: dict = dict(payload) if isinstance(payload, Mapping) else {"value": payload}
        for hook in self._chain_for(surface):
            start = time.perf_counter()
            try:
                result = hook.callback(surface, data)
                hook.calls += 1
                if isinstance(result, Mapping):
                    data = dict(result)
                elif result is not None:
                    # Contract: hooks return a dict or None. A non-dict return is
                    # a plugin bug — keep the prior payload instead of corrupting
                    # the surface with {"value": ...}, and record the violation.
                    hook.last_error = f"hook returned {type(result).__name__}, expected dict|None"
            except Exception as exc:  # noqa: BLE001 - isolation is the point
                hook.failures += 1
                hook.last_error = str(exc)
                if on_error is not None:
                    try:
                        on_error(hook.plugin_id, exc)
                    except Exception:
                        pass
            finally:
                if self._slow_hook_ms:
                    elapsed = (time.perf_counter() - start) * 1000.0
                    if elapsed > self._slow_hook_ms:
                        hook.last_error = f"slow hook {elapsed:.1f}ms"
        return data

    def overlays(self, surface: str) -> List[dict]:
        surface = _surface_id(surface)
        out: List[dict] = []
        with self._lock:
            buckets = [
                (src, dict(self._overlays.get(src) or {}))
                for src in (surface, ALL_SURFACES)
                if not (src == ALL_SURFACES and surface == ALL_SURFACES)
            ]
        for _src, bucket in buckets:
            for plugin_id, spec in sorted(bucket.items()):
                out.append({"plugin_id": plugin_id, "surface": surface, "spec": spec})
        return out

    def has_overlays(self, surface: str) -> bool:
        surface = _surface_id(surface)
        with self._lock:
            return bool(self._overlays.get(surface) or self._overlays.get(ALL_SURFACES))

    def has_hooks(self, surface: str) -> bool:
        surface = _surface_id(surface)
        with self._lock:
            return bool(self._hooks.get(surface) or self._hooks.get(ALL_SURFACES))

    # ── introspection ────────────────────────────────────────────────────
    def surfaces(self) -> Dict[str, Any]:
        with self._lock:
            ids = set(self._hooks) | set(self._overlays)
            hooks_by_surface = {surface: list(hooks) for surface, hooks in self._hooks.items()}
            overlays_by_surface = {surface: dict(bucket) for surface, bucket in self._overlays.items()}
        out: Dict[str, Any] = {}
        for surface in sorted(ids):
            hooks = hooks_by_surface.get(surface, ())
            out[surface] = {
                "hook_count": len(hooks),
                "hook_plugins": sorted({h.plugin_id for h in hooks}),
                "overlay_plugins": sorted((overlays_by_surface.get(surface) or {}).keys()),
            }
        return out

    def status(self) -> Dict[str, Any]:
        with self._lock:
            hooks_by_surface = {surface: list(hooks) for surface, hooks in self._hooks.items()}
            overlays_by_surface = {surface: dict(bucket) for surface, bucket in self._overlays.items()}
        total_hooks = sum(len(v) for v in hooks_by_surface.values())
        total_overlays = sum(len(v) for v in overlays_by_surface.values())
        failures = sum(h.failures for hooks in hooks_by_surface.values() for h in hooks)
        return {
            "hook_count": total_hooks,
            "overlay_count": total_overlays,
            "hook_failures": failures,
            "surfaces": self.surfaces(),
        }


__all__ = [
    "RenderHookRegistry",
    "OVERRIDE_KEY",
    "ALL_SURFACES",
]
