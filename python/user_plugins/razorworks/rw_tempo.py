# -*- coding: utf-8 -*-
"""RW module — unified time manipulation engine.

Four systems sharing one tick-record store per entity:

  Recall      — aim at past position within lag-compensation window
  Selfmon       — predict own movement for pre-aim and auto-halt
  Interpolation  — cubic hermite between two known samples for smooth ESP
  Extrapolation  — predict future position via velocity + acceleration

Data flow:
  snapshot tick → TickRecord → store per entity
                              ├── Recall.best_tick(crosshair)
                              ├── Interpolation.at(t)
                              └── Extrapolation.predict(dt)
  local player  → SelfTracker → peek prediction / strafe state
"""

from __future__ import annotations

import math
import time
from collections import deque
from typing import Dict, List, Optional, Sequence, Tuple

Vec3 = Tuple[float, float, float]

# ── Shared config ────────────────────────────────────────────────────────

MAX_RECORDS = 64          # ticks per entity
LAG_COMP_WINDOW = 0.2     # seconds — target server max rewind
STATIONARY_SPEED = 5.0    # below = standing still


def _v3(a: Vec3, b: Vec3, op: str) -> Vec3:
    if op == "+":
        return (a[0]+b[0], a[1]+b[1], a[2]+b[2])
    return (a[0]-b[0], a[1]-b[1], a[2]-b[2])


def _v3s(v: Vec3, s: float) -> Vec3:
    return (v[0]*s, v[1]*s, v[2]*s)


def _v3len(v: Vec3) -> float:
    return math.sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2])


def _v3dot(a: Vec3, b: Vec3) -> float:
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]


def _angle_to(src: Vec3, dst: Vec3) -> Tuple[float, float]:
    dx, dy, dz = dst[0]-src[0], dst[1]-src[1], dst[2]-src[2]
    d = math.hypot(dx, dy)
    if d < 1e-6:
        return (0.0, 0.0)
    return (-math.degrees(math.atan2(dz, d)), math.degrees(math.atan2(dy, dx)))


def _angle_fov(a: Tuple[float,float], b: Tuple[float,float]) -> float:
    dp = a[0]-b[0]
    dy = (a[1]-b[1]+180) % 360 - 180
    return math.hypot(dp, dy)


def _hermite(p0: float, v0: float, p1: float, v1: float, t: float) -> float:
    """Cubic hermite interpolation between two samples."""
    t2 = t*t; t3 = t2*t
    h00 = 2*t3 - 3*t2 + 1
    h10 = t3 - 2*t2 + t
    h01 = -2*t3 + 3*t2
    h11 = t3 - t2
    return h00*p0 + h10*v0 + h01*p1 + h11*v1


def _catmull_rom(p0: float, p1: float, p2: float, p3: float, t: float) -> float:
    """Catmull-Rom 4-point interpolation (uniform parameterisation)."""
    return 0.5 * ((2*p1) + (-p0+p2)*t + (2*p0-5*p1+4*p2-p3)*t*t
                  + (-p0+3*p1-3*p2+p3)*t*t*t)


# ═══════════════════════════════════════════════════════════════════════
#  TickRecord — one snapshot of an entity's state
# ═══════════════════════════════════════════════════════════════════════

class TickRecord:
    __slots__ = ("pos", "head", "vel", "t", "flags")

    def __init__(self, pos: Vec3, head: Optional[Vec3],
                 vel: Vec3, t: float, flags: int = 0):
        self.pos = pos
        self.head = head        # head bone world pos (or None)
        self.vel = vel          # (vx, vy, vz) game units/s
        self.t = t              # perf_counter timestamp
        self.flags = flags      # 0=normal, 1=dormant


# ═══════════════════════════════════════════════════════════════════════
#  EntityTimeline — per-entity record ring
# ═══════════════════════════════════════════════════════════════════════

