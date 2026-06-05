# -*- coding: utf-8 -*-
"""Per-buff uptime accumulator for the ACT buff/debuff coverage panel.

Fed by the periodic ``self_buffs`` snapshots that the packet bridge already
packs into ``GameState.self_buffs`` (each ``{id, uuid, begin_ms, duration_ms,
layer, count, name}``). This runs at player-update frequency (a few Hz, well
under 30 buffs) — NOT a per-damage-event hot path — so a pure-Python
snapshot-diff accumulator is the right tool (the project's Cython rule targets
per-event / heap-scan hot loops, which this is not).

Uptime model: on each snapshot the elapsed interval since the previous snapshot
is credited to every buff that was active in the previous snapshot. ``uptime_pct``
= accumulated active time / encounter window. A buff seen for the first time
increments ``apply_count`` (refresh/re-application counter).
"""

from __future__ import annotations

import threading
from typing import Any, Dict, List, Optional


def _now_ms() -> float:
    import time
    return time.time() * 1000.0


class BuffUptimeTracker:
    """Accumulate per-buff uptime over an encounter window from snapshots."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        # buff_id -> {name, total_ms, apply_count, layer, max_layer}
        self._buffs: Dict[int, Dict[str, Any]] = {}
        self._active: set = set()
        self._t0: float = 0.0          # window start (client clock, ms)
        self._last_t: float = 0.0      # last snapshot time (ms)

    def reset(self, now_ms: Optional[float] = None) -> None:
        ts = float(now_ms if now_ms is not None else _now_ms())
        with self._lock:
            self._buffs.clear()
            self._active = set()
            self._t0 = ts
            self._last_t = ts

    def update(self, buffs: Optional[List[Dict[str, Any]]],
               now_ms: Optional[float] = None) -> None:
        """Ingest one ``self_buffs`` snapshot."""
        ts = float(now_ms if now_ms is not None else _now_ms())
        with self._lock:
            if self._t0 <= 0.0:
                self._t0 = ts
            dt = (ts - self._last_t) if self._last_t > 0.0 else 0.0
            # Credit the just-elapsed interval to buffs active over it.
            if dt > 0.0 and self._active:
                for bid in self._active:
                    rec = self._buffs.get(bid)
                    if rec is not None:
                        rec['total_ms'] += dt
            new_active: set = set()
            for b in (buffs or []):
                if not isinstance(b, dict):
                    continue
                try:
                    bid = int(b.get('id') or b.get('buff_id') or 0)
                except (TypeError, ValueError):
                    continue
                if bid <= 0:
                    continue
                new_active.add(bid)
                rec = self._buffs.get(bid)
                if rec is None:
                    rec = {'name': str(b.get('name') or ''), 'total_ms': 0.0,
                           'apply_count': 0, 'layer': 0, 'max_layer': 0}
                    self._buffs[bid] = rec
                nm = b.get('name')
                if nm and not rec['name']:
                    rec['name'] = str(nm)
                try:
                    lyr = int(b.get('layer') or b.get('count') or 0)
                except (TypeError, ValueError):
                    lyr = 0
                rec['layer'] = lyr
                if lyr > rec['max_layer']:
                    rec['max_layer'] = lyr
                if bid not in self._active:
                    rec['apply_count'] += 1
            self._active = new_active
            self._last_t = ts

    def snapshot(self, now_ms: Optional[float] = None) -> Dict[str, Any]:
        """Return {elapsed_ms, buffs:[{buff_id,name,uptime_ms,uptime_pct,
        apply_count,layer,max_layer,active}]} sorted by uptime desc."""
        ts = float(now_ms if now_ms is not None else _now_ms())
        with self._lock:
            elapsed = max(1.0, ts - self._t0) if self._t0 > 0.0 else 1.0
            open_dt = max(0.0, ts - self._last_t)
            rows: List[Dict[str, Any]] = []
            for bid, rec in self._buffs.items():
                active = bid in self._active
                total = rec['total_ms'] + (open_dt if active else 0.0)
                rows.append({
                    'buff_id': bid,
                    'name': rec['name'] or f'#{bid}',
                    'uptime_ms': int(total),
                    'uptime_pct': round(min(1.0, total / elapsed), 3),
                    'apply_count': rec['apply_count'],
                    'layer': rec['layer'],
                    'max_layer': rec['max_layer'],
                    'active': active,
                })
            rows.sort(key=lambda r: r['uptime_ms'], reverse=True)
            return {'elapsed_ms': int(elapsed), 'buffs': rows}
