# -*- coding: utf-8 -*-
"""overlay_compositor — virtual layer system for the unified overlay.

Each UI element (DPS panel, menu, fisheye, float button, etc.)
registers as a CompositorLayer. The UnifiedOverlay manages a
dedicated render thread that:
  1. Processes Win32 messages for the single overlay window
  2. Composites all visible layers in z-order
  3. Presents the result via SwapBuffers

Layers upload BGRA bytes (same as BgraPresenter) or render to an
FBO via a callback. The master compositor draws all layer textures
onto the full-screen window.
"""
from __future__ import annotations

import queue
import threading
import time
from typing import Any, Callable, Dict, List, Optional, Tuple

import moderngl

import ctypes as _ct
import ctypes.wintypes as _wt

from render.overlay_host import (
    OverlayHost,
    WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP,
    WM_RBUTTONDOWN, WM_RBUTTONUP, WM_MOUSEWHEEL, WM_MOUSELEAVE,
)

# MsgWaitForMultipleObjects: sleep while still pumping Win32 messages.
# Without this, WM_NCHITTEST blocks ALL mouse input during sleep.
_QS_ALLINPUT = 0x04FF
_WAIT_OBJECT_0 = 0
_WAIT_TIMEOUT = 0x00000102
try:
    _MsgWait = _ct.windll.user32.MsgWaitForMultipleObjectsEx
    _MsgWait.restype = _wt.DWORD
    _MsgWait.argtypes = [
        _wt.DWORD, _ct.c_void_p, _wt.DWORD, _wt.DWORD, _wt.DWORD,
    ]
    _MSG_WAIT_OK = True
except Exception:
    _MSG_WAIT_OK = False


def _msg_wait_sleep(host: OverlayHost, seconds: float,
                    stop_evt: threading.Event) -> None:
    """Sleep for up to `seconds` while remaining responsive to Win32
    messages. Wakes early if stop_evt is set or a message arrives."""
    if not _MSG_WAIT_OK:
        # Fallback: short sleeps with message pumping
        deadline = time.perf_counter() + seconds
        while time.perf_counter() < deadline and not stop_evt.is_set():
            host.process_messages()
            stop_evt.wait(timeout=0.004)
        return

    deadline = time.perf_counter() + seconds
    while not stop_evt.is_set():
        remaining = deadline - time.perf_counter()
        if remaining <= 0:
            break
        ms = max(1, min(int(remaining * 1000), 100))
        # Wait for message arrival or timeout (no handles, just messages)
        _MsgWait(0, None, ms, _QS_ALLINPUT, 0)
        host.process_messages()


# ── Shader sources ───────────────────────────────────────────────
_VERT_SRC = '''
#version 330
in vec2 in_pos;
in vec2 in_uv;
out vec2 v_uv;
uniform vec4 u_rect;   // (x, y, w, h) in NDC
void main() {
    vec2 p = u_rect.xy + in_pos * u_rect.zw;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    v_uv = in_uv;
}
'''

_FRAG_SRC = '''
#version 330
uniform sampler2D u_tex;
uniform float u_alpha;
in vec2 v_uv;
out vec4 fragColor;
void main() {
    vec4 c = texture(u_tex, v_uv);
    // Input is premultiplied BGRA; swizzle to RGBA
    fragColor = c.bgra * u_alpha;
}
'''

# For layers that upload raw RGBA (no swizzle needed)
_FRAG_RGBA_SRC = '''
#version 330
uniform sampler2D u_tex;
uniform float u_alpha;
in vec2 v_uv;
out vec4 fragColor;
void main() {
    vec4 c = texture(u_tex, v_uv);
    fragColor = c * u_alpha;
}
'''


