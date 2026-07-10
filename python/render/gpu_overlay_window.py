# GpuOverlayWindow — compositor-only overlay infrastructure.
#
# All overlay windows delegate to the unified DWM compositor
# (overlay_compositor / overlay_adapter). The legacy per-window GLFW
# path has been removed; this module retains the public API surface so
# callers (GpuOverlayWindow, get_glfw_pump, glfw_supported, BgraPresenter,
# Win32 helpers) continue to work without changes.
#
# Public API:
#   glfw_supported() -> bool
#   get_glfw_pump(root) -> _PumpStub
#   GpuOverlayWindow(pump, w, h, x, y, render_fn=None, click_through=True)
#     .show() / .hide() / .destroy()
#     .set_geometry(x, y, w, h)
#     .set_render_fn(fn)
#     .request_redraw()
#     .ctx
#   BgraPresenter
from __future__ import annotations

import ctypes
import os
import queue
import sys
import threading
import time
from ctypes import wintypes
from typing import Any, Callable, Dict, List, Optional

try:
    from utils.perf_probe import phase as _phase_trace  # type: ignore
    from utils.perf_probe import gauge as _perf_gauge  # type: ignore
except Exception:  # pragma: no cover
    def _phase_trace(_name: str, _detail: str = '') -> None:  # type: ignore
        return
    def _perf_gauge(_name: str, _value: float) -> None:  # type: ignore
        return


# Set to True from popup so the next pump tick emits per-line markers.
# Auto-resets after one tick. Kept module-global so it can be toggled
# without touching any class instance.
_trace_pump_armed = False
_trace_pump_remaining_ticks = 0


def arm_pump_trace() -> None:
    # Arm one-shot fine-grained tracing for the next pump tick.
    global _trace_pump_armed, _trace_pump_remaining_ticks
    _trace_pump_armed = True
    _trace_pump_remaining_ticks = 8


# Lazy imports — keep module import cost zero when GPU overlay disabled.
_moderngl = None  # type: ignore[assignment]
_import_error: Optional[str] = None


def _try_imports() -> bool:
    # Import moderngl on first use. Returns True on success.
    global _moderngl, _import_error
    if _moderngl is not None:
        return True
    if _import_error is not None:
        return False
    try:
        import moderngl as _m  # type: ignore[import-not-found]
    except Exception as exc:
        _import_error = f'{type(exc).__name__}: {exc}'
        return False
    _moderngl = _m
    return True


def glfw_supported() -> bool:
    # True if GPU overlay path is available.
    try:
        from config import USE_GPU_OVERLAY
        if not USE_GPU_OVERLAY:
            return False
    except Exception:
        pass
    if sys.platform != 'win32':
        return False
    if not _try_imports():
        return False
    if _UNIFIED_OVERLAY_MODE:
        return True
    # Compositor not yet toggled on — still starting up.
    # Check if prestart is in progress (instance exists or will exist).
    return _unified_overlay_instance is not None


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
    _user32.ShowWindow.restype = wintypes.BOOL
    _user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]

    _dwmapi = ctypes.WinDLL('dwmapi', use_last_error=True)
    _gdi32 = ctypes.WinDLL('gdi32', use_last_error=True)

    class _MARGINS(ctypes.Structure):
        _fields_ = [
            ('cxLeftWidth', ctypes.c_int),
            ('cxRightWidth', ctypes.c_int),
            ('cyTopHeight', ctypes.c_int),
            ('cyBottomHeight', ctypes.c_int),
        ]

    class _DWM_BLURBEHIND(ctypes.Structure):
        _fields_ = [
            ('dwFlags', wintypes.DWORD),
            ('fEnable', wintypes.BOOL),
            ('hRgnBlur', wintypes.HANDLE),
            ('fTransitionOnMaximized', wintypes.BOOL),
        ]

SW_HIDE = 0
SW_SHOWNOACTIVATE = 4


