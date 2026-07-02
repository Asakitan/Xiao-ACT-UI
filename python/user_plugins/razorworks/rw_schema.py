# -*- coding: utf-8 -*-
"""RW module — offsets/config loader with fail-closed validation.

Extended to support all analysis features: tracker, RCS, reactor,
visibility check, bone reading, and local player state.
"""

from __future__ import annotations

import json
import os
from typing import Any, Dict, Optional

DEFAULT_CONFIG_VERSION = "0"

_REQUIRED_OFFSETS = ("dwEntityList", "dwViewMatrix", "dwLocalPlayerPawn")
_OPTIONAL_OFFSETS = ("dwLocalPlayerController",)

_REQUIRED_ENTITY_FIELDS = ("m_iHealth", "m_iTeamNum", "m_pGameSceneNode")
_OPTIONAL_ENTITY_FIELDS = (
    "m_lifeState", "m_bDormant",
    "m_angEyeAngles", "m_vecViewOffset", "m_aimPunchAngle",
    "m_iShotsFired", "m_iIDEntIndex", "m_bIsScoped",
    "m_vecVelocity", "m_flFOVSensitivityAdjust",
)

_REQUIRED_SCENE_FIELDS = ("m_vecOrigin",)
_OPTIONAL_SCENE_FIELDS = ("m_vecAbsOrigin",)

_DEFAULT_CONSTANTS = {
    "max_entities": 64,
    "entity_chunk_stride": 8,
    "entity_chunk_size": 512,
    "entity_identity_stride": 120,
    "pentity_offset": 16,
    "bone_array_offset": 128,      # modelState + this = bone array ptr
    "model_state_offset": 368,     # CSkeletonInstance.m_modelState (0x170)
    "head_bone_index": 6,
    "bone_count": 28,
}


class SchemaConfig:
    """Validated, read-only view of ``offsets.json``."""

    def __init__(self, raw: Dict[str, Any]) -> None:
        self.config_version = str(raw.get("config_version") or "")
        self.target_process = str(raw.get("target_process") or "cs2.exe")
        self.module = str(raw.get("module") or "client.dll")
        offsets = raw.get("offsets") or {}
        entity_fields = raw.get("entity_fields") or {}
        scene_node_fields = raw.get("scene_node_fields") or {}
        spotted_fields = raw.get("spotted_fields") or {}
        constants = raw.get("constants") or {}
        self.offsets = {k: self._parse_offset(v) for k, v in offsets.items()}
        self.entity_fields = {k: self._parse_offset(v) for k, v in entity_fields.items()}
        self.scene_node_fields = {k: self._parse_offset(v) for k, v in scene_node_fields.items()}
        self.spotted_fields = {k: self._parse_offset(v) for k, v in spotted_fields.items()}
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

    def combat_ready(self) -> bool:
        """True when tracker/RCS/reactor offsets are present."""
        needed = ("m_angEyeAngles", "m_aimPunchAngle", "m_iShotsFired",
                  "m_iIDEntIndex")
        return all(self.entity_fields.get(n) for n in needed)

    def combat_missing(self) -> list:
        needed = ("m_angEyeAngles", "m_aimPunchAngle", "m_iShotsFired",
                  "m_iIDEntIndex", "m_vecViewOffset")
        return [n for n in needed if not self.entity_fields.get(n)]

    def all_entity_offsets(self) -> dict:
        """Merged entity field offsets for the reader."""
        return dict(self.entity_fields)


def load_config(plugin_dir: str) -> Optional[SchemaConfig]:
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
    return SchemaConfig(raw)


def default_config() -> SchemaConfig:
    return SchemaConfig({
        "config_version": DEFAULT_CONFIG_VERSION,
        "target_process": "cs2.exe",
        "module": "client.dll",
        "offsets": {name: "0x0" for name in _REQUIRED_OFFSETS},
        "entity_fields": {name: "0x0" for name in _REQUIRED_ENTITY_FIELDS},
        "scene_node_fields": {name: "0x0" for name in _REQUIRED_SCENE_FIELDS},
        "constants": dict(_DEFAULT_CONSTANTS),
    })
