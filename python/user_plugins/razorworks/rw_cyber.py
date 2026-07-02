# -*- coding: utf-8 -*-
"""RW module — Cyber Menu controller.

Spawns the dashboard subprocess (rw_cyber_app.py), discovers its hidden
window, and wires it into the host compositor purely through the plugin
SDK (``ctx.create_compositor_layer`` / ``upload_compositor_frame`` /
``set_compositor_layer_input`` / ``destroy_compositor_layer``). Frame
capture (PrintWindow) and input forwarding (PostMessageW) are plain win32
glue owned by this module — they do not go through ctx because ctx has no
surface for them, but the layer itself is always ctx-managed so the host's
normal per-plugin cleanup can find and destroy it.

Lazy by design: constructing a CyberMenuController does nothing. Only
``open()`` (user clicks "Cyber Menu") spawns the subprocess.
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import threading
import time
import uuid
from collections import deque
from multiprocessing.connection import Client, Listener
from typing import Any, Callable, Optional

_user32 = ctypes.windll.user32
_gdi32 = ctypes.windll.gdi32

PW_RENDERFULLCONTENT = 2  # PrintWindow flag needed for WebView2/DComp content
DIB_RGB_COLORS = 0
BI_RGB = 0

WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_RBUTTONDOWN = 0x0204
WM_RBUTTONUP = 0x0205
WM_MOUSEWHEEL = 0x020A
MK_LBUTTON = 0x0001
MK_RBUTTON = 0x0002


class _BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
        ("biPlanes", wt.WORD), ("biBitCount", wt.WORD),
        ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
        ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG),
        ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD),
    ]


class _BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", _BITMAPINFOHEADER), ("bmiColors", wt.DWORD * 3)]


_user32.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
_user32.PrintWindow.restype = wt.BOOL
_user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
_user32.PostMessageW.restype = wt.BOOL
_user32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
_user32.FindWindowW.restype = wt.HWND
_user32.GetSystemMetrics.argtypes = [ctypes.c_int]
_user32.GetSystemMetrics.restype = ctypes.c_int
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
    """PrintWindow → premultiplied BGRA bytes (top-down), or None."""
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
        bmi.bmiHeader.biHeight = -h
        bmi.bmiHeader.biPlanes = 1
        bmi.bmiHeader.biBitCount = 32
        bmi.bmiHeader.biCompression = BI_RGB

        buf = ctypes.create_string_buffer(w * h * 4)
        got = _gdi32.GetDIBits(hdc_mem, hbmp, 0, h, ctypes.cast(buf, ctypes.c_void_p),
                                ctypes.byref(bmi), DIB_RGB_COLORS)
        if got > 0:
            result = bytes(buf)

    _gdi32.SelectObject(hdc_mem, old)
    _gdi32.DeleteObject(hbmp)
    _gdi32.DeleteDC(hdc_mem)
    _user32.ReleaseDC(0, hdc_screen)
    return result


def _make_lparam(x: float, y: float) -> int:
    return (int(y) & 0xFFFF) << 16 | (int(x) & 0xFFFF)


class CyberMenuController:
    """Owns the Cyber Menu dashboard subprocess + its compositor layer."""

    PUSH_INTERVAL_S = 0.25
    CAPTURE_FPS = 12.0
    LAYER_NAME = "rw_cyber"

    def __init__(self, ctx_provider: Callable[[], Any],
                 snapshot_fn: Callable[[], dict],
                 action_fn: Callable[[dict], None]) -> None:
        self._ctx_provider = ctx_provider
        self._snapshot_fn = snapshot_fn
        self._action_fn = action_fn
        self._lock = threading.RLock()
        self._open = False
        self._width = 960
        self._height = 680
        self._listener: Optional[Listener] = None
        self._conn = None
        self._proc: Optional[subprocess.Popen] = None
        self._hwnd = 0
        self._push_token = ""
        self._reader_thread: Optional[threading.Thread] = None
        self._capture_thread: Optional[threading.Thread] = None
        self._capture_stop = threading.Event()
        self._seq = 0
        self._history = {
            "accuracy": deque(maxlen=60),
            "tick_ms": deque(maxlen=60),
            "entity_count": deque(maxlen=60),
        }

    def is_open(self) -> bool:
        return self._open

    def toggle(self) -> None:
        if self._open:
            self.close()
        else:
            self.open()

    # ── open ────────────────────────────────────────────────────────

    def open(self) -> bool:
        with self._lock:
            if self._open:
                return True
            ctx = self._ctx_provider()
            if ctx is None:
                return False
            try:
                ok = self._open_locked(ctx)
            except Exception as exc:
                try:
                    ctx.log(f"rw cyber: open failed: {exc!r}")
                except Exception:
                    pass
                ok = False
            if not ok:
                self._rollback()
            return ok

    def _open_locked(self, ctx) -> bool:
        authkey = os.urandom(32)
        listener = Listener(("127.0.0.1", 0), authkey=authkey)
        port = listener.address[1]
        title = f"RW-Cyber-{uuid.uuid4().hex[:8]}"
        w, h = self._width, self._height

        proc = self._spawn_subprocess(port, authkey, title, w, h)
        if proc is None:
            listener.close()
            return False
        self._proc = proc

        conn_holder: dict = {}

        def _accept():
            try:
                conn_holder["conn"] = listener.accept()
            except Exception:
                pass

        accept_thread = threading.Thread(target=_accept, daemon=True)
        accept_thread.start()
        accept_thread.join(timeout=8.0)
        conn = conn_holder.get("conn")
        if conn is None:
            listener.close()
            try:
                ctx.notify("RW", "Cyber Menu 连接超时", duration_s=4.0)
            except Exception:
                pass
            return False

        hwnd = self._wait_for_hwnd(title, timeout=8.0)
        if not hwnd:
            try:
                conn.close()
            except Exception:
                pass
            listener.close()
            try:
                ctx.notify("RW", "Cyber Menu 窗口未找到", duration_s=4.0)
            except Exception:
                pass
            return False

        x, y = self._center_position(w, h)
        ctx.create_compositor_layer(self.LAYER_NAME, w, h, x=x, y=y, z=140,
                                     click_through=False)
        ctx.set_compositor_layer_input(
            self.LAYER_NAME,
            cursor_pos_fn=self._on_cursor_pos,
            mouse_button_fn=self._on_mouse_button,
            cursor_leave_fn=lambda: None,
            scroll_fn=self._on_scroll,
        )

        self._listener = listener
        self._conn = conn
        self._hwnd = hwnd
        self._open = True

        self._capture_stop.clear()
        self._capture_thread = threading.Thread(
            target=self._capture_loop, args=(ctx,), daemon=True, name="rw-cyber-capture")
        self._capture_thread.start()

        self._reader_thread = threading.Thread(
            target=self._reader_loop, daemon=True, name="rw-cyber-reader")
        self._reader_thread.start()

        self._push_token = ctx.set_interval(
            lambda: self._push_tick(ctx), self.PUSH_INTERVAL_S)
        try:
            ctx.log(f"rw cyber: opened (hwnd={hwnd:#x})")
        except Exception:
            pass
        return True

    def _spawn_subprocess(self, port: int, authkey: bytes, title: str,
                           w: int, h: int) -> Optional[subprocess.Popen]:
        plugin_dir = os.path.dirname(os.path.abspath(__file__))
        exe_path = os.path.join(plugin_dir, "rw_cyber_app.exe")
        script_path = os.path.join(plugin_dir, "rw_cyber_app.py")
        if os.path.isfile(exe_path):
            cmd = [exe_path]
        else:
            cmd = [sys.executable, script_path]
        cmd += ["--port", str(port), "--authkey", authkey.hex(),
                "--title", title, "--w", str(w), "--h", str(h)]

        env = dict(os.environ)
        env["PYTHONUNBUFFERED"] = "1"

        flags = 0
        if sys.platform == "win32":
            below_normal = 0x00004000
            new_group = 0x00000200
            flags = below_normal | new_group

        try:
            log_dir = os.path.join(os.path.expanduser("~"), ".sao")
            os.makedirs(log_dir, exist_ok=True)
            log_path = os.path.join(log_dir, "rw_cyber_subprocess.log")
            log = open(log_path, "a", encoding="utf-8")
            log.write(f"\n[RW-Cyber] launch cmd={cmd!r}\n")
            log.flush()
            return subprocess.Popen(cmd, cwd=plugin_dir, env=env, creationflags=flags,
                                     stdout=log, stderr=log, close_fds=True)
        except Exception:
            return None

    @staticmethod
    def _wait_for_hwnd(title: str, timeout: float = 8.0) -> int:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            hwnd = _user32.FindWindowW(None, title)
            if hwnd:
                return hwnd
            time.sleep(0.1)
        return 0

    @staticmethod
    def _center_position(w: int, h: int) -> tuple:
        try:
            sw = _user32.GetSystemMetrics(0)
            sh = _user32.GetSystemMetrics(1)
            return max(0, (sw - w) // 2), max(0, (sh - h) // 2)
        except Exception:
            return 100, 100

    # ── close ───────────────────────────────────────────────────────

    def close(self) -> None:
        with self._lock:
            if not self._open:
                return
            self._open = False
            ctx = self._ctx_provider()
            if ctx is not None and self._push_token:
                try:
                    ctx.clear_timer(self._push_token)
                except Exception:
                    pass
            self._push_token = ""
            self._capture_stop.set()
            if self._conn is not None:
                try:
                    self._conn.send({"type": "shutdown"})
                except Exception:
                    pass
            if self._reader_thread is not None:
                self._reader_thread.join(timeout=2.0)
            self._reader_thread = None
            if self._capture_thread is not None:
                self._capture_thread.join(timeout=2.0)
            self._capture_thread = None
            if ctx is not None:
                try:
                    ctx.destroy_compositor_layer(self.LAYER_NAME)
                except Exception:
                    pass
            self._terminate_proc()
            if self._conn is not None:
                try:
                    self._conn.close()
                except Exception:
                    pass
            self._conn = None
            if self._listener is not None:
                try:
                    self._listener.close()
                except Exception:
                    pass
            self._listener = None
            self._hwnd = 0
            if ctx is not None:
                try:
                    ctx.log("rw cyber: closed")
                except Exception:
                    pass

    def shutdown(self) -> None:
        self.close()

    def _rollback(self) -> None:
        self._terminate_proc()
        if self._conn is not None:
            try:
                self._conn.close()
            except Exception:
                pass
            self._conn = None
        if self._listener is not None:
            try:
                self._listener.close()
            except Exception:
                pass
            self._listener = None
        self._hwnd = 0
        self._open = False

    def _terminate_proc(self) -> None:
        proc = self._proc
        self._proc = None
        if proc is None:
            return
        try:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2.0)
                except Exception:
                    proc.kill()
        except Exception:
            pass

    # ── frame capture / input forwarding (plain win32, no ctx surface) ──

    def _capture_loop(self, ctx) -> None:
        interval = 1.0 / self.CAPTURE_FPS
        while not self._capture_stop.is_set():
            hwnd = self._hwnd
            if hwnd:
                bgra = _capture_window(hwnd, self._width, self._height)
                if bgra:
                    try:
                        ctx.upload_compositor_frame(
                            self.LAYER_NAME, bgra, self._width, self._height)
                    except Exception:
                        pass
            self._capture_stop.wait(interval)

    def _on_cursor_pos(self, lx: float, ly: float) -> None:
        if not self._hwnd:
            return
        lp = _make_lparam(lx, ly)
        _user32.PostMessageW(self._hwnd, WM_MOUSEMOVE, 0, lp)

    def _on_mouse_button(self, button: int, action: int, mods: int,
                          lx: float, ly: float) -> None:
        if not self._hwnd:
            return
        lp = _make_lparam(lx, ly)
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
        if not self._hwnd:
            return
        delta = int(dy * 120)
        wp = (delta & 0xFFFF) << 16
        lp = _make_lparam(self._width // 2, self._height // 2)
        _user32.PostMessageW(self._hwnd, WM_MOUSEWHEEL, wp, lp)

    # ── state push / action pull ─────────────────────────────────────

    def _push_tick(self, ctx) -> None:
        if not self._open or self._conn is None:
            return
        try:
            data = self._snapshot_fn()
        except Exception:
            return
        self._sample_history(data)
        overview = data.setdefault("overview", {})
        overview["history"] = {k: list(v) for k, v in self._history.items()}
        self._seq += 1
        msg = {"type": "state", "seq": self._seq}
        msg.update(data)
        try:
            self._conn.send(msg)
        except Exception:
            pass

    def _sample_history(self, data: dict) -> None:
        overview = data.get("overview") or {}
        self._history["accuracy"].append(overview.get("accuracy", 0.0))
        self._history["tick_ms"].append(overview.get("tick_ms", 0))
        self._history["entity_count"].append(overview.get("entity_count", 0))

    def _reader_loop(self) -> None:
        conn = self._conn
        while self._open and conn is not None:
            try:
                msg = conn.recv()
            except (EOFError, OSError):
                break
            except Exception:
                break
            if not isinstance(msg, dict):
                continue
            if msg.get("type") == "action":
                try:
                    self._action_fn(msg)
                except Exception:
                    pass