class EntityTimeline:
    """Ring buffer of TickRecords for one entity."""

    def __init__(self) -> None:
        self._ring: deque[TickRecord] = deque(maxlen=MAX_RECORDS)

    def push(self, rec: TickRecord) -> None:
        self._ring.append(rec)

    @property
    def latest(self) -> Optional[TickRecord]:
        return self._ring[-1] if self._ring else None

    @property
    def count(self) -> int:
        return len(self._ring)

    def records_in_window(self, window_s: float,
                          now: Optional[float] = None) -> List[TickRecord]:
        """Return records within *window_s* seconds from now (newest first)."""
        t = now or time.perf_counter()
        cutoff = t - window_s
        out = []
        for rec in reversed(self._ring):
            if rec.t < cutoff:
                break
            out.append(rec)
        return out

    def two_nearest(self, t: float) -> Optional[Tuple[TickRecord, TickRecord]]:
        """Return the two records bracketing timestamp *t* for interpolation."""
        if len(self._ring) < 2:
            return None
        prev = None
        for rec in self._ring:
            if rec.t >= t:
                if prev is not None:
                    return (prev, rec)
                return None
            prev = rec
        return None

    def velocity_ema(self, alpha: float = 0.4) -> Vec3:
        """Exponential moving average of velocity from recent records."""
        vx = vy = vz = 0.0
        first = True
        for rec in reversed(self._ring):
            if first:
                vx, vy, vz = rec.vel
                first = False
            else:
                vx = alpha * rec.vel[0] + (1-alpha) * vx
                vy = alpha * rec.vel[1] + (1-alpha) * vy
                vz = alpha * rec.vel[2] + (1-alpha) * vz
        return (vx, vy, vz)

    def detect_adad(self, window_s: float = 0.4) -> Tuple[bool, Optional[Vec3]]:
        """Detect ADAD strafing pattern. Returns (is_adad, envelope_center)."""
        if len(self._ring) < 4:
            return (False, None)
        now = self._ring[-1].t
        recs = [r for r in self._ring if now - r.t <= window_s]
        if len(recs) < 4:
            return (False, None)
        sign_flips = 0
        sx, sy, sz = 0.0, 0.0, 0.0
        for i in range(1, len(recs)):
            if (recs[i].vel[0] * recs[i-1].vel[0] < 0 or
                recs[i].vel[1] * recs[i-1].vel[1] < 0):
                sign_flips += 1
            sx += recs[i].pos[0]
            sy += recs[i].pos[1]
            sz += recs[i].pos[2]
        if sign_flips >= 2:
            n = len(recs) - 1
            return (True, (sx/n, sy/n, sz/n))
        return (False, None)

    def four_nearest(self, t: float) -> Optional[Tuple[TickRecord, TickRecord, TickRecord, TickRecord]]:
        """Return four records surrounding timestamp *t* for Catmull-Rom.

        Returns (r_before_prev, r_prev, r_next, r_after_next) if available,
        otherwise None (caller should fall back to two-point hermite).
        """
        if len(self._ring) < 4:
            return None
        # Find the two records bracketing t
        idx1 = None
        for i, rec in enumerate(self._ring):
            if rec.t >= t:
                idx1 = i
                break
        if idx1 is None or idx1 < 2:
            return None
        idx0 = idx1 - 1
        if idx1 + 1 >= len(self._ring):
            return None
        return (self._ring[idx0-1], self._ring[idx0], self._ring[idx1], self._ring[idx1+1])

    def acceleration(self) -> Vec3:
        """Estimate acceleration from last two records."""
        if len(self._ring) < 3:
            return (0.0, 0.0, 0.0)
        r1, r2 = self._ring[-2], self._ring[-1]
        dt = r2.t - r1.t
        if dt < 0.001:
            return (0.0, 0.0, 0.0)
        dv = _v3(r2.vel, r1.vel, "-")
        a = _v3s(dv, 1.0/dt)
        mag = _v3len(a)
        if mag > 3000.0:
            a = _v3s(a, 3000.0/mag)
        return a


# ═══════════════════════════════════════════════════════════════════════
#  Recall — aim at past positions within lag compensation window
# ═══════════════════════════════════════════════════════════════════════