# ── CompositorLayer ──────────────────────────────────────────────
class CompositorLayer:
    """A virtual window in the unified overlay compositor.

    Each layer has a position, size, z-order, and visibility state.
    Content is updated by calling upload_bgra() with premultiplied
    BGRA bytes, or by setting a render callback.
    """

    def __init__(self, name: str, width: int, height: int,
                 x: int = 0, y: int = 0, z: int = 0,
                 click_through: bool = True,
                 bgra_swizzle: bool = True):
        self.name = name
        self.x = x
        self.y = y
        self.width = max(1, width)
        self.height = max(1, height)
        self.z_order = z
        self.visible = False
        self.alpha = 1.0
        self.click_through = click_through
        self.bgra_swizzle = bgra_swizzle

        # GL resources (created lazily on render thread)
        self._texture: Optional[moderngl.Texture] = None
        self._tex_w = 0
        self._tex_h = 0

        # Frame data (set from any thread, consumed on render thread)
        self._frame_bytes: Optional[bytes] = None
        self._frame_w = 0
        self._frame_h = 0
        self._frame_seq = 0
        self._uploaded_seq = -1
        self._lock = threading.Lock()

        # Optional render-to-FBO callback
        self._render_fn: Optional[
            Callable[[moderngl.Context, float], None]
        ] = None
        self._fbo: Optional[moderngl.Framebuffer] = None
        self._fbo_tex: Optional[moderngl.Texture] = None

        # Dirty flag
        self._dirty = False

        # Tk input proxy (invisible Tk window for mouse events)
        self._input_proxy = None

        # Mouse event callbacks
        self._on_cursor_pos: Optional[
            Callable[[float, float], None]
        ] = None
        self._on_cursor_leave: Optional[Callable[[], None]] = None
        self._on_mouse_button: Optional[
            Callable[[int, int, int, float, float], None]
        ] = None
        self._on_scroll: Optional[
            Callable[[float, float], None]
        ] = None

        # Fade support
        self._fade_active = False
        self._fade_t0 = 0.0
        self._fade_from = 1.0
        self._fade_to = 1.0
        self._fade_dur = 0.3
        self._fade_done_fn: Optional[Callable[[], None]] = None

    def upload_bgra(self, bgra: bytes, w: int, h: int) -> None:
        """Upload premultiplied BGRA frame data (thread-safe)."""
        with self._lock:
            self._frame_bytes = bgra
            self._frame_w = w
            self._frame_h = h
            self._frame_seq += 1
            self._dirty = True

    def set_render_fn(
        self, fn: Optional[Callable[[moderngl.Context, float], None]],
    ) -> None:
        """Set a render callback for FBO-based layers."""
        self._render_fn = fn
        self._dirty = True

    def set_geometry(self, x: int, y: int, w: int, h: int) -> None:
        self.x = x
        self.y = y
        self.width = max(1, w)
        self.height = max(1, h)
        self._dirty = True

    def set_position(self, x: int, y: int) -> None:
        self.x = x
        self.y = y

    def show(self) -> None:
        self.visible = True
        self._dirty = True

    def hide(self) -> None:
        self.visible = False

    def start_fade(self, target: float, duration: float = 0.3,
                   done_fn: Optional[Callable[[], None]] = None) -> None:
        self._fade_active = True
        self._fade_t0 = time.perf_counter()
        self._fade_from = self.alpha
        self._fade_to = target
        self._fade_dur = max(0.001, duration)
        self._fade_done_fn = done_fn
        self._dirty = True

    def set_input_callbacks(
        self,
        cursor_pos_fn: Optional[Callable[[float, float], None]] = None,
        cursor_leave_fn: Optional[Callable[[], None]] = None,
        mouse_button_fn: Optional[
            Callable[[int, int, int, float, float], None]
        ] = None,
        scroll_fn: Optional[Callable[[float, float], None]] = None,
    ) -> None:
        """Set mouse event callbacks (same signature as GpuOverlayWindow)."""
        self._on_cursor_pos = cursor_pos_fn
        self._on_cursor_leave = cursor_leave_fn
        self._on_mouse_button = mouse_button_fn
        self._on_scroll = scroll_fn

    # ── Tk input proxy ───────────────────────────────────

    def create_input_proxy(self, root) -> None:
        """Create an invisible Tk window at this layer's position to
        receive mouse events. The compositor window is always
        WS_EX_TRANSPARENT (click-through); interactive layers use
        these Tk proxies for input instead.

        Must be called from the Tk main thread.
        """
        if self.click_through or self._input_proxy is not None:
            return
        import tkinter as _tk
        proxy = _tk.Toplevel(root)
        proxy.overrideredirect(True)
        proxy.attributes('-topmost', True)
        proxy.attributes('-alpha', 0.01)
        proxy.geometry(f'{self.width}x{self.height}'
                       f'+{self.x}+{self.y}')
        proxy.configure(bg='black')

        def _pos(e):
            if self._on_cursor_pos:
                self._on_cursor_pos(float(e.x), float(e.y))

        def _leave(e):
            if self._on_cursor_leave:
                self._on_cursor_leave()

        def _press(e):
            if self._on_mouse_button:
                btn = 0 if e.num == 1 else (1 if e.num == 3 else 2)
                self._on_mouse_button(btn, 1, 0,
                                      float(e.x), float(e.y))

        def _release(e):
            if self._on_mouse_button:
                btn = 0 if e.num == 1 else (1 if e.num == 3 else 2)
                self._on_mouse_button(btn, 0, 0,
                                      float(e.x), float(e.y))

        def _scroll(e):
            if self._on_scroll:
                dy = 1.0 if e.delta > 0 else -1.0
                self._on_scroll(0.0, dy)

        proxy.bind('<Motion>', _pos)
        proxy.bind('<Leave>', _leave)
        proxy.bind('<ButtonPress-1>', _press)
        proxy.bind('<ButtonRelease-1>', _release)
        proxy.bind('<ButtonPress-3>', _press)
        proxy.bind('<ButtonRelease-3>', _release)
        proxy.bind('<MouseWheel>', _scroll)
        self._input_proxy = proxy

    def sync_input_proxy(self) -> None:
        """Update the input proxy position/size/visibility."""
        proxy = self._input_proxy
        if proxy is None:
            return
        try:
            if self.visible and not self.click_through:
                proxy.geometry(f'{self.width}x{self.height}'
                               f'+{self.x}+{self.y}')
                proxy.deiconify()
                proxy.attributes('-topmost', True)
                proxy.lift()
            else:
                proxy.withdraw()
        except Exception:
            pass

    def destroy_input_proxy(self) -> None:
        proxy = self._input_proxy
        self._input_proxy = None
        if proxy is not None:
            try:
                proxy.destroy()
            except Exception:
                pass

    def request_redraw(self) -> None:
        self._dirty = True

    def hit_test(self, sx: int, sy: int) -> bool:
        """Check if screen point (sx, sy) is inside this layer."""
        if not self.visible or self.click_through:
            return False
        return (self.x <= sx < self.x + self.width
                and self.y <= sy < self.y + self.height)

    # ── GL resource management (render thread only) ──────────

    def _ensure_texture(self, ctx: moderngl.Context) -> None:
        with self._lock:
            seq = self._frame_seq
            w = self._frame_w
            h = self._frame_h
            data = self._frame_bytes

        if seq == self._uploaded_seq:
            return
        if not data or w <= 0 or h <= 0:
            return

        if (self._texture is None
                or self._tex_w != w or self._tex_h != h):
            if self._texture is not None:
                self._texture.release()
            self._texture = ctx.texture((w, h), 4, data=data)
            self._texture.filter = (moderngl.LINEAR, moderngl.LINEAR)
            self._tex_w = w
            self._tex_h = h
        else:
            self._texture.write(data)
        self._uploaded_seq = seq

    def _ensure_fbo(self, ctx: moderngl.Context) -> None:
        w, h = self.width, self.height
        if (self._fbo is not None
                and self._fbo_tex.width == w
                and self._fbo_tex.height == h):
            return
        if self._fbo is not None:
            self._fbo.release()
            self._fbo_tex.release()
        self._fbo_tex = ctx.texture((w, h), 4)
        self._fbo_tex.filter = (moderngl.LINEAR, moderngl.LINEAR)
        self._fbo = ctx.framebuffer(color_attachments=[self._fbo_tex])

    def _tick_fade(self) -> None:
        if not self._fade_active:
            return
        elapsed = time.perf_counter() - self._fade_t0
        t = min(1.0, elapsed / self._fade_dur)
        self.alpha = self._fade_from + (self._fade_to - self._fade_from) * t
        if t >= 1.0:
            self._fade_active = False
            self.alpha = self._fade_to
            if self._fade_done_fn:
                try:
                    self._fade_done_fn()
                except Exception:
                    pass
                self._fade_done_fn = None
        self._dirty = True

    def _release_gl(self) -> None:
        if self._texture is not None:
            self._texture.release()
            self._texture = None
        if self._fbo is not None:
            self._fbo.release()
            self._fbo_tex.release()
            self._fbo = None
            self._fbo_tex = None


