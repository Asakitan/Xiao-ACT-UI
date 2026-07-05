# -*- coding: utf-8 -*-
# overlay_adapter — drop-in GpuOverlayWindow replacement backed by
# the unified compositor.
#
# CompositorOverlayWindow has the same public API as GpuOverlayWindow
# so existing code can switch with minimal changes:
#
# # OLD:
# pump = get_glfw_pump(root)
# win = GpuOverlayWindow(pump, w=300, h=400, ...)
#
# # NEW:
# overlay = get_unified_overlay(root)
# win = CompositorOverlayWindow(overlay, w=300, h=400, ...)
#
# Both expose: show(), hide(), destroy(), set_geometry(),
# set_render_fn(), request_redraw(), set_click_through(),
# set_input_callbacks(), hwnd, ctx.
from __future__ import annotations

import threading
from typing import Any, Callable, Optional

from render.overlay_compositor import (
    CompositorLayer,
    UnifiedOverlay,
)

from render.overlay_compositor import get_unified_overlay


_next_layer_id = 0
_id_lock = threading.Lock()


def _gen_layer_name(title: str) -> str:
    global _next_layer_id
    with _id_lock:
        _next_layer_id += 1
        n = _next_layer_id
    return f'{title}_{n}' if title else f'layer_{n}'


class CompositorOverlayWindow:
    # GpuOverlayWindow-compatible wrapper around a CompositorLayer.
    #
    # Adapts the GpuOverlayWindow API surface so callers (popup menu,
    # fisheye, panels, etc.) can switch to the unified overlay with
    # minimal code changes.

    def __init__(self, compositor: UnifiedOverlay,
                 w: int = 100, h: int = 100,
                 x: int = 0, y: int = 0,
                 render_fn: Optional[Callable[[Any, float], None]] = None,
                 click_through: bool = True,
                 title: str = 'sao_overlay',
                 vsync: bool = False,
                 z: int = 100):
        self._compositor = compositor
        self._title = title
        self._name = _gen_layer_name(title)
        self._layer = compositor.create_layer(
            self._name,
            width=w, height=h, x=x, y=y, z=z,
            click_through=click_through,
        )
        self._w = w
        self._h = h
        self._x = x
        self._y = y
        self._click_through = click_through
        self._render_fn = render_fn
        self._visible = False
        self._created = True  # always "created" (no GLFW lifecycle)
        self._destroyed = False

        if render_fn is not None:
            self._layer.set_render_fn(render_fn)
        if vsync:
            try:
                from render.overlay_compositor import _detect_refresh_hz
                self._layer.target_fps = _detect_refresh_hz()
            except Exception:
                self._layer.target_fps = 60

    # ── Properties matching GpuOverlayWindow ─────────────────

    @property
    def ctx(self) -> Any:
        host = self._compositor.host
        return host.ctx if host else None

    @property
    def hwnd(self) -> int:
        return self._compositor.hwnd

    @property
    def layer(self) -> CompositorLayer:
        return self._layer

    @property
    def visible(self) -> bool:
        return self._visible

    # ── Lifecycle ────────────────────────────────────────────

    def show(self, async_create: bool = False) -> None:
        was_visible = self._visible
        self._visible = True
        self._layer.show()
        self._layer.sync_input_proxy()
        if not was_visible:
            try:
                self._compositor.sync_host_input_mode()
            except Exception:
                pass
        try:
            self._compositor.lift_all_input_proxies()
        except Exception:
            pass

    def hide(self) -> None:
        self._visible = False
        self._layer.hide()
        self._layer.sync_input_proxy()
        try:
            self._compositor.sync_host_input_mode()
        except Exception:
            pass

    def destroy(self) -> None:
        if self._destroyed:
            return
        self._destroyed = True
        self._visible = False
        # destroy_input_proxy() must not be able to block destroy_layer()
        # below — self._destroyed is already True, so a caller that sees
        # an exception here and never retries would otherwise leak this
        # layer (and its GPU FBO/texture) forever.
        try:
            self._layer.destroy_input_proxy()
        except Exception:
            pass
        self._compositor.destroy_layer(self._name)
        try:
            self._compositor.sync_host_input_mode()
        except Exception:
            pass

    def prepare_async(self) -> bool:
        return True  # always ready

    # ── Geometry ─────────────────────────────────────────────

    def set_geometry(self, x: int, y: int, w: int, h: int) -> None:
        self._x = x
        self._y = y
        self._w = w
        self._h = h
        self._layer.set_geometry(x, y, w, h)
        self._layer.sync_input_proxy()

    def move(self, x: int, y: int) -> None:
        self._x = x
        self._y = y
        self._layer.set_position(x, y)

    # ── Rendering ────────────────────────────────────────────

    def set_render_fn(
        self, fn: Optional[Callable[[Any, float], None]],
    ) -> None:
        self._render_fn = fn
        self._layer.set_render_fn(fn)

    def request_redraw(self) -> None:
        self._layer.request_redraw()

    # ── Input ────────────────────────────────────────────────

    def set_click_through(self, ct: bool) -> None:
        self._click_through = ct
        self._layer.click_through = ct
        self._layer.sync_input_proxy()
        try:
            self._compositor.sync_host_input_mode()
        except Exception:
            pass

    def set_input_callbacks(
        self,
        cursor_pos_fn: Optional[Callable] = None,
        cursor_leave_fn: Optional[Callable] = None,
        mouse_button_fn: Optional[Callable] = None,
        scroll_fn: Optional[Callable] = None,
    ) -> None:
        self._layer.set_input_callbacks(
            cursor_pos_fn, cursor_leave_fn,
            mouse_button_fn, scroll_fn,
        )
        try:
            self._compositor.sync_host_input_mode()
        except Exception:
            pass

    def enable_input_proxy(self) -> bool:
        # Route this (non-click-through) layer's input through a pinned
        # Tk proxy so the host stays click-through everywhere.
        #
        # For a persistent interactive layer such as the NerveGear trigger
        # button: without this it forces the whole host out of
        # WS_EX_TRANSPARENT, and every other layer's SetWindowRgn spans
        # start gating input — a click_through desktop pet then grows a
        # flickering click/cursor dead-zone halo (its anti-tear pad) and
        # cross-process clicks over it die. See
        # UnifiedOverlay.attach_layer_input_proxy. Returns False if there's
        # no Tk root (legacy host-HWND routing kept).
        try:
            return self._compositor.attach_layer_input_proxy(self._name)
        except Exception:
            return False

    # ── Fade / Alpha ─────────────────────────────────────────

    def set_alpha(self, alpha: float) -> None:
        self._layer.alpha = alpha
        self._layer.request_redraw()

    def start_fade(self, target: float, duration: float = 0.3,
                   done_fn: Optional[Callable] = None) -> None:
        self._layer.start_fade(target, duration, done_fn)

    # ── Z-order ──────────────────────────────────────────────

    def raise_to_top(self) -> None:
        self._compositor.raise_layer(self._name)

    def set_z(self, z: int) -> None:
        self._compositor.set_layer_z(self._name, z)


