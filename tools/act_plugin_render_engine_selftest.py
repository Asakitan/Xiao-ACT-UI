# -*- coding: utf-8 -*-
"""Regression coverage for the plugin render engine.

Covers the declarative UI spec, the render-hook + overlay registry, the
``ui_panels`` extension (render + action), plugin-unload cleanup, and the
bundled example plugins that exercise the new surface.
"""

from __future__ import annotations

import json
import os
import tempfile
import time
import unittest

from act_platform.event_bus import EventBus
from act_platform.plugins import PluginManager
from act_platform.runtime import act_plugin_menu
from act_platform.render_hooks import OVERRIDE_KEY, RenderHookRegistry
from act_platform.ui_spec import UI, normalize_ui_spec


def _write_plugin(plugin_dir: str, manifest: dict, code: str) -> None:
    os.makedirs(plugin_dir, exist_ok=True)
    with open(os.path.join(plugin_dir, "plugin.json"), "w", encoding="utf-8") as fp:
        json.dump(manifest, fp, ensure_ascii=False, indent=2)
    with open(os.path.join(plugin_dir, "plugin.py"), "w", encoding="utf-8") as fp:
        fp.write(code)


FIXTURE_SNAPSHOT = {
    "live": {
        "encounter_active": True,
        "total_damage": 9000000,
        "total_dps": 420000,
        "entities": [
            {"uid": 1, "name": "Kirito", "dps": 300000, "damage_total": 6000000, "damage_pct": 0.66, "is_self": True},
            {"uid": 2, "name": "Asuna", "dps": 120000, "damage_total": 3000000, "damage_pct": 0.34},
        ],
    },
    "render_spec": {
        "title": "Dungeon X",
        "totals": {"damage": 9000000, "dps": 420000, "heal": 50000, "elapsed_s": 21.0},
        "boss": {"active": True, "hp_pct": 0.42, "current_hp": 2100000, "total_hp": 5000000},
        "encounter": {"status": "active"},
        "rows": [
            {"rank": 1, "uid": 1, "name": "Kirito", "dps": 300000, "damage": 6000000, "damage_pct": 0.66, "is_self": True},
            {"rank": 2, "uid": 2, "name": "Asuna", "dps": 120000, "damage": 3000000, "damage_pct": 0.34},
        ],
    },
}


class UiSpecTests(unittest.TestCase):
    def test_builder_and_normalize(self) -> None:
        spec = UI.panel("Board", [
            UI.bar("HP", pct=2.0, color="bad", caption="x"),  # pct clamps to 1.0
            UI.kv("DPS", 1234),
            UI.row([UI.badge("LIVE", "ok"), UI.button("Go", action="go", style="primary")]),
            UI.table(columns=[{"key": "n", "title": "N"}], rows=[{"n": "a", "extra": "drop?"}], highlight_key="hi"),
        ])
        norm = normalize_ui_spec(spec)
        self.assertEqual(norm["title"], "Board")
        kinds = [n["type"] for n in norm["nodes"]]
        self.assertEqual(kinds, ["bar", "kv", "row", "table"])
        self.assertEqual(norm["nodes"][0]["pct"], 1.0)
        # table keeps only declared columns
        self.assertEqual(norm["nodes"][3]["rows"][0], {"n": "a"})

    def test_table_preserves_highlight_key(self) -> None:
        # Regression: the highlight flag must survive normalization even when it
        # is not one of the displayed columns, else row highlighting never fires.
        spec = UI.table(
            columns=[{"key": "name", "title": "N"}, {"key": "dps", "title": "DPS"}],
            rows=[{"name": "Me", "dps": "1M", "is_self": True},
                  {"name": "You", "dps": "2M", "is_self": False}],
            highlight_key="is_self")
        norm = normalize_ui_spec(spec)
        table = norm["nodes"][0]
        self.assertEqual(table["highlight_key"], "is_self")
        self.assertEqual(table["rows"][0].get("is_self"), True)
        self.assertEqual(table["rows"][1].get("is_self"), False)

    def test_unknown_nodes_dropped_and_json_safe(self) -> None:
        spec = {"nodes": [{"type": "bogus"}, {"type": "text", "text": "ok"}, "bare string"]}
        norm = normalize_ui_spec(spec)
        self.assertEqual([n["type"] for n in norm["nodes"]], ["text", "text"])
        json.dumps(norm)  # must be JSON-serializable

    def test_depth_and_node_bounds(self) -> None:
        # deeply nested container beyond MAX_DEPTH is truncated, never raises
        node = {"type": "text", "text": "deep"}
        for _ in range(40):
            node = {"type": "group", "children": [node]}
        norm = normalize_ui_spec(node)
        self.assertLessEqual(len(json.dumps(norm)), 20000)


