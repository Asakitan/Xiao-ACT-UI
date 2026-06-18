# -*- coding: utf-8 -*-
"""CS2 ESP plugin — Engine A reader facade.

Wraps :mod:`mem_probe.rt_io` so the plugin never touches ``rt_io``
internals directly. Engine A (``ENGINE_READONLY``) is fixed: it is the
only engine available on every tier, carries only ``CAP_READ``, and
matches this plugin's read-only contract. The facade owns engine load,
capability check, attach/detach, module base resolution, and batch
reads; it exposes a single :meth:`snapshot` that returns a JSON-safe
entity list + view matrix for the projection layer.
"""

from __future__ import annotations

import ctypes
import struct
from typing import Any, Optional

try:
    from mem_probe import rt_io
except Exception:  # pragma: no cover - import-time guard for headless tests
    rt_io = None  # type: ignore[assignment]

from .cs2_offsets import OffsetConfig
from .cs2_patterns import Pattern, default_patterns, scan_module


class ReaderError(Exception):
    """Raised when the reader cannot satisfy a request."""


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
        # RVAs resolved at attach time by scanning client.dll. When present
        # these override the (stale-prone) values in offsets.json.
        self._scanned_rvas: dict = {}
        self._scan_attempted = False

    # ── lifecycle ───────────────────────────────────────────────────────
    def ensure_engine(self) -> bool:
        """Select and load Engine A. Returns False on any failure."""
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
                self._last_error = "Engine A load failed (driver missing or probe failed)"
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
        """Return the PID of ``config.target_process`` or 0 if not found."""
        name = (self._config.target_process or "").lower()
        if not name:
            return 0
        try:
            snap = _create_toolhelp_snapshot()
            if not snap:
                return 0
            entry = _process_entry()
            got = ctypes.windll.kernel32.Process32FirstW(snap, ctypes.byref(entry))
            while got:
                exe = entry.szExeFile.decode("utf-16-le", "ignore").split("\x00", 1)[0]
                if exe.lower() == name:
                    return int(entry.th32ProcessID)
                got = ctypes.windll.kernel32.Process32NextW(snap, ctypes.byref(entry))
        except Exception:
            pass
        finally:
            if snap:
                ctypes.windll.kernel32.CloseHandle(snap)
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
        if not self._resolve_module_base():
            # Attach succeeded but module base unknown — still keep attach
            # so caller can decide; projection will fail-closed on its own.
            pass
        return True

    def detach(self) -> None:
        if rt_io is None or self._attached_pid == 0:
            self._attached_pid = 0
            self._module_base = 0
            self._module_size = 0
            return
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

    @property
    def scanned_rvas(self) -> dict:
        """RVAs resolved by pattern scanning (empty until scanned)."""
        return dict(self._scanned_rvas)

    # ── pattern scanning ────────────────────────────────────────────────
    def resolve_offsets_by_scan(self) -> dict:
        """Scan client.dll once to resolve dwEntityList / dwViewMatrix RVAs.

        Runs at most once per attach. Returns the resolved RVA map (also
        stored in ``self._scanned_rvas``). On failure returns whatever was
        resolved (possibly empty) — callers fall back to offsets.json.
        """
        if self._scan_attempted:
            return dict(self._scanned_rvas)
        self._scan_attempted = True
        if not self._use_pattern_scan:
            return {}
        if self._module_base == 0 or self._module_size == 0:
            self._last_error = "pattern scan skipped: module base/size unknown"
            return {}
        # Only scan the RVAs the plugin actually consumes. dwLocalPlayerPawn
        # is derived two-stage in cs2-dumper and the plugin does not need it
        # (local team comes from settings), so it is intentionally excluded.
        wanted = ("dwEntityList", "dwViewMatrix")
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
        if not resolved:
            self._last_error = "pattern scan matched nothing (client.dll updated?)"
        elif len(resolved) < len(wanted):
            missing = [n for n in wanted if n not in resolved]
            self._last_error = f"pattern scan partial: missing {missing}"
        return dict(resolved)

    def _rva(self, name: str) -> int:
        """Prefer a scanned RVA, fall back to offsets.json.

        Scanned RVAs are only consulted when pattern scanning is enabled,
        so disabling ``use_pattern_scan`` fully reverts to offsets.json.
        """
        if self._use_pattern_scan:
            scanned = self._scanned_rvas.get(name)
            if scanned:
                return int(scanned)
        return int(self._config.offsets.get(name) or 0)

    # ── module base ─────────────────────────────────────────────────────
    def _resolve_module_base(self) -> bool:
        self._module_base = 0
        self._module_size = 0
        if self._attached_pid == 0:
            return False
        target = (self._config.module or "").lower()
        if not target:
            return False
        try:
            snap = _create_module_snapshot(self._attached_pid)
            if not snap:
                return False
            entry = _module_entry()
            got = ctypes.windll.kernel32.Module32FirstW(snap, ctypes.byref(entry))
            while got:
                mod = entry.szModule.decode("utf-16-le", "ignore").split("\x00", 1)[0]
                if mod.lower() == target:
                    self._module_base = int(entry.modBaseAddr)
                    self._module_size = int(entry.modBaseSize)
                    return True
                got = ctypes.windll.kernel32.Module32NextW(snap, ctypes.byref(entry))
        except Exception:
            pass
        finally:
            if snap:
                ctypes.windll.kernel32.CloseHandle(snap)
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
        """Return ``{matrix, entities, error}`` for the projection layer.

        CS2 entity list traversal (Source 2 ``CEntityIdentity`` chunks):
          list_base = client.dll + dwEntityList
          chunk_ptr = read_qword(list_base + chunk_stride * (index >> 9))
          identity  = chunk_ptr + identity_stride * (index & 0x1FF)
          pawn      = read_qword(identity + pentity_offset)   # m_pEntity
        Position is indirect: pawn → m_pGameSceneNode → m_vecOrigin.

        Uses batched reads where possible. Any read failure is fail-closed:
        ``error`` is set and ``entities`` is empty.
        """
        result: dict = {"matrix": None, "entities": [], "error": ""}
        if not self.attached:
            result["error"] = "not attached"
            return result
        if self._module_base == 0:
            result["error"] = f"module {self._config.module!r} base unresolved"
            return result
        cfg = self._config
        # Resolve RVAs by pattern scan on first snapshot; fall back to
        # offsets.json values for anything the scan could not resolve.
        if self._use_pattern_scan and not self._scan_attempted:
            self.resolve_offsets_by_scan()
        dw_entity = self._rva("dwEntityList")
        dw_view = self._rva("dwViewMatrix")
        if not dw_entity or not dw_view:
            miss = [n for n in ("dwEntityList", "dwViewMatrix") if not self._rva(n)]
            result["error"] = f"missing offsets ({miss}); scan={self._scanned_rvas or 'none'}"
            return result

        # View matrix: 16 floats = 64 bytes.
        matrix_bytes = self.read(self._module_base + dw_view, 64)
        matrix = None
        if matrix_bytes and len(matrix_bytes) == 64:
            try:
                matrix = list(struct.unpack("<16f", matrix_bytes))
            except Exception:
                matrix = None
        if matrix is None:
            result["error"] = "view matrix read failed"
            return result
        result["matrix"] = matrix

        max_entities = int(cfg.constants.get("max_entities", 64) or 64)
        chunk_stride = int(cfg.constants.get("entity_chunk_stride", 8) or 8)
        chunk_size = int(cfg.constants.get("entity_chunk_size", 512) or 512)
        identity_stride = int(cfg.constants.get("entity_identity_stride", 120) or 120)
        pentity_off = int(cfg.constants.get("pentity_offset", 16) or 16)
        list_base = self._module_base + dw_entity

        h_off = cfg.entity_fields.get("m_iHealth") or 0
        t_off = cfg.entity_fields.get("m_iTeamNum") or 0
        life_off = cfg.entity_fields.get("m_lifeState") or 0
        scene_off = cfg.entity_fields.get("m_pGameSceneNode") or 0
        origin_off = cfg.scene_node_fields.get("m_vecOrigin") or 0
        if not h_off or not t_off or not scene_off or not origin_off:
            result["error"] = "missing entity/scene field offsets"
            return result

        # Batch 1: read chunk pointers (one per chunk the index range spans).
        # index >> 9 gives the chunk index; max_entities bounds the scan.
        chunk_indices = sorted({i >> 9 for i in range(max_entities)})
        chunk_reqs = [(list_base + chunk_stride * ci, 8) for ci in chunk_indices]
        chunk_bytes = self.read_batch(chunk_reqs)
        chunk_ptrs = {}
        for ci, raw in zip(chunk_indices, chunk_bytes):
            if not raw or len(raw) < 8:
                continue
            ptr = struct.unpack_from("<Q", raw, 0)[0] or 0
            if ptr:
                chunk_ptrs[ci] = ptr

        # Batch 2: read pawn pointers (m_pEntity) per entity identity.
        ident_reqs = []
        ident_index = []  # (entity_index, identity_addr)
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
                pawn = struct.unpack_from("<Q", raw, 0)[0] or 0
            if pawn:
                pawn_ptrs.append((i, pawn))

        # Batch 3: per pawn — health(int) + team(int) + lifeState(byte) +
        # scene_node pointer(qword). Block-read the contiguous span from
        # min field offset to end of scene_node pointer.
        pawn_reqs = []
        active = []  # (entity_index, pawn)
        for i, pawn in pawn_ptrs:
            field_offs = [h_off, t_off, scene_off]
            if life_off:
                field_offs.append(life_off)
            block_start = pawn + min(field_offs)
            block_end = pawn + max(scene_off + 8, t_off + 4, h_off + 4,
                                   (life_off + 1) if life_off else 0)
            size = block_end - block_start
            if size <= 0:
                continue
            pawn_reqs.append((block_start, size))
            active.append((i, pawn, min(field_offs)))

        blocks = self.read_batch(pawn_reqs) if pawn_reqs else []
        # Batch 4: read m_vecOrigin (3 floats) from each scene_node.
        entities = []
        for (i, pawn, base_off), block in zip(active, blocks):
            if not block:
                continue
            try:
                health = struct.unpack_from("<i", block, h_off - base_off)[0]
                team = struct.unpack_from("<i", block, t_off - base_off)[0]
                scene_node = struct.unpack_from("<Q", block, scene_off - base_off)[0] or 0
                life = 0
                if life_off:
                    life = struct.unpack_from("<b", block, life_off - base_off)[0]
            except Exception:
                continue
            if health <= 0 or life != 0 or scene_node == 0:
                continue
            origin_raw = self.read(scene_node + origin_off, 12)
            if not origin_raw or len(origin_raw) < 12:
                continue
            try:
                ox, oy, oz = struct.unpack("<3f", origin_raw)
            except Exception:
                continue
            entities.append({
                "index": int(i),
                "pawn": int(pawn),
                "health": int(health),
                "team": int(team),
                "origin": (float(ox), float(oy), float(oz)),
            })
        result["entities"] = entities
        return result


