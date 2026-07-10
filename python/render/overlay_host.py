# -*- coding: utf-8 -*-
# overlay_host — single global DWM overlay window (raw Win32 + WGL).
#
# Replaces the per-panel GLFW window model with ONE full-screen
# transparent window. All rendering layers compose into a single
# framebuffer; DWM presents the alpha-composited result.
#
# Window behavior:
# - Random class name from normal-looking Windows pool
# - Click-through via WM_NCHITTEST → HTTRANSPARENT (per-pixel)
# - No-activate via WM_MOUSEACTIVATE → MA_NOACTIVATE
# - Transparency via DWM glass (no WS_EX_LAYERED)
# - Topmost pulsed via SetWindowPos, configurable
# - Configurable capture exclusion for streaming mode
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import os
import random
import struct
import sys
import threading
import time
from ctypes import POINTER, WINFUNCTYPE, byref, sizeof
from typing import Any, Callable, Dict, List, Optional, Tuple

if sys.platform != 'win32':
    raise ImportError('overlay_host requires Windows')

# ── Win32 constants ──────────────────────────────────────────────
_user32 = ctypes.windll.user32
_gdi32 = ctypes.windll.gdi32
_kernel32 = ctypes.windll.kernel32
_opengl32 = ctypes.windll.opengl32
_dwmapi = ctypes.windll.dwmapi

WS_POPUP = 0x80000000
WS_VISIBLE = 0x10000000
WS_CLIPSIBLINGS = 0x04000000
WS_CLIPCHILDREN = 0x02000000
WS_EX_TOPMOST = 0x00000008
WS_EX_TRANSPARENT = 0x00000020
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_NOACTIVATE = 0x08000000
WS_EX_NOREDIRECTIONBITMAP = 0x00200000
WS_EX_LAYERED = 0x00080000

WM_NCHITTEST = 0x0084
WM_ERASEBKGND = 0x0014
WM_MOUSEACTIVATE = 0x0021
WM_DESTROY = 0x0002
WM_SIZE = 0x0005
WM_DISPLAYCHANGE = 0x007E
WM_DPICHANGED = 0x02E0
WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_RBUTTONDOWN = 0x0204
WM_RBUTTONUP = 0x0205
WM_MOUSEWHEEL = 0x020A
WM_MOUSELEAVE = 0x02A3
TME_LEAVE = 0x00000002

HTTRANSPARENT = -1
HTCLIENT = 1
MA_NOACTIVATE = 3

PM_REMOVE = 0x0001
CS_OWNDC = 0x0020
CS_VREDRAW = 0x0001
CS_HREDRAW = 0x0002

IDC_ARROW = 32512

SM_CXSCREEN = 0
SM_CYSCREEN = 1
SM_XVIRTUALSCREEN = 76
SM_YVIRTUALSCREEN = 77
SM_CXVIRTUALSCREEN = 78
SM_CYVIRTUALSCREEN = 79

SWP_NOMOVE = 0x0002
SWP_NOSIZE = 0x0001
SWP_NOACTIVATE = 0x0010
SWP_SHOWWINDOW = 0x0040

HWND_TOPMOST = wt.HWND(-1)
HWND_TOP = wt.HWND(0)

SW_SHOWNOACTIVATE = 4

GWL_EXSTYLE = -20
GWLP_HWNDPARENT = -8

PFD_DRAW_TO_WINDOW = 0x00000004
PFD_SUPPORT_OPENGL = 0x00000020
PFD_DOUBLEBUFFER = 0x00000001
PFD_SUPPORT_COMPOSITION = 0x00008000
PFD_TYPE_RGBA = 0
PFD_MAIN_PLANE = 0

LWA_ALPHA = 0x00000002

# ── Win32 structures ─────────────────────────────────────────────
class _WNDCLASSEXW(ctypes.Structure):
    _fields_ = [
        ('cbSize', wt.UINT),
        ('style', wt.UINT),
        ('lpfnWndProc', ctypes.c_void_p),
        ('cbClsExtra', ctypes.c_int),
        ('cbWndExtra', ctypes.c_int),
        ('hInstance', wt.HINSTANCE),
        ('hIcon', wt.HICON),
        ('hCursor', wt.HANDLE),
        ('hbrBackground', wt.HBRUSH),
        ('lpszMenuName', wt.LPCWSTR),
        ('lpszClassName', wt.LPCWSTR),
        ('hIconSm', wt.HICON),
    ]


class _PIXELFORMATDESCRIPTOR(ctypes.Structure):
    _fields_ = [
        ('nSize', wt.WORD),
        ('nVersion', wt.WORD),
        ('dwFlags', wt.DWORD),
        ('iPixelType', wt.BYTE),
        ('cColorBits', wt.BYTE),
        ('cRedBits', wt.BYTE), ('cRedShift', wt.BYTE),
        ('cGreenBits', wt.BYTE), ('cGreenShift', wt.BYTE),
        ('cBlueBits', wt.BYTE), ('cBlueShift', wt.BYTE),
        ('cAlphaBits', wt.BYTE), ('cAlphaShift', wt.BYTE),
        ('cAccumBits', wt.BYTE),
        ('cAccumRedBits', wt.BYTE), ('cAccumGreenBits', wt.BYTE),
        ('cAccumBlueBits', wt.BYTE), ('cAccumAlphaBits', wt.BYTE),
        ('cDepthBits', wt.BYTE),
        ('cStencilBits', wt.BYTE),
        ('cAuxBuffers', wt.BYTE),
        ('iLayerType', wt.BYTE),
        ('bReserved', wt.BYTE),
        ('dwLayerMask', wt.DWORD),
        ('dwVisibleMask', wt.DWORD),
        ('dwDamageMask', wt.DWORD),
    ]