class Recall:
    """Latency-window aim optimisation (NOT time-travel recall).

    target server rewinds hitboxes by the CLIENT'S MEASURED RTT — NOT by
    a client-reported tick.  External tools cannot inflate this window.
    Effective range: your actual ping + interp buffer ≈ 30–80 ms.

    Within that small window this IS useful: pick the historical head
    position closest to crosshair.  30 ms at full sprint ≈ 7.5 units
    (2× head hitbox radius), enough to turn a near-miss into a headshot
    against peeking / jiggling enemies.

    Default 50 ms — raise only if your ping is genuinely higher.
    Setting 200 ms when your ping is 30 ms just makes you aim at stale
    positions the server will NOT validate → lower accuracy.
    """

    def __init__(self) -> None:
        self.enabled: bool = False
        self.max_window_ms: float = 50.0    # ≈ realistic RTT + interp
        self.prefer_head: bool = True

    def best_record(self, timeline: EntityTimeline,
                    eye: Vec3, view_angles: Tuple[float, float],
                    now: Optional[float] = None) -> Optional[TickRecord]:
        """Return the historical record where the head/origin is closest
        to the current crosshair direction."""
        if not self.enabled:
            return timeline.latest
        window = self.max_window_ms / 1000.0
        records = timeline.records_in_window(window, now)
        if not records:
            return timeline.latest

        best_rec = None
        best_score = 999.0
        ref_t = now or time.perf_counter()

        for rec in records:
            target = rec.head if (self.prefer_head and rec.head) else rec.pos
            aim = _angle_to(eye, target)
            fov_dist = _angle_fov(view_angles, aim)
            # Freshness penalty: older records score worse
            age_ms = (ref_t - rec.t) * 1000.0
            score = fov_dist + age_ms * 0.01
            if score < best_score:
                best_score = score
                best_rec = rec

        return best_rec or timeline.latest

    def all_valid_positions(self, timeline: EntityTimeline,
                           now: Optional[float] = None) -> List[Vec3]:
        """All recallable head positions (for Overlay rendering)."""
        if not self.enabled:
            return []
        window = self.max_window_ms / 1000.0
        records = timeline.records_in_window(window, now)
        out = []
        for rec in records:
            p = rec.head if (self.prefer_head and rec.head) else rec.pos
            out.append(p)
        return out


# ═══════════════════════════════════════════════════════════════════════
#  SelfTracker — local player movement prediction
# ═══════════════════════════════════════════════════════════════════════

class SelfTracker:
    """Track own position/velocity for peek prediction and strafe timing.

    Peek prediction: when peeking a corner, predict where our eye will
    be in N ms so tracker can pre-aim the target from the future peek
    position rather than current (behind cover) position.
    """

    def __init__(self) -> None:
        self.enabled: bool = False
        self._timeline = EntityTimeline()
        self._strafe_state: str = "idle"   # idle/accel/decel/airborne
        self._ground_speed: float = 0.0

    def reset(self) -> None:
        self._timeline = EntityTimeline()
        self._strafe_state = "idle"

    def update(self, local: Optional[dict], now: Optional[float] = None) -> None:
        if not local:
            return
        t = now or time.perf_counter()
        pos = local.get("origin") or local.get("eye_pos")
        vel = local.get("velocity", (0, 0, 0))
        if not pos:
            return
        self._timeline.push(TickRecord(pos, local.get("eye_pos"), vel, t))

        speed = math.hypot(vel[0], vel[1])
        self._ground_speed = speed
        flags = local.get("flags", 0)
        on_ground = bool(flags & 1) if flags else (abs(vel[2]) < 10)

        if not on_ground:
            self._strafe_state = "airborne"
        elif speed < STATIONARY_SPEED:
            self._strafe_state = "idle"
        else:
            prev = self._timeline.latest
            if prev and _v3len(prev.vel) < speed:
                self._strafe_state = "accel"
            else:
                self._strafe_state = "decel"

    def predict_eye(self, dt: float) -> Optional[Vec3]:
        """Predict own eye position at t+dt (for peek pre-aim)."""
        if not self.enabled or not self._timeline.latest:
            return None
        rec = self._timeline.latest
        vel = self._timeline.velocity_ema(0.6)
        if _v3len(vel) < STATIONARY_SPEED:
            return rec.head or rec.pos
        pred = _v3(rec.pos, _v3s(vel, dt), "+")
        if rec.head:
            eye_off = _v3(rec.head, rec.pos, "-")
            return _v3(pred, eye_off, "+")
        return pred

    def should_autostop(self, threshold: float = 10.0) -> bool:
        return self._ground_speed > threshold

    def counter_keys(self) -> List[int]:
        """VK codes for counter-strafe based on current velocity direction."""
        if not self._timeline.latest:
            return []
        vel = self._timeline.latest.vel
        keys = []
        if abs(vel[0]) > STATIONARY_SPEED:
            keys.append(0x44 if vel[0] > 0 else 0x41)
        if abs(vel[1]) > STATIONARY_SPEED:
            keys.append(0x57 if vel[1] > 0 else 0x53)
        return keys

    @property
    def speed(self) -> float:
        return self._ground_speed

    @property
    def strafe_state(self) -> str:
        return self._strafe_state

    @property
    def is_moving(self) -> bool:
        return self._ground_speed > STATIONARY_SPEED


