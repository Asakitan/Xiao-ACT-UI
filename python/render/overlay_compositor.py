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
import struct as _struct
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

try:
    from _sao_cy_pixels import bgra_alpha_spans as _cy_alpha_spans
except ImportError:
    _cy_alpha_spans = None

# ── MMF zero-copy reader ────────────────────────────────────────
_k32 = _ct.windll.kernel32
_FILE_MAP_READ = 0x0004
_SOPF_MAGIC = 0x46504F53
_MMF_HEADER = 64

try:
    _k32.OpenFileMappingW.restype = _ct.c_void_p
    _k32.OpenFileMappingW.argtypes = [_wt.DWORD, _wt.BOOL, _wt.LPCWSTR]
    _k32.MapViewOfFile.restype = _ct.c_void_p
    _k32.MapViewOfFile.argtypes = [
        _ct.c_void_p, _wt.DWORD, _wt.DWORD, _wt.DWORD, _ct.c_size_t]
    _k32.UnmapViewOfFile.argtypes = [_ct.c_void_p]
    _k32.CloseHandle.argtypes = [_ct.c_void_p]
except Exception:
    pass


class _MMFReader:
    """Zero-copy reader for the pet engine's shared memory frame buffer.

    Opens a named Win32 file mapping created by XiaoACTPeto.exe,
    reads BGRA frames directly via memoryview (no Python bytes alloc).
    """
    __slots__ = (
        '_name', '_hmap', '_ptr', '_view',
        'fw', 'fh', 'slot_count', 'slot_stride', '_last_seq',
    )

    def __init__(self, name: str):
        self._name = name
        self._hmap = 0
        self._ptr = 0
        self._view: Optional[memoryview] = None
        self.fw = 0
        self.fh = 0
        self.slot_count = 0
        self.slot_stride = 0
        self._last_seq = -1

    def open(self) -> bool:
        if self._view is not None:
            return True
        try:
            hmap = _k32.OpenFileMappingW(_FILE_MAP_READ, False, self._name)
            if not hmap:
                return False
            ptr = _k32.MapViewOfFile(hmap, _FILE_MAP_READ, 0, 0, _MMF_HEADER)
            if not ptr:
                _k32.CloseHandle(hmap)
                return False
            hdr = (_ct.c_char * _MMF_HEADER).from_address(ptr)
            hdr_bytes = bytes(hdr)
            magic = _struct.unpack_from('<I', hdr_bytes, 0)[0]
            if magic != _SOPF_MAGIC:
                _k32.UnmapViewOfFile(ptr)
                _k32.CloseHandle(hmap)
                return False
            fw, fh = _struct.unpack_from('<II', hdr_bytes, 8)
            sc, ss = _struct.unpack_from('<II', hdr_bytes, 16)
            _k32.UnmapViewOfFile(ptr)
            if fw <= 0 or fh <= 0 or sc <= 0 or ss <= 0:
                _k32.CloseHandle(hmap)
                return False
            total = _MMF_HEADER + sc * ss
            ptr = _k32.MapViewOfFile(hmap, _FILE_MAP_READ, 0, 0, total)
            if not ptr:
                _k32.CloseHandle(hmap)
                return False
            self._hmap = hmap
            self._ptr = ptr
            self.fw = fw
            self.fh = fh
            self.slot_count = sc
            self.slot_stride = ss
            buf = (_ct.c_char * total).from_address(ptr)
            self._view = memoryview(buf)
            return True
        except Exception:
            return False

    def poll(self) -> Optional[memoryview]:
        """Return frame memoryview if a new frame is available, else None."""
        v = self._view
        if v is None:
            return None
        seq = _struct.unpack_from('<q', v, 24)[0]
        if seq == self._last_seq:
            return None
        self._last_seq = seq
        ws = _struct.unpack_from('<I', v, 32)[0]
        rs = (ws + self.slot_count - 1) % self.slot_count
        off = _MMF_HEADER + rs * self.slot_stride
        sz = self.fw * self.fh * 4
        return v[off:off + sz]

    def peek(self) -> Optional[memoryview]:
        """Return the current read-slot frame without advancing seq."""
        v = self._view
        if v is None:
            return None
        ws = _struct.unpack_from('<I', v, 32)[0]
        rs = (ws + self.slot_count - 1) % self.slot_count
        off = _MMF_HEADER + rs * self.slot_stride
        sz = self.fw * self.fh * 4
        return v[off:off + sz]

    def close(self) -> None:
        self._view = None
        if self._ptr:
            try:
                _k32.UnmapViewOfFile(self._ptr)
            except Exception:
                pass
            self._ptr = 0
        if self._hmap:
            try:
                _k32.CloseHandle(self._hmap)
            except Exception:
                pass
            self._hmap = 0

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
                 bgra_swizzle: bool = True,
                 high_fps: bool = False,
                 target_fps: int = 0):
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
        self.high_fps = high_fps
        self.target_fps = target_fps

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

        # MMF zero-copy source (render thread manages lifecycle)
        self._mmf_name: Optional[str] = None
        self._mmf: Optional[_MMFReader] = None

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

        # Per-layer RGN span cache (avoids re-scanning unchanged frames)
        self._rgn_cache_key: Any = None
        self._rgn_cached_spans: list = []

    def upload_bgra(self, bgra: bytes, w: int, h: int) -> None:
        """Upload premultiplied BGRA frame data (thread-safe)."""
        with self._lock:
            self._frame_bytes = bgra
            self._frame_w = w
            self._frame_h = h
            self._frame_seq += 1
            self._dirty = True

    def set_mmf_source(self, mmf_name: Optional[str]) -> None:
        """Attach an MMF zero-copy frame source (thread-safe)."""
        with self._lock:
            self._mmf_name = mmf_name
            if mmf_name is None and self._mmf is not None:
                self._mmf.close()
                self._mmf = None
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
        if self.x != x or self.y != y:
            self.x = x
            self.y = y
            self._dirty = True

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
        proxy.bind('<B1-Motion>', _pos)
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
        if self._input_proxy is not None:
            return False
        return (self.x <= sx < self.x + self.width
                and self.y <= sy < self.y + self.height)

    # ── GL resource management (render thread only) ──────────

    def _poll_mmf(self, ctx: moderngl.Context) -> bool:
        """Try to read a new frame from the MMF source. Returns True
        if a new frame was uploaded to the texture."""
        mmf_name = self._mmf_name
        if mmf_name is None:
            return False
        mmf = self._mmf
        if mmf is None:
            mmf = _MMFReader(mmf_name)
            if not mmf.open():
                return False
            self._mmf = mmf
            self._frame_w = mmf.fw
            self._frame_h = mmf.fh
            self.width = mmf.fw
            self.height = mmf.fh
        frame = mmf.poll()
        if frame is None:
            return False
        w, h = mmf.fw, mmf.fh
        if self._texture is None or self._tex_w != w or self._tex_h != h:
            if self._texture is not None:
                self._texture.release()
            self._texture = ctx.texture((w, h), 4, data=frame)
            self._texture.filter = (moderngl.LINEAR, moderngl.LINEAR)
            self._tex_w = w
            self._tex_h = h
        else:
            self._texture.write(frame)
        self._frame_seq += 1
        self._uploaded_seq = self._frame_seq
        self._dirty = True
        # Store frame memoryview for RGN alpha scanning (zero copy)
        self._frame_bytes = frame
        self._frame_w = w
        self._frame_h = h
        return True

    def _ensure_texture(self, ctx: moderngl.Context) -> None:
        if self._mmf_name is not None:
            return  # MMF layers are polled in the main loop
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
        if self._mmf is not None:
            self._mmf.close()
            self._mmf = None
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

        # Periodic z-order pulse interval
        self._topmost_interval = 2.0  # seconds
        self._last_topmost = 0.0

        # Game window HWND — set by process selector on attach.
        # The z-order pulse positions the compositor just above this.
        self._game_hwnd: int = 0

        # Video fence (frame validation gate)
        self._vf = None

        # Tk callback queue (overlay thread → Tk main thread)
        self._tk_q: queue.Queue = queue.Queue()
        self._tk_poller_id: Any = None

        # Mouse state
        self._hover_layer: Optional[str] = None
        self._capture_layer: Optional[str] = None

        # Per-pixel click passthrough via SetWindowRgn.
        self._host_rgn_key = None
        self._rgn_last_sync = 0.0
        self._rgn_interval = 0.08
        self._rgn_prev_pos: Dict[str, Tuple[int, int]] = {}
        self._rgn_moving = False

        # DirectComposition bridge (replaces SwapBuffers for WDA)
        self._dcomp = None
        self._dcomp_buf: bytearray | None = None
        self._dcomp_buf_sz = 0

        # Performance
        self._default_fps = 60
        self._target_fps = 60
        self._frame_interval = 1.0 / self._target_fps


    # ── Layer management ─────────────────────────────────────

    def create_layer(self, name: str, width: int = 1, height: int = 1,
                     x: int = 0, y: int = 0, z: int = 0,
                     click_through: bool = True,
                     bgra_swizzle: bool = True,
                     high_fps: bool = False,
                     target_fps: int = 0) -> CompositorLayer:
        layer = CompositorLayer(
            name, width, height, x, y, z, click_through, bgra_swizzle,
            high_fps=high_fps, target_fps=target_fps,
        )
        with self._lock:
            self._layers[name] = layer
            self._rebuild_z_order()
        self._recalc_fps()
        return layer

    def destroy_layer(self, name: str) -> None:
        with self._lock:
            layer = self._layers.pop(name, None)
            if layer:
                self._rebuild_z_order()
        self._recalc_fps()

        if layer:
            try:
                layer.destroy_input_proxy()
            except Exception:
                pass

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

    # ── Centralized z-order management ──────────────────────────

    def set_game_hwnd(self, hwnd: int) -> None:
        """Set the game window HWND. The z-order pulse positions the
        compositor just above this window."""
        self._game_hwnd = int(hwnd) if hwnd else 0

    def _enforce_z_order(self) -> None:
        """Position the compositor host just above the game window.

        Priority order:
          1. Kernel path (Engine A R3): set TOPMOST bit via physical
             memory — invisible to user-mode API hooks.
          2. User-mode fallback: SetWindowPos(HWND_TOPMOST) if R3
             is unavailable (no driver loaded / calibration failed).
          3. HWND_TOP if no game HWND is set.
        """
        host = self._host
        if host is None:
            return
        comp_hwnd = host.hwnd
        if not comp_hwnd:
            return
        game = self._game_hwnd
        try:
            import ctypes as _ct
            u32 = _ct.windll.user32
            _SWP = 0x0002 | 0x0001 | 0x0010  # NOMOVE | NOSIZE | NOACTIVATE
            if game and u32.IsWindow(game):
                kernel_ok = False
                game_is_topmost = False
                try:
                    from mem_probe._dc import read_exstyle, set_exstyle_bit
                    ex = read_exstyle(game)
                    if ex is not None and (ex & 0x8):
                        game_is_topmost = True
                        kernel_ok = set_exstyle_bit(comp_hwnd, 0x8)
                    else:
                        kernel_ok = True
                except Exception:
                    kernel_ok = False
                if kernel_ok:
                    u32.SetWindowPos(
                        _ct.c_void_p(comp_hwnd), _ct.c_void_p(game),
                        0, 0, 0, 0, _SWP)
                elif game_is_topmost:
                    _HWND_TOPMOST = -1
                    u32.SetWindowPos(
                        _ct.c_void_p(comp_hwnd),
                        _ct.c_void_p(_HWND_TOPMOST),
                        0, 0, 0, 0, _SWP)
                else:
                    u32.SetWindowPos(
                        _ct.c_void_p(comp_hwnd), _ct.c_void_p(game),
                        0, 0, 0, 0, _SWP)
            else:
                _HWND_TOP = 0
                u32.SetWindowPos(
                    _ct.c_void_p(comp_hwnd), _ct.c_void_p(_HWND_TOP),
                    0, 0, 0, 0, _SWP)
        except Exception:
            pass

    def _rebuild_z_order(self) -> None:
        self._z_sorted = sorted(
            self._layers.values(), key=lambda l: l.z_order,
        )

    def _recalc_fps(self) -> None:
        """Adjust compositor frame interval to match the fastest layer."""
        max_fps = self._default_fps
        with self._lock:
            for layer in self._z_sorted:
                if layer.target_fps > max_fps:
                    max_fps = layer.target_fps
        if max_fps != self._target_fps:
            self._target_fps = max_fps
            self._frame_interval = 1.0 / max_fps

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
        self._schedule_tk_poller()

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
        def _bg():
            try:
                if self._host:
                    self._host.set_capture_mode(exclude)
            except Exception:
                pass
            try:
                if exclude:
                    self._start_vf()
                else:
                    self._stop_vf()
            except Exception:
                pass
        threading.Thread(target=_bg, daemon=True).start()

    def _start_vf(self) -> None:
        if self._vf is not None:
            return
        try:
            from mem_probe._vf import _VFence
            self._vf = _VFence()
            self._vf.start()
        except Exception:
            self._vf = None

    def _stop_vf(self) -> None:
        vf = self._vf
        self._vf = None
        if vf is not None:
            try:
                vf.stop()
            except Exception:
                pass

    def _has_visible_interactive_layers(self) -> bool:
        with self._lock:
            return any(
                layer.visible and not layer.click_through
                and layer._input_proxy is None
                for layer in self._z_sorted
            )

    def sync_host_input_mode(self) -> None:
        """Toggle WS_EX_TRANSPARENT based on whether interactive layers exist.

        When removed, the host receives WM_NCHITTEST and returns HTCLIENT
        for interactive layer areas or HTTRANSPARENT elsewhere.
        HTTRANSPARENT works cross-process for top-level windows, so game
        clicks pass through.  WS_EX_TRANSPARENT does NOT work cross-process
        (Windows ignores it for windows on other threads), so it must be
        removed when layers need compositor-routed input.
        """

        def _set():
            if self._host:
                self._host.set_input_passthrough(
                    not self._has_visible_interactive_layers())
        self._cmd_q.put(_set)

    def force_host_input_passthrough(self) -> None:
        """Force the host window to WS_EX_TRANSPARENT regardless of layer state.

        Plugin layers use Tk input proxies for mouse events and never need
        the host HWND to capture input.  Callers that manage their own input
        routing (e.g. UnifiedOverlayCanvasManager) use this to keep the host
        click-through even when ``click_through=False`` layers exist.
        """

        def _set():
            if self._host:
                self._host.set_input_passthrough(True)
        self._cmd_q.put(_set)

    # ── Host region (click passthrough) ────────────────────────

    _RGN_STEP = 1
    _RGN_PAD_STILL = 0
    _RGN_PAD_MOVE = 32

    def _sync_host_rgn(self, has_visible: bool) -> None:
        """Per-pixel click passthrough via SetWindowRgn.

        Optimisations vs naive full-scan:
        * click_through + high_fps layers use a bounding rect (no alpha scan)
        * other layers cache their alpha spans keyed by (frame_seq, x, y, pad)
        """
        if self._host is None:
            return
        if not has_visible:
            if self._host_rgn_key != 'empty':
                self._host_rgn_key = 'empty'
                try:
                    _ct.windll.user32.SetWindowRgn(
                        self._host.hwnd,
                        _ct.windll.gdi32.CreateRectRgn(0, 0, 0, 0),
                        False)
                except Exception:
                    pass
            return
        if self._has_visible_interactive_layers():
            if self._host_rgn_key != 'full':
                self._host_rgn_key = 'full'
                try:
                    _ct.windll.user32.SetWindowRgn(
                        self._host.hwnd, None, False)
                except Exception:
                    pass
            return
        try:
            ox = self._host.origin_x
            oy = self._host.origin_y
            moving = False
            prev = self._rgn_prev_pos
            cur_pos = {}
            for layer in self._z_sorted:
                if not layer.visible or layer.alpha < 0.01:
                    continue
                cur_pos[layer.name] = (layer.x, layer.y)
                old = prev.get(layer.name)
                if old and (old[0] != layer.x or old[1] != layer.y):
                    moving = True
            self._rgn_prev_pos = cur_pos
            pad = self._RGN_PAD_MOVE if moving else self._RGN_PAD_STILL
            spans: list = []
            _cy = _cy_alpha_spans
            for layer in self._z_sorted:
                if not layer.visible or layer.alpha < 0.01:
                    continue
                lx = layer.x - ox
                ly = layer.y - oy

                fb = layer._frame_bytes
                fw = layer._frame_w
                fh = layer._frame_h
                if not fb or fw <= 0 or fh <= 0:
                    spans.append((
                        lx - pad, ly - pad,
                        lx + layer.width + pad,
                        ly + layer.height + pad))
                    continue

                # Per-layer span cache (avoids re-scanning unchanged frames)
                cache_key = (layer._frame_seq, lx, ly, pad)
                if layer._rgn_cache_key == cache_key:
                    spans.extend(layer._rgn_cached_spans)
                    continue

                sx = max(1, layer.width / fw)
                sy = max(1, layer.height / fh)
                if _cy is not None:
                    layer_spans = list(_cy(fb, fw, fh, lx, ly, sx, sy, pad))
                else:
                    layer_spans = []
                    stride = fw * 4
                    mv = memoryview(fb)
                    for row in range(0, fh):
                        row_off = row * stride + 3
                        x = 0
                        while x < fw:
                            if mv[row_off + x * 4] > 0:
                                x0 = x
                                x += 1
                                while x < fw and mv[row_off + x * 4] > 0:
                                    x += 1
                                layer_spans.append((
                                    int(lx + x0 * sx) - pad,
                                    int(ly + row * sy) - pad,
                                    int(lx + x * sx) + pad,
                                    int(ly + (row + 1) * sy) + pad))
                            else:
                                x += 1
                layer._rgn_cached_spans = layer_spans
                layer._rgn_cache_key = cache_key
                spans.extend(layer_spans)

            key = hash(tuple(spans)) if spans else 0
            if key == self._host_rgn_key:
                return
            self._host_rgn_key = key
            _gdi = _ct.windll.gdi32
            if not spans:
                rgn = _gdi.CreateRectRgn(0, 0, 0, 0)
            else:
                rgn = _gdi.CreateRectRgn(*spans[0])
                for s in spans[1:]:
                    tmp = _gdi.CreateRectRgn(*s)
                    _gdi.CombineRgn(rgn, rgn, tmp, 2)
                    _gdi.DeleteObject(tmp)
            _ct.windll.user32.SetWindowRgn(self._host.hwnd, rgn, False)
        except Exception:
            pass

    # ── Tk callback bridge ───────────────────────────────────

    def _schedule_tk_poller(self) -> None:
        if self._root is None or self._tk_poller_id is not None:
            return
        self._tk_poller_pending = True

    def ensure_tk_poller(self) -> None:
        if self._tk_poller_id is not None or self._root is None:
            return
        if not getattr(self, '_tk_poller_pending', False):
            return
        self._tk_poller_pending = False
        self._start_tk_poller()

    def _start_tk_poller(self) -> None:
        if self._tk_poller_id is not None:
            return
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
        layer covers (sx, sy) and uses host HWND input (no Tk proxy)."""
        for layer in reversed(self._z_sorted):
            if layer.hit_test(sx, sy):
                if layer._input_proxy is not None:
                    return False
                return True
        return False

    def _find_layer_at(self, sx: int, sy: int) -> Optional[CompositorLayer]:
        for layer in reversed(self._z_sorted):
            if layer.hit_test(sx, sy) and layer._input_proxy is None:
                return layer
        return None

    # ── Mouse event routing ────────────────────────────────

    def _on_mouse_event(self, msg: int, sx: int, sy: int,
                        button: int, delta: int) -> None:
        """Route Win32 mouse messages to the appropriate layer.

        Called on the overlay thread (from WndProc via DispatchMessage).
        Layer callbacks are posted to Tk main thread via post_to_tk().

        Mouse capture: after a button-down on a layer, all subsequent
        move and button-up events route to that same layer regardless
        of cursor position, preventing lost releases during drag.
        """
        if msg == WM_MOUSELEAVE:
            prev = self._hover_layer
            self._hover_layer = None
            self._capture_layer = None
            if prev:
                layer = self._layers.get(prev)
                if layer and layer._on_cursor_leave:
                    fn = layer._on_cursor_leave
                    self.post_to_tk(fn)
            return

        # During mouse capture, route to the captured layer
        captured = None
        if self._capture_layer is not None:
            captured = self._layers.get(self._capture_layer)
            if captured is None or not captured.visible:
                self._capture_layer = None
                captured = None

        target = captured if captured is not None else self._find_layer_at(sx, sy)

        if msg == WM_MOUSEMOVE:
            tgt_name = target.name if target else None
            if captured is None and tgt_name != self._hover_layer:
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
            is_down = msg in (WM_LBUTTONDOWN, WM_RBUTTONDOWN)
            if is_down and target is not None:
                self._capture_layer = target.name
            elif not is_down:
                self._capture_layer = None
            if target and target._on_mouse_button:
                action = 1 if is_down else 0
                lx = float(sx - target.x)
                ly = float(sy - target.y)
                fn = target._on_mouse_button
                self.post_to_tk(
                    lambda: fn(button, action, 0, lx, ly))
            return

    # ── Overlay thread main loop ─────────────────────────────

    def _run(self) -> None:
        try:
            print('[Compositor] creating host window...', flush=True)
            self._host = OverlayHost()
            self._host.create()
            print(f'[Compositor] host HWND=0x{self._host.hwnd:08X} '
                  f'{self._host.width}x{self._host.height}', flush=True)
            self._host.hit_test_fn = self._hit_test
            try:
                _gdi = _ct.windll.gdi32
                _gdi.CreateRectRgn.restype = _wt.HRGN
                _gdi.CreateRectRgn.argtypes = [
                    _ct.c_int, _ct.c_int, _ct.c_int, _ct.c_int]
            except Exception:
                pass
            self._host.mouse_fn = self._on_mouse_event
            print('[Compositor] init GL...', flush=True)
            self._init_gl()
            print('[Compositor] GL ready', flush=True)
            try:
                from render.dcomp_bridge import DCompBridge
                self._dcomp = DCompBridge(
                    self._host.hwnd, self._host.width, self._host.height)
            except Exception as _dc_exc:
                print(f'[Compositor] DComp unavailable, using SwapBuffers: '
                      f'{_dc_exc}', flush=True)
                self._dcomp = None
            self._host.show()
            self._ready.set()
            print('[Compositor] running', flush=True)
            try:
                from config import SettingsManager
                if SettingsManager().get('streaming_mode', False):
                    _paid = False
                    try:
                        from license import get_license_manager
                        _paid = get_license_manager().is_paid
                    except Exception:
                        pass
                    if _paid:
                        threading.Thread(
                            target=self._host.set_capture_mode,
                            args=(True,), daemon=True).start()
                        self._start_vf()
            except Exception:
                pass
        except Exception as exc:
            import traceback
            print(f'[Compositor] FATAL init error: {exc}', flush=True)
            traceback.print_exc()
            self._running = False
            self._ready.set()  # unblock waiters
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

            # Z-order pulse: enforce the registered priority order.
            # Uses HWND_TOP chaining (not TOPMOST) so Tk panels stay
            # above the compositor without z-order flickering.
            now = time.perf_counter()
            if now - self._last_topmost >= self._topmost_interval:
                self._enforce_z_order()
                self._last_topmost = now

            # Poll MMF-sourced layers for new frames (zero-copy)
            ctx = self._host.ctx
            for layer in self._z_sorted:
                if layer.visible and layer._mmf_name is not None:
                    layer._poll_mmf(ctx)

            # Tick layer fades + check if any layer needs rendering
            any_dirty = False
            for layer in self._z_sorted:
                layer._tick_fade()
                if layer._dirty or layer._fade_active:
                    any_dirty = True
                elif layer.visible and layer._render_fn is not None:
                    any_dirty = True

            has_visible = any(l.visible for l in self._z_sorted)
            if any_dirty:
                try:
                    self._render_frame(now - t0)
                    self._sync_host_rgn(has_visible)
                    self._present_frame()
                except Exception as _exc:
                    import traceback; traceback.print_exc()
            else:
                self._sync_host_rgn(has_visible)
            vf = self._vf
            if vf is not None and vf.poll():
                self._host.hide()
                _t0 = time.perf_counter()
                while time.perf_counter() - _t0 < 0.0005:
                    pass
                self._host.show()
                vf.release()

            interval = self._frame_interval if has_visible else 0.05
            elapsed = time.perf_counter() - frame_start
            remaining = max(0.001, interval - elapsed)
            _msg_wait_sleep(self._host, remaining, self._stop_evt)
            if self._stop_evt.is_set():
                break

        # Cleanup
        if self._dcomp:
            self._dcomp.destroy()
            self._dcomp = None
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

    def _present_frame(self) -> None:
        """Present the rendered framebuffer via DComp or SwapBuffers."""
        dc = self._dcomp
        if dc is not None and dc.alive:
            ctx = self._host.ctx
            sw = self._host.width
            sh = self._host.height
            buf_sz = sw * sh * 4

            if self._dcomp_buf_sz != buf_sz:
                self._dcomp_buf = bytearray(buf_sz)
                self._dcomp_buf_sz = buf_sz

            ctx.screen.read_into(
                self._dcomp_buf,
                viewport=(0, 0, sw, sh), components=4, alignment=1)
            dc.present(self._dcomp_buf, sw, sh)

            ctx.clear(0.0, 0.0, 0.0, 0.0)
            self._host.swap_buffers()
        else:
            self._host.swap_buffers()

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


def reset_unified_overlay() -> None:
    """Drop a failed singleton UnifiedOverlay after stopping it."""
    global _overlay
    with _overlay_lock:
        old = _overlay
        _overlay = None
    if old is not None:
        try:
            old.stop()
        except Exception:
            pass

