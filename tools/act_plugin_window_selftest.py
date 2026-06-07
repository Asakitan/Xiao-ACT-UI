# -*- coding: utf-8 -*-
"""Regression coverage for plugin panel windows: declared size + open_window.

A plugin declares per-panel window size on ``register_ui_panel`` (width/height/
min_*), and asks the host to pop a panel into its own window via
``ctx.open_window(panel_id)`` (emits ``plugin_open_window``). Host renderers use
the declared size and fall back to a default when absent.
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from act_platform.event_bus import EventBus
from act_platform.plugins import PluginManager
from act_platform.ui_spec import UI, normalize_ui_spec


PLUGIN_CODE = r'''
def on_load(ctx):
    def _act(a, p):
        if a == "pop":
            ctx.open_window("sub")
        elif a == "redraw":
            ctx.request_redraw("sub")     # targeted redraw (host filters by surface)
    ctx.register_ui_panel(
        "main", {"title": "Main", "width": 480, "height": 640, "min_width": 360},
        render=lambda p: ctx.ui.panel("Main", []), on_action=_act)
    ctx.register_ui_panel(
        "sub", {"title": "Sub", "width": 300, "height": 200, "hidden": True},
        render=lambda p: ctx.ui.panel("Sub", []))
    ctx.register_ui_panel(
        "nosize", {"title": "NoSize"},
        render=lambda p: ctx.ui.panel("NoSize", []))
'''


def _make_manager(root: str) -> PluginManager:
    d = os.path.join(root, "windemo")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "plugin.json"), "w", encoding="utf-8") as fp:
        json.dump({"id": "windemo", "name": "Win Demo", "entry": "plugin.py",
                   "capabilities": ["ui_panels"]}, fp)
    with open(os.path.join(d, "plugin.py"), "w", encoding="utf-8") as fp:
        fp.write(PLUGIN_CODE)
    mgr = PluginManager(plugin_dirs=[root], event_bus=EventBus())
    mgr.discover()
    return mgr


class PluginWindowTests(unittest.TestCase):
    def test_declared_panel_size_flows_to_meta(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pw_size_") as root:
            mgr = _make_manager(root)
            self.assertTrue(mgr.enable_plugin("windemo"), mgr._records["windemo"].last_error)
            panels = {p["id"]: p for p in mgr.list_ui_panels()}
            self.assertEqual((panels["main"]["width"], panels["main"]["height"]), (480, 640))
            self.assertEqual(panels["main"]["min_width"], 360)
            self.assertEqual((panels["sub"]["width"], panels["sub"]["height"]), (300, 200))
            # A panel that declares no size carries no width/height → host defaults.
            self.assertNotIn("width", panels["nosize"])
            self.assertNotIn("height", panels["nosize"])

    def test_hidden_subpanel_is_registered_but_flagged(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pw_hidden_") as root:
            mgr = _make_manager(root)
            mgr.enable_plugin("windemo")
            panels = {p["id"]: p for p in mgr.list_ui_panels()}
            # 'sub' is hidden (summoned via open_window, not listed) but still
            # registered + renderable; 'main'/'nosize' are visible entries.
            self.assertTrue(panels["sub"].get("hidden"))
            self.assertFalse(panels["main"].get("hidden", False))
            visible = [p for p in mgr.list_ui_panels() if not p.get("hidden")]
            self.assertEqual({p["id"] for p in visible}, {"main", "nosize"})
            # Hidden panel still renders when opened directly.
            self.assertTrue(mgr.render_ui_panel("sub").get("ok"))

    def test_open_window_emits_event(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pw_open_") as root:
            mgr = _make_manager(root)
            mgr.enable_plugin("windemo")
            captured = []
            mgr.event_bus.subscribe("plugin_open_window",
                                    lambda e: captured.append(e.get("payload")),
                                    owner_id="test")
            mgr.invoke_ui_action("main", "pop", {})
            self.assertTrue(captured)
            self.assertEqual(captured[0]["plugin_id"], "windemo")
            self.assertEqual(captured[0]["panel_id"], "sub")

    def test_request_redraw_carries_surface(self) -> None:
        # The host renderers filter plugin_ui_invalidate by surface so animating
        # one panel doesn't rebuild interactive panels in other windows.
        with tempfile.TemporaryDirectory(prefix="pw_redraw_") as root:
            mgr = _make_manager(root)
            mgr.enable_plugin("windemo")
            got = []
            mgr.event_bus.subscribe("plugin_ui_invalidate",
                                    lambda e: got.append(e.get("payload")),
                                    owner_id="test")
            mgr.invoke_ui_action("main", "redraw", {})
            self.assertTrue(any(p.get("surface") == "sub" for p in got), got)

    def test_negative_size_clamped(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pw_clamp_") as root:
            d = os.path.join(root, "clampdemo")
            os.makedirs(d)
            with open(os.path.join(d, "plugin.json"), "w", encoding="utf-8") as fp:
                json.dump({"id": "clampdemo", "entry": "plugin.py", "capabilities": ["ui_panels"]}, fp)
            with open(os.path.join(d, "plugin.py"), "w", encoding="utf-8") as fp:
                fp.write('def on_load(ctx):\n'
                         '    ctx.register_ui_panel("p", {"title":"P","width":-5,"height":"oops"},'
                         ' render=lambda x: ctx.ui.panel("P", []))\n')
            mgr = PluginManager(plugin_dirs=[root], event_bus=EventBus())
            mgr.discover()
            mgr.enable_plugin("clampdemo")
            meta = {p["id"]: p for p in mgr.list_ui_panels()}["p"]
            self.assertEqual(meta.get("width"), 0)        # -5 clamped to 0 → host default
            self.assertNotIn("height", meta)              # non-numeric dropped


class CanvasSpecTests(unittest.TestCase):
    def test_canvas_normalizes_ops_and_clamps(self) -> None:
        cv = UI.canvas(810, 150, [
            UI.rect(0, 0, 16, 150, fill="white", outline="border", width=1),
            UI.line(0, 75, 810, 75, fill="sep", width=1),
            UI.ctext(8, 140, "C4", fill="label", size=8, anchor="s"),
            UI.rect(0, 0, 4, 4, fill="#dea620"),     # hex passthrough
            {"op": "bogus", "x": 1},                 # unknown op dropped
            UI.rect(0, 0, 1, 1, fill="notacolor"),   # bad token -> ""
        ], bg="body")
        node = normalize_ui_spec(UI.panel("K", [cv]))["nodes"][0]
        self.assertEqual(node["type"], "canvas")
        kinds = [o["op"] for o in node["ops"]]
        self.assertNotIn("bogus", kinds)
        self.assertEqual(kinds.count("rect"), 3)
        self.assertEqual(node["ops"][3]["fill"], "#dea620")   # hex kept
        self.assertEqual(node["ops"][-1]["fill"], "")          # bad color cleared

    def test_canvas_dimension_and_bg_clamp(self) -> None:
        node = normalize_ui_spec(UI.canvas(99999, -3, [], bg="zzz"))["nodes"][0]
        self.assertEqual(node["width"], 4096)   # clamped to MAX_CANVAS_DIM
        self.assertEqual(node["height"], 1)     # clamped to >=1
        self.assertEqual(node["bg"], "body")    # invalid bg -> default


if __name__ == "__main__":
    unittest.main(verbosity=2)