class _MARGINS(ctypes.Structure):
    _fields_ = [
        ('cxLeftWidth', ctypes.c_int),
        ('cxRightWidth', ctypes.c_int),
        ('cyTopHeight', ctypes.c_int),
        ('cyBottomHeight', ctypes.c_int),
    ]


class _MSG(ctypes.Structure):
    _fields_ = [
        ('hwnd', wt.HWND),
        ('message', wt.UINT),
        ('wParam', wt.WPARAM),
        ('lParam', wt.LPARAM),
        ('time', wt.DWORD),
        ('pt_x', wt.LONG),
        ('pt_y', wt.LONG),
    ]


# ── Win32 function signatures ────────────────────────────────────
WNDPROC = WINFUNCTYPE(ctypes.c_long, wt.HWND, wt.UINT,
                       wt.WPARAM, wt.LPARAM)

_user32.RegisterClassExW.argtypes = [POINTER(_WNDCLASSEXW)]
_user32.RegisterClassExW.restype = wt.ATOM

_user32.CreateWindowExW.argtypes = [
    wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
    wt.HWND, wt.HMENU, wt.HINSTANCE, ctypes.c_void_p,
]
_user32.CreateWindowExW.restype = wt.HWND

_user32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT,
                                    wt.WPARAM, wt.LPARAM]
_user32.DefWindowProcW.restype = ctypes.c_long

_user32.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
_user32.ShowWindow.restype = wt.BOOL

_user32.DestroyWindow.argtypes = [wt.HWND]
_user32.DestroyWindow.restype = wt.BOOL

_user32.SetWindowPos.argtypes = [
    wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int,
    ctypes.c_int, ctypes.c_int, wt.UINT,
]
_user32.SetWindowPos.restype = wt.BOOL

_user32.GetDC.argtypes = [wt.HWND]
_user32.GetDC.restype = wt.HDC

_user32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
_user32.ReleaseDC.restype = ctypes.c_int

_user32_pump = ctypes.WinDLL('user32', use_last_error=True)
_user32_pump.PeekMessageW.argtypes = [
    POINTER(_MSG), wt.HWND, wt.UINT, wt.UINT, wt.UINT,
]
_user32_pump.PeekMessageW.restype = wt.BOOL
_user32_pump.TranslateMessage.argtypes = [POINTER(_MSG)]
_user32_pump.TranslateMessage.restype = wt.BOOL
_user32_pump.DispatchMessageW.argtypes = [POINTER(_MSG)]
_user32_pump.DispatchMessageW.restype = ctypes.c_long


_user32.GetSystemMetrics.argtypes = [ctypes.c_int]
_user32.GetSystemMetrics.restype = ctypes.c_int

_user32.LoadCursorW.argtypes = [wt.HINSTANCE, wt.LPCWSTR]
_user32.LoadCursorW.restype = wt.HANDLE

_user32.SetLayeredWindowAttributes.argtypes = [
    wt.HWND, wt.DWORD, wt.BYTE, wt.DWORD,
]
_user32.SetLayeredWindowAttributes.restype = wt.BOOL

_user32.SetWindowLongPtrW.argtypes = [wt.HWND, ctypes.c_int,
                                       ctypes.c_long]
_user32.SetWindowLongPtrW.restype = ctypes.c_long

_user32.GetWindowLongPtrW.argtypes = [wt.HWND, ctypes.c_int]
_user32.GetWindowLongPtrW.restype = ctypes.c_long

_gdi32.ChoosePixelFormat.argtypes = [
    wt.HDC, POINTER(_PIXELFORMATDESCRIPTOR),
]
_gdi32.ChoosePixelFormat.restype = ctypes.c_int

_gdi32.SetPixelFormat.argtypes = [
    wt.HDC, ctypes.c_int, POINTER(_PIXELFORMATDESCRIPTOR),
]
_gdi32.SetPixelFormat.restype = wt.BOOL

_gdi32.SwapBuffers.argtypes = [wt.HDC]
_gdi32.SwapBuffers.restype = wt.BOOL

_opengl32.wglCreateContext.argtypes = [wt.HDC]
_opengl32.wglCreateContext.restype = ctypes.c_void_p

_opengl32.wglMakeCurrent.argtypes = [wt.HDC, ctypes.c_void_p]
_opengl32.wglMakeCurrent.restype = wt.BOOL

_opengl32.wglDeleteContext.argtypes = [ctypes.c_void_p]
_opengl32.wglDeleteContext.restype = wt.BOOL

_dwmapi.DwmExtendFrameIntoClientArea.argtypes = [
    wt.HWND, POINTER(_MARGINS),
]
_dwmapi.DwmExtendFrameIntoClientArea.restype = ctypes.c_long

_dwmapi.DwmEnableBlurBehindWindow.argtypes = [wt.HWND, ctypes.c_void_p]
_dwmapi.DwmEnableBlurBehindWindow.restype = ctypes.c_long

_gdi32.CreateRectRgn.argtypes = [
    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
]
_gdi32.CreateRectRgn.restype = wt.HANDLE

_kernel32.GetModuleHandleW.argtypes = [wt.LPCWSTR]
_kernel32.GetModuleHandleW.restype = wt.HINSTANCE


