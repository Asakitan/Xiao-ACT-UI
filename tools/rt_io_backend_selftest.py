# -*- coding: utf-8 -*-
"""No-driver regression tests for the private rt_io backend.

This module never starts the helper, opens a device, loads a driver, or invokes
the live cleanup command.  Native boundaries are replaced with in-memory fakes.
"""
from __future__ import annotations

import atexit
import contextlib
import ctypes
import ctypes.wintypes as wt
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import threading
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
RT_IO_PATH = ROOT / "python" / "mem_probe" / "rt_io.py"
CLEANUP_PATH = ROOT.parent / "rt_io" / "cleanup.py"


def _load_backend():
    spec = importlib.util.spec_from_file_location("_rt_io_backend_selftest", RT_IO_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {RT_IO_PATH}")
    module = importlib.util.module_from_spec(spec)
    # Fail closed before executing any module code: even an import exception
    # cannot leave a native teardown callback registered in this test process.
    with mock.patch.object(atexit, "register", return_value=None):
        spec.loader.exec_module(module)
    return module


def _load_cleanup_tool():
    spec = importlib.util.spec_from_file_location("_rt_io_cleanup_selftest", CLEANUP_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {CLEANUP_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_BACKEND = None


def _backend():
    global _BACKEND
    if _BACKEND is None:
        _BACKEND = _load_backend()
    return _BACKEND


def _synthetic_r3_pe():
    """Return a tiny x64 PE plus its mapped header/text for identity tests."""
    data = bytearray(0x1600)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    pe = 0x80
    data[pe:pe + 4] = b"PE\0\0"
    struct.pack_into("<HHI", data, pe + 4, 0x8664, 2, 0x12345678)
    struct.pack_into("<H", data, pe + 20, 0xF0)
    optional = pe + 24
    struct.pack_into("<H", data, optional, 0x20B)
    struct.pack_into("<Q", data, optional + 24, 0x140000000)
    struct.pack_into("<I", data, optional + 56, 0x4000)
    struct.pack_into("<I", data, optional + 60, 0x400)
    struct.pack_into("<I", data, optional + 108, 16)
    struct.pack_into("<II", data, optional + 112 + 5 * 8, 0x3000, 12)
    section_table = optional + 0xF0
    data[section_table:section_table + 8] = b".text\0\0\0"
    struct.pack_into("<IIII", data, section_table + 8, 0x1000, 0x1000, 0x1000, 0x400)
    struct.pack_into("<I", data, section_table + 36, 0x60000020)
    reloc_header = section_table + 40
    data[reloc_header:reloc_header + 8] = b".reloc\0\0"
    struct.pack_into("<IIII", data, reloc_header + 8, 0x200, 0x3000, 0x200, 0x1400)
    struct.pack_into("<I", data, reloc_header + 36, 0x42000040)
    for index in range(0x1000):
        data[0x400 + index] = (index * 17 + 3) & 0xFF
    for rva in (0x1563, 0x18A6):
        offset = 0x400 + rva - 0x1000
        data[offset:offset + 3] = b"\x45\x33\xC0"
    struct.pack_into("<Q", data, 0x400 + 0x200, 0x140001234)
    struct.pack_into("<IIHH", data, 0x1400, 0x1000, 12, 0xA200, 0)
    return bytes(data), bytes(data[:0x400]), bytes(data[0x400:0x1400])


class _FakeNtBoundary:
    def __init__(self, submit_status: int, completion_status: int, information: int,
                 wait_result: int = 0):
        self.submit_status = submit_status
        self.completion_status = completion_status
        self.information = information
        self.wait_result = wait_result
        self.wait_calls = 0
        self.closed = False

    def create_event(self):
        return 101

    def device_io(self, handle, event, isb, ioctl, ci, ci_sz, co, co_sz):
        isb.Status = self.completion_status
        isb.Information = self.information
        return self.submit_status

    def wait(self, event):
        self.wait_calls += 1
        return self.wait_result

    def close_event(self, event):
        self.closed = True


class _FakeThread:
    def __init__(self, *, join_completes=True):
        self.alive = False
        self.join_completes = join_completes
        self.start_calls = 0
        self.join_calls = 0

    def start(self):
        self.start_calls += 1
        self.alive = True

    def is_alive(self):
        return self.alive

    def join(self, timeout=None):
        self.join_calls += 1
        if self.join_completes:
            self.alive = False


class NativeIoCompletionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rt = _backend()

    def test_pending_waits_for_iosb_and_preserves_zero_information(self):
        boundary = _FakeNtBoundary(
            self.rt._STATUS_PENDING,
            0,
            0,
        )
        returned = wt.DWORD(0xFFFFFFFF)

        ok = self.rt._io_d0_nt(
            boundary,
            1,
            0x222000,
            None,
            0,
            None,
            4096,
            returned,
        )

        self.assertTrue(ok)
        self.assertEqual(returned.value, 0)
        self.assertEqual(boundary.wait_calls, 1)
        self.assertTrue(boundary.closed)


class ReadContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rt = _backend()

    def test_rpm_reports_actual_short_read_length(self):
        buf = ctypes.create_string_buffer(8)
        got = ctypes.c_size_t(99)
        with mock.patch.object(self.rt, "read", return_value=b"ab"):
            self.assertTrue(
                self.rt._rpm(0, 0x1000, buf, 8, ctypes.byref(got)))
        self.assertEqual(got.value, 2)
        self.assertEqual(buf.raw[:2], b"ab")

    def test_pending_completion_failure_is_not_reported_as_success(self):
        boundary = _FakeNtBoundary(self.rt._STATUS_PENDING, -1073741823, 99)
        returned = wt.DWORD(77)
        self.assertFalse(
            self.rt._io_d0_nt(boundary, 1, 2, None, 0, None, 10, returned)
        )
        self.assertEqual(returned.value, 77)
        self.assertTrue(boundary.closed)

    def test_pending_wait_failure_is_not_reported_as_success(self):
        boundary = _FakeNtBoundary(self.rt._STATUS_PENDING, 0, 99, wait_result=258)
        returned = wt.DWORD(77)
        self.assertFalse(
            self.rt._io_d0_nt(boundary, 1, 2, None, 0, None, 10, returned)
        )
        self.assertEqual(returned.value, 77)
        self.assertTrue(boundary.closed)


class OwnedCleanupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rt = _backend()

    def test_stale_cleanup_only_removes_hash_matched_manifest_path(self):
        with tempfile.TemporaryDirectory() as td:
            stage = Path(td) / "stage"
            stage.mkdir()
            owned = stage / "owned.sys"
            unowned = stage / "unowned.sys"
            changed = stage / "changed.sys"
            owned.write_bytes(b"owned")
            unowned.write_bytes(b"unowned")
            changed.write_bytes(b"changed-now")
            manifest = {
                "owner_pid": 999999,
                "services": ["ExactService_1234"],
                "files": [
                    {"path": str(owned), "sha256": hashlib.sha256(b"owned").hexdigest()},
                    {"path": str(changed), "sha256": hashlib.sha256(b"old").hexdigest()},
                ],
            }
            state_file = Path(td) / "cache.dat"
            state_file.write_text("manifest", encoding="utf-8")

            with (
                mock.patch.object(self.rt, "_drv_stage_dir", return_value=str(stage)),
                mock.patch.object(self.rt, "_load_owned_manifest", return_value=manifest, create=True),
                mock.patch.object(self.rt, "_pid_is_alive", return_value=False, create=True),
                mock.patch.object(self.rt, "_SVC_STATE_FILE", str(state_file)),
                mock.patch.dict(self.rt.os.environ, {"SystemRoot": td}),
            ):
                self.assertTrue(self.rt._cleanup_stale())

            self.assertFalse(owned.exists())
            self.assertTrue(unowned.exists())
            self.assertTrue(changed.exists())

    def test_prior_owned_manifest_blocks_a_replacement_backend_epoch(self):
        prior = {
            "session_id": "older-session",
            "owner_pid": 999999,
            "services": [],
            "files": [],
        }
        with mock.patch.object(
            self.rt, "_load_owned_manifest", return_value=prior
        ):
            self.assertTrue(self.rt._prior_session_cleanup_pending())
        current = dict(prior, session_id=self.rt._owned_session_id)
        with mock.patch.object(
            self.rt, "_load_owned_manifest", return_value=current
        ):
            self.assertFalse(self.rt._prior_session_cleanup_pending())

    def test_ensure_loaded_refuses_before_any_backend_probe_when_cleanup_pending(self):
        with (
            mock.patch.object(
                self.rt, "_prior_session_cleanup_pending", return_value=True
            ),
            mock.patch.object(
                self.rt,
                "_g0",
                side_effect=AssertionError("guard must run before backend probe"),
            ),
        ):
            self.assertFalse(self.rt.ensure_loaded())

    def test_safe_remove_never_opens_file_for_overwrite(self):
        with (
            mock.patch.object(self.rt, "_owned_path_recorded", return_value=True),
            mock.patch.object(self.rt.os, "remove", return_value=None),
            mock.patch("builtins.open", side_effect=AssertionError("must not overwrite")) as opened,
        ):
            self.assertTrue(self.rt._safe_remove("exact-owned-path"))
            opened.assert_not_called()

    def test_safe_remove_rejects_path_outside_owned_stage(self):
        with (
            mock.patch.object(self.rt, "_owned_path_recorded", return_value=False),
            mock.patch.object(self.rt.os, "remove") as remove,
        ):
            self.assertFalse(self.rt._safe_remove(r"C:\Windows\System32\drivers\other.sys"))
        remove.assert_not_called()

    def test_external_cleanup_tool_accepts_only_authenticated_stage_children(self):
        cleanup = _load_cleanup_tool()
        with tempfile.TemporaryDirectory() as td:
            stage = Path(td) / "stage"
            stage.mkdir()
            owned = stage / "owned.sys"
            owned.write_bytes(b"owned")
            payload = {
                "version": 1,
                "session_id": "test",
                "owner_pid": 999999,
                "services": ["ExactService_1234"],
                "files": [{
                    "path": str(owned),
                    "sha256": hashlib.sha256(b"owned").hexdigest(),
                }],
                "service_files": [{
                    "name": "ExactService_1234",
                    "path": str(owned),
                    "sha256": hashlib.sha256(b"owned").hexdigest(),
                }],
            }
            manifest_path = Path(td) / "cache.dat"
            manifest_path.write_bytes(self.rt._encode_owned_manifest(payload))

            loaded = cleanup.load_manifest(manifest_path, stage, self.rt._mg())
            self.assertEqual(loaded["services"], ["ExactService_1234"])
            self.assertEqual(loaded["files"][0]["path"], str(owned.resolve()))
            self.assertEqual(loaded["service_files"][0]["name"], "ExactService_1234")

            tampered = json.loads(manifest_path.read_text(encoding="utf-8"))
            tampered["payload"]["owner_pid"] = 1
            manifest_path.write_text(json.dumps(tampered), encoding="utf-8")
            with self.assertRaises(ValueError):
                cleanup.load_manifest(manifest_path, stage, self.rt._mg())

            payload["files"][0]["path"] = str(Path(td) / "outside.sys")
            manifest_path.write_bytes(self.rt._encode_owned_manifest(payload))
            with self.assertRaises(ValueError):
                cleanup.load_manifest(manifest_path, stage, self.rt._mg())

    def test_external_cleanup_refuses_live_owner_without_running_sc(self):
        cleanup = _load_cleanup_tool()
        manifest = {
            "session_id": "test",
            "owner_pid": 1,
            "services": ["ExactService_1234"],
            "files": [],
        }
        with (
            tempfile.TemporaryDirectory() as td,
            mock.patch.object(cleanup, "_pid_is_alive", return_value=True),
            mock.patch.object(cleanup, "_service_exists", return_value=True),
            mock.patch.object(cleanup.subprocess, "run") as run,
        ):
            with self.assertRaises(RuntimeError):
                cleanup.apply_manifest(manifest, Path(td) / "cache.dat")
        run.assert_not_called()

    def test_external_cleanup_refuses_reused_service_name(self):
        cleanup = _load_cleanup_tool()
        manifest = {
            "session_id": "test",
            "owner_pid": 999999,
            "services": ["ExactService_1234"],
            "files": [],
            "service_files": [{
                "name": "ExactService_1234",
                "path": r"C:\owned\driver.sys",
                "sha256": "0" * 64,
            }],
        }
        with (
            tempfile.TemporaryDirectory() as td,
            mock.patch.object(cleanup, "_pid_is_alive", return_value=False),
            mock.patch.object(
                cleanup, "_service_image_path", return_value=r"c:\other\driver.sys"
            ),
            mock.patch.object(cleanup.subprocess, "run") as run,
        ):
            result = cleanup.apply_manifest(manifest, Path(td) / "cache.dat")
        self.assertTrue(result["failures"])
        run.assert_not_called()


class WriterReadinessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rt = _backend()

    def test_r3_patch_failure_does_not_publish_ready_or_run_cleanup_writes(self):
        cleanup = mock.Mock(return_value=True)
        close_r3 = mock.Mock()
        with (
            mock.patch.object(self.rt, "_g0", return_value=True),
            mock.patch.object(self.rt, "_r1_l", return_value=True),
            mock.patch.object(self.rt, "_r3_l", return_value=True),
            mock.patch.object(self.rt, "_r3_uc_to_cached_patch", return_value=False),
            mock.patch.object(self.rt, "_post_load_cleanup", cleanup),
            mock.patch.object(self.rt, "_r3_u", close_r3),
            mock.patch.object(self.rt.time, "sleep", return_value=None),
            mock.patch.object(self.rt, "_r5_ready", True),
        ):
            self.assertFalse(self.rt._r5_setup())
            self.assertFalse(self.rt._r5_ready)
        cleanup.assert_not_called()
        close_r3.assert_called_once_with()

    def test_r5p_fallback_must_report_a_real_writer_before_ready(self):
        with (
            mock.patch.object(self.rt, "_g0", return_value=True),
            mock.patch.object(self.rt, "_r1_l", return_value=True),
            mock.patch.object(self.rt, "_r3_l", return_value=False),
            mock.patch.object(self.rt, "_r5p_load", return_value=False),
            mock.patch.object(self.rt, "_post_load_cleanup") as cleanup,
            mock.patch.object(self.rt.time, "sleep", return_value=None),
            mock.patch.object(self.rt, "_r5_ready", True),
        ):
            self.assertFalse(self.rt._r5_setup())
            self.assertFalse(self.rt._r5_ready)
        cleanup.assert_not_called()

    def test_r5p_fallback_preserves_non_r3_post_load_cleanup(self):
        cleanup = mock.Mock(return_value=True)
        with (
            mock.patch.object(self.rt, "_g0", return_value=True),
            mock.patch.object(self.rt, "_r1_l", return_value=True),
            mock.patch.object(self.rt, "_r3_l", return_value=False),
            mock.patch.object(self.rt, "_r5p_load", return_value=True),
            mock.patch.object(self.rt, "_post_load_cleanup", cleanup),
            mock.patch.object(self.rt.time, "sleep", return_value=None),
            mock.patch.object(self.rt, "_r5_ready", False),
        ):
            self.assertTrue(self.rt._r5_setup())
            self.assertTrue(self.rt._r5_ready)
        cleanup.assert_called_once_with()

    def test_r3_writer_publishes_ready_only_after_post_cleanup_succeeds(self):
        with (
            mock.patch.object(self.rt, "_g0", return_value=True),
            mock.patch.object(self.rt, "_r1_l", return_value=True),
            mock.patch.object(self.rt, "_r3_l", return_value=True),
            mock.patch.object(self.rt, "_r3_uc_to_cached_patch", return_value=True),
            mock.patch.object(self.rt, "_post_load_cleanup", return_value=True),
            mock.patch.object(self.rt.time, "sleep", return_value=None),
            mock.patch.object(self.rt, "_r5_ready", False),
        ):
            self.assertTrue(self.rt._r5_setup())
            self.assertTrue(self.rt._r5_ready)

    def test_r3_writer_does_not_publish_ready_when_post_cleanup_fails(self):
        with (
            mock.patch.object(self.rt, "_g0", return_value=True),
            mock.patch.object(self.rt, "_r1_l", return_value=True),
            mock.patch.object(self.rt, "_r3_l", return_value=True),
            mock.patch.object(self.rt, "_r3_uc_to_cached_patch", return_value=True),
            mock.patch.object(self.rt, "_post_load_cleanup", return_value=False),
            mock.patch.object(self.rt, "_r3_u"),
            mock.patch.object(self.rt.time, "sleep", return_value=None),
            mock.patch.object(self.rt, "_r5_ready", False),
        ):
            self.assertFalse(self.rt._r5_setup())
            self.assertFalse(self.rt._r5_ready)

    def test_post_load_cleanup_stops_before_kernel_writes_when_patch_fails(self):
        cleanup_write = mock.Mock(return_value=True)
        with (
            mock.patch.object(self.rt, "_r3h", 123),
            mock.patch.object(self.rt, "_r3_uc_to_cached_patch", return_value=False),
            mock.patch.object(self.rt, "_exec_cleanup_sc_safe", cleanup_write),
            mock.patch.object(self.rt, "_save_svc_names"),
        ):
            self.assertFalse(self.rt._post_load_cleanup())
        cleanup_write.assert_not_called()

    def test_post_load_cleanup_does_not_publish_after_cleanup_write_failure(self):
        with (
            mock.patch.object(self.rt, "_r3h", 123),
            mock.patch.object(self.rt, "_r3_uc_to_cached_patch", return_value=True),
            mock.patch.object(self.rt, "_exec_cleanup_sc_safe", return_value=False),
            mock.patch.object(self.rt, "_save_svc_names"),
        ):
            self.assertFalse(self.rt._post_load_cleanup())


class R3PatchIdentityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rt = _backend()

    def test_source_pe_binds_hash_rvas_original_bytes_and_live_text(self):
        source, header, text = _synthetic_r3_pe()
        identity = self.rt._r3_pe_patch_identity(source)
        self.assertIsNotNone(identity)
        self.assertEqual(identity["source_sha256"], hashlib.sha256(source).hexdigest())
        self.assertEqual(identity["patch_offsets"], (0x563, 0x8A6))

        # Simulate the loader applying the one DIR64 relocation in .text.  The
        # normalized live digest must still match the exact packaged image.
        live_text = bytearray(text)
        struct.pack_into("<Q", live_text, 0x200, 0xFFFF800000301234)
        base = 0xFFFF800000300000

        def live_read(address, size, _mode):
            if address == base and size == len(header):
                return header
            if address == base + 0x1000 and size == len(live_text):
                return bytes(live_text)
            return None

        with (
            mock.patch.object(self.rt, "_r3_patch_identity", identity),
            mock.patch.object(self.rt, "_r1_r", side_effect=live_read),
        ):
            self.assertTrue(self.rt._r3_verify_patch_identity(base))
            # A relocation slot is not a hash mask: an arbitrary pointer must
            # fail even though the same RVA is listed in .reloc.
            struct.pack_into("<Q", live_text, 0x200, 0xFFFF800000301235)
            self.assertFalse(self.rt._r3_verify_patch_identity(base))
            struct.pack_into("<Q", live_text, 0x200, 0xFFFF800000301234)
            live_text[0x300] ^= 0xFF
            self.assertFalse(self.rt._r3_verify_patch_identity(base))

    def test_source_pe_rejects_wrong_original_bytes_at_fixed_rva(self):
        source, _header, _text = _synthetic_r3_pe()
        changed = bytearray(source)
        changed[0x400 + 0x563:0x400 + 0x566] = b"bad"
        self.assertIsNone(self.rt._r3_pe_patch_identity(bytes(changed)))

    def test_source_pe_rejects_relocation_directory_past_raw_section(self):
        source, _header, _text = _synthetic_r3_pe()
        changed = bytearray(source)
        optional = 0x80 + 24
        struct.pack_into("<II", changed, optional + 112 + 5 * 8, 0x3000, 0x300)
        self.assertIsNone(self.rt._r3_pe_patch_identity(bytes(changed)))

    def test_build_identity_rejects_unlisted_source_hash(self):
        source, _header, _text = _synthetic_r3_pe()
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "candidate.sys"
            path.write_bytes(source)
            self.assertIsNone(self.rt._r3_build_patch_identity(str(path)))
            with mock.patch.object(
                self.rt, "_R3_PATCH_ALLOWED_HASHES",
                frozenset({hashlib.sha256(source).hexdigest()}),
            ):
                self.assertIsNotNone(self.rt._r3_build_patch_identity(str(path)))

    def test_live_identity_mismatch_aborts_before_patch_site_read_or_write(self):
        devobj = 0xFFFF800000100000
        driver = 0xFFFF800000200000
        image = 0xFFFF800000300000

        def read_qword(address):
            if address == devobj + 0x08:
                return driver
            if address == driver + 0x18:
                return image
            return 0

        with (
            mock.patch.object(self.rt, "_r3h", 7),
            mock.patch.object(self.rt, "_r3_uc_patched", False),
            mock.patch.object(self.rt, "_r3_devobj", return_value=devobj),
            mock.patch.object(self.rt, "_kr8", side_effect=read_qword),
            mock.patch.object(self.rt, "_r3_verify_patch_identity", return_value=False),
            mock.patch.object(self.rt, "_r1_r") as read,
            mock.patch.object(self.rt, "_kw") as write,
        ):
            self.assertFalse(self.rt._r3_uc_to_cached_patch())
        read.assert_not_called()
        write.assert_not_called()

    def test_old_patched_flag_cannot_authorize_a_reloaded_driver(self):
        devobj = 0xFFFF800000100000
        driver = 0xFFFF800000200000
        image = 0xFFFF800000900000

        def read_qword(address):
            if address == devobj + 0x08:
                return driver
            if address == driver + 0x18:
                return image
            return 0

        with (
            mock.patch.object(self.rt, "_r3h", 7),
            mock.patch.object(self.rt, "_r3_uc_patched", True),
            mock.patch.object(self.rt, "_r3_uc_patch_binding", ("old", 1)),
            mock.patch.object(self.rt, "_r3_devobj", return_value=devobj),
            mock.patch.object(self.rt, "_kr8", side_effect=read_qword),
            mock.patch.object(self.rt, "_r3_verify_patch_identity", return_value=False) as verify,
        ):
            self.assertFalse(self.rt._r3_uc_to_cached_patch())
            verify.assert_called_once_with(image)
            self.assertFalse(self.rt._r3_uc_patched)
            self.assertIsNone(self.rt._r3_uc_patch_binding)

    def test_watchdog_cleanup_cannot_use_direct_writes_with_stale_binding(self):
        direct = mock.Mock(return_value=True)
        with (
            mock.patch.object(self.rt, "_r1h", 1),
            mock.patch.object(self.rt, "_r3h", 2),
            mock.patch.object(self.rt, "_r3_uc_patched", True),
            mock.patch.object(self.rt, "_collect_cleanup_ops", return_value=[(3, b"x")]),
            mock.patch.object(self.rt, "_r3_cached_patch_is_current", return_value=False),
            mock.patch.object(self.rt, "_apply_cleanup_ops_direct", direct),
            mock.patch.object(self.rt, "_r3_devobj", return_value=0),
        ):
            self.assertFalse(self.rt._exec_cleanup_sc_safe())
        direct.assert_not_called()


class CleanupReportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rt = _backend()

    def _common_patches(self):
        return (
            mock.patch.object(self.rt, "_wd_stop_join", return_value=True, create=True),
            mock.patch.object(self.rt, "_e_wiper_stop_join", return_value=True, create=True),
            mock.patch.object(self.rt, "_e_cleanup", return_value=True),
            mock.patch.object(self.rt, "_ob_unprotect_process", return_value=True),
            mock.patch.object(self.rt, "_restore_etw"),
            mock.patch.object(self.rt, "_ntos_drop"),
            mock.patch.object(self.rt, "_pt_cache_clear"),
            mock.patch.object(self.rt, "_ep_cache_clear"),
            mock.patch.object(self.rt, "_sysmod_cache_clear"),
            mock.patch.object(self.rt, "_handle_cache_clear"),
            mock.patch.object(self.rt, "_allow_handle_close", return_value=True, create=True),
            mock.patch.object(self.rt, "_teardown_recovery", []),
            mock.patch.object(self.rt, "_cleanup_progress", {}, create=True),
            mock.patch.object(self.rt, "_cleanup_last_report", None, create=True),
            mock.patch.object(self.rt, "_shutdown", False),
        )

    def test_drv_unload_returns_signed_ntstatus(self):
        with (
            mock.patch.object(self.rt, "_service_registry_absent", return_value=False),
            mock.patch.object(self.rt, "_NtULD", mock.Mock(return_value=-1073741823)),
        ):
            self.assertEqual(self.rt._drv_unload("ExactService"), -1073741823)
        with (
            mock.patch.object(self.rt, "_service_registry_absent", return_value=False),
            mock.patch.object(self.rt, "_NtULD", mock.Mock(return_value=0xC0000001)),
        ):
            self.assertEqual(self.rt._drv_unload("ExactService"), -1073741823)
        with mock.patch.object(self.rt, "_NtULD", None):
            self.assertIsNone(self.rt._drv_unload("ExactService"))

    def test_drv_unload_recreates_only_exact_owned_service_key(self):
        register = mock.Mock()
        with (
            mock.patch.object(self.rt, "_NtULD", mock.Mock(return_value=0)),
            mock.patch.object(self.rt, "_service_registry_absent", return_value=True),
            mock.patch.object(self.rt, "_owned_service_files", {
                "ExactService": {"path": r"C:\owned\ExactService.sys", "sha256": "0" * 64}
            }),
            mock.patch.object(self.rt, "_owned_path_allowed", return_value=True),
            mock.patch.object(self.rt, "_drv_reg", register),
        ):
            self.assertEqual(self.rt._drv_unload("ExactService"), 0)
        register.assert_called_once_with("ExactService", r"C:\owned\ExactService.sys")

    def test_close_failure_is_reported_and_prevents_unload_or_state_clear(self):
        common = self._common_patches()
        with contextlib.ExitStack() as stack:
            for patcher in common:
                stack.enter_context(patcher)
            stack.enter_context(mock.patch.object(self.rt, "_r1h", 11))
            stack.enter_context(mock.patch.object(self.rt, "_r3h", None))
            stack.enter_context(mock.patch.object(self.rt, "_r5ph", 33))
            stack.enter_context(mock.patch.object(
                self.rt.kernel32, "CloseHandle", side_effect=[False, True]
            ))
            unload = stack.enter_context(mock.patch.object(self.rt, "_drv_unload"))
            report = self.rt._z()
            self.assertFalse(report["confirmed"])
            self.assertFalse(report["handles"]["r1"]["closed"])
            self.assertTrue(report["handles"]["r5p"]["closed"])
            self.assertEqual(self.rt._r1h, 11)
            self.assertEqual(self.rt._r5ph, 33)
        unload.assert_not_called()

    def test_thread_rundown_failure_returns_report_before_kernel_cleanup(self):
        cleanup = mock.Mock(return_value=True)
        with (
            mock.patch.object(self.rt, "_wd_stop_join", return_value=False),
            mock.patch.object(self.rt, "_e_wiper_stop_join", return_value=True),
            mock.patch.object(self.rt, "_e_cleanup", cleanup),
            mock.patch.object(self.rt, "_cleanup_progress", {}),
            mock.patch.object(self.rt, "_cleanup_last_report", None),
            mock.patch.object(self.rt, "_shutdown", False),
        ):
            report = self.rt._z()
        self.assertFalse(report["confirmed"])
        self.assertEqual(report["phase"], "thread_rundown")
        cleanup.assert_not_called()

    def test_unload_failure_preserves_services_files_and_globals_for_retry(self):
        common = self._common_patches()
        with contextlib.ExitStack() as stack:
            for patcher in common:
                stack.enter_context(patcher)
            stack.enter_context(mock.patch.object(self.rt, "_r1h", None))
            stack.enter_context(mock.patch.object(self.rt, "_r3h", None))
            stack.enter_context(mock.patch.object(self.rt, "_r5ph", None))
            stack.enter_context(mock.patch.object(self.rt, "_r1sn", "svc-r1"))
            stack.enter_context(mock.patch.object(self.rt, "_r3sn", None))
            stack.enter_context(mock.patch.object(self.rt, "_r5sn", None))
            stack.enter_context(mock.patch.object(self.rt, "_r2sn", None))
            stack.enter_context(mock.patch.object(self.rt, "_tmp_files", ["owned.sys"]))
            stack.enter_context(mock.patch.object(self.rt, "_drv_unload", return_value=-1))
            remove_service = stack.enter_context(mock.patch.object(self.rt, "_drv_reg_rm"))
            remove_file = stack.enter_context(mock.patch.object(self.rt, "_safe_remove"))
            report = self.rt._z()
            self.assertFalse(report["confirmed"])
            self.assertEqual(report["drivers"]["r1"]["ntstatus"], -1)
            self.assertEqual(self.rt._r1sn, "svc-r1")
        remove_service.assert_not_called()
        remove_file.assert_not_called()

    def test_full_confirmation_commits_state_clear_and_exact_deletion(self):
        common = self._common_patches()
        with contextlib.ExitStack() as stack:
            for patcher in common:
                stack.enter_context(patcher)
            stack.enter_context(mock.patch.object(self.rt, "_r1h", 11))
            stack.enter_context(mock.patch.object(self.rt, "_r3h", 22))
            stack.enter_context(mock.patch.object(self.rt, "_r5ph", 33))
            stack.enter_context(mock.patch.object(self.rt, "_r5_ready", True))
            stack.enter_context(mock.patch.object(self.rt, "_r1sn", "svc-r1"))
            stack.enter_context(mock.patch.object(self.rt, "_r3sn", None))
            stack.enter_context(mock.patch.object(self.rt, "_r5sn", None))
            stack.enter_context(mock.patch.object(self.rt, "_r2sn", None))
            stack.enter_context(mock.patch.object(self.rt, "_tmp_files", ["owned.sys"]))
            stack.enter_context(mock.patch.object(self.rt.kernel32, "CloseHandle", return_value=True))
            stack.enter_context(mock.patch.object(self.rt, "_drv_unload", return_value=0))
            stack.enter_context(mock.patch.object(self.rt, "_drv_reg_rm"))
            stack.enter_context(mock.patch.object(
                self.rt, "_service_registry_absent", return_value=True, create=True
            ))
            stack.enter_context(mock.patch.object(self.rt, "_owned_path_allowed", return_value=True))
            stack.enter_context(mock.patch.object(self.rt, "_safe_remove", return_value=True))
            stack.enter_context(mock.patch.object(
                self.rt, "_remove_owned_manifest_if_current", return_value=True
            ))
            report = self.rt._z()
            self.assertTrue(report["confirmed"])
            serialized = json.dumps(report)
            self.assertNotIn("svc-r1", serialized)
            self.assertNotIn("owned.sys", serialized)
            self.assertIsNone(self.rt._r1h)
            self.assertIsNone(self.rt._r3h)
            self.assertIsNone(self.rt._r5ph)
            self.assertFalse(self.rt._r5_ready)
            self.assertIsNone(self.rt._r1sn)

    def test_retry_reuses_confirmed_handle_closes_without_closing_twice(self):
        common = self._common_patches()
        with contextlib.ExitStack() as stack:
            for patcher in common:
                stack.enter_context(patcher)
            stack.enter_context(mock.patch.object(self.rt, "_r1h", 11))
            stack.enter_context(mock.patch.object(self.rt, "_r3h", None))
            stack.enter_context(mock.patch.object(self.rt, "_r5ph", 33))
            stack.enter_context(mock.patch.object(self.rt, "_r1sn", "svc-r1"))
            stack.enter_context(mock.patch.object(self.rt, "_r3sn", None))
            stack.enter_context(mock.patch.object(self.rt, "_r5sn", None))
            stack.enter_context(mock.patch.object(self.rt, "_r2sn", None))
            stack.enter_context(mock.patch.object(self.rt, "_tmp_files", []))
            close = stack.enter_context(mock.patch.object(
                self.rt.kernel32, "CloseHandle", return_value=True
            ))
            unload = stack.enter_context(mock.patch.object(
                self.rt, "_drv_unload", side_effect=[-1, 0]
            ))
            stack.enter_context(mock.patch.object(self.rt, "_drv_reg_rm"))
            stack.enter_context(mock.patch.object(
                self.rt, "_service_registry_absent", return_value=True
            ))
            stack.enter_context(mock.patch.object(
                self.rt, "_remove_owned_manifest_if_current", return_value=True
            ))
            first = self.rt._z()
            second = self.rt._z()
            self.assertFalse(first["confirmed"])
            self.assertTrue(second["confirmed"])
        self.assertEqual(close.call_count, 2)
        self.assertEqual(unload.call_count, 2)


class BackgroundRundownTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rt = _backend()

    def test_watchdog_start_is_single_owner_and_stop_join_is_proven(self):
        fake = _FakeThread()
        factory = mock.Mock(return_value=fake)
        with (
            mock.patch.object(self.rt, "_wd_thread", None),
            mock.patch.object(self.rt, "_wd_lock", threading.RLock()),
            mock.patch.object(self.rt, "_wd_stop_evt", threading.Event()),
            mock.patch.object(self.rt, "_wd_hidden_entries", [(1, "x")]),
            mock.patch.object(self.rt, "_ob_reg_handle_va", 0),
            mock.patch.object(self.rt, "_shutdown", False),
            mock.patch.object(self.rt.threading, "Thread", factory),
        ):
            self.assertTrue(self.rt._wd_start())
            self.assertTrue(self.rt._wd_start())
            self.assertEqual(factory.call_count, 1)
            self.assertTrue(self.rt._wd_stop_join())
            self.assertIsNone(self.rt._wd_thread)

    def test_watchdog_join_timeout_keeps_owner_for_retry(self):
        fake = _FakeThread(join_completes=False)
        fake.alive = True
        with (
            mock.patch.object(self.rt, "_wd_thread", fake),
            mock.patch.object(self.rt, "_wd_lock", threading.RLock()),
            mock.patch.object(self.rt, "_wd_stop_evt", threading.Event()),
        ):
            self.assertFalse(self.rt._wd_stop_join(0))
            self.assertIs(self.rt._wd_thread, fake)

    def test_watchdog_recovery_is_bounded(self):
        reinstall = mock.Mock(return_value=False)
        with (
            mock.patch.object(self.rt, "_wd_lock", threading.RLock()),
            mock.patch.object(self.rt, "_wd_stop_evt", threading.Event()),
            mock.patch.object(self.rt, "_wd_reinstall_attempts", 0),
            mock.patch.object(self.rt, "_WD_MAX_REINSTALL_ATTEMPTS", 3),
            mock.patch.object(self.rt, "_wd_reinstall_count", 0),
            mock.patch.object(self.rt, "_shutdown", False),
            mock.patch.object(self.rt, "_exec_cleanup_sc_safe", reinstall),
        ):
            for _ in range(5):
                self.rt._wd_reinstall_hides()
            self.assertEqual(self.rt._wd_reinstall_attempts, 3)
        self.assertEqual(reinstall.call_count, 3)

    def test_watchdog_ob_recovery_never_discards_owned_registration(self):
        protect = mock.Mock(return_value=True)
        with (
            mock.patch.object(self.rt, "_wd_lock", threading.RLock()),
            mock.patch.object(self.rt, "_wd_stop_evt", threading.Event()),
            mock.patch.object(self.rt, "_wd_reinstall_attempts", 0),
            mock.patch.object(self.rt, "_WD_MAX_REINSTALL_ATTEMPTS", 3),
            mock.patch.object(self.rt, "_shutdown", False),
            mock.patch.object(self.rt, "_ob_reg_handle_va", 123),
            mock.patch.object(self.rt, "_ob_pool_va", 456),
            mock.patch.object(self.rt, "_ob_protect_process", protect),
        ):
            self.assertFalse(self.rt._wd_reinstall_ob())
            self.assertEqual(self.rt._ob_reg_handle_va, 123)
            self.assertEqual(self.rt._ob_pool_va, 456)
        protect.assert_not_called()

    def test_idle_wiper_start_is_single_owner_and_joined(self):
        fake = _FakeThread()
        factory = mock.Mock(return_value=fake)
        with (
            mock.patch.object(self.rt, "_e_wiper_thread", None),
            mock.patch.object(self.rt, "_e_wiper_running", False),
            mock.patch.object(self.rt, "_e_wiper_lock", threading.RLock()),
            mock.patch.object(self.rt, "_e_wiper_stop_evt", threading.Event()),
            mock.patch.object(self.rt, "_shutdown", False),
            mock.patch.object(self.rt.threading, "Thread", factory),
        ):
            self.assertTrue(self.rt._start_idle_wiper())
            self.assertTrue(self.rt._start_idle_wiper())
            self.assertEqual(factory.call_count, 1)
            self.assertTrue(self.rt._e_wiper_stop_join())
            self.assertIsNone(self.rt._e_wiper_thread)


class MapLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rt = _backend()

    def _transport(self, *, user_va=0x2000, unmap_results=()):
        outcomes = iter(unmap_results)

        def invoke(_handle, ioctl, _ci, _ci_size, co, _co_size, returned):
            if ioctl == self.rt._R3M:
                ctypes.memmove(co, int(user_va).to_bytes(8, "little"), 8)
                returned.value = 8
                return True
            return bool(next(outcomes, True))

        return mock.Mock(side_effect=invoke)

    def test_map_returns_unique_pointer_compatible_ownership_tokens(self):
        transport = self._transport(user_va=0x2000)
        with (
            mock.patch.object(self.rt, "_r3h", 7),
            mock.patch.object(self.rt, "_r3_maps", {}),
            mock.patch.object(self.rt, "_r3_map_lock", threading.RLock()),
            mock.patch.object(self.rt, "_r3_degraded", False),
            mock.patch.object(self.rt, "_io_d2", transport),
        ):
            first = self.rt._r3_m(0x1000)
            second = self.rt._r3_m(0x1000)

            self.assertIsInstance(first, int)
            self.assertIsInstance(second, int)
            self.assertEqual(int(first), 0x2000)
            self.assertEqual(int(second), 0x2000)
            self.assertIsNot(first, second)
            self.assertNotEqual(first._map_id, second._map_id)
            self.assertEqual(self.rt._r3_active_map_count(), 2)
            self.assertTrue(self.rt._r3_um(0x1000, first))
            self.assertTrue(self.rt._r3_um(0x1000, second))
            self.assertEqual(self.rt._r3_active_map_count(), 0)

    def test_plain_integer_or_wrong_token_cannot_unmap_owned_mapping(self):
        transport = self._transport(user_va=0x2400)
        with (
            mock.patch.object(self.rt, "_r3h", 7),
            mock.patch.object(self.rt, "_r3_maps", {}),
            mock.patch.object(self.rt, "_r3_map_lock", threading.RLock()),
            mock.patch.object(self.rt, "_r3_degraded", False),
            mock.patch.object(self.rt, "_r5_ready", True),
            mock.patch.object(self.rt, "_io_d2", transport),
        ):
            token = self.rt._r3_m(0x1000)
            self.assertFalse(self.rt._r3_um(0x1000, int(token)))
            self.assertTrue(self.rt._r3_degraded)
            self.assertFalse(self.rt._r5_ready)
            self.assertEqual(self.rt._r3_active_map_count(), 1)
            # Ownership is retained, so the real token can still be recovered.
            self.assertTrue(self.rt._r3_um(0x1000, token))
            self.assertEqual(self.rt._r3_active_map_count(), 0)

    def test_unmap_failure_marks_degraded_and_blocks_all_new_writes(self):
        transport = self._transport(user_va=0x2800, unmap_results=(False,))
        with (
            mock.patch.object(self.rt, "_r3h", 7),
            mock.patch.object(self.rt, "_r5ph", 9),
            mock.patch.object(self.rt, "_r3_maps", {}),
            mock.patch.object(self.rt, "_r3_map_lock", threading.RLock()),
            mock.patch.object(self.rt, "_r3_degraded", False),
            mock.patch.object(self.rt, "_r5_ready", True),
            mock.patch.object(self.rt, "_probe_result", True),
            mock.patch.object(self.rt, "_shutdown", False),
            mock.patch.object(self.rt, "_attached_pid", 123),
            mock.patch.object(self.rt, "_backend", 2),
            mock.patch.object(self.rt, "_io_d2", transport),
        ):
            token = self.rt._r3_m(0x1000)
            self.assertFalse(self.rt._r3_um(0x1000, token))
            calls_after_failure = transport.call_count
            self.assertTrue(self.rt._r3_degraded)
            self.assertFalse(self.rt._r5_ready)
            self.assertFalse(self.rt._probe_result)
            self.assertFalse(self.rt.probe())
            self.assertFalse(self.rt.has_write_engine())
            self.assertEqual(self.rt._r3_m(0x3000), 0)
            self.assertFalse(self.rt._r5p_write(0x3000, b"data"))
            self.assertFalse(self.rt.write(0x4000, b"data"))
            self.assertEqual(transport.call_count, calls_after_failure)
            self.assertEqual(self.rt._r3_active_map_count(), 1)

    def test_cleanup_retries_owned_map_before_closing_handle(self):
        transport = self._transport(user_va=0x2C00, unmap_results=(False, True))
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(self.rt, "_r3h", 7))
            stack.enter_context(mock.patch.object(self.rt, "_r1h", None))
            stack.enter_context(mock.patch.object(self.rt, "_r5ph", None))
            stack.enter_context(mock.patch.object(self.rt, "_r3_maps", {}))
            stack.enter_context(mock.patch.object(self.rt, "_r3_map_lock", threading.RLock()))
            stack.enter_context(mock.patch.object(self.rt, "_r3_degraded", False))
            stack.enter_context(mock.patch.object(self.rt, "_r5_ready", True))
            stack.enter_context(mock.patch.object(self.rt, "_shutdown", False))
            stack.enter_context(mock.patch.object(self.rt, "_cleanup_progress", {}))
            stack.enter_context(mock.patch.object(self.rt, "_cleanup_last_report", None))
            stack.enter_context(mock.patch.object(self.rt, "_teardown_recovery", []))
            stack.enter_context(mock.patch.object(self.rt, "_r1sn", None))
            stack.enter_context(mock.patch.object(self.rt, "_r3sn", None))
            stack.enter_context(mock.patch.object(self.rt, "_r5sn", None))
            stack.enter_context(mock.patch.object(self.rt, "_r2sn", None))
            stack.enter_context(mock.patch.object(self.rt, "_tmp_files", []))
            stack.enter_context(mock.patch.object(self.rt, "_owned_services", set()))
            stack.enter_context(mock.patch.object(self.rt, "_owned_files", set()))
            stack.enter_context(mock.patch.object(self.rt, "_owned_service_files", {}))
            stack.enter_context(mock.patch.object(self.rt, "_io_d2", transport))
            stack.enter_context(mock.patch.object(self.rt, "_wd_stop_join", return_value=True))
            stack.enter_context(mock.patch.object(self.rt, "_e_wiper_stop_join", return_value=True))
            stack.enter_context(mock.patch.object(self.rt, "_e_cleanup", return_value=True))
            stack.enter_context(mock.patch.object(self.rt, "_ob_unprotect_process", return_value=True))
            stack.enter_context(mock.patch.object(self.rt, "_restore_etw"))
            stack.enter_context(mock.patch.object(self.rt, "_allow_handle_close", return_value=True))
            close = stack.enter_context(mock.patch.object(
                self.rt.kernel32, "CloseHandle", return_value=True
            ))
            for name in (
                "_ntos_drop", "_pt_cache_clear", "_ep_cache_clear",
                "_sysmod_cache_clear", "_handle_cache_clear",
            ):
                stack.enter_context(mock.patch.object(self.rt, name))
            stack.enter_context(mock.patch.object(
                self.rt, "_SVC_STATE_FILE", "never-created-map-cleanup-manifest"
            ))

            token = self.rt._r3_m(0x1000)
            self.assertTrue(token)
            first = self.rt._z()
            self.assertFalse(first["confirmed"])
            self.assertEqual(first["phase"], "map_rundown")
            self.assertEqual(first["maps"]["active_after"], 1)
            self.assertEqual(close.call_count, 0)
            self.assertEqual(self.rt._r3h, 7)

            second = self.rt._z()
            self.assertTrue(second["confirmed"])
            self.assertEqual(second["maps"]["active_after"], 0)
            self.assertFalse(second["maps"]["degraded"])
            self.assertEqual(close.call_count, 1)
            self.assertIsNone(self.rt._r3h)
            self.assertEqual(self.rt._r3_active_map_count(), 0)
            self.assertFalse(self.rt._r3_degraded)

    def test_kernel_write_is_not_confirmed_when_unmap_fails(self):
        backing = ctypes.create_string_buffer(0x1000)
        with (
            mock.patch.object(self.rt, "_r1_fp", return_value=0x1000),
            mock.patch.object(self.rt, "_r1_pv8", side_effect=[0x2000, 0x3000]),
            mock.patch.object(self.rt, "_r1_w", return_value=0x5008),
            mock.patch.object(self.rt, "_r3_m", return_value=ctypes.addressof(backing)),
            mock.patch.object(self.rt, "_r3_um", return_value=False),
            mock.patch.object(self.rt, "_OFF_DTB", 0),
        ):
            self.assertFalse(self.rt._kw(0xFFFF0000, b"ABCD"))

    def test_physical_dword_write_is_not_confirmed_when_unmap_fails(self):
        backing = ctypes.create_string_buffer(0x1000)
        with (
            mock.patch.object(self.rt, "_r3h", 7),
            mock.patch.object(self.rt, "_r3_m", return_value=ctypes.addressof(backing)),
            mock.patch.object(self.rt, "_r3_um", return_value=False),
        ):
            self.assertFalse(self.rt._pw(0x1004, 0x12345678))

    def test_r5_mapped_write_uses_finally_and_propagates_unmap_failure(self):
        backing = ctypes.create_string_buffer(0x1000)
        with (
            mock.patch.object(self.rt, "_r1_fe", return_value=(0x10, 0x20)),
            mock.patch.object(self.rt, "_r1_w", return_value=0x3004),
            mock.patch.object(self.rt, "_r5ph", None),
            mock.patch.object(self.rt, "_r3_m", return_value=ctypes.addressof(backing)),
            mock.patch.object(self.rt, "_r3_um", return_value=False),
        ):
            self.assertFalse(self.rt._r5_write(123, 0x4004, b"data"))

    def test_r3_failure_close_preserves_handle_and_service_owner(self):
        remove_service = mock.Mock()
        with (
            mock.patch.object(self.rt, "_r3h", 55),
            mock.patch.object(self.rt, "_r3sn", "owned-service"),
            mock.patch.object(self.rt, "_r3_active_map_count", return_value=0),
            mock.patch.object(self.rt.kernel32, "CloseHandle", return_value=False),
            mock.patch.object(self.rt, "_drv_reg_rm", remove_service),
        ):
            self.assertFalse(self.rt._r3_u())
            self.assertEqual(self.rt._r3h, 55)
            self.assertEqual(self.rt._r3sn, "owned-service")
        remove_service.assert_not_called()

    def test_handle_obfuscation_keeps_original_when_close_fails(self):
        def duplicate(_cur, _source, _cur2, out, *_args):
            ctypes.cast(out, ctypes.POINTER(ctypes.c_void_p))[0] = 99
            return True

        closes = iter((False, True))
        with (
            mock.patch.object(self.rt, "_aux_owned_handles", set()),
            mock.patch.object(
                self.rt.kernel32, "DuplicateHandle", side_effect=duplicate),
            mock.patch.object(
                self.rt.kernel32, "CloseHandle",
                side_effect=lambda *_args: next(closes)),
            mock.patch.object(self.rt, "_io_d0", return_value=True),
        ):
            self.assertEqual(self.rt._obfuscate_handle(55), 55)
            self.assertEqual(self.rt._aux_owned_handles, set())


class TeardownGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rt = _backend()

    def _dpc_context(self, has_flush: bool):
        return {
            "mode": "dpc",
            "cancel_sc": b"cancel",
            "hp": 0x1000,
            "oh": 0x2000,
            "trig_fn": mock.Mock(),
            "trig_h": 77,
            "dpc_va": 0x3000,
            "dpc_len": 8,
            "tramp_va": 0x4000,
            "tramp_len": 8,
            "rw_va": 0x5000,
            "dpc_has_flush": has_flush,
        }

    def test_trigger_exception_still_restores_and_preserves_regions(self):
        trigger = mock.Mock(side_effect=RuntimeError("trigger failed"))
        regions = [(0x3000, b"\xCC" * 8)]
        with (
            mock.patch.object(
                self.rt, "_restore_pointer_and_verify", return_value=True
            ) as restore,
            mock.patch.object(self.rt, "_teardown_recovery", []),
            mock.patch.object(self.rt, "_wipe_kernel_regions") as wipe,
        ):
            self.assertFalse(self.rt._trigger_pointer_and_restore(
                "trigger_exception", trigger, 77, 0x1000, 0x2000,
                regions,
            ))
            restore.assert_called_once_with(0x1000, 0x2000)
            self.assertEqual(
                self.rt._teardown_recovery[-1]["kind"],
                "trigger_exception",
            )
        wipe.assert_not_called()

    def test_dpc_without_kernel_flush_preserves_all_executable_regions(self):
        writes = []

        def write(addr, data):
            writes.append((addr, bytes(data)))
            return True

        context = self._dpc_context(False)
        with (
            mock.patch.object(self.rt, "_find_exec_cave", return_value=0x6000),
            mock.patch.object(self.rt, "_kw", side_effect=write),
            mock.patch.object(self.rt, "_kr8", return_value=context["oh"]),
            mock.patch.object(self.rt, "_teardown_recovery", []),
        ):
            self.assertFalse(self.rt._e_cleanup_dpc(context))
            self.assertTrue(self.rt._teardown_recovery)

        wiped_addresses = {
            addr for addr, data in writes
            if data and (set(data) == {0xCC} or set(data) == {0})
        }
        self.assertFalse({0x6000, 0x3000, 0x4000, 0x5000} & wiped_addresses)

    def test_dpc_with_pointer_readback_and_flush_can_wipe(self):
        writes = []

        def write(addr, data):
            writes.append((addr, bytes(data)))
            return True

        context = self._dpc_context(True)
        with (
            mock.patch.object(self.rt, "_find_exec_cave", return_value=0x6000),
            mock.patch.object(self.rt, "_kw", side_effect=write),
            mock.patch.object(self.rt, "_kr8", return_value=context["oh"]),
        ):
            self.assertTrue(self.rt._e_cleanup_dpc(context))

        wiped_addresses = {
            addr for addr, data in writes
            if data and (set(data) == {0xCC} or set(data) == {0})
        }
        self.assertTrue({0x6000, 0x3000, 0x4000, 0x5000} <= wiped_addresses)

    def test_failed_cleanup_keeps_context_and_trigger_handle_for_retry(self):
        context = self._dpc_context(False)
        with (
            mock.patch.object(self.rt, "_e_ctx", context),
            mock.patch.object(self.rt, "_e_cleanup_dpc", return_value=False),
            mock.patch.object(self.rt.kernel32, "CloseHandle") as close_handle,
        ):
            self.assertFalse(self.rt._e_cleanup())
            self.assertIs(self.rt._e_ctx, context)
        close_handle.assert_not_called()

    def test_failed_cleanup_blocks_context_switch(self):
        current = {"cs": object()}
        with (
            mock.patch.object(self.rt, "_e_ctx", current),
            mock.patch.object(self.rt, "_e_cleanup", return_value=False),
            mock.patch.object(self.rt, "_e_setup") as setup,
        ):
            self.assertIsNone(self.rt._e_switch(object()))
        setup.assert_not_called()

    def test_dpc_setup_recovery_blocks_mf_fallback(self):
        def fail_dpc(_):
            self.rt._teardown_recovery.append({"kind": "dpc_setup"})
            return None

        with (
            mock.patch.object(self.rt, "_r5_ready", True),
            mock.patch.object(self.rt, "_r3h", 7),
            mock.patch.object(self.rt, "_teardown_recovery", []),
            mock.patch.object(self.rt, "_e_setup_dpc", side_effect=fail_dpc),
            mock.patch.object(self.rt, "_e_setup_mf") as setup_mf,
        ):
            self.assertIsNone(self.rt._e_setup({}))
        setup_mf.assert_not_called()

    def test_r3_patch_restore_failure_preserves_cave_and_recovery_state(self):
        writes = []
        devobj = 0xFFFF800000100000
        driver = 0xFFFF800000200000
        image = 0xFFFF800000300000
        cave = 0xFFFF800000400000
        original = 0xFFFF800000500000
        dispatch = driver + 0x70 + 8

        def read_qword(address):
            if address == devobj + 0x08:
                return driver
            if address == driver + 0x18:
                return image
            if address == dispatch:
                return cave  # restoration never becomes visible
            return 0

        def write(address, data):
            writes.append((address, bytes(data)))
            return True

        with (
            mock.patch.object(self.rt, "_r3h", 7),
            mock.patch.object(self.rt, "_r3_devobj", return_value=devobj),
            mock.patch.object(self.rt, "_kr8", side_effect=read_qword),
            mock.patch.object(self.rt, "_r3_verify_patch_identity", return_value=True),
            mock.patch.object(
                self.rt, "_pick_mf_slot", return_value=(1, original, mock.Mock())
            ),
            mock.patch.object(self.rt, "_r3_build_uc_patch_shellcode", return_value=b"shell"),
            mock.patch.object(self.rt, "_r1_r", return_value=b"\x45\x33\xC0"),
            mock.patch.object(self.rt, "_find_exec_cave", return_value=cave),
            mock.patch.object(self.rt, "_kw", side_effect=write),
            mock.patch.object(self.rt, "_open_hid_handle", return_value=88),
            mock.patch.object(self.rt.kernel32, "CloseHandle", return_value=True),
            mock.patch.object(self.rt, "_teardown_recovery", []),
        ):
            self.assertFalse(self.rt._r3_uc_to_cached_patch())
            self.assertEqual(self.rt._teardown_recovery[-1]["kind"], "r3_cache_patch")

        self.assertNotIn((cave, b"\xCC" * len(b"shell")), writes)

    def test_r3_patch_rejects_unrecognised_fixed_rva_bytes_before_writing(self):
        devobj = 0xFFFF800000100000
        driver = 0xFFFF800000200000
        image = 0xFFFF800000300000

        def read_qword(address):
            if address == devobj + 0x08:
                return driver
            if address == driver + 0x18:
                return image
            return 0

        with (
            mock.patch.object(self.rt, "_r3h", 7),
            mock.patch.object(self.rt, "_r3_devobj", return_value=devobj),
            mock.patch.object(self.rt, "_kr8", side_effect=read_qword),
            mock.patch.object(self.rt, "_r3_verify_patch_identity", return_value=True),
            mock.patch.object(self.rt, "_r1_r", return_value=b"bad"),
            mock.patch.object(self.rt, "_kw") as write,
        ):
            self.assertFalse(self.rt._r3_uc_to_cached_patch())
        write.assert_not_called()


if __name__ == "__main__":
    unittest.main(verbosity=2)
