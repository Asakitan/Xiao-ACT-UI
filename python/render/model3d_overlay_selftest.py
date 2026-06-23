# -*- coding: utf-8 -*-
"""Focused selftests for plugin unioverlay canvas/model3d support."""
from __future__ import annotations

import contextlib
import json
import math
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType, SimpleNamespace
from unittest import mock


_PY_ROOT = Path(__file__).resolve().parents[1]
if str(_PY_ROOT) not in sys.path:
    sys.path.insert(0, str(_PY_ROOT))

from act_platform.ui_spec import MAX_LAYER_POS, MAX_LAYER_Z, MAX_TEXT_LEN, UI, normalize_ui_spec
from gui_modules import sao_plugin_unified_overlay as overlay_mod
from render import model3d_backend
from render import overlay_adapter as overlay_adapter_mod
from render.model3d_backend import (
    clear_model3d_metadata_caches,
    diagnose_model3d_node,
    evaluate_retarget_pose,
    get_action_metadata,
    get_backend_status,
    get_model_metadata,
    get_model_metadata_view,
    get_retarget_plan,
    probe_model3d_backend,
)
from render.model3d_native import (
    bootstrap_builtin_native_model3d_renderers,
    build_native_model3d_context,
    clear_native_model3d_renderers,
    native_model3d_renderer_status,
    register_native_model3d_renderer,
    reset_native_model3d_resources,
)
from render.model3d_overlay import render_model3d_node


def _pad4(data: bytes, pad: bytes = b" ") -> bytes:
    extra = (-len(data)) % 4
    return data + pad * extra