# ═══════════════════════════════════════════════════════════════════════
#  Interpolation — smooth between known positions
# ═══════════════════════════════════════════════════════════════════════

class Interpolation:
    """Cubic hermite interpolation between two tick records.

    Game reads happen at 125Hz but rendering/aiming may want sub-tick
    positions. Hermite uses position + velocity at both endpoints for
    smooth C¹-continuous curves (no snapping between samples).

    Lerp fallback when only position (no velocity) is available.
    """

    def __init__(self) -> None:
        self.enabled: bool = False
        self.render_delay_ms: float = 0.0   # intentional delay for smoother ESP

    def at(self, timeline: EntityTimeline,
           t: Optional[float] = None) -> Optional[Vec3]:
        """Interpolated position at timestamp *t*.

        Uses Catmull-Rom 4-point interpolation when four surrounding samples
        are available, falling back to cubic hermite 2-point otherwise.
        """
        if not self.enabled:
            latest = timeline.latest
            return latest.pos if latest else None

        query_t = (t or time.perf_counter()) - self.render_delay_ms / 1000.0

        # Try 4-point Catmull-Rom first
        quad = timeline.four_nearest(query_t)
        if quad is not None:
            rm1, r0, r1, r2 = quad
            dt = r1.t - r0.t
            if dt > 0.0001:
                frac = max(0.0, min(1.0, (query_t - r0.t) / dt))
                result = []
                for i in range(3):
                    result.append(_catmull_rom(rm1.pos[i], r0.pos[i],
                                              r1.pos[i], r2.pos[i], frac))
                return (result[0], result[1], result[2])

        # Fallback: 2-point cubic hermite
        pair = timeline.two_nearest(query_t)
        if pair is None:
            latest = timeline.latest
            return latest.pos if latest else None

        r0, r1 = pair
        dt = r1.t - r0.t
        if dt < 0.0001:
            return r1.pos

        frac = (query_t - r0.t) / dt
        frac = max(0.0, min(1.0, frac))

        result = []
        for i in range(3):
            p0 = r0.pos[i]
            p1 = r1.pos[i]
            v0 = r0.vel[i] * dt   # tangent scaled by interval
            v1 = r1.vel[i] * dt
            result.append(_hermite(p0, v0, p1, v1, frac))
        return (result[0], result[1], result[2])

    def head_at(self, timeline: EntityTimeline,
                t: Optional[float] = None) -> Optional[Vec3]:
        """Interpolated head position at timestamp *t*.

        Uses Catmull-Rom 4-point when available, hermite 2-point fallback.
        """
        if not self.enabled:
            latest = timeline.latest
            return latest.head if latest else None

        query_t = (t or time.perf_counter()) - self.render_delay_ms / 1000.0

        # Try 4-point Catmull-Rom first
        quad = timeline.four_nearest(query_t)
        if quad is not None:
            rm1, r0, r1, r2 = quad
            if rm1.head and r0.head and r1.head and r2.head:
                dt = r1.t - r0.t
                if dt > 0.0001:
                    frac = max(0.0, min(1.0, (query_t - r0.t) / dt))
                    result = []
                    for i in range(3):
                        result.append(_catmull_rom(rm1.head[i], r0.head[i],
                                                   r1.head[i], r2.head[i], frac))
                    return (result[0], result[1], result[2])

        # Fallback: 2-point cubic hermite
        pair = timeline.two_nearest(query_t)
        if pair is None:
            latest = timeline.latest
            return latest.head if latest else None

        r0, r1 = pair
        if not r0.head or not r1.head:
            return r1.head or r0.head

        dt = r1.t - r0.t
        if dt < 0.0001:
            return r1.head

        frac = max(0.0, min(1.0, (query_t - r0.t) / dt))
        result = []
        for i in range(3):
            p0 = r0.head[i]
            p1 = r1.head[i]
            v0 = r0.vel[i] * dt
            v1 = r1.vel[i] * dt
            result.append(_hermite(p0, v0, p1, v1, frac))
        return (result[0], result[1], result[2])


