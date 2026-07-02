# -*- coding: utf-8 -*-
"""Runtime Source 2 SchemaSystem resolver.

Reads CSchemaSystem from the target process memory to resolve
entity field offsets automatically — survives game updates
without manual offset maintenance.

Chain: schemasystem.dll → CSchemaSystem → TypeScope("client.dll")
     → CSchemaClassBinding → SchemaClassFieldData_t → offset
"""

from __future__ import annotations

import struct
from typing import Callable, Dict, List, Optional, Tuple

ReadFn = Callable[[int, int], Optional[bytes]]

# Field map: our_key → (class_name, field_name)
# Fields can live in parent classes; resolver walks inheritance.
FIELD_MAP: Dict[str, Tuple[str, str]] = {
    "m_iHealth":             ("C_BaseEntity",          "m_iHealth"),
    "m_iTeamNum":            ("C_BaseEntity",          "m_iTeamNum"),
    "m_lifeState":           ("C_BaseEntity",          "m_lifeState"),
    "m_fFlags":              ("C_BaseEntity",          "m_fFlags"),
    "m_pGameSceneNode":      ("C_BaseEntity",          "m_pGameSceneNode"),
    "m_vecViewOffset":       ("C_BaseModelEntity",     "m_vecViewOffset"),
    "m_angEyeAngles":        ("C_CSPlayerPawnBase",    "m_angEyeAngles"),
    "m_iIDEntIndex":         ("C_CSPlayerPawnBase",    "m_iIDEntIndex"),
    "m_entitySpottedState":  ("C_CSPlayerPawn",        "m_entitySpottedState"),
    "m_ArmorValue":          ("C_CSPlayerPawn",        "m_ArmorValue"),
    "m_pClippingWeapon":     ("C_CSPlayerPawnBase",    "m_pClippingWeapon"),
    "m_vecVelocity":         ("C_BaseEntity",          "m_vecVelocity"),
    "m_bIsScoped":           ("C_CSPlayerPawn",        "m_bIsScoped"),
    "m_aimPunchAngle":       ("C_CSPlayerPawn",        "m_aimPunchAngle"),
    "m_iShotsFired":         ("C_CSPlayerPawn",        "m_iShotsFired"),
    "m_fAccuracyPenalty":    ("C_CSWeaponBase",        "m_fAccuracyPenalty"),
    "m_iItemDefinitionIndex":("C_EconItemView",        "m_iItemDefinitionIndex"),
}

SCENE_FIELD_MAP: Dict[str, Tuple[str, str]] = {
    "m_vecOrigin":    ("CGameSceneNode",   "m_vecOrigin"),
    "m_vecAbsOrigin": ("CGameSceneNode",   "m_vecAbsOrigin"),
    "m_bDormant":     ("CGameSceneNode",   "m_bDormant"),
}

# CSchemaSystem internal structure offsets (Source 2 engine, stable across builds)
# These are engine-level ABI, change far less often than game field offsets.
# Multiple candidates for robustness — resolver tries each and validates.
_SS_TYPE_SCOPES_CANDIDATES = [0x190, 0x188, 0x198, 0x1A0]
_SCOPE_NAME_OFF = 0x08
_SCOPE_CLASSES_CANDIDATES = [0x5B8, 0x588, 0x5E8, 0x558, 0x450, 0x638, 0x640]

# CSchemaClassBinding (SchemaClassInfoData_t)
_CB_NAME = 0x08
_CB_FIELDS = 0x28
_CB_FIELD_COUNT = 0x1C

# SchemaClassFieldData_t
_FD_NAME = 0x00
_FD_OFFSET = 0x10
_FD_STRIDE = 0x20


def _r8(read: ReadFn, addr: int) -> int:
    d = read(addr, 8)
    return struct.unpack("<Q", d)[0] if d and len(d) >= 8 else 0


def _r4(read: ReadFn, addr: int) -> int:
    d = read(addr, 4)
    return struct.unpack("<i", d)[0] if d and len(d) >= 4 else 0


def _r2(read: ReadFn, addr: int) -> int:
    d = read(addr, 2)
    return struct.unpack("<h", d)[0] if d and len(d) >= 2 else 0


def _rstr(read: ReadFn, addr: int, maxlen: int = 128) -> str:
    d = read(addr, maxlen)
    if not d:
        return ""
    try:
        return d.split(b"\x00", 1)[0].decode("utf-8", "replace")
    except Exception:
        return ""