def _write_cube_glb(path: Path) -> None:
    vertices = [
        (-1.0, -1.0, -1.0), (1.0, -1.0, -1.0),
        (1.0, 1.0, -1.0), (-1.0, 1.0, -1.0),
        (-1.0, -1.0, 1.0), (1.0, -1.0, 1.0),
        (1.0, 1.0, 1.0), (-1.0, 1.0, 1.0),
    ]
    indices = [
        0, 1, 2, 0, 2, 3,
        4, 6, 5, 4, 7, 6,
        0, 4, 5, 0, 5, 1,
        2, 6, 7, 2, 7, 3,
        1, 5, 6, 1, 6, 2,
        0, 3, 7, 0, 7, 4,
    ]
    position_blob = b"".join(struct.pack("<fff", *point) for point in vertices)
    index_offset = len(position_blob)
    index_blob = b"".join(struct.pack("<H", item) for item in indices)
    bin_blob = _pad4(position_blob + index_blob, b"\0")
    gltf = {
        "asset": {"version": "2.0"},
        "buffers": [{"byteLength": len(bin_blob)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(position_blob), "target": 34962},
            {"buffer": 0, "byteOffset": index_offset, "byteLength": len(index_blob), "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(vertices),
             "type": "VEC3", "min": [-1, -1, -1], "max": [1, 1, 1]},
            {"bufferView": 1, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
        ],
        "meshes": [{"name": "CubeMesh", "primitives": [
            {"attributes": {"POSITION": 0}, "indices": 1, "mode": 4, "material": 0}
        ]}],
        "materials": [{"name": "PreviewMat"}],
        "nodes": [{"name": "CubeNode", "mesh": 0}],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
    }
    json_blob = _pad4(json.dumps(gltf, separators=(",", ":")).encode("utf-8"), b" ")
    total = 12 + 8 + len(json_blob) + 8 + len(bin_blob)
    path.write_bytes(
        b"glTF" + struct.pack("<II", 2, total)
        + struct.pack("<II", len(json_blob), 0x4E4F534A) + json_blob
        + struct.pack("<II", len(bin_blob), 0x004E4942) + bin_blob
    )


def _write_animated_hand_glb(path: Path) -> None:
    vertices = [(-0.2, 0.0, 0.0), (0.2, 0.0, 0.0), (0.0, 0.4, 0.0)]
    indices = [0, 1, 2]
    chunks: list[bytes] = []

    def append(blob: bytes) -> tuple[int, int]:
        offset = sum(len(item) for item in chunks)
        padding = (-offset) % 4
        if padding:
            chunks.append(b"\0" * padding)
            offset += padding
        chunks.append(blob)
        return offset, len(blob)

    pos_offset, pos_len = append(b"".join(struct.pack("<fff", *point) for point in vertices))
    idx_offset, idx_len = append(b"".join(struct.pack("<H", item) for item in indices))
    time_offset, time_len = append(b"".join(struct.pack("<f", item) for item in (0.0, 1.0)))
    trans_offset, trans_len = append(
        b"".join(struct.pack("<fff", *point) for point in ((0.0, 0.0, 0.0), (0.0, 0.22, 0.0)))
    )
    bin_blob = _pad4(b"".join(chunks), b"\0")
    gltf = {
        "asset": {"version": "2.0"},
        "buffers": [{"byteLength": len(bin_blob)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": pos_offset, "byteLength": pos_len, "target": 34962},
            {"buffer": 0, "byteOffset": idx_offset, "byteLength": idx_len, "target": 34963},
            {"buffer": 0, "byteOffset": time_offset, "byteLength": time_len},
            {"buffer": 0, "byteOffset": trans_offset, "byteLength": trans_len},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(vertices),
             "type": "VEC3", "min": [-0.2, 0.0, 0.0], "max": [0.2, 0.4, 0.0]},
            {"bufferView": 1, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
            {"bufferView": 2, "componentType": 5126, "count": 2, "type": "SCALAR"},
            {"bufferView": 3, "componentType": 5126, "count": 2, "type": "VEC3"},
        ],
        "meshes": [{"name": "HandMarker", "primitives": [
            {"attributes": {"POSITION": 0}, "indices": 1, "mode": 4}
        ]}],
        "nodes": [
            {"name": "mixamorig:Hips", "children": [1], "translation": [0.0, 0.0, 0.0]},
            {"name": "mixamorig:Spine", "children": [2], "translation": [0.0, 0.48, 0.0]},
            {"name": "mixamorig:RightArm", "children": [3], "translation": [0.32, 0.32, 0.0]},
            {"name": "mixamorig:RightForeArm", "children": [4], "translation": [0.22, -0.20, 0.0]},
            {"name": "mixamorig:RightHand", "mesh": 0, "translation": [0.14, -0.14, 0.0]},
        ],
        "animations": [{"name": "Wave", "samplers": [
            {"input": 2, "output": 3, "interpolation": "LINEAR"}
        ], "channels": [
            {"sampler": 0, "target": {"node": 4, "path": "translation"}}
        ]}],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
    }
    json_blob = _pad4(json.dumps(gltf, separators=(",", ":")).encode("utf-8"), b" ")
    total = 12 + 8 + len(json_blob) + 8 + len(bin_blob)
    path.write_bytes(
        b"glTF" + struct.pack("<II", 2, total)
        + struct.pack("<II", len(json_blob), 0x4E4F534A) + json_blob
        + struct.pack("<II", len(bin_blob), 0x004E4942) + bin_blob
    )


def _write_skinned_strip_glb(path: Path, *, rotation_clip: bool = False) -> None:
    vertices = [
        (-0.24, 0.00, -0.06), (0.24, 0.00, -0.06),
        (0.24, 0.40, -0.06), (-0.24, 0.40, -0.06),
        (-0.24, 0.62, 0.06), (0.24, 0.62, 0.06),
        (0.24, 1.02, 0.06), (-0.24, 1.02, 0.06),
    ]
    indices = [
        0, 1, 2, 0, 2, 3,
        4, 6, 5, 4, 7, 6,
        0, 4, 5, 0, 5, 1,
        2, 6, 7, 2, 7, 3,
        1, 5, 6, 1, 6, 2,
        0, 3, 7, 0, 7, 4,
    ]
    joints = [
        (0, 0, 0, 0), (0, 0, 0, 0), (0, 0, 0, 0), (0, 0, 0, 0),
        (1, 0, 0, 0), (1, 0, 0, 0), (1, 0, 0, 0), (1, 0, 0, 0),
    ]
    weights = [(1.0, 0.0, 0.0, 0.0) for _ in vertices]
    chunks: list[bytes] = []

    def append(blob: bytes) -> tuple[int, int]:
        offset = sum(len(item) for item in chunks)
        padding = (-offset) % 4
        if padding:
            chunks.append(b"\0" * padding)
            offset += padding
        chunks.append(blob)
        return offset, len(blob)

    pos_offset, pos_len = append(b"".join(struct.pack("<fff", *point) for point in vertices))
    idx_offset, idx_len = append(b"".join(struct.pack("<H", item) for item in indices))
    joints_offset, joints_len = append(b"".join(struct.pack("<BBBB", *row) for row in joints))
    weights_offset, weights_len = append(b"".join(struct.pack("<ffff", *row) for row in weights))
    time_offset, time_len = append(b"".join(struct.pack("<f", item) for item in (0.0, 1.0)))
    if rotation_clip:
        value_offset, value_len = append(
            b"".join(struct.pack("<ffff", *quat) for quat in (
                (0.0, 0.0, 0.0, 1.0),
                (0.0, 0.0, 0.70710678, 0.70710678),
            ))
        )
        value_type = "VEC4"
        target_path = "rotation"
    else:
        value_offset, value_len = append(
            b"".join(struct.pack("<fff", *point) for point in ((0.0, 0.0, 0.0), (0.0, 0.55, 0.0)))
        )
        value_type = "VEC3"
        target_path = "translation"
    bin_blob = _pad4(b"".join(chunks), b"\0")
    gltf = {
        "asset": {"version": "2.0"},
        "buffers": [{"byteLength": len(bin_blob)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": pos_offset, "byteLength": pos_len, "target": 34962},
            {"buffer": 0, "byteOffset": idx_offset, "byteLength": idx_len, "target": 34963},
            {"buffer": 0, "byteOffset": joints_offset, "byteLength": joints_len, "target": 34962},
            {"buffer": 0, "byteOffset": weights_offset, "byteLength": weights_len, "target": 34962},
            {"buffer": 0, "byteOffset": time_offset, "byteLength": time_len},
            {"buffer": 0, "byteOffset": value_offset, "byteLength": value_len},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(vertices),
             "type": "VEC3", "min": [-0.24, 0.0, -0.06], "max": [0.24, 1.02, 0.06]},
            {"bufferView": 1, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
            {"bufferView": 2, "componentType": 5121, "count": len(vertices), "type": "VEC4"},
            {"bufferView": 3, "componentType": 5126, "count": len(vertices), "type": "VEC4"},
            {"bufferView": 4, "componentType": 5126, "count": 2, "type": "SCALAR"},
            {"bufferView": 5, "componentType": 5126, "count": 2, "type": value_type},
        ],
        "meshes": [{"name": "SkinnedBody", "primitives": [
            {"attributes": {"POSITION": 0, "JOINTS_0": 2, "WEIGHTS_0": 3},
             "indices": 1, "mode": 4}
        ]}],
        "skins": [{"name": "BodySkin", "skeleton": 0, "joints": [0, 1]}],
        "nodes": [
            {"name": "mixamorig:Hips", "children": [1, 2], "translation": [0.0, 0.0, 0.0]},
            {"name": "mixamorig:RightHand", "translation": [0.0, 1.0, 0.0]},
            {"name": "BodyNode", "mesh": 0, "skin": 0},
        ],
        "animations": [{"name": "Wave", "samplers": [
            {"input": 4, "output": 5, "interpolation": "LINEAR"}
        ], "channels": [
            {"sampler": 0, "target": {"node": 1, "path": target_path}}
        ]}],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
    }
    json_blob = _pad4(json.dumps(gltf, separators=(",", ":")).encode("utf-8"), b" ")
    total = 12 + 8 + len(json_blob) + 8 + len(bin_blob)
    path.write_bytes(
        b"glTF" + struct.pack("<II", 2, total)
        + struct.pack("<II", len(json_blob), 0x4E4F534A) + json_blob
        + struct.pack("<II", len(bin_blob), 0x004E4942) + bin_blob
    )


def _write_rotating_arm_glb(path: Path) -> None:
    vertices = [(-0.2, 0.0, 0.0), (0.2, 0.0, 0.0), (0.0, 0.4, 0.0)]
    indices = [0, 1, 2]
    chunks: list[bytes] = []

    def append(blob: bytes) -> tuple[int, int]:
        offset = sum(len(item) for item in chunks)
        padding = (-offset) % 4
        if padding:
            chunks.append(b"\0" * padding)
            offset += padding
        chunks.append(blob)
        return offset, len(blob)

    pos_offset, pos_len = append(b"".join(struct.pack("<fff", *point) for point in vertices))
    idx_offset, idx_len = append(b"".join(struct.pack("<H", item) for item in indices))
    time_offset, time_len = append(b"".join(struct.pack("<f", item) for item in (0.0, 1.0)))
    rot_offset, rot_len = append(
        b"".join(struct.pack("<ffff", *quat) for quat in (
            (0.0, 0.0, 0.0, 1.0),
            (0.0, 0.0, 0.70710678, 0.70710678),
        ))
    )
    bin_blob = _pad4(b"".join(chunks), b"\0")
    gltf = {
        "asset": {"version": "2.0"},
        "buffers": [{"byteLength": len(bin_blob)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": pos_offset, "byteLength": pos_len, "target": 34962},
            {"buffer": 0, "byteOffset": idx_offset, "byteLength": idx_len, "target": 34963},
            {"buffer": 0, "byteOffset": time_offset, "byteLength": time_len},
            {"buffer": 0, "byteOffset": rot_offset, "byteLength": rot_len},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(vertices),
             "type": "VEC3", "min": [-0.2, 0.0, 0.0], "max": [0.2, 0.4, 0.0]},
            {"bufferView": 1, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
            {"bufferView": 2, "componentType": 5126, "count": 2, "type": "SCALAR"},
            {"bufferView": 3, "componentType": 5126, "count": 2, "type": "VEC4"},
        ],
        "meshes": [{"name": "HandMarker", "primitives": [
            {"attributes": {"POSITION": 0}, "indices": 1, "mode": 4}
        ]}],
        "nodes": [
            {"name": "mixamorig:Hips", "children": [1], "translation": [0.0, 0.0, 0.0]},
            {"name": "mixamorig:Spine", "children": [2], "translation": [0.0, 0.48, 0.0]},
            {"name": "mixamorig:RightArm", "children": [3], "translation": [0.32, 0.32, 0.0]},
            {"name": "mixamorig:RightForeArm", "children": [4], "translation": [0.22, -0.20, 0.0]},
            {"name": "mixamorig:RightHand", "mesh": 0, "translation": [0.14, -0.14, 0.0]},
        ],
        "animations": [{"name": "Wave", "samplers": [
            {"input": 2, "output": 3, "interpolation": "LINEAR"}
        ], "channels": [
            {"sampler": 0, "target": {"node": 2, "path": "rotation"}}
        ]}],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
    }
    json_blob = _pad4(json.dumps(gltf, separators=(",", ":")).encode("utf-8"), b" ")
    total = 12 + 8 + len(json_blob) + 8 + len(bin_blob)
    path.write_bytes(
        b"glTF" + struct.pack("<II", 2, total)
        + struct.pack("<II", len(json_blob), 0x4E4F534A) + json_blob
        + struct.pack("<II", len(bin_blob), 0x004E4942) + bin_blob
    )


def _write_transformed_rest_glb(path: Path) -> None:
    vertices = [(-0.2, 0.0, 0.0), (0.2, 0.0, 0.0), (0.0, 0.4, 0.0)]
    indices = [0, 1, 2]
    position_blob = b"".join(struct.pack("<fff", *point) for point in vertices)
    index_offset = len(position_blob)
    index_blob = b"".join(struct.pack("<H", item) for item in indices)
    bin_blob = _pad4(position_blob + index_blob, b"\0")
    gltf = {
        "asset": {"version": "2.0"},
        "buffers": [{"byteLength": len(bin_blob)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(position_blob), "target": 34962},
            {"buffer": 0, "byteOffset": index_offset, "byteLength": len(index_blob), "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(vertices),
             "type": "VEC3", "min": [-0.2, 0.0, 0.0], "max": [0.2, 0.4, 0.0]},
            {"bufferView": 1, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
        ],
        "meshes": [{"name": "HandMarker", "primitives": [
            {"attributes": {"POSITION": 0}, "indices": 1, "mode": 4}
        ]}],
        "nodes": [
            {"name": "mixamorig:Hips", "children": [1], "translation": [0.0, 0.0, 0.0]},
            {
                "name": "mixamorig:Spine",
                "children": [2, 3],
                "translation": [0.0, 1.0, 0.0],
                "rotation": [0.0, 0.0, 0.70710678, 0.70710678],
                "scale": [2.0, 2.0, 2.0],
            },
            {"name": "mixamorig:RightHand", "mesh": 0, "translation": [1.0, 0.0, 0.0]},
            {
                "name": "mixamorig:LeftHand",
                "mesh": 0,
                "matrix": [
                    1.0, 0.0, 0.0, 0.0,
                    0.0, 1.0, 0.0, 0.0,
                    0.0, 0.0, 1.0, 0.0,
                    0.5, 0.0, 0.0, 1.0,
                ],
            },
        ],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
    }
    json_blob = _pad4(json.dumps(gltf, separators=(",", ":")).encode("utf-8"), b" ")
    total = 12 + 8 + len(json_blob) + 8 + len(bin_blob)
    path.write_bytes(
        b"glTF" + struct.pack("<II", 2, total)
        + struct.pack("<II", len(json_blob), 0x4E4F534A) + json_blob
        + struct.pack("<II", len(bin_blob), 0x004E4942) + bin_blob
    )


class Model3DSpecTests(unittest.TestCase):
    def test_canvas_position_and_z_are_normalized(self) -> None:
        node = normalize_ui_spec(UI.canvas(
            64, 32, [], bg="body", x=123.4, y=-9, z=7, id="meter"))["nodes"][0]

        self.assertEqual(node["type"], "canvas")
        self.assertEqual(node["id"], "meter")
        self.assertEqual(node["x"], 123)
        self.assertEqual(node["y"], -9)
        self.assertEqual(node["z"], 7)
        self.assertIs(node["draggable"], False)

        drag_node = normalize_ui_spec(UI.canvas(
            64, 32, [], bg="transparent", x=1, y=2, z=3,
            id="drag_meter", draggable=True))["nodes"][0]
        self.assertIs(drag_node["draggable"], True)

        clamped = normalize_ui_spec({
            "type": "canvas",
            "width": 16,
            "height": 16,
            "x": 999999,
            "y": -999999,
            "z": 999999,
        })["nodes"][0]
        self.assertEqual(clamped["x"], MAX_LAYER_POS)
        self.assertEqual(clamped["y"], -MAX_LAYER_POS)
        self.assertEqual(clamped["z"], MAX_LAYER_Z)

    def test_model3d_normalizes_invalid_inputs_without_loading_assets(self) -> None:
        node = normalize_ui_spec({
            "type": "model3d",
            "id": "avatar",
            "width": "bad",
            "height": -1,
            "model": {"path": "missing/avatar.fbx", "format": "fbx"},
            "action": {"speed": "fast", "json": {"ok": True}},
            "camera": {"fov": 999},
            "transform": {"scale": -4},
            "retarget": {"mode": "humanoid_auto", "preserve_proportions": True},
            "skeleton": {"bone_map": {"hips": "mixamorig:Hips"}},
            "x": 12,
            "y": 34,
            "z": -5,
        })["nodes"][0]

        self.assertEqual(node["type"], "model3d")
        self.assertEqual(node["id"], "avatar")
        self.assertEqual(node["width"], 320)
        self.assertEqual(node["height"], 1)
        self.assertEqual(node["model"]["format"], "fbx")
        self.assertEqual(node["action"]["speed"], 1.0)
        self.assertEqual(node["camera"]["fov"], 120.0)
        self.assertEqual(node["transform"]["scale"], 0.001)
        self.assertEqual(node["retarget"]["mode"], "humanoid_auto")
        self.assertIs(node["retarget"]["preserve_proportions"], True)
        self.assertEqual(node["skeleton"]["bone_map"]["hips"], "mixamorig:Hips")
        self.assertEqual((node["x"], node["y"], node["z"]), (12, 34, -5))
        self.assertIs(node["draggable"], True)
        self.assertEqual(node["phase"], 0.0)

        animated = normalize_ui_spec({
            "type": "model3d",
            "id": "animated",
            "phase": 12.5,
            "action": {"time": 9.25},
        })["nodes"][0]
        self.assertEqual(animated["phase"], 12.5)
        self.assertEqual(animated["action"]["time"], 9.25)

        fixed = normalize_ui_spec({
            "type": "model3d",
            "id": "fixed",
            "draggable": False,
            "materials": {"profile": "asaki_anime"},
            "physics": {"enabled": True},
            "procedural_action": json.dumps({
                "schema": "sao.humanoid.procedural.v1",
                "enabled": True,
                "actions": {"idle": {"body": {"breathing": 0.01}}},
            }),
            "secondary_motion": {"enabled": True, "chains": [{"name": "hair"}]},
        })["nodes"][0]
        self.assertIs(fixed["draggable"], False)
        self.assertEqual(fixed["materials"]["profile"], "asaki_anime")
        self.assertIs(fixed["physics"]["enabled"], True)
        self.assertEqual(fixed["procedural_action"]["schema"], "sao.humanoid.procedural.v1")
        self.assertEqual(fixed["secondary_motion"]["chains"][0]["name"], "hair")

        helper_with_physics = normalize_ui_spec(UI.model3d(
            "helper-physics", "avatar.fbx",
            draggable=False,
            materials={"profile": "mtoon"},
            physics={"enabled": True},
            procedural_action={"enabled": True, "actions": {"walk": {"gait": {"enabled": True}}}},
            secondary_motion={"enabled": True, "chains": [{"name": "ears"}]},
        ))["nodes"][0]
        self.assertIs(helper_with_physics["draggable"], False)
        self.assertEqual(helper_with_physics["materials"]["profile"], "mtoon")
        self.assertIn("walk", helper_with_physics["procedural_action"]["actions"])
        self.assertEqual(helper_with_physics["secondary_motion"]["chains"][0]["name"], "ears")

        long_procedural = json.dumps({
            "schema": "sao.humanoid.procedural.v1",
            "enabled": True,
            "default_action": "idle",
            "common": {
                "body": {"breathing": 0.01, "crouch": 0.02},
                "gait": {"foot_planting": True, "ground_y": "auto"},
            },
            "actions": {
                f"action_{idx}": {
                    "body": {"sway": 0.01 + idx * 0.0001},
                    "effectors": {
                        "right_hand": {
                            "offset": [0.01, 0.02, 0.03],
                            "wave": [0.01, 0.0, 0.0],
                            "frequency": 1.0,
                        },
                    },
                }
                for idx in range(80)
            },
        }, ensure_ascii=False, separators=(",", ":"))
        self.assertGreater(len(long_procedural), MAX_TEXT_LEN)
        long_node = normalize_ui_spec({
            "type": "model3d",
            "id": "long-procedural",
            "procedural_action": long_procedural,
            "retarget": {
                "adaptive_motion_scale": True,
                "motion_scale_min": 0.4,
                "motion_scale_max": 2.5,
                "prefer_model_clips": True,
            },
        })["nodes"][0]
        self.assertEqual(long_node["procedural_action"]["default_action"], "idle")
        self.assertIn("action_79", long_node["procedural_action"]["actions"])
        self.assertTrue(long_node["procedural_action"]["common"]["gait"]["foot_planting"])
        self.assertIs(long_node["retarget"]["adaptive_motion_scale"], True)
        self.assertEqual(long_node["retarget"]["motion_scale_min"], 0.4)
        self.assertEqual(long_node["retarget"]["motion_scale_max"], 2.5)
        self.assertIs(long_node["retarget"]["prefer_model_clips"], True)

    def test_model3d_preserves_action_json_text_from_csharp_plugin(self) -> None:
        raw = '{"wave":{"speed":1.7,"rightArmLift":0.8}}'
        node = normalize_ui_spec({
            "type": "model3d",
            "id": "avatar",
            "action": {"name": "wave", "json": raw},
        })["nodes"][0]

        self.assertEqual(node["action"]["json"]["wave"]["speed"], 1.7)
        self.assertEqual(node["action"]["json_text"], raw)

        helper = normalize_ui_spec(UI.model3d(
            "helper", "avatar.fbx", animation_name="wave",
            animation_json=raw,
        ))["nodes"][0]
        self.assertEqual(helper["action"]["json_text"], raw)

    def test_model3d_rejects_oversized_action_json_text_during_normalize(self) -> None:
        raw = "{" + ("x" * model3d_backend._ACTION_JSON_PARSE_LIMIT)
        with mock.patch.object(json, "loads", wraps=json.loads) as loads:
            node = normalize_ui_spec({
                "type": "model3d",
                "id": "avatar",
                "action": {"name": "wave", "json": raw},
            })["nodes"][0]

        self.assertEqual(loads.call_count, 0)
        self.assertEqual(node["action"]["json"], {})
        self.assertEqual(node["action"]["json_text"], "")
        self.assertIn("exceeds action json parse limit", node["action"]["json_error"])

        helper = normalize_ui_spec(UI.model3d(
            "helper", "avatar.fbx", animation_name="wave",
            animation_json=raw,
        ))["nodes"][0]
        self.assertEqual(helper["action"]["json_text"], "")
        self.assertIn("exceeds action json parse limit", helper["action"]["json_error"])

        meta = get_action_metadata(node)
        self.assertIn("exceeds action json parse limit", "; ".join(meta["errors"]))
        self.assertIn("_load_error", meta["selected"])

        keyframed = normalize_ui_spec({
            "type": "model3d",
            "id": "keyframed",
            "action": {
                "name": "wave",
                "json": {
                    "wave": {
                        "keyframes": [
                            {"time": 0.0, "offsets": {"right_hand": [0, 0, 0]}},
                            {"time": 0.5, "offsets": {"right_hand": [0, 0.4, 0]}},
                        ],
                    },
                },
            },
        })["nodes"][0]
        self.assertEqual(keyframed["action"]["json"]["wave"]["keyframes"][1]["offsets"]["right_hand"][1], 0.4)


class Model3DBackendTests(unittest.TestCase):
    def tearDown(self) -> None:
        clear_model3d_metadata_caches()
        clear_native_model3d_renderers()

    def test_backend_probe_and_diagnostic_are_safe_when_binaries_are_absent(self) -> None:
        with tempfile.TemporaryDirectory(prefix="model3d_empty_backend_") as root:
            empty = Path(root)
            with mock.patch.object(model3d_backend, "_candidate_roots", return_value=(empty,)):
                status = probe_model3d_backend()
        self.assertEqual(status.backend, "assimpnet")
        self.assertFalse(status.import_available)
        self.assertFalse(status.render_available)
        self.assertTrue(status.reason)

        key, lines = diagnose_model3d_node("plug", {
            "type": "model3d",
            "id": "avatar",
            "model": {"path": ""},
        }, status=status)
        self.assertEqual(key, "plug/avatar")
        self.assertTrue(any("model path is empty" in line for line in lines))

    def test_backend_probe_detects_bundled_assimpnet_importer(self) -> None:
        status = get_backend_status(force=True)

        self.assertEqual(status.backend, "assimpnet")
        self.assertTrue(status.files_present, status)
        self.assertTrue(status.import_available, status)
        self.assertFalse(status.render_available, status)
        self.assertTrue(any(path.endswith("AssimpNet.dll") for path in status.managed_files))
        self.assertTrue(any(path.endswith("assimp.dll") for path in status.native_files))

    def test_backend_probe_cache_avoids_repeated_directory_scans(self) -> None:
        clear_model3d_metadata_caches()
        with mock.patch.object(
            model3d_backend,
            "probe_model3d_backend",
            wraps=model3d_backend.probe_model3d_backend,
        ) as probe:
            first = get_backend_status()
            second = get_backend_status()
            self.assertIs(first, second)
            self.assertEqual(probe.call_count, 1)
            get_backend_status(force=True)
            self.assertEqual(probe.call_count, 2)

    def test_native_offscreen_renderer_preempts_fallback_without_window(self) -> None:
        from PIL import Image

        def fake_renderer(node, context):
            self.assertTrue(context["offscreen"])
            self.assertTrue(context["windowless"])
            self.assertEqual(context["presentation"], "existing_compositor_layer")
            self.assertIn("model", context)
            self.assertIn("action", context)
            self.assertIn("retarget", context)
            self.assertIn("pose", context)
            self.assertIn("backend", context)
            return Image.new("RGBA", (context["width"], context["height"]), (10, 20, 30, 255))

        register_native_model3d_renderer("fake-offscreen", fake_renderer)
        node = normalize_ui_spec({
            "type": "model3d",
            "id": "avatar",
            "width": 32,
            "height": 24,
            "model": {"path": "missing.fbx"},
        })["nodes"][0]

        with mock.patch("render.model3d_overlay.evaluate_retarget_pose") as pose:
            image = render_model3d_node(node)

        self.assertEqual(image.size, (32, 24))
        self.assertEqual(image.getpixel((1, 1)), (10, 20, 30, 255))
        self.assertEqual(pose.call_count, 0)
        self.assertEqual(native_model3d_renderer_status()["renderer"], "fake-offscreen")

    def test_builtin_native_renderer_bootstraps_without_window(self) -> None:
        fake_module = ModuleType("render.model3d_native_moderngl")
        calls = []

        def register_builtin_renderers(register):
            from PIL import Image

            def fake_renderer(_node, context):
                calls.append(dict(context))
                self.assertTrue(context["offscreen"])
                self.assertTrue(context["windowless"])
                self.assertEqual(context["presentation"], "existing_compositor_layer")
                return Image.new("RGBA", (context["width"], context["height"]), (3, 5, 7, 255))

            register("builtin-fake-offscreen", fake_renderer)
            return ("builtin-fake-offscreen",)

        fake_module.register_builtin_renderers = register_builtin_renderers
        with mock.patch.dict(sys.modules, {"render.model3d_native_moderngl": fake_module}):
            node = normalize_ui_spec({
                "type": "model3d",
                "id": "avatar",
                "width": 8,
                "height": 6,
                "model": {"path": "missing.fbx"},
            })["nodes"][0]
            image = render_model3d_node(node)

        self.assertEqual(image.size, (8, 6))
        self.assertEqual(image.getpixel((0, 0)), (3, 5, 7, 255))
        self.assertEqual(len(calls), 1)
        self.assertEqual(native_model3d_renderer_status()["renderer"], "builtin-fake-offscreen")

    def test_reset_native_model3d_resources_calls_loaded_provider(self) -> None:
        fake_module = ModuleType("render.model3d_native_moderngl")
        reset_calls = []

        def reset_builtin_renderer_resources():
            reset_calls.append("reset")
            return {"ok": True, "released": ("ctx",)}

        fake_module.reset_builtin_renderer_resources = reset_builtin_renderer_resources
        with mock.patch.dict(sys.modules, {"render.model3d_native_moderngl": fake_module}):
            result = reset_native_model3d_resources()

        self.assertTrue(result["ok"], result)
        self.assertEqual(result["provider_count"], 1)
        self.assertEqual(reset_calls, ["reset"])
        self.assertEqual(
            result["providers"]["render.model3d_native_moderngl"]["released"],
            ("ctx",),
        )

    def test_explicit_native_renderer_is_not_preempted_by_builtin_bootstrap(self) -> None:
        from PIL import Image

        fake_module = ModuleType("render.model3d_native_moderngl")
        bootstrap_calls = []

        def register_builtin_renderers(register):
            bootstrap_calls.append("called")

            def fake_renderer(_node, context):
                return Image.new("RGBA", (context["width"], context["height"]), (50, 60, 70, 255))

            register("builtin-should-not-run", fake_renderer)
            return ("builtin-should-not-run",)

        def explicit_renderer(_node, context):
            return Image.new("RGBA", (context["width"], context["height"]), (9, 8, 7, 255))

        fake_module.register_builtin_renderers = register_builtin_renderers
        register_native_model3d_renderer("explicit-offscreen", explicit_renderer)
        with mock.patch.dict(sys.modules, {"render.model3d_native_moderngl": fake_module}):
            node = normalize_ui_spec({
                "type": "model3d",
                "id": "avatar",
                "width": 8,
                "height": 6,
                "model": {"path": "missing.fbx"},
            })["nodes"][0]
            image = render_model3d_node(node)

        self.assertEqual(bootstrap_calls, [])
        self.assertEqual(image.getpixel((0, 0)), (9, 8, 7, 255))
        self.assertEqual(native_model3d_renderer_status()["renderer"], "explicit-offscreen")

    def test_builtin_moderngl_provider_renders_preview_without_window(self) -> None:
        from render import model3d_native_moderngl as provider

        class DummyLock:
            def __enter__(self):
                return self

            def __exit__(self, *_exc):
                return False

        class FakeUniform:
            def __init__(self):
                self.value = None

        class FakeProgram:
            def __init__(self):
                self.uniforms = {}

            def __getitem__(self, name):
                self.uniforms.setdefault(name, FakeUniform())
                return self.uniforms[name]

            def __setitem__(self, name, value):
                self[name].value = value

        class FakeBuffer:
            def __init__(self, data):
                self.data = data

            def release(self):
                pass

        class FakeVertexArray:
            def render(self, _mode):
                pass

            def release(self):
                pass

        class FakeTexture:
            def __init__(self, size):
                self.size = size

            def release(self):
                pass

        class FakeFramebuffer:
            def __init__(self, size):
                self.size = size

            def use(self):
                pass

            def clear(self, *_rgba):
                pass

            def read(self, *, components=4, alignment=1):
                width, height = self.size
                assert components == 4
                assert alignment == 1
                return bytes((18, 48, 88, 220)) * width * height

            def release(self):
                pass

        class FakeContext:
            def __init__(self):
                self.line_width = 1.0
                self.blend_func = None

            def program(self, **_kwargs):
                return FakeProgram()

            def texture(self, size, components, **_kwargs):
                assert components == 4
                return FakeTexture(size)

            def framebuffer(self, *, color_attachments):
                return FakeFramebuffer(color_attachments[0].size)

            def buffer(self, data):
                return FakeBuffer(data)

            def simple_vertex_array(self, _program, _vbo, *_attrs):
                return FakeVertexArray()

            def enable(self, _flag):
                pass

        fake_moderngl = SimpleNamespace(
            create_standalone_context=lambda **_kwargs: FakeContext(),
            BLEND=1,
            TRIANGLES=4,
            LINES=1,
            SRC_ALPHA=770,
            ONE_MINUS_SRC_ALPHA=771,
        )
        provider._reset_for_tests()
        with tempfile.TemporaryDirectory(prefix="model3d_moderngl_provider_") as root:
            model_path = Path(root) / "avatar.obj"
            model_path.write_text(
                "\n".join([
                    "o PreviewAvatar",
                    "v -1 0 0",
                    "v 1 0 0",
                    "v 1 2 0",
                    "v -1 2 0",
                    "v -1 0 1",
                    "v 1 0 1",
                    "v 1 2 1",
                    "v -1 2 1",
                    "f 1 2 3 4",
                    "f 5 6 7 8",
                    "f 1 2 6 5",
                    "f 2 3 7 6",
                ]),
                encoding="utf-8",
            )
            node = normalize_ui_spec({
                "type": "model3d",
                "width": 48,
                "height": 64,
                "model": {"path": str(model_path), "format": "obj"},
            })["nodes"][0]
            context = build_native_model3d_context(node, palette={"accent": "#7dd3fc"})
            with mock.patch.dict(sys.modules, {"moderngl": fake_moderngl}):
                with mock.patch.object(provider, "_get_wgl_serialize_lock", return_value=DummyLock()):
                    image = provider.render_moderngl_model3d(node, context)
        provider._reset_for_tests()

        self.assertEqual(image.size, (48, 64))
        self.assertEqual(image.getpixel((0, 0)), (18, 48, 88, 220))

    def test_moderngl_reset_releases_tls_resources_and_clears_cache(self) -> None:
        from render import model3d_native_moderngl as provider

        class FakeResource:
            def __init__(self):
                self.release_calls = 0

            def release(self):
                self.release_calls += 1

        program = FakeResource()
        ctx = FakeResource()
        provider._TLS.state = {"program": program, "ctx": ctx}
        provider._TOPOLOGY_CACHE[("fixture", "preview")] = ([], [])

        with mock.patch.object(
            provider,
            "_get_wgl_serialize_lock",
            return_value=contextlib.nullcontext(),
        ):
            result = provider.reset_builtin_renderer_resources()

        self.assertTrue(result["ok"], result)
        self.assertEqual(result["released"], ("program", "ctx"))
        self.assertEqual(result["topology_cache_size"], 0)
        self.assertEqual(program.release_calls, 1)
        self.assertEqual(ctx.release_calls, 1)
        self.assertFalse(hasattr(provider._TLS, "state"))
        self.assertFalse(provider._TOPOLOGY_CACHE)
        provider._reset_for_tests()

    def test_moderngl_provider_reuses_preview_topology_batches(self) -> None:
        from render import model3d_native_moderngl as provider

        if provider.np is None:
            self.skipTest("numpy is unavailable")
        provider._reset_for_tests()
        projected = [
            (8.0, 8.0, 0.0),
            (40.0, 8.0, 0.1),
            (40.0, 56.0, 0.2),
            (8.0, 56.0, 0.3),
            (18.0, 18.0, 0.4),
            (30.0, 18.0, 0.5),
            (30.0, 44.0, 0.6),
            (18.0, 44.0, 0.7),
        ]
        faces = [[0, 1, 2, 3], [4, 5, 6, 7]]
        preview = {
            "faces": faces,
            "skin": [
                [{"joint": "hair_white", "weight": 1.0}],
                [{"joint": "hair_white", "weight": 1.0}],
                [{"joint": "hair_white", "weight": 1.0}],
                [{"joint": "hair_white", "weight": 1.0}],
                [{"joint": "outfit_olive", "weight": 1.0}],
                [{"joint": "outfit_olive", "weight": 1.0}],
                [{"joint": "outfit_olive", "weight": 1.0}],
                [{"joint": "outfit_olive", "weight": 1.0}],
            ],
        }
        with mock.patch.object(provider, "_face_base_color", wraps=provider._face_base_color) as color:
            first = provider._mesh_arrays_by_color(projected, faces, 48, 64, preview, (125, 211, 252, 255))
            first_count = color.call_count
            second = provider._mesh_arrays_by_color(projected, faces, 48, 64, preview, (125, 211, 252, 255))

        provider._reset_for_tests()
        self.assertGreater(first_count, 0)
        self.assertEqual(color.call_count, first_count)
        self.assertEqual(len(first[0]), len(second[0]))
        self.assertEqual(len(first[1]), len(second[1]))

    def test_missing_builtin_native_renderer_bootstrap_is_quiet(self) -> None:
        clear_native_model3d_renderers()
        modules = {
            key: value
            for key, value in sys.modules.items()
            if key != "render.model3d_native_missing_provider"
        }
        with mock.patch.object(sys, "modules", modules):
            with mock.patch(
                "render.model3d_native._BUILTIN_PROVIDER_MODULES",
                ("render.model3d_native_missing_provider",),
            ):
                result = bootstrap_builtin_native_model3d_renderers(force=True)

        self.assertEqual(result["renderer_count"], 0)
        self.assertEqual(result["errors"], [])
        self.assertEqual(
            native_model3d_renderer_status()["reason"],
            "no native offscreen model3d renderer registered",
        )

    def test_native_context_exposes_model_action_retarget_and_pose_resources(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_native_context_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text("fixture", encoding="utf-8")
            (Path(root) / "avatar.model3d.json").write_text(json.dumps({
                "skeleton": {
                    "bones": [
                        "mixamorig:Hips",
                        "mixamorig:Spine",
                        "mixamorig:Head",
                        "mixamorig:RightForeArm",
                        "mixamorig:RightHand",
                    ],
                    "bone_map": {
                        "hips": "mixamorig:Hips",
                        "spine": "mixamorig:Spine",
                        "head": "mixamorig:Head",
                        "right_forearm": "mixamorig:RightForeArm",
                        "right_hand": "mixamorig:RightHand",
                    },
                    "rest_positions": {
                        "mixamorig:Hips": [0.0, 0.0, 0.0],
                        "mixamorig:Spine": [0.0, 0.45, 0.0],
                        "mixamorig:Head": [0.0, 1.28, 0.0],
                        "mixamorig:RightForeArm": [0.5, 0.5, 0.0],
                        "mixamorig:RightHand": [0.8, 0.42, 0.0],
                    },
                },
                "clips": ["Wave"],
                "native_unity_animations": {
                    "files": ["Unity/HandSign/K_Peace.anim"],
                    "hand_signs": {"peace": "Unity/HandSign/K_Peace.anim"},
                },
            }), encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "id": "avatar",
                "width": 64,
                "height": 96,
                "model": {"path": str(model_path)},
                "retarget": {"mode": "humanoid_auto"},
                "action": {
                    "name": "wave",
                    "json": {
                        "wave": {
                            "keyframes": [
                                {"time": 0.0, "offsets": {"right_hand": [0.0, 0.2, 0.0]}},
                            ],
                        },
                    },
                },
                "procedural_action": {
                    "schema": "sao.humanoid.procedural.v1",
                    "enabled": True,
                    "actions": {
                        "wave": {
                            "native_unity": ["Unity/HandSign/K_Peace.anim"],
                            "effectors": {
                                "right_hand": {
                                    "offset": [0.0, 0.2, 0.0],
                                    "weight": 1.0,
                                },
                            },
                        },
                    },
                },
            })["nodes"][0]

            context = build_native_model3d_context(node, palette={"accent": "#fff"})

        self.assertEqual((context["width"], context["height"]), (64, 96))
        self.assertTrue(context["offscreen"])
        self.assertTrue(context["windowless"])
        self.assertEqual(context["surface"], "unioverlay")
        self.assertTrue(context["model"]["exists"])
        self.assertIn("mixamorig:RightHand", context["model"]["bone_names"])
        self.assertEqual(context["action"]["name"], "wave")
        self.assertEqual(context["retarget"]["bone_map"]["right_hand"], "mixamorig:RightHand")
        self.assertTrue(context["pose"]["ok"], context["pose"])
        self.assertIn(
            "Unity/HandSign/K_Peace.anim",
            context["model"]["native_unity_animations"]["files"],
        )
        self.assertIn(
            "Unity/HandSign/K_Peace.anim",
            context["pose"]["procedural_action"]["native_unity"],
        )
        self.assertIn("model", context["signatures"])
        self.assertIn("action", context["signatures"])
        self.assertIn("backend", context)
        self.assertTrue(context["backend"]["import_available"], context["backend"])
        self.assertFalse(context["backend"]["render_available"], context["backend"])

    def test_native_offscreen_renderer_failure_falls_back(self) -> None:
        def failing_renderer(node, context):
            raise RuntimeError("backend unavailable")

        register_native_model3d_renderer("broken-offscreen", failing_renderer)
        node = normalize_ui_spec({
            "type": "model3d",
            "id": "avatar",
            "width": 32,
            "height": 48,
            "model": {"path": ""},
        })["nodes"][0]

        image = render_model3d_node(node)

        self.assertEqual(image.size, (32, 48))
        self.assertIsNotNone(image.getchannel("A").getbbox())
        status = native_model3d_renderer_status()
        self.assertIn("backend unavailable", "\n".join(status.get("errors") or []))

    def test_action_metadata_cache_parses_inline_json_text_once_per_hash(self) -> None:
        clear_model3d_metadata_caches()
        node = normalize_ui_spec({
            "type": "model3d",
            "id": "avatar",
            "action": {
                "name": "wave",
                "json": '{"wave":{"speed":2.25,"bounce":0.1}}',
            },
        })["nodes"][0]

        with mock.patch.object(json, "loads", wraps=json.loads) as loads:
            first = get_action_metadata(node)
            second = get_action_metadata(node)

        self.assertEqual(first["selected"]["speed"], 2.25)
        self.assertEqual(second["selected"]["bounce"], 0.1)
        self.assertEqual(loads.call_count, 1)

    def test_action_metadata_rejects_oversized_inline_json_before_parse(self) -> None:
        clear_model3d_metadata_caches()
        raw = "{" + ("x" * model3d_backend._ACTION_JSON_PARSE_LIMIT)
        node = {
            "type": "model3d",
            "id": "avatar",
            "action": {
                "name": "wave",
                "json": raw,
            },
        }

        with mock.patch.object(json, "loads", wraps=json.loads) as loads:
            with mock.patch.object(model3d_backend, "_hash_text", wraps=model3d_backend._hash_text) as hash_text:
                meta = get_action_metadata(node)

        self.assertEqual(loads.call_count, 0)
        self.assertIn("exceeds action json parse limit", "; ".join(meta["errors"]))
        self.assertIn("_load_error", meta["selected"])
        hashed_lengths = [
            len(call.args[0])
            for call in hash_text.call_args_list
            if call.args and isinstance(call.args[0], str)
        ]
        self.assertTrue(hashed_lengths)
        self.assertLessEqual(max(hashed_lengths), model3d_backend._ACTION_JSON_CACHE_PREFIX_LIMIT)

    def test_action_metadata_cache_loads_resolved_relative_action_file(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_action_") as root:
            base = Path(root)
            model_path = base / "avatar.fbx"
            action_path = base / "wave.json"
            model_path.write_text("fixture", encoding="utf-8")
            action_path.write_text(json.dumps({"wave": {"speed": 1.8}}), encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "id": "avatar",
                "model": {"path": str(model_path)},
                "action": {"name": "wave", "file": "wave.json"},
            })["nodes"][0]

            meta = get_action_metadata(node)

        self.assertEqual(meta["selected"]["speed"], 1.8)
        self.assertTrue(str(meta["resolved_file"]).endswith("wave.json"))
        self.assertEqual(meta["file_kind"], "json")

    def test_action_metadata_rejects_oversized_json_file_before_load(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_action_limit_") as root:
            base = Path(root)
            model_path = base / "avatar.fbx"
            action_path = base / "wave.json"
            model_path.write_text("fixture", encoding="utf-8")
            action_path.write_text("{" + ("x" * model3d_backend._ACTION_JSON_PARSE_LIMIT), encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "id": "avatar",
                "model": {"path": str(model_path)},
                "action": {"name": "wave", "file": "wave.json"},
            })["nodes"][0]

            with mock.patch.object(json, "load", wraps=json.load) as load:
                meta = get_action_metadata(node)

        self.assertEqual(load.call_count, 0)
        self.assertEqual(meta["file_kind"], "json")
        self.assertIn("exceeds action json parse limit", "; ".join(meta["errors"]))
        self.assertIn("_load_error", meta["selected"])

    def test_action_metadata_treats_non_json_file_as_motion_file(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_motion_") as root:
            base = Path(root)
            model_path = base / "avatar.fbx"
            motion_path = base / "wave.fbx"
            model_path.write_text("fixture", encoding="utf-8")
            motion_path.write_bytes(b"Kaydara FBX Binary fixture")
            node = normalize_ui_spec({
                "type": "model3d",
                "id": "avatar",
                "model": {"path": str(model_path)},
                "action": {"name": "wave", "file": "wave.fbx"},
            })["nodes"][0]

            with mock.patch.object(json, "load", wraps=json.load) as load:
                first = get_action_metadata(node)
                second = get_action_metadata(node)

        self.assertEqual(load.call_count, 0)
        self.assertEqual(first["file_kind"], "motion")
        self.assertEqual(second["motion_format"], "fbx")
        self.assertTrue(first["motion_exists"])
        self.assertTrue(str(first["resolved_motion_file"]).endswith("wave.fbx"))
        self.assertEqual(first["errors"], [])

    def test_action_metadata_prefers_model_dir_over_cwd_for_relative_file(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_action_priority_") as root:
            base = Path(root)
            model_dir = base / "model"
            cwd_dir = base / "cwd"
            model_dir.mkdir()
            cwd_dir.mkdir()
            model_path = model_dir / "avatar.fbx"
            model_path.write_text("fixture", encoding="utf-8")
            (model_dir / "wave.json").write_text(json.dumps({"wave": {"speed": 3.0}}), encoding="utf-8")
            (cwd_dir / "wave.json").write_text(json.dumps({"wave": {"speed": 0.25}}), encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path)},
                "action": {"name": "wave", "file": "wave.json"},
            })["nodes"][0]

            with mock.patch.object(model3d_backend.Path, "cwd", return_value=cwd_dir):
                meta = get_action_metadata(node)

        self.assertEqual(meta["selected"]["speed"], 3.0)

    def test_action_metadata_matches_export_prefixed_clip_names(self) -> None:
        clear_model3d_metadata_caches()
        node = normalize_ui_spec({
            "type": "model3d",
            "model": {"path": "missing/avatar.fbx"},
            "action": {"name": "wave", "clip": "wave"},
        })["nodes"][0]
        model_meta = {
            "cache_key": ("mock", "prefixed_clip"),
            "clip_keyframes": {
                "ARM-asaki-standard|Wave": {
                    "duration": 1.0,
                    "keyframes": [
                        {"time": 0.0, "offsets": {"right_hand": [0.0, 0.0, 0.0]}},
                        {"time": 1.0, "offsets": {"right_hand": [0.0, 0.4, 0.0]}},
                    ],
                },
            },
        }

        with mock.patch.object(model3d_backend, "get_model_metadata_view", return_value=model_meta):
            meta = get_action_metadata(node)

        self.assertEqual(meta["model_clip"], "ARM-asaki-standard|Wave")
        self.assertEqual(meta["selected"]["name"], "ARM-asaki-standard|Wave")
        self.assertTrue(meta["selected"]["keyframes"])

    def test_model_metadata_cache_invalidates_on_size_or_reload_key(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_model_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text("one", encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path), "reload_key": "a"},
            })["nodes"][0]
            first = get_model_metadata(node)
            second = get_model_metadata(node)
            self.assertEqual(first["cache_key"], second["cache_key"])

            model_path.write_text("one-two", encoding="utf-8")
            changed_size = get_model_metadata(node)
            self.assertNotEqual(first["cache_key"], changed_size["cache_key"])

            reload_node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path), "reload_key": "b"},
            })["nodes"][0]
            changed_reload = get_model_metadata(reload_node)

        self.assertNotEqual(changed_size["cache_key"], changed_reload["cache_key"])

    def test_model_metadata_cache_parses_sidecar_once_per_signature(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_sidecar_cache_") as root:
            model_path = Path(root) / "avatar.fbx"
            sidecar_path = Path(root) / "avatar.model3d.json"
            model_path.write_text("fixture", encoding="utf-8")
            sidecar_path.write_text(json.dumps({"bones": ["Hips"], "clips": ["Wave"]}), encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path)},
            })["nodes"][0]

            with mock.patch.object(json, "load", wraps=json.load) as load:
                first = get_model_metadata(node)
                second = get_model_metadata(node)

        self.assertEqual(load.call_count, 1)
        self.assertEqual(first["sidecar"]["path"], str(sidecar_path))
        self.assertEqual(second["clips"], ["Wave"])

    def test_model_metadata_view_reuses_cache_while_public_copy_stays_isolated(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_meta_view_") as root:
            model_path = Path(root) / "avatar.obj"
            model_path.write_text(
                "\n".join([
                    "o ViewAvatar",
                    "v 0 0 0",
                    "v 1 0 0",
                    "v 0 1 0",
                    "f 1 2 3",
                ]),
                encoding="utf-8",
            )
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path), "format": "obj"},
            })["nodes"][0]

            first_view = get_model_metadata_view(node)
            second_view = get_model_metadata_view(node)
            public_copy = get_model_metadata(node)
            public_copy["mesh"]["preview"]["vertices"][0][0] = 999.0
            third_view = get_model_metadata_view(node)

        self.assertIs(first_view, second_view)
        self.assertIs(first_view, third_view)
        self.assertEqual(third_view["mesh"]["preview"]["vertices"][0][0], 0.0)

    def test_model_metadata_extracts_obj_mesh_bounds(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_obj_meta_") as root:
            model_path = Path(root) / "avatar.obj"
            model_path.write_text(
                "\n".join([
                    "o CuteAvatar",
                    "v -1.0 -2.0 0.0",
                    "v 2.0 0.5 4.0",
                    "v 0.0 3.0 1.0",
                    "v 1.0 1.0 2.0",
                    "usemtl body",
                    "f 1 2 3",
                    "f 1 3 4",
                ]),
                encoding="utf-8",
            )
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path)},
            })["nodes"][0]

            meta = get_model_metadata(node)
            preview_face = list(meta["mesh"]["preview"]["faces"][0])
            meta["mesh"]["preview"]["vertices"][0][0] = 999.0
            again_preview_x = get_model_metadata(node)["mesh"]["preview"]["vertices"][0][0]

        self.assertEqual(meta["mesh"]["source"], "obj")
        self.assertEqual(meta["mesh"]["vertex_count"], 4)
        self.assertEqual(meta["mesh"]["face_count"], 2)
        self.assertEqual(meta["mesh"]["bbox"]["min"], [-1.0, -2.0, 0.0])
        self.assertEqual(meta["mesh"]["bbox"]["max"], [2.0, 3.0, 4.0])
        self.assertEqual(len(meta["mesh"]["preview"]["vertices"]), 4)
        self.assertEqual(preview_face, [0, 1, 2])
        self.assertIn("CuteAvatar", meta["nodes"])
        self.assertIn("body", meta["materials"])
        self.assertEqual(again_preview_x, -1.0)

    def test_model_metadata_imports_ply_with_bundled_assimpnet(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_assimp_ply_") as root:
            model_path = Path(root) / "avatar.ply"
            model_path.write_text(
                "\n".join([
                    "ply",
                    "format ascii 1.0",
                    "element vertex 4",
                    "property float x",
                    "property float y",
                    "property float z",
                    "element face 2",
                    "property list uchar int vertex_indices",
                    "end_header",
                    "0 0 0",
                    "1 0 0",
                    "1 1 0",
                    "0 1 0",
                    "3 0 1 2",
                    "3 0 2 3",
                ]),
                encoding="utf-8",
            )
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path), "format": "auto"},
            })["nodes"][0]

            meta = get_model_metadata(node)

        self.assertEqual(meta["mesh"]["source"], "assimpnet")
        self.assertGreaterEqual(meta["mesh"]["mesh_count"], 1)
        self.assertEqual(meta["mesh"]["vertex_count"], 4)
        self.assertEqual(meta["mesh"]["face_count"], 2)
        self.assertEqual(meta["mesh"]["preview"]["source"], "assimpnet")
        self.assertEqual(len(meta["mesh"]["preview"]["vertices"]), 4)
        self.assertEqual(len(meta["mesh"]["preview"]["faces"]), 2)
        self.assertEqual(meta["metadata_errors"], [])

    def test_assimpnet_preview_samples_late_materials_when_vertex_limited(self) -> None:
        from render import model3d_assimpnet

        def _mesh(material_index: int, x_offset: float) -> SimpleNamespace:
            vertices = [
                SimpleNamespace(X=x_offset + float(index % 4), Y=float(index // 4), Z=0.0)
                for index in range(12)
            ]
            faces = [
                SimpleNamespace(Indices=[0, 1, 4]),
                SimpleNamespace(Indices=[1, 5, 4]),
                SimpleNamespace(Indices=[6, 7, 10]),
                SimpleNamespace(Indices=[7, 11, 10]),
            ]
            return SimpleNamespace(
                Name=f"mesh_{material_index}",
                MaterialIndex=material_index,
                Vertices=vertices,
                Faces=faces,
                Bones=[],
            )

        scene = SimpleNamespace(MeshCount=2, Meshes=[_mesh(0, 0.0), _mesh(1, 10.0)])

        with mock.patch.object(model3d_assimpnet, "_MESH_PREVIEW_VERTEX_LIMIT", 8):
            with mock.patch.object(model3d_assimpnet, "_MESH_PREVIEW_FACE_LIMIT", 8):
                mesh_meta, _skins, _bones = model3d_assimpnet._extract_meshes(scene, ("Face", "Body"))

        preview = mesh_meta["preview"]
        self.assertLessEqual(len(preview["vertices"]), 8)
        self.assertIn("Face", preview["face_materials"])
        self.assertIn("Body", preview["face_materials"])

    def test_assimpnet_preview_is_full_by_default(self) -> None:
        from render import model3d_assimpnet

        vertices = [
            SimpleNamespace(X=float(index % 8), Y=float(index // 8), Z=0.0)
            for index in range(24)
        ]
        faces = [
            SimpleNamespace(Indices=[index, index + 1, index + 8])
            for index in range(16)
        ]
        scene = SimpleNamespace(
            MeshCount=1,
            Meshes=[
                SimpleNamespace(
                    Name="full_mesh",
                    MaterialIndex=0,
                    Vertices=vertices,
                    Faces=faces,
                    Bones=[],
                )
            ],
        )

        mesh_meta, _skins, _bones = model3d_assimpnet._extract_meshes(scene, ("Body",))

        preview = mesh_meta["preview"]
        self.assertEqual(mesh_meta["vertex_count"], 24)
        self.assertEqual(mesh_meta["face_count"], 16)
        self.assertEqual(len(preview["vertices"]), 24)
        self.assertEqual(len(preview["faces"]), 16)
        self.assertFalse(preview["truncated"])

    def test_preview_face_parsers_do_not_truncate_full_meshes(self) -> None:
        from render import model3d_native_moderngl
        from render import model3d_software

        raw_faces = [[0, 1, 2] for _index in range(5000)]

        self.assertEqual(len(model3d_software._faces(raw_faces, 3)), 5000)
        self.assertEqual(len(model3d_native_moderngl._faces(raw_faces, 3)), 5000)

    def test_model_metadata_extracts_ascii_fbx_mesh_bones_and_clips(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_fbx_meta_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text(
                """
; FBX 7.4.0 project file
Objects:  {
    Model: 1, "Model::mixamorig:Hips", "LimbNode" {}
    Model: 2, "Model::mixamorig:Spine", "LimbNode" {}
    Model: 3, "Model::mixamorig:Head", "LimbNode" {}
    Model: 4, "Model::BodyMesh", "Mesh" {
        Vertices: *9 {
            a: 0,0,0, 1,2,3, -1,0,2
        }
        PolygonVertexIndex: *4 {
            a: 0,1,2,-3
        }
    }
    AnimationStack: 5, "AnimStack::Wave", "" {}
}
""",
                encoding="utf-8",
            )
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path)},
                "retarget": {"mode": "humanoid_auto"},
            })["nodes"][0]

            meta = get_model_metadata(node)
            plan = get_retarget_plan(node)

        self.assertEqual(meta["mesh"]["source"], "fbx_ascii")
        self.assertEqual(meta["mesh"]["vertex_count"], 3)
        self.assertEqual(meta["mesh"]["face_count"], 1)
        self.assertEqual(meta["mesh"]["bbox"]["max"], [1.0, 2.0, 3.0])
        self.assertEqual(meta["mesh"]["preview"]["faces"], [[0, 1, 2]])
        self.assertIn("mixamorig:Hips", meta["bone_names"])
        self.assertIn("Wave", meta["clips"])
        self.assertEqual(plan["bone_map"]["hips"], "mixamorig:Hips")
        self.assertEqual(plan["bone_map"]["head"], "mixamorig:Head")

    def test_model_metadata_extracts_ascii_fbx_animation_curves_for_retarget(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_fbx_anim_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text(
                """
; FBX 7.4.0 project file
Objects:  {
    Model: 1, "Model::mixamorig:RightHand", "LimbNode" {}
    Model: 2, "Model::mixamorig:RightArm", "LimbNode" {}
    AnimationStack: 10, "AnimStack::Wave", "" {}
    AnimationLayer: 11, "AnimLayer::BaseLayer", "" {}
    AnimationCurveNode: 20, "AnimCurveNode::T", "" {}
    AnimationCurve: 21, "AnimCurve::", "" {
        KeyTime: *2 { a: 0,46186158000 }
        KeyValueFloat: *2 { a: 0,0.4 }
    }
    AnimationCurveNode: 30, "AnimCurveNode::R", "" {}
    AnimationCurve: 31, "AnimCurve::", "" {
        KeyTime: *2 { a: 0,46186158000 }
        KeyValueFloat: *2 { a: 0,90 }
    }
}
Connections:  {
    C: "OP",21,20,"d|Y"
    C: "OP",20,1,"Lcl Translation"
    C: "OO",20,11
    C: "OO",11,10
    C: "OP",31,30,"d|Z"
    C: "OP",30,2,"Lcl Rotation"
    C: "OO",30,11
}
""",
                encoding="utf-8",
            )
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path)},
                "action": {"name": "Wave", "time": 0.5},
                "retarget": {"mode": "humanoid_auto", "preserve_proportions": False},
            })["nodes"][0]

            meta = get_model_metadata(node)
            action = get_action_metadata(node)
            pose = evaluate_retarget_pose(node)

        self.assertIn("Wave", meta["clip_keyframes"])
        clip = meta["clip_keyframes"]["Wave"]
        self.assertEqual(clip["source"], "fbx_ascii")
        self.assertEqual(len(clip["keyframes"]), 2)
        self.assertEqual(clip["keyframes"][0]["bone_offsets"]["right_hand"], [0.0, 0.0, 0.0])
        self.assertEqual(clip["keyframes"][1]["bone_offsets"]["right_hand"], [0.0, 0.4, 0.0])
        quat = clip["keyframes"][1]["bone_rotations"]["right_arm"]
        self.assertAlmostEqual(quat[2], 0.70710678, places=5)
        self.assertAlmostEqual(quat[3], 0.70710678, places=5)
        self.assertEqual(action["model_clip"], "Wave")
        self.assertEqual(action["selected"]["source"], "fbx_ascii")
        self.assertTrue(pose["ok"], pose)
        self.assertEqual(pose["motion_source"], "keyframes")
        self.assertEqual(pose["motion_sample"]["rotation_count"], 1)
        self.assertGreater(pose["positions"]["right_hand"][1], pose["rest_positions"]["right_hand"][1])

    def test_sidecar_preview_skin_animates_ascii_fbx_preview(self) -> None:
        clear_model3d_metadata_caches()
        clear_native_model3d_renderers()
        with tempfile.TemporaryDirectory(prefix="model3d_fbx_sidecar_skin_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text(
                """
Objects:  {
    Geometry: 1, "Geometry::Cube", "Mesh" {
        Vertices: *24 {
            a: -0.5,-0.5,-0.5, 0.5,-0.5,-0.5, 0.5,0.5,-0.5, -0.5,0.5,-0.5,
               -0.5,-0.5,0.5, 0.5,-0.5,0.5, 0.5,0.5,0.5, -0.5,0.5,0.5
        }
        PolygonVertexIndex: *24 {
            a: 0,1,2,-4, 4,5,6,-8, 0,1,5,-5, 2,3,7,-7, 1,2,6,-6, 0,3,7,-5
        }
    }
    Model: 2, "Model::Cube", "Mesh" {}
}
""",
                encoding="utf-8",
            )
            skin = (
                [[{"joint": "hips", "weight": 1.0}] for _ in range(4)]
                + [[{"joint": "head", "weight": 1.0}] for _ in range(4)]
            )
            model_path.with_suffix(".model3d.json").write_text(json.dumps({
                "skeleton": {
                    "bones": ["Hips", "Head"],
                    "bone_map": {"hips": "Hips", "head": "Head"},
                    "rest_positions": {
                        "Hips": [0.0, -0.5, 0.0],
                        "Head": [0.0, 0.5, 0.0],
                    },
                },
                "mesh": {"preview": {"skin": skin}},
            }), encoding="utf-8")
            action_json = {
                "nod": {
                    "keyframes": [
                        {"time": 0.0, "offsets": {"head": [0.0, 0.0, 0.0]}},
                        {"time": 0.5, "offsets": {"head": [0.22, 0.10, 0.0]}},
                    ],
                },
            }
            first = normalize_ui_spec({
                "type": "model3d",
                "id": "avatar",
                "width": 160,
                "height": 160,
                "model": {"path": str(model_path)},
                "retarget": {"mode": "humanoid_auto", "stretch_limit": 0.2},
                "action": {"name": "nod", "json": action_json, "time": 0.0},
            })["nodes"][0]
            moved = normalize_ui_spec({
                "type": "model3d",
                "id": "avatar",
                "width": 160,
                "height": 160,
                "model": {"path": str(model_path)},
                "retarget": {"mode": "humanoid_auto", "stretch_limit": 0.2},
                "action": {"name": "nod", "json": action_json, "time": 0.25},
            })["nodes"][0]

            meta = get_model_metadata(first)
            image_a = render_model3d_node(first, {"accent": "#7dd3fc"})
            image_b = render_model3d_node(moved, {"accent": "#7dd3fc"})

        preview = meta["mesh"]["preview"]
        self.assertEqual(preview["skin_source"], "sidecar")
        self.assertEqual(preview["skin"][0][0]["joint"], "hips")
        self.assertEqual(preview["skin"][4][0]["joint"], "head")
        self.assertNotEqual(image_a.tobytes(), image_b.tobytes())

    def test_sidecar_preview_mesh_and_secondary_motion_are_merged(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_sidecar_preview_mesh_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text(
                """
Objects:  {
    Geometry: 1, "Geometry::Tiny", "Mesh" {
        Vertices: *9 { a: 0,0,0, 1,0,0, 0,1,0 }
        PolygonVertexIndex: *3 { a: 0,1,-3 }
    }
    Model: 2, "Model::Tiny", "Mesh" {}
}
""",
                encoding="utf-8",
            )
            model_path.with_suffix(".model3d.json").write_text(json.dumps({
                "materials": {"profile": "asaki_anime"},
                "physics": {
                    "enabled": True,
                    "secondary_motion": {
                        "enabled": True,
                        "chains": [{"name": "hair", "joints": ["hair_white"], "amplitude": 0.04}],
                    },
                },
                "mesh": {
                    "preview": {
                        "source": "sidecar_proxy",
                        "vertices": [[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]],
                        "faces": [[0, 1, 2], [0, 2, 3]],
                        "skin": [
                            [{"joint": "hair_white", "weight": 1.0}],
                            [{"joint": "hair_white", "weight": 1.0}],
                            [{"joint": "skin", "weight": 1.0}],
                            [{"joint": "skin", "weight": 1.0}],
                        ],
                    },
                },
            }), encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "id": "avatar",
                "model": {"path": str(model_path)},
            })["nodes"][0]
            meta = get_model_metadata(node)

        preview = meta["mesh"]["preview"]
        self.assertEqual(preview["source"], "sidecar_proxy")
        self.assertEqual(len(preview["vertices"]), 4)
        self.assertEqual(len(preview["faces"]), 2)
        self.assertEqual(preview["skin_source"], "sidecar")
        self.assertEqual(meta["materials_config"]["profile"], "asaki_anime")
        self.assertIs(meta["physics"]["enabled"], True)
        self.assertEqual(meta["secondary_motion"]["chains"][0]["name"], "hair")

    def test_model_metadata_extracts_gltf_json_names_and_bounds(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_gltf_meta_") as root:
            model_path = Path(root) / "avatar.gltf"
            model_path.write_text(
                json.dumps({
                    "nodes": [{"name": "Root"}, {"name": "Head"}],
                    "materials": [{"name": "skin"}, {"name": "dress"}],
                    "meshes": [{"name": "bodyMesh"}],
                    "accessors": [
                        {"min": [-0.25, 0.0, -0.5], "max": [0.25, 1.75, 0.5]},
                        {"min": [-0.5, 0.1, -0.25], "max": [0.5, 1.2, 0.25]},
                    ],
                }),
                encoding="utf-8",
            )
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path)},
            })["nodes"][0]

            meta = get_model_metadata(node)

        self.assertEqual(meta["mesh"]["source"], "gltf")
        self.assertEqual(meta["mesh"]["mesh_count"], 1)
        self.assertEqual(meta["mesh"]["bbox"]["min"], [-0.5, 0.0, -0.5])
        self.assertEqual(meta["mesh"]["bbox"]["max"], [0.5, 1.75, 0.5])
        self.assertEqual(meta["mesh"]["preview"]["source"], "bbox")
        self.assertEqual(len(meta["mesh"]["preview"]["vertices"]), 8)
        self.assertIn("Head", meta["nodes"])
        self.assertIn("dress", meta["materials"])

    def test_model_metadata_extracts_glb_mesh_preview(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_glb_meta_") as root:
            model_path = Path(root) / "avatar.glb"
            _write_cube_glb(model_path)
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path), "format": "glb"},
            })["nodes"][0]

            meta = get_model_metadata(node)

        self.assertEqual(meta["mesh"]["source"], "glb")
        self.assertEqual(meta["mesh"]["mesh_count"], 1)
        self.assertEqual(meta["mesh"]["vertex_count"], 8)
        self.assertEqual(meta["mesh"]["face_count"], 12)
        self.assertEqual(meta["mesh"]["bbox"]["min"], [-1.0, -1.0, -1.0])
        self.assertEqual(meta["mesh"]["bbox"]["max"], [1.0, 1.0, 1.0])
        self.assertEqual(meta["mesh"]["preview"]["source"], "glb")
        self.assertEqual(len(meta["mesh"]["preview"]["vertices"]), 8)
        self.assertEqual(len(meta["mesh"]["preview"]["faces"]), 12)
        self.assertIn("CubeNode", meta["nodes"])
        self.assertIn("PreviewMat", meta["materials"])

    def test_model_metadata_extracts_glb_skin_weights_for_preview(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_glb_skin_") as root:
            model_path = Path(root) / "avatar.glb"
            _write_skinned_strip_glb(model_path)
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path), "format": "glb"},
                "retarget": {"mode": "humanoid_auto"},
            })["nodes"][0]

            meta = get_model_metadata(node)
            plan = get_retarget_plan(node)

        self.assertEqual(meta["mesh"]["source"], "glb")
        self.assertEqual(meta["skins"][0]["joint_count"], 2)
        self.assertEqual(meta["skins"][0]["canonical_joints"], ["hips", "right_hand"])
        skin = meta["mesh"]["preview"]["skin"]
        self.assertEqual(len(skin), 8)
        self.assertEqual(skin[0][0]["joint"], "hips")
        self.assertEqual(skin[-1][0]["joint"], "right_hand")
        self.assertAlmostEqual(skin[-1][0]["weight"], 1.0)
        self.assertEqual(plan["bone_map"]["hips"], "mixamorig:Hips")
        self.assertEqual(plan["bone_map"]["right_hand"], "mixamorig:RightHand")

    def test_evaluate_retarget_pose_exposes_sampled_glb_rotations(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_glb_skin_rot_") as root:
            model_path = Path(root) / "avatar.glb"
            _write_skinned_strip_glb(model_path, rotation_clip=True)
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path), "format": "glb"},
                "action": {"name": "Wave", "time": 0.5},
                "retarget": {"mode": "humanoid_auto", "stretch_limit": 0.5},
            })["nodes"][0]

            pose = evaluate_retarget_pose(node)

        self.assertTrue(pose["ok"], pose)
        self.assertEqual(pose["motion_source"], "keyframes")
        self.assertEqual(pose["motion_sample"]["rotation_count"], 1)
        self.assertIn("right_hand", pose["rotations"])
        self.assertAlmostEqual(pose["positions"]["right_hand"][0], pose["rest_positions"]["right_hand"][0])
        self.assertAlmostEqual(pose["positions"]["right_hand"][1], pose["rest_positions"]["right_hand"][1])

    def test_model_metadata_applies_glb_node_rest_transforms(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_glb_rest_xform_") as root:
            model_path = Path(root) / "avatar.glb"
            _write_transformed_rest_glb(model_path)
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path), "format": "glb"},
                "retarget": {"mode": "humanoid_auto"},
            })["nodes"][0]

            meta = get_model_metadata(node)

        rest = meta["rest_positions"]
        right_hand = rest["mixamorig:RightHand"]
        left_hand = rest["mixamorig:LeftHand"]
        self.assertAlmostEqual(right_hand[0], 0.0, places=5)
        self.assertAlmostEqual(right_hand[1], 3.0, places=5)
        self.assertAlmostEqual(right_hand[2], 0.0, places=5)
        self.assertAlmostEqual(left_hand[0], 0.0, places=5)
        self.assertAlmostEqual(left_hand[1], 2.0, places=5)
        self.assertAlmostEqual(left_hand[2], 0.0, places=5)

    def test_model_metadata_extracts_glb_translation_clip_for_retarget(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_glb_anim_") as root:
            model_path = Path(root) / "avatar.glb"
            _write_animated_hand_glb(model_path)
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path), "format": "glb"},
                "action": {"name": "Wave", "time": 0.5},
                "retarget": {"mode": "humanoid_auto", "stretch_limit": 0.5},
            })["nodes"][0]

            meta = get_model_metadata(node)
            action = get_action_metadata(node)
            pose = evaluate_retarget_pose(node)

        self.assertEqual(meta["mesh"]["source"], "glb")
        self.assertIn("Wave", meta["clips"])
        self.assertIn("mixamorig:RightHand", meta["bone_names"])
        self.assertIn("Wave", meta["clip_keyframes"])
        self.assertEqual(meta["rest_positions_source"], "glb_nodes")
        rest_hand = meta["rest_positions"]["mixamorig:RightHand"]
        self.assertAlmostEqual(rest_hand[0], 0.68)
        self.assertAlmostEqual(rest_hand[1], 0.46)
        self.assertAlmostEqual(rest_hand[2], 0.0)
        self.assertEqual(action["model_clip"], "Wave")
        self.assertEqual(action["selected"]["source"], "glb")
        self.assertEqual(len(action["selected"]["keyframes"]), 2)
        self.assertTrue(pose["ok"], pose)
        self.assertEqual(pose["rest_source"], "glb_nodes")
        self.assertEqual(pose["motion_source"], "keyframes")
        self.assertGreater(pose["positions"]["right_hand"][1], 0.50)

    def test_model_metadata_extracts_glb_rotation_clip_for_retarget(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_glb_rot_") as root:
            model_path = Path(root) / "avatar.glb"
            _write_rotating_arm_glb(model_path)
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path), "format": "glb"},
                "action": {"name": "Wave", "time": 0.5},
                "retarget": {"mode": "humanoid_auto", "stretch_limit": 0.5},
            })["nodes"][0]

            meta = get_model_metadata(node)
            action = get_action_metadata(node)
            pose = evaluate_retarget_pose(node)

        clip = meta["clip_keyframes"]["Wave"]
        self.assertIn("bone_rotations", clip["keyframes"][1])
        self.assertIn("right_arm", clip["keyframes"][1]["bone_rotations"])
        self.assertEqual(action["model_clip"], "Wave")
        self.assertEqual(action["selected"]["source"], "glb")
        self.assertTrue(pose["ok"], pose)
        self.assertEqual(pose["motion_source"], "keyframes")
        self.assertEqual(pose["motion_sample"]["rotation_count"], 1)
        self.assertGreater(pose["positions"]["right_hand"][1], 0.65)

    def test_evaluate_retarget_pose_clamps_extreme_keyframe_rotation_twist(self) -> None:
        clear_model3d_metadata_caches()

        def quat_angle(value):
            x, y, z, w = [float(item) for item in value]
            if w < 0.0:
                x, y, z, w = -x, -y, -z, -w
            axis = math.sqrt(x * x + y * y + z * z)
            return 2.0 * math.atan2(axis, max(-1.0, min(1.0, w)))

        with tempfile.TemporaryDirectory(prefix="model3d_twist_limit_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text("fixture", encoding="utf-8")
            (Path(root) / "avatar.model3d.json").write_text(json.dumps({
                "skeleton": {
                    "bones": [
                        "mixamorig:RightArm",
                        "mixamorig:RightForeArm",
                        "mixamorig:RightHand",
                    ],
                    "bone_map": {
                        "right_arm": "mixamorig:RightArm",
                        "right_forearm": "mixamorig:RightForeArm",
                        "right_hand": "mixamorig:RightHand",
                    },
                    "rest_positions": {
                        "mixamorig:RightArm": [0.32, 0.82, 0.0],
                        "mixamorig:RightForeArm": [0.58, 0.58, 0.0],
                        "mixamorig:RightHand": [0.76, 0.36, 0.0],
                    },
                },
            }), encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path)},
                "retarget": {
                    "mode": "humanoid_auto",
                    "twist_limit": 0.10,
                    "stretch_limit": 0.5,
                },
                "action": {
                    "name": "wave",
                    "time": 0.0,
                    "json": {
                        "wave": {
                            "keyframes": [
                                {"time": 0.0, "rotations": {"right_arm": [0.0, 0.0, 1.0, 0.0]}},
                            ],
                        },
                    },
                },
            })["nodes"][0]

            pose = evaluate_retarget_pose(node)

        limit = pose["rotation_limit"]
        max_angle = 0.10 * math.pi
        self.assertTrue(pose["ok"], pose)
        self.assertEqual(pose["motion_sample"]["rotation_count"], 1)
        self.assertEqual(limit["clamped_count"], 1, limit)
        self.assertIn("right_arm", limit["clamped"])
        self.assertAlmostEqual(limit["max_angle"], max_angle)
        self.assertGreater(limit["source_angles"]["right_arm"], math.pi * 0.99)
        self.assertLessEqual(limit["angles"]["right_arm"], max_angle + 0.000001)
        self.assertLessEqual(quat_angle(pose["rotations"]["right_arm"]), max_angle + 0.000001)

    def test_retarget_plan_maps_mixamo_sidecar_bones(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_retarget_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text("fixture", encoding="utf-8")
            sidecar_path = Path(root) / "avatar.model3d.json"
            sidecar_path.write_text(json.dumps({
                "skeleton": {
                    "bones": [
                        "mixamorig:Hips",
                        "mixamorig:Spine",
                        "mixamorig:Spine2",
                        "mixamorig:Neck",
                        "mixamorig:Head",
                        "mixamorig:LeftArm",
                        "mixamorig:LeftForeArm",
                        "mixamorig:LeftHand",
                        "mixamorig:RightArm",
                        "mixamorig:RightForeArm",
                        "mixamorig:RightHand",
                        "mixamorig:LeftUpLeg",
                        "mixamorig:LeftLeg",
                        "mixamorig:LeftFoot",
                        "mixamorig:RightUpLeg",
                        "mixamorig:RightLeg",
                        "mixamorig:RightFoot",
                    ],
                },
                "clips": ["Idle", "Wave"],
            }), encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path)},
                "retarget": {"mode": "humanoid_auto", "stretch_limit": 0.2},
                "skeleton": {"hips": "auto", "left_arm": "auto"},
            })["nodes"][0]

            meta = get_model_metadata(node)
            plan = get_retarget_plan(node)

        self.assertEqual(meta["sidecar"]["path"], str(sidecar_path))
        self.assertIn("Wave", plan["clips"])
        self.assertEqual(plan["bone_map"]["hips"], "mixamorig:Hips")
        self.assertEqual(plan["bone_map"]["left_arm"], "mixamorig:LeftArm")
        self.assertEqual(plan["bone_map"]["right_foot"], "mixamorig:RightFoot")
        self.assertGreater(plan["coverage"], 0.75)
        self.assertAlmostEqual(plan["stretch_limit"], 0.2)

    def test_retarget_plan_maps_vroid_vrm_humanoid_bones(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_retarget_vrm_") as root:
            model_path = Path(root) / "avatar.vrm"
            model_path.write_text("fixture", encoding="utf-8")
            (Path(root) / "avatar.model3d.json").write_text(json.dumps({
                "skeleton": {
                    "bones": [
                        "J_Bip_C_Hips",
                        "J_Bip_C_Spine",
                        "J_Bip_C_Chest",
                        "J_Bip_C_Neck",
                        "J_Bip_C_Head",
                        "J_Bip_L_Shoulder",
                        "J_Bip_L_UpperArm",
                        "J_Bip_L_LowerArm",
                        "J_Bip_L_Hand",
                        "J_Bip_R_Shoulder",
                        "J_Bip_R_UpperArm",
                        "J_Bip_R_LowerArm",
                        "J_Bip_R_Hand",
                        "J_Bip_L_UpperLeg",
                        "J_Bip_L_LowerLeg",
                        "J_Bip_L_Foot",
                        "J_Bip_R_UpperLeg",
                        "J_Bip_R_LowerLeg",
                        "J_Bip_R_Foot",
                    ],
                },
            }), encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path), "format": "vrm"},
                "retarget": {"mode": "humanoid_auto"},
            })["nodes"][0]

            plan = get_retarget_plan(node)

        self.assertEqual(plan["bone_map"]["hips"], "J_Bip_C_Hips")
        self.assertEqual(plan["bone_map"]["left_arm"], "J_Bip_L_UpperArm")
        self.assertEqual(plan["bone_map"]["right_forearm"], "J_Bip_R_LowerArm")
        self.assertEqual(plan["bone_map"]["left_foot"], "J_Bip_L_Foot")
        self.assertGreater(plan["coverage"], 0.9)

    def test_retarget_plan_uses_declared_aliases_for_custom_humanoid_bones(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_retarget_aliases_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text("fixture", encoding="utf-8")
            (Path(root) / "avatar.model3d.json").write_text(json.dumps({
                "skeleton": {
                    "bones": [
                        "CenterPelvis",
                        "BodyColumn",
                        "UpperBodyNode",
                        "NeckPivot",
                        "FaceRigHead",
                        "Avatar_Left_Bicep",
                        "Avatar_Left_Elbow",
                        "Avatar_Left_Palm",
                        "Avatar_Right_Bicep",
                        "Avatar_Right_Elbow",
                        "Avatar_Right_Palm",
                    ],
                },
            }), encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path)},
                "retarget": {
                    "mode": "humanoid_auto",
                    "aliases": {
                        "hips": ["CenterPelvis"],
                        "spine": ["BodyColumn"],
                        "chest": ["UpperBodyNode"],
                        "neck": ["NeckPivot"],
                        "head": ["FaceRigHead"],
                        "left_arm": ["Avatar_Left_Bicep"],
                        "left_forearm": ["Avatar_Left_Elbow"],
                        "left_hand": ["Avatar_Left_Palm"],
                        "right_arm": ["Avatar_Right_Bicep"],
                        "right_forearm": ["Avatar_Right_Elbow"],
                        "right_hand": ["Avatar_Right_Palm"],
                    },
                },
            })["nodes"][0]

            plan = get_retarget_plan(node)

        self.assertEqual(plan["bone_map"]["hips"], "CenterPelvis")
        self.assertEqual(plan["bone_map"]["left_hand"], "Avatar_Left_Palm")
        self.assertEqual(plan["bone_map"]["right_forearm"], "Avatar_Right_Elbow")
        self.assertEqual(plan["sources"]["hips"], "declared_alias")
        self.assertIn("hips", plan["declared_aliases"])
        self.assertGreater(plan["coverage"], 0.5)

    def test_retarget_plan_cache_copies_and_invalidates_on_inputs(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_retarget_cache_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text("fixture", encoding="utf-8")
            (Path(root) / "avatar.model3d.json").write_text(json.dumps({
                "skeleton": {
                    "bones": [
                        "Hips", "Spine", "Head",
                        "LeftArm", "LeftForeArm", "LeftHand",
                        "RightArm", "RightForeArm", "RightHand",
                    ],
                },
                "clips": ["Idle"],
            }), encoding="utf-8")

            def node(**overrides):
                payload = {
                    "type": "model3d",
                    "model": {"path": str(model_path), "reload_key": "a"},
                    "retarget": {"mode": "humanoid_auto", "stretch_limit": 0.2},
                }
                payload.update(overrides)
                return normalize_ui_spec(payload)["nodes"][0]

            first = get_retarget_plan(node())
            first["bone_map"]["hips"] = "Mutated"
            second = get_retarget_plan(node())
            self.assertEqual(second["bone_map"]["hips"], "Hips")
            self.assertEqual(len(model3d_backend._RETARGET_PLAN_CACHE), 1)

            changed_retarget = get_retarget_plan(node(
                retarget={"mode": "humanoid_auto", "stretch_limit": 0.3}))
            self.assertAlmostEqual(changed_retarget["stretch_limit"], 0.3)
            self.assertEqual(len(model3d_backend._RETARGET_PLAN_CACHE), 2)

            changed_skeleton = get_retarget_plan(node(
                skeleton={"bone_map": {"hips": "CustomHips", "head": "Head"}}))
            self.assertEqual(changed_skeleton["bone_map"]["hips"], "CustomHips")
            self.assertEqual(len(model3d_backend._RETARGET_PLAN_CACHE), 3)

            changed_reload = get_retarget_plan(node(
                model={"path": str(model_path), "reload_key": "b"}))
            self.assertEqual(changed_reload["bone_map"]["hips"], "Hips")
            self.assertEqual(len(model3d_backend._RETARGET_PLAN_CACHE), 4)

    def test_retarget_plan_keeps_explicit_bone_map_without_sidecar(self) -> None:
        node = normalize_ui_spec({
            "type": "model3d",
            "model": {"path": "missing/avatar.fbx"},
            "retarget": {"mode": "humanoid_auto"},
            "skeleton": {"bone_map": {"hips": "CustomPelvis", "head": "CustomHead"}},
        })["nodes"][0]

        plan = get_retarget_plan(node)

        self.assertEqual(plan["bone_map"]["hips"], "CustomPelvis")
        self.assertEqual(plan["bone_map"]["head"], "CustomHead")
        self.assertIn("spine", plan["pending_backend"])
        self.assertTrue(plan["warnings"])

    def test_evaluate_retarget_pose_preserves_rest_lengths_and_clamps_offsets(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_pose_clamp_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text("fixture", encoding="utf-8")
            sidecar_path = Path(root) / "avatar.model3d.json"
            sidecar_path.write_text(json.dumps({
                "skeleton": {
                    "bones": [
                        "mixamorig:Hips",
                        "mixamorig:Spine",
                        "mixamorig:Spine2",
                        "mixamorig:Neck",
                        "mixamorig:Head",
                        "mixamorig:RightArm",
                        "mixamorig:RightForeArm",
                        "mixamorig:RightHand",
                    ],
                    "bone_map": {
                        "hips": "mixamorig:Hips",
                        "spine": "mixamorig:Spine",
                        "chest": "mixamorig:Spine2",
                        "neck": "mixamorig:Neck",
                        "head": "mixamorig:Head",
                        "right_arm": "mixamorig:RightArm",
                        "right_forearm": "mixamorig:RightForeArm",
                        "right_hand": "mixamorig:RightHand",
                    },
                    "rest_positions": {
                        "mixamorig:Hips": [0.0, 0.0, 0.0],
                        "mixamorig:Spine": [0.0, 0.45, 0.0],
                        "mixamorig:Spine2": [0.0, 0.84, 0.0],
                        "mixamorig:Neck": [0.0, 1.04, 0.0],
                        "mixamorig:Head": [0.0, 1.28, 0.0],
                        "mixamorig:RightArm": [0.32, 0.82, 0.0],
                        "mixamorig:RightForeArm": [0.58, 0.58, 0.0],
                        "mixamorig:RightHand": [0.76, 0.36, 0.0],
                    },
                },
            }), encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path)},
                "retarget": {
                    "mode": "humanoid_auto",
                    "preserve_proportions": True,
                    "stretch_limit": 0.05,
                },
                "action": {
                    "name": "wave",
                    "json": json.dumps({
                        "wave": {
                            "pose_offsets": {
                                "right_hand": [5.0, 5.0, 0.0],
                            },
                        },
                    }),
                },
            })["nodes"][0]

            meta = get_model_metadata(node)
            pose = evaluate_retarget_pose(node)

        self.assertIn("mixamorig:RightHand", meta["rest_positions"])
        self.assertTrue(pose["ok"], pose)
        self.assertEqual(pose["rest_source"], "sidecar")
        self.assertGreater(pose["clamped_count"], 0)
        self.assertLessEqual(pose["max_stretch"], 0.050001)
        right_hand = next(
            item for item in pose["segments"]
            if item["parent"] == "right_forearm" and item["child"] == "right_hand"
        )
        self.assertTrue(right_hand["clamped"])
        self.assertLessEqual(right_hand["length"], right_hand["rest_length"] * 1.050001)

    def test_evaluate_retarget_pose_procedural_wave_moves_hand(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_pose_wave_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text("fixture", encoding="utf-8")
            (Path(root) / "avatar.skeleton.json").write_text(json.dumps({
                "bones": [
                    "mixamorig:Hips",
                    "mixamorig:Spine",
                    "mixamorig:Head",
                    "mixamorig:RightArm",
                    "mixamorig:RightForeArm",
                    "mixamorig:RightHand",
                ],
            }), encoding="utf-8")
            first = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path)},
                "retarget": {"mode": "humanoid_auto", "stretch_limit": 0.12},
                "action": {"name": "wave"},
                "phase": 0.0,
            })["nodes"][0]
            second = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path)},
                "retarget": {"mode": "humanoid_auto", "stretch_limit": 0.12},
                "action": {"name": "wave"},
                "phase": 0.45,
            })["nodes"][0]

            pose_a = evaluate_retarget_pose(first)
            pose_b = evaluate_retarget_pose(second)

        self.assertTrue(pose_a["ok"], pose_a)
        self.assertTrue(pose_b["ok"], pose_b)
        self.assertNotEqual(pose_a["positions"]["right_hand"], pose_b["positions"]["right_hand"])
        self.assertLessEqual(pose_b["max_stretch"], 0.120001)

    def test_evaluate_retarget_pose_uses_relative_action_targets(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_pose_relative_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text("fixture", encoding="utf-8")
            (Path(root) / "avatar.model3d.json").write_text(json.dumps({
                "skeleton": {
                    "bones": [
                        "mixamorig:Hips",
                        "mixamorig:Spine",
                        "mixamorig:Spine2",
                        "mixamorig:Neck",
                        "mixamorig:Head",
                        "mixamorig:LeftArm",
                        "mixamorig:LeftForeArm",
                        "mixamorig:LeftHand",
                        "mixamorig:RightArm",
                        "mixamorig:RightForeArm",
                        "mixamorig:RightHand",
                        "mixamorig:LeftUpLeg",
                        "mixamorig:LeftLeg",
                        "mixamorig:LeftFoot",
                        "mixamorig:RightUpLeg",
                        "mixamorig:RightLeg",
                        "mixamorig:RightFoot",
                    ],
                    "rest_positions": {
                        "mixamorig:Hips": [0.0, 0.12, 0.0],
                        "mixamorig:Spine": [0.0, 0.55, 0.0],
                        "mixamorig:Spine2": [0.0, 0.88, 0.0],
                        "mixamorig:Neck": [0.0, 1.12, 0.0],
                        "mixamorig:Head": [0.0, 1.35, 0.0],
                        "mixamorig:LeftArm": [-0.40, 0.84, 0.0],
                        "mixamorig:LeftForeArm": [-0.58, 0.60, 0.0],
                        "mixamorig:LeftHand": [-0.68, 0.38, 0.0],
                        "mixamorig:RightArm": [0.40, 0.84, 0.0],
                        "mixamorig:RightForeArm": [0.58, 0.60, 0.0],
                        "mixamorig:RightHand": [0.68, 0.38, 0.0],
                        "mixamorig:LeftUpLeg": [-0.15, -0.26, 0.0],
                        "mixamorig:LeftLeg": [-0.17, -0.72, 0.0],
                        "mixamorig:LeftFoot": [-0.18, -1.08, 0.08],
                        "mixamorig:RightUpLeg": [0.15, -0.26, 0.0],
                        "mixamorig:RightLeg": [0.17, -0.72, 0.0],
                        "mixamorig:RightFoot": [0.18, -1.08, 0.08],
                    },
                },
            }), encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path)},
                "retarget": {"mode": "humanoid_auto", "stretch_limit": 0.35},
                "action": {"name": "walk"},
                "phase": 0.30,
                "procedural_action": {
                    "schema": "sao.humanoid.procedural.v1",
                    "enabled": True,
                    "mode": "relative_ik",
                    "references": ["github:sketchpunklabs/ossos"],
                    "actions": {
                        "walk": {
                            "body": {"breathing": 0.01, "sway": 0.02},
                            "head": {"look_at": [0.2, 1.45, 1.2], "weight": 0.5},
                            "gait": {
                                "enabled": True,
                                "cadence": 4.0,
                                "stride": 0.20,
                                "lift": 0.10,
                                "hip_bob": 0.03,
                                "arm_swing": 0.18,
                                "foot_planting": True,
                                "ground_y": "auto",
                            },
                            "effectors": {
                                "right_hand": {
                                    "offset": [0.0, 0.12, 0.0],
                                    "wave": [0.0, 0.03, 0.0],
                                    "frequency": 3.0,
                                },
                            },
                        },
                    },
                },
            })["nodes"][0]

            pose = evaluate_retarget_pose(node)

        self.assertTrue(pose["ok"], pose)
        self.assertTrue(pose["procedural_action"]["enabled"])
        self.assertTrue(pose["procedural_action"]["has_gait"])
        self.assertGreater(pose["procedural_action"]["offset_count"], 0)
        self.assertTrue(pose["foot_planting"]["enabled"])
        self.assertAlmostEqual(pose["foot_planting"]["ground_y"], -1.08)
        self.assertGreater(pose["positions"]["right_hand"][1], 0.38)
        self.assertGreater(pose["positions"]["head"][2], 0.0)

    def test_evaluate_retarget_pose_applies_body_lean_and_crouch(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_pose_body_controls_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text("fixture", encoding="utf-8")
            (Path(root) / "avatar.model3d.json").write_text(json.dumps({
                "skeleton": {
                    "bones": [
                        "Hips", "Spine", "Chest", "Neck", "Head",
                        "LeftKnee", "RightKnee",
                    ],
                    "bone_map": {
                        "hips": "Hips",
                        "spine": "Spine",
                        "chest": "Chest",
                        "neck": "Neck",
                        "head": "Head",
                        "left_knee": "LeftKnee",
                        "right_knee": "RightKnee",
                    },
                    "rest_positions": {
                        "Hips": [0.0, 0.12, 0.0],
                        "Spine": [0.0, 0.55, 0.0],
                        "Chest": [0.0, 0.88, 0.0],
                        "Neck": [0.0, 1.12, 0.0],
                        "Head": [0.0, 1.35, 0.0],
                        "LeftKnee": [-0.16, -0.72, 0.0],
                        "RightKnee": [0.16, -0.72, 0.0],
                    },
                },
            }), encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path)},
                "retarget": {"mode": "humanoid_auto", "stretch_limit": 0.5},
                "action": {"name": "guard"},
                "procedural_action": {
                    "schema": "sao.humanoid.procedural.v1",
                    "enabled": True,
                    "mode": "relative_ik",
                    "common": {
                        "body": {
                            "breathing": 0.0,
                            "sway": 0.0,
                            "crouch": 0.20,
                        },
                    },
                    "actions": {
                        "guard": {
                            "body": {
                                "lean": [0.08, -0.02, 0.06],
                            },
                        },
                    },
                },
            })["nodes"][0]

            pose = evaluate_retarget_pose(node)

        self.assertTrue(pose["ok"], pose)
        self.assertLess(pose["positions"]["hips"][1], pose["rest_positions"]["hips"][1])
        self.assertGreater(pose["positions"]["head"][0], pose["rest_positions"]["head"][0])
        self.assertGreater(pose["positions"]["head"][2], pose["rest_positions"]["head"][2])
        self.assertGreater(pose["positions"]["left_knee"][1], pose["rest_positions"]["left_knee"][1])

    def test_evaluate_retarget_pose_samples_looped_keyframes(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_pose_keyframes_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text("fixture", encoding="utf-8")
            (Path(root) / "avatar.model3d.json").write_text(json.dumps({
                "skeleton": {
                    "bones": [
                        "mixamorig:Hips",
                        "mixamorig:Spine",
                        "mixamorig:Spine2",
                        "mixamorig:Neck",
                        "mixamorig:Head",
                        "mixamorig:RightArm",
                        "mixamorig:RightForeArm",
                        "mixamorig:RightHand",
                    ],
                    "rest_positions": {
                        "mixamorig:Hips": [0, 0.0, 0],
                        "mixamorig:Spine": [0, 0.5, 0],
                        "mixamorig:Spine2": [0, 0.9, 0],
                        "mixamorig:Neck": [0, 1.1, 0],
                        "mixamorig:Head": [0, 1.35, 0],
                        "mixamorig:RightArm": [0.35, 0.85, 0],
                        "mixamorig:RightForeArm": [0.55, 0.62, 0],
                        "mixamorig:RightHand": [0.68, 0.42, 0],
                    },
                },
            }), encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path)},
                "retarget": {"mode": "humanoid_auto", "preserve_proportions": False},
                "action": {
                    "name": "wave",
                    "time": 1.25,
                    "json": {
                        "wave": {
                            "duration": 1.0,
                            "loop": True,
                            "keyframes": [
                                {"time": 0.0, "offsets": {"right_hand": [0.0, 0.0, 0.0]}},
                                {"time": 0.5, "offsets": {"right_hand": [0.0, 0.4, 0.0]}},
                                {"time": 1.0, "offsets": {"right_hand": [0.0, 0.0, 0.0]}},
                            ],
                        },
                    },
                },
            })["nodes"][0]

            pose = evaluate_retarget_pose(node)

        self.assertTrue(pose["ok"], pose)
        self.assertEqual(pose["motion_source"], "keyframes")
        self.assertAlmostEqual(pose["motion_sample"]["sample_time"], 0.25)
        self.assertAlmostEqual(pose["motion_sample"]["blend"], 0.5)
        self.assertEqual(pose["motion_scale"]["scaled_count"], 1)
        self.assertGreater(pose["motion_scale"]["scales"]["right_hand"], 0.98)
        self.assertLess(pose["motion_scale"]["scales"]["right_hand"], 1.0)
        self.assertGreater(pose["positions"]["right_hand"][1], 0.61)
        self.assertLess(pose["positions"]["right_hand"][1], 0.62)

    def test_evaluate_retarget_pose_scales_motion_to_target_limb_length(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_pose_motion_scale_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text("fixture", encoding="utf-8")
            default_parent = model3d_backend._DEFAULT_REST_POSITIONS["right_forearm"]
            default_child = model3d_backend._DEFAULT_REST_POSITIONS["right_hand"]
            half_vec = tuple((default_child[index] - default_parent[index]) * 0.5 for index in range(3))
            target_hand = [default_parent[index] + half_vec[index] for index in range(3)]
            (Path(root) / "avatar.model3d.json").write_text(json.dumps({
                "skeleton": {
                    "bones": ["mixamorig:RightForeArm", "mixamorig:RightHand"],
                    "bone_map": {
                        "right_forearm": "mixamorig:RightForeArm",
                        "right_hand": "mixamorig:RightHand",
                    },
                    "rest_positions": {
                        "mixamorig:RightForeArm": list(default_parent),
                        "mixamorig:RightHand": target_hand,
                    },
                },
            }), encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path)},
                "retarget": {
                    "mode": "humanoid_auto",
                    "preserve_proportions": False,
                },
                "action": {
                    "name": "wave",
                    "json": {
                        "wave": {
                            "duration": 1.0,
                            "loop": False,
                            "keyframes": [
                                {"time": 0.0, "offsets": {"right_hand": [0.0, 0.0, 0.0]}},
                                {"time": 1.0, "offsets": {"right_hand": [0.0, 0.4, 0.0]}},
                            ],
                        },
                    },
                    "time": 1.0,
                },
            })["nodes"][0]

            pose = evaluate_retarget_pose(node)

        self.assertTrue(pose["ok"], pose)
        self.assertEqual(pose["motion_source"], "keyframes")
        self.assertAlmostEqual(pose["motion_scale"]["scales"]["right_hand"], 0.5)
        self.assertAlmostEqual(pose["positions"]["right_hand"][1], target_hand[1] + 0.2)

    def test_evaluate_retarget_pose_can_disable_motion_scaling(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_pose_no_motion_scale_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text("fixture", encoding="utf-8")
            default_parent = model3d_backend._DEFAULT_REST_POSITIONS["right_forearm"]
            default_child = model3d_backend._DEFAULT_REST_POSITIONS["right_hand"]
            half_vec = tuple((default_child[index] - default_parent[index]) * 0.5 for index in range(3))
            target_hand = [default_parent[index] + half_vec[index] for index in range(3)]
            (Path(root) / "avatar.model3d.json").write_text(json.dumps({
                "skeleton": {
                    "bones": ["mixamorig:RightForeArm", "mixamorig:RightHand"],
                    "bone_map": {
                        "right_forearm": "mixamorig:RightForeArm",
                        "right_hand": "mixamorig:RightHand",
                    },
                    "rest_positions": {
                        "mixamorig:RightForeArm": list(default_parent),
                        "mixamorig:RightHand": target_hand,
                    },
                },
            }), encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path)},
                "retarget": {
                    "mode": "humanoid_auto",
                    "preserve_proportions": False,
                    "adaptive_motion_scale": False,
                },
                "action": {
                    "name": "wave",
                    "json": {
                        "wave": {
                            "keyframes": [
                                {"time": 0.0, "offsets": {"right_hand": [0.0, 0.4, 0.0]}},
                            ],
                        },
                    },
                },
            })["nodes"][0]

            pose = evaluate_retarget_pose(node)

        self.assertTrue(pose["ok"], pose)
        self.assertFalse(pose["motion_scale"]["enabled"])
        self.assertEqual(pose["motion_scale"]["scaled_count"], 0)
        self.assertAlmostEqual(pose["positions"]["right_hand"][1], target_hand[1] + 0.4)

    def test_evaluate_retarget_pose_clamps_shortened_segment_to_lower_bound(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_pose_short_clamp_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text("fixture", encoding="utf-8")
            (Path(root) / "avatar.model3d.json").write_text(json.dumps({
                "skeleton": {
                    "bones": ["mixamorig:RightForeArm", "mixamorig:RightHand"],
                    "bone_map": {
                        "right_forearm": "mixamorig:RightForeArm",
                        "right_hand": "mixamorig:RightHand",
                    },
                    "rest_positions": {
                        "mixamorig:RightForeArm": [0.50, 0.50, 0.0],
                        "mixamorig:RightHand": [0.90, 0.50, 0.0],
                    },
                },
            }), encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path)},
                "retarget": {
                    "mode": "humanoid_auto",
                    "preserve_proportions": True,
                    "stretch_limit": 0.2,
                },
                "action": {
                    "name": "idle",
                    "json": json.dumps({
                        "idle": {
                            "pose_offsets": {
                                "right_hand": [-0.39, 0.0, 0.0],
                            },
                        },
                    }),
                },
            })["nodes"][0]

            pose = evaluate_retarget_pose(node)

        segment = next(
            item for item in pose["segments"]
            if item["parent"] == "right_forearm" and item["child"] == "right_hand"
        )
        self.assertTrue(segment["clamped"])
        self.assertGreaterEqual(segment["length"], segment["rest_length"] * 0.799999)

    def test_evaluate_retarget_pose_preserve_false_does_not_clamp(self) -> None:
        clear_model3d_metadata_caches()
        with tempfile.TemporaryDirectory(prefix="model3d_pose_no_clamp_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text("fixture", encoding="utf-8")
            (Path(root) / "avatar.model3d.json").write_text(json.dumps({
                "skeleton": {
                    "bones": ["mixamorig:RightForeArm", "mixamorig:RightHand"],
                    "bone_map": {
                        "right_forearm": "mixamorig:RightForeArm",
                        "right_hand": "mixamorig:RightHand",
                    },
                    "rest_positions": {
                        "mixamorig:RightForeArm": [0.50, 0.50, 0.0],
                        "mixamorig:RightHand": [0.90, 0.50, 0.0],
                    },
                },
            }), encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path)},
                "retarget": {
                    "mode": "humanoid_auto",
                    "preserve_proportions": "false",
                    "stretch_limit": 0.05,
                },
                "action": {
                    "name": "idle",
                    "json": json.dumps({
                        "idle": {
                            "pose_offsets": {
                                "right_hand": [2.0, 0.0, 0.0],
                            },
                        },
                    }),
                },
            })["nodes"][0]

            pose = evaluate_retarget_pose(node)

        self.assertFalse(pose["preserve_proportions"])
        segment = next(
            item for item in pose["segments"]
            if item["parent"] == "right_forearm" and item["child"] == "right_hand"
        )
        self.assertEqual(pose["clamped_count"], 0)
        self.assertGreater(segment["stretch"], pose["stretch_limit"])

    def test_diagnose_model3d_node_includes_retarget_coverage(self) -> None:
        with tempfile.TemporaryDirectory(prefix="model3d_diag_retarget_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text("fixture", encoding="utf-8")
            (Path(root) / "avatar.skeleton.json").write_text(json.dumps({
                "bones": ["Hips", "Spine", "Head"],
            }), encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "model": {"path": str(model_path)},
                "retarget": {"mode": "humanoid_auto"},
            })["nodes"][0]

            _key, lines = diagnose_model3d_node("plug", node)

        self.assertTrue(any("retarget humanoid_auto" in line for line in lines), lines)