# ── Mouse event types ────────────────────────────────────────────
MOUSE_MOVE = 'move'
MOUSE_PRESS = 'press'
MOUSE_RELEASE = 'release'
MOUSE_SCROLL = 'scroll'
MOUSE_LEAVE = 'leave'


# ── UnifiedOverlay ───────────────────────────────────────────────
class UnifiedOverlay:
    """Manages the single overlay window and composites all layers.

    Usage:
        overlay = UnifiedOverlay()
        overlay.start()

        layer = overlay.create_layer('dps', w=300, h=400, x=100, y=200, z=200)
        layer.upload_bgra(bgra_bytes, 300, 400)
        layer.show()

        # ... later
        overlay.stop()
    """

    def __init__(self, root: Any = None):
        self._root = root
        self._host: Optional[OverlayHost] = None
        self._layers: Dict[str, CompositorLayer] = {}
        self._z_sorted: List[CompositorLayer] = []
        self._lock = threading.RLock()
        self._thread: Optional[threading.Thread] = None
        self._running = False
        self._stop_evt = threading.Event()

        # GL resources for compositing
        self._bgra_prog: Optional[moderngl.Program] = None
        self._rgba_prog: Optional[moderngl.Program] = None
        self._quad_vao_bgra: Optional[moderngl.VertexArray] = None
        self._quad_vao_rgba: Optional[moderngl.VertexArray] = None

        # Command queue for cross-thread operations
        self._cmd_q: queue.Queue = queue.Queue()

        # Ready event: set when host window is created
        self._ready = threading.Event()

        # Topmost pulse interval
        self._topmost_interval = 2.0  # seconds
        self._last_topmost = 0.0

        # Tk callback queue (overlay thread → Tk main thread)
        self._tk_q: queue.Queue = queue.Queue()
        self._tk_poller_id: Any = None

        # Mouse state
        self._hover_layer: Optional[str] = None

        # Performance
        self._target_fps = 60
        self._frame_interval = 1.0 / self._target_fps

    # ── Layer management ─────────────────────────────────────

    def create_layer(self, name: str, width: int = 1, height: int = 1,
                     x: int = 0, y: int = 0, z: int = 0,
                     click_through: bool = True,
                     bgra_swizzle: bool = True) -> CompositorLayer:
        layer = CompositorLayer(
            name, width, height, x, y, z, click_through, bgra_swizzle,
        )
        with self._lock:
            self._layers[name] = layer
            self._rebuild_z_order()
        return layer

    def destroy_layer(self, name: str) -> None:
        with self._lock:
            layer = self._layers.pop(name, None)
            if layer:
                self._rebuild_z_order()

        def _release():
            if layer:
                layer._release_gl()
        self._cmd_q.put(_release)

    def get_layer(self, name: str) -> Optional[CompositorLayer]:
        return self._layers.get(name)

    def set_layer_z(self, name: str, z: int) -> None:
        with self._lock:
            layer = self._layers.get(name)
            if layer:
                layer.z_order = z
                self._rebuild_z_order()

    def raise_layer(self, name: str) -> None:
        with self._lock:
            if not self._z_sorted:
                return
            max_z = max(l.z_order for l in self._z_sorted)
            layer = self._layers.get(name)
            if layer:
                layer.z_order = max_z + 1
                self._rebuild_z_order()

    def _rebuild_z_order(self) -> None:
        self._z_sorted = sorted(
            self._layers.values(), key=lambda l: l.z_order,
        )

    def lift_all_input_proxies(self) -> None:
        """Re-lift all input proxies in z-order so higher-z layers
        receive clicks above lower-z layers (e.g., popup above fisheye)."""
        for layer in self._z_sorted:
            if (layer._input_proxy is not None
                    and layer.visible and not layer.click_through):
                try:
                    layer._input_proxy.lift()
                except Exception:
                    pass

    # ── Lifecycle ────────────────────────────────────────────

    def start(self) -> None:
        if self._running:
            return
        self._running = True
        self._stop_evt.clear()
        self._thread = threading.Thread(
            target=self._run, daemon=True,
        )
        self._thread.start()
        if self._root is not None:
            self._start_tk_poller()

    def stop(self) -> None:
        self._running = False
        self._stop_evt.set()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=5.0)
        self._thread = None
        if self._tk_poller_id is not None and self._root:
            try:
                self._root.after_cancel(self._tk_poller_id)
            except Exception:
                pass
            self._tk_poller_id = None

    def wait_ready(self, timeout: float = 5.0) -> bool:
        """Block until the overlay host is created. Returns True if ready."""
        return self._ready.wait(timeout=timeout)

    @property
    def host(self) -> Optional[OverlayHost]:
        return self._host

    @property
    def hwnd(self) -> int:
        return self._host.hwnd if self._host else 0

    # ── Streaming mode ───────────────────────────────────────

    def set_streaming_mode(self, exclude: bool) -> None:
        """Toggle capture exclusion (streaming mode).

        Primary path uses kernel-level tagWND physical memory write
        (bypasses anti-cheat API hooks).  Falls back to direct
        SetWindowDisplayAffinity if kernel engines are unavailable.
        """
        def _set():
            if self._host:
                self._host.set_capture_mode(exclude)
        self._cmd_q.put(_set)

    # ── Tk callback bridge ───────────────────────────────────

    def _start_tk_poller(self) -> None:
        def _drain():
            for _ in range(64):
                try:
                    fn = self._tk_q.get_nowait()
                    fn()
                except queue.Empty:
                    break
                except Exception:
                    pass
            self._tk_poller_id = self._root.after(8, _drain)
        self._tk_poller_id = self._root.after(8, _drain)

    def post_to_tk(self, fn: Callable[[], None]) -> None:
        self._tk_q.put(fn)

    # ── Hit testing ──────────────────────────────────────────

    def _hit_test(self, sx: int, sy: int) -> bool:
        """Screen-space hit test. Returns True if any interactive
        layer covers (sx, sy)."""
        for layer in reversed(self._z_sorted):
            if layer.hit_test(sx, sy):
                return True
        return False

    def _find_layer_at(self, sx: int, sy: int) -> Optional[CompositorLayer]:
        for layer in reversed(self._z_sorted):
            if layer.hit_test(sx, sy):
                return layer
        return None

    # ── Mouse event routing ────────────────────────────────

    def _on_mouse_event(self, msg: int, sx: int, sy: int,
                        button: int, delta: int) -> None:
        """Route Win32 mouse messages to the appropriate layer.

        Called on the overlay thread (from WndProc via DispatchMessage).
        Layer callbacks are posted to Tk main thread via post_to_tk().
        """
        if msg == WM_MOUSELEAVE:
            prev = self._hover_layer
            self._hover_layer = None
            if prev:
                layer = self._layers.get(prev)
                if layer and layer._on_cursor_leave:
                    fn = layer._on_cursor_leave
                    self.post_to_tk(fn)
            return

        target = self._find_layer_at(sx, sy)

        if msg == WM_MOUSEMOVE:
            tgt_name = target.name if target else None
            if tgt_name != self._hover_layer:
                if self._hover_layer:
                    old = self._layers.get(self._hover_layer)
                    if old and old._on_cursor_leave:
                        fn = old._on_cursor_leave
                        self.post_to_tk(fn)
                self._hover_layer = tgt_name
            if target and target._on_cursor_pos:
                lx = float(sx - target.x)
                ly = float(sy - target.y)
                fn = target._on_cursor_pos
                self.post_to_tk(lambda: fn(lx, ly))
            return

        if msg == WM_MOUSEWHEEL:
            if target and target._on_scroll:
                dx = 0.0
                dy = delta / 120.0
                fn = target._on_scroll
                self.post_to_tk(lambda: fn(dx, dy))
            return

        if msg in (WM_LBUTTONDOWN, WM_LBUTTONUP,
                   WM_RBUTTONDOWN, WM_RBUTTONUP):
            if target and target._on_mouse_button:
                action = 1 if msg in (WM_LBUTTONDOWN, WM_RBUTTONDOWN) else 0
                lx = float(sx - target.x)
                ly = float(sy - target.y)
                fn = target._on_mouse_button
                self.post_to_tk(
                    lambda: fn(button, action, 0, lx, ly))
            return

    # ── Overlay thread main loop ─────────────────────────────

    def _run(self) -> None:
        try:
            self._host = OverlayHost()
            self._host.create()
            self._host.hit_test_fn = self._hit_test
            self._host.mouse_fn = self._on_mouse_event
            self._init_gl()
            # Keep WGL context current for the entire thread lifetime.
            # Different threads' WGL contexts are independent — holding
            # ours doesn't block GLFW pump or render workers.
            self._host.show()
            self._host.set_capture_mode(True)
            self._ready.set()
        except Exception as exc:
            import traceback
            traceback.print_exc()
            self._running = False
            return

        t0 = time.perf_counter()
        while self._running:
            frame_start = time.perf_counter()

            # Drain command queue
            for _ in range(64):
                try:
                    cmd = self._cmd_q.get_nowait()
                    cmd()
                except queue.Empty:
                    break
                except Exception:
                    pass

            # Process Win32 messages
            self._host.process_messages()

            # Topmost pulse
            now = time.perf_counter()
            if now - self._last_topmost >= self._topmost_interval:
                self._host.raise_topmost()
                self._last_topmost = now

            # Tick layer fades + check if any layer needs rendering
            any_dirty = False
            for layer in self._z_sorted:
                layer._tick_fade()
                if layer._dirty or layer._fade_active:
                    any_dirty = True
                elif layer.visible and layer._render_fn is not None:
                    any_dirty = True

            if any_dirty:
                try:
                    self._render_frame(now - t0)
                    self._host.swap_buffers()
                except Exception:
                    pass

            # Adaptive sleep — idle longer when no layers are visible.
            # CRITICAL: must pump Win32 messages even while sleeping,
            # because WM_NCHITTEST is synchronous — Windows blocks ALL
            # mouse input until our WndProc returns HTTRANSPARENT.
            # Using Event.wait() would freeze mouse for the entire
            # sleep duration. Instead, use MsgWaitForMultipleObjects
            # which wakes on message arrival OR timeout.
            has_visible = any(l.visible for l in self._z_sorted)
            interval = self._frame_interval if has_visible else 0.05
            elapsed = time.perf_counter() - frame_start
            remaining = max(0.001, interval - elapsed)
            _msg_wait_sleep(self._host, remaining, self._stop_evt)
            if self._stop_evt.is_set():
                break

        # Cleanup
        self._cleanup_gl()
        self._host.destroy()
        self._host = None

    def _init_gl(self) -> None:
        """Initialize compositor GL resources on the overlay thread."""
        ctx = self._host.ctx

        # BGRA shader (most layers upload premultiplied BGRA)
        self._bgra_prog = ctx.program(
            vertex_shader=_VERT_SRC, fragment_shader=_FRAG_SRC,
        )
        # RGBA shader (for FBO-rendered layers)
        self._rgba_prog = ctx.program(
            vertex_shader=_VERT_SRC, fragment_shader=_FRAG_RGBA_SRC,
        )

        # Fullscreen quad vertices: pos(x,y) + uv(s,t)
        import struct

        # BGRA textures are top-down: UV y=0 = top of image
        verts_bgra = struct.pack(
            '16f',
            0.0, 0.0, 0.0, 1.0,  # GL bottom-left → UV bottom (y=1)
            1.0, 0.0, 1.0, 1.0,  # GL bottom-right
            0.0, 1.0, 0.0, 0.0,  # GL top-left → UV top (y=0)
            1.0, 1.0, 1.0, 0.0,  # GL top-right
        )
        vbo_bgra = ctx.buffer(verts_bgra)
        self._quad_vao_bgra = ctx.vertex_array(
            self._bgra_prog,
            [(vbo_bgra, '2f 2f', 'in_pos', 'in_uv')],
        )

        # FBO textures are bottom-up (GL convention): UV y=0 = bottom
        verts_fbo = struct.pack(
            '16f',
            0.0, 0.0, 0.0, 0.0,  # GL bottom-left → UV bottom-left
            1.0, 0.0, 1.0, 0.0,  # GL bottom-right
            0.0, 1.0, 0.0, 1.0,  # GL top-left → UV top-left
            1.0, 1.0, 1.0, 1.0,  # GL top-right
        )
        vbo_fbo = ctx.buffer(verts_fbo)
        self._quad_vao_rgba = ctx.vertex_array(
            self._rgba_prog,
            [(vbo_fbo, '2f 2f', 'in_pos', 'in_uv')],
        )

    def _render_frame(self, t: float) -> None:
        ctx = self._host.ctx
        sw = self._host.width
        sh = self._host.height
        ox = self._host.origin_x
        oy = self._host.origin_y

        ctx.screen.use()
        ctx.viewport = (0, 0, sw, sh)
        ctx.clear(0.0, 0.0, 0.0, 0.0)

        for layer in self._z_sorted:
            if not layer.visible or layer.alpha <= 0.001:
                layer._dirty = False
                continue

            # Render-fn layers: draw to FBO then composite
            if layer._render_fn is not None:
                layer._ensure_fbo(ctx)
                layer._fbo.use()
                ctx.viewport = (0, 0, layer.width, layer.height)
                ctx.clear(0.0, 0.0, 0.0, 0.0)
                try:
                    layer._render_fn(ctx, t)
                except Exception:
                    pass
                ctx.screen.use()
                ctx.viewport = (0, 0, sw, sh)
                tex = layer._fbo_tex
                prog = self._rgba_prog
                vao = self._quad_vao_rgba
            else:
                # BGRA upload layers
                layer._ensure_texture(ctx)
                if layer._texture is None:
                    layer._dirty = False
                    continue
                tex = layer._texture
                prog = self._bgra_prog
                vao = self._quad_vao_bgra

            # Compute NDC rect from screen coords
            rx = (layer.x - ox) / sw
            ry = 1.0 - (layer.y - oy + layer.height) / sh
            rw = layer.width / sw
            rh = layer.height / sh

            prog['u_rect'].value = (rx, ry, rw, rh)
            prog['u_alpha'].value = layer.alpha
            tex.use(location=0)
            vao.render(moderngl.TRIANGLE_STRIP)

            layer._dirty = False

    def _cleanup_gl(self) -> None:
        for layer in self._layers.values():
            layer._release_gl()
        # Programs and VAOs are released when context is destroyed


# ── Singleton ────────────────────────────────────────────────────
_overlay: Optional[UnifiedOverlay] = None
_overlay_lock = threading.Lock()


def get_unified_overlay(root: Any = None) -> UnifiedOverlay:
    """Get or create the singleton UnifiedOverlay."""
    global _overlay
    with _overlay_lock:
        if _overlay is None:
            _overlay = UnifiedOverlay(root)
        return _overlay