# ═══════════════════════════════════════════════════════════════════════
#  Extrapolation — predict future position
# ═══════════════════════════════════════════════════════════════════════

class Extrapolation:
    """Predict where an entity WILL BE at t+dt.

    Uses EMA velocity + acceleration from the timeline.
    Confidence scoring based on direction consistency.
    """

    def __init__(self) -> None:
        self.enabled: bool = False
        self.lookahead_ms: float = 20.0
        self.min_confidence: float = 0.3

    def predict(self, timeline: EntityTimeline,
                now: Optional[float] = None) -> Tuple[Vec3, float]:
        """Return (predicted_pos, confidence)."""
        latest = timeline.latest
        if not latest or not self.enabled:
            return (latest.pos if latest else (0,0,0), 0.0)

        # ADAD detection: if target is strafing, aim at envelope center
        is_adad, center = timeline.detect_adad()
        if is_adad and center is not None:
            return (center, 0.7)

        # Velocity sign-flip cooldown: if the last 2 records have opposite
        # velocity signs and the data is very fresh (< 30ms), hold position
        if timeline.count >= 2:
            r_prev = list(timeline._ring)[-2]
            age = latest.t - r_prev.t
            if age < 0.030:
                if (latest.vel[0] * r_prev.vel[0] < 0 or
                    latest.vel[1] * r_prev.vel[1] < 0):
                    return (latest.pos, 0.3)

        vel = timeline.velocity_ema(0.5)
        speed = _v3len(vel)
        if speed < STATIONARY_SPEED:
            return (latest.pos, 0.0)

        dt = self.lookahead_ms / 1000.0
        accel = timeline.acceleration()

        pred = _v3(latest.pos, _v3s(vel, dt), "+")
        pred = _v3(pred, _v3s(accel, 0.5*dt*dt), "+")

        # Sanity cap
        delta = _v3(pred, latest.pos, "-")
        dist = _v3len(delta)
        max_d = 450.0 * dt
        if dist > max_d:
            pred = _v3(latest.pos, _v3s(delta, max_d/dist), "+")

        # Multi-dimensional confidence scoring
        conf = self._direction_confidence(timeline, vel, speed)
        return (pred, conf)

    def predict_head(self, timeline: EntityTimeline,
                     now: Optional[float] = None) -> Tuple[Optional[Vec3], float]:
        latest = timeline.latest
        if not latest or not latest.head or not self.enabled:
            return (latest.head if latest else None, 0.0)

        pred_origin, conf = self.predict(timeline, now)
        if conf < self.min_confidence:
            return (latest.head, conf)

        head_off = _v3(latest.head, latest.pos, "-")
        return (_v3(pred_origin, head_off, "+"), conf)

    def _direction_confidence(self, tl: EntityTimeline,
                              vel: Vec3, speed: float) -> float:
        """Multi-dimensional confidence: dir_conf * speed_stability * sample_factor * timing_regularity."""
        if tl.count < 3 or speed < STATIONARY_SPEED:
            return 0.0
        records = list(tl._ring)
        count = len(records)

        # 1) Direction consistency (original logic)
        cur_dir = _v3s(vel, 1.0/speed)
        prev_vel = records[-2].vel
        prev_speed = _v3len(prev_vel)
        if prev_speed < STATIONARY_SPEED:
            dir_conf = 0.5
        else:
            prev_dir = _v3s(prev_vel, 1.0/prev_speed)
            dot = _v3dot(cur_dir, prev_dir)
            dir_conf = max(0.0, min(1.0, (dot + 1.0) * 0.5))

        # 2) Speed stability: 1 - min(1, stddev / mean_speed)
        speeds = [_v3len(r.vel) for r in records[-8:]]
        n_sp = len(speeds)
        mean_speed = sum(speeds) / n_sp
        if mean_speed < STATIONARY_SPEED:
            speed_stability = 0.5
        else:
            variance = sum((s - mean_speed) ** 2 for s in speeds) / n_sp
            stddev = math.sqrt(variance)
            speed_stability = 1.0 - min(1.0, stddev / mean_speed)

        # 3) Sample factor: min(1, count / 8)
        sample_factor = min(1.0, count / 8.0)

        # 4) Timing regularity: based on dt jitter among recent records
        if count >= 3:
            dts = [records[i].t - records[i-1].t for i in range(-min(count-1, 7), 0)]
            if dts:
                mean_dt = sum(dts) / len(dts)
                if mean_dt > 0.0001:
                    dt_var = sum((d - mean_dt) ** 2 for d in dts) / len(dts)
                    dt_jitter = math.sqrt(dt_var) / mean_dt
                    timing_regularity = max(0.0, 1.0 - dt_jitter)
                else:
                    timing_regularity = 0.5
            else:
                timing_regularity = 0.5
        else:
            timing_regularity = 0.5

        return dir_conf * speed_stability * sample_factor * timing_regularity