# ── tagWND diagnostic dump (SAO_DUMP_TW=1) ──────────────────────
# 环境变量守护的自诊: 读回 tagWND 里的 rcWindow / ExStyle, 与
# GetWindowRect / GetWindowLong 的用户可见值对比, 追加到 dump 文件.
# 用来验证 hide_window_rect / hide_exstyle 有没有真正把物理内存写下去.

_TW_DUMP_PATH = os.path.join(
    os.environ.get('TEMP', os.path.expanduser('~')), 'sao_tw_dump.jsonl')


def _tw_dump_snapshot(hwnd: int, phase: str, **extra) -> None:
    import json
    import time as _t
    record = {'phase': phase, 'hwnd': int(hwnd), 'ts': _t.time()}
    record.update(extra)

    try:
        from mem_probe import _dc
        record['exs_off'] = _dc._exs_off
        record['rect_off'] = _dc._rect_off
        record['cal_done'] = _dc._cal_done
    except Exception as e:
        record['dc_probe_err'] = repr(e)

    try:
        from mem_probe import rt_io as _rt_io_mod
        record['rt_io_module'] = getattr(_rt_io_mod, '__name__', '?')
        record['rt_io_file'] = getattr(_rt_io_mod, '__file__', '?')
        record['has_pw'] = callable(getattr(_rt_io_mod, '_pw', None))
        record['has_tw'] = callable(getattr(_rt_io_mod, '_tw', None))
        record['has_hmv_addr'] = callable(getattr(_rt_io_mod, '_hmv_addr', None))
        record['rt_io_connected'] = getattr(_rt_io_mod, '_connected', None)
    except Exception as e:
        record['rt_io_probe_err'] = repr(e)

    try:
        rect = wt.RECT()
        _user32.GetWindowRect(hwnd, byref(rect))
        record['user_rect'] = [rect.left, rect.top, rect.right, rect.bottom]
    except Exception as e:
        record['user_rect_err'] = repr(e)

    try:
        exs = _user32.GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
        record['user_exstyle'] = int(exs) & 0xFFFFFFFF
    except Exception as e:
        record['user_exstyle_err'] = repr(e)

    try:
        from mem_probe import _dc
        tw = _dc._get_tw(hwnd)
        record['tagwnd_addr'] = tw
        if tw and _dc._rect_off >= 0:
            rc = _dc._read_tw_bytes(tw, _dc._rect_off, 16)
            if rc and len(rc) == 16:
                l, t, r, b = struct.unpack('<iiii', rc)
                record['tagwnd_rect'] = [l, t, r, b]
            else:
                record['tagwnd_rect_read_fail'] = True
        if tw and _dc._exs_off >= 0:
            es = _dc._read_tw_bytes(tw, _dc._exs_off, 4)
            if es and len(es) == 4:
                record['tagwnd_exstyle'] = struct.unpack('<I', es)[0]
            else:
                record['tagwnd_exstyle_read_fail'] = True
    except Exception as e:
        record['tagwnd_probe_err'] = repr(e)

    try:
        with open(_TW_DUMP_PATH, 'a', encoding='utf-8') as f:
            f.write(json.dumps(record, ensure_ascii=False) + '\n')
    except Exception:
        pass


# ── Class-name generation ────────────────────────────────────────
# Pool looks like legitimate Windows component class names.
# At startup, pick one and append a random hex suffix.
def _generate_class_name() -> str:
    try:
        from _sao_cy_wnd import _w1
        return _w1()
    except ImportError:
        pass
    import uuid
    return '{%s}' % str(uuid.uuid4()).upper()