# ── Windows toolhelp helpers (kept local; Engine A has no module API) ─────

_TH32CS_SNAPPROCESS = 0x00000002
_TH32CS_SNAPMODULE = 0x00000008
_TH32CS_SNAPMODULE32 = 0x00000010


class _ProcessEntry32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", ctypes.c_uint32),
        ("cntUsage", ctypes.c_uint32),
        ("th32ProcessID", ctypes.c_uint32),
        ("th32DefaultHeapID", ctypes.c_void_p),
        ("th32ModuleID", ctypes.c_uint32),
        ("cntThreads", ctypes.c_uint32),
        ("th32ParentProcessID", ctypes.c_uint32),
        ("pcPriClassBase", ctypes.c_int32),
        ("dwFlags", ctypes.c_uint32),
        ("szExeFile", ctypes.c_wchar * 260),
    ]


class _ModuleEntry32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", ctypes.c_uint32),
        ("th32ModuleID", ctypes.c_uint32),
        ("th32ProcessID", ctypes.c_uint32),
        ("GlblcntUsage", ctypes.c_uint32),
        ("ProccntUsage", ctypes.c_uint32),
        ("modBaseAddr", ctypes.c_void_p),
        ("modBaseSize", ctypes.c_uint32),
        ("hModule", ctypes.c_void_p),
        ("szModule", ctypes.c_wchar * 256),
        ("szExePath", ctypes.c_wchar * 260),
    ]


def _process_entry() -> _ProcessEntry32W:
    entry = _ProcessEntry32W()
    entry.dwSize = ctypes.sizeof(_ProcessEntry32W)
    return entry


def _module_entry() -> _ModuleEntry32W:
    entry = _ModuleEntry32W()
    entry.dwSize = ctypes.sizeof(_ModuleEntry32W)
    return entry


def _create_toolhelp_snapshot() -> int:
    try:
        return ctypes.windll.kernel32.CreateToolhelp32Snapshot(_TH32CS_SNAPPROCESS, 0) or 0
    except Exception:
        return 0


def _create_module_snapshot(pid: int) -> int:
    try:
        flags = _TH32CS_SNAPMODULE | _TH32CS_SNAPMODULE32
        return ctypes.windll.kernel32.CreateToolhelp32Snapshot(flags, int(pid)) or 0
    except Exception:
        return 0

