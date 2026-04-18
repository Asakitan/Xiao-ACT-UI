"""overlay_scheduler.py - Shared 60 FPS frame pacer for Tk ULW overlays.

Design:
- One ``root.after`` loop ticks at TARGET_HZ using a high-resolution
  perf-counter deadline. No drift, no pileup.
- Each overlay registers ``tick_fn(now)``; scheduler calls it on the Tk main
  thread. Overlay ``_advance`` (animation state mutation) runs on this thread.
- Heavy compose + premultiply work is offloaded to per-overlay worker threads
  via ``AsyncFrameWorker`` (see overlay_render_worker.py). Only the cheap
  ``UpdateLayeredWindow`` GDI commit runs on the main thread.
- Idle overlays (``animating_fn()`` returns False) only tick every Nth frame,
  giving roughly 20 Hz for idle HP/DPS while the rest of the loop stays at 60.
"""
from __future__ import annotations

import threading
import time
from typing import Callable, Dict, Optional


TARGET_HZ = 60
FRAME_SEC = 1.0 / TARGET_HZ
_SLACK_SEC = 0.0015


class _Job:
    __slots__ = ('ident', 'tick_fn', 'animating_fn', 'phase')

    def __init__(self, ident: str,
                 tick_fn: Callable[[float], None],
                 animating_fn: Callable[[], bool]):
        self.ident = ident
        self.tick_fn = tick_fn
        self.animating_fn = animating_fn
        # Stagger idle ticks across overlays so they don't all pile onto
        # the same frame.
        self.phase = 0


class OverlayScheduler:
    IDLE_EVERY_N = 3   # 60 Hz / 3 = 20 Hz when idle

    def __init__(self, root):
        self._root = root
        self._jobs: Dict[str, _Job] = {}
        self._jobs_lock = threading.Lock()
        self._running = False
        self._next_deadline = 0.0
        self._tick_after_id: Optional[str] = None
        self._frame_idx = 0
        # Perf counters (read-only from outside).
        self.last_frame_ms = 0.0
        self.avg_frame_ms = 0.0
        self.frame_count = 0

    # lifecycle

    def start(self) -> None:
        if self._running:
            return
        self._running = True
        self._next_deadline = time.perf_counter() + FRAME_SEC
        self._schedule_next()

    def stop(self) -> None:
        self._running = False
        if self._tick_after_id is not None:
            try:
                self._root.after_cancel(self._tick_after_id)
            except Exception:
                pass
            self._tick_after_id = None

    # registration

    def register(self, ident: str,
                 tick_fn: Callable[[float], None],
                 animating_fn: Callable[[], bool]) -> None:
        with self._jobs_lock:
            job = _Job(ident, tick_fn, animating_fn)
            job.phase = len(self._jobs) % self.IDLE_EVERY_N
            self._jobs[ident] = job
        if not self._running:
            self.start()

    def unregister(self, ident: str) -> None:
        with self._jobs_lock:
            self._jobs.pop(ident, None)
            should_stop = not self._jobs
        if should_stop:
            self.stop()

    # main loop

    def _schedule_next(self) -> None:
        if not self._running:
            return
        now_pc = time.perf_counter()
        delta = self._next_deadline - now_pc - _SLACK_SEC
        if delta < 0:
            delay_ms = 0
            # Behind: resync forward rather than "catch up" fast.
            if -delta > 2 * FRAME_SEC:
                self._next_deadline = now_pc + FRAME_SEC
            else:
                self._next_deadline += FRAME_SEC
        else:
            delay_ms = max(1, int(delta * 1000))
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

        frame = self._frame_idx
        for job in jobs:
            try:
                animating = bool(job.animating_fn())
            except Exception:
                animating = True
            if not animating and ((frame + job.phase) % self.IDLE_EVERY_N) != 0:
                continue
            try:
                job.tick_fn(now)
            except Exception as exc:
                try:
                    print(f'[Sched] tick error ({job.ident}): {exc}')
                except Exception:
                    pass

        self._next_deadline += FRAME_SEC
        # If we've fallen more than 2 frames behind (e.g. a recognition
        # hitch), resync rather than spiral.
        if time.perf_counter() - self._next_deadline > 2 * FRAME_SEC:
            self._next_deadline = time.perf_counter() + FRAME_SEC

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
