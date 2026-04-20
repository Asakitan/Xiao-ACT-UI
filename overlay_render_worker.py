# -*- coding: utf-8 -*-
"""Off-thread frame composition for ULW overlays.

The main-thread scheduler calls _tick → _advance (animate) → _render.
Previously, _render did **compose_frame** (PIL/numpy heavy, ~3-8 ms) +
**_ulw_update** (premultiply + Win32 commit, ~1-2 ms) all on the Tk
thread, which blocks the event loop and causes visible tearing/jank.

This module provides ``AsyncFrameWorker`` handles backed by shared fixed
render lanes. Each overlay worker is pinned to one background thread, so
thread-affine resources such as standalone GL contexts remain stable while
multiple overlays can still render in parallel across CPU cores.

Architecture:
    Main thread                 Render lanes / CPU task pool
    ──────────                  ────────────────────────────
    _advance(now)
    submit_compose(fn, now)  →  pinned lane: fn(now) → premultiply → store result
                ⋮ (returns immediately)
    if result ready:
        _ulw_commit(hwnd, result)  ← (GDI only, <0.3 ms)

PIL and NumPy release the GIL in their C routines, so these background
jobs genuinely run on other cores.

"""
from __future__ import annotations

import ctypes
import itertools
import os
import threading
from concurrent.futures import ThreadPoolExecutor
from typing import Callable, Dict, List, Optional, Sequence, Tuple, TypeVar

import numpy as np
from PIL import Image

from render_capture_sync import wait_until_capture_idle

# ── Win32 structures (mirrored from sao_gui_dps for independence) ──
_user32 = ctypes.windll.user32
_gdi32 = ctypes.windll.gdi32
ULW_ALPHA = 2
T = TypeVar('T')


class _POINT(ctypes.Structure):
    _fields_ = [('x', ctypes.c_long), ('y', ctypes.c_long)]


class _SIZE(ctypes.Structure):
    _fields_ = [('cx', ctypes.c_long), ('cy', ctypes.c_long)]


class _BLENDFUNCTION(ctypes.Structure):
    _fields_ = [
        ('BlendOp', ctypes.c_byte),
        ('BlendFlags', ctypes.c_byte),
        ('SourceConstantAlpha', ctypes.c_byte),
        ('AlphaFormat', ctypes.c_byte),
    ]


class _BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ('biSize', ctypes.c_uint32),
        ('biWidth', ctypes.c_int32),
        ('biHeight', ctypes.c_int32),
        ('biPlanes', ctypes.c_uint16),
        ('biBitCount', ctypes.c_uint16),
        ('biCompression', ctypes.c_uint32),
        ('biSizeImage', ctypes.c_uint32),
        ('biXPelsPerMeter', ctypes.c_int32),
        ('biYPelsPerMeter', ctypes.c_int32),
        ('biClrUsed', ctypes.c_uint32),
        ('biClrImportant', ctypes.c_uint32),
    ]


# ── Premultiplied BGRA buffer ready for UpdateLayeredWindow ──

class FrameBuffer:
    """Immutable result of off-thread composition."""
    __slots__ = ('bgra_bytes', 'width', 'height', 'x', 'y')

    def __init__(self, bgra: bytes, w: int, h: int, x: int, y: int):
        self.bgra_bytes = bgra
        self.width = w
        self.height = h
        self.x = x
        self.y = y


def _premultiply_to_bgra(img: Image.Image) -> bytes:
    """RGBA PIL → premultiplied BGRA bytes.  Pure numpy, releases GIL."""
    rgba = np.asarray(img, dtype=np.uint8)
    a = rgba[:, :, 3:4].astype(np.uint16)
    rgb = (rgba[:, :, :3].astype(np.uint16) * a + 127) // 255
    bgra = np.empty_like(rgba)
    bgra[:, :, 0] = rgb[:, :, 2]  # B
    bgra[:, :, 1] = rgb[:, :, 1]  # G
    bgra[:, :, 2] = rgb[:, :, 0]  # R
    bgra[:, :, 3] = rgba[:, :, 3]  # A
    return bgra.tobytes()


