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
from typing import Any, Callable, Dict, Optional

from render.webview_proxy import (
    _capture_window, _make_lparam, _user32,
    WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP,
    WM_RBUTTONDOWN, WM_RBUTTONUP, WM_MOUSEWHEEL,
    MK_LBUTTON, MK_RBUTTON,
)

WS_EX_NOACTIVATE = 0x08000000
GWL_EXSTYLE = -20


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
                 capture_fps: float = 30.0):
        self._tk_win = tk_win
        self._name = name
        self._z = z
        self._capture_interval = 1.0 / max(1.0, capture_fps)

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

        ex = _user32.GetWindowLongPtrW(self._hwnd, GWL_EXSTYLE)
        _user32.SetWindowLongPtrW(
            self._hwnd, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE)

        _user32.MoveWindow(
            self._hwnd, -10000, -10000,
            self._width, self._height, False)

        self._install_geometry_hook()

        from render.gpu_overlay_window import _get_unified_overlay
        uo = _get_unified_overlay()
        self._layer = uo.create_layer(
            f'tk_{self._name}',
            width=self._width,
            height=self._height,
            x=self._screen_x,
            y=self._screen_y,
            z=self._z,
            click_through=False,
        )
        self._layer.set_input_callbacks(
            cursor_pos_fn=self._on_cursor_pos,
            mouse_button_fn=self._on_mouse_button,
            scroll_fn=self._on_scroll,
        )

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

        try:
            _user32.MoveWindow(
                self._hwnd,
                self._screen_x, self._screen_y,
                self._width, self._height, True)
        except Exception:
            pass
        self._attached = False

    def show(self) -> None:
        self._visible = True
        if self._layer:
            self._layer.show()

    def hide(self) -> None:
        self._visible = False
        if self._layer:
            self._layer.hide()

    def set_position(self, x: int, y: int) -> None:
        self._screen_x = x
        self._screen_y = y
        if self._layer:
            self._layer.set_position(x, y)

    def set_geometry(self, x: int, y: int, w: int, h: int) -> None:
        self._screen_x = x
        self._screen_y = y
        changed = (w != self._width or h != self._height)
        self._width = w
        self._height = h
        if self._layer:
            self._layer.set_geometry(x, y, w, h)
        if changed and self._hwnd:
            _user32.MoveWindow(
                self._hwnd, -10000, -10000, w, h, True)

    # ── Capture loop ─────────────────────────────────────────

    def _capture_loop(self) -> None:
        while self._running:
            if self._visible and self._layer and self._hwnd:
                bgra = _capture_window(
                    self._hwnd, self._width, self._height)
                if bgra:
                    self._layer.upload_bgra(
                        bgra, self._width, self._height)
                    self._layer.request_redraw()
            self._stop_evt.wait(timeout=self._capture_interval)

    # ── Input forwarding ─────────────────────────────────────

    def _on_cursor_pos(self, lx: float, ly: float) -> None:
        x, y = int(lx), int(ly)
        lp = _make_lparam(x, y)
        _user32.PostMessageW(self._hwnd, WM_MOUSEMOVE, 0, lp)

    def _on_mouse_button(self, button: int, action: int,
                         mods: int, lx: float, ly: float) -> None:
        x, y = int(lx), int(ly)
        lp = _make_lparam(x, y)
        if button == 0:
            msg = WM_LBUTTONDOWN if action == 1 else WM_LBUTTONUP
            wp = MK_LBUTTON if action == 1 else 0
        elif button == 1:
            msg = WM_RBUTTONDOWN if action == 1 else WM_RBUTTONUP
            wp = MK_RBUTTON if action == 1 else 0
        else:
            return
        _user32.PostMessageW(self._hwnd, msg, wp, lp)
        if action == 1:
            try:
                _user32.SetFocus(self._hwnd)
            except Exception:
                pass

    def _on_scroll(self, dx: float, dy: float) -> None:
        delta = int(dy * 120)
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


class SaoToplevel(tk.Toplevel):
    """Tk Toplevel that automatically renders through the compositor.

    Drop-in replacement: ``SaoToplevel(root, mirror_name='panel')``
    instead of ``tk.Toplevel(root)``.  Show/hide/destroy route through
    TkMirrorLayer transparently.  Falls back to normal Tk rendering
    when ``compositor_tk_panels`` config is False or compositor is
    unavailable.
    """

    def __init__(self, *args, mirror_name: Optional[str] = None,
                 mirror_z: int = 500, mirror_fps: float = 30.0, **kwargs):
        super().__init__(*args, **kwargs)
        self._mirror: Optional[TkMirrorLayer] = None
        self._mirror_name = mirror_name or f'tk_{id(self)}'
        self._mirror_z = mirror_z
        self._mirror_fps = mirror_fps
        self._mirror_attached = False

    def _ensure_mirror(self) -> None:
        if self._mirror_attached:
            return
        self._mirror_attached = True
        if not _compositor_tk_panels_enabled():
            return
        try:
            m = TkMirrorLayer(self, self._mirror_name,
                              z=self._mirror_z,
                              capture_fps=self._mirror_fps)
            m.attach()
            self._mirror = m
        except Exception:
            self._mirror = None

    def deiconify(self) -> None:
        self._ensure_mirror()
        if self._mirror is not None:
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
