"""GpuOverlayWindow — Phase 2 (v2.3.0) overlay infrastructure.

Provides a borderless transparent always-on-top click-through GLFW window
that hosts a moderngl context. Designed as a drop-in alternative to the
``tk.Toplevel`` + ``UpdateLayeredWindow`` (ULW) presentation path used by
SkillFX / HP / BossHP today.

Why bother:
- ULW costs ~1-2 ms per commit (BGRA premultiply roundtrip + Win32 GDI
  copy of the entire bitmap). For a panel that's already on the GPU
  (SkillFX SDF, BossHP burst), that's a wasted GPU→CPU→GPU bounce; a
  GLFW window with ``TRANSPARENT_FRAMEBUFFER`` lets the DWM compositor
  pull the framebuffer directly.
- Cleanly separates "compose" (worker thread) from "present" (Tk main
  thread): the present cost drops to a texture upload + one shader draw
  + ``swap_buffers``.

Constraints:
- GLFW init / window create / poll_events / swap_buffers must all run
  on the same thread. We pin everything to the Tk main thread by
  driving ``glfw.poll_events`` from a ``root.after`` pump and demanding
  callers schedule render from the same thread.
- Off-by-default. Set ``SAO_GPU_OVERLAY=1`` to enable. Without that
  env var the module imports cleanly but ``glfw_supported()`` returns
  ``False`` and overlays fall back to their ULW path.
- Each window owns its own moderngl context; resource sharing across
  windows is a Phase 2c concern (atlas, shared blur kernel).

Public API:
    glfw_supported() -> bool
    get_glfw_pump(root: tk.Misc) -> GlfwPump
    GpuOverlayWindow(pump, w, h, x, y, render_fn=None, click_through=True)
        .show() / .hide() / .destroy()
        .set_geometry(x, y, w, h)
        .set_render_fn(fn)  # fn(ctx: moderngl.Context, t: float) -> None
        .request_redraw()   # mark dirty; will render on next pump tick
        .ctx                # moderngl.Context (lazy)
"""
from __future__ import annotations

import ctypes
import os
import sys
import threading
import time
from ctypes import wintypes
from typing import Any, Callable, Dict, List, Optional

# Lazy imports — keep module import cost zero when GPU overlay disabled.
_glfw = None  # type: ignore[assignment]
_moderngl = None  # type: ignore[assignment]
_import_error: Optional[str] = None


def _try_imports() -> bool:
    """Import glfw + moderngl on first use. Returns True on success."""
    global _glfw, _moderngl, _import_error
    if _glfw is not None and _moderngl is not None:
        return True
    if _import_error is not None:
        return False
    try:
        import glfw as _g  # type: ignore[import-not-found]
        import moderngl as _m  # type: ignore[import-not-found]
    except Exception as exc:
        _import_error = f'{type(exc).__name__}: {exc}'
        return False
    _glfw = _g
    _moderngl = _m
    return True


def glfw_supported() -> bool:
    """True if GPU overlay path is available AND opted in.

    Opt-in via ``SAO_GPU_OVERLAY=1``. Without that flag we never touch
    GLFW at all — keeps the existing ULW path the default.
    """
    if not os.environ.get('SAO_GPU_OVERLAY'):
        return False
    if sys.platform != 'win32':
        return False
    return _try_imports()


# ── Win32 ex-style helpers (click-through reinforcement) ───────────────────
GWL_EXSTYLE = -20
WS_EX_LAYERED = 0x00080000
WS_EX_TRANSPARENT = 0x00000020
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_TOPMOST = 0x00000008
WS_EX_NOACTIVATE = 0x08000000
LWA_ALPHA = 0x00000002
HWND_TOPMOST = -1
SWP_NOMOVE = 0x0002
SWP_NOSIZE = 0x0001
SWP_NOACTIVATE = 0x0010
SWP_SHOWWINDOW = 0x0040

