# -*- coding: utf-8 -*-
"""CS2 plugin — Engine A reader facade.

Extended to support full combat features: local player state, bone
reading, and spotted/visibility data alongside the entity list.

Bug fixes from audit (2026-06-21):
  - entity list traversal adds entity_list_offset (+0x10 past header)
  - pentity_offset default 0 (m_pEntity at CEntityIdentity base)
  - m_iTeamNum read as uint8 (0x3EB is a byte field in Source 2)
"""

from __future__ import annotations

import struct
from typing import Any, Optional

try:
    from mem_probe import rt_io
except Exception:
    rt_io = None  # type: ignore[assignment]

from .cs2_bones import read_bones, head_position
from .cs2_local import read_local_player
from .cs2_offsets import OffsetConfig
from .cs2_patterns import Pattern, default_patterns, scan_module


class ReaderError(Exception):
    pass


class EngineAReader:
    """Read-only reader backed by Engine A (``ENGINE_READONLY``)."""

    ENGINE_NAME = "A"
    ENGINE_ID = "ENGINE_READONLY"

    def __init__(self, config: OffsetConfig, *, use_pattern_scan: bool = True) -> None:
        self._config = config
        self._attached_pid: int = 0
        self._module_base: int = 0
        self._module_size: int = 0
        self._last_error: str = ""
        self._use_pattern_scan = bool(use_pattern_scan)
        self._scanned_rvas: dict = {}
        self._scan_attempted = False

    # ── lifecycle ───────────────────────────────────────────────────────
    def ensure_engine(self) -> bool:
        self._last_error = ""
        if rt_io is None:
            self._last_error = "rt_io module unavailable"
            return False
        try:
            rt_io.select_engine(rt_io.ENGINE_READONLY)
        except Exception as exc:
            self._last_error = f"select_engine failed: {exc}"
            return False
        try:
            if not rt_io.ensure_loaded(rt_io.ENGINE_READONLY):
                self._last_error = "Engine A load failed"
                return False
        except Exception as exc:
            self._last_error = f"ensure_loaded failed: {exc}"
            return False
        info = self.engine_info()
        if not info.get("can_read"):
            self._last_error = "Engine A loaded but cannot read"
            return False
        return True

    def engine_info(self) -> dict:
        if rt_io is None:
            return {"tier": "S", "capabilities": 0, "can_read": False, "can_write": False}
        try:
            return rt_io.engine_info()
        except Exception:
            return {"tier": "S", "capabilities": 0, "can_read": False, "can_write": False}

    def status(self) -> dict:
        base = {"tier": "S", "driver_available": False, "attached_pid": 0}
        if rt_io is None:
            return base
        try:
            s = rt_io.status() or {}
            base.update(s)
        except Exception:
            pass
        base["attached_pid"] = self._attached_pid
        base["module_base"] = self._module_base
        base["last_error"] = self._last_error
        return base

    def find_process(self) -> int:
        name = (self._config.target_process or "").lower()
        if not name:
            return 0
        if rt_io is None:
            return 0
        try:
            return rt_io.find_process_by_name(name)
        except AttributeError:
            pass
        # Manual EPROCESS iteration via rt_io kernel primitives
        try:
            initial = rt_io._r1_fp()
            if not initial:
                return 0
            off_links = getattr(rt_io, "_OFF_LINKS", 0x448)
            off_pid = getattr(rt_io, "_OFF_PID", 0x440)
            off_name = getattr(rt_io, "_OFF_NAME", 0x5A8)
            cur = rt_io._r1_v8(initial + off_links)
            if not cur:
                return 0
            head = initial + off_links
            visited = set()
            while cur and cur != head and cur not in visited and len(visited) < 512:
                visited.add(cur)
                ep = cur - off_links
                img_raw = rt_io._r1_vn(ep + off_name, 15)
                if img_raw:
                    try:
                        img = img_raw.split(b"\x00", 1)[0].decode("utf-8", "replace").lower()
                        if img == name:
                            pid = rt_io._r1_v8(ep + off_pid)
                            if pid and pid > 0:
                                return int(pid)
                    except Exception:
                        pass
                nxt = rt_io._r1_v8(cur)
                if not nxt:
                    break
                cur = nxt
        except Exception:
            pass
        return 0

    def attach(self, pid: int) -> bool:
        self._last_error = ""
        if rt_io is None:
            self._last_error = "rt_io unavailable"
            return False
        if pid <= 0:
            self._last_error = "invalid pid"
            return False
        try:
            if not rt_io.attach(int(pid)):
                self._last_error = "rt_io.attach returned False"
                return False
        except Exception as exc:
            self._last_error = f"attach failed: {exc}"
            return False
        self._attached_pid = int(pid)
        self._resolve_module_base()
        return True

    def detach(self) -> None:
        if rt_io is not None and self._attached_pid != 0:
            try:
                rt_io.detach()
            except Exception:
                pass
        self._attached_pid = 0
        self._module_base = 0
        self._module_size = 0

    @property
    def attached(self) -> bool:
        return self._attached_pid != 0

    @property
    def module_base(self) -> int:
        return self._module_base

    def scanned_rvas(self) -> dict:
        return dict(self._scanned_rvas)

    # ── pattern scanning ────────────────────────────────────────────────
    def resolve_offsets_by_scan(self) -> dict:
        if self._scan_attempted:
            return dict(self._scanned_rvas)
        self._scan_attempted = True
        if not self._use_pattern_scan:
            return {}
        if self._module_base == 0 or self._module_size == 0:
            return {}
        wanted = ("dwEntityList", "dwViewMatrix", "dwLocalPlayerPawn",
                  "dwLocalPlayerController", "dwViewAngles")
        patterns = [p for p in default_patterns() if p.name in wanted]
        if not patterns:
            return {}
        try:
            resolved = scan_module(self.read, self._module_base,
                                   self._module_size, patterns)
        except Exception as exc:
            self._last_error = f"pattern scan failed: {exc}"
            return {}
        self._scanned_rvas = resolved
        return dict(resolved)

    def _rva(self, name: str) -> int:
        if self._use_pattern_scan:
            scanned = self._scanned_rvas.get(name)
            if scanned:
                return int(scanned)
        return int(self._config.offsets.get(name) or 0)

    # ── module base ─────────────────────────────────────────────────────
    def _resolve_module_base(self) -> bool:
        self._module_base = 0
        self._module_size = 0
        if self._attached_pid == 0 or rt_io is None:
            return False
        target = (self._config.module or "").lower()
        if not target:
            return False
        try:
            ep, cr3 = rt_io._r1_fe(self._attached_pid)
        except Exception:
            return False
        if not ep or not cr3:
            return False
        off_peb = getattr(rt_io, "_OFF_PEB", None)
        if off_peb is None:
            return False
        peb_ptr = rt_io._r1_v8(ep + off_peb)
        if not peb_ptr:
            return False
        # PEB->Ldr at +0x18 (x64)
        ldr_data = rt_io._r5_read(self._attached_pid, peb_ptr + 0x18, 8)
        if not ldr_data:
            return False
        ldr = struct.unpack("<Q", ldr_data)[0]
        if not ldr:
            return False
        # InLoadOrderModuleList at Ldr + 0x10
        list_head = ldr + 0x10
        flink_data = rt_io._r5_read(self._attached_pid, list_head, 8)
        if not flink_data:
            return False
        flink = struct.unpack("<Q", flink_data)[0]
        visited = set()
        while flink and flink != list_head and flink not in visited and len(visited) < 300:
            visited.add(flink)
            # LDR_DATA_TABLE_ENTRY layout (x64):
            #   +0x30  DllBase
            #   +0x40  SizeOfImage
            #   +0x58  BaseDllName.Length
            #   +0x60  BaseDllName.Buffer
            entry_data = rt_io._r5_read(self._attached_pid, flink, 0x68)
            if not entry_data or len(entry_data) < 0x68:
                break
            dll_base = struct.unpack_from("<Q", entry_data, 0x30)[0]
            size_of_image = struct.unpack_from("<I", entry_data, 0x40)[0]
            name_len = struct.unpack_from("<H", entry_data, 0x58)[0]
            name_buf = struct.unpack_from("<Q", entry_data, 0x60)[0]
            if name_len > 0 and name_len < 520 and name_buf:
                name_raw = rt_io._r5_read(self._attached_pid, name_buf, min(name_len, 520))
                if name_raw:
                    try:
                        mod_name = name_raw.decode("utf-16-le").rstrip("\x00").lower()
                        if mod_name == target or mod_name.endswith("\\" + target):
                            self._module_base = dll_base
                            self._module_size = size_of_image
                            return True
                    except Exception:
                        pass
            next_data = rt_io._r5_read(self._attached_pid, flink, 8)
            if not next_data:
                break
            flink = struct.unpack("<Q", next_data)[0]
        return False

    # ── reads ───────────────────────────────────────────────────────────
    def read(self, addr: int, size: int) -> Optional[bytes]:
        if rt_io is None or self._attached_pid == 0 or size <= 0 or addr <= 0:
            return None
        try:
            return rt_io.read(int(addr), int(size))
        except Exception:
            return None

    def read_batch(self, requests) -> list:
        if rt_io is None or self._attached_pid == 0 or not requests:
            return [None] * len(requests) if requests else []
        try:
            return rt_io.read_batch(list(requests))
        except Exception:
            return [None] * len(requests)

    # ── snapshot ────────────────────────────────────────────────────────
    def snapshot(self) -> dict:
        """Return full game state snapshot for all features.

        Returns: {matrix, entities, local_player, error}
        Each entity has: index, pawn, health, team, origin, bones, head_pos,
                         spotted_mask, spotted, is_visible
        local_player has: pawn, eye_pos, view_angles, aim_punch, shots_fired,
                          crosshair_entity, team, velocity, is_scoped, fov_scale
        """
        result: dict = {"matrix": None, "entities": [], "local_player": None, "error": ""}
        if not self.attached:
            result["error"] = "not attached"
            return result
        if self._module_base == 0:
            result["error"] = f"module {self._config.module!r} base unresolved"
            return result
        cfg = self._config

        if self._use_pattern_scan and not self._scan_attempted:
            self.resolve_offsets_by_scan()

        dw_entity = self._rva("dwEntityList")
        dw_view = self._rva("dwViewMatrix")
        dw_local = self._rva("dwLocalPlayerPawn")
        if not dw_entity or not dw_view:
            result["error"] = "missing dwEntityList or dwViewMatrix"
            return result

        # View matrix
        matrix_bytes = self.read(self._module_base + dw_view, 64)
        matrix = None
        if matrix_bytes and len(matrix_bytes) == 64:
            try:
                matrix = list(struct.unpack("<16f", matrix_bytes))
            except Exception:
                pass
        if matrix is None:
            result["error"] = "view matrix read failed"
            return result
        result["matrix"] = matrix

        # Local player
        if dw_local:
            local = read_local_player(
                self.read, self._module_base, dw_local,
                cfg.all_entity_offsets()
            )
            result["local_player"] = local

        # Entity list traversal (with bug fixes)
        max_entities = int(cfg.constants.get("max_entities", 64))
        chunk_stride = int(cfg.constants.get("entity_chunk_stride", 8))
        chunk_size = int(cfg.constants.get("entity_chunk_size", 512))
        identity_stride = int(cfg.constants.get("entity_identity_stride", 120))
        pentity_off = int(cfg.constants.get("pentity_offset", 0))
        entity_list_off = int(cfg.constants.get("entity_list_offset", 16))
        list_base = self._module_base + dw_entity

        h_off = cfg.entity_fields.get("m_iHealth") or 0
        t_off = cfg.entity_fields.get("m_iTeamNum") or 0
        life_off = cfg.entity_fields.get("m_lifeState") or 0
        scene_off = cfg.entity_fields.get("m_pGameSceneNode") or 0
        origin_off = cfg.scene_node_fields.get("m_vecOrigin") or 0

        spotted_state_off = cfg.entity_fields.get("m_entitySpottedState") or 0
        spotted_rel = cfg.spotted_fields.get("m_bSpotted", 8)
        mask_rel = cfg.spotted_fields.get("m_bSpottedByMask", 12)

        model_state_off = int(cfg.constants.get("model_state_offset", 368))
        bone_array_off = int(cfg.constants.get("bone_array_offset", 128))
        bone_count = int(cfg.constants.get("bone_count", 28))

        if not h_off or not t_off or not scene_off or not origin_off:
            result["error"] = "missing entity/scene field offsets"
            return result

        # Batch 1: chunk pointers (fixed: add entity_list_offset)
        chunk_indices = sorted({i >> 9 for i in range(max_entities)})
        chunk_reqs = [(list_base + entity_list_off + chunk_stride * ci, 8)
                      for ci in chunk_indices]
        chunk_bytes = self.read_batch(chunk_reqs)
        chunk_ptrs = {}
        for ci, raw in zip(chunk_indices, chunk_bytes):
            if not raw or len(raw) < 8:
                continue
            ptr = struct.unpack_from("<Q", raw)[0]
            if ptr:
                chunk_ptrs[ci] = ptr

        # Batch 2: entity pointers from identity entries
        ident_reqs = []
        ident_index = []
        for i in range(max_entities):
            ci = i >> 9
            chunk_ptr = chunk_ptrs.get(ci)
            if not chunk_ptr:
                continue
            ident_addr = chunk_ptr + identity_stride * (i & (chunk_size - 1))
            ident_reqs.append((ident_addr + pentity_off, 8))
            ident_index.append((i, ident_addr))
        ident_bytes = self.read_batch(ident_reqs) if ident_reqs else []
        pawn_ptrs = []
        for (i, _addr), raw in zip(ident_index, ident_bytes):
            pawn = 0
            if raw and len(raw) >= 8:
                pawn = struct.unpack_from("<Q", raw)[0]
            if pawn:
                pawn_ptrs.append((i, pawn))

        # Batch 3: per pawn — health + team + lifeState + scene_node
        pawn_reqs = []
        active = []
        for i, pawn in pawn_ptrs:
            field_offs = [h_off, t_off, scene_off]
            if life_off:
                field_offs.append(life_off)
            block_start = pawn + min(field_offs)
            block_end = pawn + max(scene_off + 8, t_off + 1, h_off + 4,
                                   (life_off + 1) if life_off else 0)
            size = block_end - block_start
            if size <= 0:
                continue
            pawn_reqs.append((block_start, size))
            active.append((i, pawn, min(field_offs)))

        blocks = self.read_batch(pawn_reqs) if pawn_reqs else []
        entities = []
        local_team = 0
        local_index = -1
        if result["local_player"]:
            local_team = result["local_player"].get("team", 0)

        for (i, pawn, base_off), block in zip(active, blocks):
            if not block:
                continue
            try:
                health = struct.unpack_from("<i", block, h_off - base_off)[0]
                team = struct.unpack_from("<B", block, t_off - base_off)[0]
                scene_node = struct.unpack_from("<Q", block, scene_off - base_off)[0]
                life = 0
                if life_off:
                    life = struct.unpack_from("<b", block, life_off - base_off)[0]
            except Exception:
                continue
            if health <= 0 or life != 0 or scene_node == 0:
                continue

            # Check if this is the local player pawn
            lp = result.get("local_player")
            if lp and pawn == lp.get("pawn", 0):
                local_index = i
                continue

            origin_raw = self.read(scene_node + origin_off, 12)
            if not origin_raw or len(origin_raw) < 12:
                continue
            try:
                ox, oy, oz = struct.unpack("<3f", origin_raw)
            except Exception:
                continue

            ent: dict = {
                "index": int(i),
                "pawn": int(pawn),
                "health": int(health),
                "team": int(team),
                "origin": (float(ox), float(oy), float(oz)),
            }

            # Spotted state
            if spotted_state_off:
                sp_base = pawn + spotted_state_off
                sp_raw = self.read(sp_base, 16)
                if sp_raw and len(sp_raw) >= 16:
                    try:
                        ent["spotted"] = bool(sp_raw[spotted_rel])
                        ent["spotted_mask"] = struct.unpack_from("<I", sp_raw, mask_rel)[0]
                    except Exception:
                        pass

            # Bones
            bones = read_bones(self.read, pawn, scene_off,
                               model_state_off, bone_array_off, bone_count)
            if bones:
                ent["bones"] = bones
                head = head_position(bones)
                if head:
                    ent["head_pos"] = head

            entities.append(ent)

        # Annotate visibility using local player index
        if local_index >= 0:
            from .cs2_visibility import annotate_visibility
            entities = annotate_visibility(entities, local_index)

        result["entities"] = entities
        result["local_index"] = local_index
        return result