def _reassert_topmost(hwnd: int) -> None:
    # Re-apply HWND_TOPMOST via SetWindowPos.
    if sys.platform != 'win32':
        return
    _user32.SetWindowPos(
        hwnd, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)


def _apply_click_through(hwnd: int) -> None:
    # Set WS_EX_TRANSPARENT + TOPMOST for mouse pass-through.
    _apply_click_through_exstyle(hwnd)
    _reassert_topmost(hwnd)


def _apply_click_through_exstyle(hwnd: int) -> None:
    if sys.platform != 'win32':
        return
    cur = _user32.GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
    new = (cur | WS_EX_LAYERED | WS_EX_TRANSPARENT
           | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE)
    _user32.SetWindowLongPtrW(hwnd, GWL_EXSTYLE, new)
    _user32.SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)


def _apply_interactive(hwnd: int) -> None:
    # Remove WS_EX_TRANSPARENT so the window receives input.
    _apply_interactive_exstyle(hwnd)
    _reassert_topmost(hwnd)


def _apply_interactive_exstyle(hwnd: int) -> None:
    if sys.platform != 'win32':
        return
    cur = _user32.GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
    new = ((cur | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST)
           & ~WS_EX_TRANSPARENT & ~WS_EX_NOACTIVATE)
    _user32.SetWindowLongPtrW(hwnd, GWL_EXSTYLE, new)
    _user32.SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)


def _apply_dwm_transparency(hwnd: int) -> None:
    # Enable DWM per-pixel alpha for overlay windows.
    #
    # Three escalation levels, all applied unconditionally (no vendor
    # branching — harmless on NVIDIA, required on AMD/Intel):
    # L1  DwmExtendFrameIntoClientArea  (belt-and-suspenders)
    # L2  DwmEnableBlurBehindWindow     (required by AMD/Intel for GL alpha)
    if sys.platform != 'win32':
        return
    # L1
    try:
        margins = _MARGINS(-1, -1, -1, -1)
        _dwmapi.DwmExtendFrameIntoClientArea(hwnd, ctypes.byref(margins))
    except Exception:
        pass
    # L2
    try:
        _DWM_BB_ENABLE = 0x01
        _DWM_BB_BLURREGION = 0x02
        bb = _DWM_BLURBEHIND()
        bb.dwFlags = _DWM_BB_ENABLE | _DWM_BB_BLURREGION
        bb.fEnable = True
        bb.hRgnBlur = _gdi32.CreateRectRgn(0, 0, -1, -1)
        _dwmapi.DwmEnableBlurBehindWindow(hwnd, ctypes.byref(bb))
        if bb.hRgnBlur:
            _gdi32.DeleteObject(bb.hRgnBlur)
    except Exception:
        pass


def _show_no_activate(hwnd: int) -> None:
    if sys.platform != 'win32' or not hwnd:
        return
    _user32.ShowWindow(hwnd, SW_SHOWNOACTIVATE)
    _user32.SetWindowPos(
        hwnd, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)


_overlay_creation_lock = threading.Lock()
_overlay_creation_suspended = 0


def suspend_gpu_overlay_creation() -> None:
    # Temporarily block new overlay windows from being created.
    global _overlay_creation_suspended
    with _overlay_creation_lock:
        _overlay_creation_suspended += 1


def resume_gpu_overlay_creation() -> None:
    # Release one startup-time creation block.
    global _overlay_creation_suspended
    with _overlay_creation_lock:
        if _overlay_creation_suspended > 0:
            _overlay_creation_suspended -= 1


def gpu_overlay_creation_allowed() -> bool:
    # True when new overlay windows may be created.
    with _overlay_creation_lock:
        return _overlay_creation_suspended <= 0