if sys.platform == 'win32':
    _user32 = ctypes.WinDLL('user32', use_last_error=True)
    _user32.GetWindowLongPtrW.restype = ctypes.c_ssize_t
    _user32.GetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
    _user32.SetWindowLongPtrW.restype = ctypes.c_ssize_t
    _user32.SetWindowLongPtrW.argtypes = [
        wintypes.HWND, ctypes.c_int, ctypes.c_ssize_t]
    _user32.SetLayeredWindowAttributes.restype = wintypes.BOOL
    _user32.SetLayeredWindowAttributes.argtypes = [
        wintypes.HWND, wintypes.COLORREF, wintypes.BYTE, wintypes.DWORD]
    _user32.SetWindowPos.restype = wintypes.BOOL
    _user32.SetWindowPos.argtypes = [
        wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
        ctypes.c_int, ctypes.c_int, wintypes.UINT]


def _apply_click_through(hwnd: int) -> None:
    """Belt-and-suspenders: GLFW 3.4 ``MOUSE_PASSTHROUGH`` already sets
    ``WS_EX_TRANSPARENT`` on Windows, but some drivers/DWM configs miss
    the layered-attributes call. Re-apply explicitly so DWM treats the
    framebuffer alpha as the per-pixel mask."""
    if sys.platform != 'win32':
        return
    cur = _user32.GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
    new = (cur | WS_EX_LAYERED | WS_EX_TRANSPARENT
           | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE)
    _user32.SetWindowLongPtrW(hwnd, GWL_EXSTYLE, new)
    _user32.SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)
    _user32.SetWindowPos(
        hwnd, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)


# ── GlfwPump ────────────────────────────────────────────────────────────────
class GlfwPump:
    """Single shared ``glfw.poll_events`` driver, ticked from a Tk
    ``after`` loop. One pump per process. Owns a registry of visible
    overlay windows and renders each one in order on every tick.

    The pump only runs while at least one window is visible; when the
    last window hides, the after-loop self-terminates so we don't spin
    in the background.
    """

    def __init__(self, root: Any):
        self._root = root
        self._lock = threading.Lock()
        self._windows: List['GpuOverlayWindow'] = []
        self._after_id: Optional[str] = None
        self._running = False
        self._tick_hz = 60
        self._tick_ms = max(1, int(round(1000.0 / self._tick_hz)))
        self._t0 = time.perf_counter()
        self._last_tick_t = self._t0
        # Init GLFW exactly once.
        self._inited = False

    def _ensure_init(self) -> None:
        if self._inited:
            return
        if not _try_imports():
            raise RuntimeError(
                f'glfw/moderngl unavailable: {_import_error}')
        if not _glfw.init():  # type: ignore[union-attr]
            raise RuntimeError('glfw.init() failed')
        self._inited = True

    def register(self, win: 'GpuOverlayWindow') -> None:
        with self._lock:
            if win not in self._windows:
                self._windows.append(win)
        self._kick()

    def unregister(self, win: 'GpuOverlayWindow') -> None:
        with self._lock:
            try:
                self._windows.remove(win)
            except ValueError:
                pass

    def _kick(self) -> None:
        if self._running:
            return
        self._running = True
        # Schedule first tick immediately. After-loop handles rescheduling.
        self._after_id = self._root.after(0, self._tick)

    def _tick(self) -> None:
        self._after_id = None
        if not self._running:
            return
        try:
            _glfw.poll_events()  # type: ignore[union-attr]
        except Exception:
            pass
        now = time.perf_counter()
        t = now - self._t0
        with self._lock:
            wins = list(self._windows)
        any_visible = False
        for w in wins:
            if not w._visible:
                continue
            any_visible = True
            try:
                w._render_once(t)
            except Exception:
                # Per-window render failure shouldn't kill the pump.
                pass
        self._last_tick_t = now
        if not any_visible:
            # Nobody to draw → suspend the loop until something registers.
            self._running = False
            return
        # Compute next deadline at fixed 60 Hz cadence (DWM caps anyway).
        elapsed_ms = (time.perf_counter() - now) * 1000.0
        delay_ms = max(1, int(round(self._tick_ms - elapsed_ms)))
        try:
            self._after_id = self._root.after(delay_ms, self._tick)
        except Exception:
            self._running = False

    def shutdown(self) -> None:
        self._running = False
        if self._after_id is not None:
            try:
                self._root.after_cancel(self._after_id)
            except Exception:
                pass
            self._after_id = None
        with self._lock:
            wins = list(self._windows)
            self._windows.clear()
        for w in wins:
            try:
                w.destroy()
            except Exception:
                pass
        if self._inited:
            try:
                _glfw.terminate()  # type: ignore[union-attr]
            except Exception:
                pass
            self._inited = False


