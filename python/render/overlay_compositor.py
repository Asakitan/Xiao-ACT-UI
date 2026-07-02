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

import math
import os
import queue
import struct as _struct
import threading
import time
from typing import Any, Callable, Dict, List, Optional, Tuple

import moderngl
import numpy as np

import ctypes as _ct
import ctypes.wintypes as _wt

from render.overlay_host import (
    OverlayHost,
    WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP,
    WM_RBUTTONDOWN, WM_RBUTTONUP, WM_MOUSEWHEEL, WM_MOUSELEAVE,
)
from utils.perf_probe import probe as _probe, gauge as _perf_gauge

try:
    from _sao_cy_pixels import bgra_alpha_spans as _cy_alpha_spans
except ImportError:
    _cy_alpha_spans = None

try:
    from _sao_cy_pixels import pad_and_merge_row_spans as _cy_pad_merge_spans
except ImportError:
    _cy_pad_merge_spans = None


_REFRESH_MIN_HZ = 60
_REFRESH_MAX_HZ = 240
_REFRESH_DEFAULT_HZ = 60


def _detect_refresh_hz() -> int:
    """Best-effort primary-monitor refresh-rate detection.

    Same approach as ``overlay_scheduler._detect_refresh_hz`` (GDI
    ``GetDeviceCaps(VREFRESH)``, clamped 60-240 Hz, falls back to 60
    on failure or when a driver reports the RDP-style ``1`` sentinel).
    Duplicated locally (13 lines, no shared deps) rather than imported
    to avoid coupling this module's render-thread startup to the Tk
    scheduler module's import order.
    """
    if os.name != 'nt':
        return _REFRESH_DEFAULT_HZ
    try:
        user32 = _ct.windll.user32
        gdi32 = _ct.windll.gdi32
        VREFRESH = 116
        hdc = user32.GetDC(0)
        if not hdc:
            return _REFRESH_DEFAULT_HZ
        try:
            rate = int(gdi32.GetDeviceCaps(hdc, VREFRESH))
        finally:
            user32.ReleaseDC(0, hdc)
        if rate <= 1:
            return _REFRESH_DEFAULT_HZ
        return max(_REFRESH_MIN_HZ, min(_REFRESH_MAX_HZ, rate))
    except Exception:
        return _REFRESH_DEFAULT_HZ


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