class Model3DOverlayRenderTests(unittest.TestCase):
    @unittest.skipIf(overlay_mod.Image is None, "PIL is unavailable")
    def test_fallback_avatar_hides_diagnostics_for_existing_model(self) -> None:
        with tempfile.TemporaryDirectory(prefix="model3d_avatar_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text("fixture", encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "id": "avatar",
                "width": 320,
                "height": 480,
                "model": {"path": str(model_path)},
                "action": {"name": "wave"},
            })["nodes"][0]

            image = render_model3d_node(node, {"accent": "#7dd3fc"})

        self.assertIsNotNone(image)
        crop = image.crop((0, 0, 110, 42))
        self.assertFalse(crop.getchannel("A").getbbox())

    @unittest.skipIf(overlay_mod.Image is None, "PIL is unavailable")
    def test_fallback_avatar_shows_diagnostics_for_missing_model(self) -> None:
        node = normalize_ui_spec({
            "type": "model3d",
            "id": "avatar",
            "width": 320,
            "height": 480,
            "model": {"path": "missing/avatar.fbx"},
            "action": {"name": "wave"},
        })["nodes"][0]

        image = render_model3d_node(node, {"accent": "#7dd3fc"})

        self.assertIsNotNone(image)
        crop = image.crop((0, 0, 110, 42))
        self.assertTrue(crop.getchannel("A").getbbox())

    @unittest.skipIf(overlay_mod.Image is None, "PIL is unavailable")
    def test_strict_assets_does_not_draw_avatar_fallback_for_existing_model(self) -> None:
        with tempfile.TemporaryDirectory(prefix="model3d_strict_assets_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text("fixture", encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "id": "avatar",
                "width": 320,
                "height": 480,
                "model": {"path": str(model_path)},
                "asset_policy": "strict",
                "strict_assets": True,
                "fallback": "diagnostic_only",
                "action": {"name": "wave"},
            })["nodes"][0]

            with mock.patch("render.model3d_overlay._draw_stylized_avatar") as stylized:
                with mock.patch("render.model3d_overlay._draw_retarget_pose_avatar") as retarget:
                    image = render_model3d_node(node, {"accent": "#7dd3fc"})

        self.assertIsNotNone(image)
        self.assertEqual(stylized.call_count, 0)
        self.assertEqual(retarget.call_count, 0)
        crop = image.crop((0, 0, 110, 42))
        self.assertTrue(crop.getchannel("A").getbbox())

    @unittest.skipIf(overlay_mod.Image is None, "PIL is unavailable")
    def test_software_mesh_preview_renders_before_avatar_fallback(self) -> None:
        with tempfile.TemporaryDirectory(prefix="model3d_software_") as root:
            model_path = Path(root) / "avatar.obj"
            model_path.write_text(
                "\n".join([
                    "o PreviewAvatar",
                    "v -1 0 0",
                    "v 1 0 0",
                    "v 1 2 0",
                    "v -1 2 0",
                    "v -1 0 1",
                    "v 1 0 1",
                    "v 1 2 1",
                    "v -1 2 1",
                    "f 1 2 3 4",
                    "f 5 6 7 8",
                    "f 1 2 6 5",
                    "f 2 3 7 6",
                ]),
                encoding="utf-8",
            )
            node = normalize_ui_spec({
                "type": "model3d",
                "width": 96,
                "height": 128,
                "model": {"path": str(model_path), "format": "obj"},
            })["nodes"][0]

            with mock.patch("render.model3d_overlay.evaluate_retarget_pose") as pose:
                image = render_model3d_node(node, {"accent": "#7dd3fc"})

        self.assertEqual(image.size, (96, 128))
        self.assertIsNotNone(image.getchannel("A").getbbox())
        self.assertEqual(pose.call_count, 0)

    @unittest.skipIf(overlay_mod.Image is None, "PIL is unavailable")
    def test_software_mesh_preview_uses_semantic_skin_colors(self) -> None:
        clear_model3d_metadata_caches()
        clear_native_model3d_renderers()
        with tempfile.TemporaryDirectory(prefix="model3d_semantic_skin_") as root:
            model_path = Path(root) / "avatar.obj"
            model_path.write_text(
                "\n".join([
                    "o SemanticAvatar",
                    "v -0.90 0.25 0",
                    "v -0.55 0.25 0",
                    "v -0.55 1.70 0",
                    "v -0.90 1.70 0",
                    "v -0.24 0.90 0.02",
                    "v 0.24 0.90 0.02",
                    "v 0.24 1.18 0.02",
                    "v -0.24 1.18 0.02",
                    "v 0.50 1.00 0",
                    "v 0.80 1.00 0",
                    "v 0.80 1.85 0",
                    "v 0.50 1.85 0",
                    "v -0.25 0.10 0.01",
                    "v 0.35 0.10 0.01",
                    "v 0.35 0.80 0.01",
                    "v -0.25 0.80 0.01",
                    "f 1 2 3 4",
                    "f 5 6 7 8",
                    "f 9 10 11 12",
                    "f 13 14 15 16",
                ]),
                encoding="utf-8",
            )
            (Path(root) / "avatar.model3d.json").write_text(json.dumps({
                "mesh": {
                    "preview": {
                        "skin": [
                            [{"joint": "hair_white", "weight": 1.0}],
                            [{"joint": "hair_white", "weight": 1.0}],
                            [{"joint": "hair_white", "weight": 1.0}],
                            [{"joint": "hair_white", "weight": 1.0}],
                            [{"joint": "eye_green", "weight": 1.0}],
                            [{"joint": "eye_green", "weight": 1.0}],
                            [{"joint": "eye_green", "weight": 1.0}],
                            [{"joint": "eye_green", "weight": 1.0}],
                            [{"joint": "rabbit_ear", "weight": 1.0}],
                            [{"joint": "rabbit_ear", "weight": 1.0}],
                            [{"joint": "rabbit_ear", "weight": 1.0}],
                            [{"joint": "rabbit_ear", "weight": 1.0}],
                            [{"joint": "outfit_olive", "weight": 1.0}],
                            [{"joint": "outfit_olive", "weight": 1.0}],
                            [{"joint": "outfit_olive", "weight": 1.0}],
                            [{"joint": "outfit_olive", "weight": 1.0}],
                        ]
                    }
                }
            }), encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "width": 128,
                "height": 128,
                "model": {"path": str(model_path), "format": "obj"},
            })["nodes"][0]

            image = render_model3d_node(node, {"accent": "#7dd3fc"})

        data = image.get_flattened_data() if hasattr(image, "get_flattened_data") else image.getdata()
        pixels = [rgba for rgba in data if rgba[3] > 100]
        self.assertTrue(any(r > 150 and g > 145 and b > 125 for r, g, b, _a in pixels))
        self.assertTrue(any(g > 120 and b > 85 and r < 105 for r, g, b, _a in pixels))

    @unittest.skipIf(overlay_mod.Image is None, "PIL is unavailable")
    def test_software_mesh_preview_uses_sidecar_material_texture_colors(self) -> None:
        clear_model3d_metadata_caches()
        clear_native_model3d_renderers()
        with tempfile.TemporaryDirectory(prefix="model3d_material_texture_") as root:
            root_path = Path(root)
            model_path = root_path / "avatar.fbx"
            model_path.write_text("fixture", encoding="utf-8")
            texture_dir = root_path / "Texture"
            texture_dir.mkdir()
            overlay_mod.Image.new("RGBA", (4, 4), (230, 20, 30, 255)).save(texture_dir / "Body.png")
            overlay_mod.Image.new("RGBA", (4, 4), (20, 45, 230, 255)).save(texture_dir / "Hair.png")
            (root_path / "avatar.model3d.json").write_text(json.dumps({
                "mesh": {
                    "preview": {
                        "vertices": [
                            [-0.9, 0.2, 0.0],
                            [-0.1, 0.2, 0.0],
                            [-0.1, 1.6, 0.0],
                            [-0.9, 1.6, 0.0],
                            [0.1, 0.2, 0.0],
                            [0.9, 0.2, 0.0],
                            [0.9, 1.6, 0.0],
                            [0.1, 1.6, 0.0],
                        ],
                        "faces": [[0, 1, 2, 3], [4, 5, 6, 7]],
                        "face_materials": ["Body", "Hair"],
                    },
                },
                "materials": {
                    "textures": {
                        "base": {
                            "Body": "Texture/Body.png",
                            "Hair": "Texture/Hair.png",
                        },
                    },
                },
            }), encoding="utf-8")
            node = normalize_ui_spec({
                "type": "model3d",
                "width": 128,
                "height": 128,
                "model": {"path": str(model_path), "format": "fbx"},
                "asset_policy": "strict",
                "strict_assets": True,
            })["nodes"][0]

            image = render_model3d_node(node, {"accent": "#7dd3fc"})

        data = image.get_flattened_data() if hasattr(image, "get_flattened_data") else image.getdata()
        pixels = [rgba for rgba in data if rgba[3] > 100]
        self.assertTrue(any(r > 145 and g < 90 and b < 100 for r, g, b, _a in pixels))
        self.assertTrue(any(b > 145 and r < 100 and g < 110 for r, g, b, _a in pixels))

    @unittest.skipIf(overlay_mod.Image is None, "PIL is unavailable")
    def test_glb_software_preview_renders_without_native_window(self) -> None:
        with tempfile.TemporaryDirectory(prefix="model3d_glb_render_") as root:
            model_path = Path(root) / "avatar.glb"
            _write_cube_glb(model_path)
            node = normalize_ui_spec({
                "type": "model3d",
                "width": 96,
                "height": 128,
                "model": {"path": str(model_path), "format": "glb"},
            })["nodes"][0]

            with mock.patch("render.model3d_overlay.evaluate_retarget_pose") as pose:
                image = render_model3d_node(node, {"accent": "#7dd3fc"})

        self.assertEqual(image.size, (96, 128))
        self.assertIsNotNone(image.getchannel("A").getbbox())
        self.assertEqual(pose.call_count, 0)

    @unittest.skipIf(overlay_mod.Image is None, "PIL is unavailable")
    def test_skinned_glb_software_preview_deforms_with_action_pose(self) -> None:
        clear_model3d_metadata_caches()
        clear_native_model3d_renderers()
        with tempfile.TemporaryDirectory(prefix="model3d_glb_skin_render_") as root:
            from PIL import ImageChops

            model_path = Path(root) / "avatar.glb"
            _write_skinned_strip_glb(model_path)
            base = {
                "type": "model3d",
                "width": 220,
                "height": 220,
                "model": {"path": str(model_path), "format": "glb"},
                "retarget": {"mode": "humanoid_auto", "stretch_limit": 0.5},
            }
            first = normalize_ui_spec({
                **base,
                "action": {"name": "Wave", "time": 0.0},
            })["nodes"][0]
            moved = normalize_ui_spec({
                **base,
                "action": {"name": "Wave", "time": 0.5},
            })["nodes"][0]

            image_a = render_model3d_node(first, {"accent": "#7dd3fc"})
            image_b = render_model3d_node(moved, {"accent": "#7dd3fc"})
            pose = evaluate_retarget_pose(moved)
            meta = get_model_metadata(moved)
            diff = ImageChops.difference(image_a, image_b)

        self.assertTrue(pose["ok"], pose)
        self.assertEqual(pose["motion_source"], "keyframes")
        self.assertIn("skin", meta["mesh"]["preview"])
        self.assertIsNotNone(diff.getbbox())

    @unittest.skipIf(overlay_mod.Image is None, "PIL is unavailable")
    def test_skinned_glb_software_preview_uses_sampled_bone_rotation(self) -> None:
        clear_model3d_metadata_caches()
        clear_native_model3d_renderers()
        with tempfile.TemporaryDirectory(prefix="model3d_glb_skin_rot_render_") as root:
            from PIL import ImageChops

            model_path = Path(root) / "avatar.glb"
            _write_skinned_strip_glb(model_path, rotation_clip=True)
            base = {
                "type": "model3d",
                "width": 220,
                "height": 220,
                "model": {"path": str(model_path), "format": "glb"},
                "retarget": {"mode": "humanoid_auto", "stretch_limit": 0.5},
            }
            first = normalize_ui_spec({
                **base,
                "action": {"name": "Wave", "time": 0.0},
            })["nodes"][0]
            rotated = normalize_ui_spec({
                **base,
                "action": {"name": "Wave", "time": 0.5},
            })["nodes"][0]

            image_a = render_model3d_node(first, {"accent": "#7dd3fc"})
            image_b = render_model3d_node(rotated, {"accent": "#7dd3fc"})
            pose = evaluate_retarget_pose(rotated)
            diff = ImageChops.difference(image_a, image_b)

        self.assertTrue(pose["ok"], pose)
        self.assertIn("right_hand", pose["rotations"])
        self.assertIsNotNone(diff.getbbox())

    @unittest.skipIf(overlay_mod.Image is None, "PIL is unavailable")
    def test_retarget_pose_preview_animates_existing_model_without_extra_window(self) -> None:
        with tempfile.TemporaryDirectory(prefix="model3d_pose_render_") as root:
            model_path = Path(root) / "avatar.fbx"
            model_path.write_text("fixture", encoding="utf-8")
            (Path(root) / "avatar.model3d.json").write_text(json.dumps({
                "skeleton": {
                    "bones": [
                        "mixamorig:Hips",
                        "mixamorig:Spine",
                        "mixamorig:Spine2",
                        "mixamorig:Neck",
                        "mixamorig:Head",
                        "mixamorig:LeftArm",
                        "mixamorig:LeftForeArm",
                        "mixamorig:LeftHand",
                        "mixamorig:RightArm",
                        "mixamorig:RightForeArm",
                        "mixamorig:RightHand",
                    ],
                    "rest_positions": {
                        "mixamorig:Hips": [0.0, 0.0, 0.0],
                        "mixamorig:Spine": [0.0, 0.42, 0.0],
                        "mixamorig:Spine2": [0.0, 0.78, 0.0],
                        "mixamorig:Neck": [0.0, 1.02, 0.0],
                        "mixamorig:Head": [0.0, 1.28, 0.0],
                        "mixamorig:LeftArm": [-0.34, 0.76, 0.0],
                        "mixamorig:LeftForeArm": [-0.56, 0.53, 0.0],
                        "mixamorig:LeftHand": [-0.68, 0.34, 0.0],
                        "mixamorig:RightArm": [0.34, 0.76, 0.0],
                        "mixamorig:RightForeArm": [0.56, 0.53, 0.0],
                        "mixamorig:RightHand": [0.68, 0.34, 0.0],
                    },
                },
            }), encoding="utf-8")
            first = normalize_ui_spec({
                "type": "model3d",
                "id": "avatar",
                "width": 240,
                "height": 320,
                "model": {"path": str(model_path)},
                "retarget": {"mode": "humanoid_auto", "stretch_limit": 0.1},
                "action": {"name": "wave"},
                "phase": 0.0,
            })["nodes"][0]
            second = normalize_ui_spec({
                "type": "model3d",
                "id": "avatar",
                "width": 240,
                "height": 320,
                "model": {"path": str(model_path)},
                "retarget": {"mode": "humanoid_auto", "stretch_limit": 0.1},
                "action": {"name": "wave"},
                "phase": 0.45,
            })["nodes"][0]

            image_a = render_model3d_node(first, {"accent": "#7dd3fc"})
            image_b = render_model3d_node(second, {"accent": "#7dd3fc"})

        self.assertIsNotNone(image_a)
        self.assertIsNotNone(image_b)
        self.assertTrue(image_a.getchannel("A").getbbox())
        self.assertNotEqual(image_a.tobytes(), image_b.tobytes())