# ── Pump stub (replaces legacy GlfwPump) ───────────────────────────────────
# Callers still call get_glfw_pump(root) before creating GpuOverlayWindow.
# This minimal stub satisfies that contract without any GLFW dependency.
class _PumpStub:
    def __init__(self, root: Any):
        self._root = root
        self._tick_hz = 60

    def exec_on_pump(self, fn: Callable[[], Any], timeout: float = 5.0) -> Any:
        # In compositor mode all work goes through the compositor thread,
        # not a pump thread. Just run inline.
        return fn()

    def post_cmd(self, fn: Callable[[], Any]) -> None:
        try:
            fn()
        except Exception:
            pass

    def post_to_tk(self, fn: Callable[[], Any]) -> None:
        try:
            self._root.after(0, fn)
        except Exception:
            try:
                fn()
            except Exception:
                pass

    def register(self, win: Any) -> None:
        pass

    def unregister(self, win: Any) -> None:
        pass

    def kick_redraw(self) -> None:
        pass

    def shutdown(self) -> None:
        pass


# For backward compat: GlfwPump name still referenced in type hints/comments.
GlfwPump = _PumpStub

_pump_lock = threading.Lock()
_pump: Optional[_PumpStub] = None

# ── WGL driver serialization ────────────────────────────────────────────────
# Process-wide RLock that any thread doing native WGL/moderngl work must
# acquire briefly. Required to keep worker threads with their own moderngl
# context (e.g. fisheye standalone context, gpu_renderer worker GL blocks)
# from racing the compositor's WGL ops.
_wgl_serialize_lock = threading.RLock()


def get_wgl_serialize_lock() -> threading.RLock:
    # Return the process-wide WGL/moderngl serialization RLock.
    return _wgl_serialize_lock


def get_glfw_pump(root: Any) -> _PumpStub:
    # Singleton pump stub bound to the Tk root.
    global _pump
    with _pump_lock:
        if _pump is None:
            _pump = _PumpStub(root)
        return _pump


# ── Unified overlay mode toggle ─────────────────────────────────────────────
# When True, GpuOverlayWindow delegates to a CompositorLayer in the
# single unified overlay window (single HWND, no per-panel GLFW windows).
_UNIFIED_OVERLAY_MODE = False
_unified_overlay_instance = None
_unified_overlay_lock = threading.Lock()


def set_unified_overlay_mode(enabled: bool) -> None:
    # Enable/disable unified overlay compositor for all new windows.
    global _UNIFIED_OVERLAY_MODE
    _UNIFIED_OVERLAY_MODE = bool(enabled)


def get_unified_overlay_mode() -> bool:
    return _UNIFIED_OVERLAY_MODE


def _get_unified_overlay(root: Any = None):
    global _unified_overlay_instance
    from render.overlay_compositor import get_unified_overlay
    uo = get_unified_overlay(root)
    _unified_overlay_instance = uo  # keep in sync for direct readers
    if not uo._running:
        uo.start()
    return uo


def prestart_unified_overlay(root: Any = None) -> None:
    # Synchronously initialize the compositor. Blocks until it is fully ready.
    # 契约: 起不来直接 raise; 不再降级到 Tk/Canvas. 主人要求"没 compositor
    # 直接退出", 由 sao_gui 顶层 catch → sys.exit.
    #
    # 旧版把 _init 扔到后台 daemon thread 立刻返回, 主线程继续走 __init__,
    # 结果后续消费者 (LinkStart/NerveGear) 建 GpuOverlayWindow 时 compositor
    # 还没 ready → 各自 wait_ready(8s) 超时 → 走 Tk/Canvas 降级. 上一轮实测
    # HWND=0x000409C2 那次日志里两条 "compositor not ready after 8s" 就是这
    # 个症状 (rt_io driver load 拉长了 compositor init 首轮).
    global _UNIFIED_OVERLAY_MODE, _unified_overlay_instance
    if not _UNIFIED_OVERLAY_MODE:
        return
    max_attempts = 3
    last_exc: Optional[Exception] = None
    for attempt in range(1, max_attempts + 1):
        try:
            uo = _get_unified_overlay(root)
            if not uo.wait_ready(timeout=10.0):
                raise RuntimeError('compositor did not become ready in 10s')
            print('[Overlay] compositor ready', flush=True)
            return
        except Exception as exc:
            last_exc = exc
            print(f'[Overlay] compositor init attempt {attempt}/{max_attempts} '
                  f'failed: {exc}', flush=True)
            if attempt < max_attempts:
                import time as _t
                _t.sleep(2.0)
    # All attempts exhausted — drop the singleton only if teardown is proven.
    # A live host reference is more important than making a later retry look
    # clean: clearing it here could let a second compositor start.
    cleanup_confirmed = False
    try:
        from render import overlay_compositor as _oc
        cleanup_confirmed = bool(_oc.reset_unified_overlay())
    except Exception:
        cleanup_confirmed = False
    if cleanup_confirmed:
        _unified_overlay_instance = None
        _UNIFIED_OVERLAY_MODE = False
    raise RuntimeError(
        f'compositor failed after {max_attempts} attempts: {last_exc}')


