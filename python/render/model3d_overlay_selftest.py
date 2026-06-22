# -*- coding: utf-8 -*-
"""Focused selftests for plugin unioverlay canvas/model3d support."""
from __future__ import annotations

import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


_PY_ROOT = Path(__file__).resolve().parents[1]
if str(_PY_ROOT) not in sys.path:
    sys.path.insert(0, str(_PY_ROOT))

from act_platform.ui_spec import MAX_LAYER_POS, MAX_LAYER_Z, UI, normalize_ui_spec
from gui_modules import sao_plugin_unified_overlay as overlay_mod
from render import model3d_backend
from render.model3d_backend import (
    clear_model3d_metadata_caches,
    diagnose_model3d_node,
    evaluate_retarget_pose,
    get_action_metadata,
    get_backend_status,
    get_model_metadata,
    get_retarget_plan,
    probe_model3d_backend,
)
from render.model3d_native import (
    clear_native_model3d_renderers,
    native_model3d_renderer_status,
    register_native_model3d_renderer,
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
        })["nodes"][0]
        self.assertIs(fixed["draggable"], False)

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
        status = probe_model3d_backend()
        self.assertEqual(status.backend, "assimpnet")
        self.assertFalse(status.render_available)
        self.assertTrue(status.reason)

        key, lines = diagnose_model3d_node("plug", {
            "type": "model3d",
            "id": "avatar",
            "model": {"path": ""},
        }, status=status)
        self.assertEqual(key, "plug/avatar")
        self.assertTrue(any("model path is empty" in line for line in lines))

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
            self.assertEqual(context["presentation"], "existing_compositor_layer")
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
        self.assertAlmostEqual(pose["positions"]["right_hand"][1], 0.62, places=4)

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
                    "preserve_proportions": False,
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
                               side_effect=[(100, 100), (130, 150)]), \
             mock.patch("act_platform.runtime.act_plugin_action") as action:
            host._refresh_now()
            self.assertEqual(fake_overlay.created[0].geometry, (11, 22, 20, 10))
            self.assertIs(fake_overlay.created[0].click_through, True)
            self.assertIs(fake_overlay.created[1].click_through, False)
            fake_overlay.created[1].mouse_button_fn(0, 1, 0, 10.0, 20.0)
            fake_overlay.created[1].cursor_pos_fn(40.0, 70.0)
            fake_overlay.created[1].mouse_button_fn(0, 0, 0, 40.0, 70.0)
            self.assertEqual(fake_overlay.created[1].geometry, (74, 105, 40, 30))
            action.assert_called_once()
            args, kwargs = action.call_args
            self.assertEqual(args[1], "script.overlay.set_position")
            self.assertEqual(kwargs.get("plugin_id"), "plug")
            self.assertEqual(args[2]["x"], 74)
            self.assertEqual(args[2]["y"], 105)

        with mock.patch.object(overlay_mod, "render_overlays",
                               lambda _owner, _surface: {"overlays": self._sample_overlays()}):
            host._refresh_now()

        self.assertEqual(fake_overlay.created[1].geometry, (74, 105, 40, 30))


if __name__ == "__main__":
    unittest.main(verbosity=2)
