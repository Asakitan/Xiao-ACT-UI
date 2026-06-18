# -*- coding: utf-8 -*-
"""webview_proxy — capture pywebview windows and present via compositor.

Instead of showing pywebview windows directly (which creates detectable
overlay HWNDs), this module:
  1. Moves pywebview windows off-screen (invisible to external window enumeration)
  2. Captures their rendered content via PrintWindow → BGRA
  3. Uploads the BGRA to compositor layers (single DWM overlay)
  4. Forwards mouse/keyboard events from the layer to the hidden window

The result: WebView content renders in the unified overlay, no extra
overlay-style windows visible to EnumWindows.
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import threading
import time
from typing import Any, Callable, Dict, List, Optional, Tuple

_user32 = ctypes.windll.user32
_gdi32 = ctypes.windll.gdi32

# ── Win32 capture constants ──────────────────────────────────────
PW_RENDERFULLCONTENT = 2  # PrintWindow flag for WebView2/DComp content
SRCCOPY = 0x00CC0020
DIB_RGB_COLORS = 0
BI_RGB = 0

WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_RBUTTONDOWN = 0x0204
WM_RBUTTONUP = 0x0205
WM_MOUSEWHEEL = 0x020A
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_CHAR = 0x0102

MK_LBUTTON = 0x0001
MK_RBUTTON = 0x0002

SW_HIDE = 0
SW_SHOWNOACTIVATE = 4
SWP_NOMOVE = 0x0002
SWP_NOSIZE = 0x0001
SWP_NOACTIVATE = 0x0010
SWP_NOZORDER = 0x0004


class _BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ('biSize', wt.DWORD),
        ('biWidth', wt.LONG),
        ('biHeight', wt.LONG),
        ('biPlanes', wt.WORD),
        ('biBitCount', wt.WORD),
        ('biCompression', wt.DWORD),
        ('biSizeImage', wt.DWORD),
        ('biXPelsPerMeter', wt.LONG),
        ('biYPelsPerMeter', wt.LONG),
        ('biClrUsed', wt.DWORD),
        ('biClrImportant', wt.DWORD),
    ]


class _BITMAPINFO(ctypes.Structure):
    _fields_ = [
        ('bmiHeader', _BITMAPINFOHEADER),
        ('bmiColors', wt.DWORD * 3),
    ]


# ── Win32 function signatures ────────────────────────────────────
_user32.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
_user32.PrintWindow.restype = wt.BOOL

_user32.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
_user32.SendMessageW.restype = ctypes.c_long

_user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
_user32.PostMessageW.restype = wt.BOOL

_user32.MoveWindow.argtypes = [wt.HWND, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, ctypes.c_int, wt.BOOL]
_user32.MoveWindow.restype = wt.BOOL

_user32.SetWindowPos.argtypes = [
    wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int,
    ctypes.c_int, ctypes.c_int, wt.UINT,
]
_user32.SetWindowPos.restype = wt.BOOL

_user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
_user32.GetWindowRect.restype = wt.BOOL

_gdi32.CreateCompatibleDC.argtypes = [wt.HDC]
_gdi32.CreateCompatibleDC.restype = wt.HDC

_gdi32.CreateCompatibleBitmap.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int]
_gdi32.CreateCompatibleBitmap.restype = wt.HBITMAP

_gdi32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
_gdi32.SelectObject.restype = wt.HGDIOBJ

_gdi32.DeleteObject.argtypes = [wt.HGDIOBJ]
_gdi32.DeleteObject.restype = wt.BOOL

_gdi32.DeleteDC.argtypes = [wt.HDC]
_gdi32.DeleteDC.restype = wt.BOOL

_gdi32.GetDIBits.argtypes = [
    wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT,
    ctypes.c_void_p, ctypes.POINTER(_BITMAPINFO), wt.UINT,
]
_gdi32.GetDIBits.restype = ctypes.c_int


def _capture_window(hwnd: int, w: int, h: int) -> Optional[bytes]:
    """Capture a window's content via PrintWindow → BGRA bytes.

    Returns premultiplied BGRA bytes (top-down) or None on failure.
    Uses PW_RENDERFULLCONTENT to capture WebView2/DComp content.
    """
    hdc_screen = _user32.GetDC(0)
    hdc_mem = _gdi32.CreateCompatibleDC(hdc_screen)
    hbmp = _gdi32.CreateCompatibleBitmap(hdc_screen, w, h)
    old = _gdi32.SelectObject(hdc_mem, hbmp)

    ok = _user32.PrintWindow(hwnd, hdc_mem, PW_RENDERFULLCONTENT)
    if not ok:
        ok = _user32.PrintWindow(hwnd, hdc_mem, 0)

    result = None
    if ok:
        bmi = _BITMAPINFO()
        bmi.bmiHeader.biSize = ctypes.sizeof(_BITMAPINFOHEADER)
        bmi.bmiHeader.biWidth = w
        bmi.bmiHeader.biHeight = -h  # top-down
        bmi.bmiHeader.biPlanes = 1
        bmi.bmiHeader.biBitCount = 32
        bmi.bmiHeader.biCompression = BI_RGB

        buf = ctypes.create_string_buffer(w * h * 4)
        got = _gdi32.GetDIBits(
            hdc_mem, hbmp, 0, h,
            ctypes.cast(buf, ctypes.c_void_p),
            ctypes.byref(bmi), DIB_RGB_COLORS,
        )
        if got > 0:
            result = bytes(buf)

    _gdi32.SelectObject(hdc_mem, old)
    _gdi32.DeleteObject(hbmp)
    _gdi32.DeleteDC(hdc_mem)
    _user32.ReleaseDC(0, hdc_screen)
    return result


def _make_lparam(x: int, y: int) -> int:
    """Pack (x, y) into LPARAM for mouse messages."""
    return (y & 0xFFFF) << 16 | (x & 0xFFFF)


# ── WebViewProxy ─────────────────────────────────────────────────
class WebViewProxy:
    """Proxies a pywebview window through the unified overlay compositor.

    Captures the window's rendered content and presents it as a
    compositor layer. Forwards input events from the layer to the
    hidden window.
    """

    def __init__(self, hwnd: int, name: str,
                 width: int, height: int,
                 screen_x: int, screen_y: int,
                 z: int = 150,
                 click_through: bool = True,
                 capture_fps: float = 15.0):
        self._hwnd = hwnd
        self._name = name
        self._width = width
        self._height = height
        self._screen_x = screen_x
        self._screen_y = screen_y
        self._z = z
        self._click_through = click_through
        self._capture_interval = 1.0 / max(1.0, capture_fps)
        self._layer = None
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._stop_evt = threading.Event()
        self._visible = False

    def start(self) -> None:
        """Start capturing and presenting the webview window."""
        if self._running:
            return

        from render.gpu_overlay_window import _get_unified_overlay
        uo = _get_unified_overlay()

        self._layer = uo.create_layer(
            f'wv_{self._name}',
            width=self._width,
            height=self._height,
            x=self._screen_x,
            y=self._screen_y,
            z=self._z,
            click_through=self._click_through,
        )

        if not self._click_through:
            self._layer.set_input_callbacks(
                cursor_pos_fn=self._on_cursor_pos,
                mouse_button_fn=self._on_mouse_button,
                scroll_fn=self._on_scroll,
            )

        # Move the real window off-screen
        _user32.MoveWindow(self._hwnd, -10000, -10000,
                           self._width, self._height, False)

        self._running = True
        self._stop_evt.clear()
        self._thread = threading.Thread(
            target=self._capture_loop, daemon=True,
            name=f'wv-proxy-{self._name}',
        )
        self._thread.start()

    def stop(self) -> None:
        self._running = False
        self._stop_evt.set()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=3.0)
        self._thread = None
        if self._layer is not None:
            try:
                from render.gpu_overlay_window import _get_unified_overlay
                _get_unified_overlay().destroy_layer(f'wv_{self._name}')
            except Exception:
                pass
            self._layer = None

    def show(self) -> None:
        self._visible = True
        if self._layer:
            self._layer.show()

    def hide(self) -> None:
        self._visible = False
        if self._layer:
            self._layer.hide()

    def set_geometry(self, x: int, y: int, w: int, h: int) -> None:
        self._screen_x = x
        self._screen_y = y
        changed = (w != self._width or h != self._height)
        self._width = w
        self._height = h
        if self._layer:
            self._layer.set_geometry(x, y, w, h)
        if changed:
            _user32.MoveWindow(self._hwnd, -10000, -10000, w, h, True)

    def set_click_through(self, ct: bool) -> None:
        self._click_through = ct
        if self._layer:
            self._layer.click_through = ct

    # ── Capture loop ─────────────────────────────────────────

    def _capture_loop(self) -> None:
        while self._running:
            if self._visible and self._layer:
                bgra = _capture_window(
                    self._hwnd, self._width, self._height)
                if bgra:
                    self._layer.upload_bgra(bgra, self._width, self._height)
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
        if button == 1:
            msg = WM_LBUTTONDOWN if action == 1 else WM_LBUTTONUP
            wp = MK_LBUTTON if action == 1 else 0
        elif button == 2:
            msg = WM_RBUTTONDOWN if action == 1 else WM_RBUTTONUP
            wp = MK_RBUTTON if action == 1 else 0
        else:
            return
        _user32.PostMessageW(self._hwnd, msg, wp, lp)

    def _on_scroll(self, dx: float, dy: float) -> None:
        delta = int(dy * 120)
        wp = (delta & 0xFFFF) << 16
        lp = _make_lparam(self._width // 2, self._height // 2)
        _user32.PostMessageW(self._hwnd, WM_MOUSEWHEEL, wp, lp)


# ── Registry ─────────────────────────────────────────────────────
_proxies: Dict[str, WebViewProxy] = {}
_proxy_lock = threading.Lock()


def register_webview_proxy(
    hwnd: int, name: str,
    width: int, height: int,
    screen_x: int, screen_y: int,
    z: int = 150,
    click_through: bool = True,
    capture_fps: float = 15.0,
) -> WebViewProxy:
    """Register a pywebview window for compositor proxying."""
    with _proxy_lock:
        if name in _proxies:
            _proxies[name].stop()
        proxy = WebViewProxy(
            hwnd, name, width, height, screen_x, screen_y,
            z, click_through, capture_fps,
        )
        _proxies[name] = proxy
    return proxy


def unregister_webview_proxy(name: str) -> None:
    with _proxy_lock:
        proxy = _proxies.pop(name, None)
    if proxy:
        proxy.stop()


def get_webview_proxy(name: str) -> Optional[WebViewProxy]:
    return _proxies.get(name)


def stop_all_proxies() -> None:
    with _proxy_lock:
        names = list(_proxies.keys())
    for name in names:
        unregister_webview_proxy(name)
