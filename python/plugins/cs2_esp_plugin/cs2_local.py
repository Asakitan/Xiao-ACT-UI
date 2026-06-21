# -*- coding: utf-8 -*-
"""CS2 plugin — local player state reader.

Reads the local player pawn from dwLocalPlayerPawn and extracts all
fields needed by aimbot, RCS, triggerbot, and visibility check:
    eye position, view angles, aim punch, shots fired,
    crosshair entity, team, velocity, scoped state, FOV scale.
"""

from __future__ import annotations

import struct
from typing import Optional, Tuple

Vec3 = Tuple[float, float, float]


def read_local_player(read_fn, module_base: int,
                      dw_local_pawn: int,
                      offsets: dict,
                      global_offsets: Optional[dict] = None) -> Optional[dict]:
    """Read the local player state from memory.

    ``read_fn(addr, size) -> Optional[bytes]`` is the reader callback.
    ``dw_local_pawn`` is the RVA of dwLocalPlayerPawn in client.dll.
    ``offsets`` is a dict of field name → offset within the pawn.
    ``global_offsets`` (optional) has dwViewAngles etc for direct reads.

    Returns a dict with all local player fields, or None if not in-game.
    """
    if not module_base or not dw_local_pawn:
        return None

    pawn_ptr_raw = read_fn(module_base + dw_local_pawn, 8)
    if not pawn_ptr_raw or len(pawn_ptr_raw) < 8:
        return None
    pawn = struct.unpack_from("<Q", pawn_ptr_raw)[0]
    if not pawn:
        return None

    result = {"pawn": pawn}
    g_off = global_offsets or {}

    # Health + team + lifeState — skip if dead
    h_off = offsets.get("m_iHealth", 0)
    t_off = offsets.get("m_iTeamNum", 0)
    life_off = offsets.get("m_lifeState", 0)
    if h_off:
        raw = read_fn(pawn + h_off, 4)
        result["health"] = struct.unpack_from("<i", raw)[0] if raw and len(raw) >= 4 else 0
    if t_off:
        raw = read_fn(pawn + t_off, 4)
        result["team"] = struct.unpack_from("<i", raw)[0] if raw and len(raw) >= 4 else 0
    if life_off:
        raw = read_fn(pawn + life_off, 1)
        result["life_state"] = raw[0] if raw else 0

    if result.get("health", 0) <= 0 or result.get("life_state", 0) != 0:
        return None

    # View angles: prefer dwViewAngles (real-time input angle) over
    # m_angEyeAngles (networked, up to 1 tick lag).
    # Bullet direction = ViewAngles + aimPunch × 2.0
    dw_va = g_off.get("dwViewAngles", 0)
    if dw_va and module_base:
        raw = read_fn(module_base + dw_va, 12)
        if raw and len(raw) >= 12:
            p, y, _r = struct.unpack_from("<3f", raw)
            result["view_angles"] = (p, y)
    if "view_angles" not in result:
        eye_ang_off = offsets.get("m_angEyeAngles", 0)
        if eye_ang_off:
            raw = read_fn(pawn + eye_ang_off, 12)
            if raw and len(raw) >= 12:
                p, y, _r = struct.unpack_from("<3f", raw)
                result["view_angles"] = (p, y)

    # Eye position = origin + vecViewOffset
    origin_off = offsets.get("m_pGameSceneNode", 0)
    view_off = offsets.get("m_vecViewOffset", 0)
    vec_origin_off = offsets.get("m_vecOrigin", 0)
    if origin_off and vec_origin_off:
        sn_raw = read_fn(pawn + origin_off, 8)
        if sn_raw and len(sn_raw) >= 8:
            scene_node = struct.unpack_from("<Q", sn_raw)[0]
            if scene_node:
                pos_raw = read_fn(scene_node + vec_origin_off, 12)
                if pos_raw and len(pos_raw) >= 12:
                    ox, oy, oz = struct.unpack_from("<3f", pos_raw)
                    result["origin"] = (ox, oy, oz)
                    if view_off:
                        vo_raw = read_fn(pawn + view_off, 12)
                        if vo_raw and len(vo_raw) >= 12:
                            vx, vy, vz = struct.unpack_from("<3f", vo_raw)
                            result["eye_pos"] = (ox + vx, oy + vy, oz + vz)
                        else:
                            result["eye_pos"] = (ox, oy, oz + 64.0)
                    else:
                        result["eye_pos"] = (ox, oy, oz + 64.0)

    # Aim punch (m_aimPunchAngle: pitch, yaw, roll — 3 floats)
    punch_off = offsets.get("m_aimPunchAngle", 0)
    if punch_off:
        raw = read_fn(pawn + punch_off, 12)
        if raw and len(raw) >= 12:
            pp, py, _pr = struct.unpack_from("<3f", raw)
            result["aim_punch"] = (pp, py)

    # Shots fired
    shots_off = offsets.get("m_iShotsFired", 0)
    if shots_off:
        raw = read_fn(pawn + shots_off, 4)
        result["shots_fired"] = struct.unpack_from("<i", raw)[0] if raw and len(raw) >= 4 else 0

    # Crosshair entity (m_iIDEntIndex)
    cross_off = offsets.get("m_iIDEntIndex", 0)
    if cross_off:
        raw = read_fn(pawn + cross_off, 4)
        result["crosshair_entity"] = struct.unpack_from("<i", raw)[0] if raw and len(raw) >= 4 else -1

    # Velocity
    vel_off = offsets.get("m_vecVelocity", 0)
    if vel_off:
        raw = read_fn(pawn + vel_off, 12)
        if raw and len(raw) >= 12:
            vx, vy, vz = struct.unpack_from("<3f", raw)
            result["velocity"] = (vx, vy, vz)

    # Scoped state
    scoped_off = offsets.get("m_bIsScoped", 0)
    if scoped_off:
        raw = read_fn(pawn + scoped_off, 1)
        result["is_scoped"] = bool(raw[0]) if raw else False

    # FOV sensitivity adjust
    fov_off = offsets.get("m_flFOVSensitivityAdjust", 0)
    if fov_off:
        raw = read_fn(pawn + fov_off, 4)
        if raw and len(raw) >= 4:
            result["fov_scale"] = struct.unpack_from("<f", raw)[0]

    # Flags (FL_ONGROUND etc.)
    flags_off = offsets.get("m_fFlags", 0)
    if flags_off:
        raw = read_fn(pawn + flags_off, 4)
        if raw and len(raw) >= 4:
            result["flags"] = struct.unpack_from("<I", raw)[0]

    # Weapon accuracy penalty (m_fAccuracyPenalty on active weapon)
    weapon_off = offsets.get("m_pClippingWeapon", 0)
    acc_off = offsets.get("m_fAccuracyPenalty", 0)
    if weapon_off and acc_off:
        wp_raw = read_fn(pawn + weapon_off, 8)
        if wp_raw and len(wp_raw) >= 8:
            weapon_ptr = struct.unpack_from("<Q", wp_raw)[0]
            if weapon_ptr:
                ac_raw = read_fn(weapon_ptr + acc_off, 4)
                if ac_raw and len(ac_raw) >= 4:
                    result["accuracy_penalty"] = struct.unpack_from("<f", ac_raw)[0]

    return result