# For GPU-shared-texture layers (Part B): the shared texture holds
# straight (non-premultiplied) RGBA — the producer process never
# touches the pixels on the CPU, so premultiply happens here instead
# (a shader multiply is effectively free; doing it upstream would cost
# either a CPU pass or extra native-plugin shader work).
_FRAG_SHARED_SRC = '''
#version 330
uniform sampler2D u_tex;
uniform float u_alpha;
in vec2 v_uv;
out vec4 fragColor;
void main() {
    vec4 c = texture(u_tex, v_uv);
    fragColor = vec4(c.rgb * c.a, c.a) * u_alpha;
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

        # GPU-shared D3D11 texture source (Part B) — set from any
        # thread via set_shared_texture_source(); registered/locked
        # only on the render thread. When active, this is the layer's
        # color source (drawn via a raw-GL-bound external texture);
        # _mmf_name (if also set) keeps being polled purely for its
        # alpha byte, feeding _sync_host_rgn's click-through scan —
        # see _ensure_shared_texture()/_poll_mmf().
        self._shared_tex_handle: Optional[int] = None
        self._shared_tex_w = 0
        self._shared_tex_h = 0
        self._shared_reg_handle_value = 0
        self._shared_d3d_tex = None    # c_void_p from open_shared_texture()
        self._shared_gl_tex_id = 0
        self._shared_hobj = None       # interop object handle (lock/unlock)
        self._shared_keyed_mutex = None  # IDXGIKeyedMutex (None = unsynced producer)
        self._shared_attempt_failed = False  # a set handle failed to register

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

        # Screen-space rect (host-origin-relative) this layer occupied the
        # last time it was actually drawn — used to build the per-frame
        # dirty rect for partial DComp readback/present (see
        # UnifiedOverlay._compute_dirty_rect). None until first drawn.
        self._last_draw_rect: Optional[Tuple[int, int, int, int]] = None

        # Per-layer RGN span cache (avoids re-scanning unchanged frames)
        self._rgn_cache_key: Any = None
        self._rgn_cached_spans: list = []

    def upload_bgra(self, bgra: bytes, w: int, h: int) -> None:
        """Upload premultiplied BGRA frame data (thread-safe).

        Rejects a (w, h) that doesn't match ``len(bgra)`` instead of storing
        it — callers race their own dimension fields against a background
        capture in flight (a resize/animation mid-capture) often enough
        that this shouldn't be treated as exceptional. Storing a mismatched
        pair here would otherwise crash the render thread's
        ``ctx.texture()`` call on every frame until the next successful
        upload overwrites it; skipping just keeps showing the last good
        frame for one tick.
        """
        if w <= 0 or h <= 0 or len(bgra) != w * h * 4:
            return
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

    def set_shared_texture_source(self, handle: int, width: int, height: int) -> None:
        """Attach a GPU-shared D3D11 texture as this layer's color
        source (thread-safe). ``handle <= 0`` clears it — the render
        thread unregisters/releases whatever was registered on its
        next tick. Does not touch ``_mmf_name``: a layer can keep an
        MMF source attached purely for its alpha byte (RGN scanning)
        while its color comes from the shared texture instead."""
        with self._lock:
            h = int(handle) if handle else 0
            self._shared_tex_handle = h if h > 0 else None
            self._shared_tex_w = max(1, int(width)) if h > 0 else 0
            self._shared_tex_h = max(1, int(height)) if h > 0 else 0
        self._shared_attempt_failed = False
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
        # Must open the render gate: the vacated screen area only gets
        # repainted if a frame is actually rendered+presented after the
        # hide. Without this, hiding while nothing else animates leaves
        # the layer's last pixels on screen until some other layer goes
        # dirty ("ghost" residue after closing a panel).
        self._dirty = True

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
        # A layer with an active GPU shared-texture color source (Part B)
        # only needs this MMF frame for its alpha byte (RGN click-through
        # scanning) — the color already comes from the shared texture, so
        # skip the GL upload entirely. Re-checked every poll since the
        # shared-texture handshake can complete/drop at any time.
        if self._shared_tex_handle is None:
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

    def _ensure_shared_texture(self, dc) -> bool:
        """Register/refresh this layer's GPU-shared texture source
        (render-thread only, mirrors ``_poll_mmf``'s lazy-open shape).

        Returns True if the layer currently has a valid registered GL
        texture ready to draw from. On any failure — no handle set, no
        GPU interop available, OpenSharedResource/register failing —
        releases whatever was registered and returns False; the caller
        must skip drawing this layer's shared-texture branch that tick
        (the layer's plain MMF/upload path, if any, is unaffected).
        """
        from render.dcomp_bridge import (
            gl_gen_texture, gl_delete_texture, gl_bind_texture_unit0,
            gl_set_bound_texture_linear, open_keyed_mutex, release_com)

        with self._lock:
            handle = self._shared_tex_handle
            w = self._shared_tex_w
            h = self._shared_tex_h

        if handle is None or dc is None or not dc.gl_interop_active:
            if self._shared_hobj is not None:
                self._release_shared_texture(dc)
            if handle is not None:
                # A handle IS attached but interop is gone (disabled
                # after a device loss / never came up) — that's a real
                # failure the producer must learn about, not a benign
                # "nothing attached" state.
                self._shared_attempt_failed = True
                self._log_shared_fail(handle, 'GPU interop inactive')
            return False

        if handle == self._shared_reg_handle_value and self._shared_hobj is not None:
            return True  # already registered against this exact handle

        # New handle (first time, or the producer recreated its shared
        # texture e.g. on resize) — tear down the old registration and
        # open fresh.
        self._release_shared_texture(dc)

        d3d_tex = dc.open_shared_texture(handle)
        if d3d_tex is None:
            self._shared_attempt_failed = True
            self._log_shared_fail(handle, 'OpenSharedResource failed')
            return False
        gl_id = gl_gen_texture()
        if not gl_id:
            release_com(d3d_tex)
            self._shared_attempt_failed = True
            self._log_shared_fail(handle, 'glGenTextures failed')
            return False
        hobj = dc.register_external_texture(d3d_tex.value, gl_id)
        if hobj is None:
            gl_delete_texture(gl_id)
            release_com(d3d_tex)
            self._shared_attempt_failed = True
            self._log_shared_fail(handle, 'wglDXRegisterObjectNV failed')
            return False

        # A raw GL texture name defaults to a mipmapping min-filter; the
        # registered texture has one level, so it would be INCOMPLETE and
        # sample black without explicit filters. GL may only touch the
        # object while locked.
        if dc.lock_external_texture(hobj):
            try:
                gl_bind_texture_unit0(gl_id)
                gl_set_bound_texture_linear()
            finally:
                dc.unlock_external_texture(hobj)

        # Producers that created the texture with the keyed-mutex flag
        # get tear-free sampling (acquire around the draw); detected by
        # QI so plain-shared producers need no protocol change.
        self._shared_keyed_mutex = open_keyed_mutex(d3d_tex)

        self._shared_d3d_tex = d3d_tex
        self._shared_gl_tex_id = gl_id
        self._shared_hobj = hobj
        self._shared_reg_handle_value = handle
        self._shared_attempt_failed = False
        self.width = w
        self.height = h
        self._dirty = True
        print(f'[Compositor] layer {self.name!r}: GPU shared texture '
              f'0x{handle:X} registered ({w}x{h}, '
              f'{"keyed-mutex" if self._shared_keyed_mutex else "unsynced"})',
              flush=True)
        return True

    def _log_shared_fail(self, handle: int, why: str) -> None:
        # One line per distinct failing handle — this runs every frame
        # while a handle is set, so unthrottled printing would flood.
        if getattr(self, '_shared_fail_logged', None) == handle:
            return
        self._shared_fail_logged = handle
        print(f'[Compositor] layer {self.name!r}: shared texture '
              f'0x{handle:X} unusable ({why}) — MMF color path stays',
              flush=True)

    @property
    def shared_texture_active(self) -> bool:
        """False only once an attached handle actually FAILED to
        register on the render thread. "Handle set but not attempted
        yet" (layer hidden, first draw pending) still reports True so
        a producer polling right after its handshake doesn't false-
        trigger its fallback while the layer simply hasn't drawn."""
        if self._shared_hobj is not None:
            return True
        with self._lock:
            handle = self._shared_tex_handle
        return handle is not None and not self._shared_attempt_failed

    def _release_shared_texture(self, dc) -> None:
        from render.dcomp_bridge import gl_delete_texture, release_com

        if self._shared_hobj is not None and dc is not None:
            dc.unregister_external_texture(self._shared_hobj)
        self._shared_hobj = None
        if self._shared_gl_tex_id:
            gl_delete_texture(self._shared_gl_tex_id)
            self._shared_gl_tex_id = 0
        if self._shared_keyed_mutex is not None:
            release_com(self._shared_keyed_mutex)
            self._shared_keyed_mutex = None
        if self._shared_d3d_tex is not None:
            release_com(self._shared_d3d_tex)
            self._shared_d3d_tex = None
        self._shared_reg_handle_value = 0

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

    def _release_gl(self, dc=None) -> None:
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
        if self._shared_hobj is not None or self._shared_d3d_tex is not None:
            self._release_shared_texture(dc)


# ── Mouse event types ────────────────────────────────────────────
MOUSE_MOVE = 'move'
MOUSE_PRESS = 'press'
MOUSE_RELEASE = 'release'
MOUSE_SCROLL = 'scroll'
MOUSE_LEAVE = 'leave'


# ── Batched HRGN construction ────────────────────────────────────
# RDH_RECTANGLES
_RDH_RECTANGLES = 1


class _RGNDATAHEADER(_ct.Structure):
    _fields_ = [
        ('dwSize', _ct.c_uint32), ('iType', _ct.c_uint32),
        ('nCount', _ct.c_uint32), ('nRgnSize', _ct.c_uint32),
        ('rcBound', _wt.RECT),
    ]


_gdi32 = _ct.windll.gdi32
try:
    _gdi32.ExtCreateRegion.restype = _ct.c_void_p
    _gdi32.ExtCreateRegion.argtypes = [_ct.c_void_p, _ct.c_uint32, _ct.c_void_p]
except Exception:
    pass


#: Reused across calls so a complex frame (many spans) doesn't force a
#: fresh heap allocation every tick — grown (never shrunk) on demand.
#: See _build_region_from_rects for why this matters on fast pet motion.
_region_buf: Optional[_ct.Array] = None
_region_buf_cap: int = 0


def _build_region_from_rects(rects: list):
    """Build an HRGN as the union of *rects* in a single GDI call.

    Text-heavy panels (DPS/HP/buff lists) can produce hundreds to
    thousands of per-scanline alpha spans. Building the union via one
    ``CreateRectRgn``+``CombineRgn``+``DeleteObject`` triple per rect
    costs ~15-20ms of GDI syscalls for ~2500 spans (measured) — enough
    to blow a whole frame budget at 90 fps. ``ExtCreateRegion`` takes
    the entire rect list in one kernel transition and produces the
    identical union shape (verified via RGN_XOR against the iterative
    form), ~13x faster.

    The rect buffer itself is also a per-call cost worth avoiding: a
    fast-moving, hairy/fuzzy silhouette (a desktop pet mid-animation)
    can peak at thousands of spans, and the previous version allocated
    a fresh ``create_string_buffer`` *and* filled it via a Python-level
    ``for`` loop constructing one ``RECT`` per span every single tick —
    exactly on the frames where span count spikes (fast motion), which
    read as an occasional single-frame hitch. Reusing a module-level
    buffer (grown, never shrunk) plus a bulk ``memmove`` from a numpy
    array (``wintypes.RECT`` is 4 contiguous ``LONG`` fields — bit-
    identical layout to an int32 (left, top, right, bottom) row, so a
    raw memory copy is exact, not an approximation) turns an O(n)
    Python loop + allocation into one C-level block copy.
    """
    global _region_buf, _region_buf_cap
    n = len(rects)
    if n == 0:
        return _gdi32.CreateRectRgn(0, 0, 0, 0)
    header_size = _ct.sizeof(_RGNDATAHEADER)
    rect_size = _ct.sizeof(_wt.RECT)
    needed = header_size + n * rect_size
    if _region_buf is None or _region_buf_cap < needed:
        # Grow with slack so a slightly-larger next frame doesn't
        # immediately force another reallocation.
        _region_buf_cap = needed + rect_size * 256
        _region_buf = _ct.create_string_buffer(_region_buf_cap)
    buf = _region_buf

    arr = np.asarray(rects, dtype=np.int32)  # (n, 4): x0, y0, x1, y1
    minx = int(arr[:, 0].min())
    miny = int(arr[:, 1].min())
    maxx = int(arr[:, 2].max())
    maxy = int(arr[:, 3].max())

    hdr = _RGNDATAHEADER.from_buffer(buf, 0)
    hdr.dwSize = header_size
    hdr.iType = _RDH_RECTANGLES
    hdr.nCount = n
    hdr.nRgnSize = 0
    hdr.rcBound = _wt.RECT(minx, miny, maxx, maxy)

    if not arr.flags['C_CONTIGUOUS']:
        arr = np.ascontiguousarray(arr)
    _ct.memmove(_ct.addressof(buf) + header_size, arr.ctypes.data, n * rect_size)

    hrgn = _gdi32.ExtCreateRegion(None, needed, _ct.byref(buf))
    return hrgn if hrgn else _gdi32.CreateRectRgn(0, 0, 0, 0)


def _pad_and_merge_row_spans_py(spans: list, pad: int) -> list:
    """Pure-Python fallback for ``_sao_cy_pixels.pad_and_merge_row_spans``
    (used only when the Cython accelerator isn't built).

    Pads each (x0, y0, x1, y1) span by *pad* and merges same-row spans
    that touch or overlap once padded, in one linear pass. ``spans`` must
    be in scanline order (row-major, ascending x within a row) — exactly
    what the alpha-span scanner emits. This is a lossless reshape: the
    final GDI region union is identical whether spans are pre-merged or
    fed to ExtCreateRegion one-by-one. It matters because a detailed
    character sprite (hair/fur edges) can emit tens of thousands of
    1-2px spans per scan; building the region from that many individual
    rects costs far more than the scan itself (measured ~15ms vs ~3ms at
    768x1152), and it collapses to ~1-2k rects after merging.
    """
    if not spans:
        return []
    padded = [(s[0] - pad, s[1] - pad, s[2] + pad, s[3] + pad) for s in spans]
    merged = [list(padded[0])]
    for r in padded[1:]:
        cur = merged[-1]
        if r[1] == cur[1] and r[3] == cur[3] and r[0] <= cur[2]:
            if r[2] > cur[2]:
                cur[2] = r[2]
        else:
            merged.append(list(r))
    return [tuple(x) for x in merged]


def _pad_and_merge_row_spans(spans: list, pad: int) -> list:
    if _cy_pad_merge_spans is not None:
        return _cy_pad_merge_spans(spans, pad)
    return _pad_and_merge_row_spans_py(spans, pad)


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

        # One-shot render gate opener for events the per-layer dirty
        # scan can't see (destroying a visible layer removes it from
        # the scan entirely) — consumed by the overlay loop.
        self._layers_changed = False

        # This tick's position/size snapshot from _render_frame, shared
        # with _sync_host_rgn so the clip region always agrees with what
        # was actually drawn (see _render_frame's docstring on why a
        # second live read of layer.x/y races a plugin's position
        # writes).
        self._last_render_snapshots: Optional[
            Dict[str, Tuple[int, int, int, int]]] = None

        # DirectComposition bridge (replaces SwapBuffers for WDA)
        self._dcomp = None
        self._dcomp_buf: bytearray | None = None
        self._dcomp_buf_sz = 0
        self._dcomp_partial_buf: bytearray | None = None
        # moderngl wrap of the bridge's interop FBO (Part A). Rendering
        # into that FBO MUST go through this wrap — raw glBindFramebuffer
        # behind moderngl's back gets silently undone by the next
        # ctx.clear()/Framebuffer.use() (moderngl rebinds its own tracked
        # framebuffer), scattering draws across stale targets
        # (gpu_interop_selftest.py check 2 vs 3 proves both halves).
        self._gpu_fbo_wrap = None
        self._gpu_fbo_wrap_gen = -1

        # Performance — default follows the primary monitor's actual
        # refresh rate (e.g. 144 Hz) instead of a hardcoded 60. Layers may
        # still request a higher target_fps via create_layer(); the
        # compositor uses whichever is greater (see _recalc_fps).
        self._default_fps = _detect_refresh_hz()
        self._target_fps = self._default_fps
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
        if layer is not None and layer.visible:
            # The removed layer no longer participates in the dirty
            # scan, so nothing else would trigger the render+present
            # that repaints its vacated screen area — force one frame.
            self._layers_changed = True

        if layer:
            try:
                layer.destroy_input_proxy()
            except Exception:
                pass

        def _release():
            if layer:
                layer._release_gl(self._dcomp)
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

    def force_host_hidden(self, hidden: bool) -> None:
        """Temporarily hide/show the host window itself.

        Used around blocking native dialogs (file pickers). Passthrough
        alone (``force_host_input_passthrough``) fixes click routing, but
        the host is still WS_EX_TOPMOST and has its z-order re-asserted
        every ``_topmost_interval`` seconds by ``_enforce_z_order()`` — a
        normal (non-topmost) dialog window can still end up visually
        buried under it even though clicks now pass through. Hiding the
        host outright avoids both the input *and* the visual-obstruction
        problem for the dialog's duration; nothing is drawn or hit-tested
        while it's hidden.
        """

        def _set():
            if self._host:
                if hidden:
                    self._host.hide()
                else:
                    self._host.show()
        self._cmd_q.put(_set)

    # ── Host region (click passthrough) ────────────────────────

    _RGN_STEP = 1
    _RGN_PAD_STILL = 0
    _RGN_PAD_MOVE = 32

    def _sync_host_rgn(
        self, has_visible: bool,
        snapshots: Optional[Dict[str, Tuple[int, int, int, int]]] = None,
    ) -> None:
        """Per-pixel click passthrough via SetWindowRgn.

        Optimisations vs naive full-scan:
        * click_through layers still need a precise per-pixel alpha mask:
          WM_NCHITTEST/HTTRANSPARENT only forwards a click within the same
          thread (see OverlayHost.set_input_passthrough's docstring) — it
          does NOT reach a different process. When any non-click_through
          layer is visible, WS_EX_TRANSPARENT is off, so SetWindowRgn is
          the only thing that lets a click through the transparent part of
          a click_through layer all the way to the game process. A
          bounding rect there would swallow clicks in the "empty" padding
          around the sprite instead of passing them through.
        * every layer (click_through or not) caches its unpadded alpha
          spans keyed by (frame_seq, x, y) — padding is applied per-layer
          as a cheap arithmetic expand *after* the cache lookup, so one
          layer moving only re-pads that layer instead of invalidating
          every other layer's cache and forcing a full re-scan of all of
          them. The scan itself is the cheap part (nogil Cython over
          already-decoded RGBA); it only runs when *this* layer's content
          or position actually changed.

        *snapshots*: the SAME per-tick position/size snapshot
        ``_render_frame`` used to draw and compute the dirty rect (see
        its docstring). Position is mutated unsynchronized from another
        thread (a desktop pet's plugin Tick, up to 60Hz during walking
        or a fast drag) — re-reading ``layer.x``/``layer.y`` live here
        instead of using that same snapshot let the clip region disagree
        with what was actually drawn this tick, clipping the sprite
        against the wrong rect for one frame (visible as a torn/bitten
        edge exactly on fast motion). ``None`` (the "nothing changed
        this tick" caller) falls back to live reads — safe there because
        no position write means no ``_dirty``, so nothing to race.
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
            prev = self._rgn_prev_pos
            cur_pos = {}
            for layer in self._z_sorted:
                if not layer.visible or layer.alpha < 0.01:
                    continue
                if snapshots is not None and layer.name in snapshots:
                    sx0, sy0, _sw0, _sh0 = snapshots[layer.name]
                else:
                    sx0, sy0 = layer.x, layer.y
                cur_pos[layer.name] = (sx0, sy0)
            self._rgn_prev_pos = cur_pos
            spans: list = []
            _cy = _cy_alpha_spans
            for layer in self._z_sorted:
                if not layer.visible or layer.alpha < 0.01:
                    continue
                # Use the exact position/size _render_frame drew this
                # tick with, not a fresh (possibly already-stale-or-not)
                # read of layer.x/y/width/height — see the snapshots
                # param docstring above for why the two can disagree.
                if snapshots is not None and layer.name in snapshots:
                    layer_x, layer_y, layer_w, layer_h = snapshots[layer.name]
                else:
                    layer_x, layer_y = layer.x, layer.y
                    layer_w, layer_h = layer.width, layer.height
                lx = layer_x - ox
                ly = layer_y - oy

                old = prev.get(layer.name)
                layer_moving = bool(
                    old and (old[0] != layer_x or old[1] != layer_y))
                if layer_moving:
                    # Scale the pad to the actual per-tick movement: the
                    # region is applied by DWM up to a frame out of step
                    # with the presented pixels, so a fast drag (easily
                    # 50-100 px/tick) overruns a fixed 32 px pad and the
                    # stale region visibly clips the sprite's leading
                    # edge — reads as "tearing" on fast motion. Capped:
                    # the pad is also the area where clicks over empty
                    # pixels get swallowed while the layer moves.
                    step = max(abs(layer_x - old[0]), abs(layer_y - old[1]))
                    pad = min(256, max(self._RGN_PAD_MOVE, step * 2))
                else:
                    pad = self._RGN_PAD_STILL

                fb = layer._frame_bytes
                fw = layer._frame_w
                fh = layer._frame_h
                if not fb or fw <= 0 or fh <= 0:
                    spans.append((
                        lx - pad, ly - pad,
                        lx + layer_w + pad,
                        ly + layer_h + pad))
                    continue

                # Per-layer span cache, keyed only on content + position —
                # padding is NOT part of the key, so another layer's move
                # (or this layer's own moving flag flipping) never forces a
                # re-scan when the actual pixels haven't changed.
                cache_key = (layer._frame_seq, lx, ly)
                if layer._rgn_cache_key == cache_key:
                    layer_spans = layer._rgn_cached_spans
                else:
                    sx = max(1, layer_w / fw)
                    sy = max(1, layer_h / fh)
                    if _cy is not None:
                        layer_spans = list(_cy(fb, fw, fh, lx, ly, sx, sy, 0))
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
                                    # floor the low edge / ceil the high
                                    # edge (not a plain int() truncation)
                                    # so an upscaled span always fully
                                    # covers its source pixel run — see
                                    # bgra_alpha_spans' docstring in
                                    # _sao_cy_pixels.pyx for why a
                                    # truncated high edge silently drops
                                    # the trailing pixels of every span.
                                    layer_spans.append((
                                        int(math.floor(lx + x0 * sx)),
                                        int(math.floor(ly + row * sy)),
                                        int(math.ceil(lx + x * sx)),
                                        int(math.ceil(ly + (row + 1) * sy))))
                                else:
                                    x += 1
                    layer._rgn_cached_spans = layer_spans
                    layer._rgn_cache_key = cache_key

                if pad:
                    spans.extend(_pad_and_merge_row_spans(layer_spans, pad))
                else:
                    spans.extend(layer_spans)

            key = hash(tuple(spans)) if spans else 0
            if key == self._host_rgn_key:
                return
            self._host_rgn_key = key
            rgn = _build_region_from_rects(spans)
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
                try:
                    if self._dcomp.enable_gl_interop(self._host.hdc):
                        print('[Compositor] GPU present path active '
                              '(WGL_NV_DX_interop2)', flush=True)
                    else:
                        print('[Compositor] GPU present path unavailable, '
                              'using CPU readback present', flush=True)
                except Exception as _interop_exc:
                    print(f'[Compositor] enable_gl_interop failed, using '
                          f'CPU readback present: {_interop_exc}', flush=True)
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

        # Windows' default timer/scheduler quantum is ~15.6ms, which caps
        # MsgWaitForMultipleObjectsEx (used by _msg_wait_sleep below) to the
        # same granularity — a 144 Hz target (6.9ms frame interval) would
        # silently degrade to ~60-64 Hz regardless of _frame_interval.
        # overlay_scheduler.py already works around this for the Tk pacer;
        # this thread needs the same timeBeginPeriod(1) boost since it does
        # its own independent sleep/wait.
        _winmm = None
        try:
            _winmm = _ct.windll.winmm
            _winmm.timeBeginPeriod(1)
        except Exception:
            _winmm = None

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
            with _probe('compositor.poll_mmf'):
                for layer in self._z_sorted:
                    if layer.visible and layer._mmf_name is not None:
                        layer._poll_mmf(ctx)

            # Tick layer fades + check if any layer needs rendering.
            #
            # The `_dirty` flag is set by set_position()/set_geometry(),
            # called unsynchronized from whatever thread owns the caller
            # (e.g. the C# plugin bridge dragging a desktop pet). That
            # write (layer.x = x; layer.y = y; layer._dirty = True) is not
            # atomic as a group, so this loop can sample `_dirty` a hair
            # before the write lands and conclude nothing changed — which
            # skips render_frame()/present_frame() entirely for the tick
            # ("gap frame"). _compute_dirty_rect has its own moved_or_new
            # fallback for exactly this reason, but that fallback is dead
            # if the gate below never lets it run. Cross-check the current
            # position against what was last actually presented so a
            # missed `_dirty` still opens the gate.
            ox = self._host.origin_x
            oy = self._host.origin_y
            any_dirty = False
            for layer in self._z_sorted:
                layer._tick_fade()
                if layer._dirty or layer._fade_active:
                    any_dirty = True
                elif layer.visible and layer._render_fn is not None:
                    any_dirty = True
                elif layer.visible and layer.alpha > 0.001:
                    cur_rect = (layer.x - ox, layer.y - oy,
                                layer.x - ox + layer.width, layer.y - oy + layer.height)
                    if cur_rect != layer._last_draw_rect:
                        any_dirty = True

            if self._layers_changed:
                self._layers_changed = False
                any_dirty = True

            has_visible = any(l.visible for l in self._z_sorted)
            if any_dirty:
                try:
                    with _probe('compositor.render_frame'):
                        dirty_rect = self._render_frame(now - t0)
                    with _probe('compositor.sync_host_rgn'):
                        self._sync_host_rgn(
                            has_visible, self._last_render_snapshots)
                    with _probe('compositor.present_frame'):
                        self._present_frame(dirty_rect)
                except Exception as _exc:
                    import traceback; traceback.print_exc()
            else:
                with _probe('compositor.sync_host_rgn'):
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
            _perf_gauge('compositor.frame_ms', elapsed * 1000.0)
            _perf_gauge('compositor.target_fps', self._target_fps)
            remaining = max(0.001, interval - elapsed)
            _msg_wait_sleep(self._host, remaining, self._stop_evt)
            if self._stop_evt.is_set():
                break

        # Cleanup
        if _winmm is not None:
            try:
                _winmm.timeEndPeriod(1)
            except Exception:
                pass
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

        # GPU-shared-texture layers (Part B): same premultiply-free
        # straight-alpha shader, bottom-up UV. Interop aliases the D3D11
        # texture memory 1:1 into GL, so the producer's row order is
        # what t=0 samples. Unity D3D11 render targets store row 0 =
        # image BOTTOM (its render-to-texture projection flip — provable
        # from SharedMemoryFramePublisher.OnReadback needing a
        # (_height-1-srcY) flip to produce the top-down MMF), which is
        # exactly GL's bottom-up convention → vbo_fbo winding. A
        # producer with top-down rows would need verts_bgra instead.
        self._shared_prog = ctx.program(
            vertex_shader=_VERT_SRC, fragment_shader=_FRAG_SHARED_SRC,
        )
        self._quad_vao_shared = ctx.vertex_array(
            self._shared_prog,
            [(vbo_fbo, '2f 2f', 'in_pos', 'in_uv')],
        )

    def _present_frame(
        self, dirty_rect: Optional[Tuple[int, int, int, int]] = None,
    ) -> None:
        """Present the rendered framebuffer via DComp or SwapBuffers.

        *dirty_rect*, if given, is the host-relative, top-left-origin
        (x0, y0, x1, y1) rect that actually needs re-presenting this
        frame (see ``_compute_dirty_rect``). The GPU-side render pass
        always redraws the full framebuffer regardless — only the
        CPU-side readback + D3D11 staging write is narrowed to this
        rect, which is where the measured cost lived (~2-3ms/frame for a
        full-screen readback+copy vs a typical small dirty rect) before
        Part A's GPU interop path removed the CPU readback entirely.
        """
        dc = self._dcomp
        if dc is not None and dc.gl_interop_active:
            with _probe('compositor.dcomp_present_gpu'):
                if dc.present_gpu():
                    return
            # Present failed mid-session (driver reset, device lost) —
            # disable interop permanently for this run and fall through
            # to the CPU path below for this frame and all future ones.
            dc.disable_gl_interop()
        if dc is not None and dc.alive:
            ctx = self._host.ctx
            sw = self._host.width
            sh = self._host.height
            buf_sz = sw * sh * 4
            resized = self._dcomp_buf_sz != buf_sz
            if resized:
                self._dcomp_buf = bytearray(buf_sz)
                self._dcomp_buf_sz = buf_sz

            use_partial = (
                not resized and dirty_rect is not None
                and dirty_rect != (0, 0, sw, sh))
            if use_partial:
                x0, y0, x1, y1 = dirty_rect
                vw = x1 - x0
                vh = y1 - y0
                gl_x = x0
                gl_y = sh - y1
                need = vw * vh * 4
                buf = self._dcomp_partial_buf
                if buf is None or len(buf) != need:
                    buf = bytearray(need)
                    self._dcomp_partial_buf = buf
                with _probe('compositor.dcomp_readback'):
                    ctx.screen.read_into(
                        buf, viewport=(gl_x, gl_y, vw, vh),
                        components=4, alignment=1)
                with _probe('compositor.dcomp_present'):
                    ok = dc.present_partial(
                        buf, sw, sh, gl_x, gl_y, vw, vh)
                if not ok:
                    use_partial = False

            if not use_partial:
                with _probe('compositor.dcomp_readback'):
                    ctx.screen.read_into(
                        self._dcomp_buf,
                        viewport=(0, 0, sw, sh), components=4, alignment=1)
                with _probe('compositor.dcomp_present'):
                    dc.present(self._dcomp_buf, sw, sh)
        else:
            self._host.swap_buffers()

    def _compute_dirty_rect(
        self, sw: int, sh: int, ox: int, oy: int,
        snapshots: Dict[str, Tuple[int, int, int, int]],
        layers: List['CompositorLayer'],
    ) -> Optional[Tuple[int, int, int, int]]:
        """Union bounding rect (host-relative, top-left origin) of every
        screen area that actually needs re-presenting this frame.

        For each layer, unions in its *current* rect if it needs redrawing
        (dirty/fading/render_fn) and its *previous* rect if that differs
        from the current one (covers "moved away from", "shrunk", or
        "became invisible" — the vacated area must still be refreshed even
        though the layer itself has nothing new to draw there). A layer
        that is visible, unchanged, and at the same rect as last frame
        contributes nothing — its presented pixels are already correct.

        The GPU-side render pass in ``_render_frame`` is NOT restricted to
        this rect — it always redraws the whole framebuffer (cheap, see
        profiling: ~0.2-0.3ms). Only the CPU-side readback/present step
        uses this rect, which is where the real cost was measured
        (~2-3ms/frame from a full 1920x1080+ readback+D3D11 copy).
        """
        x0 = y0 = x1 = y1 = None
        pad = 2  # small defensive margin against off-by-one rect edges
        # *layers* is the caller's per-tick capture of _z_sorted — the
        # same list `snapshots` was built from. Re-reading self._z_sorted
        # here instead would race _rebuild_z_order() replacing it from
        # another thread (layer created/destroyed mid-tick) and hit a
        # KeyError on a layer that isn't in `snapshots`.
        for layer in layers:
            visible_now = layer.visible and layer.alpha > 0.001
            cur_rect = None
            if visible_now:
                lx, ly, lw, lh = snapshots[layer.name]
                lx0 = lx - ox
                ly0 = ly - oy
                cur_rect = (lx0, ly0, lx0 + lw, ly0 + lh)

            was_rect = layer._last_draw_rect
            # A rect mismatch (appeared, moved, or resized) forces a redraw
            # in its own right — don't rely solely on the _dirty flag being
            # set by every caller that mutates x/y/width/height.
            moved_or_new = visible_now and was_rect != cur_rect
            needs_redraw = visible_now and (
                layer._dirty or layer._fade_active
                or layer._render_fn is not None or moved_or_new)

            if needs_redraw and cur_rect is not None:
                rx0, ry0, rx1, ry1 = cur_rect
                x0 = rx0 if x0 is None else min(x0, rx0)
                y0 = ry0 if y0 is None else min(y0, ry0)
                x1 = rx1 if x1 is None else max(x1, rx1)
                y1 = ry1 if y1 is None else max(y1, ry1)
            if was_rect is not None and was_rect != cur_rect:
                rx0, ry0, rx1, ry1 = was_rect
                x0 = rx0 if x0 is None else min(x0, rx0)
                y0 = ry0 if y0 is None else min(y0, ry0)
                x1 = rx1 if x1 is None else max(x1, rx1)
                y1 = ry1 if y1 is None else max(y1, ry1)

            layer._last_draw_rect = cur_rect

        if x0 is None:
            return None
        x0 = max(0, x0 - pad)
        y0 = max(0, y0 - pad)
        x1 = min(sw, x1 + pad)
        y1 = min(sh, y1 + pad)
        if x1 <= x0 or y1 <= y0:
            return None
        return (x0, y0, x1, y1)

    def _render_frame(self, t: float) -> Optional[Tuple[int, int, int, int]]:
        ctx = self._host.ctx
        sw = self._host.width
        sh = self._host.height
        ox = self._host.origin_x
        oy = self._host.origin_y

        # Snapshot each layer's geometry ONCE for this tick. Layer
        # position is mutated directly (unsynchronized, from whatever
        # thread owns the caller — e.g. the C# plugin bridge calling
        # set_compositor_layer_position for a desktop pet) rather than
        # through the render thread's command queue. Re-reading
        # layer.x/y/width/height separately in _compute_dirty_rect and
        # again here could see two different positions if a mutation
        # lands in between, so the presented dirty rect and the actually
        # drawn pixels could disagree — the sprite vanishes for a frame
        # because the rect that got read back/presented doesn't cover
        # where it was actually drawn. Snapshotting once keeps both
        # consumers looking at the exact same values for this tick.
        # Capture the layer list itself ONCE too. _rebuild_z_order()
        # (another thread creating/destroying a layer, e.g. fisheye's
        # dynamic sao_fisheye_gpu_N panels) REPLACES self._z_sorted with
        # a new list under _lock — it never mutates in place — so this
        # local reference is a stable per-tick view. The snapshot dict,
        # _compute_dirty_rect, and the draw loop below must all iterate
        # THIS list: re-reading self._z_sorted in each let a layer
        # created between two reads show up in a later loop but not in
        # `snapshots`, KeyError-ing the frame mid-render (and, before
        # the try/finally below existed, leaking the interop lock —
        # every subsequent begin() then failed while present_gpu kept
        # presenting the last good frame: the whole overlay froze until
        # restart, e.g. "fisheye won't open anymore").
        layers = self._z_sorted
        snapshots: Dict[str, Tuple[int, int, int, int]] = {
            layer.name: (layer.x, layer.y, layer.width, layer.height)
            for layer in layers
        }
        # _sync_host_rgn is a THIRD consumer of layer position (besides
        # _compute_dirty_rect and this method's own draw loop, both of
        # which already share `snapshots` per the comment above) — it
        # used to re-read layer.x/layer.y live, which raced against
        # set_compositor_layer_position() being called from another
        # thread (the desktop pet's plugin Tick, which writes position
        # unsynchronized and at up to 60Hz during walking/dragging — see
        # CompositorLayer.set_position). The clip region computed from a
        # position that had already moved past what was actually drawn
        # this tick — or vice versa — clipped the sprite against the
        # WRONG rect for one frame, visible as a torn/bitten edge
        # exactly on fast motion. Stashing the snapshot here and having
        # _sync_host_rgn consume it (see its call site below) makes all
        # three consumers agree on one immutable position per tick.
        self._last_render_snapshots = snapshots

        dirty_rect = self._compute_dirty_rect(sw, sh, ox, oy, snapshots,
                                              layers)

        # Part A: render straight into the interop-registered D3D11
        # texture when available, so present_frame() can CopyResource
        # it into the swapchain with zero CPU touch. Any failure here
        # (begin returns False) falls straight back to ctx.screen —
        # the exact same path used when interop was never enabled.
        dc = self._dcomp
        gpu_target_active = dc is not None and dc.render_to_gpu_texture_begin()
        try:
            if gpu_target_active:
                # Bind through a moderngl wrap so moderngl's framebuffer
                # state tracking agrees with reality — see the field's
                # comment in __init__ for why a raw bind is not enough.
                gen = dc.gpu_target_generation
                if self._gpu_fbo_wrap is None or self._gpu_fbo_wrap_gen != gen:
                    self._gpu_fbo_wrap = ctx.detect_framebuffer(dc.gpu_fbo_id)
                    self._gpu_fbo_wrap_gen = gen
                self._gpu_fbo_wrap.use()
                ctx.viewport = (0, 0, sw, sh)
            else:
                ctx.screen.use()
                ctx.viewport = (0, 0, sw, sh)
            ctx.clear(0.0, 0.0, 0.0, 0.0)

            from render.dcomp_bridge import (
                gl_bind_texture_unit0, keyed_mutex_acquire,
                keyed_mutex_release)

            self._draw_layers(
                ctx, dc, gpu_target_active, layers, snapshots,
                sw, sh, ox, oy, t,
                gl_bind_texture_unit0, keyed_mutex_acquire,
                keyed_mutex_release)
        finally:
            # MUST run even if a draw throws: begin() locked the interop
            # object, and a leaked lock makes every future begin() fail
            # while present_gpu() keeps presenting the stale gpu texture
            # — the overlay freezes permanently (observed live when the
            # pre-`layers`-capture KeyError above fired).
            if gpu_target_active:
                dc.render_to_gpu_texture_end()

        return dirty_rect

    def _draw_layers(self, ctx, dc, gpu_target_active, layers, snapshots,
                     sw, sh, ox, oy, t,
                     gl_bind_texture_unit0, keyed_mutex_acquire,
                     keyed_mutex_release) -> None:
        for layer in layers:
            if not layer.visible or layer.alpha <= 0.001:
                layer._dirty = False
                continue

            lx, ly, lw, lh = snapshots[layer.name]

            # GPU-shared-texture layers (Part B): color comes straight
            # from an interop-registered external texture — no
            # moderngl Texture object involved, so this branch handles
            # its own bind/draw further down instead of setting a
            # (tex, prog, vao) triple for the shared tail.
            is_shared = (
                layer._shared_tex_handle is not None
                and layer._ensure_shared_texture(dc))
            if is_shared:
                prog = self._shared_prog
                vao = self._quad_vao_shared
            # Render-fn layers: draw to FBO then composite
            elif layer._render_fn is not None:
                layer._ensure_fbo(ctx)
                layer._fbo.use()
                ctx.viewport = (0, 0, layer.width, layer.height)
                ctx.clear(0.0, 0.0, 0.0, 0.0)
                try:
                    layer._render_fn(ctx, t)
                except Exception:
                    pass
                if gpu_target_active:
                    self._gpu_fbo_wrap.use()
                else:
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

            # Compute NDC rect from the snapshotted screen coords (same
            # values _compute_dirty_rect used above)
            rx = (lx - ox) / sw
            ry = 1.0 - (ly - oy + lh) / sh
            rw = lw / sw
            rh = lh / sh

            prog['u_rect'].value = (rx, ry, rw, rh)
            prog['u_alpha'].value = layer.alpha
            if is_shared:
                # Keyed mutex (when the producer created one): hold key 0
                # across the sampled draw so the producer's CopyResource
                # can't overwrite the texture mid-read — that overlap was
                # visible as horizontal tearing on fast pet motion. On the
                # rare acquire timeout (producer died mid-hold) draw
                # unsynced rather than blink the layer out for a frame.
                km = layer._shared_keyed_mutex
                km_held = km is not None and keyed_mutex_acquire(km)
                try:
                    if dc.lock_external_texture(layer._shared_hobj):
                        try:
                            gl_bind_texture_unit0(layer._shared_gl_tex_id)
                            vao.render(moderngl.TRIANGLE_STRIP)
                        finally:
                            dc.unlock_external_texture(layer._shared_hobj)
                finally:
                    if km_held:
                        keyed_mutex_release(km)
            else:
                tex.use(location=0)
                vao.render(moderngl.TRIANGLE_STRIP)

            layer._dirty = False

    def _cleanup_gl(self) -> None:
        dc = self._dcomp
        # Drop the interop-FBO wrap reference only — releasing it would
        # glDeleteFramebuffers the bridge's own FBO out from under it.
        self._gpu_fbo_wrap = None
        self._gpu_fbo_wrap_gen = -1
        for layer in self._layers.values():
            layer._release_gl(dc)
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

