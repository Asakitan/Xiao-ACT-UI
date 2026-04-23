"""gpu_capture.py — Asynchronous Windows.Graphics.Capture-backed HWND grabber.

The original recognition path uses ``PrintWindow`` which synchronously asks the
target window's WndProc to render to a DC.  For our game window that costs
30–100 ms per call and **blocks the calling thread** the whole time.  Worse,
``recognition.py`` wraps it in ``capture_section()`` so all overlay ULW commits
also have to wait for the capture lock — which is exactly why the menu HUD,
HP / DPS / BOSSHP panels feel "1–2 FPS" during combat even though their tick
loops keep firing at 30 Hz.

Switching to Windows.Graphics.Capture (WGC, available since Windows 10 1903)
fixes both problems at once:

* WGC runs on its own DirectX-backed worker thread.  Frames arrive via a
  callback; the recognition tick simply reads the latest snapshot — never
  blocks for capture.
* It captures the game window's swap-chain directly, so we get a real GPU
  blit, not a synchronous WndProc round-trip.
* The window doesn't need to be in the foreground or fully visible.

If WGC fails to initialise for any reason (Windows < 1903, GPU driver issue,
target HWND not capturable yet, ``SAO_GPU_CAPTURE=0``), this module silently
returns ``None`` and the caller falls back to the legacy PrintWindow path.

Public API
----------
    ensure_session(hwnd)        → bool   (True if GPU capture is providing
                                          frames for this HWND right now)
    get_latest_bgr(hwnd, max_age_s=0.25)
                                → Optional[np.ndarray]   shape (H, W, 3),
                                          BGR uint8, top-down
    stop()                      → tear down current session (e.g. window
                                          changed)
"""

from __future__ import annotations

import ctypes
import os
import threading
import time
from typing import Dict, List, Optional, Tuple

import numpy as np


_user32 = ctypes.windll.user32


class _RECT(ctypes.Structure):
    _fields_ = [('left', ctypes.c_long), ('top', ctypes.c_long),
                ('right', ctypes.c_long), ('bottom', ctypes.c_long)]


class _POINT(ctypes.Structure):
    _fields_ = [('x', ctypes.c_long), ('y', ctypes.c_long)]


_MONITORENUMPROC = ctypes.WINFUNCTYPE(
    ctypes.c_int,
    ctypes.c_ulonglong,
    ctypes.c_ulonglong,
    ctypes.POINTER(_RECT),
    ctypes.c_longlong,
)


def _client_inset(hwnd: int) -> Optional[Tuple[int, int, int, int]]:
    """Return ``(off_x, off_y, client_w, client_h)`` mapping the client area
    onto the full window swap-chain that WGC hands us. ``None`` on failure.
    """
    try:
        win = _RECT()
        cli = _RECT()
        if not _user32.GetWindowRect(hwnd, ctypes.byref(win)):
            return None
        if not _user32.GetClientRect(hwnd, ctypes.byref(cli)):
            return None
        cli_origin = _POINT(0, 0)
        if not _user32.ClientToScreen(hwnd, ctypes.byref(cli_origin)):
            return None
        off_x = cli_origin.x - win.left
        off_y = cli_origin.y - win.top
        return (max(0, off_x), max(0, off_y),
                int(cli.right), int(cli.bottom))
    except Exception:
        return None


_DISABLED = os.environ.get('SAO_GPU_CAPTURE', '') == '0'

_session_lock = threading.Lock()           # protects _session / _hwnd / _last_*
_session = None                            # CaptureControl from start_free_threaded
_session_hwnd = 0
_session_started_t = 0.0
_session_failed = False                    # one-shot kill once we know WGC won't work

# Latest frame slot. We use a one-element list because list[0] = ... is a
# single STORE_SUBSCR / mp_ass_subscript call that does NO memory
# allocation and NO Py_BEGIN_ALLOW_THREADS — critical for the WGC pyo3
# callback thread which intermittently runs with _PyThreadState_Current ==
# NULL (see _SafeWindowsCapture.on_frame_arrived for the full root cause
# analysis).
#
# Tuple shape: (raw_bgra_bytes, timestamp_s, width, height, row_pitch)
_frame_slot: list = [None]
_frame_count: int = 0

