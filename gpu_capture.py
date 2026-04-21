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
from typing import Optional, Tuple

import numpy as np


_user32 = ctypes.windll.user32


class _RECT(ctypes.Structure):
    _fields_ = [('left', ctypes.c_long), ('top', ctypes.c_long),
                ('right', ctypes.c_long), ('bottom', ctypes.c_long)]


class _POINT(ctypes.Structure):
    _fields_ = [('x', ctypes.c_long), ('y', ctypes.c_long)]


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

# Latest frame ring (single slot, latest wins). The capture callback writes
# the BGR buffer + dimensions; readers grab a snapshot under the frame lock.
_frame_lock = threading.Lock()
_frame_bgr: Optional[np.ndarray] = None
_frame_t: float = 0.0
_frame_w: int = 0
_frame_h: int = 0
_frame_count: int = 0


def _import_wgc():
    if _DISABLED:
        return None
    try:
        import windows_capture  # type: ignore
        return windows_capture
    except Exception:
        return None


def _on_frame(frame, control) -> None:
    """WGC callback — runs on the dedicated capture thread.

    ``frame.frame_buffer`` is a numpy view into a Rust-owned native buffer
    that becomes invalid the instant we return.  We therefore copy the BGR
    bytes out into our own array before storing it.
    """
    global _frame_bgr, _frame_t, _frame_w, _frame_h, _frame_count
    try:
        # Convert in-place from BGRA → BGR (drop alpha). Doing it via numpy
        # slicing then .copy() keeps the data alive past the callback return
        # without an extra cv2 dependency hop.
        buf = frame.frame_buffer
        if buf is None or buf.size == 0:
            return
        if buf.ndim != 3 or buf.shape[2] != 4:
            return
        h, w = int(buf.shape[0]), int(buf.shape[1])
        bgr = np.ascontiguousarray(buf[:, :, :3])  # forces a fresh copy
        with _frame_lock:
            _frame_bgr = bgr
            _frame_t = time.time()
            _frame_w = w
            _frame_h = h
            _frame_count += 1
    except Exception:
        # Never let a Python exception propagate back into the Rust callback;
        # the next frame arrival will retry naturally.
        pass


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
    try:
        cap = wgc.WindowsCapture(
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

    # Bind callbacks (the library inspects __name__).
    def on_frame_arrived(frame, control):  # noqa: ANN001
        _on_frame(frame, control)
    on_frame_arrived.__name__ = 'on_frame_arrived'

    def on_closed():
        _on_closed()
    on_closed.__name__ = 'on_closed'

    cap.event(on_frame_arrived)
    cap.event(on_closed)

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
    if _DISABLED or _session_failed or hwnd <= 0:
        return False
    with _session_lock:
        if _session is not None and _session_hwnd == hwnd:
            return True
        # HWND changed (window relaunched, or first ever call) — restart.
        if _session is not None and _session_hwnd != hwnd:
            _stop_locked()
            # Reset frame state so we don't return a stale frame from the
            # previous window.
            with _frame_lock:
                global _frame_bgr, _frame_t
                _frame_bgr = None
                _frame_t = 0.0
        return _start_locked(hwnd)


def get_latest_bgr(hwnd: int,
                   max_age_s: float = 0.25) -> Optional[np.ndarray]:
    """Return the most recent BGR frame for ``hwnd`` cropped to its client
    area, if it's fresh enough.

    The returned array is owned by us (capture thread already deep-copied
    out of the Rust buffer); callers may slice / .copy() it freely.
    """
    if _DISABLED or _session_failed:
        return None
    if hwnd <= 0:
        return None
    if _session is None or _session_hwnd != hwnd:
        return None
    with _frame_lock:
        bgr = _frame_bgr
        ts = _frame_t
    if bgr is None:
        return None
    if max_age_s > 0 and (time.time() - ts) > max_age_s:
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
    with _session_lock:
        _stop_locked()
    with _frame_lock:
        global _frame_bgr, _frame_t
        _frame_bgr = None
        _frame_t = 0.0


def stats() -> dict:
    with _frame_lock:
        return {
            'session': _session is not None,
            'hwnd': _session_hwnd,
            'frame_count': _frame_count,
            'last_frame_age': (time.time() - _frame_t) if _frame_t else None,
            'width': _frame_w,
            'height': _frame_h,
            'failed': _session_failed,
        }
