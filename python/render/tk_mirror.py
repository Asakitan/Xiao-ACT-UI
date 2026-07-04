# -*- coding: utf-8 -*-
"""tk_mirror — capture Tk Toplevel windows and present via compositor.

Mirrors a Tk Toplevel through the unified overlay compositor:
  1. Makes the real Tk window invisible (alpha≈0, WS_EX_TRANSPARENT)
  2. Captures its rendered content via PrintWindow → BGRA at target FPS
  3. Uploads the BGRA to a compositor layer (protected by WDA)
  4. Forwards mouse events by walking this window's own Tk widget geometry
     to resolve the hit widget (GetCursorPos coords) and synthesizing the
     event directly onto it with event_generate()
  5. Intercepts geometry() calls so drag operations move the compositor layer
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import threading
import time
from typing import Any, Callable, Dict, Optional, Tuple

import numpy as np

from render.webview_proxy import _capture_window, _user32

WS_EX_NOACTIVATE = 0x08000000
WS_EX_TRANSPARENT = 0x00000020
GWL_EXSTYLE = -20

_user32.GetCursorPos.argtypes = [ctypes.POINTER(wt.POINT)]
_user32.GetCursorPos.restype = wt.BOOL

#: Default corner radius (px) applied to mirrored Tk panels — matches the
#: DWM window-rounding look used elsewhere (rounded_panel()/CreateRoundRectRgn
#: precedent in this codebase uses 10-14px for whole-panel rounding).
DEFAULT_MIRROR_CORNER_RADIUS = 12

#: Feather width (px) for the anti-aliased corner edge — wide enough to erase
#: the binary-cutoff staircase at the default 12px radius, narrow enough not
#: to visibly eat into the arc.
_CORNER_FEATHER_PX = 1.0

# Keyed by radius only, not (w, h, radius): the quarter-circle coverage
# pattern depends solely on the radius (circle center is fixed at
# (r-0.5, r-0.5) within the block); w/h only decide where the block gets
# mirrored to. Every panel on the platform shares the same default radius,
# so this cache is effectively a single entry — unlike the old (w, h, radius)
# key, which got evicted every resize/open-close animation tick as w/h
# changed, defeating the cache exactly when it mattered most.
_corner_alpha_cache: Dict[int, np.ndarray] = {}


def _corner_quadrant_alpha(r: int) -> Optional[np.ndarray]:
    """(r, r) float32 coverage ramp for one rounded corner, in [0, 1].

    1.0 well inside the arc, 0.0 well outside, with a ~1px linear feather
    across the boundary instead of a hard cutoff. The other three corners
    reuse this same array via axis flips when applied.
    """
    if r <= 0:
        return None
    cached = _corner_alpha_cache.get(r)
    if cached is not None:
        return cached
    yy, xx = np.mgrid[0:r, 0:r].astype(np.float32)
    c = r - 0.5
    dist = np.sqrt((xx - c) ** 2 + (yy - c) ** 2)
    fw = _CORNER_FEATHER_PX
    alpha = np.clip((r + fw * 0.5 - dist) / fw, 0.0, 1.0).astype(np.float32)
    if len(_corner_alpha_cache) > 8:
        _corner_alpha_cache.clear()
    _corner_alpha_cache[r] = alpha
    return alpha


def _apply_corner_mask(bgra: bytes, w: int, h: int, radius: int) -> bytes:
    """Feather (premultiplied) BGRA pixels outside the rounded-rect corners.

    Only the four ``r``x``r`` corner blocks are touched (not the whole
    frame): each channel of a premultiplied pixel scales linearly with a
    coverage factor in [0, 1], so multiplying all four BGRA bytes by the
    same per-pixel factor is a correct edge-to-transparent blend without an
    unpremultiply/re-premultiply round trip.
    """
    if radius <= 0 or w <= 0 or h <= 0:
        return bgra
    r = min(int(radius), w // 2, h // 2)
    quad = _corner_quadrant_alpha(r)
    if quad is None:
        return bgra
    expected = w * h * 4
    arr = np.frombuffer(bgra, dtype=np.uint8)
    if arr.size != expected:
        return bgra
    img = arr.reshape(h, w, 4).copy()
    a = quad[:, :, None]  # (r, r, 1) — broadcasts across all 4 BGRA channels

    def _scale(block: np.ndarray, coeff: np.ndarray) -> np.ndarray:
        # np.rint (not truncation) — truncating float->uint8 is systematically
        # biased dark by ~0.5/255 per channel at the feathered edge.
        return np.rint(block.astype(np.float32) * coeff).astype(np.uint8)

    img[:r, :r] = _scale(img[:r, :r], a)
    img[:r, w - r:] = _scale(img[:r, w - r:], a[:, ::-1])
    img[h - r:, :r] = _scale(img[h - r:, :r], a[::-1, :])
    img[h - r:, w - r:] = _scale(img[h - r:, w - r:], a[::-1, ::-1])
    return img.tobytes()


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
        self._compositor = None
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
        self._held_button: int = -1
        self._pressed_widget = None
        self._hover_widget = None
        # (widget, x0, y0, x1, y1) for the last resolved hit — re-verified
        # cheaply (4 winfo_* calls) on every move instead of re-walking the
        # whole widget tree, since that's the common case (cursor sitting
        # still or drifting slightly within the same control).
        self._hit_rect_cache: Optional[Tuple[Any, int, int, int, int]] = None

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
            raise RuntimeError('Tk mirror requires a Tk root')

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
                cursor_leave_fn=self._on_cursor_leave,
                mouse_button_fn=self._on_mouse_button,
                scroll_fn=self._on_scroll,
            )
            # A mirrored Tk panel is a solid window: hit-test its whole
            # rect, not the captured per-pixel alpha. PrintWindow gives
            # native Entry/Text controls a zero alpha byte, so an
            # alpha-span click region would exclude exactly the text
            # fields (opaque custom-drawn buttons stayed clickable) —
            # that is the "text boxes can't be clicked" regression.
            layer.rect_hit = True
        except Exception:
            if layer is not None:
                try:
                    uo.destroy_layer(layer_name)
                except Exception:
                    pass
            raise

        self._layer = layer
        self._compositor = uo

        try:
            self._orig_exstyle = _user32.GetWindowLongPtrW(
                self._hwnd, GWL_EXSTYLE)
            try:
                self._orig_alpha = float(win.attributes('-alpha') or 1.0)
            except Exception:
                self._orig_alpha = 1.0

            _user32.SetWindowLongPtrW(
                self._hwnd, GWL_EXSTYLE,
                self._orig_exstyle | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)
            try:
                win.attributes('-alpha', 0.01)
            except Exception:
                pass

            self._install_geometry_hook()
        except Exception:
            try:
                uo.destroy_layer(layer_name)
            except Exception:
                pass
            self._layer = None
            raise

        self._attached = True
        self._running = True
        self._stop_evt.clear()
        self._thread = threading.Thread(
            target=self._capture_loop, daemon=True,
            name=f'tk-mirror-{self._name}',
        )
        self._thread.start()
        self._sync_host_input()

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
        self._sync_host_input()

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

    def _sync_host_input(self) -> None:
        try:
            if self._compositor is not None:
                self._compositor.sync_host_input_mode()
        except Exception:
            pass

    def show(self) -> None:
        self._visible = True
        if self._hwnd:
            # Undo hide()'s off-screen park before the capture loop (see
            # below) resumes reading this window — otherwise the first
            # captured frame(s) after reopening would show whatever sat
            # at (-32000, -32000), not the panel's actual content.
            try:
                _user32.MoveWindow(
                    self._hwnd, self._screen_x, self._screen_y,
                    self._width, self._height, False)
            except Exception:
                pass
        if self._layer:
            self._layer.show()
        self._sync_host_input()

    def hide(self) -> None:
        self._visible = False
        if self._layer:
            self._layer.hide()
        if self._hwnd:
            # The real backing Tk window has to stay MAPPED (not
            # iconic/withdrawn) for PrintWindow to keep capturing
            # correctly the next time show() runs — see attach()'s
            # comment on why it's pinned at alpha=0.01 instead of
            # actually withdrawn. That's a reason to keep it non-
            # iconic, not a reason to leave it sitting at its normal
            # on-screen rect for the rest of the session while the
            # panel is "closed": park it off-screen instead, same
            # technique already used elsewhere in this codebase for a
            # window that must stay alive but never actually visible.
            try:
                _user32.MoveWindow(
                    self._hwnd, -32000, -32000,
                    self._width, self._height, False)
            except Exception:
                pass
        self._sync_host_input()

    def set_alpha(self, value: float) -> None:
        """Fade the compositor layer directly.

        ``SaoToplevel.attributes('-alpha', ...)`` is a no-op once mirrored
        (see below) — the real Tk window's alpha is pinned at 0.01 so it
        never becomes visible on the desktop. Callers that want a fade
        (e.g. a close animation) need to fade the compositor layer instead.
        """
        if self._layer is not None:
            self._layer.alpha = max(0.0, min(1.0, value))
            self._layer._dirty = True
        # A layer that hides mid-press (e.g. its own close button) never
        # gets the matching button-up — the compositor's capture routing
        # sees .visible go False and stops delivering to this layer at all.
        # Without this reset, _pressed_widget stays stale and the *next*
        # time this panel is reopened, the first hover move is wrongly
        # treated as an in-progress drag instead of resolving normally.
        self._pressed_widget = None
        self._hover_widget = None
        self._held_button = -1
        self._hit_rect_cache = None

    def set_position(self, x: int, y: int) -> None:
        self._screen_x = x
        self._screen_y = y
        self._hit_rect_cache = None
        if self._layer:
            self._layer.set_position(x, y)
        if self._hwnd:
            _user32.MoveWindow(
                self._hwnd, x, y,
                self._width, self._height, False)

    def set_geometry(self, x: int, y: int, w: int, h: int) -> None:
        self._screen_x = x
        self._screen_y = y
        self._width = w
        self._height = h
        self._hit_rect_cache = None
        if self._layer:
            self._layer.set_geometry(x, y, w, h)
        if self._hwnd:
            _user32.MoveWindow(self._hwnd, x, y, w, h, True)

    def sync_from_window(self) -> None:
        """Sync layer size from actual Win32 window rect."""
        if not self._hwnd or self._layer is None:
            return
        rect = wt.RECT()
        if not _user32.GetWindowRect(self._hwnd, ctypes.byref(rect)):
            return
        w = max(1, rect.right - rect.left)
        h = max(1, rect.bottom - rect.top)
        if w != self._width or h != self._height:
            self._width = w
            self._height = h
            try:
                self._layer.set_geometry(
                    self._screen_x, self._screen_y, w, h)
            except Exception:
                pass

    # ── Capture loop ─────────────────────────────────────────

    def _capture_loop(self) -> None:
        rect = wt.RECT()
        while self._running:
            layer = self._layer
            if self._visible and layer is not None and self._hwnd:
                if _user32.GetWindowRect(self._hwnd, ctypes.byref(rect)):
                    aw = max(1, rect.right - rect.left)
                    ah = max(1, rect.bottom - rect.top)
                    if aw != self._width or ah != self._height:
                        self._width = aw
                        self._height = ah
                        try:
                            layer.set_geometry(
                                self._screen_x, self._screen_y, aw, ah)
                        except Exception:
                            pass
                # Snapshot once and reuse for capture + mask + upload.
                # self._width/_height can be written concurrently from the
                # Tk thread (set_geometry(), driven by e.g. a dialog's
                # open/close resize animation firing every ~16ms) — reading
                # them twice (once for the capture call, again for the
                # upload call) let the buffer be captured at one size but
                # labeled with a different, newer size, which crashed
                # _ensure_texture's ctx.texture() with a data/size mismatch.
                cap_w, cap_h = self._width, self._height
                # An uncaught exception here escapes the while loop and
                # silently kills this whole background thread (Python's
                # default behavior for an unhandled thread exception is to
                # print a traceback and exit — no auto-restart), permanently
                # freezing this panel's mirror until it's closed and
                # reopened. A single bad frame shouldn't take the thread
                # down; skip it and retry next tick instead.
                try:
                    bgra = _capture_window(self._hwnd, cap_w, cap_h)
                except Exception:
                    bgra = None
                if bgra and layer is not None:
                    try:
                        if self._corner_radius > 0:
                            bgra = _apply_corner_mask(
                                bgra, cap_w, cap_h, self._corner_radius)
                        layer.upload_bgra(bgra, cap_w, cap_h)
                        layer.request_redraw()
                    except Exception:
                        pass
            self._stop_evt.wait(timeout=self._capture_interval)

    # ── Input forwarding (direct Tk widget dispatch) ──
    #
    # Previously this forwarded via SendMessageW(WM_LBUTTONDOWN/UP/MOVE) to
    # the hidden (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, alpha=0.01) source
    # HWND and relied on Tk's own Win32 message pump to hit-test the click
    # against a widget. That round-trip was unreliable for this window
    # (never activated, never really painted) — clicks would frequently not
    # reach any widget's binding at all.
    #
    # Instead: read the *current* absolute cursor position (GetCursorPos,
    # always accurate — unaffected by any queued/stale compositor-thread
    # coords), manually walk this mirror's own Tk widget tree to find which
    # widget's rect contains that screen point, and synthesize the event
    # straight onto that widget with ``event_generate``. This never touches
    # the Win32 message queue — it is pure in-process Tk event dispatch, the
    # same mechanism real mouse input would use once a target is resolved.
    #
    # Deliberately not Tk's built-in ``winfo_containing()``: on Windows that
    # resolves the toplevel via a WindowFromPoint-style OS query first, and
    # this source window is WS_EX_TRANSPARENT (needed so it doesn't itself
    # block real clicks to the game/compositor) — such a query would skip
    # right over it and find nothing. Walking ``self._tk_win``'s own child
    # geometry directly needs no OS window lookup at all: we already know
    # which toplevel to search.
    #
    # A press on widget W keeps W as the target for subsequent move/release
    # events regardless of where the cursor drifts (implicit grab), matching
    # how a real click-drag on W would behave.

    def _cursor_screen_pos(self) -> Tuple[int, int]:
        pt = wt.POINT()
        _user32.GetCursorPos(ctypes.byref(pt))
        return pt.x, pt.y

    # Bound how long a cached hit-rect can be reused without re-walking the
    # tree, so a layout rebuild under a stationary cursor (e.g. the plugin
    # card list refreshing) can't leave the cache stale indefinitely.
    _HIT_CACHE_MAX_AGE = 0.3

    def _widget_at(self, sx: int, sy: int):
        # Only a *leaf* widget's rect is safe to trust via plain containment:
        # a container's bounding rect can have children positioned inside
        # it, and a point inside the container's rect but over one of those
        # children must resolve to the child, not the container. Since we
        # don't track the "holes" a container's children carve out of its
        # rect, reusing a container hit via containment alone would wrongly
        # swallow points that actually belong to a child (e.g. the header
        # frame's cached rect masking its close button) — which is exactly
        # why hover effects were firing inconsistently. So the cache only
        # short-circuits for widgets with no children at all.
        cache = self._hit_rect_cache
        if cache is not None:
            widget, x0, y0, x1, y1, stamp = cache
            if (time.monotonic() - stamp < self._HIT_CACHE_MAX_AGE
                    and x0 <= sx < x1 and y0 <= sy < y1):
                try:
                    if widget.winfo_viewable() and not widget.winfo_children():
                        return widget
                except Exception:
                    pass
            self._hit_rect_cache = None

        win = self._tk_win
        try:
            wx0, wy0 = win.winfo_rootx(), win.winfo_rooty()
            wx1, wy1 = wx0 + win.winfo_width(), wy0 + win.winfo_height()
            if not (wx0 <= sx < wx1 and wy0 <= sy < wy1):
                return None
        except Exception:
            return None
        best = win
        best_rect = (wx0, wy0, wx1, wy1)
        best_is_leaf = False

        def _descend(children) -> None:
            nonlocal best, best_rect, best_is_leaf
            for ch in children:
                try:
                    if not ch.winfo_viewable():
                        continue
                    cx, cy = ch.winfo_rootx(), ch.winfo_rooty()
                    cw, chh = ch.winfo_width(), ch.winfo_height()
                    if cx <= sx < cx + cw and cy <= sy < cy + chh:
                        best = ch
                        best_rect = (cx, cy, cx + cw, cy + chh)
                        grandchildren = ch.winfo_children()
                        best_is_leaf = not grandchildren
                        if grandchildren:
                            _descend(grandchildren)
                except Exception:
                    continue

        try:
            _descend(win.winfo_children())
        except Exception:
            pass
        if best_is_leaf:
            x0, y0, x1, y1 = best_rect
            self._hit_rect_cache = (best, x0, y0, x1, y1, time.monotonic())
        else:
            self._hit_rect_cache = None
        return best

    # X11/Tk event-state bits (used by real Tk on every platform, including
    # Windows) — set explicitly rather than relying on event_generate() to
    # infer them from the sequence's modifier prefix.
    _STATE_BUTTON1 = 0x100
    _STATE_BUTTON3 = 0x400

    def _generate(self, widget, sequence: str, sx: int, sy: int,
                  state: int = 0, **extra) -> None:
        try:
            lx = sx - widget.winfo_rootx()
            ly = sy - widget.winfo_rooty()
            widget.event_generate(
                sequence, x=lx, y=ly, rootx=sx, rooty=sy, state=state,
                **extra)
        except Exception:
            pass

    def _on_cursor_pos(self, _lx: float, _ly: float) -> None:
        sx, sy = self._cursor_screen_pos()
        if self._pressed_widget is not None:
            # Real Tk/X11 suppresses Enter/Leave while a button is held
            # (implicit grab) — only the grabbed widget gets motion.
            target = self._pressed_widget
            if self._held_button == 0:
                self._generate(target, '<B1-Motion>', sx, sy, state=self._STATE_BUTTON1)
            elif self._held_button == 1:
                self._generate(target, '<B3-Motion>', sx, sy, state=self._STATE_BUTTON3)
            return

        target = self._widget_at(sx, sy)
        if target is not self._hover_widget:
            prev = self._hover_widget
            self._hover_widget = target
            if prev is not None:
                self._generate(prev, '<Leave>', sx, sy)
            if target is not None:
                self._generate(target, '<Enter>', sx, sy)
        if target is not None:
            self._generate(target, '<Motion>', sx, sy)

    def _on_cursor_leave(self) -> None:
        prev = self._hover_widget
        self._hover_widget = None
        if prev is not None:
            sx, sy = self._cursor_screen_pos()
            self._generate(prev, '<Leave>', sx, sy)

    def _on_mouse_button(self, button: int, action: int,
                         mods: int, _lx: float, _ly: float) -> None:
        if button not in (0, 1):
            return
        sx, sy = self._cursor_screen_pos()
        if action == 1:
            target = self._widget_at(sx, sy)
            if target is None:
                return
            self._pressed_widget = target
            self._held_button = button
            try:
                target.focus_set()
            except Exception:
                pass
            seq = '<ButtonPress-1>' if button == 0 else '<ButtonPress-3>'
            self._generate(target, seq, sx, sy)
        else:
            target = self._pressed_widget
            self._pressed_widget = None
            self._held_button = -1
            if target is None:
                return
            seq = '<ButtonRelease-1>' if button == 0 else '<ButtonRelease-3>'
            state = self._STATE_BUTTON1 if button == 0 else self._STATE_BUTTON3
            self._generate(target, seq, sx, sy, state=state)

    def _on_scroll(self, _dx: float, dy: float) -> None:
        sx, sy = self._cursor_screen_pos()
        target = self._pressed_widget or self._widget_at(sx, sy)
        if target is None:
            return
        self._generate(target, '<MouseWheel>', sx, sy, delta=int(dy * 120))

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

    def attributes(self, *args):
        if self._mirror is not None and args:
            if str(args[0]) == '-alpha' and len(args) >= 2:
                return ''
        return super().attributes(*args)

    wm_attributes = attributes

    def state(self, *args):
        """Report 'withdrawn' based on mirror visibility, not real Tk state.

        Once a mirror is attached, ``withdraw()`` never calls
        ``super().withdraw()`` (see below) — the real Tk window has to stay
        mapped for PrintWindow capture to keep working even while the panel
        is "closed" from the compositor's point of view. That means plain
        ``tk.Toplevel.state()`` never reports 'withdrawn' again for the rest
        of this window's life, which broke every panel's own
        ``is_visible()`` (they almost all check ``win.state() !=
        'withdrawn'``) and any code that re-shows panels based on that check
        — a panel the user had already closed would get silently
        re-deiconified as a side effect of unrelated panel/state-restore
        logic elsewhere reading a stale 'not withdrawn' state.
        """
        if self._mirror is not None and not args:
            return 'normal' if self._mirror._visible else 'withdrawn'
        return super().state(*args)

    def deiconify(self) -> None:
        self._ensure_mirror()
        if self._mirror is not None:
            try:
                super().deiconify()
                super().update_idletasks()
                super().attributes('-alpha', 0.01)
            except Exception:
                pass
            self._mirror.sync_from_window()
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