# Pause gate: set during glfw.create_window() / moderngl init so the pyo3
# capture thread doesn't try to run Python callbacks while the main thread
# holds the GIL released for a long WGL / D3D ctypes call (the race that
# triggers the "PyEval_RestoreThread: NULL tstate" fatal crash).
_session_paused: bool = False
_resume_hwnd: int = 0
_dxgi_local = threading.local()


def _import_wgc():
    if _DISABLED:
        return None
    try:
        import windows_capture  # type: ignore
        return windows_capture
    except Exception:
        return None


def _get_dxgi_state() -> Tuple[Dict[int, object], Dict[int, Tuple[float, np.ndarray]]]:
    sessions = getattr(_dxgi_local, 'sessions', None)
    if sessions is None:
        sessions = {}
        _dxgi_local.sessions = sessions
    last_frames = getattr(_dxgi_local, 'last_frames', None)
    if last_frames is None:
        last_frames = {}
        _dxgi_local.last_frames = last_frames
    return sessions, last_frames


def list_monitors() -> List[Dict[str, int]]:
    """Return monitors in EnumDisplayMonitors order."""
    monitors: List[Dict[str, int]] = []

    @_MONITORENUMPROC
    def _callback(hmonitor, _hdc, rect_ptr, _lparam):
        rect = rect_ptr.contents
        monitors.append({
            'index': len(monitors),
            'left': int(rect.left),
            'top': int(rect.top),
            'right': int(rect.right),
            'bottom': int(rect.bottom),
            'width': int(rect.right - rect.left),
            'height': int(rect.bottom - rect.top),
            'hmonitor': int(hmonitor),
        })
        return 1

    try:
        _user32.EnumDisplayMonitors(0, 0, _callback, 0)
    except Exception:
        return []
    return monitors


def _monitor_from_point(x: int, y: int) -> Optional[Dict[str, int]]:
    try:
        pt = _POINT(int(x), int(y))
        hmonitor = int(_user32.MonitorFromPoint(pt, 2) or 0)  # MONITOR_DEFAULTTONEAREST
    except Exception:
        hmonitor = 0
    monitors = list_monitors()
    if hmonitor:
        for mon in monitors:
            if int(mon.get('hmonitor', 0)) == hmonitor:
                return mon
    if not monitors:
        return None
    for mon in monitors:
        if mon['left'] <= x < mon['right'] and mon['top'] <= y < mon['bottom']:
            return mon
    return monitors[0]


def _pick_monitor_index_for_point(x: int, y: int) -> Optional[int]:
    mon = _monitor_from_point(x, y)
    if mon is None:
        return None
    return int(mon['index'])


def _get_dxgi_session(monitor_index: int):
    wgc = _import_wgc()
    if wgc is None:
        return None
    sessions, _last_frames = _get_dxgi_state()
    sess = sessions.get(monitor_index)
    if sess is not None:
        return sess
    try:
        sess = wgc.DxgiDuplicationSession(monitor_index=monitor_index)
    except Exception:
        return None
    sessions[monitor_index] = sess
    return sess


def capture_monitor_bgr(
    monitor_index: Optional[int],
    timeout_ms: int = 16,
    max_age_s: float = 0.25,
) -> Optional[np.ndarray]:
    """Return the latest DXGI BGR frame for a monitor, if available."""
    if _DISABLED:
        return None
    idx = int(monitor_index or 0)
    _sessions, last_frames = _get_dxgi_state()
    sess = _get_dxgi_session(idx)
    if sess is None:
        return None
    try:
        try:
            frame = sess.acquire_frame(timeout_ms=timeout_ms)
        except BaseException:
            try:
                sess.recreate()
                frame = sess.acquire_frame(timeout_ms=timeout_ms)
            except BaseException:
                frame = None
        if frame is not None:
            try:
                bgr = frame.to_bgr(copy=True)
            except BaseException:
                bgr = None
            if bgr is not None and getattr(bgr, 'size', 0) > 0:
                last_frames[idx] = (time.time(), bgr)
                return bgr
    except BaseException:
        return None
    cached = last_frames.get(idx)
    if cached is None:
        return None
    ts, img = cached
    if max_age_s > 0 and (time.time() - ts) > max_age_s:
        return None
    return img.copy()


