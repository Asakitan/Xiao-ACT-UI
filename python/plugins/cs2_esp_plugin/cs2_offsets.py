# -*- coding: utf-8 -*-
"""CS2 ESP plugin — offsets/config loader with fail-closed validation.

Runtime offsets live in ``offsets.json`` next to this file. They are
game-version sensitive: every CS2 Steam update can move RVAs, so the
plugin refuses to run when the config version does not match the
expected marker. Replace the placeholder ``0x0`` values with real
RVAs from a current hazedumper/n0zie dump before enabling.
"""

from __future__ import annotations

import json
import os
from typing import Any, Dict, Optional

DEFAULT_CONFIG_VERSION = "0"
_REQUIRED_OFFSETS = ("dwEntityList", "dwLocalPlayerPawn", "dwViewMatrix")
_REQUIRED_ENTITY_FIELDS = ("m_iHealth", "m_iTeamNum", "m_pGameSceneNode")
_REQUIRED_SCENE_FIELDS = ("m_vecOrigin",)
_DEFAULT_CONSTANTS = {
    "max_entities": 64,
    "entity_chunk_stride": 8,       # bytes between chunk pointers in dwEntityList
    "entity_chunk_size": 512,       # CEntityIdentity entries per chunk
    "entity_identity_stride": 120,  # sizeof(CEntityIdentity)
    "pentity_offset": 16,           # m_pEntity offset inside CEntityIdentity
}


class OffsetConfig:
    """Validated, read-only view of ``offsets.json``."""

    def __init__(self, raw: Dict[str, Any]) -> None:
        self.config_version = str(raw.get("config_version") or "")
        self.target_process = str(raw.get("target_process") or "cs2.exe")
        self.module = str(raw.get("module") or "client.dll")
        offsets = raw.get("offsets") or {}
        entity_fields = raw.get("entity_fields") or {}
        scene_node_fields = raw.get("scene_node_fields") or {}
        constants = raw.get("constants") or {}
        self.offsets = {k: self._parse_offset(v) for k, v in offsets.items()}
        self.entity_fields = {k: self._parse_offset(v) for k, v in entity_fields.items()}
        self.scene_node_fields = {k: self._parse_offset(v) for k, v in scene_node_fields.items()}
        merged = dict(_DEFAULT_CONSTANTS)
        if isinstance(constants, dict):
            for k, v in constants.items():
                try:
                    merged[str(k)] = int(v)
                except (TypeError, ValueError):
                    pass
        self.constants = merged

    @staticmethod
    def _parse_offset(value: Any) -> int:
        if isinstance(value, int):
            return int(value)
        if isinstance(value, str):
            text = value.strip()
            if not text:
                return 0
            try:
                return int(text, 16) if text.lower().startswith("0x") else int(text)
            except ValueError:
                return 0
        return 0

    def is_complete(self) -> bool:
        """True only when every required offset/field has a non-zero value."""
        for name in _REQUIRED_OFFSETS:
            if not self.offsets.get(name):
                return False
        for name in _REQUIRED_ENTITY_FIELDS:
            if not self.entity_fields.get(name):
                return False
        for name in _REQUIRED_SCENE_FIELDS:
            if not self.scene_node_fields.get(name):
                return False
        return True

    def missing(self) -> list:
        miss = []
        for name in _REQUIRED_OFFSETS:
            if not self.offsets.get(name):
                miss.append(f"offsets.{name}")
        for name in _REQUIRED_ENTITY_FIELDS:
            if not self.entity_fields.get(name):
                miss.append(f"entity_fields.{name}")
        for name in _REQUIRED_SCENE_FIELDS:
            if not self.scene_node_fields.get(name):
                miss.append(f"scene_node_fields.{name}")
        return miss


def load_config(plugin_dir: str) -> Optional[OffsetConfig]:
    """Load and validate ``offsets.json`` from ``plugin_dir``.

    Returns ``None`` (and is intentionally silent) when the file is
    missing or unreadable; callers surface the reason via ``OffsetConfig``
    diagnostics when present.
    """
    path = os.path.join(plugin_dir, "offsets.json")
    if not os.path.isfile(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as handle:
            raw = json.load(handle)
    except Exception:
        return None
    if not isinstance(raw, dict):
        return None
    return OffsetConfig(raw)


def default_config() -> OffsetConfig:
    """A placeholder config that always reports incomplete."""
    return OffsetConfig({
        "config_version": DEFAULT_CONFIG_VERSION,
        "target_process": "cs2.exe",
        "module": "client.dll",
        "offsets": {name: "0x0" for name in _REQUIRED_OFFSETS},
        "entity_fields": {name: "0x0" for name in _REQUIRED_ENTITY_FIELDS},
        "scene_node_fields": {name: "0x0" for name in _REQUIRED_SCENE_FIELDS},
        "constants": dict(_DEFAULT_CONSTANTS),
    })