# ── GpuOverlayWindow ────────────────────────────────────────────────────────
class GpuOverlayWindow:
    # Overlay window delegating to the unified DWM compositor.
    #
    # Lifecycle:
    #   ow = GpuOverlayWindow(pump, w=800, h=200, x=100, y=100)
    #   ow.set_render_fn(lambda ctx, t: ...)
    #   ow.show()
    #   ...
    #   ow.set_geometry(x, y, w, h)
    #   ow.hide()
    #   ow.destroy()

    def __init__(self, pump: Any, w: int, h: int,
                 x: int = 100, y: int = 100,
                 render_fn: Optional[Callable[[Any, float], None]] = None,
                 click_through: bool = True,
                 title: str = 'sao_overlay',
                 vsync: bool = False):
        self._unified = _UNIFIED_OVERLAY_MODE
        self._delegate = None
        self._pump = pump
        self._root = getattr(pump, '_root', None)
        self._w = max(1, int(w))
        self._h = max(1, int(h))
        self._x = int(x)
        self._y = int(y)
        self._click_through = bool(click_through)
        self._title = title
        self._visible = False
        self._created = True
        self._shown = False
        self._show_pending = False
        self._dirty = False
        self._hwnd = 0
        self._ctx = None
        self._win = None
        self._render_fn = render_fn
        self._vsync = vsync
        # Compositor delegation
        if self._unified:
            try:
                uo = _get_unified_overlay(self._root)
                if not uo._ready.wait(timeout=8.0):
                    raise RuntimeError('compositor not ready after 8s')
                from render.overlay_adapter import CompositorOverlayWindow
                _tl = title.lower()
                if 'linkstart' in _tl or 'transition' in _tl:
                    _z = 500
                elif 'fisheye' in _tl:
                    _z = 10
                elif 'popup' in _tl:
                    _z = 80
                elif 'menu_bar' in _tl or 'child_bar' in _tl:
                    _z = 85
                elif 'menu_hud' in _tl or 'left_info' in _tl:
                    _z = 90
                elif 'nervegear' in _tl or 'float' in _tl:
                    _z = 200
                elif any(k in _tl for k in ('dps', 'hp', 'boss', 'buff', 'skill', 'player', 'session')):
                    _z = 150
                else:
                    _z = 100
                self._delegate = CompositorOverlayWindow(
                    uo, w=w, h=h, x=x, y=y,
                    render_fn=render_fn, click_through=click_through,
                    title=title, vsync=vsync, z=_z,
                )
                self._hwnd = uo.hwnd
                self._ctx = uo.host.ctx if uo.host else None
            except Exception as exc:
                print(f'[GOW] unified delegation failed for {title}: {exc}',
                      flush=True)
                raise

    # ---- properties ----

    @property
    def ctx(self) -> Any:
        if self._delegate is not None:
            return getattr(self._delegate, 'ctx', self._ctx)
        return self._ctx

    # ---- lifecycle ----

    def _create(self) -> None:
        pass  # always "created" in compositor mode

    def prepare_async(self) -> bool:
        return True

    def show(self, async_create: bool = False) -> None:
        self._visible = True
        self._shown = True
        if self._delegate:
            self._delegate.show(async_create)

    def hide(self) -> None:
        self._visible = False
        if self._delegate:
            self._delegate.hide()

    def destroy(self) -> None:
        self._visible = False
        if self._delegate:
            self._delegate.destroy()

    # ---- mutators ----

    def set_geometry(self, x: int, y: int, w: int, h: int) -> None:
        self._x, self._y = int(x), int(y)
        self._w, self._h = max(1, int(w)), max(1, int(h))
        if self._delegate:
            self._delegate.set_geometry(self._x, self._y, self._w, self._h)

    def set_click_through(self, click_through: bool) -> None:
        self._click_through = bool(click_through)
        if self._delegate:
            self._delegate.set_click_through(click_through)

    def set_render_fn(self, fn: Callable[[Any, float], None]) -> None:
        self._render_fn = fn
        if self._delegate:
            self._delegate.set_render_fn(fn)

    def set_input_callbacks(
            self,
            cursor_pos_fn: Optional[Callable[[float, float], None]] = None,
            cursor_leave_fn: Optional[Callable[[], None]] = None,
            mouse_button_fn: Optional[
                Callable[[int, int, int, float, float], None]] = None,
            scroll_fn: Optional[Callable[[float, float], None]] = None) -> None:
        if self._delegate:
            self._delegate.set_input_callbacks(
                cursor_pos_fn, cursor_leave_fn,
                mouse_button_fn, scroll_fn,
            )

    def enable_input_proxy(self) -> bool:
        if self._delegate is not None:
            try:
                return self._delegate.enable_input_proxy()
            except Exception:
                return False
        return False

    def request_redraw(self) -> None:
        # Thread-safe: mark dirty so compositor draws.
        self._dirty = True
        if self._delegate:
            self._delegate.request_redraw()