# ── OverlayHost ──────────────────────────────────────────────────
class OverlayHost:
    # Single global DWM overlay window with raw Win32 + WGL + ModernGL.
    #
    # Creates a full-screen transparent popup window. All UI layers
    # (panels, menus, effects) render into this window's single GL
    # context via the compositor.
    #
    # Thread safety: create() and destroy() must be called from the
    # overlay thread. process_messages() and swap_buffers() are also
    # overlay-thread-only.

    def __init__(self, width: int = 0, height: int = 0,
                 dc_mutations=None):
        if width <= 0:
            width = _user32.GetSystemMetrics(SM_CXSCREEN)
        if height <= 0:
            height = _user32.GetSystemMetrics(SM_CYSCREEN)
        self.width = width
        self.height = height
        self.origin_x = 0
        self.origin_y = 0

        self.hwnd: int = 0
        self.hdc: int = 0
        self.hglrc: int = 0
        # Whether the window is currently click-through everywhere
        # (WS_EX_TRANSPARENT set). The window is created with that style
        # (see _create_window), so it starts True. set_input_passthrough
        # keeps this in sync — the compositor reads it to decide whether
        # a per-pixel SetWindowRgn clip region is needed at all (it isn't
        # while the whole window passes clicks through).
        self.input_passthrough: bool = True
        self.ctx: Any = None  # moderngl.Context
        self._owner_hwnd: int = 0
        # 2026-07-10 双 hwnd (方案 B, α 语义): self.hwnd 保持指向 hRender
        # (全屏, DComp target), 所有现有消费者行为不变. control_hwnd 是
        # 1x1 诱饵 — 保留在 z-order chain 里让 EnumWindows 扫到, 反作弊
        # 看到"进程有个 1x1 无害窗口" 假象. hRender 走 hide_z_order 从
        # chain unlink, 反作弊扫不到.
        self.control_hwnd: int = 0
        self._class_name: str = ''
        self._class_atom: int = 0
        self._wndproc_ref: Any = None  # prevent GC of ctypes callback
        self._destroyed = False
        self._capture_excluded = False
        self._dc_mutations = dc_mutations

        # Hit-test callback: (screen_x, screen_y) -> bool (True=interactive)
        self.hit_test_fn: Optional[Callable[[int, int], bool]] = None

        # Mouse event callback: (msg_type, screen_x, screen_y, button, delta)
        # msg_type: WM_MOUSEMOVE, WM_LBUTTONDOWN, etc.
        self.mouse_fn: Optional[
            Callable[[int, int, int, int, int], None]
        ] = None
        self._tracking_leave = False

    def _submit_dc(self, operation: str, method_name: str,
                   *args, **kwargs) -> bool:
        coordinator = self._dc_mutations
        if coordinator is not None:
            try:
                return coordinator.submit_dc(
                    self.hwnd, operation, method_name, *args, **kwargs)
            except Exception:
                return False
        try:
            from mem_probe import _dc
            fn = getattr(_dc, method_name, None)
            return bool(callable(fn) and fn(self.hwnd, *args, **kwargs))
        except Exception:
            return False

    def create(self) -> 'OverlayHost':
        # Create the overlay window, WGL context, and ModernGL context.
        if self.hwnd:
            return self
        self._create_window()
        desired_exstyle = (
            WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW
            | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP)
        style_ok = False
        try:
            from mem_probe._dc import OVERLAY_EXSTYLE_MASK, \
                syscall_set_window_long
            style_ok = syscall_set_window_long(
                self.hwnd, -20, desired_exstyle)
        except Exception:
            OVERLAY_EXSTYLE_MASK = desired_exstyle
        if not style_ok:
            try:
                _user32.SetWindowLongPtrW(
                    self.hwnd, -20, desired_exstyle)
                style_ok = int(_user32.GetWindowLongPtrW(
                    self.hwnd, -20)) == int(desired_exstyle)
            except Exception:
                style_ok = False
        if not style_ok:
            raise OSError('failed to establish host passthrough exstyle')
        try:
            _dump_tw = os.environ.get('SAO_DUMP_TW') == '1'
            if _dump_tw:
                _tw_dump_snapshot(self.hwnd, phase='before_hide')
            _hs_ok = self._submit_dc(
                'host-exstyle', 'hide_exstyle', OVERLAY_EXSTYLE_MASK)
            _hr_ok = self._submit_dc(
                'host-rect', 'hide_window_rect')
            if _dump_tw:
                _tw_dump_snapshot(self.hwnd, phase='after_hide',
                                  hide_exstyle_ret=_hs_ok,
                                  hide_window_rect_ret=_hr_ok)
        except Exception:
            pass
        # Serialize WGL context creation against any other thread doing
        # WGL work concurrently (GLFW pump, moderngl standalone contexts
        # e.g. fisheye's worker). Without this, two threads racing
        # wglCreateContext/wglMakeCurrent on the same GPU driver can
        # corrupt each other's tracked current-context state, surfacing
        # as spurious "WGL: Failed to clear current context: handle
        # invalid" errors from GLFW elsewhere in the process.
        try:
            from render.gpu_overlay_window import get_wgl_serialize_lock
            lock = get_wgl_serialize_lock()
        except Exception:
            import contextlib
            lock = contextlib.nullcontext()
        with lock:
            self._setup_wgl()
        self._setup_dwm()
        return self

    def _create_window(self) -> None:
        hinst = _kernel32.GetModuleHandleW(None)
        self._class_name = _generate_class_name()

        # WndProc — handle hit-testing, activation, and mouse events.
        # 三窗口共享同一 wndproc: hRender (self.hwnd, 真交互), hControl
        # (self.control_hwnd, 1x1 decoy), _owner_hwnd (invisible owner).
        # 只有 hRender 应该跑 hit_test/mouse_fn 逻辑, 其他两个必须走
        # DefWindowProc.
        #
        # 陷阱: hControl/owner 都建在 hRender 之前, 期间 self.hwnd=0,
        # "if self.hwnd and hwnd != self.hwnd" 会假成短路 → hControl/owner
        # 的早期消息 (WM_NCCREATE/WM_NCCALCSIZE/WM_CREATE/WM_MOUSEACTIVATE)
        # 全落到 hRender 的交互分支. 今天 hit_test_fn=None + mouse_fn=None
        # 撞不上, 明天加一行日志就炸. 显式检查: 只有 hwnd == self.hwnd
        # (已建成的 hRender) 才走交互分支.
        def _wndproc(hwnd: int, msg: int, wp: int, lp: int) -> int:
            if not self.hwnd or hwnd != self.hwnd:
                return _user32.DefWindowProcW(hwnd, msg, wp, lp)
            if msg == WM_NCHITTEST:
                x = lp & 0xFFFF
                if x > 0x7FFF:
                    x -= 0x10000
                y = (lp >> 16) & 0xFFFF
                if y > 0x7FFF:
                    y -= 0x10000
                if self.hit_test_fn and self.hit_test_fn(x, y):
                    return HTCLIENT
                return HTTRANSPARENT
            if msg == WM_MOUSEACTIVATE:
                return MA_NOACTIVATE
            if msg == WM_ERASEBKGND:
                return 1
            if msg in (WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP,
                       WM_RBUTTONDOWN, WM_RBUTTONUP, WM_MOUSEWHEEL,
                       WM_MOUSELEAVE):
                if self.mouse_fn is not None:
                    if msg == WM_MOUSEWHEEL:
                        delta = ctypes.c_short((wp >> 16) & 0xFFFF).value
                        pt_x = lp & 0xFFFF
                        if pt_x > 0x7FFF:
                            pt_x -= 0x10000
                        pt_y = (lp >> 16) & 0xFFFF
                        if pt_y > 0x7FFF:
                            pt_y -= 0x10000
                        self.mouse_fn(msg, pt_x, pt_y, 0, delta)
                    elif msg == WM_MOUSELEAVE:
                        self._tracking_leave = False
                        self.mouse_fn(msg, 0, 0, 0, 0)
                    else:
                        cx = lp & 0xFFFF
                        if cx > 0x7FFF:
                            cx -= 0x10000
                        cy = (lp >> 16) & 0xFFFF
                        if cy > 0x7FFF:
                            cy -= 0x10000
                        sx = cx + self.origin_x
                        sy = cy + self.origin_y
                        # GLFW convention: 0=left, 1=right, 2=middle
                        button = 0
                        if msg in (WM_LBUTTONDOWN, WM_LBUTTONUP):
                            button = 0  # GLFW_MOUSE_BUTTON_LEFT
                        elif msg in (WM_RBUTTONDOWN, WM_RBUTTONUP):
                            button = 1  # GLFW_MOUSE_BUTTON_RIGHT
                        self.mouse_fn(msg, sx, sy, button, 0)
                    if msg == WM_MOUSEMOVE and not self._tracking_leave:
                        self._request_leave_tracking(hwnd)
                return 0
            return _user32.DefWindowProcW(hwnd, msg, wp, lp)

        self._wndproc_ref = WNDPROC(_wndproc)

        wc = _WNDCLASSEXW()
        wc.cbSize = sizeof(_WNDCLASSEXW)
        wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW
        wc.lpfnWndProc = ctypes.cast(self._wndproc_ref, ctypes.c_void_p)
        wc.hInstance = hinst
        wc.hCursor = _user32.LoadCursorW(None, wt.LPCWSTR(IDC_ARROW))
        wc.lpszClassName = self._class_name

        self._class_atom = _user32.RegisterClassExW(byref(wc))
        if not self._class_atom:
            raise OSError(
                f'RegisterClassExW failed: {ctypes.GetLastError()}'
            )

        # Invisible owner to suppress taskbar button
        self._owner_hwnd = _user32.CreateWindowExW(
            0, self._class_name, '', WS_POPUP,
            0, 0, 1, 1, None, None, hinst, None,
        )
        if not self._owner_hwnd:
            raise OSError(
                f'owner CreateWindowExW failed: {ctypes.GetLastError()}'
            )

        # hControl (1x1 decoy) — 反作弊 EnumWindows 会扫到, 拿到 GetWindowRect
        # 是 1x1, 表现为"无害小窗". 不做 DComp 绑定 / 不做实际渲染 / 不
        # unlink z-order. TOOLWINDOW + NOACTIVATE 让它不进 Alt+Tab, 不抢焦点.
        self.control_hwnd = _user32.CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            self._class_name, '',
            WS_POPUP,
            0, 0, 1, 1,   # 真实 1x1 rcWindow, 不用擦 tagWND
            self._owner_hwnd,
            None, hinst, None,
        )
        if not self.control_hwnd:
            raise OSError(
                f'hControl CreateWindowExW failed: {ctypes.GetLastError()}'
            )

        # hRender (self.hwnd, α 语义) — 全屏, DComp target 挂这里, rcWindow
        # 保持真实全屏, DWM 合成不受影响. 用 hide_z_order 从 chain unlink
        # 让 EnumWindows 扫不到 (existing tick 逻辑仍然对 self.hwnd 生效).
        self.hwnd = _user32.CreateWindowExW(
            WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            self._class_name,
            '',
            WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
            self.origin_x, self.origin_y,
            self.width, self.height,
            self._owner_hwnd,
            None, hinst, None,
        )
        if not self.hwnd:
            raise OSError(
                f'CreateWindowExW failed: {ctypes.GetLastError()}'
            )
        print(f'[OverlayHost] hRender=0x{self.hwnd:08X} '
              f'hControl=0x{(self.control_hwnd or 0):08X} '
              f'owner=0x{(self._owner_hwnd or 0):08X} pid={os.getpid()}',
              flush=True)

    def _setup_wgl(self) -> None:
        # Set up WGL pixel format + OpenGL context with alpha support.
        #
        # Uses wglChoosePixelFormatARB (via bootstrap context) to
        # guarantee 8-bit alpha + DWM composition support. The basic
        # ChoosePixelFormat is unreliable — some GPU drivers return a
        # format without alpha, causing the window to render as opaque
        # black instead of transparent.
        self.hdc = _user32.GetDC(self.hwnd)
        if not self.hdc:
            raise OSError('GetDC failed')

        pf = self._try_arb_pixel_format()
        if not pf:
            pf = self._fallback_pixel_format()
        if not pf:
            raise OSError('No suitable pixel format with alpha')

        pfd = _PIXELFORMATDESCRIPTOR()
        pfd.nSize = sizeof(_PIXELFORMATDESCRIPTOR)
        if not _gdi32.SetPixelFormat(self.hdc, pf, byref(pfd)):
            raise OSError('SetPixelFormat failed')

        self.hglrc = _opengl32.wglCreateContext(self.hdc)
        if not self.hglrc:
            raise OSError('wglCreateContext failed')

        _opengl32.wglMakeCurrent(self.hdc, self.hglrc)

        try:
            _wglGetProcAddress = _opengl32.wglGetProcAddress
            _wglGetProcAddress.restype = ctypes.c_void_p
            _wglGetProcAddress.argtypes = [ctypes.c_char_p]
            _addr = _wglGetProcAddress(b'wglSwapIntervalEXT')
            if _addr:
                _fn = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int)(_addr)
                _fn(0)
        except Exception:
            pass

        import moderngl
        self.ctx = moderngl.create_context()
        self.ctx.enable(moderngl.BLEND)
        self.ctx.blend_func = (moderngl.ONE,
                               moderngl.ONE_MINUS_SRC_ALPHA)

    def _try_arb_pixel_format(self) -> int:
        # Use wglChoosePixelFormatARB for guaranteed alpha support.
        #
        # Requires a bootstrap dummy context to get the extension.
        # Returns pixel format index or 0 on failure.
        try:
            hinst = _kernel32.GetModuleHandleW(None)
            # Dummy window for bootstrap WGL context
            dummy = _user32.CreateWindowExW(
                0, self._class_name, '', WS_POPUP,
                0, 0, 1, 1, None, None, hinst, None,
            )
            if not dummy:
                return 0
            dummy_dc = _user32.GetDC(dummy)

            pfd = _PIXELFORMATDESCRIPTOR()
            pfd.nSize = sizeof(_PIXELFORMATDESCRIPTOR)
            pfd.nVersion = 1
            pfd.dwFlags = (PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL
                           | PFD_DOUBLEBUFFER)
            pfd.iPixelType = PFD_TYPE_RGBA
            pfd.cColorBits = 32
            pfd.cAlphaBits = 8

            tmp_pf = _gdi32.ChoosePixelFormat(dummy_dc, byref(pfd))
            if not tmp_pf:
                _user32.ReleaseDC(dummy, dummy_dc)
                _user32.DestroyWindow(dummy)
                return 0
            _gdi32.SetPixelFormat(dummy_dc, tmp_pf, byref(pfd))
            tmp_rc = _opengl32.wglCreateContext(dummy_dc)
            if not tmp_rc:
                _user32.ReleaseDC(dummy, dummy_dc)
                _user32.DestroyWindow(dummy)
                return 0
            _opengl32.wglMakeCurrent(dummy_dc, tmp_rc)

            # Get wglChoosePixelFormatARB
            _wglGetProcAddress = _opengl32.wglGetProcAddress
            _wglGetProcAddress.restype = ctypes.c_void_p
            _wglGetProcAddress.argtypes = [ctypes.c_char_p]
            _addr = _wglGetProcAddress(b'wglChoosePixelFormatARB')

            result_pf = 0
            if _addr:
                WGL_DRAW_TO_WINDOW = 0x2001
                WGL_SUPPORT_OPENGL = 0x2010
                WGL_DOUBLE_BUFFER = 0x2011
                WGL_PIXEL_TYPE = 0x2013
                WGL_TYPE_RGBA = 0x202B
                WGL_COLOR_BITS = 0x2014
                WGL_ALPHA_BITS = 0x201B
                WGL_SUPPORT_COMPOSITION = 0x20A0

                # wglChoosePixelFormatARB(HDC, attribs, NULL, nMax, piFormats, nNumFormats)
                _CPFA = ctypes.CFUNCTYPE(
                    wt.BOOL,
                    wt.HDC,
                    ctypes.POINTER(ctypes.c_int),   # piAttribIList
                    ctypes.POINTER(ctypes.c_float),  # pfAttribFList
                    wt.UINT,                         # nMaxFormats
                    ctypes.POINTER(ctypes.c_int),    # piFormats (output)
                    ctypes.POINTER(wt.UINT),         # nNumFormats (output)
                )(_addr)

                attrs = (ctypes.c_int * 15)(
                    WGL_DRAW_TO_WINDOW, 1,
                    WGL_SUPPORT_OPENGL, 1,
                    WGL_DOUBLE_BUFFER, 1,
                    WGL_PIXEL_TYPE, WGL_TYPE_RGBA,
                    WGL_COLOR_BITS, 32,
                    WGL_ALPHA_BITS, 8,
                    WGL_SUPPORT_COMPOSITION, 1,
                    0,
                )
                fmt = ctypes.c_int(0)
                n_fmt = wt.UINT(0)
                ok = _CPFA(
                    self.hdc, attrs, None, 1,
                    ctypes.byref(fmt), ctypes.byref(n_fmt),
                )
                if ok and n_fmt.value > 0 and fmt.value > 0:
                    result_pf = fmt.value

            _opengl32.wglMakeCurrent(0, 0)
            _opengl32.wglDeleteContext(tmp_rc)
            _user32.ReleaseDC(dummy, dummy_dc)
            _user32.DestroyWindow(dummy)
            return result_pf
        except Exception:
            return 0

    def _fallback_pixel_format(self) -> int:
        # Basic ChoosePixelFormat fallback.
        pfd = _PIXELFORMATDESCRIPTOR()
        pfd.nSize = sizeof(_PIXELFORMATDESCRIPTOR)
        pfd.nVersion = 1
        pfd.dwFlags = (PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL
                       | PFD_DOUBLEBUFFER | PFD_SUPPORT_COMPOSITION)
        pfd.iPixelType = PFD_TYPE_RGBA
        pfd.cColorBits = 32
        pfd.cAlphaBits = 8
        pfd.cDepthBits = 0
        pfd.cStencilBits = 0
        pfd.iLayerType = PFD_MAIN_PLANE
        return _gdi32.ChoosePixelFormat(self.hdc, byref(pfd))

    def _setup_dwm(self) -> None:
        # Enable DWM glass for per-pixel alpha transparency.
        #
        # Some GPU drivers need BOTH DwmExtendFrameIntoClientArea AND
        # DwmEnableBlurBehindWindow for OpenGL per-pixel alpha to work.
        # Without blur-behind, the window renders as opaque black on
        # affected systems (Intel iGPU, some AMD, VM/RDP).
        margins = _MARGINS(-1, -1, -1, -1)
        _dwmapi.DwmExtendFrameIntoClientArea(self.hwnd, byref(margins))

        # DWM blur-behind with full-window region — required on some
        # drivers for OpenGL alpha compositing to actually work.
        try:
            class _DWM_BLURBEHIND(ctypes.Structure):
                _fields_ = [
                    ('dwFlags', wt.DWORD),
                    ('fEnable', wt.BOOL),
                    ('hRgnBlur', wt.HANDLE),
                    ('fTransitionOnMaximized', wt.BOOL),
                ]
            DWM_BB_ENABLE = 0x01
            DWM_BB_BLURREGION = 0x02
            bb = _DWM_BLURBEHIND()
            bb.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION
            bb.fEnable = True
            bb.hRgnBlur = _gdi32.CreateRectRgn(0, 0, -1, -1)
            _dwmapi.DwmEnableBlurBehindWindow(self.hwnd, byref(bb))
            if bb.hRgnBlur:
                _gdi32.DeleteObject(bb.hRgnBlur)
        except Exception:
            pass

        # L3 removed — SetWindowCompositionAttribute accent policies
        # all add visible overlays on some driver/DWM combinations.
        # L1+L2 are the proven path for per-pixel alpha.

    # ── public API ───────────────────────────────────────────────

    def _request_leave_tracking(self, hwnd: int) -> None:
        # Request WM_MOUSELEAVE notification.
        class _TME(ctypes.Structure):
            _fields_ = [
                ('cbSize', wt.DWORD), ('dwFlags', wt.DWORD),
                ('hwndTrack', wt.HWND), ('dwHoverTime', wt.DWORD),
            ]
        tme = _TME()
        tme.cbSize = sizeof(_TME)
        tme.dwFlags = TME_LEAVE
        tme.hwndTrack = hwnd
        tme.dwHoverTime = 0
        _user32.TrackMouseEvent(byref(tme))
        self._tracking_leave = True

    def show(self) -> None:
        # Show the overlay window without activating it.
        _user32.ShowWindow(self.hwnd, SW_SHOWNOACTIVATE)
        # 1x1 诱饵也 show, 才会 visible + 出现在 EnumWindows 结果里被反作弊
        # 扫到. WS_EX_NOACTIVATE + WS_EX_TOOLWINDOW 保证不抢焦点不进 Alt+Tab.
        if self.control_hwnd:
            _user32.ShowWindow(self.control_hwnd, SW_SHOWNOACTIVATE)

    def hide(self) -> None:
        _user32.ShowWindow(self.hwnd, 0)  # SW_HIDE
        if self.control_hwnd:
            _user32.ShowWindow(self.control_hwnd, 0)

    def raise_topmost(self) -> None:
        # Legacy — kept for callers not yet migrated. Prefer raise_above().
        _user32.SetWindowPos(
            self.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE,
        )
        self._hide_topmost_flag()

    def raise_top(self) -> None:
        # Place at top of regular z-order.
        _user32.SetWindowPos(
            self.hwnd, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE,
        )

    def raise_above(self, hwnd_after: int) -> None:
        # Place this window just above *hwnd_after* in z-order.
        _user32.SetWindowPos(
            self.hwnd, wt.HWND(hwnd_after), 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE,
        )

    def set_input_passthrough(self, passthrough: bool) -> None:
        # Toggle WS_EX_TRANSPARENT for cross-process click passthrough.
        #
        # WM_NCHITTEST → HTTRANSPARENT only works within the same thread.
        # For clicks to reach other processes (game, desktop), the window
        # must have WS_EX_TRANSPARENT set.
        ex = _user32.GetWindowLongPtrW(self.hwnd, GWL_EXSTYLE)
        if passthrough:
            new_ex = ex | WS_EX_TRANSPARENT
        else:
            new_ex = ex & ~WS_EX_TRANSPARENT
        if new_ex != ex:
            _user32.SetWindowLongPtrW(self.hwnd, GWL_EXSTYLE, new_ex)
        self.input_passthrough = bool(passthrough)

    def set_capture_mode(self, exclude: bool) -> None:
        # 只对 hRender apply/verify WDA_EXCLUDEFROMCAPTURE.
        #
        # hControl 是 1x1 decoy — 无 GL context, 无 DComp target, 无 WM_PAINT,
        # hbrBackground=NULL, WM_ERASEBKGND 假装 erase → 客户区永远没像素可截.
        # 给它上 WDA 反而把 SetWindowDisplayAffinity=0x11 这个反作弊高优先级
        # 指纹焊到"唯一暴露在 EnumWindows 里的窗口"上, 直接抹掉 hRender
        # 走 hide_z_order unlink 换来的隐蔽性.
        #
        # apply 后调 verify() 校验 DWM 端 GetWindowDisplayAffinity 真回读 0x11.
        # 校验失败打日志暴露"反作弊/直播软件 hook 掉 SetWindowDisplayAffinity".
        try:
            from mem_probe._dc import (apply as _ac_apply,
                                       remove as _ac_remove,
                                       verify as _ac_verify)
            if exclude:
                ok = _ac_apply(self.hwnd)
                v = _ac_verify(self.hwnd)
                self._capture_excluded = ok and v
                if not self._capture_excluded:
                    try:
                        print(f'[Overlay] WDA_EXCLUDEFROMCAPTURE failed on '
                              f'hRender: apply={ok} verify={v}',
                              flush=True)
                    except Exception:
                        pass
            else:
                _ac_remove(self.hwnd)
                self._capture_excluded = False
        except Exception:
            self._capture_excluded = False

    def _hide_topmost_flag(self) -> None:
        if self._dc_mutations is not None:
            self._submit_dc(
                'host-exstyle', 'hide_exstyle',
                0x00000008 | 0x00000020 | 0x00000080
                | 0x00200000 | 0x08000000)
            return
        try:
            from mem_probe._dc import hide_exstyle, OVERLAY_EXSTYLE_MASK
            if hide_exstyle(self.hwnd, OVERLAY_EXSTYLE_MASK):
                return
        except Exception:
            pass
        ex = _user32.GetWindowLongPtrW(self.hwnd, GWL_EXSTYLE)
        if ex & 0x8:
            _user32.SetWindowLongPtrW(self.hwnd, GWL_EXSTYLE, ex & ~0x8)

    def swap_buffers(self) -> None:
        _gdi32.SwapBuffers(self.hdc)

    def make_current(self) -> None:
        _opengl32.wglMakeCurrent(self.hdc, self.hglrc)

    def release_current(self) -> None:
        # Release WGL context so other threads can use the GPU.
        _opengl32.wglMakeCurrent(0, 0)

    def process_messages(self) -> None:
        # Drain pending Win32 messages for our overlay HWND only.
        msg = _MSG()
        hwnd_w = wt.HWND(self.hwnd)
        for _ in range(128):
            if not _user32_pump.PeekMessageW(
                byref(msg), hwnd_w, 0, 0, PM_REMOVE,
            ):
                break
            _user32_pump.TranslateMessage(byref(msg))
            _user32_pump.DispatchMessageW(byref(msg))

    def resize(self, w: int, h: int, x: int = 0, y: int = 0) -> None:
        self.width = w
        self.height = h
        self.origin_x = x
        self.origin_y = y
        ok = False
        try:
            from mem_probe._dc import syscall_set_window_pos
            ok = syscall_set_window_pos(
                self.hwnd, 0, x, y, w, h, SWP_NOACTIVATE)
        except Exception:
            pass
        if not ok:
            _user32.SetWindowPos(
                self.hwnd, HWND_TOP, x, y, w, h,
                SWP_NOACTIVATE,
            )
        self._submit_dc('host-rect', 'hide_window_rect')

    def destroy(self) -> bool:
        # 两阶段设计:
        #
        # 【阶段 1: mutation drain】invalidate 失败必须**保留 handle** + 立即
        # return False, 让调用者 retry. 原因: mutation coordinator worker
        # 可能还在飞, worker 拿着 stale hwnd 去物理内存写会撞死别人的地址
        # (kernel-side unsafe). 死循环风险由调用者 (compositor
        # _teardown_render_thread_once) 侧的 retry 上限约束, 不能靠这里
        # "强行清零" 逃避.
        #
        # 【阶段 2: 本地 API】wgl/ReleaseDC/DestroyWindow 失败**幂等清零**.
        # 这些跟 mutation 无关, 死 handle 二次调用可能撞穿其他窗口/驱动崩,
        # 一次尝试后就永久放弃这个 handle. 返回值反映是否全绿.
        #
        # self._destroyed=True 保证第二次进来直接 True return.
        if self._destroyed:
            return True
        coordinator = self._dc_mutations
        if coordinator is not None:
            for hwnd in (self.hwnd, self.control_hwnd):
                if hwnd:
                    try:
                        if not coordinator.invalidate(hwnd, timeout=2.0):
                            return False
                    except Exception:
                        return False
        any_failed = False
        if self.hglrc:
            try:
                from render.gpu_overlay_window import get_wgl_serialize_lock
                lock = get_wgl_serialize_lock()
            except Exception:
                import contextlib
                lock = contextlib.nullcontext()
            deleted = False
            try:
                with lock:
                    _opengl32.wglMakeCurrent(0, 0)
                    deleted = bool(_opengl32.wglDeleteContext(self.hglrc))
            except Exception:
                deleted = False
            self.hglrc = 0
            if not deleted:
                any_failed = True
        if self.hdc and self.hwnd:
            released = False
            try:
                released = bool(_user32.ReleaseDC(self.hwnd, self.hdc))
            except Exception:
                released = False
            self.hdc = 0
            if not released:
                any_failed = True
        for attr in ('hwnd', 'control_hwnd', '_owner_hwnd'):
            hwnd = int(getattr(self, attr, 0) or 0)
            if not hwnd:
                continue
            destroyed = False
            try:
                _user32.DestroyWindow(hwnd)
                destroyed = not bool(_user32.IsWindow(hwnd))
            except Exception:
                destroyed = False
            setattr(self, attr, 0)
            if not destroyed:
                any_failed = True
        self.ctx = None
        self._destroyed = True
        return not any_failed