# ═══════════════════════════════════════════════════════════════════════
#  TempoEngine — unified manager
# ═══════════════════════════════════════════════════════════════════════

class TempoEngine:
    """Manages all four time systems with a shared timeline store."""

    def __init__(self) -> None:
        self.recall = Recall()
        self.selfmon = SelfTracker()
        self.interp = Interpolation()
        self.extrap = Extrapolation()
        self._timelines: Dict[int, EntityTimeline] = {}
        self._last_clean: float = 0.0

    def reset(self) -> None:
        self._timelines.clear()
        self.selfmon.reset()

    def feed(self, index: int, pos: Vec3,
             head: Optional[Vec3] = None,
             vel: Vec3 = (0, 0, 0),
             now: Optional[float] = None) -> None:
        """Record one tick of entity state."""
        t = now or time.perf_counter()
        if index not in self._timelines:
            self._timelines[index] = EntityTimeline()
        self._timelines[index].push(TickRecord(pos, head, vel, t))

    def feed_local(self, local: Optional[dict],
                   now: Optional[float] = None) -> None:
        """Record local player state for selfmon."""
        self.selfmon.update(local, now)

    def timeline(self, index: int) -> Optional[EntityTimeline]:
        return self._timelines.get(index)

    # ── Tracker target resolution ──

    def resolve_aim_target(self, index: int,
                           eye: Vec3,
                           view_angles: Tuple[float, float],
                           now: Optional[float] = None) -> Optional[Vec3]:
        """Pick the best head position for aiming at entity *index*.

        Priority: recall > extrapolation > interpolation > current.
        Returns the head position to aim at.
        """
        tl = self._timelines.get(index)
        if tl is None or tl.latest is None:
            return None

        # Recall: find best historical position
        if self.recall.enabled:
            best = self.recall.best_record(tl, eye, view_angles, now)
            if best and best.head:
                return best.head

        # Extrapolation: predict future position
        if self.extrap.enabled:
            pred_head, conf = self.extrap.predict_head(tl, now)
            if pred_head and conf >= self.extrap.min_confidence:
                return pred_head

        # Interpolation: smooth current position
        if self.interp.enabled:
            h = self.interp.head_at(tl, now and now or None)
            if h:
                return h

        # Fallback: latest head
        return tl.latest.head

    # ── Reactor target resolution ──

    def resolve_reactor_head(self, index: int,
                                now: Optional[float] = None) -> Optional[Vec3]:
        """Get best head position for hitchance calculation."""
        tl = self._timelines.get(index)
        if tl is None or tl.latest is None:
            return None

        if self.extrap.enabled:
            pred, conf = self.extrap.predict_head(tl, now)
            if pred and conf >= self.extrap.min_confidence:
                return pred

        if self.interp.enabled:
            h = self.interp.head_at(tl)
            if h:
                return h

        return tl.latest.head

    # ── Overlay smooth position ──

    def smooth_position(self, index: int,
                        now: Optional[float] = None) -> Optional[Vec3]:
        """Interpolated origin for Overlay rendering."""
        tl = self._timelines.get(index)
        if not tl:
            return None
        if self.interp.enabled:
            return self.interp.at(tl, now)
        return tl.latest.pos if tl.latest else None

    # ── Peek pre-aim ──

    def peek_eye(self, dt: float = 0.1) -> Optional[Vec3]:
        """Predict own eye position for peek pre-aim."""
        return self.selfmon.predict_eye(dt)

    # ── Cleanup ──

    def cleanup(self, alive: set, now: Optional[float] = None) -> None:
        t = now or time.perf_counter()
        if t - self._last_clean < 1.0:
            return
        self._last_clean = t
        dead = [k for k in self._timelines if k not in alive]
        for k in dead:
            del self._timelines[k]
