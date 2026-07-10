# -*- coding: utf-8 -*-
from __future__ import annotations

import os
import time
from typing import Dict, Optional


class CpuTracker:
    # Track per-process CPU% via NtQuerySystemInformation time deltas.
    #
    #     Call ``update(entries)`` with a list of ProcessEntry objects from
    #     ``_iter_process_entries_ext()``.  Returns a dict mapping pid → cpu%.
    #

    def __init__(self) -> None:
        self._prev: Dict[int, int] = {}
        self._prev_wall: float = 0.0
        self._cpu_count: int = os.cpu_count() or 1

    def update(self, entries) -> Dict[int, float]:
        now = time.perf_counter()
        cur: Dict[int, int] = {}
        for e in entries:
            cur[e.pid] = e.kernel_time + e.user_time

        result: Dict[int, float] = {}

        wall_delta = now - self._prev_wall if self._prev_wall > 0 else 0.0
        if wall_delta > 0.05 and self._prev:
            wall_ticks = wall_delta * 10_000_000
            total_ticks = wall_ticks * self._cpu_count
            for pid, ticks in cur.items():
                prev_ticks = self._prev.get(pid, 0)
                if prev_ticks > 0:
                    delta = ticks - prev_ticks
                    if delta >= 0:
                        result[pid] = (delta / total_ticks) * 100.0

        self._prev = cur
        self._prev_wall = now
        return result