_pump_lock = threading.Lock()
_pump: Optional[GlfwPump] = None


def get_glfw_pump(root: Any) -> GlfwPump:
    """Singleton pump bound to the Tk root. First call must pass root."""
    global _pump
    with _pump_lock:
        if _pump is None:
            _pump = GlfwPump(root)
        return _pump


# ── GpuOverlayWindow ────────────────────────────────────────────────────────
class GpuOverlayWindow:
    """Borderless transparent click-through GLFW window.

    Lifecycle:
      ow = GpuOverlayWindow(pump, w=800, h=200, x=100, y=100)
      ow.set_render_fn(lambda ctx, t: ...)   # required before show()
      ow.show()
      ...
      ow.set_geometry(x, y, w, h)
      ow.hide()
      ow.destroy()

    Threading: ALL methods must be called from the Tk main thread (same
    thread that owns the pump). ``request_redraw()`` is safe to call
    from any thread (just sets a flag).
    """

    def __init__(self, pump: GlfwPump, w: int, h: int,
                 x: int = 100, y: int = 100,
                 render_fn: Optional[Callable[[Any, float], None]] = None,
                 click_through: bool = True,
                 title: str = 'sao_overlay'):
        self._pump = pump
        self._w = max(1, int(w))
        self._h = max(1, int(h))
        self._x = int(x)
        self._y = int(y)
        self._render_fn = render_fn
        self._click_through = bool(click_through)
        self._title = title
        self._win = None  # GLFW window handle
        self._ctx: Any = None  # moderngl.Context
        self._visible = False
        self._dirty = True
        self._created = False

    # ---- lifecycle ----

    def _create(self) -> None:
        if self._created:
            return
        self._pump._ensure_init()
        glfw = _glfw
        glfw.window_hint(glfw.CONTEXT_VERSION_MAJOR, 3)  # type: ignore[union-attr]
        glfw.window_hint(glfw.CONTEXT_VERSION_MINOR, 3)  # type: ignore[union-attr]
        glfw.window_hint(glfw.OPENGL_PROFILE, glfw.OPENGL_CORE_PROFILE)  # type: ignore[union-attr]
        glfw.window_hint(glfw.DECORATED, glfw.FALSE)  # type: ignore[union-attr]
        glfw.window_hint(glfw.TRANSPARENT_FRAMEBUFFER, glfw.TRUE)  # type: ignore[union-attr]
        glfw.window_hint(glfw.FLOATING, glfw.TRUE)  # type: ignore[union-attr]
        glfw.window_hint(glfw.RESIZABLE, glfw.FALSE)  # type: ignore[union-attr]
        glfw.window_hint(glfw.SAMPLES, 0)  # type: ignore[union-attr]
        glfw.window_hint(glfw.VISIBLE, glfw.FALSE)  # type: ignore[union-attr]
        if self._click_through:
            try:
                glfw.window_hint(glfw.MOUSE_PASSTHROUGH, glfw.TRUE)  # type: ignore[union-attr]
            except Exception:
                # Older GLFW (<3.4): fall back to Win32 ex-style only.
                pass
        win = glfw.create_window(  # type: ignore[union-attr]
            self._w, self._h, self._title, None, None)
        if not win:
            raise RuntimeError('glfw.create_window failed for GpuOverlayWindow')
        glfw.set_window_pos(win, self._x, self._y)  # type: ignore[union-attr]
        glfw.make_context_current(win)  # type: ignore[union-attr]
        glfw.swap_interval(1)  # type: ignore[union-attr]

        if self._click_through and sys.platform == 'win32':
            try:
                hwnd = glfw.get_win32_window(win)  # type: ignore[union-attr]
                _apply_click_through(hwnd)
            except Exception:
                pass

        self._ctx = _moderngl.create_context()  # type: ignore[union-attr]
        self._ctx.enable(_moderngl.BLEND)  # type: ignore[union-attr]
        # Render targets must output PREMULTIPLIED alpha for DWM.
        self._ctx.blend_func = (
            _moderngl.ONE, _moderngl.ONE_MINUS_SRC_ALPHA)  # type: ignore[union-attr]

        self._win = win
        self._created = True

    def show(self) -> None:
        if not self._created:
            self._create()
        if self._win is None:
            return
        try:
            _glfw.show_window(self._win)  # type: ignore[union-attr]
        except Exception:
            pass
        self._visible = True
        self._dirty = True
        self._pump.register(self)

    def hide(self) -> None:
        self._visible = False
        if self._win is not None:
            try:
                _glfw.hide_window(self._win)  # type: ignore[union-attr]
            except Exception:
                pass
        self._pump.unregister(self)

    def destroy(self) -> None:
        self._visible = False
        self._pump.unregister(self)
        if self._win is None:
            return
        try:
            _glfw.make_context_current(self._win)  # type: ignore[union-attr]
        except Exception:
            pass
        if self._ctx is not None:
            try:
                self._ctx.release()
            except Exception:
                pass
            self._ctx = None
        try:
            _glfw.destroy_window(self._win)  # type: ignore[union-attr]
        except Exception:
            pass
        self._win = None
        self._created = False

    # ---- mutators ----

    def set_geometry(self, x: int, y: int, w: int, h: int) -> None:
        self._x, self._y = int(x), int(y)
        nw, nh = max(1, int(w)), max(1, int(h))
        size_changed = (nw, nh) != (self._w, self._h)
        self._w, self._h = nw, nh
        if self._win is not None:
            try:
                _glfw.set_window_pos(self._win, self._x, self._y)  # type: ignore[union-attr]
                if size_changed:
                    _glfw.set_window_size(self._win, self._w, self._h)  # type: ignore[union-attr]
            except Exception:
                pass
        self._dirty = True

    def set_render_fn(self, fn: Callable[[Any, float], None]) -> None:
        self._render_fn = fn
        self._dirty = True

    def request_redraw(self) -> None:
        """Thread-safe: mark dirty so next pump tick draws even if the
        scheduler has been throttling idle frames."""
        self._dirty = True

    # ---- internal ----

    def _render_once(self, t: float) -> None:
        if self._win is None or self._ctx is None or self._render_fn is None:
            return
        glfw = _glfw
        try:
            glfw.make_context_current(self._win)  # type: ignore[union-attr]
        except Exception:
            return
        ctx = self._ctx
        try:
            ctx.screen.use()
            ctx.viewport = (0, 0, self._w, self._h)
            # Always clear to fully transparent before user draws so old
            # frame doesn't bleed through under DWM transparency.
            ctx.clear(0.0, 0.0, 0.0, 0.0)
            self._render_fn(ctx, t)
        except Exception:
            pass
        try:
            glfw.swap_buffers(self._win)  # type: ignore[union-attr]
        except Exception:
            pass
        self._dirty = False