def _recommended_lane_count() -> int:
    cpu_total = max(1, os.cpu_count() or 1)
    if cpu_total <= 2:
        return 1
    if cpu_total <= 4:
        return 2
    if cpu_total <= 6:
        return 3
    if cpu_total <= 8:
        return 4
    # v2.1.16: high-core systems (≥12c) get up to 6 lanes so 5–6 panels +
    # menu can compose in parallel instead of serializing on 4 lanes.
    return min(6, cpu_total - 2)


def _recommended_cpu_task_workers() -> int:
    cpu_total = max(1, os.cpu_count() or 1)
    if cpu_total <= 2:
        return 1
    if cpu_total <= 6:
        return max(2, min(4, cpu_total - 1))
    return min(8, cpu_total - 2)


# v2.1.16: pin background render threads to specific CPU cores on Windows.
# Reduces context-switch thrash + improves L1/L2 cache locality when several
# panels compose in parallel during heavy combat (DPS+BossHP+Burst+menu).
_AFFINITY_CURSOR = itertools.count(2)  # leave cores 0/1 for Tk + capture
_AFFINITY_LOCK = threading.Lock()
_AFFINITY_FAILED = False


def _pin_current_thread_to_core(slot: int) -> None:
    """Pin the calling thread to a single CPU core on Windows."""
    global _AFFINITY_FAILED
    if _AFFINITY_FAILED:
        return
    cpu_total = max(1, os.cpu_count() or 1)
    if cpu_total <= 2:
        return
    core_id = slot % cpu_total
    try:
        kernel32 = ctypes.windll.kernel32
        # SetThreadAffinityMask(HANDLE hThread, DWORD_PTR mask) -> DWORD_PTR
        kernel32.SetThreadAffinityMask.restype = ctypes.c_size_t
        kernel32.SetThreadAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        h_thread = kernel32.GetCurrentThread()
        mask = ctypes.c_size_t(1 << core_id)
        if kernel32.SetThreadAffinityMask(h_thread, mask) == 0:
            _AFFINITY_FAILED = True
    except Exception:
        _AFFINITY_FAILED = True


def _next_affinity_slot() -> int:
    with _AFFINITY_LOCK:
        return next(_AFFINITY_CURSOR)


def _cpu_pool_initializer() -> None:
    _pin_current_thread_to_core(_next_affinity_slot())


_cpu_task_workers = _recommended_cpu_task_workers()
_cpu_task_pool = ThreadPoolExecutor(
    max_workers=_cpu_task_workers,
    thread_name_prefix='overlay-layer',
    initializer=_cpu_pool_initializer,
)


def run_cpu_tasks(tasks: Sequence[Callable[[], T]]) -> List[T]:
    """Run CPU-only layer tasks on the shared pool.

    Task bodies must not touch thread-affine GL contexts. This is intended
    for PIL/numpy image composition that benefits from extra CPU cores.
    """
    if not tasks:
        return []
    if len(tasks) == 1 or _cpu_task_workers <= 1:
        return [task() for task in tasks]
    futures = [_cpu_task_pool.submit(task) for task in tasks]
    return [future.result() for future in futures]