def capture_monitor_bgr_for_point(
    x: int,
    y: int,
    timeout_ms: int = 16,
    max_age_s: float = 0.25,
) -> Optional[np.ndarray]:
    idx = _pick_monitor_index_for_point(int(x), int(y))
    if idx is None:
        return None
    return capture_monitor_bgr(idx, timeout_ms=timeout_ms, max_age_s=max_age_s)


# Frame processing lives inside _SafeWindowsCapture.on_frame_arrived
# (defined below in _start_locked) which OVERRIDES the library wrapper
# and bypasses numpy entirely on the pyo3 callback thread.


def _on_closed() -> None:
    global _session, _session_hwnd
    with _session_lock:
        _session = None
        _session_hwnd = 0


def _start_locked(hwnd: int) -> bool:
    global _session, _session_hwnd, _session_started_t, _session_failed
    wgc = _import_wgc()
    if wgc is None:
        _session_failed = True
        return False

    # ===================================================================
    # ROOT CAUSE & FIX
    # ===================================================================
    # Crash signature:
    #   Fatal Python error: PyEval_RestoreThread: ... GIL is released
    #   (the current Python thread state is NULL)
    #   Thread N (most recent call first):
    #     File "gpu_capture.py", line 127 in _on_frame      [old code]
    #     File "gpu_capture.py", line 172 in on_frame_arrived
    #
    # Why: ``windows_capture.WindowsCapture.on_frame_arrived`` (the
    # library's wrapper) is invoked by the pyo3 capture thread.  BEFORE
    # calling our Python handler it executes:
    #     ndarray = numpy.ctypeslib.as_array(
    #         ctypes.cast(buf, ctypes.POINTER(ctypes.c_uint8)),
    #         shape=(height, width, 4))
    # ``numpy.ctypeslib.as_array`` walks the buffer protocol and during
    # buffer setup CPython hits ``Py_BEGIN_ALLOW_THREADS`` checkpoints.
    # On some pyo3 builds the capture thread holds the GIL via
    # ``Python::assume_gil_acquired()`` which leaves
    # ``_PyThreadState_Current`` NULL.  ``Py_BEGIN_ALLOW_THREADS`` then
    # saves NULL and ``Py_END_ALLOW_THREADS`` calls
    # ``PyEval_RestoreThread(NULL)`` → fatal abort.  The race window is
    # widest while the Tk main thread is in a long GIL-released ctypes
    # call (``glfw.create_window``, ``glfw.swap_buffers`` with vsync,
    # ``moderngl.create_context``).  Hence the user-visible pattern:
    # "crashes immediately after startup or right after opening a menu;
    # waiting 1–2 s for things to settle avoids the crash".
    #
    # Fix: subclass ``WindowsCapture`` and override ``on_frame_arrived``.
    # Python MRO ensures pyo3's stored callable resolves to OUR override,
    # so the library's numpy code path is bypassed entirely.  Our
    # override does only:
    #   - ``ctypes.string_at(addr, n)``  (PYFUNCTYPE →
    #     PyBytes_FromStringAndSize, a single C memcpy with NO
    #     Py_BEGIN/END_ALLOW_THREADS)
    #   - module-global list slot assignment (STORE_SUBSCR → list
    #     mp_ass_subscript, no allocation, no GIL release)
    # Nothing else.  No numpy, no Lock, no time.time() (uses
    # time.monotonic via a cached function reference — a single C call
    # returning a Python int).
    # ===================================================================
    _slot = _frame_slot      # close over module-level list
    _string_at = ctypes.string_at
    _cast = ctypes.cast
    _c_void_p = ctypes.c_void_p
    _now = time.time

    class _SafeWindowsCapture(wgc.WindowsCapture):  # type: ignore[misc]
        """Override the library wrapper to bypass numpy on the pyo3 thread."""

        def __init__(self, **kwargs):  # type: ignore[override]
            super().__init__(**kwargs)
            # ``start_free_threaded()`` validates that frame_handler /
            # closed_handler are non-None.  We override the methods that
            # actually use those handlers (on_frame_arrived / on_closed),
            # but the validation still runs, so install no-op stubs.
            def _noop_frame(_frame, _ctrl) -> None:
                pass
            _noop_frame.__name__ = 'on_frame_arrived'

            def _noop_closed() -> None:
                pass
            _noop_closed.__name__ = 'on_closed'

            self.frame_handler = _noop_frame
            self.closed_handler = _noop_closed

        def on_frame_arrived(  # type: ignore[override]
                self, buf, buf_len, width, height, stop_list, timespan):
            # CRITICAL: this method runs on the pyo3 capture thread which
            # may have a NULL _PyThreadState_Current.  Do NOT use any
            # operation that can trigger Py_BEGIN_ALLOW_THREADS:
            #   - no numpy / PIL / cv2
            #   - no blocking Lock.acquire (waiting releases GIL)
            #   - no print() (file I/O releases GIL)
            #   - no large object creation (allocator may release GIL)
            global _frame_count
            try:
                n = buf_len
                h = height
                w = width
                if n <= 0 or h <= 0 or w <= 0:
                    return
                addr = _cast(buf, _c_void_p).value
                if not addr:
                    return
                # PYFUNCTYPE call → PyBytes_FromStringAndSize. Pure C
                # memcpy. NO Py_BEGIN_ALLOW_THREADS.
                raw = _string_at(addr, n)
                # list[0] = ... → STORE_SUBSCR → PyList_SetItem-equivalent.
                # Atomic, no allocation, no GIL release.
                _slot[0] = (raw, _now(), w, h, n // h if h else n)
                _frame_count += 1
            except BaseException:
                # Never let an exception propagate back to Rust.
                pass

        def on_closed(self) -> None:  # type: ignore[override]
            _on_closed()
    # ===================================================================

    try:
        cap = _SafeWindowsCapture(
            cursor_capture=False,
            draw_border=False,
            window_hwnd=int(hwnd),
            # Game runs at ~60 fps; we only sample at 10 Hz from recognition,
            # but a 16 ms minimum interval keeps the GPU worker quiet without
            # missing rapid HUD changes.
            minimum_update_interval=16,
        )
    except Exception as exc:
        try:
            print(f'[GPU-CAP] WindowsCapture init failed: {exc}')
        except Exception:
            pass
        return False

    try:
        ctrl = cap.start_free_threaded()
    except Exception as exc:
        try:
            print(f'[GPU-CAP] start_free_threaded failed: {exc}')
        except Exception:
            pass
        # Some failure modes (target HWND not yet eligible) are transient
        # — don't latch _session_failed here, let the caller try again.
        return False

    _session = ctrl
    _session_hwnd = int(hwnd)
    _session_started_t = time.time()
    try:
        print(f'[GPU-CAP] session started for hwnd={hwnd}')
    except Exception:
        pass
    return True


def _stop_locked() -> None:
    global _session, _session_hwnd
    sess = _session
    _session = None
    _session_hwnd = 0
    if sess is not None:
        try:
            sess.stop()
        except Exception:
            pass


def ensure_session(hwnd: int) -> bool:
    """Make sure a WGC session is running for ``hwnd``.

    Returns True if frames are (or are about to be) flowing.  Returns False
    when WGC is unavailable on this machine — the caller should fall back to
    PrintWindow.  Cheap to call every recognition tick.
    """
    global _resume_hwnd
    if _DISABLED or _session_failed or hwnd <= 0:
        return False
    with _session_lock:
        if _session_paused:
            # Capture is suspended (a long GIL-released ctypes call is in
            # progress on the main thread).  Remember the desired hwnd so
            # resume_capture() can restart the session for it.
            _resume_hwnd = hwnd
            return False
        if _session is not None and _session_hwnd == hwnd:
            return True
        # HWND changed (window relaunched, or first ever call) — restart.
        if _session is not None and _session_hwnd != hwnd:
            _stop_locked()
            # Reset the slot so we don't return a stale frame from the
            # previous window.
            _frame_slot[0] = None
        return _start_locked(hwnd)


def get_latest_bgr(hwnd: int,
                   max_age_s: float = 0.25) -> Optional[np.ndarray]:
    """Return the most recent BGR frame for ``hwnd`` cropped to its client
    area, if it's fresh enough.

    The returned array is owned by us (we deep-copy out of the slot so the
    bytes object can be replaced by the next callback safely); callers may
    slice / .copy() it freely.
    """
    if _DISABLED or _session_failed:
        return None
    if hwnd <= 0:
        return None
    if _session is None or _session_hwnd != hwnd:
        return None
    # Atomic read of the slot reference (single Python operation, never
    # torn).  The bytes object the slot points to is immutable so the
    # callback can replace the slot at any time without affecting us.
    snap = _frame_slot[0]
    if snap is None:
        return None
    raw, ts, w, h, rp = snap
    if max_age_s > 0 and (time.time() - ts) > max_age_s:
        return None
    # Convert raw BGRA bytes → BGR numpy on the CALLER's thread (Tk main
    # / recognition lane) where _PyThreadState_Current is always valid.
    try:
        flat = np.frombuffer(raw, dtype=np.uint8)
        if rp == w * 4 or rp == 0:
            bgr = flat.reshape(h, w, 4)[:, :, :3].copy()
        else:
            # GPU-padded rows: each row is rp bytes, only w*4 are valid.
            bgr = flat.reshape(h, rp)[:, :w * 4].reshape(
                h, w, 4)[:, :, :3].copy()
    except Exception:
        return None
    # WGC frames cover the actual window swap-chain content. On modern
    # DWM-composed windows this is *typically* the client area only — the
    # 8 px invisible drag-shadow borders that GetWindowRect reports are
    # not part of the swap-chain. Detect that case and skip the crop. For
    # bordered/title-barred windows (chrome inside the swap-chain) we do
    # need to subtract the inset.
    fh, fw = int(bgr.shape[0]), int(bgr.shape[1])
    inset = _client_inset(hwnd)
    if inset is None:
        return bgr  # best effort — caller checks dimensions
    off_x, off_y, cw, ch = inset
    if cw <= 0 or ch <= 0:
        return None
    # Fast path: WGC already gave us client-sized swap-chain pixels.
    if fw == cw and fh == ch:
        return bgr
    # Slow path: WGC frame is larger (real chrome present). Crop with
    # the computed inset, but only if it fits.
    if off_x + cw > fw or off_y + ch > fh:
        return None
    if off_x == 0 and off_y == 0:
        return bgr[:ch, :cw]
    return np.ascontiguousarray(bgr[off_y:off_y + ch, off_x:off_x + cw])


def stop() -> None:
    global _session_paused, _resume_hwnd
    with _session_lock:
        _session_paused = False
        _resume_hwnd = 0
        _stop_locked()
    _frame_slot[0] = None


def pause_capture() -> None:
    """Suspend the WGC session before a long GIL-releasing ctypes call
    (``glfw.create_window``, ``moderngl.create_context``) on the main
    thread.  This eliminates the race window for the
    "PyEval_RestoreThread: NULL tstate" pyo3 crash.  Pair with
    :func:`resume_capture`.  Idempotent and fast when no session is
    running.
    """
    global _session_paused, _resume_hwnd
    with _session_lock:
        if _session_paused:
            return
        _session_paused = True
        # Save the active hwnd BEFORE _stop_locked clears it.
        _resume_hwnd = _session_hwnd
        if _session is not None:
            _stop_locked()


def resume_capture() -> None:
    """Restart the WGC session after the long ctypes call completes.
    Pairs with :func:`pause_capture`.
    """
    global _session_paused, _resume_hwnd
    with _session_lock:
        if not _session_paused:
            return
        _session_paused = False
        hwnd = _resume_hwnd
        _resume_hwnd = 0
        if hwnd > 0 and _session is None and not _session_failed:
            _start_locked(hwnd)


def stats() -> dict:
    snap = _frame_slot[0]
    if snap is None:
        return {
            'session': _session is not None,
            'hwnd': _session_hwnd,
            'frame_count': _frame_count,
            'last_frame_age': None,
            'width': 0,
            'height': 0,
            'failed': _session_failed,
        }
    _, ts, w, h, _rp = snap
    return {
        'session': _session is not None,
        'hwnd': _session_hwnd,
        'frame_count': _frame_count,
        'last_frame_age': (time.time() - ts) if ts else None,
        'width': w,
        'height': h,
        'failed': _session_failed,
    }