class RenderHookRegistryTests(unittest.TestCase):
    def test_chain_priority_override_overlay_cleanup(self) -> None:
        reg = RenderHookRegistry()

        reg.register_hook("low", "dps", lambda s, p: {**p, "order": p.get("order", []) + ["low"]}, priority=1)
        reg.register_hook("high", "dps", lambda s, p: {**p, "order": p.get("order", []) + ["high"]}, priority=9)
        out = reg.apply("dps", {"order": []})
        self.assertEqual(out["order"], ["high", "low"])  # higher priority first

        reg.register_hook("all", "*", lambda s, p: {**p, "wild": True})
        self.assertTrue(reg.apply("anything", {})["wild"])

        def takeover(s, p):
            p[OVERRIDE_KEY] = {"type": "panel", "children": [{"type": "text", "text": "x"}]}
            return p
        reg.register_hook("to", "dps", takeover, priority=100)
        self.assertIn(OVERRIDE_KEY, reg.apply("dps", {}))

        reg.set_overlay("ov", "dps", UI.panel("O", [UI.text("hi")]))
        self.assertEqual(len(reg.overlays("dps")), 1)

        reg.unregister_plugin("ov")
        self.assertEqual(reg.overlays("dps"), [])
        reg.unregister_plugin("low")
        reg.unregister_plugin("high")
        reg.unregister_plugin("to")
        reg.unregister_plugin("all")
        self.assertFalse(reg.apply("dps", {"order": []}).get("order"))

    def test_non_dict_hook_result_preserves_payload(self) -> None:
        # Regression: a hook violating the dict|None contract must NOT corrupt
        # the payload into {"value": ...}; the prior payload is kept instead.
        reg = RenderHookRegistry()
        reg.register_hook("bad", "dps", lambda s, p: [1, 2, 3], priority=5)
        reg.register_hook("good", "dps", lambda s, p: {**p, "ok": True}, priority=1)
        out = reg.apply("dps", {"keep": 1})
        self.assertEqual(out.get("keep"), 1)
        self.assertTrue(out.get("ok"))
        self.assertNotIn("value", out)

    def test_hook_error_isolated(self) -> None:
        reg = RenderHookRegistry()
        seen = []
        reg.register_hook("bad", "dps", lambda s, p: (_ for _ in ()).throw(ValueError("boom")))
        reg.register_hook("good", "dps", lambda s, p: {**p, "ok": True})
        out = reg.apply("dps", {}, on_error=lambda pid, exc: seen.append(pid))
        self.assertTrue(out["ok"])
        self.assertEqual(seen, ["bad"])


PANEL_PLUGIN = '''
_ctx = None
def on_load(ctx):
    global _ctx
    _ctx = ctx
    ctx.register_ui_panel("demo", {"title": "Demo"}, render=_render, on_action=_action)
    ctx.register_render_hook("dps", _hook, priority=5)
    ctx.set_overlay("dps", ctx.ui.panel("OV", [ctx.ui.text("o")]))

def _render(payload):
    return _ctx.ui.panel("Demo", [_ctx.ui.kv("k", payload.get("k", "v")), _ctx.ui.button("Go", action="go")])

def _action(action_id, payload):
    if action_id == "go":
        _ctx.request_redraw("demo")
        return _ctx.ui.panel("Done", [_ctx.ui.text("done", style="ok")])
    return {"ok": False}

def _hook(surface, payload):
    payload["touched"] = True
    return payload
'''