# --- Hit rate statistics -----------------------------------------------------

class HitStats:
    """Track shots fired vs hits for accuracy display."""

    def __init__(self) -> None:
        self.total_shots: int = 0
        self.hits: int = 0
        self._prev_shots: int = 0
        self._prev_enemy_health: dict = {}  # index → last known health

    def reset(self) -> None:
        self.total_shots = 0
        self.hits = 0
        self._prev_shots = 0
        self._prev_enemy_health.clear()

    def update(self, shots_fired: int, enemies: list) -> None:
        """Call each tick with current shots_fired and enemy list.

        Detects new shots by shots_fired increase, and detects hits
        by enemy health decrease in the same window.
        """
        if shots_fired > self._prev_shots and self._prev_shots > 0:
            new_shots = shots_fired - self._prev_shots
            self.total_shots += new_shots
            for e in enemies:
                idx = e.get("index", -1)
                hp = e.get("health", 0)
                prev_hp = self._prev_enemy_health.get(idx, hp)
                if hp < prev_hp:
                    self.hits += min(new_shots, 1)
                    break

        if shots_fired == 0 and self._prev_shots > 0:
            pass  # spray ended, stats carry over
        self._prev_shots = shots_fired

        self._prev_enemy_health.clear()
        for e in enemies:
            self._prev_enemy_health[e.get("index", -1)] = e.get("health", 0)

    @property
    def accuracy(self) -> float:
        if self.total_shots == 0:
            return 0.0
        return self.hits / self.total_shots

    @property
    def display(self) -> str:
        pct = self.accuracy * 100
        return f"{self.hits}/{self.total_shots} ({pct:.0f}%)"
