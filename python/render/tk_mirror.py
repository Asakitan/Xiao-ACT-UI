# -*- coding: utf-8 -*-
"""tk_mirror — capture Tk Toplevel windows and present via compositor.

Mirrors a Tk Toplevel through the unified overlay compositor:
  1. Hides the real Tk window off-screen (-10000, -10000)
  2. Captures its rendered content via PrintWindow → BGRA at target FPS
  3. Uploads the BGRA to a compositor layer
  4. Forwards mouse events from the compositor layer to the hidden Tk window
  5. Intercepts geometry() calls so drag operations move the compositor layer

API matches WebViewProxy pattern from webview_proxy.py.
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import threading
import time
from typing import Any, Callable, Dict, Optional, Tuple

import numpy as np

from render.webview_proxy import (
    _capture_window, _make_lparam, _user32,
    WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP,
    WM_RBUTTONDOWN, WM_RBUTTONUP, WM_MOUSEWHEEL,
    MK_LBUTTON, MK_RBUTTON,
)

WS_EX_NOACTIVATE = 0x08000000
GWL_EXSTYLE = -20

#: Default corner radius (px) applied to mirrored Tk panels — matches the
#: DWM window-rounding look used elsewhere (rounded_panel()/CreateRoundRectRgn
#: precedent in this codebase uses 10-14px for whole-panel rounding).
DEFAULT_MIRROR_CORNER_RADIUS = 12

_corner_mask_cache: Dict[Tuple[int, int, int], np.ndarray] = {}


def _corner_cut_mask(w: int, h: int, radius: int) -> Optional[np.ndarray]:
    """Boolean (h, w) mask of pixels OUTSIDE the rounded-rect corners.

    True where the pixel should be cut (alpha zeroed). Only the four
    ``radius``x``radius`` corner blocks are non-trivial; cached per
    (w, h, radius) since a captured panel keeps the same size across most
    frames and only changes on resize.
    """
    if radius <= 0 or w <= 0 or h <= 0:
        return None
    r = min(radius, w // 2, h // 2)
    if r <= 0:
        return None
    key = (w, h, r)
    cached = _corner_mask_cache.get(key)
    if cached is not None:
        return cached
    yy, xx = np.mgrid[0:r, 0:r]
    dist = np.sqrt((xx - r + 0.5) ** 2 + (yy - r + 0.5) ** 2)
    corner_cut = dist > r  # True = outside the circle → cut
    mask = np.zeros((h, w), dtype=bool)
    mask[:r, :r] = corner_cut
    mask[:r, w - r:] = corner_cut[:, ::-1]
    mask[h - r:, :r] = corner_cut[::-1, :]
    mask[h - r:, w - r:] = corner_cut[::-1, ::-1]
    if len(_corner_mask_cache) > 32:
        _corner_mask_cache.clear()
    _corner_mask_cache[key] = mask
    return mask


def _apply_corner_mask(bgra: bytes, w: int, h: int, radius: int) -> bytes:
    """Zero out (premultiplied) BGRA pixels outside the rounded-rect corners."""
    mask = _corner_cut_mask(w, h, radius)
    if mask is None:
        return bgra
    expected = w * h * 4
    arr = np.frombuffer(bgra, dtype=np.uint8)
    if arr.size != expected:
        return bgra
    arr = arr.reshape(h, w, 4).copy()
    arr[mask] = 0
    return arr.tobytes()


class TkMirrorLayer:
    """Mirrors a Tk Toplevel through the compositor.

    Usage::

        mirror = TkMirrorLayer(tk_toplevel, 'ai_editor', z=500)
        mirror.attach()   # hides Tk window, starts capture
        mirror.show()     # compositor layer visible
        # ...
        mirror.hide()
        mirror.detach()   # stops capture, restores Tk window
    """

    def __init__(self, tk_win, name: str, z: int = 500,
                 capture_fps: float = 30.0,
                 corner_radius: int = DEFAULT_MIRROR_CORNER_RADIUS):
        self._tk_win = tk_win
        self._name = name
        self._z = z
        self._capture_interval = 1.0 / max(1.0, capture_fps)
        self._corner_radius = max(0, int(corner_radius or 0))

        self._hwnd: int = 0
        self._width: int = 0
        self._height: int = 0
        self._screen_x: int = 0
        self._screen_y: int = 0

        self._layer = None
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._stop_evt = threading.Event()
        self._visible = False
        self._attached = False

        self._orig_geometry: Optional[Callable] = None
        self._orig_x: int = 0
        self._orig_y: int = 0
        self._orig_w: int = 0
        self._orig_h: int = 0
        self._press_target: Optional[Any] = None

    def attach(self) -> None:
        if self._attached:
            return
        win = self._tk_win
        try:
            win.update_idletasks()
        except Exception:
            pass

        self._hwnd = int(
            _user32.GetParent(win.winfo_id()) or win.winfo_id())
        self._orig_x = win.winfo_x()
        self._orig_y = win.winfo_y()
        self._width = max(1, win.winfo_width())
        self._height = max(1, win.winfo_height())
        self._screen_x = self._orig_x
        self._screen_y = self._orig_y
        self._orig_w = self._width
        self._orig_h = self._height

        root = _tk_root_for(win)
        if root is None:
            raise RuntimeError('Tk mirror requires a Tk root for input proxy')

        from render.gpu_overlay_window import (
            _get_unified_overlay, get_unified_overlay_mode,
        )
        if not get_unified_overlay_mode():
            raise RuntimeError('unified overlay mode is disabled')
        uo = _get_unified_overlay(root)
        try:
            uo.ensure_tk_poller()
        except Exception:
            pass
        if not uo.wait_ready(timeout=0.15):
            raise RuntimeError('unified overlay is not ready')

        layer_name = f'tk_{self._name}'
        layer = None
        try:
            layer = uo.create_layer(
                layer_name,
                width=self._width,
                height=self._height,
                x=self._screen_x,
                y=self._screen_y,
                z=self._z,
                click_through=False,
            )
            layer.set_input_callbacks(
                cursor_pos_fn=self._on_cursor_pos,
                mouse_button_fn=self._on_mouse_button,
                scroll_fn=self._on_scroll,
            )
            layer.create_input_proxy(root)
            if layer._input_proxy is None:
                raise RuntimeError('failed to create Tk mirror input proxy')
            layer.sync_input_proxy()
        except Exception:
            if layer is not None:
                try:
                    uo.destroy_layer(layer_name)
                except Exception:
                    pass
            raise

        self._layer = layer

        # Save original exstyle + alpha for detach restore after compositor
        # input is ready. If setup fails before this point, the real Tk window
        # stays normal and clickable.
        self._orig_exstyle = _user32.GetWindowLongPtrW(
            self._hwnd, GWL_EXSTYLE)
        try:
            self._orig_alpha = float(win.attributes('-alpha') or 1.0)
        except Exception:
            self._orig_alpha = 1.0

        # Make the Tk window nearly invisible (alpha≈0) and click-through.
        # The window stays at its normal position so PrintWindow captures
        # valid content (moving off-screen causes empty frames).
        _user32.SetWindowLongPtrW(
            self._hwnd, GWL_EXSTYLE,
            self._orig_exstyle | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)
        try:
            win.attributes('-alpha', 0.01)
        except Exception:
            pass

        self._install_geometry_hook()

        self._attached = True
        self._running = True
        self._stop_evt.clear()
        self._thread = threading.Thread(
            target=self._capture_loop, daemon=True,
            name=f'tk-mirror-{self._name}',
        )
        self._thread.start()

    def detach(self) -> None:
        if not self._attached:
            return
        self._running = False
        self._stop_evt.set()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=3.0)
        self._thread = None

        self._uninstall_geometry_hook()

        if self._layer is not None:
            try:
                from render.gpu_overlay_window import _get_unified_overlay
                _get_unified_overlay().destroy_layer(f'tk_{self._name}')
            except Exception:
                pass
            self._layer = None

        # Restore original alpha + exstyle
        try:
            self._tk_win.attributes('-alpha', self._orig_alpha)
        except Exception:
            pass
        try:
            _user32.SetWindowLongPtrW(
                self._hwnd, GWL_EXSTYLE, self._orig_exstyle)
        except Exception:
            pass
        self._attached = False

    def show(self) -> None:
        self._visible = True
        if self._layer:
            self._layer.show()
            self._layer.sync_input_proxy()

    def hide(self) -> None:
        self._visible = False
        if self._layer:
            self._layer.hide()
            self._layer.sync_input_proxy()

    def set_position(self, x: int, y: int) -> None:
        self._screen_x = x
        self._screen_y = y
        if self._layer:
            self._layer.set_position(x, y)
            self._layer.sync_input_proxy()
        if self._hwnd:
            _user32.MoveWindow(
                self._hwnd, x, y,
                self._width, self._height, False)

    def set_geometry(self, x: int, y: int, w: int, h: int) -> None:
        self._screen_x = x
        self._screen_y = y
        changed = (w != self._width or h != self._height)
        self._width = w
        self._height = h
        if self._layer:
            self._layer.set_geometry(x, y, w, h)
            self._layer.sync_input_proxy()
        if self._hwnd:
            _user32.MoveWindow(self._hwnd, x, y, w, h, True)

    # ── Capture loop ─────────────────────────────────────────

    def _capture_loop(self) -> None:
        while self._running:
            layer = self._layer
            if self._visible and layer is not None and self._hwnd:
                bgra = _capture_window(
                    self._hwnd, self._width, self._height)
                if bgra and layer is not None:
                    try:
                        if self._corner_radius > 0:
                            bgra = _apply_corner_mask(
                                bgra, self._width, self._height,
                                self._corner_radius)
                        layer.upload_bgra(
                            bgra, self._width, self._height)
                        layer.request_redraw()
                    except Exception:
                        pass
            self._stop_evt.wait(timeout=self._capture_interval)

    # ── Input forwarding ─────────────────────────────────────

    def _widget_offset(self, widget) -> Tuple[int, int]:
        ox = 0
        oy = 0
        cur = widget
        while cur is not None and cur is not self._tk_win:
            try:
                ox += int(cur.winfo_x())
                oy += int(cur.winfo_y())
                cur = cur.master
            except Exception:
                break
        return ox, oy

    def _widget_at(self, widget, x: int, y: int,
                   ox: int = 0, oy: int = 0):
        try:
            children = list(widget.winfo_children())
        except Exception:
            children = []
        for child in reversed(children):
            try:
                if not child.winfo_ismapped():
                    continue
                cx = ox + int(child.winfo_x())
                cy = oy + int(child.winfo_y())
                cw = int(child.winfo_width())
                ch = int(child.winfo_height())
            except Exception:
                continue
            if cx <= x < cx + cw and cy <= y < cy + ch:
                return self._widget_at(child, x, y, cx, cy)
        return widget, x - ox, y - oy

    def _forward_tk_mouse_event(self, sequence: str, x: int, y: int,
                                *, capture: bool = False,
                                delta: Optional[int] = None) -> bool:
        try:
            target_info = None
            if capture and self._press_target is not None:
                target = self._press_target
                ox, oy = self._widget_offset(target)
                target_info = (target, x - ox, y - oy)
            if target_info is None:
                target_info = self._widget_at(self._tk_win, x, y)
            target, lx, ly = target_info
            if sequence.startswith('<ButtonPress'):
                try:
                    target.focus_set()
                except Exception:
                    pass
            kwargs = {
                'x': max(0, int(lx)),
                'y': max(0, int(ly)),
                'rootx': int(self._screen_x + x),
                'rooty': int(self._screen_y + y),
            }
            if delta is not None:
                kwargs['delta'] = int(delta)
            target.event_generate(sequence, **kwargs)
            return True
        except Exception:
            return False

    def _on_cursor_pos(self, lx: float, ly: float) -> None:
        x, y = int(lx), int(ly)
        if self._forward_tk_mouse_event('<Motion>', x, y, capture=True):
            return
        lp = _make_lparam(x, y)
        _user32.PostMessageW(self._hwnd, WM_MOUSEMOVE, 0, lp)

    def _on_mouse_button(self, button: int, action: int,
                         mods: int, lx: float, ly: float) -> None:
        x, y = int(lx), int(ly)
        lp = _make_lparam(x, y)
        if button == 0:
            msg = WM_LBUTTONDOWN if action == 1 else WM_LBUTTONUP
            wp = MK_LBUTTON if action == 1 else 0
            sequence = '<ButtonPress-1>' if action == 1 else '<ButtonRelease-1>'
        elif button == 1:
            msg = WM_RBUTTONDOWN if action == 1 else WM_RBUTTONUP
            wp = MK_RBUTTON if action == 1 else 0
            sequence = '<ButtonPress-3>' if action == 1 else '<ButtonRelease-3>'
        else:
            return
        if action == 1:
            try:
                self._press_target = self._widget_at(self._tk_win, x, y)[0]
            except Exception:
                self._press_target = None
        forwarded = self._forward_tk_mouse_event(
            sequence, x, y, capture=action == 0)
        if action == 0:
            self._press_target = None
        if forwarded:
            return
        _user32.PostMessageW(self._hwnd, msg, wp, lp)
        if action == 1:
            try:
                _user32.SetFocus(self._hwnd)
            except Exception:
                pass

    def _on_scroll(self, dx: float, dy: float) -> None:
        delta = int(dy * 120)
        if self._forward_tk_mouse_event(
                '<MouseWheel>', self._width // 2, self._height // 2,
                delta=delta):
            return
        wp = (delta & 0xFFFF) << 16
        lp = _make_lparam(self._width // 2, self._height // 2)
        _user32.PostMessageW(self._hwnd, WM_MOUSEWHEEL, wp, lp)

    # ── Geometry hook (intercept Tk drag → move compositor layer) ─

    def _install_geometry_hook(self) -> None:
        win = self._tk_win
        self._orig_geometry = win.geometry

        mirror = self

        def _hooked_geometry(newGeometry=None):
            if newGeometry is None:
                return mirror._orig_geometry()
            x, y, w, h = mirror._screen_x, mirror._screen_y, \
                mirror._width, mirror._height
            try:
                parts = newGeometry.replace('+', ' +').replace('-', ' -').split()
                for p in parts:
                    if 'x' in p:
                        dims = p.split('x')
                        w = int(dims[0])
                        h = int(dims[1]) if len(dims) > 1 else h
                    elif p.startswith('+') or p.startswith('-'):
                        if x == mirror._screen_x and p != parts[-1]:
                            x = int(p)
                        else:
                            y = int(p)
            except Exception:
                return mirror._orig_geometry(newGeometry)
            mirror.set_geometry(x, y, w, h)

        win.geometry = _hooked_geometry
        win.wm_geometry = _hooked_geometry

    def _uninstall_geometry_hook(self) -> None:
        if self._orig_geometry is not None:
            try:
                self._tk_win.geometry = self._orig_geometry
                self._tk_win.wm_geometry = self._orig_geometry
            except Exception:
                pass
            self._orig_geometry = None


# ── Registry ─────────────────────────────────────────────────────
_mirrors: Dict[str, TkMirrorLayer] = {}
_mirror_lock = threading.Lock()


def mirror_tk_panel(tk_win, name: str, z: int = 500,
                    capture_fps: float = 30.0) -> TkMirrorLayer:
    """Create and attach a TkMirrorLayer for a Tk Toplevel."""
    with _mirror_lock:
        old = _mirrors.get(name)
        if old is not None:
            old.detach()
        m = TkMirrorLayer(tk_win, name, z, capture_fps)
        _mirrors[name] = m
    return m


def unmirror_tk_panel(name: str) -> None:
    with _mirror_lock:
        m = _mirrors.pop(name, None)
    if m is not None:
        m.detach()


def get_tk_mirror(name: str) -> Optional[TkMirrorLayer]:
    return _mirrors.get(name)


def stop_all_mirrors() -> None:
    with _mirror_lock:
        names = list(_mirrors.keys())
    for name in names:
        unmirror_tk_panel(name)


# ── SaoToplevel: drop-in replacement for tk.Toplevel ─────────────
#
# Usage:  from render.tk_mirror import SaoToplevel
#         win = SaoToplevel(root, mirror_name='my_panel')
#
# deiconify/withdraw/destroy are auto-hooked — the panel code
# needs ZERO boilerplate for compositor mirroring.

import tkinter as tk


def _compositor_tk_panels_enabled() -> bool:
    try:
        from config import SettingsManager
        return bool(SettingsManager().get('compositor_tk_panels', True))
    except Exception:
        return False


def _tk_root_for(widget) -> Optional[Any]:
    """Return the Tk root required for compositor input proxy windows."""
    try:
        root_fn = getattr(widget, '_root', None)
        if callable(root_fn):
            root = root_fn()
            if root is not None:
                return root
    except Exception:
        pass
    try:
        root = getattr(tk, '_default_root', None)
        if root is not None:
            return root
    except Exception:
        pass
    return getattr(widget, 'master', None)


class SaoToplevel(tk.Toplevel):
    """Tk Toplevel that automatically renders through the compositor.

    Drop-in replacement: ``SaoToplevel(root, mirror_name='panel')``
    instead of ``tk.Toplevel(root)``.  Show/hide/destroy route through
    TkMirrorLayer transparently.  Falls back to normal Tk rendering
    when ``compositor_tk_panels`` config is False or compositor is
    unavailable.
    """

    def __init__(self, *args, mirror_name: Optional[str] = None,
                 mirror_z: int = 500, mirror_fps: float = 30.0,
                 mirror_corner_radius: int = DEFAULT_MIRROR_CORNER_RADIUS,
                 **kwargs):
        super().__init__(*args, **kwargs)
        self._mirror: Optional[TkMirrorLayer] = None
        self._mirror_name = mirror_name or f'tk_{id(self)}'
        self._mirror_z = mirror_z
        self._mirror_fps = mirror_fps
        self._mirror_corner_radius = mirror_corner_radius
        self._mirror_attached = False

    def _ensure_mirror(self) -> None:
        if self._mirror_attached:
            return
        if not _compositor_tk_panels_enabled():
            self._mirror_attached = True
            return
        try:
            m = TkMirrorLayer(self, self._mirror_name,
                              z=self._mirror_z,
                              capture_fps=self._mirror_fps,
                              corner_radius=self._mirror_corner_radius)
            m.attach()
            self._mirror = m
            self._mirror_attached = True
        except Exception:
            self._mirror = None
            self._mirror_attached = False

    def deiconify(self) -> None:
        self._ensure_mirror()
        if self._mirror is not None:
            try:
                super().deiconify()
                self.attributes('-alpha', 0.01)
            except Exception:
                pass
            self._mirror.show()
        else:
            super().deiconify()

    def withdraw(self) -> None:
        if self._mirror is not None:
            self._mirror.hide()
        else:
            super().withdraw()

    def destroy(self) -> None:
        if self._mirror is not None:
            try:
                self._mirror.detach()
            except Exception:
                pass
            self._mirror = None
        super().destroy()