class UiPanelTests(unittest.TestCase):
    def _manager(self, root):
        _write_plugin(
            os.path.join(root, "panel_demo"),
            {"id": "panel_demo", "name": "Panel Demo", "version": "1.0.0",
             "entry": "plugin.py", "enabled": True},
            PANEL_PLUGIN)
        m = PluginManager(plugin_dirs=[root])
        m.discover()
        self.assertTrue(m.load_plugin("panel_demo"), m.status()["plugins"][0]["last_error"])
        return m

    def test_render_action_and_redraw_event(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_ui_panel_") as root:
            m = self._manager(root)
            invalidated = []
            m.event_bus.subscribe("plugin_ui_invalidate", invalidated.append, owner_id="t")

            panels = m.list_ui_panels()
            self.assertEqual(panels[0]["id"], "demo")
            self.assertTrue(panels[0]["available"])
            self.assertTrue(panels[0]["has_actions"])

            r = m.render_ui_panel("demo", {"k": "hello"})
            self.assertTrue(r["ok"])
            self.assertEqual(r["spec"]["nodes"][0]["value"], "hello")

            a = m.invoke_ui_action("demo", "go")
            self.assertTrue(a["ok"])
            self.assertEqual(a["spec"]["title"], "Done")
            self.assertEqual(len(invalidated), 1)

            # render hook + overlay registered by the plugin
            self.assertTrue(m.apply_render_hooks("dps", {})["touched"])
            self.assertEqual(len(m.surface_overlays("dps")), 1)

    def test_unload_clears_hooks_overlays_actions(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_ui_panel_clr_") as root:
            m = self._manager(root)
            self.assertTrue(m.unload_plugin("panel_demo"))
            self.assertEqual(m.surface_overlays("dps"), [])
            self.assertNotIn("touched", m.apply_render_hooks("dps", {}))
            self.assertEqual(m.list_ui_panels(), [])
            self.assertFalse(m.invoke_ui_action("demo", "go")["ok"])

    def test_render_budget_failure_recorded(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_ui_panel_slow_") as root:
            _write_plugin(
                os.path.join(root, "slow_panel"),
                {"id": "slow_panel", "name": "Slow", "version": "1.0.0",
                 "entry": "plugin.py", "enabled": True},
                "import time\n"
                "def _r(p):\n    time.sleep(0.05)\n    return {'type':'panel','children':[]}\n"
                "def on_load(ctx):\n    ctx.register_ui_panel('slow', {}, render=_r)\n")
            m = PluginManager(plugin_dirs=[root], max_failures=5)
            m.discover()
            self.assertTrue(m.load_plugin("slow_panel"))
            r = m.render_ui_panel("slow", time_budget_ms=1)
            self.assertFalse(r["ok"])

    def test_action_time_budget_enforced(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_ui_action_slow_") as root:
            _write_plugin(
                os.path.join(root, "slow_action"),
                {"id": "slow_action", "name": "Slow Action", "version": "1.0.0",
                 "entry": "plugin.py", "enabled": True},
                "import time\n"
                "def _r(p):\n    return {'type':'panel','children':[]}\n"
                "def _a(action_id, p):\n    time.sleep(0.03)\n    return {'ok': True}\n"
                "def on_load(ctx):\n    ctx.register_ui_panel('sa', {}, render=_r, on_action=_a)\n")
            m = PluginManager(plugin_dirs=[root], max_failures=5)
            m.discover()
            self.assertTrue(m.load_plugin("slow_action"))
            r = m.invoke_ui_action("sa", "go", time_budget_ms=1)
            self.assertFalse(r["ok"])
            self.assertTrue(r.get("timed_out"))


CAPABILITY_PLUGIN = '''
import time
_ctx = None
state = {"ticks": 0, "loaded": ""}
def on_load(ctx):
    global _ctx
    _ctx = ctx
    helper = ctx.load_local("helper.py")
    state["loaded"] = helper.who()
    state["token"] = ctx.set_interval(lambda: state.__setitem__("ticks", state["ticks"] + 1), 0.03)
    ctx.register_ui_panel("cap", {}, render=lambda p: ctx.ui.panel("Cap", [ctx.ui.text("ok")]))
'''
HELPER_CODE = 'def who():\n    return "local-helper"\n'


class PluginCapabilityTests(unittest.TestCase):
    def test_scheduler_load_local_and_unload_cancels_timers(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_cap_") as root:
            _write_plugin(
                os.path.join(root, "cap_demo"),
                {"id": "cap_demo", "name": "Cap", "version": "1.0.0",
                 "entry": "plugin.py", "enabled": True},
                CAPABILITY_PLUGIN)
            with open(os.path.join(root, "cap_demo", "helper.py"), "w", encoding="utf-8") as fp:
                fp.write(HELPER_CODE)
            m = PluginManager(plugin_dirs=[root])
            m.discover()
            self.assertTrue(m.load_plugin("cap_demo"), m.status()["plugins"][0]["last_error"])
            mod = __import__("sys").modules["act_plugin_cap_demo"]
            self.assertEqual(mod.state["loaded"], "local-helper")  # load_local worked
            time.sleep(0.18)
            self.assertGreaterEqual(mod.state["ticks"], 2)  # set_interval fired
            frozen = mod.state["ticks"]
            self.assertIn("act_plugin_cap_demo__helper", __import__("sys").modules)
            m.unload_plugin("cap_demo")
            time.sleep(0.12)
            self.assertEqual(mod.state["ticks"], frozen)  # timers cancelled on unload
            # ctx.load_local module removed from sys.modules on unload (no leak)
            self.assertNotIn("act_plugin_cap_demo__helper", __import__("sys").modules)

    def test_notify_dispatches_to_owner_alert(self) -> None:
        alerts = []

        class Owner:
            class root:
                @staticmethod
                def after(_ms, fn):
                    fn()

            def _show_entity_alert(self, title, message, display_time=0):
                alerts.append((title, message))

        owner = Owner()
        with tempfile.TemporaryDirectory(prefix="act_notify_") as root:
            _write_plugin(
                os.path.join(root, "noti"),
                {"id": "noti", "name": "Noti", "version": "1.0.0",
                 "entry": "plugin.py", "enabled": True},
                "def on_load(ctx):\n    ctx.notify('T', 'hello', duration_s=5)\n")
            m = PluginManager(plugin_dirs=[root], owner_provider=lambda: owner)
            m.discover()
            self.assertTrue(m.load_plugin("noti"))
            self.assertEqual(alerts, [("T", "hello")])

    def test_ensure_manager_load_is_idempotent(self) -> None:
        # Regression: repeated ensure(load=True) / UI calls must NOT reload
        # plugins (which would reset state and kill engines plugins own).
        from act_platform import runtime as rt

        class Owner:
            pass

        owner = Owner()
        with tempfile.TemporaryDirectory(prefix="act_idem_") as root:
            _write_plugin(
                os.path.join(root, "stateful"),
                {"id": "stateful", "name": "Stateful", "version": "1.0.0",
                 "entry": "plugin.py", "enabled": True},
                "_loads = {'n': 0}\n"
                "def on_load(ctx):\n    _loads['n'] += 1\n"
                "def reload_count():\n    return _loads['n']\n")
            rt.ensure_act_plugin_manager(owner, load=True, plugin_dirs=[root])
            mod = __import__("sys").modules["act_plugin_stateful"]
            self.assertEqual(mod.reload_count(), 1)
            # Subsequent calls (as the UI poll funcs make them — no plugin_dirs)
            # must reuse the loaded manager, not reload.
            for _ in range(4):
                rt.ensure_act_plugin_manager(owner, load=True)
            self.assertEqual(mod.reload_count(), 1)


PIN_HOTKEY_PLUGIN = '''
fired = {"n": 0}
_ctx = None
def on_load(ctx):
    global _ctx
    _ctx = ctx
    ctx.register_ui_panel("p", {"title": "P"}, render=lambda _p: ctx.ui.panel("P", []))
    ctx.register_hotkey("go", lambda: fired.__setitem__("n", fired["n"] + 1),
                        default_key="F7", label="Go")
'''


class PinHotkeyDeclareTests(unittest.TestCase):
    def _mgr(self, root, caps):
        _write_plugin(
            os.path.join(root, "ph"),
            {"id": "ph", "name": "PH", "version": "1.0.0", "entry": "plugin.py",
             "enabled": True, "capabilities": caps},
            PIN_HOTKEY_PLUGIN)
        m = PluginManager(plugin_dirs=[root])
        m.discover()
        self.assertTrue(m.load_plugin("ph"), m.status()["plugins"][0]["last_error"])
        return m

    def test_declares_panel_from_manifest(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_decl_") as root:
            m = self._mgr(root, [{"id": "ui_panels", "title": "P"}])
            self.assertTrue(m.status()["plugins"][0]["declares_panel"])
        with tempfile.TemporaryDirectory(prefix="act_decl2_") as root2:
            # No ui_panels capability -> declares_panel False even though it
            # registers a panel at runtime (the manifest is the declaration).
            _write_plugin(
                os.path.join(root2, "nopanel"),
                {"id": "nopanel", "name": "NP", "version": "1.0.0", "entry": "plugin.py",
                 "enabled": True, "capabilities": []},
                "def on_load(ctx):\n    ctx.log('x')\n")
            m2 = PluginManager(plugin_dirs=[root2])
            m2.discover()
            m2.load_plugin("nopanel")
            self.assertFalse(m2.status()["plugins"][0]["declares_panel"])

    def test_hotkey_register_dispatch_cleanup(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_hk_") as root:
            m = self._mgr(root, [{"id": "ui_panels"}])
            hks = m.list_hotkeys()
            self.assertEqual(hks[0]["action"], "plugin.ph.go")
            self.assertEqual(hks[0]["default_key"], "F7")
            self.assertIn("plugin.ph.go", m.hotkey_actions())
            mod = __import__("sys").modules["act_plugin_ph"]
            self.assertTrue(m.dispatch_hotkey("plugin.ph.go"))
            self.assertEqual(mod.fired["n"], 1)
            m.unload_plugin("ph")
            self.assertEqual(m.list_hotkeys(), [])
            self.assertFalse(m.dispatch_hotkey("plugin.ph.go"))

    def test_pin_promotes_in_menu(self) -> None:
        with tempfile.TemporaryDirectory(prefix="act_pin_") as root:
            # two plugins; pin the second -> it sorts first
            for pid in ("aaa", "zzz"):
                _write_plugin(
                    os.path.join(root, pid),
                    {"id": pid, "name": pid, "version": "1.0.0", "entry": "plugin.py", "enabled": True},
                    "def on_load(ctx):\n    pass\n")
            m = PluginManager(plugin_dirs=[root])
            m.discover()
            m.load_all()
            self.assertEqual(m.set_pinned("zzz", True), ["zzz"])
            self.assertTrue({p["id"]: p for p in m.status()["plugins"]}["zzz"]["pinned"])

            class Owner:
                pass
            owner = Owner()
            owner._act_plugin_manager = m
            menu = act_plugin_menu(owner)
            self.assertEqual(menu["plugins"][0]["id"], "zzz")
            self.assertTrue(menu["plugins"][0]["pinned"])
            self.assertEqual([it["type"] for it in menu["items"][:3]], ["manage", "panels", "reload"])
            m.set_pinned("zzz", False)
            self.assertNotIn("zzz", m.pinned_plugins())


class BundledExampleTests(unittest.TestCase):
    def test_examples_render_and_intercept(self) -> None:
        plugin_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "plugins"))

        class FakeAutoKey:
            def get_status(self):
                return {"running": False}

            def start(self):
                return True

            def stop(self):
                return True

        class Owner:
            _auto_key_engine = FakeAutoKey()

        owner = Owner()
        m = PluginManager(
            plugin_dirs=[plugin_root],
            event_bus=EventBus(),
            snapshot_provider=lambda: FIXTURE_SNAPSHOT,
            owner_provider=lambda: owner,
        )
        m.discover()
        status = m.load_all()
        active = {p["id"] for p in status["plugins"] if p["active"]}
        self.assertIn("live_dps_overlay_plugin", active)
        self.assertIn("combat_pace_plugin", active)
        self.assertIn("report_share_plugin", active)

        for panel_id in ("live_dps", "pace", "report_share"):
            r = m.render_ui_panel(panel_id)
            self.assertTrue(r["ok"], (panel_id, r))
            self.assertTrue(r["spec"]["nodes"])

        # combat_pace custom trigger
        inv = m.invoke_extension("trigger_types", "dps_floor",
                                 {"rule": {"threshold": 500000}, "render_spec": FIXTURE_SNAPSHOT["render_spec"]})
        self.assertTrue(inv["ok"] and inv["result"]["matched"])

        # report_share exporter
        exp = m.invoke_extension("exporters", "markdown_report",
                                 {"render_spec": FIXTURE_SNAPSHOT["render_spec"]})
        self.assertTrue(exp["ok"])
        self.assertIn("# Dungeon X", exp["result"]["text"])

        # render_skin interception (disabled by default → enable)
        self.assertTrue(m.enable_plugin("render_skin_plugin"))
        skinned = m.apply_render_hooks("act_aggregate", {"overview": {"dungeon_name": "live"}})
        self.assertTrue(str(skinned["overview"]["dungeon_name"]).startswith("🔌"))
        m.invoke_ui_action("skin_control", "mode:takeover")
        self.assertIn(OVERRIDE_KEY, m.apply_render_hooks("act_aggregate", {"overview": {}}))


if __name__ == "__main__":
    unittest.main()
