# -*- coding: utf-8 -*-
"""Focused selftests for plugin unioverlay canvas/model3d support."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


_PY_ROOT = Path(__file__).resolve().parents[1]
if str(_PY_ROOT) not in sys.path:
    sys.path.insert(0, str(_PY_ROOT))

from act_platform.ui_spec import MAX_LAYER_POS, MAX_LAYER_Z, UI, normalize_ui_spec
from gui_modules import sao_plugin_unified_overlay as overlay_mod
from render.model3d_backend import diagnose_model3d_node, probe_model3d_backend


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
        })["nodes"][0]
        self.assertEqual(animated["phase"], 12.5)

        fixed = normalize_ui_spec({
            "type": "model3d",
            "id": "fixed",
            "draggable": False,
        })["nodes"][0]
        self.assertIs(fixed["draggable"], False)


class Model3DBackendTests(unittest.TestCase):
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
                               side_effect=[(100, 100), (130, 150)]):
            host._refresh_now()
            self.assertEqual(fake_overlay.created[0].geometry, (11, 22, 20, 10))
            self.assertIs(fake_overlay.created[0].click_through, True)
            self.assertIs(fake_overlay.created[1].click_through, False)
            fake_overlay.created[1].mouse_button_fn(0, 1, 0, 10.0, 20.0)
            fake_overlay.created[1].cursor_pos_fn(40.0, 70.0)
            fake_overlay.created[1].mouse_button_fn(0, 0, 0, 40.0, 70.0)
            self.assertEqual(fake_overlay.created[1].geometry, (74, 105, 40, 30))

        with mock.patch.object(overlay_mod, "render_overlays",
                               lambda _owner, _surface: {"overlays": self._sample_overlays()}):
            host._refresh_now()

        self.assertEqual(fake_overlay.created[1].geometry, (74, 105, 40, 30))


if __name__ == "__main__":
    unittest.main(verbosity=2)
