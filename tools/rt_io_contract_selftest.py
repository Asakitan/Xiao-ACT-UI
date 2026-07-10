# -*- coding: utf-8 -*-
"""No-driver contract tests for the private rt_io helper/proxy boundary.

The tests load source modules in isolation and never start the helper, open a
pipe, or touch a driver.  Run from the repository root with::

    python tools/rt_io_contract_selftest.py
"""
from __future__ import annotations

import importlib.util
import importlib.abc
import importlib.machinery
import ctypes
import hashlib
import io
import os
from pathlib import Path
import struct
import sys
import types
import unittest
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[1]
MEM_PROBE = REPO_ROOT / "python" / "mem_probe"


def _load_source(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


class _RecordingChildFinder(importlib.abc.MetaPathFinder, importlib.abc.Loader):
    """Provide inert child modules while recording package import order."""

    def __init__(self, package_name: str) -> None:
        self.package_name = package_name
        self.loaded: list[str] = []
        self.children = {
            f"{package_name}.cy_memscan",
            f"{package_name}.rt_io_proxy",
            f"{package_name}.rt_io",
            f"{package_name}._rt_atomic_globals",
        }

    def find_spec(self, fullname, path=None, target=None):
        if fullname in self.children:
            return importlib.machinery.ModuleSpec(fullname, self)
        return None

    def create_module(self, spec):
        return None

    def exec_module(self, module) -> None:
        self.loaded.append(module.__name__)


def _load_package_init(name: str, env: dict[str, str]):
    finder = _RecordingChildFinder(name)
    spec = importlib.util.spec_from_file_location(
        name,
        MEM_PROBE / "__init__.py",
        submodule_search_locations=[str(MEM_PROBE)],
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load mem_probe package init")
    package = importlib.util.module_from_spec(spec)
    sys.modules[name] = package
    old_env = os.environ.copy()
    routed_rt_io = None
    try:
        os.environ.update(env)
        sys.meta_path.insert(0, finder)
        spec.loader.exec_module(package)
        routed_rt_io = sys.modules.get(f"{name}.rt_io")
    finally:
        if finder in sys.meta_path:
            sys.meta_path.remove(finder)
        os.environ.clear()
        os.environ.update(old_env)
        for key in list(sys.modules):
            if key == name or key.startswith(name + "."):
                sys.modules.pop(key, None)
    return package, finder.loaded, routed_rt_io


class HelperEntryContractTests(unittest.TestCase):
    def test_import_does_not_execute_helper_entry(self) -> None:
        calls: list[bool] = []
        bootstrap = types.SimpleNamespace(
            receive_session_key=lambda **_kwargs: calls.append(True),
            is_ready=lambda: False,
        )
        fake_license = types.ModuleType("license")
        fake_license._bootstrap = bootstrap
        fake_license._wbox = types.SimpleNamespace()

        module_name = "_rt_io_helper_entry_contract_test"
        with mock.patch.dict(sys.modules, {"license": fake_license}):
            with mock.patch.object(sys, "argv", ["helper"]):
                try:
                    _load_source(
                        module_name,
                        MEM_PROBE / "_rt_io_helper_entry.py",
                    )
                finally:
                    sys.modules.pop(module_name, None)

        self.assertEqual(calls, [], "module import must not execute _main()")

    def test_bootstrap_package_import_does_not_load_backend(self) -> None:
        package_name = "mem_probe_bootstrap_contract_test"
        package, loaded, routed = _load_package_init(
            package_name,
            {
                "SAO_RT_IO_HELPER": "1",
                "SAO_RT_IO_BOOTSTRAP": "1",
            },
        )
        self.assertNotIn(f"{package_name}.rt_io", loaded)
        self.assertNotIn(f"{package_name}.rt_io_proxy", loaded)
        self.assertTrue(getattr(routed, "__sao_bootstrap_block__", False))
        with self.assertRaises(RuntimeError):
            package.rt_io.ensure_loaded

    def test_main_process_routes_rt_io_to_proxy_only(self) -> None:
        package_name = "mem_probe_main_contract_test"
        package, loaded, routed = _load_package_init(
            package_name,
            {
                "SAO_RT_IO_HELPER": "0",
                "SAO_RT_IO_BOOTSTRAP": "0",
            },
        )
        self.assertIn(f"{package_name}.rt_io_proxy", loaded)
        self.assertNotIn(f"{package_name}.rt_io", loaded)
        self.assertIs(
            routed,
            getattr(package, "_rt", None),
        )

    def test_helper_authenticates_and_negotiates_before_backend_call(self) -> None:
        events: list[object] = []
        raw_key = bytes(range(32))
        derived_secret = b"s" * 32
        protocol_version = 2

        bootstrap = types.SimpleNamespace(
            receive_session_key=lambda **_kwargs: events.append("auth"),
            is_ready=lambda: True,
        )
        wbox = types.SimpleNamespace(
            wbox_derive_key=lambda _ctx, length=32: derived_secret[:length],
        )
        fake_license = types.ModuleType("license")
        fake_license._bootstrap = bootstrap
        fake_license._wbox = wbox

        fake_mem_probe = types.ModuleType("mem_probe")
        fake_mem_probe.__path__ = []
        fake_rag = types.ModuleType("mem_probe._rt_atomic_globals")
        fake_rag.PROTOCOL_VERSION = protocol_version
        fake_rag.set_all = lambda _items: events.append("secrets")
        fake_rag.get = lambda name: (
            derived_secret if name == "rt_driver_authtoken" else b""
        )
        fake_rag.mark_bootstrap_complete = (
            lambda version: events.append(("ready", version))
        )
        fake_rag.derive_protocol_key = lambda key, version: hashlib.sha256(
            b"rt-io-wire" + bytes([version]) + key
        ).digest()
        fake_rt = types.ModuleType("mem_probe.rt_io")
        fake_rt._helper_main = lambda pipe, key_hex, parent_pid=0: events.append(
            ("backend", pipe, key_hex, parent_pid)
        )
        fake_mem_probe._rt_atomic_globals = fake_rag
        fake_mem_probe.rt_io = fake_rt

        module_name = "_rt_io_helper_entry_protocol_test"
        modules = {
            "license": fake_license,
            "mem_probe": fake_mem_probe,
            "mem_probe._rt_atomic_globals": fake_rag,
            "mem_probe.rt_io": fake_rt,
        }
        env = {
            "SAO_HELPER_STRICT_BOOTSTRAP": "1",
            "SAO_HELPER_AUTHTOKEN_HEX": derived_secret.hex(),
            "SAO_RT_IO_BOOTSTRAP": "1",
            "SAO_RT_IO_PARENT_PID": str(os.getppid()),
        }
        with mock.patch.dict(sys.modules, modules):
            with mock.patch.dict(os.environ, env, clear=False):
                with mock.patch.object(
                    sys,
                    "stdin",
                    types.SimpleNamespace(
                        buffer=io.BytesIO(raw_key.hex().encode("ascii"))
                    ),
                ):
                    entry = _load_source(
                        module_name,
                        MEM_PROBE / "_rt_io_helper_entry.py",
                    )
                    try:
                        result = entry._main([
                            "helper",
                            r"\\.\pipe\contract",
                            "-",
                            str(protocol_version),
                        ])
                    finally:
                        sys.modules.pop(module_name, None)

        expected_wire_key = fake_rag.derive_protocol_key(
            raw_key, protocol_version
        ).hex()
        self.assertEqual(result, 0)
        self.assertEqual(events[0], "auth")
        self.assertLess(events.index("auth"), len(events) - 1)
        self.assertEqual(
            events[-1],
            (
                "backend",
                r"\\.\pipe\contract",
                expected_wire_key,
                os.getppid(),
            ),
        )

    def test_helper_rejects_old_or_mismatched_protocol(self) -> None:
        fake_license = types.ModuleType("license")
        fake_license._bootstrap = types.SimpleNamespace(
            receive_session_key=lambda **_kwargs: None,
            is_ready=lambda: False,
        )
        fake_license._wbox = types.SimpleNamespace()
        fake_mem_probe = types.ModuleType("mem_probe")
        fake_mem_probe.__path__ = []
        fake_rag = types.ModuleType("mem_probe._rt_atomic_globals")
        fake_rag.PROTOCOL_VERSION = 2
        fake_mem_probe._rt_atomic_globals = fake_rag
        module_name = "_rt_io_helper_entry_mismatch_test"
        with mock.patch.dict(sys.modules, {
            "license": fake_license,
            "mem_probe": fake_mem_probe,
            "mem_probe._rt_atomic_globals": fake_rag,
        }):
            entry = _load_source(
                module_name,
                MEM_PROBE / "_rt_io_helper_entry.py",
            )
            try:
                with self.assertRaises(entry.ProtocolMismatchError):
                    entry._main(["helper", "pipe", "00" * 32, "0"])
                self.assertEqual(
                    entry._entrypoint(["helper", "pipe", "00" * 32, "0"]),
                    0x2E,
                )
            finally:
                sys.modules.pop(module_name, None)

    def test_strict_helper_rejects_missing_main_auth_before_backend(self) -> None:
        events: list[str] = []
        fake_license = types.ModuleType("license")
        fake_license._bootstrap = types.SimpleNamespace(
            receive_session_key=lambda **_kwargs: events.append("auth"),
            is_ready=lambda: True,
        )
        fake_license._wbox = types.SimpleNamespace(
            wbox_derive_key=lambda _ctx, length=32: b"k" * length,
        )
        fake_mem_probe = types.ModuleType("mem_probe")
        fake_mem_probe.__path__ = []
        fake_rag = types.ModuleType("mem_probe._rt_atomic_globals")
        fake_rag.PROTOCOL_VERSION = 2
        fake_rag.set_all = lambda _items: None
        fake_rag.get = lambda _name: b"k" * 32
        fake_rag.mark_bootstrap_complete = lambda _version: events.append("ready")
        fake_rag.derive_protocol_key = lambda key, _version: key
        fake_rt = types.ModuleType("mem_probe.rt_io")
        fake_rt._helper_main = lambda *_args, **_kwargs: events.append("backend")
        fake_mem_probe._rt_atomic_globals = fake_rag
        fake_mem_probe.rt_io = fake_rt
        modules = {
            "license": fake_license,
            "mem_probe": fake_mem_probe,
            "mem_probe._rt_atomic_globals": fake_rag,
            "mem_probe.rt_io": fake_rt,
        }
        module_name = "_rt_io_helper_entry_strict_auth_test"
        with mock.patch.dict(sys.modules, modules):
            with mock.patch.dict(os.environ, {
                "SAO_HELPER_STRICT_BOOTSTRAP": "1",
                "SAO_HELPER_AUTHTOKEN_HEX": "",
                "SAO_RT_IO_BOOTSTRAP": "1",
                "SAO_RT_IO_PARENT_PID": str(os.getppid()),
            }, clear=False):
                with mock.patch.object(
                    sys,
                    "stdin",
                    types.SimpleNamespace(
                        buffer=io.BytesIO((b"b" * 32).hex().encode("ascii"))
                    ),
                ):
                    entry = _load_source(
                        module_name,
                        MEM_PROBE / "_rt_io_helper_entry.py",
                    )
                    try:
                        with self.assertRaises(entry.BootstrapAuthenticationError):
                            entry._main(["helper", "pipe", "-", "2"])
                    finally:
                        sys.modules.pop(module_name, None)
        self.assertEqual(events, ["auth"])


class ProxyContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.proxy_name = "_rt_io_proxy_contract_test"
        cls.proxy = _load_source(
            cls.proxy_name,
            MEM_PROBE / "rt_io_proxy.py",
        )

    @classmethod
    def tearDownClass(cls) -> None:
        sys.modules.pop(cls.proxy_name, None)

    def tearDown(self) -> None:
        self.proxy._engine = None
        self.proxy._selected_engine = None
        self.proxy._attached_pid = 0

    def test_engine_mapping_and_public_api_are_present(self) -> None:
        p = self.proxy
        self.assertEqual(p.ENGINE_TIER[p.ENGINE_READONLY], p.TIER_A)
        self.assertEqual(p.ENGINE_TIER[p.ENGINE_PHYSRW], p.TIER_E)
        for name in (
            "status",
            "available_engines",
            "engine_info",
            "select_engine",
            "probe",
            "request_capability",
            "read_into",
        ):
            self.assertTrue(callable(getattr(p, name, None)), name)

    def test_proxy_and_bootstrap_derive_the_same_version_bound_wire_key(self) -> None:
        atomic_name = "_rt_atomic_globals_contract_test"
        atomic = _load_source(
            atomic_name,
            MEM_PROBE / "_rt_atomic_globals.py",
        )
        try:
            raw_key = bytes(range(32))
            expected = atomic.derive_protocol_key(
                raw_key, atomic.PROTOCOL_VERSION
            )
            actual = self.proxy._derive_protocol_key(
                raw_key, self.proxy._PROTOCOL_VERSION
            )
            self.assertEqual(actual, expected)
            self.assertNotEqual(actual, raw_key)
            atomic.mark_bootstrap_complete(atomic.PROTOCOL_VERSION)
            self.assertTrue(atomic.bootstrap_ready(atomic.PROTOCOL_VERSION))
            self.assertFalse(atomic.bootstrap_ready(atomic.PROTOCOL_VERSION + 1))
        finally:
            sys.modules.pop(atomic_name, None)

    def test_helper_environment_uses_explicit_dev_or_strict_bootstrap_gate(self) -> None:
        p = self.proxy
        with mock.patch.object(p, "_helper_authtoken_hex", return_value=""):
            with mock.patch.object(p.sys, "frozen", False, create=True):
                dev_env = p._helper_env()
            with mock.patch.object(p.sys, "frozen", True, create=True):
                prod_env = p._helper_env()
        self.assertEqual(dev_env["SAO_RT_IO_HELPER"], "1")
        self.assertEqual(dev_env["SAO_RT_IO_BOOTSTRAP"], "1")
        self.assertEqual(
            dev_env["SAO_RT_IO_PROTOCOL_VERSION"], str(p._PROTOCOL_VERSION)
        )
        self.assertEqual(dev_env["SAO_RT_IO_DEV_BOOTSTRAP"], "1")
        self.assertEqual(dev_env["SAO_HELPER_STRICT_BOOTSTRAP"], "0")
        self.assertNotIn("SAO_RT_IO_DEV_BOOTSTRAP", prod_env)
        self.assertEqual(prod_env["SAO_HELPER_STRICT_BOOTSTRAP"], "1")

    def test_dev_helper_command_invokes_side_effect_free_entry_once(self) -> None:
        p = self.proxy
        command = p._build_helper_command(
            str(MEM_PROBE / "_rt_io_helper_entry.py"),
            r"\\.\pipe\contract",
            "-",
            str(p._PROTOCOL_VERSION),
        )
        self.assertEqual(command[:2], [p.sys.executable, "-c"])
        script = command[2]
        self.assertEqual(script.count("_entrypoint()"), 1)
        self.assertNotIn("_main()", script)
        self.assertIn(str(p._PROTOCOL_VERSION), script)
        self.assertNotIn("ab" * 32, script)

    def test_real_backend_disables_legacy_helper_entry(self) -> None:
        source = (MEM_PROBE / "rt_io.py").read_text(encoding="utf-8")
        legacy_branch = source[source.rfind('if __name__ == "__main__":') :]
        self.assertIn("legacy rt_io.py --helper entry is disabled", legacy_branch)
        self.assertNotIn("_helper_main(_sys.argv[2], _sys.argv[3])", legacy_branch)

    def test_status_and_engine_info_keep_real_backend_shapes(self) -> None:
        p = self.proxy
        p._engine = p.ENGINE_PHYSRW
        p._attached_pid = 4242
        payload = struct.pack("<BBQQQBBB", 5, 1, 0, 0, 7, 1, 1, 1)
        with mock.patch.object(p, "_call", return_value=payload):
            self.assertEqual(
                p.status(),
                {
                    "tier": p.TIER_E,
                    "backend_ready": True,
                    "attached_pid": 4242,
                    "session_epoch": 0,
                    "session_state": "STOPPED",
                },
            )
            self.assertEqual(
                p.engine_info(),
                {
                    "tier": p.TIER_E,
                    "capabilities": p.CAP_READ | p.CAP_WRITE | p.CAP_INPUT,
                    "can_read": True,
                    "can_write": True,
                },
            )

    def test_failed_reattach_clears_previous_attached_pid(self) -> None:
        p = self.proxy
        p._attached_pid = 111
        with mock.patch.object(p, "_call", return_value=b"\x00"):
            self.assertFalse(p.attach(222))
        self.assertEqual(p._attached_pid, 0)

    def test_select_and_request_capability_preserve_bool_contract(self) -> None:
        p = self.proxy
        with mock.patch.object(p, "_lic_check", return_value=True):
            self.assertIsNone(p.select_engine(p.ENGINE_PHYSRW))
        self.assertEqual(p._selected_engine, p.ENGINE_PHYSRW)
        p._engine = None
        with mock.patch.object(p, "ensure_loaded", return_value=True) as load:
            self.assertTrue(p.request_capability(p.CAP_INPUT))
        load.assert_called_once_with(p.ENGINE_PHYSRW)

    def test_available_engines_has_real_backend_record_shape(self) -> None:
        p = self.proxy
        with mock.patch.object(p, "_lic_check", return_value=True):
            records = p.available_engines()
        self.assertEqual(
            [item["id"] for item in records],
            [
                p.ENGINE_READONLY,
                p.ENGINE_READWRITE,
                p.ENGINE_READWRITE_SB,
                p.ENGINE_PRVMODE,
                p.ENGINE_PHYSRW,
            ],
        )
        self.assertTrue(all(set(item) == {
            "id", "name", "caps", "available", "licensed"
        } for item in records))

    def test_read_into_and_rpm_report_actual_short_read_length(self) -> None:
        p = self.proxy
        dest = (ctypes.c_ubyte * 4)()
        got = ctypes.c_size_t(999)
        with mock.patch.object(p, "read", return_value=b"x"):
            self.assertEqual(p.read_into(0x1000, dest, 4), 1)
            self.assertTrue(p._rpm(None, 0x1000, dest, 4, ctypes.pointer(got)))
        self.assertEqual(bytes(dest[:1]), b"x")
        self.assertEqual(got.value, 1)

    def test_sysmodule_parser_accepts_minimum_fourteen_byte_header(self) -> None:
        p = self.proxy
        response = struct.pack("<QIH", 0x12340000, 0x2000, 1) + b"x"
        with mock.patch.object(p, "_call", return_value=response):
            self.assertEqual(
                p._query_sys_modules(),
                {"x": (0x12340000, 0x2000)},
            )

    def test_wndshield_supports_real_zero_arg_state_contract(self) -> None:
        p = self.proxy

        class ImmediateThread:
            def __init__(self, target, args=(), **_kwargs):
                self.target = target
                self.args = args

            def start(self):
                self.target(*self.args)

        calls = []
        with mock.patch.object(p.threading, "Thread", ImmediateThread):
            with mock.patch.object(
                p, "set_wp", side_effect=lambda hwnd, en=True: calls.append((hwnd, en))
            ):
                shield = p.WndShield()
                self.assertEqual(shield._h, [])
                self.assertFalse(shield._s)
                self.assertFalse(shield.streaming)
                shield.register(101)
                self.assertEqual(shield._h, [101])
                self.assertTrue(shield.toggle())
                shield.unregister(101)
        self.assertEqual(calls, [(101, True), (101, False), (101, False)])


if __name__ == "__main__":
    unittest.main(verbosity=2)