def ulw_commit(hwnd: int, fb: FrameBuffer, alpha: int = 255,
               allow_during_capture: bool = False) -> bool:
    """Blit a pre-composed FrameBuffer to a layered window.

    Only does the GDI DIBSection + UpdateLayeredWindow call — no numpy,
    no PIL.  Safe (and required) to call from the Tk main thread.
    """
    if (not allow_during_capture) and (not wait_until_capture_idle(0.010)):
        return False
    w, h = fb.width, fb.height
    hdc_screen = _user32.GetDC(0)
    hdc_mem = _gdi32.CreateCompatibleDC(hdc_screen)

    bmi = _BITMAPINFOHEADER()
    bmi.biSize = ctypes.sizeof(_BITMAPINFOHEADER)
    bmi.biWidth = w
    bmi.biHeight = -h
    bmi.biPlanes = 1
    bmi.biBitCount = 32
    bmi.biCompression = 0

    bits = ctypes.c_void_p()
    hbm = _gdi32.CreateDIBSection(
        hdc_mem, ctypes.byref(bmi), 0, ctypes.byref(bits), None, 0)
    old_bm = _gdi32.SelectObject(hdc_mem, hbm)

    ctypes.memmove(bits, fb.bgra_bytes, len(fb.bgra_bytes))

    pt_dst = _POINT(fb.x, fb.y)
    sz = _SIZE(w, h)
    pt_src = _POINT(0, 0)
    blend = _BLENDFUNCTION(0, 0, max(0, min(255, int(alpha))), 1)

    _user32.UpdateLayeredWindow(
        ctypes.c_void_p(hwnd),
        hdc_screen,
        ctypes.byref(pt_dst),
        ctypes.byref(sz),
        hdc_mem,
        ctypes.byref(pt_src),
        0,
        ctypes.byref(blend),
        ULW_ALPHA,
    )

    _gdi32.SelectObject(hdc_mem, old_bm)
    _gdi32.DeleteObject(hbm)
    _gdi32.DeleteDC(hdc_mem)
    _user32.ReleaseDC(0, hdc_screen)
    return True


# ── Shared async compose backend ──

_worker_id_counter = itertools.count(1)


class _RenderLane:
    def __init__(self, index: int):
        self.index = index
        self._lock = threading.Lock()
        self._cond = threading.Condition(self._lock)
        self._pending: Dict[int, Tuple[Callable, float, int, int, int]] = {}
        self._results: Dict[int, FrameBuffer] = {}
        self._workers: list[int] = []
        self._cursor = 0
        self._affinity_slot = _next_affinity_slot()
        self._thread = threading.Thread(
            target=self._loop,
            daemon=True,
            name=f'overlay-compose-{index + 1}',
        )
        self._thread.start()

    def register(self, worker_id: int) -> None:
        with self._lock:
            if worker_id not in self._workers:
                self._workers.append(worker_id)

    def worker_count(self) -> int:
        with self._lock:
            return len(self._workers)

    def unregister(self, worker_id: int) -> None:
        with self._lock:
            self._pending.pop(worker_id, None)
            self._results.pop(worker_id, None)
            try:
                self._workers.remove(worker_id)
            except ValueError:
                pass
            if self._cursor >= len(self._workers):
                self._cursor = 0

    def submit(self, worker_id: int,
               compose_fn: Callable[[float], Image.Image],
               now: float, hwnd: int, x: int, y: int) -> None:
        with self._lock:
            if worker_id not in self._workers:
                self._workers.append(worker_id)
            self._pending[worker_id] = (compose_fn, now, hwnd, x, y)
            self._cond.notify()

    def take_result(self, worker_id: int) -> Optional[FrameBuffer]:
        with self._lock:
            return self._results.pop(worker_id, None)

    def reset(self, worker_id: int) -> None:
        with self._lock:
            self._pending.pop(worker_id, None)
            self._results.pop(worker_id, None)

    def _next_job_locked(self) -> Optional[Tuple[int, Tuple[Callable, float, int, int, int]]]:
        if not self._pending:
            return None
        total = len(self._workers)
        if total > 0:
            for step in range(total):
                idx = (self._cursor + step) % total
                worker_id = self._workers[idx]
                job = self._pending.pop(worker_id, None)
                if job is not None:
                    self._cursor = (idx + 1) % max(1, len(self._workers))
                    return worker_id, job
        worker_id, job = self._pending.popitem()
        return worker_id, job

    def _loop(self) -> None:
        _pin_current_thread_to_core(self._affinity_slot)
        while True:
            with self._lock:
                while not self._pending:
                    self._cond.wait(timeout=0.1)
                picked = self._next_job_locked()
            if picked is None:
                continue
            worker_id, job = picked
            compose_fn, now, hwnd, x, y = job
            try:
                img = compose_fn(now)
                w, h = img.size
                bgra = _premultiply_to_bgra(img)
                fb = FrameBuffer(bgra, w, h, x, y)
                with self._lock:
                    if worker_id in self._workers:
                        self._results[worker_id] = fb
            except Exception as exc:
                try:
                    print(f'[RenderWorker] compose error: {exc}')
                except Exception:
                    pass


