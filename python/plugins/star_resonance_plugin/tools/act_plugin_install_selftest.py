# -*- coding: utf-8 -*-
"""Regression coverage for one-click plugin import (.zip → user_plugins → live).

Covers: zip-slip-safe install, flat + nested archive layouts, vendored-dep
auto-import (sys.path managed by the manager), requirements bootstrap, the
non-disruptive single-plugin refresh+enable path, upgrade-in-place, uninstall
(user-only), and the end-to-end ``act_plugin_import`` runtime action.
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from act_platform import plugin_deps, plugin_install, runtime as act_runtime
from act_platform.event_bus import EventBus
from act_platform.plugins import PluginManager


class DictSettings:
    def __init__(self) -> None:
        self.data: dict = {}

    def get(self, key, default=None):
        return self.data.get(key, default)

    def set(self, key, value):
        self.data[key] = value

    def save(self):
        pass


def _write_plugin_tree(plugin_dir: str, *, plugin_id: str, version: str = "1.0.0",
                       dep_module: str = "", entry_extra: str = "") -> None:
    """Author a plugin directory: plugin.json + plugin.py (+ vendor dep)."""
    os.makedirs(plugin_dir, exist_ok=True)
    manifest = {
        "id": plugin_id,
        "name": plugin_id.replace("_", " ").title(),
        "version": version,
        "entry": "plugin.py",
        "enabled": True,
    }
    if dep_module:
        manifest["permissions"] = []
        with open(os.path.join(plugin_dir, "requirements.txt"), "w", encoding="utf-8") as fp:
            fp.write(f"# bundled pure-python dep\n{dep_module}\n")
        vendor_dir = os.path.join(plugin_dir, "vendor")
        os.makedirs(vendor_dir, exist_ok=True)
        with open(os.path.join(vendor_dir, f"{dep_module}.py"), "w", encoding="utf-8") as fp:
            fp.write(f'MARK = "{dep_module}_OK"\n')
    with open(os.path.join(plugin_dir, "plugin.json"), "w", encoding="utf-8") as fp:
        json.dump(manifest, fp, ensure_ascii=False, indent=2)
    body = ["LOADED_MARK = None", "VERSION = %r" % version]
    if dep_module:
        body.append(f"import {dep_module} as _dep")
        body.append("LOADED_MARK = _dep.MARK")
    body.append("")
    body.append("def on_load(ctx):")
    body.append("    ctx.log('loaded ' + VERSION)")
    if entry_extra:
        body.append("    " + entry_extra)
    body.append("")
    with open(os.path.join(plugin_dir, "plugin.py"), "w", encoding="utf-8") as fp:
        fp.write("\n".join(body))


def _zip_dir(src_dir: str, zip_path: str, *, arc_prefix: str = "") -> str:
    """Zip the contents of src_dir. arc_prefix='' → flat (manifest at root)."""
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for base, _dirs, files in os.walk(src_dir):
            for name in files:
                full = os.path.join(base, name)
                rel = os.path.relpath(full, src_dir)
                arcname = os.path.join(arc_prefix, rel) if arc_prefix else rel
                zf.write(full, arcname)
    return zip_path


class PluginInstallTests(unittest.TestCase):
    def setUp(self) -> None:
        self._created_modules = set(sys.modules)

    def tearDown(self) -> None:
        # Drop any vendor dep modules our plugins imported, so tests don't leak.
        for name in list(sys.modules):
            if name not in self._created_modules and name.startswith("acme_"):
                sys.modules.pop(name, None)

    # ── install (archive → user_plugins) ──────────────────────────────────
    def test_flat_archive_installs_to_user_plugins(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pi_flat_") as root:
            build = os.path.join(root, "build", "demo")
            _write_plugin_tree(build, plugin_id="flat_demo", version="1.2.3")
            archive = _zip_dir(build, os.path.join(root, "flat_demo.zip"))
            user_dir = os.path.join(root, "user_plugins")
            result = plugin_install.install_plugin_archive(archive, user_dir)
            self.assertTrue(result["ok"], result)
            self.assertEqual(result["id"], "flat_demo")
            self.assertEqual(result["version"], "1.2.3")
            self.assertTrue(os.path.isfile(os.path.join(user_dir, "flat_demo", "plugin.json")))
            self.assertTrue(os.path.isfile(os.path.join(user_dir, "flat_demo", "plugin.py")))

    def test_nested_archive_locates_manifest_in_subfolder(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pi_nest_") as root:
            build = os.path.join(root, "build", "demo")
            _write_plugin_tree(build, plugin_id="nested_demo")
            # Everything nested under a single top-level folder inside the zip.
            archive = _zip_dir(build, os.path.join(root, "nested_demo.zip"),
                               arc_prefix="nested_demo")
            user_dir = os.path.join(root, "user_plugins")
            result = plugin_install.install_plugin_archive(archive, user_dir)
            self.assertTrue(result["ok"], result)
            self.assertEqual(result["id"], "nested_demo")
            self.assertTrue(os.path.isfile(os.path.join(user_dir, "nested_demo", "plugin.json")))

    def test_zip_slip_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pi_slip_") as root:
            archive = os.path.join(root, "evil.zip")
            with zipfile.ZipFile(archive, "w") as zf:
                zf.writestr("plugin.json", json.dumps({"id": "evil", "entry": "plugin.py"}))
                zf.writestr("plugin.py", "def on_load(ctx):\n    pass\n")
                zf.writestr("../escape.txt", "pwned")
            user_dir = os.path.join(root, "user_plugins")
            result = plugin_install.install_plugin_archive(archive, user_dir)
            self.assertFalse(result["ok"])
            self.assertFalse(os.path.exists(os.path.join(root, "escape.txt")))

    def test_missing_manifest_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pi_nomani_") as root:
            archive = os.path.join(root, "nomani.zip")
            with zipfile.ZipFile(archive, "w") as zf:
                zf.writestr("readme.txt", "no manifest here")
            result = plugin_install.install_plugin_archive(archive, os.path.join(root, "user_plugins"))
            self.assertFalse(result["ok"])

    def test_reimport_upgrades_in_place(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pi_up_") as root:
            user_dir = os.path.join(root, "user_plugins")
            b1 = os.path.join(root, "b1", "demo")
            _write_plugin_tree(b1, plugin_id="up_demo", version="1.0.0")
            r1 = plugin_install.install_plugin_archive(_zip_dir(b1, os.path.join(root, "v1.zip")), user_dir)
            self.assertTrue(r1["ok"])
            self.assertFalse(r1["replaced"])
            b2 = os.path.join(root, "b2", "demo")
            _write_plugin_tree(b2, plugin_id="up_demo", version="2.0.0")
            r2 = plugin_install.install_plugin_archive(_zip_dir(b2, os.path.join(root, "v2.zip")), user_dir)
            self.assertTrue(r2["ok"])
            self.assertTrue(r2["replaced"])
            self.assertEqual(r2["previous_version"], "1.0.0")
            self.assertEqual(r2["version"], "2.0.0")

    # ── live load: vendored dep auto-imports, refresh is non-disruptive ────
    def test_vendored_dep_auto_imports_on_load(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pi_vendor_") as root:
            user_dir = os.path.join(root, "user_plugins")
            build = os.path.join(root, "build", "demo")
            _write_plugin_tree(build, plugin_id="vendor_demo", dep_module="acme_vendor")
            archive = _zip_dir(build, os.path.join(root, "vendor_demo.zip"))
            res = plugin_install.install_plugin_archive(archive, user_dir)
            self.assertTrue(res["ok"], res)

            mgr = PluginManager(plugin_dirs=[os.path.join(root, "plugins"), user_dir],
                                event_bus=EventBus(), settings=DictSettings())
            mgr.discover()
            self.assertTrue(mgr.enable_plugin("vendor_demo"))
            rec = mgr._records["vendor_demo"]
            self.assertTrue(rec.active, rec.last_error)
            # The vendored dep was importable purely because the manager prepended
            # the plugin's vendor/ to sys.path before exec'ing the entry module.
            self.assertEqual(rec.module.LOADED_MARK, "acme_vendor_OK")
            # Unload restores sys.path (no leaked vendor dir).
            vendor_path = os.path.abspath(os.path.join(rec.path, "vendor"))
            self.assertIn(vendor_path, sys.path)
            mgr.disable_plugin("vendor_demo")
            self.assertNotIn(vendor_path, sys.path)

    def test_requirements_bootstrap_resolves_vendor(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pi_req_") as root:
            build = os.path.join(root, "demo")
            _write_plugin_tree(build, plugin_id="req_demo", dep_module="acme_req")
            rec = plugin_deps.ensure_requirements(build, install=False)
            self.assertEqual(rec["deps"].get("acme_req"), "vendor")
            plugin_deps.restore_paths(rec)
            self.assertNotIn(os.path.abspath(os.path.join(build, "vendor")), sys.path)

    def test_refresh_plugin_does_not_disturb_other_plugins(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pi_refresh_") as root:
            builtin_dir = os.path.join(root, "plugins")
            user_dir = os.path.join(root, "user_plugins")
            os.makedirs(user_dir, exist_ok=True)
            _write_plugin_tree(os.path.join(builtin_dir, "stable"),
                               plugin_id="stable", entry_extra="ctx.log('stable up')")
            mgr = PluginManager(plugin_dirs=[builtin_dir, user_dir],
                                event_bus=EventBus(), settings=DictSettings())
            mgr.load_all()
            stable_module = mgr._records["stable"].module
            self.assertIsNotNone(stable_module)

            # Install + register a NEW plugin without touching the running one.
            build = os.path.join(root, "build", "late")
            _write_plugin_tree(build, plugin_id="late")
            res = plugin_install.install_plugin_archive(
                _zip_dir(build, os.path.join(root, "late.zip")), user_dir)
            self.assertTrue(res["ok"])
            mgr.refresh_plugin(res["path"])
            self.assertTrue(mgr.enable_plugin("late"))
            # The previously-loaded plugin's module object is untouched.
            self.assertIs(mgr._records["stable"].module, stable_module)
            self.assertTrue(mgr._records["stable"].active)

    # ── uninstall (user-only) ─────────────────────────────────────────────
    def test_uninstall_removes_user_plugin_only(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pi_uninst_") as root:
            builtin_dir = os.path.join(root, "plugins")
            user_dir = os.path.join(root, "user_plugins")
            os.makedirs(user_dir, exist_ok=True)
            _write_plugin_tree(os.path.join(builtin_dir, "core"), plugin_id="core")
            build = os.path.join(root, "build", "ext")
            _write_plugin_tree(build, plugin_id="ext")
            plugin_install.install_plugin_archive(
                _zip_dir(build, os.path.join(root, "ext.zip")), user_dir)

            mgr = PluginManager(plugin_dirs=[builtin_dir, user_dir],
                                event_bus=EventBus(), settings=DictSettings(),
                                user_plugin_dirs=[user_dir])
            mgr.discover()
            self.assertTrue(mgr.is_user_plugin("ext"))
            self.assertFalse(mgr.is_user_plugin("core"))
            # built-in protected
            guard = plugin_install.remove_installed_plugin(
                mgr._records["core"].path, user_dir)
            self.assertFalse(guard["ok"])
            self.assertTrue(os.path.isdir(mgr._records["core"].path))
            # user plugin removable
            ext_path = mgr._records["ext"].path
            self.assertTrue(mgr.forget_plugin("ext"))
            rem = plugin_install.remove_installed_plugin(ext_path, user_dir)
            self.assertTrue(rem["ok"])
            self.assertFalse(os.path.isdir(ext_path))

    # ── end-to-end runtime action ─────────────────────────────────────────
    def test_act_plugin_import_end_to_end(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pi_e2e_") as root:
            os.makedirs(os.path.join(root, "plugins"), exist_ok=True)
            os.makedirs(os.path.join(root, "user_plugins"), exist_ok=True)
            build = os.path.join(root, "build", "e2e")
            _write_plugin_tree(build, plugin_id="e2e_demo", version="3.1.0",
                               dep_module="acme_e2e")
            archive = _zip_dir(build, os.path.join(root, "e2e_demo.zip"))

            class Owner:
                def __init__(self):
                    self.settings = DictSettings()

            owner = Owner()
            orig_base = act_runtime.project_base_dir
            act_runtime.project_base_dir = lambda: root  # redirect user_plugins to temp
            try:
                res = act_runtime.act_plugin_import(owner, archive)
                self.assertTrue(res["ok"], res)
                self.assertEqual(res["id"], "e2e_demo")
                self.assertTrue(res["loaded"], res.get("load_error"))
                self.assertEqual(res["deps"].get("acme_e2e"), "vendor")
                self.assertTrue(os.path.isfile(
                    os.path.join(root, "user_plugins", "e2e_demo", "plugin.json")))
                # Plugin shows up active in the status the action returned.
                ids = {p["id"]: p for p in res["status"]["plugins"]}
                self.assertIn("e2e_demo", ids)
                self.assertTrue(ids["e2e_demo"]["active"])
                self.assertTrue(ids["e2e_demo"]["user_installed"])

                # Uninstall through the runtime action.
                rem = act_runtime.act_plugin_uninstall(owner, "e2e_demo")
                self.assertTrue(rem["ok"], rem)
                self.assertFalse(os.path.isdir(
                    os.path.join(root, "user_plugins", "e2e_demo")))
            finally:
                act_runtime.project_base_dir = orig_base


if __name__ == "__main__":
    unittest.main(verbosity=2)