class UnifiedOverlayDrawableTests(unittest.TestCase):
    def _sample_overlays(self):
        spec = {"nodes": [
            UI.canvas(20, 10, [UI.rect(0, 0, 20, 10, fill="accent")],
                      x=11, y=22, z=3, id="meter"),
            UI.model3d("avatar", "missing/avatar.fbx", width=40, height=30,
                       x=44, y=55, z=8),
        ]}
        return [{"plugin_id": "plug", "surface": "unioverlay", "spec": normalize_ui_spec(spec)}]

    def test_plugin_host_uses_platform_unified_overlay_provider(self) -> None:
        self.assertEqual(
            getattr(overlay_mod.get_unified_overlay, "__name__", ""),
            "_get_unified_overlay",
        )

    def test_overlay_adapter_uses_platform_unified_overlay_provider(self) -> None:
        self.assertEqual(
            getattr(overlay_adapter_mod.get_unified_overlay, "__name__", ""),
            "_get_unified_overlay",
        )

    def test_overlay_adapter_routes_input_without_tk_proxy(self) -> None:
        class FakeLayer:
            def __init__(self):
                self.click_through = False
                self.callbacks = None
                self.proxy_created = False

            def set_render_fn(self, _fn):
                return None

            def set_input_callbacks(self, *callbacks):
                self.callbacks = callbacks

            def create_input_proxy(self, _root):
                self.proxy_created = True
                raise AssertionError("adapter should not create a Tk proxy")

            def sync_input_proxy(self):
                return None

            def show(self):
                return None

            def hide(self):
                return None

            def destroy_input_proxy(self):
                return None

        class FakeOverlay:
            def __init__(self):
                self.layer = FakeLayer()
                self.syncs = 0

            def create_layer(self, *_args, **_kwargs):
                return self.layer

            def destroy_layer(self, _name):
                return None

            def sync_host_input_mode(self):
                self.syncs += 1

            def force_host_input_passthrough(self):
                self.sync_host_input_mode()

        fake = FakeOverlay()
        win = overlay_adapter_mod.CompositorOverlayWindow(
            fake, w=20, h=10, click_through=False)
        win.set_input_callbacks(mouse_button_fn=lambda *_args: None)

        self.assertIsNotNone(fake.layer.callbacks)
        self.assertFalse(fake.layer.proxy_created)
        self.assertEqual(fake.syncs, 1)

    def test_plugin_host_does_not_own_overlay_startup(self) -> None:
        class FakeOverlay:
            def __init__(self):
                self._running = False
                self.start_calls = 0
                self.passthrough_calls = 0

            def start(self):
                self.start_calls += 1

            def force_host_input_passthrough(self):
                self.passthrough_calls += 1

        fake_overlay = FakeOverlay()
        host = overlay_mod.PluginUnifiedOverlayHost(
            SimpleNamespace(root=object()), surface="unioverlay")

        with mock.patch.object(overlay_mod, "get_unified_overlay", lambda _root: fake_overlay):
            self.assertIs(host._ensure_overlay(), fake_overlay)

        self.assertEqual(fake_overlay.start_calls, 0)
        self.assertEqual(fake_overlay.passthrough_calls, 1)

    @unittest.skipIf(overlay_mod.Image is None, "PIL is unavailable")
    def test_plugin_host_refresh_never_creates_legacy_overlay_window(self) -> None:
        overlays = [{
            "plugin_id": "plug",
            "surface": "unioverlay",
            "spec": normalize_ui_spec({
                "nodes": [
                    UI.canvas(20, 10, [UI.rect(0, 0, 20, 10, fill="accent")],
                              x=11, y=22, z=3, id="meter")
                ],
            }),
        }]

        class FakeLayer:
            def __init__(self, name, w, h, x, y, z, click_through=True):
                self.name = name
                self.geometry = (x, y, w, h)
                self.z_order = z
                self.click_through = click_through
                self.visible = False

            def show(self):
                self.visible = True

            def hide(self):
                self.visible = False

            def request_redraw(self):
                return None

            def set_geometry(self, x, y, w, h):
                self.geometry = (x, y, w, h)

            def set_input_callbacks(self, cursor_pos_fn=None, cursor_leave_fn=None,
                                    mouse_button_fn=None, scroll_fn=None):
                return None

            def create_input_proxy(self, _root):
                raise AssertionError("plugin host should not create a Tk proxy")

            def sync_input_proxy(self):
                return None

            def destroy_input_proxy(self):
                return None

        class FakeOverlay:
            def __init__(self):
                self._running = True
                self.created = []
                self.passthrough_calls = 0

            def create_layer(self, name, w, h, x, y, z, click_through=True, bgra_swizzle=True):
                layer = FakeLayer(name, w, h, x, y, z, click_through)
                self.created.append(layer)
                return layer

            def destroy_layer(self, _name):
                return None

            def force_host_input_passthrough(self):
                self.passthrough_calls += 1

        class FakePresenter:
            def __init__(self, layer):
                self.layer = layer
                self.frames = []

            def set_frame(self, bgra, w, h):
                self.frames.append((len(bgra), w, h))

        host = overlay_mod.PluginUnifiedOverlayHost(
            SimpleNamespace(root=object()), surface="unioverlay")
        fake_overlay = FakeOverlay()

        with mock.patch.object(overlay_mod, "get_unified_overlay", lambda _root: fake_overlay), \
             mock.patch.object(overlay_mod, "CompositorBgraPresenter", FakePresenter), \
             mock.patch.object(overlay_mod, "render_overlays",
                               lambda _owner, _surface: {"overlays": overlays}), \
             mock.patch.object(overlay_adapter_mod, "create_overlay_window",
                               side_effect=AssertionError("legacy overlay window must not be created")), \
             mock.patch("render.gpu_overlay_window.GpuOverlayWindow",
                        side_effect=AssertionError("GpuOverlayWindow must not be created")):
            host._refresh_now()

        self.assertEqual(len(fake_overlay.created), 1)
        self.assertEqual(fake_overlay.created[0].geometry, (11, 22, 20, 10))
        self.assertTrue(fake_overlay.created[0].visible)
        self.assertGreater(fake_overlay.passthrough_calls, 0)

    def test_unified_overlay_host_input_mode_follows_interactive_layers(self) -> None:
        from render.overlay_compositor import UnifiedOverlay

        class FakeHost:
            def __init__(self):
                self.passthrough = []

            def set_input_passthrough(self, enabled):
                self.passthrough.append(bool(enabled))

        overlay = UnifiedOverlay(root=None)
        overlay._host = FakeHost()
        layer = overlay.create_layer(
            "drag_model", width=40, height=30, x=10, y=20, z=5,
            click_through=False)

        overlay.sync_host_input_mode()
        overlay._cmd_q.get_nowait()()
        self.assertEqual(overlay._host.passthrough[-1], True)

        layer.show()
        overlay.sync_host_input_mode()
        overlay._cmd_q.get_nowait()()
        self.assertEqual(overlay._host.passthrough[-1], False)

        layer.hide()
        overlay.sync_host_input_mode()
        overlay._cmd_q.get_nowait()()
        self.assertEqual(overlay._host.passthrough[-1], True)

    def test_clean_tick_idles_until_next_invalidate(self) -> None:
        class FakeRoot:
            def __init__(self):
                self.after_calls = []

            def after(self, delay, callback):
                self.after_calls.append((delay, callback))
                return f"after_{len(self.after_calls)}"

        root = FakeRoot()
        host = overlay_mod.PluginUnifiedOverlayHost(
            SimpleNamespace(root=root), surface="unioverlay")

        with mock.patch.object(host, "_refresh_now") as refresh:
            host._tick()

        refresh.assert_called_once()
        self.assertEqual(root.after_calls, [])

        host.mark_dirty()

        self.assertEqual(len(root.after_calls), 1)
        self.assertEqual(root.after_calls[0][0], 0)

    def test_hidden_host_defers_invalidate_refresh_until_restore(self) -> None:
        class FakeRoot:
            def __init__(self):
                self.after_calls = []

            def after(self, delay, callback):
                self.after_calls.append((delay, callback))
                return f"after_{len(self.after_calls)}"

        class FakeBus:
            def __init__(self):
                self.callbacks = {}

            def subscribe(self, topic, callback, owner_id=""):
                self.callbacks[str(topic)] = callback
                return f"token_{topic}"

        root = FakeRoot()
        bus = FakeBus()
        host = overlay_mod.PluginUnifiedOverlayHost(
            SimpleNamespace(root=root), surface="unioverlay")
        host.hide_temporarily()

        with mock.patch.object(overlay_mod, "ensure_act_event_bus", lambda _owner: bus):
            host._subscribe()

        bus.callbacks["plugin_ui_invalidate"]({
            "payload": {"surface": "unioverlay", "reason": "overlay_set"}
        })
        self.assertTrue(host._dirty)
        self.assertEqual(root.after_calls, [])

        host.restore()

        self.assertEqual(len(root.after_calls), 1)
        self.assertEqual(root.after_calls[0][0], 0)

    def test_hidden_host_skips_pending_tick_until_restore(self) -> None:
        class FakeRoot:
            def __init__(self):
                self.after_calls = []

            def after(self, delay, callback):
                self.after_calls.append((delay, callback))
                return f"after_{len(self.after_calls)}"

        root = FakeRoot()
        host = overlay_mod.PluginUnifiedOverlayHost(
            SimpleNamespace(root=root), surface="unioverlay")
        host.mark_dirty()
        self.assertEqual(len(root.after_calls), 1)
        host.hide_temporarily()

        with mock.patch.object(host, "_refresh_now") as refresh:
            root.after_calls[0][1]()

        refresh.assert_not_called()
        self.assertTrue(host._dirty)
        host.restore()
        self.assertEqual(len(root.after_calls), 2)
        self.assertEqual(root.after_calls[1][0], 0)

    def test_tick_reschedules_if_refresh_marks_dirty_again(self) -> None:
        class FakeRoot:
            def __init__(self):
                self.after_calls = []

            def after(self, delay, callback):
                self.after_calls.append((delay, callback))
                return f"after_{len(self.after_calls)}"

        root = FakeRoot()
        host = overlay_mod.PluginUnifiedOverlayHost(
            SimpleNamespace(root=root), surface="unioverlay")

        def _refresh() -> None:
            host._dirty = True

        with mock.patch.object(host, "_refresh_now", side_effect=_refresh):
            host._tick()

        self.assertEqual(len(root.after_calls), 1)
        self.assertEqual(root.after_calls[0][0], 0)

    def test_drawables_preserve_keys_geometry_and_order(self) -> None:
        drawables = overlay_mod._iter_layer_drawables(self._sample_overlays())

        self.assertEqual([d["kind"] for d in drawables], ["canvas", "model3d"])
        self.assertEqual(drawables[0]["key"], "canvas:plug/meter")
        self.assertEqual(drawables[0]["x"], 11)
        self.assertEqual(drawables[0]["y"], 22)
        self.assertEqual(drawables[0]["z"], 3)
        self.assertEqual(drawables[1]["key"], "model3d:plug/avatar")
        self.assertEqual(drawables[1]["x"], 44)
        self.assertEqual(drawables[1]["y"], 55)
        self.assertEqual(drawables[1]["z"], 8)

    def test_model3d_drawable_key_does_not_probe_backend_or_touch_files(self) -> None:
        with mock.patch.object(
            overlay_mod,
            "diagnose_model3d_node",
            side_effect=AssertionError("key path should not diagnose"),
        ), mock.patch.object(
            overlay_mod,
            "probe_model3d_backend",
            side_effect=AssertionError("key path should not probe"),
        ):
            drawables = overlay_mod._iter_layer_drawables(self._sample_overlays())

        self.assertEqual(drawables[1]["key"], "model3d:plug/avatar")

    @unittest.skipIf(overlay_mod.Image is None, "PIL is unavailable")
    def test_model3d_successful_render_signature_does_not_diagnose(self) -> None:
        drawables = overlay_mod._iter_layer_drawables(self._sample_overlays())
        image = overlay_mod.Image.new("RGBA", (40, 30), (1, 2, 3, 4))
        with mock.patch.object(
            overlay_mod,
            "render_model3d_node",
            return_value=image,
        ), mock.patch.object(
            overlay_mod,
            "diagnose_model3d_node",
            side_effect=AssertionError("successful render should not diagnose"),
        ):
            frame = overlay_mod._render_drawable_frame(
                drawables[1], overlay_mod._pal(), probe_model3d_backend())

        self.assertIsNotNone(frame)

    @unittest.skipIf(overlay_mod.Image is None, "PIL is unavailable")
    def test_model3d_diagnostic_frame_is_visible_bgra(self) -> None:
        drawables = overlay_mod._iter_layer_drawables(self._sample_overlays())
        frame = overlay_mod._render_drawable_frame(
            drawables[1], overlay_mod._pal(), probe_model3d_backend())

        self.assertIsNotNone(frame)
        signature, bgra, width, height = frame
        self.assertIn("model3d", signature)
        self.assertEqual((width, height), (40, 30))
        self.assertEqual(len(bgra), width * height * 4)
        self.assertTrue(any(bgra[idx] for idx in range(3, len(bgra), 4)))

    @unittest.skipIf(overlay_mod.Image is None, "PIL is unavailable")
    def test_host_refresh_creates_positioned_keyed_layers_and_clears_stale(self) -> None:
        class FakeLayer:
            def __init__(self, name, w, h, x, y, z, click_through=True):
                self.name = name
                self.geometry = (x, y, w, h)
                self.z_order = z
                self.click_through = click_through
                self.destroyed = False
                self.visible = False
                self.cursor_pos_fn = None
                self.mouse_button_fn = None

            def show(self):
                self.visible = True

            def hide(self):
                self.visible = False

            def request_redraw(self):
                self.redrawn = True

            def set_geometry(self, x, y, w, h):
                self.geometry = (x, y, w, h)

            def set_input_callbacks(self, cursor_pos_fn=None, cursor_leave_fn=None,
                                    mouse_button_fn=None, scroll_fn=None):
                self.cursor_pos_fn = cursor_pos_fn
                self.mouse_button_fn = mouse_button_fn

            def create_input_proxy(self, _root):
                self.proxy_created = not self.click_through

            def sync_input_proxy(self):
                self.proxy_synced = True

            def destroy_input_proxy(self):
                self.proxy_destroyed = True

        class FakeOverlay:
            def __init__(self):
                self._running = True
                self.created = []
                self.destroyed = []
                self.passthrough_calls = 0

            def create_layer(self, name, w, h, x, y, z, click_through=True, bgra_swizzle=True):
                layer = FakeLayer(name, w, h, x, y, z, click_through)
                self.created.append(layer)
                return layer

            def destroy_layer(self, name):
                self.destroyed.append(name)
                for layer in self.created:
                    if layer.name == name:
                        layer.destroyed = True

            def set_layer_z(self, name, z):
                for layer in self.created:
                    if layer.name == name:
                        layer.z_order = z

            def force_host_input_passthrough(self):
                self.passthrough_calls += 1

        class FakePresenter:
            def __init__(self, layer):
                self.layer = layer
                self.frames = []

            def set_frame(self, bgra, w, h):
                self.frames.append((len(bgra), w, h))

        host = overlay_mod.PluginUnifiedOverlayHost(
            SimpleNamespace(root=object()), surface="unioverlay")
        fake_overlay = FakeOverlay()

        with mock.patch.object(overlay_mod, "get_unified_overlay", lambda _root: fake_overlay), \
             mock.patch.object(overlay_mod, "CompositorBgraPresenter", FakePresenter), \
             mock.patch.object(overlay_mod, "render_overlays",
                               lambda _owner, _surface: {"overlays": self._sample_overlays()}):
            host._refresh_now()

        self.assertEqual(len(fake_overlay.created), 2)
        self.assertEqual(fake_overlay.created[0].geometry, (11, 22, 20, 10))
        self.assertIs(fake_overlay.created[0].click_through, True)
        self.assertEqual(fake_overlay.created[0].z_order, overlay_mod._BASE_Z + 3)
        self.assertEqual(fake_overlay.created[1].geometry, (44, 55, 40, 30))
        self.assertIs(fake_overlay.created[1].click_through, False)
        self.assertIsNotNone(fake_overlay.created[1].cursor_pos_fn)
        self.assertIsNotNone(fake_overlay.created[1].mouse_button_fn)
        self.assertFalse(hasattr(fake_overlay.created[1], "proxy_created"))
        self.assertEqual(fake_overlay.created[1].z_order, overlay_mod._BASE_Z + 8)
        self.assertGreater(fake_overlay.passthrough_calls, 0)
        self.assertEqual(set(host._layers), {"canvas:plug/meter", "model3d:plug/avatar"})

        with mock.patch.object(overlay_mod, "render_overlays",
                               lambda _owner, _surface: {"overlays": []}):
            host._refresh_now()

        self.assertFalse(host._layers)
        self.assertTrue(all(layer.destroyed for layer in fake_overlay.created))

    def test_host_lifecycle_unload_event_destroys_plugin_layers_immediately(self) -> None:
        class FakeLayer:
            def destroy_input_proxy(self):
                self.proxy_destroyed = True

        class FakeOverlay:
            def __init__(self):
                self.destroyed = []

            def destroy_layer(self, name):
                self.destroyed.append(name)

            def force_host_input_passthrough(self):
                return None

        fake_overlay = FakeOverlay()
        host = overlay_mod.PluginUnifiedOverlayHost(
            SimpleNamespace(root=object()), surface="unioverlay")
        host._overlay = fake_overlay
        host._layers = {
            "canvas:plug/meter": {"layer": FakeLayer(), "layer_name": "plugin canvas plug meter"},
            "model3d:other/avatar": {"layer": FakeLayer(), "layer_name": "plugin model other avatar"},
        }

        host._handle_plugin_lifecycle({
            "payload": {
                "plugin_id": "plug",
                "action": "unloaded",
                "surfaces": ["unioverlay"],
            }
        })

        self.assertEqual(set(host._layers), {"model3d:other/avatar"})
        self.assertEqual(fake_overlay.destroyed, ["plugin canvas plug meter"])
        self.assertTrue(host._dirty)

    def test_host_destroying_last_model3d_layer_resets_native_resources(self) -> None:
        class FakeLayer:
            def __init__(self):
                self.proxy_destroyed = False

            def destroy_input_proxy(self):
                self.proxy_destroyed = True

        class FakeOverlay:
            def __init__(self):
                self.destroyed = []

            def destroy_layer(self, name):
                self.destroyed.append(name)

            def force_host_input_passthrough(self):
                return None

        fake_overlay = FakeOverlay()
        host = overlay_mod.PluginUnifiedOverlayHost(
            SimpleNamespace(root=object()), surface="unioverlay")
        host._overlay = fake_overlay
        host._layers = {
            "canvas:plug/meter": {"layer": FakeLayer(), "layer_name": "plugin canvas plug meter"},
            "model3d:plug/avatar": {"layer": FakeLayer(), "layer_name": "plugin model plug avatar"},
            "model3d:other/avatar": {"layer": FakeLayer(), "layer_name": "plugin model other avatar"},
        }

        with mock.patch.object(
            overlay_mod,
            "reset_native_model3d_resources",
            return_value={"ok": True},
        ) as reset:
            host._destroy_layer("canvas:plug/meter")
            self.assertEqual(reset.call_count, 0)

            host._destroy_layer("model3d:plug/avatar")
            self.assertEqual(reset.call_count, 0)

            host._destroy_layer("model3d:other/avatar")
            self.assertEqual(reset.call_count, 1)

        self.assertFalse(host._layers)
        self.assertEqual(fake_overlay.destroyed, [
            "plugin canvas plug meter",
            "plugin model plug avatar",
            "plugin model other avatar",
        ])

    @unittest.skipIf(overlay_mod.Image is None, "PIL is unavailable")
    def test_canvas_only_noop_refresh_skips_model_probe_and_layer_churn(self) -> None:
        overlays = [{
            "plugin_id": "plug",
            "surface": "unioverlay",
            "spec": normalize_ui_spec({
                "nodes": [
                    UI.canvas(20, 10, [UI.rect(0, 0, 20, 10, fill="accent")],
                              x=11, y=22, z=3, id="meter")
                ],
            }),
        }]

        class FakeLayer:
            def __init__(self, name, w, h, x, y, z, click_through=True):
                self.name = name
                self.geometry = (x, y, w, h)
                self.z_order = z
                self.click_through = click_through
                self.visible = False
                self.geometry_calls = 0
                self.redraws = 0
                self.syncs = 0
                self.shows = 0

            def show(self):
                self.visible = True
                self.shows += 1

            def hide(self):
                self.visible = False

            def request_redraw(self):
                self.redraws += 1

            def set_geometry(self, x, y, w, h):
                self.geometry = (x, y, w, h)
                self.geometry_calls += 1

            def set_input_callbacks(self, cursor_pos_fn=None, cursor_leave_fn=None,
                                    mouse_button_fn=None, scroll_fn=None):
                return None

            def create_input_proxy(self, _root):
                return None

            def sync_input_proxy(self):
                self.syncs += 1

            def destroy_input_proxy(self):
                return None

        class FakeOverlay:
            def __init__(self):
                self._running = True
                self.created = []

            def create_layer(self, name, w, h, x, y, z, click_through=True, bgra_swizzle=True):
                layer = FakeLayer(name, w, h, x, y, z, click_through)
                self.created.append(layer)
                return layer

            def destroy_layer(self, name):
                return None

            def force_host_input_passthrough(self):
                return None

        class FakePresenter:
            def __init__(self, layer):
                self.layer = layer
                self.frames = []
                layer.presenter = self

            def set_frame(self, bgra, w, h):
                self.frames.append((len(bgra), w, h))

        host = overlay_mod.PluginUnifiedOverlayHost(
            SimpleNamespace(root=object()), surface="unioverlay")
        fake_overlay = FakeOverlay()

        with mock.patch.object(overlay_mod, "get_unified_overlay", lambda _root: fake_overlay), \
             mock.patch.object(overlay_mod, "CompositorBgraPresenter", FakePresenter), \
             mock.patch.object(overlay_mod, "render_overlays",
                               lambda _owner, _surface: {"overlays": overlays}), \
             mock.patch.object(overlay_mod, "probe_model3d_backend",
                               side_effect=AssertionError("canvas refresh should not probe model3d backend")):
            host._refresh_now()
            layer = fake_overlay.created[0]
            first_counts = (
                layer.geometry_calls,
                layer.redraws,
                layer.syncs,
                layer.shows,
                len(layer.presenter.frames),
            )
            host._refresh_now()

        self.assertEqual(layer.geometry, (11, 22, 20, 10))
        self.assertEqual(layer.geometry_calls, first_counts[0])
        self.assertEqual(layer.redraws, first_counts[1])
        self.assertEqual(layer.syncs, first_counts[2])
        self.assertEqual(layer.shows, first_counts[3])
        self.assertEqual(len(layer.presenter.frames), first_counts[4])

    @unittest.skipIf(overlay_mod.Image is None, "PIL is unavailable")
    def test_unchanged_layer_skips_rasterize_until_spec_changes(self) -> None:
        current_spec = {
            "nodes": [
                UI.canvas(20, 10, [UI.rect(0, 0, 20, 10, fill="accent")],
                          x=11, y=22, z=3, id="meter")
            ],
        }

        class FakeLayer:
            def __init__(self, name, w, h, x, y, z, click_through=True):
                self.name = name
                self.geometry = (x, y, w, h)
                self.z_order = z
                self.click_through = click_through
                self.redraws = 0
                self.syncs = 0
                self.visible = False

            def show(self):
                self.visible = True

            def hide(self):
                self.visible = False

            def request_redraw(self):
                self.redraws += 1

            def set_geometry(self, x, y, w, h):
                self.geometry = (x, y, w, h)

            def sync_input_proxy(self):
                self.syncs += 1

            def destroy_input_proxy(self):
                return None

        class FakeOverlay:
            def __init__(self):
                self.created = []

            def create_layer(self, name, w, h, x, y, z, click_through=True, bgra_swizzle=True):
                layer = FakeLayer(name, w, h, x, y, z, click_through)
                self.created.append(layer)
                return layer

            def destroy_layer(self, name):
                return None

            def force_host_input_passthrough(self):
                return None

        class FakePresenter:
            def __init__(self, layer):
                self.layer = layer
                self.frames = []

            def set_frame(self, bgra, w, h):
                self.frames.append((len(bgra), w, h))

        def _overlays(_owner, _surface):
            return {"overlays": [{
                "plugin_id": "plug",
                "surface": "unioverlay",
                "spec": normalize_ui_spec(current_spec),
            }]}

        render_calls = []

        def _render(drawable, pal, backend_status=None):
            render_calls.append(str(drawable.get("key") or ""))
            width = int(drawable.get("width") or 1)
            height = int(drawable.get("height") or 1)
            return f"frame-{len(render_calls)}", bytes(width * height * 4), width, height

        host = overlay_mod.PluginUnifiedOverlayHost(
            SimpleNamespace(root=object()), surface="unioverlay")
        fake_overlay = FakeOverlay()

        with mock.patch.object(overlay_mod, "get_unified_overlay", lambda _root: fake_overlay), \
             mock.patch.object(overlay_mod, "CompositorBgraPresenter", FakePresenter), \
             mock.patch.object(overlay_mod, "render_overlays", _overlays), \
             mock.patch.object(overlay_mod, "_render_drawable_frame", side_effect=_render):
            host._refresh_now()
            first_redraws = fake_overlay.created[0].redraws
            host._refresh_now()
            current_spec = {
                "nodes": [
                    UI.canvas(20, 10, [UI.rect(0, 0, 20, 10, fill="accent")],
                              x=31, y=42, z=5, id="meter")
                ],
            }
            host._refresh_now()
            self.assertEqual(fake_overlay.created[0].geometry, (31, 42, 20, 10))
            self.assertGreater(fake_overlay.created[0].redraws, first_redraws)
            current_spec = {
                "nodes": [
                    UI.canvas(20, 10, [UI.rect(0, 0, 20, 10, fill="gold")],
                              x=31, y=42, z=5, id="meter")
                ],
            }
            host._refresh_now()

        self.assertEqual(render_calls, ["canvas:plug/meter", "canvas:plug/meter"])
        self.assertEqual(len(fake_overlay.created), 1)

    @unittest.skipIf(overlay_mod.Image is None, "PIL is unavailable")
    def test_model3d_layer_drag_updates_position_without_affecting_canvas_clickthrough(self) -> None:
        class FakeLayer:
            def __init__(self, name, w, h, x, y, z, click_through=True):
                self.name = name
                self.geometry = (x, y, w, h)
                self.z_order = z
                self.click_through = click_through
                self.cursor_pos_fn = None
                self.mouse_button_fn = None

            def show(self):
                self.visible = True

            def hide(self):
                self.visible = False

            def request_redraw(self):
                self.redrawn = True

            def set_geometry(self, x, y, w, h):
                self.geometry = (x, y, w, h)

            def set_input_callbacks(self, cursor_pos_fn=None, cursor_leave_fn=None,
                                    mouse_button_fn=None, scroll_fn=None):
                self.cursor_pos_fn = cursor_pos_fn
                self.mouse_button_fn = mouse_button_fn

            def create_input_proxy(self, _root):
                self.proxy_created = not self.click_through

            def sync_input_proxy(self):
                self.proxy_synced = True

            def destroy_input_proxy(self):
                self.proxy_destroyed = True

        class FakeOverlay:
            def __init__(self):
                self._running = True
                self.created = []

            def create_layer(self, name, w, h, x, y, z, click_through=True, bgra_swizzle=True):
                layer = FakeLayer(name, w, h, x, y, z, click_through)
                self.created.append(layer)
                return layer

            def destroy_layer(self, name):
                return None

            def set_layer_z(self, name, z):
                return None

            def force_host_input_passthrough(self):
                return None

        class FakePresenter:
            def __init__(self, layer):
                self.layer = layer

            def set_frame(self, bgra, w, h):
                return None

        host = overlay_mod.PluginUnifiedOverlayHost(
            SimpleNamespace(root=object()), surface="unioverlay")
        fake_overlay = FakeOverlay()

        with mock.patch.object(overlay_mod, "get_unified_overlay", lambda _root: fake_overlay), \
             mock.patch.object(overlay_mod, "CompositorBgraPresenter", FakePresenter), \
             mock.patch.object(overlay_mod, "render_overlays",
                               lambda _owner, _surface: {"overlays": self._sample_overlays()}), \
             mock.patch.object(overlay_mod, "_cursor_screen_pos",
                               side_effect=[(100, 100), (130, 150), (140, 160)]), \
             mock.patch("act_platform.runtime.act_plugin_action") as action:
            host._refresh_now()
            self.assertEqual(fake_overlay.created[0].geometry, (11, 22, 20, 10))
            self.assertIs(fake_overlay.created[0].click_through, True)
            self.assertIs(fake_overlay.created[1].click_through, False)
            self.assertFalse(hasattr(fake_overlay.created[1], "proxy_created"))
            fake_overlay.created[1].mouse_button_fn(0, 1, 0, 10.0, 20.0)
            fake_overlay.created[1].cursor_pos_fn(40.0, 70.0)
            fake_overlay.created[1].mouse_button_fn(0, 0, 0, 40.0, 70.0)
            self.assertEqual(fake_overlay.created[1].geometry, (74, 105, 40, 30))
            self.assertEqual(action.call_count, 4)
            dispatched = [(call.args[1], call.args[2], call.kwargs) for call in action.call_args_list]
            self.assertEqual(
                [item[0] for item in dispatched],
                [
                    "script.overlay.pointer",
                    "script.overlay.pointer",
                    "script.overlay.pointer",
                    "script.overlay.set_position",
                ],
            )
            self.assertEqual([item[1].get("event") for item in dispatched[:3]], ["press", "drag", "release"])
            args, kwargs = action.call_args
            self.assertEqual(args[1], "script.overlay.set_position")
            self.assertEqual(kwargs.get("plugin_id"), "plug")
            self.assertEqual(args[2]["x"], 74)
            self.assertEqual(args[2]["y"], 105)
            action.reset_mock()
            fake_overlay.created[1].mouse_button_fn(0, 1, 0, 12.0, 22.0)
            fake_overlay.created[1].mouse_button_fn(0, 0, 0, 12.0, 22.0)
            self.assertEqual(action.call_count, 3)
            dispatched = [(call.args[1], call.args[2]) for call in action.call_args_list]
            self.assertEqual(
                [item[0] for item in dispatched],
                ["script.overlay.pointer", "script.overlay.pointer", "script.overlay.set_position"],
            )
            self.assertEqual([item[1].get("event") for item in dispatched[:2]], ["press", "click"])

        with mock.patch.object(overlay_mod, "render_overlays",
                               lambda _owner, _surface: {"overlays": self._sample_overlays()}):
            host._refresh_now()

        self.assertEqual(fake_overlay.created[1].geometry, (74, 105, 40, 30))


if __name__ == "__main__":
    unittest.main(verbosity=2)