# ── BgraPresenter ───────────────────────────────────────────────────────────
# Helper that turns a GpuOverlayWindow into a "present already-composed
# BGRA bytes" surface. The compose worker still produces premultiplied
# BGRA bytes (same as the ULW path); we just upload them to a moderngl
# texture and draw a fullscreen quad.
#
# Texture format note: moderngl's RGBA8 texture sampled as ``texture(...)``
# returns the bytes in (R, G, B, A) order. Our worker emits BGRA, so the
# fragment shader swizzles ``c.bgra`` to recover (R, G, B, A).

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
uniform float u_alpha;
void main() {
    vec4 c = texture(u_tex, v_uv);
    // Bytes packed as BGRA — swizzle to (R, G, B, A). Already premult.
    // u_alpha (default 1.0) lets callers fade the whole frame without
    // re-premultiplying the BGRA bytes on every frame.
    fragColor = c.bgra * u_alpha;
}
"""


class BgraPresenter:
    # Owns the texture+VAO+program needed to blit a BGRA-bytes buffer
    # onto a GpuOverlayWindow as a fullscreen quad.
    #
    # Usage:
    #   presenter = BgraPresenter()
    #   win = GpuOverlayWindow(pump, w, h, x, y,
    #                          render_fn=presenter.render,
    #                          click_through=True)
    #   win.show()
    #   ...
    #   # Each frame:
    #   presenter.set_frame(bgra_bytes, w, h)
    #   win.request_redraw()

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
        self._alpha = 1.0
        # Dedup glTexSubImage2D when alpha-only ticks trigger a redraw
        # without changing frame content. The staged frame travels as one
        # (bytes, w, h, seq) tuple so the render thread never sees a
        # half-updated combination.
        self._frame_snap: Optional[tuple] = None
        self._frame_seq = 0
        self._uploaded_seq = -1
        # Pump-driven time-based fade. Eliminates Tk after(16) for fade
        # animation — the compositor pump drives alpha smoothly even when
        # main thread is busy.
        self._fade_active = False
        self._fade_t0 = 0.0
        self._fade_dur = 0.0
        self._fade_from = 1.0
        self._fade_to = 1.0
        self._fade_done_cb: Optional[Callable[[], None]] = None

    def start_fade(self, target_alpha: float, duration_s: float,
                   on_done: Optional[Callable[[], None]] = None) -> None:
        # Begin a time-based fade from current alpha to ``target_alpha``
        # over ``duration_s``. Cancels any previous fade.
        self._fade_from = float(self._alpha)
        self._fade_to = max(0.0, min(1.0, float(target_alpha)))
        self._fade_dur = max(0.001, float(duration_s))
        self._fade_t0 = time.perf_counter()
        self._fade_active = True
        self._fade_done_cb = on_done
        self._dirty = True

    def is_fading(self) -> bool:
        return self._fade_active

    def set_alpha(self, alpha: float) -> None:
        # Global alpha multiplier (0..1) applied during render.
        self._fade_active = False  # cancel any in-flight fade
        self._alpha = max(0.0, min(1.0, float(alpha)))

    def set_frame(self, bgra: bytes, w: int, h: int) -> None:
        # Stage a frame for the next render. Cheap (just stores refs).
        if bgra is not self._frame_bytes:
            self._frame_seq += 1
        self._frame_bytes = bgra
        self._frame_w = int(w)
        self._frame_h = int(h)
        self._frame_snap = (bgra, int(w), int(h), self._frame_seq)
        self._dirty = True

    def clear(self) -> None:
        # Stage a transparent frame (drops the cached bytes).
        self._frame_bytes = None
        self._frame_snap = None
        self._dirty = True

    def render(self, ctx: Any, _t: float) -> None:
        # GpuOverlayWindow render_fn. Uploads + draws latest frame.
        if _moderngl is None:
            return
        # Pump-driven fade: compute alpha from elapsed time.
        if self._fade_active:
            elapsed = time.perf_counter() - self._fade_t0
            if elapsed >= self._fade_dur:
                self._alpha = self._fade_to
                self._fade_active = False
                cb = self._fade_done_cb
                self._fade_done_cb = None
                if cb is not None:
                    try:
                        cb()
                    except Exception:
                        pass
            else:
                k = elapsed / self._fade_dur
                self._alpha = self._fade_from + (self._fade_to - self._fade_from) * k
                self._dirty = True
        snap = self._frame_snap
        bgra, w, h, seq = snap if snap is not None else (None, 0, 0, -1)
        if self._prog is None:
            self._prog = ctx.program(
                vertex_shader=_BGRA_VS, fragment_shader=_BGRA_FS)
            self._prog['u_tex'].value = 0
            try:
                self._prog['u_alpha'].value = 1.0
            except Exception:
                pass
            import numpy as _np
            quad = _np.array([-1, -1, 1, -1, -1, 1, 1, 1], dtype='f4')
            self._vbo = ctx.buffer(quad.tobytes())
            self._vao = ctx.vertex_array(
                self._prog, [(self._vbo, '2f', 'in_pos')])
        try:
            self._prog['u_alpha'].value = float(self._alpha)
        except Exception:
            pass
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
                self._uploaded_seq = seq
            else:
                if seq != getattr(self, '_uploaded_seq', -1):
                    try:
                        self._tex.write(bgra)
                        self._uploaded_seq = seq
                    except Exception:
                        try:
                            self._tex.release()
                        except Exception:
                            pass
                        self._tex = ctx.texture((w, h), 4, bgra)
                        self._tex_w = w
                        self._tex_h = h
                        self._uploaded_seq = seq
            self._tex.use(location=0)
            self._vao.render(_moderngl.TRIANGLE_STRIP)  # type: ignore[union-attr]
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
        self._frame_snap = None
