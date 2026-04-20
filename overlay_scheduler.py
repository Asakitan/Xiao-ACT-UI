"""overlay_scheduler.py - Shared display-synced frame pacer for Tk ULW overlays.

Design:
- One ``root.after`` loop ticks at the monitor's refresh rate (auto-detected
  on Windows; clamps to 60-240 Hz). Uses a high-resolution perf-counter
  deadline so the cadence does not drift.
- On Windows, ``winmm.timeBeginPeriod(1)`` is engaged while the scheduler is
  running. Without it, Tk's ``after`` rounds up to the default ~15.6 ms
  scheduler quantum and overlays degrade to ~10 Hz under load. With it,
  sleep/after resolution is ~1 ms, which is what a 60/120/144 Hz cadence
  actually needs.
- Each overlay registers ``tick_fn(now)``; scheduler calls it on the Tk main
  thread every frame. Heavy compose + premultiply work is already offloaded
  to worker threads (see overlay_render_worker.py), so all panels — idle or
  animating — now tick every frame. GPU composition has removed the CPU
  bottleneck that originally motivated per-overlay idle downsampling.
"""
from __future__ import annotations

import ctypes
import os
import threading
import time
from typing import Callable, Dict, Optional


_DEFAULT_HZ = 60
_MIN_HZ = 60
_MAX_HZ = 240
_SLACK_SEC = 0.0010


def _detect_refresh_hz() -> int:
    """Best-effort refresh-rate detection. Falls back to 60 Hz."""
    if os.name != 'nt':
        return _DEFAULT_HZ
    try:
        user32 = ctypes.windll.user32
        gdi32 = ctypes.windll.gdi32
        VREFRESH = 116
        hdc = user32.GetDC(0)
        if not hdc:
            return _DEFAULT_HZ
        try:
            rate = int(gdi32.GetDeviceCaps(hdc, VREFRESH))
        finally:
            user32.ReleaseDC(0, hdc)
        if rate <= 1:
            return _DEFAULT_HZ
        return max(_MIN_HZ, min(_MAX_HZ, rate))
    except Exception:
        return _DEFAULT_HZ


class _WinTimerResolution:
    """RAII-ish wrapper around ``timeBeginPeriod(1)`` on Windows.

    The default scheduler tick on Windows is ~15.6 ms, which clamps any
    ``root.after(1, ...)`` to the same 15.6 ms quantum. That bottoms out Tk
    overlays at ~64 Hz at best and ~10-20 Hz under load. Raising the multimedia
    timer resolution to 1 ms lets ``after`` actually pace at the monitor
    refresh rate. We release it on ``stop()`` so the process doesn't leave the
    system-wide timer pinned high.
    """

    def __init__(self) -> None:
        self._engaged = False
        self._winmm = None
        if os.name == 'nt':
            try:
                self._winmm = ctypes.windll.winmm
            except Exception:
                self._winmm = None

    def acquire(self) -> None:
        if self._engaged or self._winmm is None:
            return
        try:
            if self._winmm.timeBeginPeriod(1) == 0:  # TIMERR_NOERROR
                self._engaged = True
        except Exception:
            pass

    def release(self) -> None:
        if not self._engaged or self._winmm is None:
            return
        try:
            self._winmm.timeEndPeriod(1)
        except Exception:
            pass
        self._engaged = False


# Module-level so TARGET_HZ / FRAME_SEC stay importable for legacy callers.
TARGET_HZ = _detect_refresh_hz()
FRAME_SEC = 1.0 / TARGET_HZ


class _Job:
    __slots__ = ('ident', 'tick_fn', 'animating_fn', 'visibility_fn')

    def __init__(self, ident: str,
                 tick_fn: Callable[[float], None],
                 animating_fn: Callable[[], bool],
                 visibility_fn: Optional[Callable[[], bool]] = None):
        self.ident = ident
        self.tick_fn = tick_fn
        self.animating_fn = animating_fn
        self.visibility_fn = visibility_fn