class CompositorBgraPresenter:
    # BgraPresenter-compatible wrapper that feeds a CompositorLayer.
    #
    # Drop-in replacement for the existing BgraPresenter. Instead of
    # owning a GL texture and shader program, this simply forwards
    # BGRA bytes to the underlying layer.

    def __init__(self, layer: CompositorLayer):
        self._layer = layer
        self._frame_bytes: Optional[bytes] = None
        self._frame_w = 0
        self._frame_h = 0

    def set_frame(self, bgra: bytes, w: int, h: int,
                  x: int = 0, y: int = 0) -> None:
        # Stage a new frame (same API as BgraPresenter).
        self._frame_bytes = bgra
        self._frame_w = w
        self._frame_h = h
        self._layer.upload_bgra(bgra, w, h)
        if x or y:
            self._layer.set_position(x, y)

    def start_fade(self, target: float, duration: float = 0.3,
                   done_fn: Optional[Callable] = None) -> None:
        self._layer.start_fade(target, duration, done_fn)

    def render(self, ctx: Any, t: float) -> None:
        # No-op. In unified overlay, master compositor handles drawing.
        pass

    @property
    def alpha(self) -> float:
        return self._layer.alpha

    @alpha.setter
    def alpha(self, val: float) -> None:
        self._layer.alpha = val
        self._layer.request_redraw()


# ── Factory ──────────────────────────────────────────────────────
_USE_UNIFIED = True


def create_overlay_window(
    root: Any = None,
    w: int = 100, h: int = 100,
    x: int = 0, y: int = 0,
    render_fn: Optional[Callable] = None,
    click_through: bool = True,
    title: str = 'sao_overlay',
    vsync: bool = False,
    z: int = 100,
) -> Any:
    # Create an overlay window — unified compositor or legacy GLFW.
    #
    # Returns CompositorOverlayWindow (unified) or GpuOverlayWindow
    # (legacy) depending on _USE_UNIFIED toggle.
    if _USE_UNIFIED:
        overlay = get_unified_overlay(root)
        if not overlay._running:
            overlay.start()
        return CompositorOverlayWindow(
            overlay, w=w, h=h, x=x, y=y,
            render_fn=render_fn, click_through=click_through,
            title=title, vsync=vsync, z=z,
        )
    # Legacy path
    from render.gpu_overlay_window import GpuOverlayWindow, get_glfw_pump
    pump = get_glfw_pump(root)
    return GpuOverlayWindow(
        pump, w=w, h=h, x=x, y=y,
        render_fn=render_fn, click_through=click_through,
        title=title, vsync=vsync,
    )