# ── BgraPresenter ───────────────────────────────────────────────────────────
# Helper that turns a GpuOverlayWindow into a "present already-composed
# BGRA bytes" surface. The compose worker still produces premultiplied
# BGRA bytes (same as the ULW path); we just upload them to a moderngl
# texture and draw a fullscreen quad. Zero visual change vs ULW —
# this is purely a presentation-layer swap that eliminates the per-frame
# Win32 GDI bitmap copy.
#
# Texture format note: moderngl's RGBA8 texture sampled as ``texture(...)``
# returns the bytes in (R, G, B, A) order. Our worker emits BGRA, so the
# fragment shader swizzles ``c.bgra`` to recover (R, G, B, A). The bytes
# are already premultiplied so we leave them as-is.

_BGRA_VS = """
#version 330
in vec2 in_pos;
out vec2 v_uv;
void main() {
    // Flip Y: BGRA buffer is top-down (matches Windows DIB), but GL
    // sampling has bottom-left origin. Map UV vertically to get
    // pixel-perfect orientation without any CPU-side flipud.
    v_uv = vec2(in_pos.x * 0.5 + 0.5,
                1.0 - (in_pos.y * 0.5 + 0.5));
    gl_Position = vec4(in_pos, 0.0, 1.0);
}
"""

_BGRA_FS = """
#version 330
in vec2 v_uv;
out vec4 fragColor;
uniform sampler2D u_tex;
void main() {
    vec4 c = texture(u_tex, v_uv);
    // Bytes packed as BGRA — swizzle to (R, G, B, A). Already premult.
    fragColor = c.bgra;
}
"""