class OverlayScheduler:
    def __init__(self, root):
        self._root = root
        self._jobs: Dict[str, _Job] = {}
        self._jobs_lock = threading.Lock()
        self._running = False
        self._next_deadline = 0.0
        self._tick_after_id: Optional[str] = None
        self._frame_idx = 0
        # Monitor-synced cadence. Re-resolve at start in case the display
        # configuration changed between imports and scheduler start.
        self._target_hz = TARGET_HZ
        self._frame_sec = FRAME_SEC
        self._timer_res = _WinTimerResolution()
        # Perf counters (read-only from outside).
        self.last_frame_ms = 0.0
        self.avg_frame_ms = 0.0
        self.frame_count = 0

    # lifecycle

    def start(self) -> None:
        if self._running:
            return
        self._target_hz = _detect_refresh_hz()
        self._frame_sec = 1.0 / max(1, self._target_hz)
        self._timer_res.acquire()
        self._running = True
        self._next_deadline = time.perf_counter() + self._frame_sec
        self._schedule_next()

    def stop(self) -> None:
        self._running = False
        if self._tick_after_id is not None:
            try:
                self._root.after_cancel(self._tick_after_id)
            except Exception:
                pass
            self._tick_after_id = None
        self._timer_res.release()

    # registration

    def register(self, ident: str,
                 tick_fn: Callable[[float], None],
                 animating_fn: Callable[[], bool],
                 visibility_fn: Optional[Callable[[], bool]] = None) -> None:
        with self._jobs_lock:
            self._jobs[ident] = _Job(ident, tick_fn, animating_fn, visibility_fn)
        if not self._running:
            self.start()

    def unregister(self, ident: str) -> None:
        with self._jobs_lock:
            self._jobs.pop(ident, None)
            should_stop = not self._jobs
        if should_stop:
            self.stop()

    @property
    def target_hz(self) -> int:
        return self._target_hz

    # main loop

    def _schedule_next(self) -> None:
        if not self._running:
            return
        now_pc = time.perf_counter()
        delta = self._next_deadline - now_pc - _SLACK_SEC
        if delta <= 0:
            delay_ms = 0
            # Behind: resync forward rather than "catch up" fast.
            if -delta > 2 * self._frame_sec:
                self._next_deadline = now_pc + self._frame_sec
            else:
                self._next_deadline += self._frame_sec
        else:
            # Round to nearest ms instead of floor, so 6.94 ms (144 Hz) doesn't
            # collapse to 6 ms and run hot.
            delay_ms = max(1, int(round(delta * 1000.0)))
        try:
            self._tick_after_id = self._root.after(delay_ms, self._tick)
        except Exception:
            self._tick_after_id = None

    def _tick(self) -> None:
        self._tick_after_id = None
        if not self._running:
            return
        t_start = time.perf_counter()
        self._frame_idx += 1
        now = time.time()

        with self._jobs_lock:
            jobs = list(self._jobs.values())

        # v2.2.10: respect the render FPS target on slower machines.
        # When the average frame already eats more than ~70% of the frame
        # budget, idle (non-animating) panels get downsampled to roughly
        # 20 Hz so the actively-animating panels (SkillFX burst, BossHP
        # break row, menu fisheye) keep their full per-frame budget.
        # Without this every panel ticks every frame, the main thread
        # falls further behind, and the user sees the panels appear to
        # lock at ~10–15 fps during heavy combat with the menu open.
        budget_sec = self._frame_sec
        overloaded = self.avg_frame_ms > 0.7 * budget_sec * 1000.0
        idle_skip_n = 5 if overloaded else 0
        frame_idx = self._frame_idx

        for job in jobs:
            try:
                if job.visibility_fn is not None and not bool(job.visibility_fn()):
                    continue
            except Exception:
                pass
            if idle_skip_n > 0:
                try:
                    is_animating = bool(job.animating_fn())
                except Exception:
                    is_animating = True
                if not is_animating and (frame_idx % idle_skip_n) != 0:
                    continue
            try:
                job.tick_fn(now)
            except Exception as exc:
                try:
                    print(f'[Sched] tick error ({job.ident}): {exc}')
                except Exception:
                    pass

        self._next_deadline += self._frame_sec
        # If we've fallen more than 2 frames behind (e.g. a recognition
        # hitch), resync rather than spiral.
        if time.perf_counter() - self._next_deadline > 2 * self._frame_sec:
            self._next_deadline = time.perf_counter() + self._frame_sec

        self.last_frame_ms = (time.perf_counter() - t_start) * 1000.0
        self.avg_frame_ms = self.avg_frame_ms * 0.9 + self.last_frame_ms * 0.1
        self.frame_count += 1

        self._schedule_next()


_scheduler_lock = threading.Lock()
_scheduler: Optional[OverlayScheduler] = None


def get_scheduler(root=None) -> OverlayScheduler:
    global _scheduler
    with _scheduler_lock:
        if _scheduler is None:
            if root is None:
                raise RuntimeError(
                    'scheduler not yet initialised; first call must pass root'
                )
            _scheduler = OverlayScheduler(root)
            _scheduler.start()
        return _scheduler


def stop_scheduler() -> None:
    global _scheduler
    with _scheduler_lock:
        if _scheduler is not None:
            _scheduler.stop()
            _scheduler = None