class _SharedRenderBackend:
    def __init__(self):
        lane_count = _recommended_lane_count()
        self._lock = threading.Lock()
        self._lanes = [_RenderLane(index) for index in range(lane_count)]
        self._worker_lanes: Dict[int, int] = {}

    def _pick_lane_locked(self) -> int:
        lane_loads = [(lane.worker_count(), index) for index, lane in enumerate(self._lanes)]
        lane_loads.sort(key=lambda item: (item[0], item[1]))
        return lane_loads[0][1]

    def _ensure_lane(self, worker_id: int) -> _RenderLane:
        with self._lock:
            lane_index = self._worker_lanes.get(worker_id)
            if lane_index is None:
                lane_index = self._pick_lane_locked()
                self._worker_lanes[worker_id] = lane_index
            lane = self._lanes[lane_index]
        lane.register(worker_id)
        return lane

    def register(self, worker_id: int) -> None:
        self._ensure_lane(worker_id)

    def unregister(self, worker_id: int) -> None:
        with self._lock:
            lane_index = self._worker_lanes.pop(worker_id, None)
        if lane_index is None:
            return
        self._lanes[lane_index].unregister(worker_id)

    def submit(self, worker_id: int,
               compose_fn: Callable[[float], Image.Image],
               now: float, hwnd: int, x: int, y: int) -> None:
        lane = self._ensure_lane(worker_id)
        lane.submit(worker_id, compose_fn, now, hwnd, x, y)

    def take_result(self, worker_id: int,
                    allow_during_capture: bool = False) -> Optional[FrameBuffer]:
        if (not allow_during_capture) and (not wait_until_capture_idle(0.0)):
            return None
        lane = self._ensure_lane(worker_id)
        return lane.take_result(worker_id)

    def reset(self, worker_id: int) -> None:
        lane = self._ensure_lane(worker_id)
        lane.reset(worker_id)


_shared_backend: Optional[_SharedRenderBackend] = None
_shared_backend_lock = threading.Lock()


def _get_shared_backend() -> _SharedRenderBackend:
    global _shared_backend
    with _shared_backend_lock:
        if _shared_backend is None:
            _shared_backend = _SharedRenderBackend()
        return _shared_backend


class AsyncFrameWorker:
    """Handle onto the shared overlay render thread.

    Each overlay keeps an isolated latest-frame queue, but all heavy CPU
    rendering is funneled through one shared render thread so recognition
    and rendering stay on separate lanes.
    """

    def __init__(self):
        self._worker_id = next(_worker_id_counter)
        self._backend = _get_shared_backend()
        self._backend.register(self._worker_id)
        self._stopped = False

    def submit(self, compose_fn: Callable[[float], Image.Image],
               now: float, hwnd: int, x: int, y: int) -> None:
        if self._stopped:
            return
        self._backend.submit(self._worker_id, compose_fn, now, hwnd, x, y)

    def take_result(self, allow_during_capture: bool = False) -> Optional[FrameBuffer]:
        if self._stopped:
            return None
        return self._backend.take_result(
            self._worker_id,
            allow_during_capture=allow_during_capture,
        )

    def reset(self) -> None:
        if self._stopped:
            return
        self._backend.reset(self._worker_id)

    def stop(self) -> None:
        if self._stopped:
            return
        self._stopped = True
        self._backend.unregister(self._worker_id)