class SchemaResolver:
    """Resolve entity field offsets from Source 2 SchemaSystem at runtime."""

    def __init__(self, read_fn: ReadFn, find_module_fn: Callable[[str], int]) -> None:
        self._read = read_fn
        self._find_module = find_module_fn
        self._schema_sys = 0
        self._client_scope = 0
        self._classes: Dict[str, int] = {}  # class_name → CSchemaClassBinding addr
        self._scope_classes_off = 0

    def resolve(self) -> Dict[str, Dict[str, int]]:
        """Resolve all field offsets. Returns {"entity_fields": {...}, "scene_node_fields": {...}}."""
        if not self._find_schema_system():
            return {}
        if not self._find_client_scope():
            return {}
        if not self._enumerate_classes():
            return {}

        entity = {}
        for our_key, (cls, field) in FIELD_MAP.items():
            off = self._get_field_offset(cls, field)
            if off is not None and off > 0:
                entity[our_key] = off

        scene = {}
        for our_key, (cls, field) in SCENE_FIELD_MAP.items():
            off = self._get_field_offset(cls, field)
            if off is not None and off > 0:
                scene[our_key] = off

        return {"entity_fields": entity, "scene_node_fields": scene}

    def _find_schema_system(self) -> bool:
        """Find CSchemaSystem singleton via schemasystem.dll pattern scan."""
        base = self._find_module("schemasystem.dll")
        if not base:
            return False

        # Strategy: scan schemasystem.dll for CreateInterface → InterfaceReg list
        # → "SchemaSystem_001" entry → create function → singleton RIP reference
        #
        # Alternatively, scan for known instruction patterns that load the global.
        patterns = [
            b"\x48\x8D\x0D",  # lea rcx, [rip+disp]
            b"\x48\x8B\x05",  # mov rax, [rip+disp]
        ]

        # Read .text section (first 0x200000 bytes should cover it)
        code = self._read(base, min(0x200000, 0x200000))
        if not code:
            return False

        # Find "SchemaSystem_001" string first
        target = b"SchemaSystem_001\x00"
        str_idx = code.find(target)
        if str_idx < 0:
            return False
        str_va = base + str_idx

        # Find InterfaceReg referencing this string.
        # InterfaceReg layout: +0x00 create_fn, +0x08 name_ptr, +0x10 next
        # Search for the string VA in .rdata
        str_va_bytes = struct.pack("<Q", str_va)
        ref_idx = code.find(str_va_bytes)
        if ref_idx < 0:
            return False

        # InterfaceReg.m_CreateFn is at ref_idx - 8
        create_fn = struct.unpack_from("<Q", code, ref_idx - 8)[0]
        if not create_fn or create_fn < base:
            return False

        # Read the create function (small, ~16 bytes)
        fn_code = self._read(create_fn, 32)
        if not fn_code:
            return False

        # Look for lea rax, [rip+disp] or mov rax, [rip+disp]
        for pat in (b"\x48\x8D\x05", b"\x48\x8B\x05", b"\x48\x8D\x0D"):
            idx = fn_code.find(pat)
            if idx >= 0 and idx + 7 <= len(fn_code):
                disp = struct.unpack_from("<i", fn_code, idx + 3)[0]
                ptr = create_fn + idx + 7 + disp
                # Validate: should be a valid kernel-like pointer
                if ptr > base and ptr < base + 0x10000000:
                    self._schema_sys = ptr
                    return True

        return False

    def _find_client_scope(self) -> bool:
        """Find the 'client.dll' CSchemaSystemTypeScope."""
        ss = self._schema_sys
        if not ss:
            return False

        for ts_off in _SS_TYPE_SCOPES_CANDIDATES:
            vec_data = _r8(self._read, ss + ts_off)
            vec_count = _r4(self._read, ss + ts_off + 8)
            if not vec_data or vec_count <= 0 or vec_count > 64:
                continue

            for i in range(vec_count):
                scope_ptr = _r8(self._read, vec_data + i * 8)
                if not scope_ptr:
                    continue
                name = _rstr(self._read, scope_ptr + _SCOPE_NAME_OFF, 64)
                if name == "client.dll":
                    self._client_scope = scope_ptr
                    return True

        return False

    def _enumerate_classes(self) -> bool:
        """Walk the class hash table in the client scope to build class index."""
        scope = self._client_scope
        if not scope:
            return False

        # Try each candidate offset for the class bindings hash table
        for cls_off in _SCOPE_CLASSES_CANDIDATES:
            self._scope_classes_off = cls_off
            self._classes.clear()

            hash_base = scope + cls_off
            n = self._walk_hash_table(hash_base)
            if n > 50:  # client.dll should have hundreds of classes
                return True

        return False

    def _walk_hash_table(self, hash_base: int) -> int:
        """Walk CUtlTSHash memory pool and extract CSchemaClassBinding entries."""
        # CUtlTSHash layout:
        # +0x00: m_nBucketCount (int)
        # +0x10: CUtlMemoryPool (the allocation pool)
        #   CUtlMemoryPool:
        #     +0x00: m_BlockSize (int)
        #     +0x04: m_BlocksPerBlob (int)
        #     +0x10+0x18: blob list data ptr (CUtlVector<byte*>)
        #     +0x10+0x20: blob list count

        pool_base = hash_base + 0x10
        block_size = _r4(self._read, pool_base)
        blocks_per_blob = _r4(self._read, pool_base + 4)

        if block_size <= 0 or block_size > 512 or blocks_per_blob <= 0 or blocks_per_blob > 4096:
            return 0

        # Blob list is CUtlVector at pool_base + 0x18
        blob_data_ptr = _r8(self._read, pool_base + 0x18)
        blob_count = _r4(self._read, pool_base + 0x20)

        if not blob_data_ptr or blob_count <= 0 or blob_count > 256:
            return 0

        count = 0
        for bi in range(blob_count):
            blob_ptr = _r8(self._read, blob_data_ptr + bi * 8)
            if not blob_ptr:
                continue
            for ei in range(blocks_per_blob):
                entry_addr = blob_ptr + ei * block_size
                # HashFixedData_t: data at +0x10
                binding_ptr = entry_addr + 0x10
                # CSchemaClassBinding.m_pszName at +0x08
                name_ptr = _r8(self._read, binding_ptr + _CB_NAME)
                if not name_ptr:
                    continue
                name = _rstr(self._read, name_ptr, 128)
                if name and name.startswith("C") and len(name) > 2:
                    self._classes[name] = binding_ptr
                    count += 1

        return count

    def _get_field_offset(self, class_name: str, field_name: str) -> Optional[int]:
        """Get a field's offset from a resolved class binding."""
        binding = self._classes.get(class_name)
        if not binding:
            return self._search_all_classes(field_name)
        return self._read_field_inherited(binding, field_name)

    def _read_field_inherited(self, binding: int, field_name: str, depth: int = 0) -> Optional[int]:
        """Walk the inheritance chain to find a field offset."""
        if depth > 8:
            return None
        result = self._read_field_from_binding(binding, field_name)
        if result is not None:
            return result
        # Try parent class (base class pointer at ~+0x38)
        base_ptr = _r8(self._read, binding + 0x38)
        if base_ptr and base_ptr > 0x10000:
            parent_binding = _r8(self._read, base_ptr)
            if parent_binding and parent_binding > 0x10000:
                return self._read_field_inherited(parent_binding, field_name, depth + 1)
        return None

    def _read_field_from_binding(self, binding: int, field_name: str) -> Optional[int]:
        """Read a specific field offset from a CSchemaClassBinding."""
        field_count = _r2(self._read, binding + _CB_FIELD_COUNT)
        fields_ptr = _r8(self._read, binding + _CB_FIELDS)

        if field_count <= 0 or field_count > 512 or not fields_ptr:
            return None

        if not fields_ptr or fields_ptr < 0x10000 or fields_ptr > 0x7FFFFFFFFFFF:
            return None

        # Validate the first field name is readable ASCII
        if field_count > 0:
            test_ptr = _r8(self._read, fields_ptr + _FD_NAME)
            if test_ptr:
                test_name = _rstr(self._read, test_ptr, 32)
                if not test_name or not all(c.isascii() for c in test_name):
                    return None

        for i in range(field_count):
            fd_addr = fields_ptr + i * _FD_STRIDE
            fn_ptr = _r8(self._read, fd_addr + _FD_NAME)
            if not fn_ptr:
                continue
            fn = _rstr(self._read, fn_ptr, 64)
            if fn == field_name:
                return _r4(self._read, fd_addr + _FD_OFFSET)

        return None

    def _search_all_classes(self, field_name: str) -> Optional[int]:
        """Search all resolved classes for a field (handles inheritance flattening)."""
        for binding in self._classes.values():
            result = self._read_field_from_binding(binding, field_name)
            if result is not None and result > 0:
                return result
        return None


def resolve_offsets(read_fn: ReadFn, find_module_fn: Callable[[str], int]) -> Dict[str, Dict[str, int]]:
    """One-shot resolve: returns {"entity_fields": {...}, "scene_node_fields": {...}} or {}."""
    try:
        r = SchemaResolver(read_fn, find_module_fn)
        return r.resolve()
    except Exception:
        return {}