class BgraPresenter:
    """Owns the texture+VAO+program needed to blit a BGRA-bytes buffer
    onto a GpuOverlayWindow as a fullscreen quad.

    Usage:
        presenter = BgraPresenter()
        win = GpuOverlayWindow(pump, w, h, x, y,
                                render_fn=presenter.render,
                                click_through=True)
        win.show()
        ...
        # Each frame, on Tk main thread:
        presenter.set_frame(bgra_bytes, w, h)
        win.request_redraw()

    The presenter reuses its texture across frames as long as (w, h)
    doesn't change; resizes trigger a single texture realloc. Safe to
    share one presenter across windows of the same size, but the
    common case is one presenter per window.
    """

    def __init__(self) -> None:
        self._prog = None
        self._vbo = None
        self._vao = None
        self._tex = None
        self._tex_w = 0
        self._tex_h = 0
        self._frame_bytes: Optional[bytes] = None
        self._frame_w = 0
        self._frame_h = 0
        self._dirty = False

    def set_frame(self, bgra: bytes, w: int, h: int) -> None:
        """Stage a frame for the next render. Cheap (just stores refs).

        Safe to call from any thread, but typically called from the
        same thread as ``render`` (Tk main).
        """
        self._frame_bytes = bgra
        self._frame_w = int(w)
        self._frame_h = int(h)
        self._dirty = True

    def clear(self) -> None:
        """Stage a transparent frame (drops the cached bytes; next
        ``render`` will just clear to (0,0,0,0))."""
        self._frame_bytes = None
        self._dirty = True

    def render(self, ctx: Any, _t: float) -> None:
        """``GpuOverlayWindow`` render_fn. Uploads + draws latest frame.

        Called on the Tk main thread (pump thread). Cheap when no new
        frame is pending: just re-draws the existing texture.
        """
        if _moderngl is None:
            return
        bgra = self._frame_bytes
        w = self._frame_w
        h = self._frame_h
        if self._prog is None:
            self._prog = ctx.program(
                vertex_shader=_BGRA_VS, fragment_shader=_BGRA_FS)
            self._prog['u_tex'].value = 0
            import numpy as _np
            quad = _np.array([-1, -1, 1, -1, -1, 1, 1, 1], dtype='f4')
            self._vbo = ctx.buffer(quad.tobytes())
            self._vao = ctx.vertex_array(
                self._prog, [(self._vbo, '2f', 'in_pos')])
        if bgra is not None and w > 0 and h > 0:
            if self._tex is None or self._tex_w != w or self._tex_h != h:
                if self._tex is not None:
                    try:
                        self._tex.release()
                    except Exception:
                        pass
                self._tex = ctx.texture((w, h), 4, bgra)
                self._tex_w = w
                self._tex_h = h
            else:
                # Fast path: realloc-free upload of BGRA bytes.
                try:
                    self._tex.write(bgra)
                except Exception:
                    # Size mismatch fallback — recreate texture.
                    try:
                        self._tex.release()
                    except Exception:
                        pass
                    self._tex = ctx.texture((w, h), 4, bgra)
                    self._tex_w = w
                    self._tex_h = h
            self._tex.use(location=0)
            self._vao.render(_moderngl.TRIANGLE_STRIP)  # type: ignore[union-attr]
        # If no frame staged: ctx.clear in GpuOverlayWindow already
        # produced a fully transparent frame; nothing to draw.
        self._dirty = False

    def release(self) -> None:
        for obj_name in ('_tex', '_vao', '_vbo', '_prog'):
            obj = getattr(self, obj_name, None)
            if obj is None:
                continue
            try:
                obj.release()
            except Exception:
                pass
            setattr(self, obj_name, None)
        self._frame_bytes = None

